#pragma once

#include <cstdint>
#include <optional>

#include "runtime/memory/address_space.hpp"

namespace duckemu::runtime::svc {

struct SyscallContext {
    uint64_t args[6] = {};
    RuntimeMemory& memory;
    uint32_t pid = 0;
    uint32_t tid = 0;
    bool exit_requested = false;
    int32_t exit_code = 0;

    void requestExit(int32_t code) {
        exit_requested = true;
        exit_code = code;
    }
};

// Each subsystem returns nullopt for "not my syscall"; the dispatcher then
// tries the next one and finally answers -ENOSYS.
std::optional<int64_t> handleMemorySyscall(SyscallContext& ctx, uint32_t number);
std::optional<int64_t> handleFsSyscall(SyscallContext& ctx, uint32_t number);
std::optional<int64_t> handleMiscSyscall(SyscallContext& ctx, uint32_t number);

} // namespace duckemu::runtime::svc
