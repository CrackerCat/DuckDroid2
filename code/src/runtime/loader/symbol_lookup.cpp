#include "runtime/loader/symbol_lookup.hpp"

#include <deque>
#include <unordered_set>
#include <vector>

namespace duckemu::runtime::loader
{
namespace
{

constexpr uint8_t kStbGlobal = 1;
constexpr uint8_t kStbWeak = 2;

} // namespace

SymbolResolver::SymbolResolver(ModuleRegistry &registry, VirtualBionic &virtuals)
	: registry_(registry)
	, virtuals_(virtuals)
{
}

bool SymbolResolver::findDefined(const Module &module, std::string_view name, GuestVA &address)
{
	if (module.is_virtual)
	{
		return false;
	}

	bool found_weak = false;
	for (const ElfSymbol &symbol : module.symbols)
	{
		if (symbol.name != name || symbol.undefined() || symbol.name.empty())
		{
			continue;
		}
		const uint8_t bind = symbol.bind();
		if (bind != kStbGlobal && bind != kStbWeak)
		{
			continue;
		}
		if (bind == kStbWeak)
		{
			found_weak = true;
			continue;
		}
		address = GuestVA{ module.base + symbol.value };
		return true;
	}
	if (found_weak)
	{
		for (const ElfSymbol &symbol : module.symbols)
		{
			if (symbol.name == name && !symbol.undefined() && symbol.bind() == kStbWeak)
			{
				address = GuestVA{ module.base + symbol.value };
				return true;
			}
		}
	}
	return false;
}

Result<ResolvedSymbol> SymbolResolver::resolve(const Module &requester,
	std::string_view name,
	bool weak_reference) const
{
	GuestVA address;
	if (findDefined(requester, name, address))
	{
		return ResolvedSymbol{ address, const_cast<Module *>(&requester), false };
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
		if (findDefined(*module, name, address))
		{
			return ResolvedSymbol{ address, const_cast<Module *>(module), false };
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
		if (findDefined(*module, name, address))
		{
			return ResolvedSymbol{ address, module.get(), false };
		}
	}

	// Virtual bionic. Membership is checked by name; the thunk is created on
	// first use and cached inside VirtualBionic.
	if (VirtualBionic::declaresAny(name))
	{
		auto thunk = virtuals_.resolve(name);
		if (!thunk.ok())
		{
			return thunk.error();
		}
		return ResolvedSymbol{ thunk.value(), nullptr, true };
	}

	if (weak_reference)
	{
		return ResolvedSymbol{ GuestVA{}, nullptr, false };
	}

	std::string message = "undefined symbol '" + std::string(name) + "' referenced by " + requester.soname;
	return Error{ ErrorCode::NotFound, std::move(message) };
}

Result<GuestVA> SymbolResolver::lookupExported(const Module &module, std::string_view name) const
{
	if (module.is_virtual)
	{
		if (!VirtualBionic::declares(module.soname, name) && !VirtualBionic::declaresAny(name))
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
