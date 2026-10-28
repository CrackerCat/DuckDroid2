// Unicorn hooks -> observer events.
//
// Three callbacks live here, all installed per engine by UnicornEngine:
//   - UC_HOOK_INTR: every SVC trap, handed to the engine's SvcDispatcher
//   - UC_HOOK_MEM_WRITE: guest writes, used for code-page invalidation (SMC)
//   - UC_HOOK_MEM_READ|WRITE: optional diagnostic feed (M2: opt-in)

#include "runtime/backend/unicorn/unicorn_engine.hpp"

#include "common/log/logger.hpp"

namespace duckemu::runtime
{

void UnicornEngine::onIntr(uc_engine *uc, uint32_t intno, void *user_data)
{
	auto *engine = static_cast<UnicornEngine *>(user_data);
	if (engine->svc_handler_)
	{
		engine->svc_handler_(uc, intno);
		return;
	}
	DUCKEMU_ERROR("UC_HOOK_INTR (intno=%u) without a handler; stopping", intno);
	uc_emu_stop(uc);
}

bool UnicornEngine::onMemWrite(uc_engine *uc,
	uc_mem_type type,
	uint64_t address,
	int size,
	int64_t value,
	void *user_data)
{
	(void)uc;
	(void)type;
	(void)value;
	auto *engine = static_cast<UnicornEngine *>(user_data);
	if (size <= 0 || !engine->code_write_sink_)
	{
		return true;
	}
	engine->code_write_sink_(GuestVA{ address }, static_cast<uint64_t>(size));
	return true;
}

bool UnicornEngine::onDiagnosticAccess(uc_engine *uc,
	uc_mem_type type,
	uint64_t address,
	int size,
	int64_t value,
	void *user_data)
{
	auto *engine = static_cast<UnicornEngine *>(user_data);
	if (engine->diagnostic_sink_ == nullptr || size <= 0)
	{
		return true;
	}
	// Only successful accesses; *_UNMAPPED and *_PROT variants are noise here.
	if (type != UC_MEM_READ && type != UC_MEM_WRITE)
	{
		return true;
	}

	observer::DiagnosticEvent event;
	event.kind = type == UC_MEM_WRITE ? observer::DiagnosticKind::MemoryWrite :
					    observer::DiagnosticKind::MemoryRead;
	event.address = address;
	event.size = static_cast<uint32_t>(size);
	if (event.kind == observer::DiagnosticKind::MemoryWrite && size <= 8)
	{
		event.value = static_cast<uint64_t>(value);
	}
	if (uc_reg_read(uc, UC_ARM64_REG_PC, &event.pc) != UC_ERR_OK)
	{
		event.pc = 0;
	}

	engine->diagnostic_sink_(event);
	return true;
}

} // namespace duckemu::runtime
