#include "runtime/observer/event_bus.hpp"

#include <exception>
#include <utility>

#include "common/log/logger.hpp"

namespace duckemu::runtime::observer
{
namespace
{

// Marks the whole delivery window (including nested publishes triggered by a
// listener) so the engine entry can refuse to re-enter the emulator.
class DeliverScope
{
    public:
	explicit DeliverScope(uint32_t &depth) noexcept
		: depth_(depth)
	{
		++depth_;
	}
	~DeliverScope()
	{
		--depth_;
	}
	DeliverScope(const DeliverScope &) = delete;
	DeliverScope &operator=(const DeliverScope &) = delete;

    private:
	uint32_t &depth_;
};

// Iterating over a snapshot lets a listener add or remove listeners from
// inside its own callback without invalidating the traversal.
template <typename Listener, typename Event>
void deliverAll(const char *channel,
	uint32_t &depth,
	const std::vector<std::pair<uint32_t, Listener> > &listeners,
	const Event &event)
{
	DeliverScope scope(depth);
	const std::vector<std::pair<uint32_t, Listener> > snapshot = listeners;
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

EventBus::EventBus(size_t diagnostic_capacity)
	: diagnostics_(diagnostic_capacity)
{
}

uint32_t EventBus::addSvcListener(SvcListener listener)
{
	const uint32_t id = next_listener_id_++;
	svc_listeners_.emplace_back(id, std::move(listener));
	return id;
}

bool EventBus::removeSvcListener(uint32_t id)
{
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
	const uint32_t id = next_listener_id_++;
	memory_listeners_.emplace_back(id, std::move(listener));
	return id;
}

bool EventBus::removeMemoryListener(uint32_t id)
{
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

void EventBus::publishSvc(const SvcEvent &event)
{
	deliverAll("svc", deliver_depth_, svc_listeners_, event);
}

void EventBus::publishMemory(const MemoryEvent &event)
{
	deliverAll("memory", deliver_depth_, memory_listeners_, event);
}

} // namespace duckemu::runtime::observer
