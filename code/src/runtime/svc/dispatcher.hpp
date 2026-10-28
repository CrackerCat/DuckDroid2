#pragma once

#include <array>
#include <cstdint>

#include <unicorn/unicorn.h>

#include "runtime/memory/address_space.hpp"
#include "runtime/memory/thunk/thunk_table.hpp"
#include "runtime/observer/semantic/svc_event.hpp"
#include "runtime/observer/trace/trace_clock.hpp"

namespace duckemu::runtime::observer
{
class EventBus;
}

namespace duckemu::runtime::threads
{
class RuntimeThread;
class ThreadSystem;
}

namespace duckemu::runtime::svc
{

// One dispatcher per guest thread: it sits on that thread's only engine, so
// tid, blocking state and the pending syscall are all thread-local.
class SvcDispatcher
{
    public:
	SvcDispatcher(RuntimeMemory &memory,
		ThunkTable &thunks,
		observer::EventBus &events,
		observer::TraceClock &clock,
		uint32_t pid,
		threads::RuntimeThread &thread,
		threads::ThreadSystem &threads);

	// UC_HOOK_INTR entry. Decodes the trapping SVC at PC - 4:
	//   imm == 0            -> Linux syscall, number in x8, args x0..x5
	//   imm & 0x8000        -> host stub, index = imm & 0x7fff
	// Publishes one SvcEvent per dispatch, whether it was supported or not.
	// A blocking syscall leaves the event on the thread (pending_event) and is
	// published by the owner when it actually resumes.
	void onTrap(uc_engine *uc, uint32_t intno);

	void resetRunState();

	struct Stats
	{
		uint64_t svc_calls = 0;
		uint64_t host_calls = 0;
		uint64_t unknown = 0;
		int64_t last_syscall = -1;
	};
	[[nodiscard]] const Stats &stats() const
	{
		return stats_;
	}

    private:
	int64_t dispatchSyscall(uint32_t number, const std::array<uint64_t, 6> &args, bool &supported);
	int64_t invokeHostStub(uint32_t index, const std::array<uint64_t, 8> &args, bool &supported);
	void captureStack(uc_engine *uc, observer::SvcEvent &event) const;
	void logUnsupported(const observer::SvcEvent &event) const;

	RuntimeMemory &memory_;
	ThunkTable &thunks_;
	observer::EventBus &events_;
	observer::TraceClock &clock_;
	uint32_t pid_;
	threads::RuntimeThread &thread_;
	threads::ThreadSystem &threads_;
	Stats stats_;
};

} // namespace duckemu::runtime::svc
