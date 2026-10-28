#include <cstdint>

#include "runtime/svc/handler.hpp"
#include "runtime/svc/syscall_numbers.hpp"

namespace duckemu::runtime::svc
{
namespace
{

int64_t sysGetpid(SyscallContext &ctx)
{
	return static_cast<int64_t>(ctx.pid);
}

int64_t sysGettid(SyscallContext &ctx)
{
	return static_cast<int64_t>(ctx.tid);
}

} // namespace

// exit/exit_group moved to thread_handlers.cpp: they act on the calling
// RuntimeThread and, for exit_group, on the whole instance.
std::optional<int64_t> handleMiscSyscall(SyscallContext &ctx, uint32_t number)
{
	switch (number)
	{
	case kSysGetpid:
		return sysGetpid(ctx);
	case kSysGettid:
		return sysGettid(ctx);
	default:
		return std::nullopt;
	}
}

} // namespace duckemu::runtime::svc
