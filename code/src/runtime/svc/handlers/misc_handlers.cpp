#include <cstdint>

#include "runtime/svc/handler.hpp"
#include "runtime/svc/syscall_numbers.hpp"

namespace duckemu::runtime::svc {
namespace {

int64_t sysGetpid(SyscallContext& ctx) {
    return static_cast<int64_t>(ctx.pid);
}

int64_t sysGettid(SyscallContext& ctx) {
    return static_cast<int64_t>(ctx.tid);
}

int64_t sysExit(SyscallContext& ctx) {
    ctx.requestExit(static_cast<int32_t>(ctx.args[0]));
    return 0;
}

} // namespace

std::optional<int64_t> handleMiscSyscall(SyscallContext& ctx, uint32_t number) {
    switch (number) {
    case kSysGetpid:
        return sysGetpid(ctx);
    case kSysGettid:
        return sysGettid(ctx);
    case kSysExit:
    case kSysExitGroup:
        return sysExit(ctx);
    default:
        return std::nullopt;
    }
}

} // namespace duckemu::runtime::svc
