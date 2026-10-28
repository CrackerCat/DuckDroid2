#include "runtime/svc/dispatcher.hpp"

#include <optional>

#include "common/log/logger.hpp"
#include "runtime/svc/handler.hpp"
#include "runtime/svc/syscall_numbers.hpp"

namespace duckemu::runtime::svc {
namespace {

constexpr uint32_t kSvcInstructionMask = 0xFFE0001Fu;
constexpr uint32_t kSvcInstruction = 0xD4000001u;
constexpr uint32_t kHostStubBase = 0x8000u;
constexpr uint32_t kHostStubIndexMask = 0x7FFFu;
constexpr uint32_t kIntrSwi = 2;

constexpr int kArgRegs[8] = {
    UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3,
    UC_ARM64_REG_X4, UC_ARM64_REG_X5, UC_ARM64_REG_X6, UC_ARM64_REG_X7,
};

Result<uint64_t> readRegister(uc_engine* uc, int reg) {
    uint64_t value = 0;
    const uc_err err = uc_reg_read(uc, reg, &value);
    if (err != UC_ERR_OK) {
        return Error{ErrorCode::EmulationFault, uc_strerror(err)};
    }
    return value;
}

Result<void> writeRegister(uc_engine* uc, int reg, uint64_t value) {
    const uc_err err = uc_reg_write(uc, reg, &value);
    if (err != UC_ERR_OK) {
        return Error{ErrorCode::EmulationFault, uc_strerror(err)};
    }
    return {};
}

} // namespace

SvcDispatcher::SvcDispatcher(RuntimeMemory& memory, ThunkTable& thunks,
                             uint32_t pid, uint32_t tid)
    : memory_(memory), thunks_(thunks), pid_(pid), tid_(tid) {}

void SvcDispatcher::resetRunState() {
    exit_requested_ = false;
    exit_code_ = 0;
}

void SvcDispatcher::onTrap(uc_engine* uc, uint32_t intno) {
    if (intno != kIntrSwi) {
        DUCKEMU_ERROR("unexpected UC_HOOK_INTR intno=%u (only SVC is handled)",
                      intno);
        uc_emu_stop(uc);
        return;
    }

    auto pc = readRegister(uc, UC_ARM64_REG_PC);
    if (!pc.ok() || pc.value() < 4) {
        DUCKEMU_ERROR("cannot read PC in the SVC hook");
        uc_emu_stop(uc);
        return;
    }

    auto instruction = memory_.readU32(GuestVA{pc.value() - 4});
    if (!instruction.ok() ||
        (instruction.value() & kSvcInstructionMask) != kSvcInstruction) {
        DUCKEMU_ERROR("UC_HOOK_INTR at 0x%llx but PC-4 is not an SVC",
                      static_cast<unsigned long long>(pc.value()));
        uc_emu_stop(uc);
        return;
    }

    const uint32_t imm = (instruction.value() >> 5) & 0xFFFFu;

    if (imm == 0) {
        auto number = readRegister(uc, UC_ARM64_REG_X8);
        if (!number.ok()) {
            uc_emu_stop(uc);
            return;
        }

        uint64_t args[6] = {};
        for (size_t i = 0; i < 6; ++i) {
            auto value = readRegister(uc, kArgRegs[i]);
            if (!value.ok()) {
                uc_emu_stop(uc);
                return;
            }
            args[i] = value.value();
        }

        const int64_t result =
            dispatchSyscall(static_cast<uint32_t>(number.value()), args);
        if (auto written = writeRegister(uc, UC_ARM64_REG_X0,
                                         static_cast<uint64_t>(result));
            !written.ok()) {
            uc_emu_stop(uc);
            return;
        }

        if (exit_requested_) {
            uc_emu_stop(uc);
        }
        return;
    }

    if ((imm & kHostStubBase) != 0) {
        handleHostStub(uc, imm & kHostStubIndexMask);
        return;
    }

    DUCKEMU_ERROR("unsupported SVC immediate 0x%x at 0x%llx", imm,
                  static_cast<unsigned long long>(pc.value() - 4));
    uc_emu_stop(uc);
}

int64_t SvcDispatcher::dispatchSyscall(uint32_t number,
                                       const uint64_t (&args)[6]) {
    ++stats_.svc_calls;
    stats_.last_syscall = number;

    SyscallContext ctx{{}, memory_, pid_, tid_};
    for (size_t i = 0; i < 6; ++i) {
        ctx.args[i] = args[i];
    }

    std::optional<int64_t> result = handleMemorySyscall(ctx, number);
    if (!result.has_value()) {
        result = handleFsSyscall(ctx, number);
    }
    if (!result.has_value()) {
        result = handleMiscSyscall(ctx, number);
    }

    int64_t value = 0;
    if (result.has_value()) {
        value = result.value();
    } else {
        ++stats_.unknown;
        DUCKEMU_WARN("unimplemented syscall %u (x8), returning -ENOSYS", number);
        value = kError(kEnosys);
    }

    if (ctx.exit_requested) {
        exit_requested_ = true;
        exit_code_ = ctx.exit_code;
    }
    return value;
}

void SvcDispatcher::handleHostStub(uc_engine* uc, uint32_t index) {
    HostCallArgs call;
    for (size_t i = 0; i < call.x.size(); ++i) {
        auto value = readRegister(uc, kArgRegs[i]);
        if (!value.ok()) {
            uc_emu_stop(uc);
            return;
        }
        call.x[i] = value.value();
    }

    ++stats_.host_calls;
    auto result = thunks_.invoke(index, call);
    if (!result.ok()) {
        DUCKEMU_ERROR("host stub %u is not registered", index);
        if (auto written =
                writeRegister(uc, UC_ARM64_REG_X0,
                              static_cast<uint64_t>(kError(kEnosys)));
            !written.ok()) {
            uc_emu_stop(uc);
        }
        return;
    }

    if (auto written = writeRegister(uc, UC_ARM64_REG_X0,
                                     static_cast<uint64_t>(result.value()));
        !written.ok()) {
        uc_emu_stop(uc);
    }
}

} // namespace duckemu::runtime::svc
