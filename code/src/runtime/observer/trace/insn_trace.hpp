// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common/error/result.hpp"
#include "common/id/ids.hpp"

namespace duckemu::runtime
{
class RuntimeMemory;
class ThunkTable;
namespace loader
{
class ModuleRegistry;
}
namespace observer
{

// One instruction-level trace session. `begin`/`begin + size` must stay inside
// one loaded module: `offset` in code.log is relative to that module's base.
// `max_instructions == 0` means unlimited; reaching the budget stops recording
// but does not stop emulation.
struct InsnTraceOptions
{
	std::string directory;
	GuestVA begin;
	uint64_t size = 0;
	uint64_t max_instructions = 0;
	// QBDI-style nested trace end: arm at the first in-range instruction
	// (the called function's entry), capture its LR, and stop recording when
	// the PC reaches that LR. `begin/size` is only the hook coverage (normally
	// the target module); no function-size guess is involved.
	bool stop_at_return = false;
};

// The engine-facing half of the trace. Implemented by the file sink when
// DUCKEMU_ENABLE_TRACE is on and by a stub that refuses to open otherwise.
//
// Calls arrive on the emulating thread, under the instance execution lock:
//   onInstruction   UC_HOOK_CODE, before the instruction runs
//   onMemoryAccess  UC_HOOK_MEM_READ|WRITE, at the access
//   onRunEnd        after uc_emu_start returns (flushes the POST of the last
//                   instruction of the run)
class InsnTraceSink
{
    public:
	virtual ~InsnTraceSink() = default;

	// `engine` is the uc_engine* used for register reads; `engine_id` is a
	// stable per-engine key (pointers can be reused after an engine dies).
	virtual void onInstruction(void *engine, uint64_t engine_id, uint32_t tid, uint64_t address) = 0;
	virtual void onMemoryAccess(void *engine,
		uint64_t engine_id,
		uint32_t tid,
		bool write,
		uint64_t address,
		uint64_t size,
		uint64_t value) = 0;
	virtual void onRunEnd(void *engine, uint64_t engine_id) = 0;
};

// Opens code.log / bl.log / rw.log / threads.tsv under options.directory and
// validates the trace range. Returns Unsupported when the build has no
// instruction trace (no capstone).
[[nodiscard]] Result<std::unique_ptr<InsnTraceSink> > createInsnTrace(
	const InsnTraceOptions &options,
	RuntimeMemory &memory,
	const loader::ModuleRegistry &modules,
	const ThunkTable *thunks);

} // namespace observer
} // namespace duckemu::runtime
