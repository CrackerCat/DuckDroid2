#include <cstdint>

#include "runtime/svc/handler.hpp"
#include "runtime/svc/syscall_numbers.hpp"
#include "runtime/threads/thread_system.hpp"

namespace duckemu::runtime::svc
{
namespace
{

// futex(2) operation encoding. The private/realtime bits are modifiers, not
// operations; M3 implements WAIT and WAKE only.
constexpr uint32_t kFutexWait = 0;
constexpr uint32_t kFutexWake = 1;
constexpr uint32_t kFutexPrivateFlag = 128;
constexpr uint32_t kFutexClockRealtime = 256;
constexpr uint32_t kFutexCmdMask = ~(kFutexPrivateFlag | kFutexClockRealtime);

int64_t sysClone(SyscallContext &ctx)
{
	threads::CloneRequest request;
	request.flags = ctx.args[0];
	request.stack_top = GuestVA{ ctx.args[1] };
	request.parent_tid = GuestVA{ ctx.args[2] };
	request.child_tid = GuestVA{ ctx.args[3] };
	request.tls = GuestVA{ ctx.args[4] };
	return ctx.threads.cloneThread(ctx.thread, request);
}

std::optional<int64_t> sysFutex(SyscallContext &ctx)
{
	const GuestVA uaddr{ ctx.args[0] };
	const uint32_t operation = static_cast<uint32_t>(ctx.args[1]) & kFutexCmdMask;
	const uint32_t value = static_cast<uint32_t>(ctx.args[2]);

	switch (operation)
	{
	case kFutexWait:
		return ctx.threads.futexWait(ctx.thread, uaddr, value, ctx.args[3]);
	case kFutexWake:
		return ctx.threads.futexWake(uaddr, static_cast<int32_t>(value));
	default:
		// WAIT_BITSET, PI, requeue, ... are not implemented in M3.
		return std::nullopt;
	}
}

int64_t sysSetTidAddress(SyscallContext &ctx)
{
	ctx.threads.setClearChildTid(ctx.thread, GuestVA{ ctx.args[0] });
	return static_cast<int64_t>(ctx.tid);
}

int64_t sysExit(SyscallContext &ctx)
{
	ctx.threads.exitThread(ctx.thread, static_cast<int32_t>(ctx.args[0]));
	return 0;
}

int64_t sysExitGroup(SyscallContext &ctx)
{
	ctx.threads.exitGroup(ctx.thread, static_cast<int32_t>(ctx.args[0]));
	return 0;
}

} // namespace

std::optional<int64_t> handleThreadSyscall(SyscallContext &ctx, uint32_t number)
{
	switch (number)
	{
	case kSysClone:
		return sysClone(ctx);
	case kSysFutex:
		return sysFutex(ctx);
	case kSysSetTidAddress:
		return sysSetTidAddress(ctx);
	case kSysExit:
		return sysExit(ctx);
	case kSysExitGroup:
		return sysExitGroup(ctx);
	default:
		return std::nullopt;
	}
}

} // namespace duckemu::runtime::svc
