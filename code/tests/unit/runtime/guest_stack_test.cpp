#include <cstdint>

#include "runtime/memory/address_space.hpp"
#include "runtime/observer/diagnostic/guest_stack.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

namespace {

void testCompleteChain() {
    RuntimeMemory memory;
    auto stack = memory.map(0x1000, PermReadWrite, "stack");
    auto code = memory.map(0x1000, PermReadExec, "code");
    CHECK_RESULT(stack);
    CHECK_RESULT(code);
    if (!stack.ok() || !code.ok()) {
        return;
    }

    const uint64_t frame_address = stack.value().value() + 0x1000 - 16;
    const uint64_t caller = code.value().value() + 0x20;
    const uint64_t faulting = code.value().value() + 0x40;

    const uint64_t record[2] = { 0, caller };
    CHECK_RESULT(memory.write(GuestVA{ frame_address }, record, sizeof(record)));

    const auto walk = observer::unwindGuestStack(memory, faulting, frame_address, frame_address);
    CHECK_EQ(walk.frames.size(), 2);
    CHECK(!walk.truncated);
    CHECK(walk.stop == "fp=0");
    if (walk.frames.size() == 2) {
        CHECK_EQ(walk.frames[0].pc, faulting);
        CHECK_EQ(walk.frames[1].pc, caller);
        CHECK(walk.frames[1].name == "code!0x20");
    }
}

void testNoFramePointer() {
    RuntimeMemory memory;
    auto stack = memory.map(0x1000, PermReadWrite, "stack");
    CHECK_RESULT(stack);
    if (!stack.ok()) {
        return;
    }

    const auto walk = observer::unwindGuestStack(memory, 0x40100010, 0, stack.value().value());
    CHECK_EQ(walk.frames.size(), 1);
    CHECK(!walk.truncated);
    CHECK(walk.stop == "fp=0");
    CHECK(walk.frames[0].name == "0x40100010");
}

void testUnalignedFramePointer() {
    RuntimeMemory memory;
    auto stack = memory.map(0x1000, PermReadWrite, "stack");
    CHECK_RESULT(stack);
    if (!stack.ok()) {
        return;
    }

    const uint64_t sp = stack.value().value() + 0x800;
    const auto walk = observer::unwindGuestStack(memory, 0x40100010, sp + 8, sp);
    CHECK_EQ(walk.frames.size(), 1);
    CHECK(walk.truncated);
    CHECK(walk.stop == "unaligned fp");
}

void testFramePointerOutsideStack() {
    RuntimeMemory memory;
    auto stack = memory.map(0x1000, PermReadWrite, "stack");
    auto code = memory.map(0x1000, PermReadExec, "code");
    CHECK_RESULT(stack);
    CHECK_RESULT(code);
    if (!stack.ok() || !code.ok()) {
        return;
    }

    const auto walk = observer::unwindGuestStack(
        memory, code.value().value() + 0x10, code.value().value(), stack.value().value() + 0x800);
    CHECK_EQ(walk.frames.size(), 1);
    CHECK(walk.truncated);
    CHECK(walk.stop == "fp outside stack");
}

void testSelfReferentialChainHitsLimit() {
    RuntimeMemory memory;
    auto stack = memory.map(0x1000, PermReadWrite, "stack");
    CHECK_RESULT(stack);
    if (!stack.ok()) {
        return;
    }

    const uint64_t fp = stack.value().value() + 0x1000 - 16;
    const uint64_t record[2] = { fp, 0x40100020 };
    CHECK_RESULT(memory.write(GuestVA{ fp }, record, sizeof(record)));

    const auto walk = observer::unwindGuestStack(memory, 0x40100010, fp, fp);
    CHECK_EQ(walk.frames.size(), observer::kMaxGuestFrames);
    CHECK(walk.truncated);
    CHECK(walk.stop == "frame limit");
}

void testUnmappedStackPointer() {
    RuntimeMemory memory;
    const auto walk = observer::unwindGuestStack(memory, 0x40100010, 0xdeadfff0, 0xdeadfff0);
    CHECK_EQ(walk.frames.size(), 1);
    CHECK(walk.truncated);
    CHECK(walk.stop == "sp unmapped");
}

} // namespace

int main() {
    testCompleteChain();
    testNoFramePointer();
    testUnalignedFramePointer();
    testFramePointerOutsideStack();
    testSelfReferentialChainHitsLimit();
    testUnmappedStackPointer();
    return test::finish("guest_stack_test");
}
