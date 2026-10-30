// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <optional>

namespace duckemu::runtime::threads
{

enum class WakeReason : uint8_t
{
	FutexWoke = 0,
	TimerExpired,
	Signal,
	ThreadExited,
	// Some descriptor changed through a guest read/write; the waiter
	// re-evaluates readiness and reports a spurious wake by returning nullopt.
	PollWoke,
};

// A blocking syscall, created when the handler decides to sleep and attached
// to the RuntimeThread. The handler runs exactly once (decision 02 §3.1: no
// replay); completion produces the value written to x0 and, for some
// operations, the side effects of the syscall.
//
// complete() returns nullopt when the reason does not actually satisfy the
// operation (spurious wakeup); the thread keeps sleeping.
class PendingSyscall
{
    public:
	virtual ~PendingSyscall() = default;
	virtual std::optional<int64_t> complete(WakeReason reason) = 0;
};

} // namespace duckemu::runtime::threads
