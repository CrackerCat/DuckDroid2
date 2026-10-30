// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/error/result.hpp"
#include "runtime/backend/unicorn/unicorn_engine.hpp"
#include "runtime/memory/code_notifier.hpp"
#include "runtime/observer/hook/hook_context.hpp"
#include "runtime/observer/trace/insn_trace.hpp"

namespace duckemu::runtime
{

class RuntimeMemory;
class ThunkTable;
namespace loader
{
class ModuleRegistry;
}

// Owns every engine of one RuntimeInstance and keeps their address-space view
// and translated-code caches in sync. Engines themselves are owned by their
// RuntimeThread; the backend only keeps non-owning pointers and is told when
// one is destroyed.
class UnicornBackend final : public MapObserver, public CodeWriteNotifier
{
    public:
	static Result<std::unique_ptr<UnicornBackend> > create(RuntimeMemory &memory);
	~UnicornBackend();

	UnicornBackend(const UnicornBackend &) = delete;
	UnicornBackend &operator=(const UnicornBackend &) = delete;
	UnicornBackend(UnicornBackend &&) = delete;
	UnicornBackend &operator=(UnicornBackend &&) = delete;

	// Creates an engine, mirrors every existing region into it, installs the
	// instance's address hooks / trace hooks, and registers it for future
	// broadcasts. The caller must hold the instance execution lock (or
	// otherwise guarantee no engine is running).
	Result<std::unique_ptr<UnicornEngine> > createEngine(UnicornEngine::SvcTrapHandler handler,
		UnicornEngine::DiagnosticSink sink,
		uint32_t tid);

	// Address hooks. The callback runs on the emulating thread before the
	// instruction at `address`; all engines (including later ones) get it.
	Result<uint32_t> addAddressHook(GuestVA address, observer::AddressHook hook);
	bool removeAddressHook(uint32_t id);
	void resetHookHitCounts();
	// True while a user address-hook callback is executing on this thread.
	[[nodiscard]] bool inHookCallback() const noexcept;

	// Instruction trace. The range must be inside one module; see
	// observer::InsnTraceOptions. One session at a time.
	Result<void> startInsnTrace(const observer::InsnTraceOptions &options,
		RuntimeMemory &memory,
		const loader::ModuleRegistry &modules,
		const ThunkTable *thunks);
	void stopInsnTrace();
	[[nodiscard]] bool insnTraceActive() const noexcept;

	// Must be called before the engine is destroyed.
	void destroyEngine(UnicornEngine *engine);

	// MapObserver: broadcast to every engine.
	Result<void> onMapped(const Region &region) override;
	Result<void> onUnmapped(GuestVA address, uint64_t size) override;
	Result<void> onProtected(GuestVA address, uint64_t size, uint32_t perms) override;

	// CodeWriteNotifier: invalidate translated code in every engine.
	void onCodeWritten(GuestVA address, uint64_t size) override;

	[[nodiscard]] size_t engineCount() const;

    private:
	struct AddressHookEntry
	{
		uint32_t id = 0;
		uint64_t address = 0;
		observer::AddressHook callback;
		uint64_t hit_count = 0;
	};

	struct EngineState;

	explicit UnicornBackend(RuntimeMemory &memory);

	void broadcastInvalidate(GuestVA address, uint64_t size);
	// Guest-side SMC: pages written by guest code must drop stale TBs, but only
	// where the region is executable.
	void onGuestCodeWrite(GuestVA address, uint64_t size);

	Result<void> installEngineHooks(EngineState &state);
	void removeEngineHooks(EngineState &state);
	Result<void> rebuildEngineHooks();

	void dispatchCode(EngineState &state, uc_engine *uc, uint64_t address);
	void dispatchMemory(EngineState &state,
		uc_engine *uc,
		uc_mem_type type,
		uint64_t address,
		int size,
		int64_t value);
	void dispatchRunEnd(EngineState &state, uc_engine *uc);

	static void onCodeHook(uc_engine *uc, uint64_t address, uint32_t size, void *user_data);
	static bool onMemoryHook(uc_engine *uc,
		uc_mem_type type,
		uint64_t address,
		int size,
		int64_t value,
		void *user_data);

	RuntimeMemory &memory_;
	mutable std::mutex mutex_;
	std::vector<UnicornEngine *> engines_;
	std::unordered_map<UnicornEngine *, std::unique_ptr<EngineState> > engine_states_;
	std::unordered_map<uint32_t, AddressHookEntry> address_hooks_;
	std::vector<uint32_t> address_hook_order_;
	std::unique_ptr<observer::InsnTraceSink> trace_sink_;
	observer::InsnTraceOptions trace_options_;
	bool trace_active_ = false;
	uint32_t next_hook_id_ = 1;
	uint64_t next_engine_id_ = 1;
};

} // namespace duckemu::runtime
