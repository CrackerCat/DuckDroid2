// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/loader/module_registry.hpp"

#include <utility>

namespace duckemu::runtime::loader
{

Module &ModuleRegistry::add(std::unique_ptr<Module> module)
{
	module->handle = next_handle_++;
	Module &reference = *module;
	modules_.push_back(std::move(module));
	reindex();
	return reference;
}

void ModuleRegistry::erase(uint64_t handle)
{
	for (auto it = modules_.begin(); it != modules_.end(); ++it)
	{
		if ((*it)->handle == handle)
		{
			modules_.erase(it);
			reindex();
			return;
		}
	}
}

void ModuleRegistry::reindex()
{
	by_soname_.clear();
	by_handle_.clear();
	for (size_t i = 0; i < modules_.size(); ++i)
	{
		by_soname_[modules_[i]->soname] = i;
		by_handle_[modules_[i]->handle] = i;
	}
}

Module *ModuleRegistry::findBySoname(std::string_view soname) const
{
	const auto it = by_soname_.find(std::string(soname));
	return it == by_soname_.end() ? nullptr : modules_[it->second].get();
}

Module *ModuleRegistry::findByHandle(uint64_t handle) const
{
	const auto it = by_handle_.find(handle);
	return it == by_handle_.end() ? nullptr : modules_[it->second].get();
}

Module *ModuleRegistry::findByPath(std::string_view path) const
{
	for (const auto &module : modules_)
	{
		if (!module->is_virtual && module->path == path)
		{
			return module.get();
		}
	}
	return nullptr;
}

Module *ModuleRegistry::findContaining(uint64_t address) const
{
	for (const auto &module : modules_)
	{
		if (module->contains(address))
		{
			return module.get();
		}
	}
	return nullptr;
}

} // namespace duckemu::runtime::loader
