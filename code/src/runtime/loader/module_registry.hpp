#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "runtime/loader/module.hpp"

namespace duckemu::runtime::loader
{

// Owns every loaded module of one RuntimeInstance and keeps the soname and
// handle indexes. Load order is preserved for the global symbol scope.
class ModuleRegistry
{
    public:
	ModuleRegistry() = default;
	ModuleRegistry(const ModuleRegistry &) = delete;
	ModuleRegistry &operator=(const ModuleRegistry &) = delete;

	// Registers `module` and assigns its handle.
	Module &add(std::unique_ptr<Module> module);
	void erase(uint64_t handle);

	[[nodiscard]] Module *findBySoname(std::string_view soname) const;
	[[nodiscard]] Module *findByHandle(uint64_t handle) const;
	[[nodiscard]] Module *findByPath(std::string_view path) const;
	[[nodiscard]] Module *findContaining(uint64_t address) const;

	// Modules in load order (dependencies before dependants).
	[[nodiscard]] const std::vector<std::unique_ptr<Module> > &modules() const
	{
		return modules_;
	}
	[[nodiscard]] size_t size() const
	{
		return modules_.size();
	}

    private:
	void reindex();

	std::vector<std::unique_ptr<Module> > modules_;
	std::unordered_map<std::string, size_t> by_soname_;
	std::unordered_map<uint64_t, size_t> by_handle_;
	uint64_t next_handle_ = 1;
};

} // namespace duckemu::runtime::loader
