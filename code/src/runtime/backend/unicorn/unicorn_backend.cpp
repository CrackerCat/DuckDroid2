#include "runtime/backend/unicorn/unicorn_backend.hpp"

#include <utility>

#include "common/log/logger.hpp"
#include "runtime/memory/address_space.hpp"

namespace duckemu::runtime {
namespace {

Error unicornError(uc_err err, const char* what) {
    return Error{ErrorCode::EmulationFault,
                 std::string(what) + ": " + uc_strerror(err)};
}

} // namespace

UnicornBackend::UnicornBackend(uc_engine* engine, RuntimeMemory& memory)
    : engine_(engine), memory_(memory) {}

UnicornBackend::~UnicornBackend() {
    if (engine_ != nullptr) {
        uc_close(engine_);
    }
}

Result<std::unique_ptr<UnicornBackend>> UnicornBackend::create(
    RuntimeMemory& memory) {
    uc_engine* engine = nullptr;
    const uc_err open_err = uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &engine);
    if (open_err != UC_ERR_OK) {
        return unicornError(open_err, "uc_open");
    }

    // M0 measured Unicorn's default ARM64 page size as 0x400 (TARGET_PAGE_BITS_MIN
    // = 10). Android's guest pages are 4 KiB; align the emulator with that
    // before the engine is initialized by the first map/run.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
    const uc_err page_err = uc_ctl_set_page_size(engine, static_cast<uint32_t>(kGuestPageSize));
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
    if (page_err != UC_ERR_OK) {
        uc_close(engine);
        return unicornError(page_err, "uc_ctl_set_page_size");
    }

    std::unique_ptr<UnicornBackend> backend(new UnicornBackend(engine, memory));

    const uc_err intr_err =
        uc_hook_add(engine, &backend->intr_hook_, UC_HOOK_INTR,
                    reinterpret_cast<void*>(&UnicornBackend::onIntr), backend.get(),
                    1, 0);
    if (intr_err != UC_ERR_OK) {
        return unicornError(intr_err, "uc_hook_add(UC_HOOK_INTR)");
    }

    const uc_err write_err =
        uc_hook_add(engine, &backend->mem_write_hook_, UC_HOOK_MEM_WRITE,
                    reinterpret_cast<void*>(&UnicornBackend::onMemWrite),
                    backend.get(), 1, 0);
    if (write_err != UC_ERR_OK) {
        return unicornError(write_err, "uc_hook_add(UC_HOOK_MEM_WRITE)");
    }

    return backend;
}

void UnicornBackend::setSvcTrapHandler(
    std::function<void(uc_engine*, uint32_t)> handler) {
    svc_handler_ = std::move(handler);
}

Result<RunOutcome> UnicornBackend::run(GuestVA begin, GuestVA until,
                                       uint64_t timeout_us) {
    RunOutcome outcome;
    outcome.err = uc_emu_start(engine_, begin.value(), until.value(), timeout_us, 0);
    auto pc = readRegister(UC_ARM64_REG_PC);
    if (!pc.ok()) {
        return pc.error();
    }
    outcome.pc = pc.value();
    return outcome;
}

void UnicornBackend::stop() { uc_emu_stop(engine_); }

Result<uint64_t> UnicornBackend::readRegister(int reg) const {
    uint64_t value = 0;
    const uc_err err = uc_reg_read(engine_, reg, &value);
    if (err != UC_ERR_OK) {
        return unicornError(err, "uc_reg_read");
    }
    return value;
}

Result<void> UnicornBackend::writeRegister(int reg, uint64_t value) {
    const uc_err err = uc_reg_write(engine_, reg, &value);
    if (err != UC_ERR_OK) {
        return unicornError(err, "uc_reg_write");
    }
    return {};
}

void UnicornBackend::invalidateCode(GuestVA address, uint64_t size) {
    if (size == 0) {
        return;
    }
    const uint64_t start = pageDown(address.value());
    const uint64_t end = pageUp(address.value() + size);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
    const uc_err err = uc_ctl_remove_cache(engine_, start, end);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
    if (err != UC_ERR_OK) {
        DUCKEMU_WARN("uc_ctl_remove_cache(0x%llx, 0x%llx) failed: %s",
                     static_cast<unsigned long long>(start),
                     static_cast<unsigned long long>(end), uc_strerror(err));
    }
}

Result<void> UnicornBackend::onMapped(const Region& region) {
    const uc_err err = uc_mem_map_ptr(engine_, region.base.value(), region.size,
                                      region.perms, region.hostPtr());
    if (err != UC_ERR_OK) {
        return unicornError(err, "uc_mem_map_ptr");
    }
    return {};
}

Result<void> UnicornBackend::onUnmapped(GuestVA address, uint64_t size) {
    const uc_err err = uc_mem_unmap(engine_, address.value(), size);
    if (err != UC_ERR_OK) {
        return unicornError(err, "uc_mem_unmap");
    }
    return {};
}

Result<void> UnicornBackend::onProtected(GuestVA address, uint64_t size,
                                         uint32_t perms) {
    const uc_err err = uc_mem_protect(engine_, address.value(), size, perms);
    if (err != UC_ERR_OK) {
        return unicornError(err, "uc_mem_protect");
    }
    return {};
}

void UnicornBackend::onCodeWritten(GuestVA address, uint64_t size) {
    invalidateCode(address, size);
}

void UnicornBackend::onIntr(uc_engine* uc, uint32_t intno, void* user_data) {
    auto* backend = static_cast<UnicornBackend*>(user_data);
    if (backend->svc_handler_) {
        backend->svc_handler_(uc, intno);
        return;
    }
    DUCKEMU_ERROR("UC_HOOK_INTR (intno=%u) without a handler; stopping", intno);
    uc_emu_stop(uc);
}

bool UnicornBackend::onMemWrite(uc_engine* uc, uc_mem_type type, uint64_t address,
                                int size, int64_t value, void* user_data) {
    (void)uc;
    (void)type;
    (void)value;
    auto* backend = static_cast<UnicornBackend*>(user_data);
    if (size <= 0) {
        return true;
    }

    const uint64_t first_page = pageDown(address);
    const uint64_t last_page =
        pageDown(address + static_cast<uint64_t>(size) - 1);
    for (uint64_t page = first_page;; page += kGuestPageSize) {
        if (backend->memory_.isExecutable(GuestVA{page})) {
            backend->invalidateCode(GuestVA{page}, kGuestPageSize);
        }
        if (page >= last_page) {
            break;
        }
    }
    return true;
}

} // namespace duckemu::runtime
