#pragma once

#include <functional>
#include <memory>

#include <unicorn/unicorn.h>

#include "common/error/result.hpp"
#include "runtime/memory/code_notifier.hpp"
#include "runtime/observer/diagnostic/diagnostic_event.hpp"

namespace duckemu::runtime
{

class RuntimeMemory;

struct RunOutcome
{
	uc_err err = UC_ERR_OK;
	uint64_t pc = 0;
};

// Unicorn backend. M1 owns exactly one engine; M3 adds one engine per
// RuntimeThread and broadcasts map/protect/cache operations to all of them.
class UnicornBackend final : public MapObserver, public CodeWriteNotifier
{
    public:
	static Result<std::unique_ptr<UnicornBackend> > create(RuntimeMemory &memory);
	~UnicornBackend() override;

	UnicornBackend(const UnicornBackend &) = delete;
	UnicornBackend &operator=(const UnicornBackend &) = delete;
	UnicornBackend(UnicornBackend &&) = delete;
	UnicornBackend &operator=(UnicornBackend &&) = delete;

	[[nodiscard]] uc_engine *engine() const
	{
		return engine_;
	}

	void setSvcTrapHandler(std::function<void(uc_engine *, uint32_t)> handler);

	// Installs the diagnostic memory-access hook (reads and writes). The sink
	// is invoked on the emulating thread and must not re-enter the engine.
	// Passing an empty function disables the hook's data path; M2 installs it
	// once, before the first run.
	void setDiagnosticSink(std::function<void(const observer::DiagnosticEvent &)> sink);

	Result<RunOutcome> run(GuestVA begin, GuestVA until, uint64_t timeout_us);
	void stop();

	Result<uint64_t> readRegister(int reg) const;
	Result<void> writeRegister(int reg, uint64_t value);

	void invalidateCode(GuestVA address, uint64_t size);

	// MapObserver
	Result<void> onMapped(const Region &region) override;
	Result<void> onUnmapped(GuestVA address, uint64_t size) override;
	Result<void> onProtected(GuestVA address, uint64_t size, uint32_t perms) override;

	// CodeWriteNotifier
	void onCodeWritten(GuestVA address, uint64_t size) override;

    private:
	UnicornBackend(uc_engine *engine, RuntimeMemory &memory);

	static void onIntr(uc_engine *uc, uint32_t intno, void *user_data);
	static bool
	onMemWrite(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value, void *user_data);
	static bool
	onDiagnosticAccess(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value, void *user_data);

	uc_engine *engine_ = nullptr;
	RuntimeMemory &memory_;
	std::function<void(uc_engine *, uint32_t)> svc_handler_;
	std::function<void(const observer::DiagnosticEvent &)> diagnostic_sink_;
	uc_hook intr_hook_ = 0;
	uc_hook mem_write_hook_ = 0;
	uc_hook diagnostic_hook_ = 0;
};

} // namespace duckemu::runtime
