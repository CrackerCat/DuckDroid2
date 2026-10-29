// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/loader/symbol_lookup.hpp"

#include <deque>
#include <unordered_set>

namespace duckemu::runtime::loader
{
namespace
{

constexpr uint8_t kStbGlobal = 1;
constexpr uint8_t kStbWeak = 2;
constexpr uint8_t kSttTls = 6;

} // namespace

SymbolResolver::SymbolResolver(ModuleRegistry &registry, VirtualBionic &virtuals)
	: registry_(registry)
	, virtuals_(virtuals)
{
}

const ElfSymbol *SymbolResolver::findDefinition(const Module &module, std::string_view name)
{
	if (module.is_virtual)
	{
		return nullptr;
	}

	const ElfSymbol *weak = nullptr;
	for (const ElfSymbol &symbol : module.symbols)
	{
		if (symbol.name != name || symbol.undefined() || symbol.name.empty())
		{
			continue;
		}
		const uint8_t bind = symbol.bind();
		if (bind == kStbGlobal)
		{
			return &symbol;
		}
		if (bind == kStbWeak && weak == nullptr)
		{
			weak = &symbol;
		}
	}
	return weak;
}

bool SymbolResolver::findDefined(const Module &module, std::string_view name, GuestVA &address)
{
	const ElfSymbol *symbol = findDefinition(module, name);
	if (symbol == nullptr)
	{
		return false;
	}
	address = GuestVA{ module.base + symbol->value };
	return true;
}

Result<ResolvedSymbol> SymbolResolver::resolve(const Module &requester,
	std::string_view name,
	bool weak_reference) const
{
	if (const ElfSymbol *symbol = findDefinition(requester, name))
	{
		return ResolvedSymbol{ GuestVA{ requester.base + symbol->value },
			const_cast<Module *>(&requester),
			false,
			symbol->type(),
			symbol->value };
	}

	// DT_NEEDED closure, breadth first, in load order.
	std::unordered_set<std::string> visited;
	std::deque<const Module *> queue;
	visited.insert(requester.soname);
	for (const std::string &needed : requester.needed)
	{
		if (Module *dependency = registry_.findBySoname(needed))
		{
			if (visited.insert(dependency->soname).second)
			{
				queue.push_back(dependency);
			}
		}
	}
	while (!queue.empty())
	{
		const Module *module = queue.front();
		queue.pop_front();
		if (const ElfSymbol *symbol = findDefinition(*module, name))
		{
			return ResolvedSymbol{ GuestVA{ module->base + symbol->value },
				const_cast<Module *>(module),
				false,
				symbol->type(),
				symbol->value };
		}
		for (const std::string &needed : module->needed)
		{
			if (Module *dependency = registry_.findBySoname(needed))
			{
				if (visited.insert(dependency->soname).second)
				{
					queue.push_back(dependency);
				}
			}
		}
	}

	// Global scope: every real module in load order.
	for (const auto &module : registry_.modules())
	{
		if (module->is_virtual || visited.count(module->soname) != 0)
		{
			continue;
		}
		if (const ElfSymbol *symbol = findDefinition(*module, name))
		{
			return ResolvedSymbol{ GuestVA{ module->base + symbol->value },
				module.get(),
				false,
				symbol->type(),
				symbol->value };
		}
	}

	// Import-stub modules named by the requester's DT_NEEDED list: symbols
	// nobody else defines become return-0 thunks. Checked after real modules,
	// so a symbol a loaded library really provides is never shadowed.
	for (const std::string &needed : requester.needed)
	{
		if (!virtuals_.isImportStub(needed))
		{
			continue;
		}
		auto thunk = virtuals_.resolveStub(name);
		if (!thunk.ok())
		{
			return thunk.error();
		}
		return ResolvedSymbol{ thunk.value(), nullptr, true, 0, 0 };
	}

	// Virtual import table. Membership is checked by name; the thunk is created
	// on first use and cached inside VirtualBionic.
	if (virtuals_.declaresAny(name))
	{
		auto thunk = virtuals_.resolve(name);
		if (!thunk.ok())
		{
			return thunk.error();
		}
		return ResolvedSymbol{ thunk.value(), nullptr, true, 0, 0 };
	}

	if (weak_reference)
	{
		return ResolvedSymbol{ GuestVA{}, nullptr, false, 0, 0 };
	}

	std::string message = "undefined symbol '" + std::string(name) + "' referenced by " + requester.soname;
	return Error{ ErrorCode::NotFound, std::move(message) };
}

Result<ResolvedTlsSymbol> SymbolResolver::resolveTls(const Module &requester,
	std::string_view name,
	bool weak_reference) const
{
	auto fromModule = [&](const Module &module, const ElfSymbol &symbol) -> Result<ResolvedTlsSymbol> {
		if (symbol.type() != kSttTls)
		{
			return Error{ ErrorCode::InvalidArgument,
				module.soname + ": TLS relocation references non-TLS symbol '" +
					std::string(name) + "'" };
		}
		if (!module.has_tls)
		{
			return Error{ ErrorCode::InvalidArgument,
				module.soname + ": TLS symbol '" + std::string(name) +
					"' is defined in a module without PT_TLS" };
		}
		return ResolvedTlsSymbol{ &module, symbol.value, false };
	};

	if (const ElfSymbol *symbol = findDefinition(requester, name))
	{
		return fromModule(requester, *symbol);
	}

	std::unordered_set<std::string> visited;
	std::deque<const Module *> queue;
	visited.insert(requester.soname);
	for (const std::string &needed : requester.needed)
	{
		if (Module *dependency = registry_.findBySoname(needed))
		{
			if (visited.insert(dependency->soname).second)
			{
				queue.push_back(dependency);
			}
		}
	}
	while (!queue.empty())
	{
		const Module *module = queue.front();
		queue.pop_front();
		if (const ElfSymbol *symbol = findDefinition(*module, name))
		{
			return fromModule(*module, *symbol);
		}
		for (const std::string &needed : module->needed)
		{
			if (Module *dependency = registry_.findBySoname(needed))
			{
				if (visited.insert(dependency->soname).second)
				{
					queue.push_back(dependency);
				}
			}
		}
	}

	for (const auto &module : registry_.modules())
	{
		if (module->is_virtual || visited.count(module->soname) != 0)
		{
			continue;
		}
		if (const ElfSymbol *symbol = findDefinition(*module, name))
		{
			return fromModule(*module, *symbol);
		}
	}

	if (weak_reference)
	{
		return ResolvedTlsSymbol{ nullptr, 0, true };
	}

	return Error{ ErrorCode::NotFound,
		"undefined TLS symbol '" + std::string(name) + "' referenced by " + requester.soname };
}

Result<GuestVA> SymbolResolver::lookupExported(const Module &module, std::string_view name) const
{
	if (module.is_virtual)
	{
		if (virtuals_.isImportStub(module.soname))
		{
			return virtuals_.resolveStub(name);
		}
		if (!virtuals_.declares(module.soname, name) && !virtuals_.declaresAny(name))
		{
			return Error{ ErrorCode::NotFound,
				"virtual module " + module.soname + " does not provide '" + std::string(name) + "'" };
		}
		return virtuals_.resolve(name);
	}

	GuestVA address;
	if (!findDefined(module, name, address))
	{
		return Error{ ErrorCode::NotFound,
			module.soname + " does not export '" + std::string(name) + "'" };
	}
	return address;
}

} // namespace duckemu::runtime::loader
