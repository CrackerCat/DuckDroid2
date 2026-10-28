#include <cstdint>
#include <vector>

#include <unistd.h>

#include "common/log/logger.hpp"
#include "runtime/memory/address_space.hpp"
#include "runtime/svc/handler.hpp"
#include "runtime/svc/syscall_numbers.hpp"

namespace duckemu::runtime::svc {
namespace {

constexpr uint64_t kMaxIoBytes = 1ULL << 20; // 1 MiB per call in M1

int64_t sysWrite(SyscallContext& ctx) {
    const uint64_t fd = ctx.args[0];
    const uint64_t buffer_address = ctx.args[1];
    const uint64_t count = ctx.args[2];

    if (fd != 1 && fd != 2) {
        return kError(kEbadf);
    }
    if (count > kMaxIoBytes) {
        return kError(kEinval);
    }

    std::vector<uint8_t> buffer(static_cast<size_t>(count));
    auto copied =
        ctx.memory.read(GuestVA{buffer_address}, buffer.data(), count);
    if (!copied.ok()) {
        return kError(kEfault);
    }

    const ssize_t written =
        ::write(static_cast<int>(fd), buffer.data(), buffer.size());
    if (written < 0) {
        DUCKEMU_WARN("host write(fd=%llu) failed",
                     static_cast<unsigned long long>(fd));
        return kError(kEio);
    }
    return static_cast<int64_t>(written);
}

int64_t sysRead(SyscallContext& ctx) {
    const uint64_t fd = ctx.args[0];
    const uint64_t buffer_address = ctx.args[1];
    const uint64_t count = ctx.args[2];

    if (fd != 0) {
        return kError(kEbadf);
    }
    if (count > kMaxIoBytes) {
        return kError(kEinval);
    }

    std::vector<uint8_t> buffer(static_cast<size_t>(count));
    const ssize_t received = ::read(0, buffer.data(), buffer.size());
    if (received < 0) {
        return kError(kEio);
    }

    auto copied = ctx.memory.write(GuestVA{buffer_address}, buffer.data(),
                                   static_cast<uint64_t>(received));
    if (!copied.ok()) {
        return kError(kEfault);
    }
    return static_cast<int64_t>(received);
}

int64_t sysClose(SyscallContext& ctx) {
    const uint64_t fd = ctx.args[0];
    // Do not close the host's stdio out from under the tests.
    if (fd <= 2) {
        return 0;
    }
    return kError(kEbadf);
}

} // namespace

std::optional<int64_t> handleFsSyscall(SyscallContext& ctx, uint32_t number) {
    switch (number) {
    case kSysWrite:
        return sysWrite(ctx);
    case kSysRead:
        return sysRead(ctx);
    case kSysClose:
        return sysClose(ctx);
    default:
        return std::nullopt;
    }
}

} // namespace duckemu::runtime::svc
