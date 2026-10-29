// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>

namespace duckemu::runtime::observer
{

// Instance-wide SVC ordering counters. `seq` is assigned when a dispatch is
// entered, `id` when its event is published. Both are written only by the
// thread that holds the instance execution lock, so plain integers are enough.
struct TraceClock
{
	uint64_t next_seq = 0;
	uint64_t next_id = 0;
};

} // namespace duckemu::runtime::observer
