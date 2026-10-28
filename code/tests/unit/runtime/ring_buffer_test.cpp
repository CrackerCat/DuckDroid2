#include <cstdint>
#include <thread>
#include <vector>

#include "runtime/observer/diagnostic/ring_buffer.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using duckemu::runtime::observer::RingBuffer;

namespace {

void testDropKeepsOldest() {
    RingBuffer<uint32_t> ring(8);
    CHECK_EQ(ring.capacity(), 8);

    for (uint32_t i = 0; i < 20; ++i) {
        (void)ring.tryPush(i);
    }

    CHECK_EQ(ring.size(), 8);
    CHECK_EQ(ring.dropped(), 12);

    uint32_t value = 0;
    for (uint32_t expected = 0; expected < 8; ++expected) {
        CHECK(ring.tryPop(value));
        CHECK_EQ(value, expected);
    }
    CHECK(!ring.tryPop(value));
    CHECK_EQ(ring.size(), 0);
}

void testCapacityRounding() {
    RingBuffer<uint32_t> odd(5);
    CHECK_EQ(odd.capacity(), 8);

    RingBuffer<uint32_t> empty(0);
    CHECK_EQ(empty.capacity(), 2);

    RingBuffer<uint32_t> exact(16);
    CHECK_EQ(exact.capacity(), 16);
}

void testConcurrentOrderAndNoLoss() {
    // Capacity above the item count: tryPush cannot overflow, so the test can
    // assert strict ordering with no loss. Overflow itself is covered by
    // testDropKeepsOldest.
    constexpr uint32_t kCount = 100000;
    RingBuffer<uint32_t> ring(1u << 17);

    std::thread producer([&ring]() {
        for (uint32_t i = 0; i < kCount;) {
            if (ring.tryPush(i)) {
                ++i;
            }
        }
    });

    std::vector<uint32_t> received;
    received.reserve(kCount);
    uint32_t value = 0;
    while (received.size() < kCount) {
        if (ring.tryPop(value)) {
            received.push_back(value);
        }
    }
    producer.join();

    CHECK_EQ(ring.dropped(), 0);
    CHECK_EQ(received.size(), kCount);
    bool ordered = true;
    for (uint32_t i = 0; i < received.size(); ++i) {
        if (received[i] != i) {
            ordered = false;
            break;
        }
    }
    CHECK(ordered);
}

} // namespace

int main() {
    testDropKeepsOldest();
    testCapacityRounding();
    testConcurrentOrderAndNoLoss();
    return test::finish("ring_buffer_test");
}
