#include "runtime/threads/tls/tls_manager.hpp"

#include <random>

#include "common/log/logger.hpp"
#include "runtime/memory/address_space.hpp"
#include "runtime/threads/tls/tls_layout_android13.hpp"

namespace duckemu::runtime::tls {
namespace {

using namespace android13;

// One region holding the TCB, DTV, bionic_tls and pthread_internal_t.
constexpr uint64_t kDtvOffset = 0x100;
constexpr uint64_t kBionicTlsOffset = 0x200;
constexpr uint64_t kThreadOffset = 0x3200;
constexpr uint64_t kRegionSize = 0x4000;

GuestVA slotAddress(GuestVA thread_pointer, int slot) {
    const auto tp = static_cast<int64_t>(thread_pointer.value());
    return GuestVA{static_cast<uint64_t>(tp + static_cast<int64_t>(slot) * 8)};
}

Result<void> writeU64(RuntimeMemory& memory, GuestVA address, uint64_t value) {
    return memory.write(address, &value, sizeof(value));
}

Result<void> writeU32(RuntimeMemory& memory, GuestVA address, uint32_t value) {
    return memory.write(address, &value, sizeof(value));
}

Result<void> writeI32(RuntimeMemory& memory, GuestVA address, int32_t value) {
    return memory.write(address, &value, sizeof(value));
}

} // namespace

TlsManager::TlsManager(RuntimeMemory& memory) : memory_(memory) {}

uint64_t TlsManager::makeStackGuard() {
    std::random_device device;
    uint64_t guard = (static_cast<uint64_t>(device()) << 32) ^ device();
    if (guard == 0) {
        guard = 0x9e3779b97f4a7c15ULL;
    }
    return guard;
}

Result<TlsInstance> TlsManager::create(uint32_t tid, uint32_t pid,
                                       uint64_t stack_top) {
    auto region = memory_.map(kRegionSize, PermReadWrite, "tls");
    if (!region.ok()) {
        return region.error();
    }

    TlsInstance instance;
    instance.region_base = region.value();
    instance.thread_pointer =
        GuestVA{region.value().value() + kThreadPointerOffset};
    instance.dtv = GuestVA{region.value().value() + kDtvOffset};
    instance.bionic_tls = GuestVA{region.value().value() + kBionicTlsOffset};
    instance.pthread_internal = GuestVA{region.value().value() + kThreadOffset};
    instance.stack_guard = makeStackGuard();

    // TCB slots (slot -2 starts at the beginning of the region).
    if (auto r = writeU64(memory_, slotAddress(instance.thread_pointer, kSlotDtv),
                          instance.dtv.value());
        !r.ok()) {
        return r.error();
    }
    if (auto r = writeU64(memory_,
                          slotAddress(instance.thread_pointer, kSlotThreadId),
                          instance.pthread_internal.value());
        !r.ok()) {
        return r.error();
    }
    if (auto r = writeU64(memory_,
                          slotAddress(instance.thread_pointer, kSlotBionicTls),
                          instance.bionic_tls.value());
        !r.ok()) {
        return r.error();
    }
    if (auto r = writeU64(memory_,
                          slotAddress(instance.thread_pointer, kSlotStackGuard),
                          instance.stack_guard);
        !r.ok()) {
        return r.error();
    }

    // pthread_internal_t: only the fields M1 needs; everything else is zero.
    const GuestVA thread = instance.pthread_internal;
    if (auto r = writeU32(memory_, GuestVA{thread.value() + kThreadTid}, tid);
        !r.ok()) {
        return r.error();
    }
    if (auto r = writeU32(memory_, GuestVA{thread.value() + kThreadCachedPid},
                          pid & 0x7FFFFFFFu);
        !r.ok()) {
        return r.error();
    }
    if (auto r = writeU64(memory_, GuestVA{thread.value() + kThreadStackTop},
                          stack_top);
        !r.ok()) {
        return r.error();
    }
    if (auto r = writeU64(memory_, GuestVA{thread.value() + kThreadBionicTls},
                          instance.bionic_tls.value());
        !r.ok()) {
        return r.error();
    }
    if (auto r = writeI32(memory_, GuestVA{thread.value() + kThreadErrno}, 0);
        !r.ok()) {
        return r.error();
    }

    main_ = instance;
    DUCKEMU_DEBUG("TLS: tp=0x%llx dtv=0x%llx thread=0x%llx bionic_tls=0x%llx",
                  static_cast<unsigned long long>(instance.thread_pointer.value()),
                  static_cast<unsigned long long>(instance.dtv.value()),
                  static_cast<unsigned long long>(instance.pthread_internal.value()),
                  static_cast<unsigned long long>(instance.bionic_tls.value()));
    return instance;
}

Result<void> TlsManager::setErrno(int32_t value) {
    if (main_.pthread_internal.value() == 0) {
        return Error{ErrorCode::Internal, "TLS instance is not initialized"};
    }
    return writeI32(
        memory_, GuestVA{main_.pthread_internal.value() + kThreadErrno}, value);
}

} // namespace duckemu::runtime::tls
