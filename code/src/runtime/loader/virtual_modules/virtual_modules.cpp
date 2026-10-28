#include "runtime/loader/virtual_modules/virtual_modules.hpp"

#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <utility>

#include <unistd.h>

#include "runtime/memory/address_space.hpp"
#include "runtime/threads/tls/tls_layout_android13.hpp"

namespace duckemu::runtime::loader
{
namespace
{

constexpr int64_t kEbadf = 9;
constexpr int64_t kEfault = 14;
constexpr int64_t kEinval = 22;

constexpr uint64_t kMallocHeapSize = 1024 * 1024;
constexpr uint64_t kMallocAlignment = 16;
constexpr uint64_t kMaxWriteBytes = 1ULL << 20;

struct VirtualEntry
{
	std::string_view module;
	std::string_view symbol;
};

// Symbols a real Android linker exports from ld-android.so. M4.5 only needs
// the slots to bind; the implementations return 0 and M4.6 replaces the ones
// that carry state (__loader_shared_globals).
constexpr VirtualEntry kLinkerEntries[] = {
	{ "ld-android.so", "__loader_dlopen" },
	{ "ld-android.so", "__loader_dlsym" },
	{ "ld-android.so", "__loader_dlvsym" },
	{ "ld-android.so", "__loader_dlerror" },
	{ "ld-android.so", "__loader_dladdr" },
	{ "ld-android.so", "__loader_dlclose" },
	{ "ld-android.so", "__loader_dl_iterate_phdr" },
	{ "ld-android.so", "__loader_android_dlopen_ext" },
	{ "ld-android.so", "__loader_android_get_application_target_sdk_version" },
	{ "ld-android.so", "__loader_android_get_LD_LIBRARY_PATH" },
	{ "ld-android.so", "__loader_android_handle_signal" },
	{ "ld-android.so", "__loader_cfi_fail" },
	{ "ld-android.so", "__loader_shared_globals" },
	{ "ld-android.so", "__loader_add_thread_local_dtor" },
	{ "ld-android.so", "__loader_remove_thread_local_dtor" },
	{ "ld-android.so", "__loader_android_get_exported_namespace" },
};

// M4's fixture table. Production keeps it disabled: these names now come from
// the real libc.so/liblog.so loaded from disk.
constexpr VirtualEntry kTestEntries[] = {
	{ "libc.so", "__errno" },
	{ "libc.so", "write" },
	{ "libc.so", "malloc" },
	{ "libc.so", "free" },
	{ "libc.so", "pthread_once" },
	{ "liblog.so", "log_print" },
};

// Shared by the malloc stub. Captured, so the stub does not depend on this
// object outliving the thunk table.
struct MallocState
{
	std::mutex mutex;
	GuestVA base;
	uint64_t used = 0;
	uint64_t size = 0;
};

bool matches(const VirtualEntry *entries, size_t count, std::string_view module, std::string_view symbol)
{
	for (size_t i = 0; i < count; ++i)
	{
		if (entries[i].module == module && entries[i].symbol == symbol)
		{
			return true;
		}
	}
	return false;
}

bool matchesAny(const VirtualEntry *entries, size_t count, std::string_view symbol)
{
	for (size_t i = 0; i < count; ++i)
	{
		if (entries[i].symbol == symbol)
		{
			return true;
		}
	}
	return false;
}

} // namespace

VirtualBionic::VirtualBionic(RuntimeMemory &memory, ThunkTable &thunks, bool enable_test_import_stubs)
	: memory_(memory)
	, thunks_(thunks)
	, test_stubs_(enable_test_import_stubs)
{
}

bool VirtualBionic::isVirtualModule(std::string_view soname) const noexcept
{
	if (soname == "ld-android.so")
	{
		return true;
	}
	if (!test_stubs_)
	{
		return false;
	}
	for (const VirtualEntry &entry : kTestEntries)
	{
		if (entry.module == soname)
		{
			return true;
		}
	}
	return false;
}

bool VirtualBionic::declares(std::string_view module_soname, std::string_view symbol) const noexcept
{
	if (matches(kLinkerEntries, std::size(kLinkerEntries), module_soname, symbol))
	{
		return true;
	}
	return test_stubs_ && matches(kTestEntries, std::size(kTestEntries), module_soname, symbol);
}

bool VirtualBionic::declaresAny(std::string_view symbol) const noexcept
{
	if (matchesAny(kLinkerEntries, std::size(kLinkerEntries), symbol))
	{
		return true;
	}
	return test_stubs_ && matchesAny(kTestEntries, std::size(kTestEntries), symbol);
}

namespace
{

GuestVA errnoAddress(RuntimeMemory &memory, uint64_t thread_pointer)
{
	using namespace tls::android13;

	uint64_t thread = 0;
	const GuestVA slot = GuestVA{ thread_pointer + static_cast<uint64_t>(kSlotThreadId) * kPointerSize };
	if (thread_pointer == 0 || !memory.read(slot, &thread, sizeof(thread)).ok() || thread == 0)
	{
		return GuestVA{};
	}
	return GuestVA{ thread + kThreadErrno };
}

} // namespace

Result<HostStub> VirtualBionic::makeStub(std::string_view symbol)
{
	// ld-android.so: binding-only stubs. Returning 0 is safe for load; any
	// symbol that needs real behaviour is picked up in M4.6.
	if (matchesAny(kLinkerEntries, std::size(kLinkerEntries), symbol))
	{
		return HostStub{ [](HostCallArgs &) -> int64_t { return 0; } };
	}

	if (symbol == "__errno")
	{
		// Captures the memory reference only: the thunk table outlives this
		// object during RuntimeInstance teardown.
		RuntimeMemory *memory = &memory_;
		return HostStub{ [memory](HostCallArgs &args) -> int64_t {
			return static_cast<int64_t>(errnoAddress(*memory, args.thread_pointer).value());
		} };
	}

	if (symbol == "write")
	{
		RuntimeMemory *memory = &memory_;
		return HostStub{ [memory](HostCallArgs &args) -> int64_t {
			const uint64_t fd = args.x[0];
			const uint64_t address = args.x[1];
			const uint64_t count = args.x[2];
			if (fd != 1 && fd != 2)
			{
				return -kEbadf;
			}
			if (count > kMaxWriteBytes)
			{
				return -kEinval;
			}
			std::vector<uint8_t> buffer(static_cast<size_t>(count));
			if (!memory->read(GuestVA{ address }, buffer.data(), count).ok())
			{
				return -kEfault;
			}
			const ssize_t written = ::write(static_cast<int>(fd), buffer.data(), buffer.size());
			return written < 0 ? -kEfault : static_cast<int64_t>(written);
		} };
	}

	if (symbol == "malloc")
	{
		RuntimeMemory *memory = &memory_;
		auto state = std::make_shared<MallocState>();
		return HostStub{ [memory, state](HostCallArgs &args) -> int64_t {
			const uint64_t requested = args.x[0];
			if (requested == 0)
			{
				return 0;
			}
			std::lock_guard<std::mutex> lock(state->mutex);
			if (state->base.value() == 0)
			{
				auto mapped = memory->map(kMallocHeapSize, PermReadWrite, "malloc-heap");
				if (!mapped.ok())
				{
					return 0;
				}
				state->base = mapped.value();
				state->size = kMallocHeapSize;
			}
			const uint64_t aligned = alignUp(requested, kMallocAlignment);
			if (aligned > state->size - state->used)
			{
				return 0; // the M4 heap does not grow
			}
			const GuestVA result = GuestVA{ state->base.value() + state->used };
			state->used += aligned;
			return static_cast<int64_t>(result.value());
		} };
	}

	if (symbol == "free")
	{
		// Bump allocator: free is a no-op.
		return HostStub{ [](HostCallArgs &) -> int64_t { return 0; } };
	}

	if (symbol == "pthread_once")
	{
		RuntimeMemory *memory = &memory_;
		return HostStub{ [memory](HostCallArgs &args) -> int64_t {
			const GuestVA control = GuestVA{ args.x[0] };
			const GuestVA init = GuestVA{ args.x[1] };
			uint32_t state = 0;
			if (!memory->read(control, &state, sizeof(state)).ok())
			{
				return -kEinval;
			}
			if (state == 2)
			{
				return 0;
			}
			// A recursive pthread_once on the serialized execution model would
			// deadlock in bionic; treat it as already running.
			if (state == 1)
			{
				return 0;
			}
			const uint32_t running = 1;
			if (!memory->write(control, &running, sizeof(running)).ok())
			{
				return -kEinval;
			}
			if (!args.call_guest)
			{
				const uint32_t not_started = 0;
				memory->write(control, &not_started, sizeof(not_started));
				return -kEinval;
			}

			auto called = args.call_guest(init, std::array<uint64_t, 8>{});
			if (!called.ok())
			{
				const uint32_t not_started = 0;
				memory->write(control, &not_started, sizeof(not_started));
				return -kEfault;
			}
			const uint32_t done = 2;
			if (!memory->write(control, &done, sizeof(done)).ok())
			{
				return -kEfault;
			}
			return 0;
		} };
	}

	if (symbol == "log_print")
	{
		return HostStub{ [](HostCallArgs &) -> int64_t { return 1; } };
	}

	return Error{ ErrorCode::NotFound, "no virtual implementation for " + std::string(symbol) };
}

Result<GuestVA> VirtualBionic::resolve(std::string_view symbol)
{
	for (const auto &[name, address] : thunks_by_name_)
	{
		if (name == symbol)
		{
			return address;
		}
	}

	auto stub = makeStub(symbol);
	if (!stub.ok())
	{
		return stub.error();
	}
	auto thunk = thunks_.add(symbol, std::move(stub).value());
	if (!thunk.ok())
	{
		return thunk.error();
	}
	thunks_by_name_.emplace_back(std::string(symbol), thunk.value());
	return thunk.value();
}

} // namespace duckemu::runtime::loader
