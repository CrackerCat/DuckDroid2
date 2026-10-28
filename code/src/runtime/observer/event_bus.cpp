#include "runtime/observer/event_bus.hpp"

#include <exception>
#include <utility>

#include "common/log/logger.hpp"

namespace duckemu::runtime::observer
{
namespace
{

// Per-thread: whether this thread is currently inside a listener callback.
// With one engine per guest thread, any guest thread can be the deliverer, but
// re-entry is only ever dangerous from the thread that is already delivering.
thread_local uint32_t t_deliver_depth = 0;

// Marks the whole delivery window (including nested publishes triggered by a
// listener) so the engine entry can refuse to re-enter the emulator.
class DeliverScope
{
    public:
	DeliverScope() noexcept
	{
		++t_deliver_depth;
	}
	~DeliverScope()
	{
		--t_deliver_depth;
	}
	DeliverScope(const DeliverScope &) = delete;
	DeliverScope &operator=(const DeliverScope &) = delete;
};

// The caller takes the snapshot under the bus mutex. Iterating over that copy
// lets a listener add or remove listeners from inside its own callback without
// invalidating the traversal and without holding the lock across the callback.
template <typename Listener, typename Event>
void deliverAll(const char *channel, const std::vector<std::pair<uint32_t, Listener> > &snapshot, const Event &event)
{
	DeliverScope scope;
	for (const auto &entry : snapshot)
	{
		try
		{
			entry.second(event);
		}
		catch (const std::exception &error)
		{
			DUCKEMU_ERROR("%s listener %u threw: %s", channel, entry.first, error.what());
		}
		catch (...)
		{
			DUCKEMU_ERROR("%s listener %u threw a non-standard exception", channel, entry.first);
		}
	}
}

} // namespace

uint32_t EventBus::addSvcListener(SvcListener listener)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const uint32_t id = next_listener_id_++;
	svc_listeners_.emplace_back(id, std::move(listener));
	return id;
}

bool EventBus::removeSvcListener(uint32_t id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto it = svc_listeners_.begin(); it != svc_listeners_.end(); ++it)
	{
		if (it->first == id)
		{
			svc_listeners_.erase(it);
			return true;
		}
	}
	return false;
}

uint32_t EventBus::addMemoryListener(MemoryListener listener)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const uint32_t id = next_listener_id_++;
	memory_listeners_.emplace_back(id, std::move(listener));
	return id;
}

bool EventBus::removeMemoryListener(uint32_t id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto it = memory_listeners_.begin(); it != memory_listeners_.end(); ++it)
	{
		if (it->first == id)
		{
			memory_listeners_.erase(it);
			return true;
		}
	}
	return false;
}

uint32_t EventBus::addThreadListener(ThreadListener listener)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const uint32_t id = next_listener_id_++;
	thread_listeners_.emplace_back(id, std::move(listener));
	return id;
}

bool EventBus::removeThreadListener(uint32_t id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto it = thread_listeners_.begin(); it != thread_listeners_.end(); ++it)
	{
		if (it->first == id)
		{
			thread_listeners_.erase(it);
			return true;
		}
	}
	return false;
}

bool EventBus::delivering() const noexcept
{
	return t_deliver_depth != 0;
}

void EventBus::publishSvc(const SvcEvent &event)
{
	// Copy-construct under the lock. Copy assignment of a vector of std::function
	// trips a GCC 16 -Wnull-dereference false positive in stl_pair.h.
	const std::vector<std::pair<uint32_t, SvcListener> > snapshot = [this] {
		std::lock_guard<std::mutex> lock(mutex_);
		return svc_listeners_;
	}();
	deliverAll("svc", snapshot, event);
}

void EventBus::publishMemory(const MemoryEvent &event)
{
	const std::vector<std::pair<uint32_t, MemoryListener> > snapshot = [this] {
		std::lock_guard<std::mutex> lock(mutex_);
		return memory_listeners_;
	}();
	deliverAll("memory", snapshot, event);
}

void EventBus::publishThread(ThreadEvent event)
{
	uint64_t id = 0;
	// Copy-construct under the lock (same GCC 16 false positive as publishSvc).
	const std::vector<std::pair<uint32_t, ThreadListener> > snapshot = [this, &id] {
		std::lock_guard<std::mutex> lock(mutex_);
		id = next_thread_event_id_++;
		return thread_listeners_;
	}();
	event.id = id;
	deliverAll("thread", snapshot, event);
}

} // namespace duckemu::runtime::observer
