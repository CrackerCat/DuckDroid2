#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/error/result.hpp"
#include "common/id/ids.hpp"
#include "runtime/loader/elf/elf_image.hpp"
#include "runtime/loader/module_registry.hpp"
#include "runtime/loader/reloc/reloc_applier.hpp"
#include "runtime/loader/symbol_lookup.hpp"
#include "runtime/loader/virtual_modules/virtual_modules.hpp"
#include "runtime/vfs/runtime_vfs.hpp"

namespace duckemu::runtime
{
class RuntimeMemory;
class ThunkTable;
namespace observer
{
class EventBus;
}
} // namespace duckemu::runtime

namespace duckemu::runtime::loader
{

// A constructor that still has to run. Dependencies come before dependants.
struct Initializer
{
	uint64_t handle = 0;
	GuestVA function;
};

// Maps a module graph into guest memory, relocates it and collects the
// constructors. It never runs guest code itself: the facade drains
// takeInitializers() and calls them through the scheduler, outside the
// execution lock it took for loading.
class ModuleLoader
{
    public:
	ModuleLoader(RuntimeMemory &memory,
		ThunkTable &thunks,
		observer::EventBus *events,
		std::vector<std::string> search_paths);

	ModuleLoader(const ModuleLoader &) = delete;
	ModuleLoader &operator=(const ModuleLoader &) = delete;

	[[nodiscard]] Result<uint64_t> load(std::string_view path);
	[[nodiscard]] Result<GuestVA> lookup(uint64_t handle, std::string_view symbol) const;
	[[nodiscard]] Result<GuestVA> lookupGlobal(std::string_view symbol) const;

	[[nodiscard]] const ModuleRegistry &registry() const
	{
		return registry_;
	}
	[[nodiscard]] size_t moduleCount() const
	{
		return registry_.size();
	}

	// Drains the constructors discovered by load() calls since the last drain.
	[[nodiscard]] std::vector<Initializer> takeInitializers();

    private:
	[[nodiscard]] Result<uint64_t> loadCanonical(const std::string &path);
	[[nodiscard]] Result<uint64_t> loadResolvedPath(const std::string &path);
	[[nodiscard]] Result<Module *> loadImage(const std::string &path, ElfImage image);
	[[nodiscard]] Result<Module *> loadVirtual(std::string_view soname);
	[[nodiscard]] Result<void> mapSegments(Module &module, const ElfImage &image);
	void unmapSegments(const Module &module);
	[[nodiscard]] Result<uint64_t> allocateBase(uint64_t span, uint64_t align);
	void collectInitializers(Module &module);

	RuntimeMemory &memory_;
	observer::EventBus *events_;
	vfs::RuntimeVfs vfs_;
	ModuleRegistry registry_;
	VirtualBionic virtuals_;
	SymbolResolver resolver_;
	RelocApplier relocations_;
	std::vector<Initializer> initializers_;
	uint64_t next_base_;
};

} // namespace duckemu::runtime::loader
