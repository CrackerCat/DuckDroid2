// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <vector>

#include "common/error/result.hpp"

namespace duckemu::runtime
{
class RuntimeInstance;
}

namespace duckemu::user::taobao
{

// The SecurityGuard init chain captured from the reference device:
//   10102 -> 10101 -> 10103 -> 10104 -> 10107
// Command ids and argument values come from logs/hook_a.log; the old case
// proved each one and the three values the decompiled-source guess got wrong.
Result<void> init(runtime::RuntimeInstance &instance);

// Stage isolation for diagnosis: 10102 alone (the plugin registration). The
// full init calls this first; --stage 10102 stops here.
Result<void> init10102(runtime::RuntimeInstance &instance);

// Stage isolation: 10102 + 10101 only. The full init continues with
// 10103/10104/10107; --stage 10101 stops after the plugin init.
Result<void> init10101(runtime::RuntimeInstance &instance);

// One doCommandNative call. |elements| become the Object[] (0 slots stay
// null); the returned object address is returned (0 for null).
Result<uint64_t> callCommand(runtime::RuntimeInstance &instance,
	int32_t command,
	const std::vector<uint64_t> &elements);

} // namespace duckemu::user::taobao
