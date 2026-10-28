#pragma once

#include <cstdint>

#include <unicorn/unicorn.h>

#include "runtime/memory/address_space.hpp"
#include "runtime/memory/thunk/thunk_table.hpp"

namespace duckemu::runtime::svc {

class SvcDispatcher {
public:
    SvcDispatcher(RuntimeMemory& memory, ThunkTable& thunks, uint32_t pid,
                  uint32_t tid);

    // UC_HOOK_INTR entry. Decodes the trapping SVC at PC - 4:
    //   imm == 0            -> Linux syscall, number in x8, args x0..x5
    //   imm & 0x8000        -> host stub, index = imm & 0x7fff
    void onTrap(uc_engine* uc, uint32_t intno);

    void resetRunState();

    [[nodiscard]] bool exitRequested() const { return exit_requested_; }
    [[nodiscard]] int32_t exitCode() const { return exit_code_; }

    struct Stats {
        uint64_t svc_calls = 0;
        uint64_t host_calls = 0;
        uint64_t unknown = 0;
        int64_t last_syscall = -1;
    };
    [[nodiscard]] const Stats& stats() const { return stats_; }

private:
    int64_t dispatchSyscall(uint32_t number, const uint64_t (&args)[6]);
    void handleHostStub(uc_engine* uc, uint32_t index);

    RuntimeMemory& memory_;
    ThunkTable& thunks_;
    uint32_t pid_;
    uint32_t tid_;
    bool exit_requested_ = false;
    int32_t exit_code_ = 0;
    Stats stats_;
};

} // namespace duckemu::runtime::svc
