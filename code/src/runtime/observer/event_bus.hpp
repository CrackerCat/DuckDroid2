// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "runtime/observer/semantic/jni_event.hpp"
#include "runtime/observer/semantic/memory_event.hpp"
#include "runtime/observer/semantic/module_event.hpp"
#include "runtime/observer/semantic/svc_event.hpp"
#include "runtime/observer/semantic/thread_event.hpp"

namespace duckemu::runtime::observer
{

// Per-instance observation hub. Semantic events are delivered synchronously on
// the emulating thread, in guest order. Listeners must not re-enter the
// emulator (M0 probe 6: a nested uc_emu_start on the same engine clobbers the
// outer register set) and must not throw -- exceptions are caught and logged,
// because they cannot cross Unicorn's C frames.
//
// M3: every guest thread has its own engine, so any guest thread may publish.
// Listener registration and publication take the bus mutex; callbacks run
// outside it, against a snapshot, so a listener may add/remove listeners.
class EventBus
{
    public:
	using SvcListener = std::function<void(const SvcEvent &)>;
	using MemoryListener = std::function<void(const MemoryEvent &)>;
	using ThreadListener = std::function<void(const ThreadEvent &)>;
	using ModuleListener = std::function<void(const ModuleEvent &)>;
	using JniListener = std::function<void(const JniEvent &)>;

	EventBus() = default;

	uint32_t addSvcListener(SvcListener listener);
	bool removeSvcListener(uint32_t id);
	uint32_t addMemoryListener(MemoryListener listener);
	bool removeMemoryListener(uint32_t id);
	uint32_t addThreadListener(ThreadListener listener);
	bool removeThreadListener(uint32_t id);
	uint32_t addModuleListener(ModuleListener listener);
	bool removeModuleListener(uint32_t id);
	uint32_t addJniListener(JniListener listener);
	bool removeJniListener(uint32_t id);

	void publishSvc(const SvcEvent &event);
	void publishMemory(const MemoryEvent &event);
	// Assigns ThreadEvent::id (publication order) before delivering.
	void publishThread(ThreadEvent event);
	// Assigns ModuleEvent::id (publication order) before delivering.
	void publishModule(ModuleEvent event);
	// Assigns JniEvent::id (publication order) before delivering.
	void publishJni(JniEvent event);

	// True while a listener is being invoked on *this* thread. Listeners run
	// inside the Unicorn hook, so starting the engine again from one would nest
	// emulation on the same engine and clobber the outer register set (M0 probe
	// 6). The guard lives at the engine entry (RuntimeInstance::callFunction),
	// not here: publishing from inside a listener only recurses into listeners
	// and does not touch the engine. Thread-local because once every guest
	// thread has its own engine, any guest thread may be the one delivering.
	[[nodiscard]] bool delivering() const noexcept;

    private:
	mutable std::mutex mutex_;
	std::vector<std::pair<uint32_t, SvcListener> > svc_listeners_;
	std::vector<std::pair<uint32_t, MemoryListener> > memory_listeners_;
	std::vector<std::pair<uint32_t, ThreadListener> > thread_listeners_;
	std::vector<std::pair<uint32_t, ModuleListener> > module_listeners_;
	std::vector<std::pair<uint32_t, JniListener> > jni_listeners_;
	uint32_t next_listener_id_ = 1;
	uint64_t next_thread_event_id_ = 0;
	uint64_t next_module_event_id_ = 0;
	uint64_t next_jni_event_id_ = 0;
};

} // namespace duckemu::runtime::observer
