#include "runtime/backend/unicorn/unicorn_engine.hpp"

#include <utility>

#include "common/log/logger.hpp"

namespace duckemu::runtime
{
namespace
{

Error unicornError(uc_err err, const char *what)
{
	return Error{ ErrorCode::EmulationFault, std::string(what) + ": " + uc_strerror(err) };
}

} // namespace

UnicornEngine::UnicornEngine(uc_engine *engine)
	: engine_(engine)
{
}

UnicornEngine::~UnicornEngine()
{
	if (engine_ != nullptr)
	{
		uc_close(engine_);
	}
}

Result<std::unique_ptr<UnicornEngine> > UnicornEngine::create()
{
	uc_engine *engine = nullptr;
	const uc_err open_err = uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &engine);
	if (open_err != UC_ERR_OK)
	{
		return unicornError(open_err, "uc_open");
	}

	// M0 measured Unicorn's default ARM64 page size as 0x400 (TARGET_PAGE_BITS_MIN
	// = 10). Android's guest pages are 4 KiB; align the emulator with that
	// before the engine is initialized by the first map/run.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
	const uc_err page_err = uc_ctl_set_page_size(engine, static_cast<uint32_t>(kGuestPageSize));
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
	if (page_err != UC_ERR_OK)
	{
		uc_close(engine);
		return unicornError(page_err, "uc_ctl_set_page_size");
	}

	std::unique_ptr<UnicornEngine> self(new UnicornEngine(engine));

	const uc_err intr_err = uc_hook_add(engine,
		&self->intr_hook_,
		UC_HOOK_INTR,
		reinterpret_cast<void *>(&UnicornEngine::onIntr),
		self.get(),
		1,
		0);
	if (intr_err != UC_ERR_OK)
	{
		return unicornError(intr_err, "uc_hook_add(UC_HOOK_INTR)");
	}

	const uc_err write_err = uc_hook_add(engine,
		&self->mem_write_hook_,
		UC_HOOK_MEM_WRITE,
		reinterpret_cast<void *>(&UnicornEngine::onMemWrite),
		self.get(),
		1,
		0);
	if (write_err != UC_ERR_OK)
	{
		return unicornError(write_err, "uc_hook_add(UC_HOOK_MEM_WRITE)");
	}

	return self;
}

void UnicornEngine::setSvcTrapHandler(SvcTrapHandler handler)
{
	svc_handler_ = std::move(handler);
}

void UnicornEngine::setDiagnosticSink(DiagnosticSink sink)
{
	diagnostic_sink_ = std::move(sink);
	if (!diagnostic_sink_ || diagnostic_hook_ != 0)
	{
		return;
	}

	const uc_err err = uc_hook_add(engine_,
		&diagnostic_hook_,
		UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE,
		reinterpret_cast<void *>(&UnicornEngine::onDiagnosticAccess),
		this,
		1,
		0);
	if (err != UC_ERR_OK)
	{
		DUCKEMU_WARN("uc_hook_add(UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE) failed: %s", uc_strerror(err));
	}
}

void UnicornEngine::setCodeWriteSink(CodeWriteSink sink)
{
	code_write_sink_ = std::move(sink);
}

Result<RunOutcome> UnicornEngine::run(GuestVA begin, GuestVA until, uint64_t timeout_us)
{
	RunOutcome outcome;
	outcome.err = uc_emu_start(engine_, begin.value(), until.value(), timeout_us, 0);
	auto pc = readRegister(UC_ARM64_REG_PC);
	if (!pc.ok())
	{
		return pc.error();
	}
	outcome.pc = pc.value();
	return outcome;
}

void UnicornEngine::stop()
{
	uc_emu_stop(engine_);
}

Result<uint64_t> UnicornEngine::readRegister(int reg) const
{
	uint64_t value = 0;
	const uc_err err = uc_reg_read(engine_, reg, &value);
	if (err != UC_ERR_OK)
	{
		return unicornError(err, "uc_reg_read");
	}
	return value;
}

Result<void> UnicornEngine::writeRegister(int reg, uint64_t value)
{
	const uc_err err = uc_reg_write(engine_, reg, &value);
	if (err != UC_ERR_OK)
	{
		return unicornError(err, "uc_reg_write");
	}
	return {};
}

void UnicornEngine::invalidateCode(GuestVA address, uint64_t size)
{
	if (size == 0)
	{
		return;
	}
	const uint64_t start = pageDown(address.value());
	const uint64_t end = pageUp(address.value() + size);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
	const uc_err err = uc_ctl_remove_cache(engine_, start, end);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
	if (err != UC_ERR_OK)
	{
		DUCKEMU_WARN("uc_ctl_remove_cache(0x%llx, 0x%llx) failed: %s",
			static_cast<unsigned long long>(start),
			static_cast<unsigned long long>(end),
			uc_strerror(err));
	}
}

Result<void> UnicornEngine::mapRegion(const Region &region)
{
	const uc_err err =
		uc_mem_map_ptr(engine_, region.base.value(), region.size, region.perms, region.hostPtr());
	if (err != UC_ERR_OK)
	{
		return unicornError(err, "uc_mem_map_ptr");
	}
	return {};
}

Result<void> UnicornEngine::unmapRegion(GuestVA address, uint64_t size)
{
	const uc_err err = uc_mem_unmap(engine_, address.value(), size);
	if (err != UC_ERR_OK)
	{
		return unicornError(err, "uc_mem_unmap");
	}
	return {};
}

Result<void> UnicornEngine::protectRegion(GuestVA address, uint64_t size, uint32_t perms)
{
	const uc_err err = uc_mem_protect(engine_, address.value(), size, perms);
	if (err != UC_ERR_OK)
	{
		return unicornError(err, "uc_mem_protect");
	}
	return {};
}

UnicornEngine::SavedContext::SavedContext(uc_context *context)
	: context_(context)
{
}

UnicornEngine::SavedContext::~SavedContext()
{
	if (context_ != nullptr)
	{
		uc_free(context_);
	}
}

Result<std::unique_ptr<UnicornEngine::SavedContext> > UnicornEngine::saveContext()
{
	uc_context *context = nullptr;
	const uc_err alloc_err = uc_context_alloc(engine_, &context);
	if (alloc_err != UC_ERR_OK)
	{
		return unicornError(alloc_err, "uc_context_alloc");
	}

	std::unique_ptr<SavedContext> saved(new SavedContext(context));
	const uc_err save_err = uc_context_save(engine_, context);
	if (save_err != UC_ERR_OK)
	{
		return unicornError(save_err, "uc_context_save");
	}
	return saved;
}

Result<void> UnicornEngine::restoreContext(const SavedContext &context)
{
	const uc_err err = uc_context_restore(engine_, context.context_);
	if (err != UC_ERR_OK)
	{
		return unicornError(err, "uc_context_restore");
	}
	return {};
}

} // namespace duckemu::runtime
