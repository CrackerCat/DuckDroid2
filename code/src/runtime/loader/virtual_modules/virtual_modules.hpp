#pragma once

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

	// Does `module_soname` declare `symbol`? Membership only; use resolve()
	// to get an address.
	[[nodiscard]] bool declares(std::string_view module_soname, std::string_view symbol) const noexcept;

	// True when any enabled virtual module declares `symbol`.
	[[nodiscard]] bool declaresAny(std::string_view symbol) const noexcept;

	// Creates (once) the thunk that implements `symbol`.
	Result<GuestVA> resolve(std::string_view symbol);

	// Diagnostics: how many symbols were served.
	[[nodiscard]] size_t resolvedCount() const
	{
		return thunks_by_name_.size();
	}

    private:
	RuntimeMemory &memory_;
	ThunkTable &thunks_;
	bool test_stubs_;
	std::vector<std::pair<std::string, GuestVA> > thunks_by_name_;

	Result<HostStub> makeStub(std::string_view symbol);
};

} // namespace duckemu::runtime::loader
