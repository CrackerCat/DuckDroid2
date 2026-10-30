// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "common/error/result.hpp"
#include "common/id/ids.hpp"
#include "runtime/loader/elf/elf_image.hpp"
#include "runtime/loader/module_registry.hpp"
#include "runtime/loader/reloc/reloc_applier.hpp"
#include "runtime/loader/symbol_lookup.hpp"
#include "runtime/loader/tls/tls_layout.hpp"
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

class BionicGlobals;
class DlService;

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
		std::vector<std::string> search_paths,
		bool enable_test_import_stubs = false);

	ModuleLoader(const ModuleLoader &) = delete;
	ModuleLoader &operator=(const ModuleLoader &) = delete;

	[[nodiscard]] Result<uint64_t> load(std::string_view path);

	// dlopen entry point: a path when `name` contains '/', otherwise a
	// DT_NEEDED-style name looked up in `requester_directory` and then the
	// configured search paths. An already-loaded soname returns its handle.
	[[nodiscard]] Result<uint64_t> loadByName(std::string_view name,
		std::string_view requester_directory);

	[[nodiscard]] Result<GuestVA> lookup(uint64_t handle, std::string_view symbol) const;
	[[nodiscard]] Result<GuestVA> lookupGlobal(std::string_view symbol) const;

	// Same scopes, but keeps the definition's STT_* type and st_value. dlsym
	// needs both for STT_TLS, whose result is a thread-local address rather
	// than module.base + st_value.
	[[nodiscard]] Result<ResolvedSymbol> lookupResolved(uint64_t handle, std::string_view symbol) const;
	[[nodiscard]] Result<ResolvedSymbol> lookupGlobalResolved(std::string_view symbol) const;

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

	// IFUNC/IRELATIVE slots whose resolver still has to run in the guest.
	// Draining is mandatory after a load attempt, successful or not.
	[[nodiscard]] std::vector<IfuncFixup> takeIfuncFixups();

	// Writes a module's PT_TLS image into the calling thread's TLS block. Set by
	// RuntimeInstance once the main TLS instance exists.
	using TlsTemplateSink =
		std::function<Result<void>(int64_t tls_offset, const void *data, uint64_t size, uint64_t memsz)>;
	void setTlsTemplateSink(TlsTemplateSink sink)
	{
		tls_sink_ = std::move(sink);
	}

	[[nodiscard]] const TlsLayout &tlsLayout() const
	{
		return tls_layout_;
	}

	void setBionicGlobals(BionicGlobals *globals)
	{
		globals_ = globals;
		virtuals_.setBionicGlobals(globals);
	}

	// Guest address of the host thunk that implements bionic's
	// tlsdesc_resolver_dynamic. The loader writes it into TLSDESC descriptors
	// of modules that are not part of the static TLS block.
	void setDynamicTlsDescResolver(GuestVA resolver)
	{
		relocations_.setDynamicTlsDescResolver(resolver);
	}

	// linker_finalize_static_tls(): the startup graph is complete. Modules
	// loaded afterwards get dynamic TLS (DTV) instead of a static slot.
	void finalizeStaticTls();

	[[nodiscard]] bool staticTlsFinalized() const
	{
		return static_tls_finished_;
	}

	[[nodiscard]] const BionicGlobals *bionicGlobals() const
	{
		return globals_;
	}

	// Backs the __loader_dl* entry points. Set once the facade exists.
	void setDlService(DlService *service)
	{
		virtuals_.setDlService(service);
	}

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
	std::vector<IfuncFixup> ifunc_fixups_;
	RelocApplier relocations_;
	TlsLayout tls_layout_;
	uint32_t next_tls_module_id_ = 1;
	bool static_tls_finished_ = false;
	TlsTemplateSink tls_sink_;
	BionicGlobals *globals_ = nullptr;
	std::vector<Initializer> initializers_;
	uint64_t next_base_;
};

} // namespace duckemu::runtime::loader
