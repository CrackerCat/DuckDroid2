// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <optional>

#include "runtime/device/device_profile.hpp"
#include "runtime/memory/address_space.hpp"

namespace duckemu::runtime
{
class RuntimeClock;
}

namespace duckemu::runtime::threads
{
class RuntimeThread;
class ThreadSystem;
}

namespace duckemu::runtime::vfs
{
class VirtualFileSystem;
}

namespace duckemu::runtime::svc
{

struct SyscallContext
{
	uint64_t args[6] = {};
	RuntimeMemory &memory;
	uint32_t pid = 0;
	uint32_t tid = 0;
	// The calling guest thread and the scheduler operations handlers may use.
	threads::RuntimeThread &thread;
	threads::ThreadSystem &threads;
	// Process-wide file namespace and descriptor table.
	vfs::VirtualFileSystem &vfs;
	// Instance-wide wall/monotonic clock; shared with Java System.
	RuntimeClock &clock;
	// Device identity (uname, getuid/prctl, ownership).
	const device::DeviceProfile &device;
};

// Each subsystem returns nullopt for "not my syscall"; the dispatcher then
// tries the next one and finally answers -ENOSYS.
std::optional<int64_t> handleMemorySyscall(SyscallContext &ctx, uint32_t number);
std::optional<int64_t> handleFsSyscall(SyscallContext &ctx, uint32_t number);
std::optional<int64_t> handleMiscSyscall(SyscallContext &ctx, uint32_t number);
std::optional<int64_t> handleThreadSyscall(SyscallContext &ctx, uint32_t number);
std::optional<int64_t> handleEventSyscall(SyscallContext &ctx, uint32_t number);

} // namespace duckemu::runtime::svc
