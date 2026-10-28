#include "runtime/loader/virtual_modules/virtual_modules.hpp"

#include <cstring>
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
constexpr int64_t kEnomem = 12;
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

// The M4 surface. A symbol that is not here is reported unresolved; the error
// names it, which is what a north-star load will need first.
constexpr VirtualEntry kEntries[] = {
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

} // namespace

VirtualBionic::VirtualBionic(RuntimeMemory &memory, ThunkTable &thunks)
	: memory_(memory)
	, thunks_(thunks)
{
}

bool VirtualBionic::isVirtualModule(std::string_view soname) noexcept
{
	for (const std::string_view name : moduleNames())
	{
		if (name == soname)
		{
			return true;
		}
	}
	return false;
}

std::vector<std::string_view> VirtualBionic::moduleNames()
{
	return { "libc.so", "liblog.so", "libdl.so", "libm.so" };
}

bool VirtualBionic::declares(std::string_view module_soname, std::string_view symbol) noexcept
{
	for (const VirtualEntry &entry : kEntries)
	{
		if (entry.module == module_soname && entry.symbol == symbol)
		{
			return true;
		}
	}
	return false;
}

bool VirtualBionic::declaresAny(std::string_view symbol) noexcept
{
	for (const VirtualEntry &entry : kEntries)
	{
		if (entry.symbol == symbol)
		{
			return true;
		}
	}
	return false;
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
