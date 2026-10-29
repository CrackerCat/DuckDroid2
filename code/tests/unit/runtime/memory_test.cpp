// SPDX-License-Identifier: GPL-2.0-only
// Unit tests for the M1 MemoryManager: region table, split/merge behaviour,
// MAP_FIXED replacement, cross-region access and brk.

#include <cstdint>
#include <cstring>

#include "runtime/memory/address_space.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

namespace {

void runMemoryTests() {
    RuntimeMemory memory;

    // --- mapFixed + host access ---
    auto three = memory.mapFixed(GuestVA{0x100000}, 3 * kGuestPageSize,
                                 PermReadWrite, "three", false);
    CHECK_RESULT(three);
    if (!three.ok()) {
        return;
    }
    CHECK_EQ(three.value().value(), 0x100000);

    const uint64_t value = 0x1122334455667788ULL;
    CHECK_RESULT(memory.write(GuestVA{0x100000}, &value, sizeof(value)));
    uint64_t readback = 0;
    CHECK_RESULT(memory.read(GuestVA{0x100000}, &readback, sizeof(readback)));
    CHECK_EQ(readback, value);

    // --- protect splits the region, perms are per subrange ---
    CHECK_RESULT(memory.protect(GuestVA{0x101000}, kGuestPageSize, PermRead));
    CHECK_EQ(memory.permissionsAt(GuestVA{0x100800}), PermReadWrite);
    CHECK_EQ(memory.permissionsAt(GuestVA{0x101000}), PermRead);
    CHECK_EQ(memory.permissionsAt(GuestVA{0x102000}), PermReadWrite);

    // Host writes bypass guest permissions (the host is trusted).
    CHECK_RESULT(memory.write(GuestVA{0x101000}, &value, sizeof(value)));

    // --- unmap a middle page ---
    CHECK_RESULT(memory.unmap(GuestVA{0x101000}, kGuestPageSize));
    CHECK(!memory.isMapped(GuestVA{0x101000}));
    uint64_t scratch = 0;
    CHECK(!memory.read(GuestVA{0x101000}, &scratch, sizeof(scratch)).ok());
    CHECK_EQ(memory.permissionsAt(GuestVA{0x100000}), PermReadWrite);
    CHECK_EQ(memory.permissionsAt(GuestVA{0x102000}), PermReadWrite);

    // --- MAP_FIXED replacement of [0x100000, 0x102000) ---
    auto replaced = memory.mapFixed(GuestVA{0x100000}, 2 * kGuestPageSize,
                                    PermRead, "replaced", true);
    CHECK_RESULT(replaced);
    CHECK_EQ(memory.permissionsAt(GuestVA{0x100000}), PermRead);
    CHECK_EQ(memory.permissionsAt(GuestVA{0x101000}), PermRead);
    CHECK_EQ(memory.permissionsAt(GuestVA{0x102000}), PermReadWrite);

    // Fresh anonymous backing: old contents are gone.
    readback = 0;
    CHECK_RESULT(memory.read(GuestVA{0x100000}, &readback, sizeof(readback)));
    CHECK_EQ(readback, 0);

    // --- one access spanning two adjacent regions ---
    CHECK_RESULT(memory.mapFixed(GuestVA{0x200000}, kGuestPageSize,
                                 PermReadWrite, "left", false));
    CHECK_RESULT(memory.mapFixed(GuestVA{0x201000}, kGuestPageSize,
                                 PermReadWrite, "right", false));

    uint8_t span[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    CHECK_RESULT(memory.write(GuestVA{0x200FF8}, span, sizeof(span)));
    uint8_t span_read[16] = {};
    CHECK_RESULT(memory.read(GuestVA{0x200FF8}, span_read, sizeof(span_read)));
    CHECK_EQ(std::memcmp(span, span_read, sizeof(span)), 0);

    // --- MAP_FIXED without replace must refuse an occupied range ---
    auto conflict = memory.mapFixed(GuestVA{0x200000}, kGuestPageSize,
                                    PermReadWrite, "conflict", false);
    CHECK(!conflict.ok());

    // --- unmap of a whole region frees it ---
    CHECK_RESULT(memory.unmap(GuestVA{0x200000}, kGuestPageSize));
    CHECK(!memory.isMapped(GuestVA{0x200000}));
    CHECK(memory.isMapped(GuestVA{0x201000}));

    // --- brk ---
    auto brk_start = memory.brk(GuestVA{0});
    CHECK_RESULT(brk_start);
    CHECK_EQ(brk_start.value().value(), 0x30000000);
    auto brk_new = memory.brk(GuestVA{0x30000100});
    CHECK_RESULT(brk_new);
    CHECK_EQ(brk_new.value().value(), 0x30000100);
    // Out-of-range brk returns the old break.
    auto brk_bad = memory.brk(GuestVA{0x90000000});
    CHECK_RESULT(brk_bad);
    CHECK_EQ(brk_bad.value().value(), 0x30000100);
}

} // namespace

int main() {
    runMemoryTests();
    return test::finish("memory_test");
}
