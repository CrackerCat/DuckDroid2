// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace duckemu::runtime::observer
{

enum class ModuleEventKind : uint8_t
{
	Load = 0,
	Needed,
	Resolve,
};

constexpr std::string_view moduleEventKindName(ModuleEventKind kind) noexcept
{
	switch (kind)
	{
	case ModuleEventKind::Load:
		return "load";
	case ModuleEventKind::Needed:
		return "needed";
	case ModuleEventKind::Resolve:
		return "resolve";
	}
	return "?";
}

// One module-lifecycle fact. `id` is the publication order inside the instance
// (assigned by the bus). `detail` is the dependency soname for Needed, the
// symbol name for Resolve, and the module path for Load. `ok` is false when a
// lookup failed; the loader still publishes, so a failed load leaves a trace.
struct ModuleEvent
{
	uint64_t id = 0;
	ModuleEventKind kind = ModuleEventKind::Load;
	std::string soname;
	uint64_t base = 0;
	bool ok = true;
	std::string detail;
};

} // namespace duckemu::runtime::observer
