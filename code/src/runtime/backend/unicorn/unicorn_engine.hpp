#pragma once

#include <functional>
#include <memory>

#include <unicorn/unicorn.h>

#include "common/error/result.hpp"
#include "runtime/memory/region.hpp"
#include "runtime/observer/diagnostic/diagnostic_event.hpp"

namespace duckemu::runtime
{

struct RunOutcome
{
	uc_err err = UC_ERR_OK;
	uint64_t pc = 0;
};

// One guest CPU: one uc_engine. A UnicornEngine belongs to exactly one
// RuntimeThread. Only its owning host thread runs it; the instance execution
// lock guarantees that no two engines execute guest code at the same time.
class UnicornEngine
{
    public:
	using SvcTrapHandler = std::function<void(uc_engine *, uint32_t)>;
	using DiagnosticSink = std::function<void(const observer::DiagnosticEvent &)>;
	using CodeWriteSink = std::function<void(GuestVA, uint64_t)>;

	static Result<std::unique_ptr<UnicornEngine> > create();
	~UnicornEngine();

	UnicornEngine(const UnicornEngine &) = delete;
	UnicornEngine &operator=(const UnicornEngine &) = delete;
	UnicornEngine(UnicornEngine &&) = delete;
	UnicornEngine &operator=(UnicornEngine &&) = delete;

	[[nodiscard]] uc_engine *handle() const
	{
		return engine_;
	}

	// Handler for every UC_HOOK_INTR on this engine (SVC dispatch).
	void setSvcTrapHandler(SvcTrapHandler handler);

	// Optional diagnostic feed. Installing a non-empty sink adds the read/write
	// hooks; an empty sink disables the data path but leaves the hook installed.
	void setDiagnosticSink(DiagnosticSink sink);

	// Guest wrote to memory. The sink runs on the emulating thread and is
	// expected to invalidate translated code in every engine of the instance.
	void setCodeWriteSink(CodeWriteSink sink);

	// Runs guest code until `until`, a stop request, a timeout or a fault.
	// `until == 0` means "no end address"; the thread only leaves through an
	// SVC handler, a fault or a stop request.
	Result<RunOutcome> run(GuestVA begin, GuestVA until, uint64_t timeout_us);
	void stop();

	Result<uint64_t> readRegister(int reg) const;
	Result<void> writeRegister(int reg, uint64_t value);

	void invalidateCode(GuestVA address, uint64_t size);

	// Region mirroring. The backend drives these for every engine so all CPUs
	// of an instance share one address-space view.
	Result<void> mapRegion(const Region &region);
	Result<void> unmapRegion(GuestVA address, uint64_t size);
	Result<void> protectRegion(GuestVA address, uint64_t size, uint32_t perms);

	// Whole CPU context (PC/GPR/SP and CP registers). Clone saves the parent
	// and restores the child; both engines are the same arch, so the context
	// is portable between them. Not a substitute for save/restore around a
	// nested uc_emu_start (M0 probe 6 / M2 decision) -- that still needs the
	// full save-run-restore sequence.
	class SavedContext
	{
	    public:
		~SavedContext();
		SavedContext(const SavedContext &) = delete;
		SavedContext &operator=(const SavedContext &) = delete;

	    private:
		friend class UnicornEngine;
		explicit SavedContext(uc_context *context);
		uc_context *context_ = nullptr;
	};

	Result<std::unique_ptr<SavedContext> > saveContext();
	Result<void> restoreContext(const SavedContext &context);

    private:
	explicit UnicornEngine(uc_engine *engine);

	static void onIntr(uc_engine *uc, uint32_t intno, void *user_data);
	static bool
	onMemWrite(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value, void *user_data);
	static bool
	onDiagnosticAccess(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value, void *user_data);

	uc_engine *engine_ = nullptr;
	SvcTrapHandler svc_handler_;
	DiagnosticSink diagnostic_sink_;
	CodeWriteSink code_write_sink_;
	uc_hook intr_hook_ = 0;
	uc_hook mem_write_hook_ = 0;
	uc_hook diagnostic_hook_ = 0;
};

} // namespace duckemu::runtime
