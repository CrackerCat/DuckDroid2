#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/error/result.hpp"
#include "common/id/ids.hpp"
#include "runtime/memory/thunk/thunk_table.hpp"
#include "runtime/observer/event_bus.hpp"

namespace duckemu::runtime
{

struct RuntimeConfig
{
	uint32_t pid = 1000;
	uint32_t tid = 1;
	uint64_t stack_size = 64 * 1024;
	uint64_t default_timeout_us = 5000000;

	// Observation. An empty trace_path disables svc.log; record_memory_access
	// enables the (lossy) diagnostic memory-access feed.
	std::string trace_path;
	bool record_memory_access = false;
	size_t diagnostic_capacity = 1u << 14;
};

// The single facade over one guest world. M1 owns one host thread and one
// Unicorn engine; the guest memory, TLS, thunk page and SVC dispatcher are
// private implementation details.
class RuntimeInstance
{
    public:
	static Result<std::unique_ptr<RuntimeInstance> > create(const RuntimeConfig &config = {});

	~RuntimeInstance();
	RuntimeInstance(const RuntimeInstance &) = delete;
	RuntimeInstance &operator=(const RuntimeInstance &) = delete;
	RuntimeInstance(RuntimeInstance &&) = delete;
	RuntimeInstance &operator=(RuntimeInstance &&) = delete;

	Result<GuestVA> mapMemory(uint64_t size, uint32_t perms, std::string_view name);
	Result<void> writeMemory(GuestVA address, const void *data, uint64_t size);
	Result<void> readMemory(GuestVA address, void *data, uint64_t size) const;

	// Registers a host implementation and returns the guest thunk address.
	// Calling it from the guest goes SVC -> dispatcher -> stub, then `ret`.
	Result<GuestVA> registerHostFunction(std::string_view name, HostStub stub);

	struct CallResult
	{
		uint64_t return_value = 0;
		std::optional<int32_t> exit_code;
	};

	// Calls a guest function with x0..x7, SP set to the instance stack and
	// LR pointing at an internal return trap.
	Result<CallResult> callFunction(GuestVA entry, const std::array<uint64_t, 8> &args = {});

	// Semantic observation. Listeners run synchronously on the emulating
	// thread, in guest order, and must not call back into the emulator.
	uint32_t addSvcListener(observer::EventBus::SvcListener listener);
	uint32_t addMemoryListener(observer::EventBus::MemoryListener listener);
	bool removeSvcListener(uint32_t id);
	bool removeMemoryListener(uint32_t id);

	// Diagnostic channel. Returns the number of events moved into `out`;
	// droppedDiagnostics() counts events lost because the ring was full.
	size_t drainDiagnostics(std::vector<observer::DiagnosticEvent> &out);
	[[nodiscard]] uint64_t droppedDiagnostics() const;

    private:
	struct Impl;
	explicit RuntimeInstance(std::unique_ptr<Impl> impl);

	std::unique_ptr<Impl> impl_;
};

} // namespace duckemu::runtime
