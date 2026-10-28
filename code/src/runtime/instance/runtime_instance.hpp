#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include "common/error/result.hpp"
#include "common/id/ids.hpp"
#include "runtime/memory/thunk/thunk_table.hpp"

namespace duckemu::runtime
{

struct RuntimeConfig
{
	uint32_t pid = 1000;
	uint32_t tid = 1;
	uint64_t stack_size = 64 * 1024;
	uint64_t default_timeout_us = 5000000;
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

    private:
	struct Impl;
	explicit RuntimeInstance(std::unique_ptr<Impl> impl);

	std::unique_ptr<Impl> impl_;
};

} // namespace duckemu::runtime
