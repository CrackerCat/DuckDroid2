// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/error/result.hpp"
#include "common/id/ids.hpp"
#include "runtime/memory/thunk/thunk_table.hpp"

namespace duckemu::runtime
{
class RuntimeMemory;
}

namespace duckemu::runtime::loader
{

class BionicGlobals;
class DlService;

// Per-instance state behind the __loader_dl* thunks: the service pointer, the
// sticky dlerror message and the guest scratch used for strings and
// dl_phdr_info records. Shared with the thunks so its lifetime is independent
// of this object.
struct DlStubState;

// Host implementations behind the virtual modules. Production only virtualizes
// what a real Android linker provides (ld-android.so); the modules that real
// bionic supplies from disk (libc/libm/libdl/liblog/...) are loaded from files.
//
// `enable_test_import_stubs` re-enables the M4 fixture table (libc/liblog
// stubs). It exists so 04_so_load keeps testing the thunk path without pulling
// real libc into that test; the M4.5 gate leaves it off.
class VirtualBionic
{
    public:
	explicit VirtualBionic(RuntimeMemory &memory,
		ThunkTable &thunks,
		bool enable_test_import_stubs = false);

	// True for the module names that are satisfied without a file.
	[[nodiscard]] bool isVirtualModule(std::string_view soname) const noexcept;

	// Platform modules that are only needed so an app SO can resolve its
	// DT_NEEDED list. Their symbols are import stubs returning 0; the target
	// must not call them on the supported path. Resolution is scoped to a
	// requester that names the module, so global lookup stays strict.
	[[nodiscard]] bool isImportStub(std::string_view soname) const noexcept;

	// Generic return-0 thunk for a symbol from an import-stub module.
	Result<GuestVA> resolveStub(std::string_view symbol);

	// Does `module_soname` declare `symbol`? Membership only; use resolve()
	// to get an address.
	[[nodiscard]] bool declares(std::string_view module_soname, std::string_view symbol) const noexcept;

	// True when any enabled virtual module declares `symbol`.
	[[nodiscard]] bool declaresAny(std::string_view symbol) const noexcept;

	// Creates (once) the thunk that implements `symbol`.
	Result<GuestVA> resolve(std::string_view symbol);

	// Lets __loader_shared_globals return the guest libc_shared_globals block.
	void setBionicGlobals(BionicGlobals *globals)
	{
		bionic_globals_ = globals;
	}

	// Backs the __loader_dl* thunks. Without a service they stay the M4
	// binding-only stubs (return 0).
	void setDlService(DlService *service);

	// Diagnostics: how many symbols were served.
	[[nodiscard]] size_t resolvedCount() const
	{
		return thunks_by_name_.size();
	}

    private:
	RuntimeMemory &memory_;
	ThunkTable &thunks_;
	bool test_stubs_;
	BionicGlobals *bionic_globals_ = nullptr;
	std::shared_ptr<DlStubState> dl_;
	std::vector<std::pair<std::string, GuestVA> > thunks_by_name_;

	Result<HostStub> makeStub(std::string_view symbol);

	// One of the __loader_dl* entry points, or std::nullopt for anything else.
	// Always returns a stub when the symbol is one of them, even before
	// setDlService, so the binding surface does not change with configuration.
	std::optional<HostStub> makeDlStub(std::string_view symbol);
};

} // namespace duckemu::runtime::loader
