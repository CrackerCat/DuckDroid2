// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/clock/runtime_clock.hpp"

#include <chrono>
#include <limits>

namespace duckemu::runtime
{

uint64_t RuntimeClock::hostRealtimeNs() const
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					     std::chrono::system_clock::now().time_since_epoch())
					 .count());
}

uint64_t RuntimeClock::hostMonotonicNs() const
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					     std::chrono::steady_clock::now().time_since_epoch())
					 .count());
}

uint64_t RuntimeClock::nextRealtimeNs()
{
	if (!wall_injected_)
	{
		return hostRealtimeNs();
	}
	const uint64_t value = wall_ns_;
	wall_ns_ = wall_ns_ > std::numeric_limits<uint64_t>::max() - kAdvanceNs
				   ? std::numeric_limits<uint64_t>::max()
				   : wall_ns_ + kAdvanceNs;
	return value;
}

uint64_t RuntimeClock::nextMonotonicNs()
{
	if (!monotonic_injected_)
	{
		return hostMonotonicNs();
	}
	const uint64_t value = monotonic_ns_;
	monotonic_ns_ = monotonic_ns_ > std::numeric_limits<uint64_t>::max() - kAdvanceNs
						? std::numeric_limits<uint64_t>::max()
						: monotonic_ns_ + kAdvanceNs;
	return value;
}

void RuntimeClock::setWallTimeNs(uint64_t ns)
{
	wall_ns_ = ns;
	wall_injected_ = true;
}

void RuntimeClock::setMonotonicTimeNs(uint64_t ns)
{
	monotonic_ns_ = ns;
	monotonic_injected_ = true;
}

void RuntimeClock::useHostWallClock()
{
	wall_injected_ = false;
	wall_ns_ = 0;
}

void RuntimeClock::useHostMonotonicClock()
{
	monotonic_injected_ = false;
	monotonic_ns_ = 0;
}

} // namespace duckemu::runtime
