// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
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
	uint8_t type = 0;       // STT_* of the definition, 0 when virtual/unknown
	uint64_t value = 0;     // st_value of the definition (TLS: segment offset)
};

// A TLS symbol resolved across the same scope order as ordinary symbols.
struct ResolvedTlsSymbol
{
	const Module *owner = nullptr;
	uint64_t value = 0;
	bool weak = false;
};

// ELF symbol scoping for one instance. M4 uses the bionic global-group order:
// the requesting module, its DT_NEEDED closure (BFS), the global load order,
// then the virtual import table. Version requirements (DT_VERSYM) are ignored.
class SymbolResolver
{
    public:
	SymbolResolver(ModuleRegistry &registry, VirtualBionic &virtuals);

	// Resolves a reference made from `requester`. Weak undefined references
	// resolve to 0 when nothing matches.
	[[nodiscard]] Result<ResolvedSymbol> resolve(const Module &requester,
		std::string_view name,
		bool weak_reference) const;

	// Resolves a TLS symbol reference. Only modules with a PT_TLS segment can
	// own one. `weak` is set when an undefined weak reference has no match.
	[[nodiscard]] Result<ResolvedTlsSymbol> resolveTls(const Module &requester,
		std::string_view name,
		bool weak_reference) const;

	// Resolves a symbol exported by exactly one module (host lookup path).
	[[nodiscard]] Result<GuestVA> lookupExported(const Module &module, std::string_view name) const;

	// Definition of `name` inside one module, ignoring undefined entries.
	static bool findDefined(const Module &module, std::string_view name, GuestVA &address);
	static const ElfSymbol *findDefinition(const Module &module, std::string_view name);

    private:
	ModuleRegistry &registry_;
	VirtualBionic &virtuals_;
};

} // namespace duckemu::runtime::loader
