// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/backend/unicorn/unicorn_backend.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <optional>
#include <string>
#include <utility>

#include "common/log/logger.hpp"
#include "runtime/memory/address_space.hpp"

namespace duckemu::runtime
{
namespace
{

thread_local bool g_in_hook_callback = false;

} // namespace

struct UnicornBackend::EngineState
{
	UnicornBackend *backend = nullptr;
	UnicornEngine *engine = nullptr;
	uint32_t tid = 0;
	uint64_t id = 0;
	uc_hook trace_code_hook = 0;
	uc_hook trace_mem_hook = 0;
	std::unordered_map<uint32_t, uc_hook> address_hooks;
};

UnicornBackend::UnicornBackend(RuntimeMemory &memory)
	: memory_(memory)
{
}

UnicornBackend::~UnicornBackend() = default;

Result<std::unique_ptr<UnicornBackend> > UnicornBackend::create(RuntimeMemory &memory)
{
	return std::unique_ptr<UnicornBackend>(new UnicornBackend(memory));
}

Result<std::unique_ptr<UnicornEngine> > UnicornBackend::createEngine(UnicornEngine::SvcTrapHandler handler,
	UnicornEngine::DiagnosticSink sink,
	uint32_t tid)
{
	auto created = UnicornEngine::create();
	if (!created.ok())
	{
		return created.error();
	}
	std::unique_ptr<UnicornEngine> engine = std::move(created).value();
	engine->setSvcTrapHandler(std::move(handler));
	engine->setDiagnosticSink(std::move(sink));
	engine->setCodeWriteSink([this](GuestVA address, uint64_t size) {
		onGuestCodeWrite(address, size);
	});

	auto state = std::make_unique<EngineState>();
	state->backend = this;
	state->engine = engine.get();
	state->tid = tid;
	state->id = next_engine_id_++;
	EngineState *state_pointer = state.get();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		engine_states_[engine.get()] = std::move(state);
	}
	engine->setRunEndSink([this, state_pointer](uc_engine *uc) {
		dispatchRunEnd(*state_pointer, uc);
	});

	auto installed = installEngineHooks(*state_pointer);
	if (!installed.ok())
	{
		engine->setRunEndSink({});
		std::lock_guard<std::mutex> lock(mutex_);
		engine_states_.erase(engine.get());
		return installed.error();
	}

	// Replay the current address space, then register the engine while still
	// holding the lock so a concurrent map cannot slip in between.
	std::lock_guard<std::mutex> lock(mutex_);
	std::optional<Error> failed;
	memory_.forEachRegion([&](const Region &region) {
		if (failed.has_value())
		{
			return;
		}
		auto mapped = engine->mapRegion(region);
		if (!mapped.ok())
		{
			failed = mapped.error();
		}
	});
	if (failed.has_value())
	{
		engine->setRunEndSink({});
		engine_states_.erase(engine.get());
		return failed.value();
	}
	engines_.push_back(engine.get());
	return engine;
}

void UnicornBackend::destroyEngine(UnicornEngine *engine)
{
	std::lock_guard<std::mutex> lock(mutex_);
	engine->setRunEndSink({});
	const auto state = engine_states_.find(engine);
	if (state != engine_states_.end())
	{
		removeEngineHooks(*state->second);
		engine_states_.erase(state);
	}
	for (auto it = engines_.begin(); it != engines_.end(); ++it)
	{
		if (*it == engine)
		{
			engines_.erase(it);
			return;
		}
	}
}

Result<void> UnicornBackend::onMapped(const Region &region)
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::optional<Error> failed;
	for (UnicornEngine *engine : engines_)
	{
		auto mapped = engine->mapRegion(region);
		if (!mapped.ok())
		{
			failed = mapped.error();
		}
	}
	if (failed.has_value())
	{
		return failed.value();
	}
	return {};
}

Result<void> UnicornBackend::onUnmapped(GuestVA address, uint64_t size)
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::optional<Error> failed;
	for (UnicornEngine *engine : engines_)
	{
		auto unmapped = engine->unmapRegion(address, size);
		if (!unmapped.ok())
		{
			failed = unmapped.error();
		}
	}
	if (failed.has_value())
	{
		return failed.value();
	}
	return {};
}

Result<void> UnicornBackend::onProtected(GuestVA address, uint64_t size, uint32_t perms)
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::optional<Error> failed;
	for (UnicornEngine *engine : engines_)
	{
		auto protected_ = engine->protectRegion(address, size, perms);
		if (!protected_.ok())
		{
			failed = protected_.error();
		}
	}
	if (failed.has_value())
	{
		return failed.value();
	}
	return {};
}

void UnicornBackend::onCodeWritten(GuestVA address, uint64_t size)
{
	broadcastInvalidate(address, size);
}

size_t UnicornBackend::engineCount() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return engines_.size();
}

void UnicornBackend::onGuestCodeWrite(GuestVA address, uint64_t size)
{
	if (size == 0)
	{
		return;
	}
	const uint64_t first_page = pageDown(address.value());
	const uint64_t last_page = pageDown(address.value() + size - 1);
	for (uint64_t page = first_page;; page += kGuestPageSize)
	{
		if (memory_.isExecutable(GuestVA{ page }))
		{
			broadcastInvalidate(GuestVA{ page }, kGuestPageSize);
		}
		if (page >= last_page)
		{
			break;
		}
	}
}

void UnicornBackend::broadcastInvalidate(GuestVA address, uint64_t size)
{
	if (size == 0)
	{
		return;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	for (UnicornEngine *engine : engines_)
	{
		engine->invalidateCode(address, size);
	}
}

// ---------------------------------------------------------------------------
// Address hooks and instruction trace
// ---------------------------------------------------------------------------

Result<void> UnicornBackend::installEngineHooks(EngineState &state)
{
	removeEngineHooks(state);
	if (state.engine == nullptr)
	{
		return {};
	}

	if (trace_active_ && trace_sink_)
	{
		const uint64_t begin = trace_options_.begin.value();
		const uint64_t end = begin + trace_options_.size;
		auto code_hook = state.engine->addCodeHook(begin,
			end,
			&UnicornBackend::onCodeHook,
			&state);
		if (!code_hook.ok())
		{
			return code_hook.error();
		}
		state.trace_code_hook = code_hook.value();
		auto memory_hook = state.engine->addMemoryHook(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE,
			&UnicornBackend::onMemoryHook,
			&state);
		if (!memory_hook.ok())
		{
			removeEngineHooks(state);
			return memory_hook.error();
		}
		state.trace_mem_hook = memory_hook.value();
	}

	const uint64_t trace_begin = trace_options_.begin.value();
	const uint64_t trace_end = trace_begin + trace_options_.size;
	for (const uint32_t id : address_hook_order_)
	{
		const auto found = address_hooks_.find(id);
		if (found == address_hooks_.end())
		{
			continue;
		}
		const uint64_t address = found->second.address;
		if (trace_active_ && address >= trace_begin && address < trace_end)
		{
			// The trace code hook dispatches this one; a second hook at the
			// same address would call the callback twice.
			continue;
		}
		auto hook = state.engine->addCodeHook(address,
			address + 4,
			&UnicornBackend::onCodeHook,
			&state);
		if (!hook.ok())
		{
			removeEngineHooks(state);
			return hook.error();
		}
		state.address_hooks[id] = hook.value();
	}
	return {};
}

void UnicornBackend::removeEngineHooks(EngineState &state)
{
	if (state.engine == nullptr)
	{
		return;
	}
	if (state.trace_code_hook != 0)
	{
		(void)state.engine->removeHook(state.trace_code_hook);
		state.trace_code_hook = 0;
	}
	if (state.trace_mem_hook != 0)
	{
		(void)state.engine->removeHook(state.trace_mem_hook);
		state.trace_mem_hook = 0;
	}
	for (const auto &entry : state.address_hooks)
	{
		(void)state.engine->removeHook(entry.second);
	}
	state.address_hooks.clear();
}

Result<void> UnicornBackend::rebuildEngineHooks()
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &entry : engine_states_)
	{
		auto installed = installEngineHooks(*entry.second);
		if (!installed.ok())
		{
			return installed.error();
		}
	}
	return {};
}

Result<uint32_t> UnicornBackend::addAddressHook(GuestVA address, observer::AddressHook hook)
{
	if (inHookCallback())
	{
		return Error{ ErrorCode::InvalidArgument, "cannot add an address hook from inside a hook callback" };
	}
	if (untagAddress(address.value()) % 4 != 0)
	{
		return Error{ ErrorCode::InvalidArgument, "address hook must be 4-byte aligned" };
	}
	if (!memory_.isExecutable(GuestVA{ untagAddress(address.value()) }))
	{
		return Error{ ErrorCode::InvalidArgument, "address hook must point into an executable region" };
	}

	const uint64_t clean = untagAddress(address.value());
	const uint32_t id = next_hook_id_++;
	AddressHookEntry entry;
	entry.id = id;
	entry.address = clean;
	entry.callback = std::move(hook);
	address_hooks_.emplace(id, std::move(entry));
	address_hook_order_.push_back(id);

	auto rebuilt = rebuildEngineHooks();
	if (!rebuilt.ok())
	{
		address_hook_order_.pop_back();
		address_hooks_.erase(id);
		(void)rebuildEngineHooks();
		return rebuilt.error();
	}
	return id;
}

bool UnicornBackend::removeAddressHook(uint32_t id)
{
	if (inHookCallback())
	{
		DUCKEMU_ERROR("cannot remove an address hook from inside a hook callback");
		return false;
	}
	const auto found = address_hooks_.find(id);
	if (found == address_hooks_.end())
	{
		return false;
	}
	address_hooks_.erase(found);
	address_hook_order_.erase(std::remove(address_hook_order_.begin(), address_hook_order_.end(), id),
		address_hook_order_.end());
	auto rebuilt = rebuildEngineHooks();
	if (!rebuilt.ok())
	{
		DUCKEMU_ERROR("address hook removal could not rebuild engine hooks: %s",
			rebuilt.error().message.c_str());
	}
	return true;
}

void UnicornBackend::resetHookHitCounts()
{
	for (auto &entry : address_hooks_)
	{
		entry.second.hit_count = 0;
	}
}

bool UnicornBackend::inHookCallback() const noexcept
{
	return g_in_hook_callback;
}

Result<void> UnicornBackend::startInsnTrace(const observer::InsnTraceOptions &options,
	RuntimeMemory &memory,
	const loader::ModuleRegistry &modules,
	const ThunkTable *thunks)
{
	if (inHookCallback())
	{
		return Error{ ErrorCode::InvalidArgument,
			"cannot start an instruction trace from inside a hook callback" };
	}
	if (trace_active_ || trace_sink_)
	{
		return Error{ ErrorCode::AlreadyExists, "an instruction trace is already active" };
	}
	observer::InsnTraceOptions normalized = options;
	normalized.begin = GuestVA{ untagAddress(normalized.begin.value()) };
	auto sink = observer::createInsnTrace(normalized, memory, modules, thunks);
	if (!sink.ok())
	{
		return sink.error();
	}

	trace_options_ = normalized;
	trace_sink_ = std::move(sink).value();
	trace_active_ = true;
	auto rebuilt = rebuildEngineHooks();
	if (!rebuilt.ok())
	{
		trace_active_ = false;
		trace_sink_.reset();
		(void)rebuildEngineHooks();
		return rebuilt.error();
	}
	return {};
}

void UnicornBackend::stopInsnTrace()
{
	if (inHookCallback())
	{
		DUCKEMU_ERROR("cannot stop an instruction trace from inside a hook callback");
		return;
	}
	if (!trace_active_ && !trace_sink_)
	{
		return;
	}
	trace_active_ = false;
	auto rebuilt = rebuildEngineHooks();
	if (!rebuilt.ok())
	{
		DUCKEMU_ERROR("stopping the instruction trace could not rebuild engine hooks: %s",
			rebuilt.error().message.c_str());
	}
	trace_sink_.reset();
}

bool UnicornBackend::insnTraceActive() const noexcept
{
	return trace_active_ && trace_sink_ != nullptr;
}

void UnicornBackend::dispatchCode(EngineState &state, uc_engine *uc, uint64_t address)
{
	for (const uint32_t id : address_hook_order_)
	{
		const auto found = address_hooks_.find(id);
		if (found == address_hooks_.end() || found->second.address != address)
		{
			continue;
		}
		AddressHookEntry &entry = found->second;
		if (!entry.callback)
		{
			continue;
		}
		++entry.hit_count;

		std::array<uint64_t, 8> arguments{};
		for (size_t i = 0; i < arguments.size(); ++i)
		{
			(void)uc_reg_read(uc,
				UC_ARM64_REG_X0 + static_cast<int>(i),
				&arguments[i]);
		}
		uint64_t lr = 0;
		uint64_t sp = 0;
		(void)uc_reg_read(uc, UC_ARM64_REG_LR, &lr);
		(void)uc_reg_read(uc, UC_ARM64_REG_SP, &sp);

		observer::HookContext context(uc,
			&memory_,
			state.tid,
			address,
			entry.hit_count,
			arguments,
			lr,
			sp);
		const bool previous = g_in_hook_callback;
		g_in_hook_callback = true;
		try
		{
			entry.callback(context);
		}
		catch (const std::exception &error)
		{
			DUCKEMU_ERROR("address hook at 0x%llx threw: %s",
				static_cast<unsigned long long>(address),
				error.what());
		}
		catch (...)
		{
			DUCKEMU_ERROR("address hook at 0x%llx threw an unknown exception",
				static_cast<unsigned long long>(address));
		}
		g_in_hook_callback = previous;
	}

	if (!trace_active_ || !trace_sink_)
	{
		return;
	}
	const uint64_t begin = trace_options_.begin.value();
	const uint64_t end = begin + trace_options_.size;
	if (address < begin || address >= end)
	{
		return;
	}
	uint64_t pc = 0;
	if (uc_reg_read(uc, UC_ARM64_REG_PC, &pc) != UC_ERR_OK || pc != address)
	{
		// A user hook redirected execution; this instruction will not run.
		return;
	}
	trace_sink_->onInstruction(uc, state.id, state.tid, address);
}

void UnicornBackend::dispatchMemory(EngineState &state,
	uc_engine *uc,
	uc_mem_type type,
	uint64_t address,
	int size,
	int64_t value)
{
	if (!trace_active_ || !trace_sink_)
	{
		return;
	}
	if (type != UC_MEM_READ && type != UC_MEM_WRITE)
	{
		return;
	}
	trace_sink_->onMemoryAccess(uc,
		state.id,
		state.tid,
		type == UC_MEM_WRITE,
		address,
		static_cast<uint64_t>(size),
		static_cast<uint64_t>(value));
}

void UnicornBackend::dispatchRunEnd(EngineState &state, uc_engine *uc)
{
	if (trace_sink_)
	{
		trace_sink_->onRunEnd(uc, state.id);
	}
}

void UnicornBackend::onCodeHook(uc_engine *uc, uint64_t address, uint32_t size, void *user_data)
{
	(void)size;
	auto *state = static_cast<EngineState *>(user_data);
	state->backend->dispatchCode(*state, uc, address);
}

bool UnicornBackend::onMemoryHook(uc_engine *uc,
	uc_mem_type type,
	uint64_t address,
	int size,
	int64_t value,
	void *user_data)
{
	auto *state = static_cast<EngineState *>(user_data);
	state->backend->dispatchMemory(*state, uc, type, address, size, value);
	return true;
}

} // namespace duckemu::runtime
