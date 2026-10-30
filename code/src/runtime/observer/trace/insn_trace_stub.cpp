// SPDX-License-Identifier: GPL-2.0-only
// Builds without -DDUCKEMU_ENABLE_TRACE. The API stays linkable so callers do
// not need their own #ifdef; starting a trace reports why it is unavailable.
#include "runtime/observer/trace/insn_trace.hpp"

namespace duckemu::runtime::observer
{

Result<std::unique_ptr<InsnTraceSink> > createInsnTrace(
	const InsnTraceOptions &options,
	RuntimeMemory &memory,
	const loader::ModuleRegistry &modules,
	const ThunkTable *thunks)
{
	(void)options;
	(void)memory;
	(void)modules;
	(void)thunks;
	return Error{ ErrorCode::Unsupported,
		"instruction trace is not built (configure with -DDUCKEMU_ENABLE_TRACE=ON)" };
}

} // namespace duckemu::runtime::observer
