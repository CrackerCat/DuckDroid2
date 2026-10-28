#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common/id/ids.hpp"

namespace duckemu::runtime {

inline constexpr uint64_t kGuestPageSize = 0x1000;

// Guest permission bits. Numerically identical to Linux PROT_* and Unicorn
// UC_PROT_*; no translation table is needed.
enum GuestPerm : uint32_t {
    PermNone = 0,
    PermRead = 1,
    PermWrite = 2,
    PermExec = 4,
    PermReadWrite = PermRead | PermWrite,
    PermReadExec = PermRead | PermExec,
    PermAll = PermRead | PermWrite | PermExec,
};

[[nodiscard]] constexpr uint64_t alignDown(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1);
}

[[nodiscard]] constexpr uint64_t alignUp(uint64_t value, uint64_t alignment) {
    return alignDown(value + alignment - 1, alignment);
}

[[nodiscard]] constexpr uint64_t pageDown(uint64_t value) {
    return alignDown(value, kGuestPageSize);
}

[[nodiscard]] constexpr uint64_t pageUp(uint64_t value) {
    return alignUp(value, kGuestPageSize);
}

// Host backing for one or more guest regions. mmap'ed RW and owned by the
// MemoryManager; Unicorn never frees it (RAM_PREALLOC).
struct HostMapping {
    void* base = nullptr;
    uint64_t size = 0;

    HostMapping() = default;
    HostMapping(const HostMapping&) = delete;
    HostMapping& operator=(const HostMapping&) = delete;
    ~HostMapping();
};

// A contiguous guest VA range backed by [host_offset, host_offset + size) of
// HostMapping. Splitting a region only adjusts host_offset/size; the host
// mapping is shared until the last slice is gone.
struct Region {
    GuestVA base;
    uint64_t size = 0;
    uint32_t perms = PermNone;
    std::shared_ptr<HostMapping> host;
    uint64_t host_offset = 0;
    std::string name;

    [[nodiscard]] uint64_t end() const { return base.value() + size; }

    [[nodiscard]] bool contains(uint64_t address, uint64_t length) const {
        if (address < base.value()) {
            return false;
        }
        const uint64_t offset = address - base.value();
        return offset <= size && length <= size - offset;
    }

    [[nodiscard]] uint8_t* hostPtr() const {
        return static_cast<uint8_t*>(host->base) + host_offset;
    }

    [[nodiscard]] bool executable() const { return (perms & PermExec) != 0; }
};

} // namespace duckemu::runtime
