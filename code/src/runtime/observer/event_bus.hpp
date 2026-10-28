#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "runtime/observer/diagnostic/diagnostic_event.hpp"
#include "runtime/observer/diagnostic/ring_buffer.hpp"
#include "runtime/observer/semantic/memory_event.hpp"
#include "runtime/observer/semantic/svc_event.hpp"

namespace duckemu::runtime::observer
{

// Per-instance observation hub. Semantic events are delivered synchronously on
// the emulating thread, in guest order. Listeners must not re-enter the
// emulator (M0 probe 6: a nested uc_emu_start on the same engine clobbers the
// outer register set) and must not throw -- exceptions are caught and logged,
// because they cannot cross Unicorn's C frames.
//
// M2 is single-threaded and takes no lock; M3 adds the instance lock.
class EventBus
{
    public:
	using SvcListener = std::function<void(const SvcEvent &)>;
	using MemoryListener = std::function<void(const MemoryEvent &)>;

	explicit EventBus(size_t diagnostic_capacity = 1u << 14);

	uint32_t addSvcListener(SvcListener listener);
	bool removeSvcListener(uint32_t id);
	uint32_t addMemoryListener(MemoryListener listener);
	bool removeMemoryListener(uint32_t id);

	void publishSvc(const SvcEvent &event);
	void publishMemory(const MemoryEvent &event);

	[[nodiscard]] RingBuffer<DiagnosticEvent> &diagnostics()
	{
		return diagnostics_;
	}

	// True while a listener is being invoked. Listeners run inside the Unicorn
	// hook, so starting the engine again from one would nest emulation on the
	// same engine and clobber the outer register set (M0 probe 6). The guard
	// lives at the engine entry (RuntimeInstance::callFunction), not here:
	// publishing from inside a listener only recurses into listeners and does
	// not touch the engine.
	[[nodiscard]] bool delivering() const noexcept
	{
		return deliver_depth_ != 0;
	}

    private:
	std::vector<std::pair<uint32_t, SvcListener> > svc_listeners_;
	std::vector<std::pair<uint32_t, MemoryListener> > memory_listeners_;
	uint32_t next_listener_id_ = 1;
	uint32_t deliver_depth_ = 0;
	RingBuffer<DiagnosticEvent> diagnostics_;
};

} // namespace duckemu::runtime::observer
