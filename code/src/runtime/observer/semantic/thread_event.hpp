// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <string_view>

namespace duckemu::runtime::observer
{

enum class ThreadEventKind : uint8_t
{
	Created = 0,
	Blocked,
	Runnable,
	Exited,
};

constexpr std::string_view threadEventKindName(ThreadEventKind kind) noexcept
{
	switch (kind)
	{
	case ThreadEventKind::Created:
		return "created";
	case ThreadEventKind::Blocked:
		return "blocked";
	case ThreadEventKind::Runnable:
		return "runnable";
	case ThreadEventKind::Exited:
		return "exited";
	}
	return "?";
}

// One guest-thread lifecycle transition. `id` is the publication order inside
// the instance (assigned by the bus), `tid` the guest thread id and
// `parent_tid` the thread that called clone (0 for the main thread).
struct ThreadEvent
{
	uint64_t id = 0;
	ThreadEventKind kind = ThreadEventKind::Created;
	uint32_t tid = 0;
	uint32_t parent_tid = 0;
};

} // namespace duckemu::runtime::observer
