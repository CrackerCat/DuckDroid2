// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "common/error/result.hpp"
#include "common/id/ids.hpp"

namespace duckemu::runtime::loader
{

// Handle dlopen(NULL) returns: bionic hands back the executable's handle, and
// dlsym on it means the global scope. Our registry handles start at 1, so this
// value cannot collide with a loaded module.
constexpr uint64_t kMainProgramHandle = 0x7FFFFFFFFFFFFFFFULL;

struct DlModuleInfo
{
	std::string path;
	uint64_t base = 0;
	GuestVA phdr; // guest copy of the program header table, 0 for virtual modules
	uint16_t phnum = 0;
};

// Host side of the bionic linker entry points (__loader_*). RuntimeInstance
// implements it; VirtualBionic marshals the guest arguments and calls back.
// Every method runs on the emulating thread while the execution lock is held,
// so implementing them touches guest memory directly, without re-locking.
class DlService
{
    public:
	// Guest code invoked from inside a host stub: the same nested-engine path
	// pthread_once uses. dlopen runs the new module's constructors and IFUNC
	// resolvers through it.
	using GuestCall =
		std::function<Result<int64_t>(GuestVA entry, const std::array<uint64_t, 8> &args)>;

	virtual ~DlService() = default;

	// dlopen / android_dlopen_ext. An empty `name` is dlopen(NULL).
	// `caller_address` is the return address bionic passes in the third
	// argument; it selects the requesting module's directory for the search.
	virtual Result<uint64_t> open(std::string_view name,
		uint64_t caller_address,
		const GuestCall &call_guest) = 0;

	// dlsym. Handle 0 is RTLD_DEFAULT, kMainProgramHandle is dlopen(NULL);
	// both search every loaded module. For an STT_TLS definition bionic
	// returns the calling thread's copy (allocating a dynamic block on first
	// use), which is why the live thread pointer and guest call path are
	// threaded through here.
	virtual Result<GuestVA> symbol(uint64_t handle,
		std::string_view name,
		uint64_t thread_pointer,
		const GuestCall &call_guest) = 0;

	// dladdr. Returns false when `address` belongs to no loaded module.
	virtual bool address(uint64_t address,
		DlModuleInfo &module,
		std::string &symbol,
		uint64_t &symbol_address) = 0;

	// dl_iterate_phdr enumeration, in load order.
	virtual std::vector<DlModuleInfo> moduleList() = 0;

	// dlclose. Reference counts are not modelled: a loaded module stays until
	// the instance is destroyed.
	virtual void close(uint64_t handle) = 0;
};

} // namespace duckemu::runtime::loader
