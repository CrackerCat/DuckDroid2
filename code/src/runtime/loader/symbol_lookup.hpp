#pragma once

#include <string_view>

#include "common/error/result.hpp"
#include "common/id/ids.hpp"
#include "runtime/loader/module.hpp"
#include "runtime/loader/module_registry.hpp"
#include "runtime/loader/virtual_modules/virtual_modules.hpp"

namespace duckemu::runtime::loader
{

struct ResolvedSymbol
{
	GuestVA address;
	Module *owner = nullptr; // null when served by a virtual module
	bool from_virtual = false;
};

// ELF symbol scoping for one instance. M4 uses the bionic global-group order:
// the requesting module, its DT_NEEDED closure (BFS), the global load order,
// then the virtual bionic table. Version requirements (DT_VERSYM) are ignored.
class SymbolResolver
{
    public:
	SymbolResolver(ModuleRegistry &registry, VirtualBionic &virtuals);

	// Resolves a reference made from `requester`. Weak undefined references
	// resolve to 0 when nothing matches.
	[[nodiscard]] Result<ResolvedSymbol> resolve(const Module &requester,
		std::string_view name,
		bool weak_reference) const;

	// Resolves a symbol exported by exactly one module (host lookup path).
	[[nodiscard]] Result<GuestVA> lookupExported(const Module &module, std::string_view name) const;

	// Definition of `name` inside one module, ignoring undefined entries.
	static bool findDefined(const Module &module, std::string_view name, GuestVA &address);

    private:
	ModuleRegistry &registry_;
	VirtualBionic &virtuals_;
};

} // namespace duckemu::runtime::loader
