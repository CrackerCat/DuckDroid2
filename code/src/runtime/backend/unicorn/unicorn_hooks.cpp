// Unicorn hooks -> observer events.
//
// Three callbacks live here, all installed by the backend:
//   - UC_HOOK_INTR: every SVC trap, handed to the SvcDispatcher
//   - UC_HOOK_MEM_WRITE: guest writes, used for code-page invalidation (SMC)
//   - UC_HOOK_MEM_READ|WRITE: optional diagnostic feed (M2: opt-in)

#include "runtime/backend/unicorn/unicorn_backend.hpp"

#include <utility>

#include "common/log/logger.hpp"
#include "runtime/memory/address_space.hpp"

namespace duckemu::runtime
{

void UnicornBackend::setDiagnosticSink(std::function<void(const observer::DiagnosticEvent &)> sink)
{
	diagnostic_sink_ = std::move(sink);
	if (!diagnostic_sink_) {
		return;
	}

	const uc_err err = uc_hook_add(engine_,
		&diagnostic_hook_,
		UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE,
		reinterpret_cast<void *>(&UnicornBackend::onDiagnosticAccess),
		this,
		1,
		0);
	if (err != UC_ERR_OK) {
		DUCKEMU_WARN("uc_hook_add(UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE) failed: %s", uc_strerror(err));
	}
}

void UnicornBackend::onIntr(uc_engine *uc, uint32_t intno, void *user_data)
{
	auto *backend = static_cast<UnicornBackend *>(user_data);
	if (backend->svc_handler_) {
		backend->svc_handler_(uc, intno);
		return;
	}
	DUCKEMU_ERROR("UC_HOOK_INTR (intno=%u) without a handler; stopping", intno);
	uc_emu_stop(uc);
}

bool UnicornBackend::onMemWrite(uc_engine *uc,
	uc_mem_type type,
	uint64_t address,
	int size,
	int64_t value,
	void *user_data)
{
	(void)uc;
	(void)type;
	(void)value;
	auto *backend = static_cast<UnicornBackend *>(user_data);
	if (size <= 0) {
		return true;
	}

	const uint64_t first_page = pageDown(address);
	const uint64_t last_page = pageDown(address + static_cast<uint64_t>(size) - 1);
	for (uint64_t page = first_page;; page += kGuestPageSize) {
		if (backend->memory_.isExecutable(GuestVA{ page })) {
			backend->invalidateCode(GuestVA{ page }, kGuestPageSize);
		}
		if (page >= last_page) {
			break;
		}
	}
	return true;
}

bool UnicornBackend::onDiagnosticAccess(uc_engine *uc,
	uc_mem_type type,
	uint64_t address,
	int size,
	int64_t value,
	void *user_data)
{
	auto *backend = static_cast<UnicornBackend *>(user_data);
	if (backend->diagnostic_sink_ == nullptr || size <= 0) {
		return true;
	}
	// Only successful accesses; *_UNMAPPED and *_PROT variants are noise here.
	if (type != UC_MEM_READ && type != UC_MEM_WRITE) {
		return true;
	}

	observer::DiagnosticEvent event;
	event.kind = type == UC_MEM_WRITE ? observer::DiagnosticKind::MemoryWrite :
					    observer::DiagnosticKind::MemoryRead;
	event.address = address;
	event.size = static_cast<uint32_t>(size);
	if (event.kind == observer::DiagnosticKind::MemoryWrite && size <= 8) {
		event.value = static_cast<uint64_t>(value);
	}
	if (uc_reg_read(uc, UC_ARM64_REG_PC, &event.pc) != UC_ERR_OK) {
		event.pc = 0;
	}

	backend->diagnostic_sink_(event);
	return true;
}

} // namespace duckemu::runtime
