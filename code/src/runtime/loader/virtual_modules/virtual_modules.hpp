#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
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

// Host implementations behind a virtual bionic module. A module that imports
// `write` gets the thunk page entry created here; nothing is read from disk.
//
// M4 only implements the symbols the gate needs. Everything else is reported
// as unresolved with the symbol name, which is what a real target would hit.
class VirtualBionic
{
    public:
	explicit VirtualBionic(RuntimeMemory &memory, ThunkTable &thunks);

	// True for the module names that are satisfied without a file.
	static bool isVirtualModule(std::string_view soname) noexcept;

	// Names of the virtual modules this build knows.
	static std::vector<std::string_view> moduleNames();

	// Does `module_soname` declare `symbol`? Membership only; use resolve()
	// to get an address.
	static bool declares(std::string_view module_soname, std::string_view symbol) noexcept;

	// True when any virtual module declares `symbol`.
	static bool declaresAny(std::string_view symbol) noexcept;

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
	std::vector<std::pair<std::string, GuestVA> > thunks_by_name_;

	Result<HostStub> makeStub(std::string_view symbol);
};

} // namespace duckemu::runtime::loader
