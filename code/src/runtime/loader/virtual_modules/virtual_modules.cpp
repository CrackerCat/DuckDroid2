// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/loader/virtual_modules/virtual_modules.hpp"

#include <array>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include "runtime/loader/bionic/shared_globals.hpp"
#include "runtime/loader/dl_service.hpp"
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

// Import-stub modules: present in an app SO's DT_NEEDED list, but their real
// images pull in the whole framework (or the mainline APEXes). Symbols are
// bound to a return-0 thunk. The signing paths served so far never call them.
constexpr std::string_view kImportStubModules[] = { "libandroid.so", "libmediandk.so" };

bool isImportStubModuleName(std::string_view soname)
{
	for (const std::string_view name : kImportStubModules)
	{
		if (name == soname)
		{
			return true;
		}
	}
	return false;
}

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

// State behind the __loader_dl* thunks. Shared with the thunks (they capture
// the shared_ptr) so it does not depend on this object's lifetime, and so
// setDlService can land after the thunks were created.
struct DlStubState
{
	RuntimeMemory *memory = nullptr;
	DlService *service = nullptr;
	// Recursive: a constructor run by dlopen may itself call dlopen.
	std::recursive_mutex mutex;
	// Bump arena for guest-visible strings and dl_phdr_info records.
	GuestVA scratch;
	uint64_t scratch_size = 0;
	uint64_t scratch_used = 0;
	GuestVA error;
	bool error_pending = false;
};

namespace
{

constexpr uint64_t kDlScratchSize = 64 * 1024;
constexpr uint64_t kMaxGuestString = 4096;
// bionic's struct dl_phdr_info on LP64: addr, name, phdr, phnum, adds, subs,
// tls_modid, tls_data.
constexpr uint64_t kDlPhdrInfoSize = 64;
constexpr uint64_t kDlPhdrNumOffset = 24;

std::optional<std::string> readGuestCString(RuntimeMemory &memory, GuestVA address)
{
	std::string text;
	text.reserve(64);
	for (uint64_t i = 0; i < kMaxGuestString; ++i)
	{
		char ch = 0;
		if (!memory.read(GuestVA{ address.value() + i }, &ch, 1).ok())
		{
			return std::nullopt;
		}
		if (ch == '\0')
		{
			return text;
		}
		text.push_back(ch);
	}
	return std::nullopt;
}

GuestVA allocScratch(DlStubState &state, uint64_t size, uint64_t align)
{
	if (state.memory == nullptr)
	{
		return GuestVA{};
	}
	if (state.scratch.value() == 0)
	{
		auto mapped = state.memory->map(kDlScratchSize, PermReadWrite, "dl-scratch");
		if (!mapped.ok())
		{
			return GuestVA{};
		}
		state.scratch = mapped.value();
		state.scratch_size = kDlScratchSize;
		state.scratch_used = 0;
	}
	const uint64_t start = alignUp(state.scratch_used, align == 0 ? 8 : align);
	if (size > state.scratch_size || start > state.scratch_size - size)
	{
		return GuestVA{};
	}
	state.scratch_used = start + size;
	return GuestVA{ state.scratch.value() + start };
}

GuestVA writeGuestString(DlStubState &state, std::string_view text)
{
	const GuestVA at = allocScratch(state, text.size() + 1, 8);
	if (at.value() == 0)
	{
		return GuestVA{};
	}
	if (!text.empty() && !state.memory->write(at, text.data(), text.size()).ok())
	{
		return GuestVA{};
	}
	const char terminator = '\0';
	if (!state.memory->write(GuestVA{ at.value() + text.size() }, &terminator, 1).ok())
	{
		return GuestVA{};
	}
	return at;
}

// The message stays readable by the guest until dlerror consumes it.
void recordError(DlStubState &state, std::string_view message)
{
	state.error_pending = true;
	state.error = writeGuestString(state, message);
}

} // namespace

VirtualBionic::VirtualBionic(RuntimeMemory &memory, ThunkTable &thunks, bool enable_test_import_stubs)
	: memory_(memory)
	, thunks_(thunks)
	, test_stubs_(enable_test_import_stubs)
{
	dl_ = std::make_shared<DlStubState>();
	dl_->memory = &memory_;
}

void VirtualBionic::setDlService(DlService *service)
{
	std::lock_guard<std::recursive_mutex> lock(dl_->mutex);
	dl_->service = service;
}

std::optional<HostStub> VirtualBionic::makeDlStub(std::string_view symbol)
{
	const std::shared_ptr<DlStubState> state = dl_;

	if (symbol == "__loader_dlopen" || symbol == "__loader_android_dlopen_ext")
	{
		// Bionic passes the caller's return address last: argument 3 for
		// __loader_dlopen, 4 for android_dlopen_ext (after extinfo).
		const bool extended = symbol == "__loader_android_dlopen_ext";
		return HostStub{ [state, extended](HostCallArgs &args) -> int64_t {
			std::lock_guard<std::recursive_mutex> lock(state->mutex);
			if (state->service == nullptr)
			{
				return 0;
			}
			std::string name;
			if (args.x[0] != 0)
			{
				auto text = readGuestCString(*state->memory, GuestVA{ args.x[0] });
				if (!text.has_value())
				{
					recordError(*state, "dlopen: unreadable filename");
					return 0;
				}
				name = std::move(text).value();
			}
			// android_dlextinfo is ignored: no reserved address, no namespace.
			auto opened = state->service->open(name, args.x[extended ? 3 : 2], args.call_guest);
			if (!opened.ok())
			{
				recordError(*state, opened.error().message);
				return 0;
			}
			return static_cast<int64_t>(opened.value());
		} };
	}

	if (symbol == "__loader_dlsym" || symbol == "__loader_dlvsym")
	{
		// __loader_dlvsym(handle, symbol, version, caller_addr): same first two
		// arguments, no symbol versions in this runtime.
		return HostStub{ [state](HostCallArgs &args) -> int64_t {
			std::lock_guard<std::recursive_mutex> lock(state->mutex);
			if (state->service == nullptr)
			{
				return 0;
			}
			if (args.x[1] == 0)
			{
				recordError(*state, "dlsym: null symbol");
				return 0;
			}
			auto text = readGuestCString(*state->memory, GuestVA{ args.x[1] });
			if (!text.has_value())
			{
				recordError(*state, "dlsym: unreadable symbol");
				return 0;
			}
			auto found = state->service->symbol(args.x[0], text.value(), args.thread_pointer, args.call_guest);
			if (!found.ok())
			{
				recordError(*state, found.error().message);
				return 0;
			}
			return static_cast<int64_t>(found.value().value());
		} };
	}

	if (symbol == "__loader_dlerror")
	{
		return HostStub{ [state](HostCallArgs &) -> int64_t {
			std::lock_guard<std::recursive_mutex> lock(state->mutex);
			if (!state->error_pending)
			{
				return 0;
			}
			state->error_pending = false;
			return static_cast<int64_t>(state->error.value());
		} };
	}

	if (symbol == "__loader_dlclose")
	{
		return HostStub{ [state](HostCallArgs &args) -> int64_t {
			std::lock_guard<std::recursive_mutex> lock(state->mutex);
			if (state->service != nullptr)
			{
				state->service->close(args.x[0]);
			}
			return 0;
		} };
	}

	if (symbol == "__loader_dladdr")
	{
		return HostStub{ [state](HostCallArgs &args) -> int64_t {
			std::lock_guard<std::recursive_mutex> lock(state->mutex);
			if (state->service == nullptr || args.x[1] == 0)
			{
				return 0;
			}
			state->scratch_used = 0;
			DlModuleInfo module;
			std::string symbol_name;
			uint64_t symbol_address = 0;
			if (!state->service->address(args.x[0], module, symbol_name, symbol_address))
			{
				return 0;
			}
			// Dl_info { dli_fname, dli_fbase, dli_sname, dli_saddr }.
			const GuestVA path = writeGuestString(*state, module.path);
			const GuestVA name = writeGuestString(*state, symbol_name);
			const uint64_t fields[4] = { path.value(), module.base, name.value(), symbol_address };
			if (!state->memory->write(GuestVA{ args.x[1] }, fields, sizeof(fields)).ok())
			{
				return 0;
			}
			return 1;
		} };
	}

	if (symbol == "__loader_dl_iterate_phdr")
	{
		return HostStub{ [state](HostCallArgs &args) -> int64_t {
			std::lock_guard<std::recursive_mutex> lock(state->mutex);
			if (state->service == nullptr || args.x[0] == 0 || !args.call_guest)
			{
				return 0;
			}
			state->scratch_used = 0;
			const uint64_t callback = args.x[0];
			const uint64_t data = args.x[1];
			for (const DlModuleInfo &module : state->service->moduleList())
			{
				const GuestVA info = allocScratch(*state, kDlPhdrInfoSize, 16);
				if (info.value() == 0)
				{
					return 0;
				}
				const std::array<uint8_t, kDlPhdrInfoSize> zero{};
				if (!state->memory->write(info, zero.data(), zero.size()).ok())
				{
					return 0;
				}
				const GuestVA name = writeGuestString(*state, module.path);
				const uint64_t fields[3] = { module.base, name.value(), module.phdr.value() };
				if (!state->memory->write(info, fields, sizeof(fields)).ok())
				{
					return 0;
				}
				const uint16_t phnum = module.phnum;
				if (!state->memory
					     ->write(GuestVA{ info.value() + kDlPhdrNumOffset }, &phnum, sizeof(phnum))
					     .ok())
				{
					return 0;
				}
				auto called = args.call_guest(GuestVA{ callback },
					{ info.value(), kDlPhdrInfoSize, data });
				if (!called.ok())
				{
					return 0;
				}
				if (called.value() != 0)
				{
					return called.value();
				}
			}
			return 0;
		} };
	}

	return std::nullopt;
}

bool VirtualBionic::isVirtualModule(std::string_view soname) const noexcept
{
	if (soname == "ld-android.so" || isImportStubModuleName(soname))
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

bool VirtualBionic::isImportStub(std::string_view soname) const noexcept
{
	return isImportStubModuleName(soname);
}

Result<GuestVA> VirtualBionic::resolveStub(std::string_view symbol)
{
	// One thunk per name, so repeated relocations share the address.
	for (const auto &entry : thunks_by_name_)
	{
		if (entry.first == symbol)
		{
			return entry.second;
		}
	}
	auto thunk = thunks_.add(std::string("stub:") + std::string(symbol),
		[](HostCallArgs &) -> int64_t { return 0; });
	if (!thunk.ok())
	{
		return thunk.error();
	}
	thunks_by_name_.emplace_back(std::string(symbol), thunk.value());
	return thunk.value();
}

bool VirtualBionic::declares(std::string_view module_soname, std::string_view symbol) const noexcept
{
	if (isImportStubModuleName(module_soname))
	{
		return true;
	}
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
	// The one linker symbol M4.6 needs for real: libc forwards
	// __libc_shared_globals() here. The block is prepared by RuntimeInstance.
	if (symbol == "__loader_shared_globals")
	{
		BionicGlobals *globals = bionic_globals_;
		return HostStub{ [globals](HostCallArgs &) -> int64_t {
			return globals != nullptr ? static_cast<int64_t>(globals->address().value()) : 0;
		} };
	}

	// The dynamic-linking entry points are real now: libc's dlopen() forwards
	// here, and an SDK that loads a sub-library at runtime depends on it.
	if (auto dl_stub = makeDlStub(symbol); dl_stub.has_value())
	{
		return std::move(dl_stub).value();
	}

	// Remaining ld-android.so symbols: binding-only stubs.
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
