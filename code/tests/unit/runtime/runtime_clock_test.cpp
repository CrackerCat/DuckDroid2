// SPDX-License-Identifier: GPL-2.0-only
// Deterministic clock (DuckDroid parity): injected values start at the base
// and advance exactly 1 ms per read; host mode still returns real time.

#include <cstdint>

#include "runtime/clock/runtime_clock.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

int main()
{
	RuntimeClock clock;

	// Host mode: plausible epoch nanoseconds, and it is not stuck.
	CHECK(!clock.wallTimeInjected());
	const uint64_t host_now = clock.nextRealtimeNs();
	CHECK(host_now > 1500000000000000000ULL);
	CHECK(!clock.monotonicTimeInjected());
	const uint64_t host_mono = clock.nextMonotonicNs();
	CHECK(host_mono > 0);

	// Injected wall clock: base, base+1ms, base+2ms.
	constexpr uint64_t kBase = 1792886400000000000ULL;
	clock.setWallTimeNs(kBase);
	CHECK(clock.wallTimeInjected());
	CHECK_EQ(clock.nextRealtimeNs(), kBase);
	CHECK_EQ(clock.nextRealtimeNs(), kBase + 1000000ULL);
	CHECK_EQ(clock.nextRealtimeNs(), kBase + 2000000ULL);

	// Injected monotonic clock is independent.
	CHECK(!clock.monotonicTimeInjected());
	clock.setMonotonicTimeNs(123000000ULL);
	CHECK_EQ(clock.nextMonotonicNs(), 123000000ULL);
	CHECK_EQ(clock.nextMonotonicNs(), 124000000ULL);
	CHECK_EQ(clock.nextRealtimeNs(), kBase + 3000000ULL);

	// Zero is a valid injected base (epoch), not "host".
	clock.setWallTimeNs(0);
	CHECK(clock.wallTimeInjected());
	CHECK_EQ(clock.nextRealtimeNs(), 0);
	CHECK_EQ(clock.nextRealtimeNs(), 1000000ULL);

	// Restoring host time stops the deterministic sequence.
	clock.useHostWallClock();
	CHECK(!clock.wallTimeInjected());
	CHECK(clock.nextRealtimeNs() > 1500000000000000000ULL);
	clock.useHostMonotonicClock();
	CHECK(!clock.monotonicTimeInjected());

	// Saturating advance: never wraps below the base.
	clock.setWallTimeNs(UINT64_MAX - 1);
	const uint64_t near_max = clock.nextRealtimeNs();
	CHECK_EQ(near_max, UINT64_MAX - 1);
	CHECK_EQ(clock.nextRealtimeNs(), UINT64_MAX);
	CHECK_EQ(clock.nextRealtimeNs(), UINT64_MAX);

	return test::finish("runtime_clock_test");
}
