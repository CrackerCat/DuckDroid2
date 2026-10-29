// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/id/ids.hpp"
#include "runtime/loader/elf/elf_image.hpp"

namespace duckemu::runtime::loader
{

enum class ModuleState : uint8_t
{
	Loading = 0,
	Loaded,
};

// One loaded object. Virtual bionic modules have no image, no base and no
// segments; their symbols resolve to thunk-page addresses on demand.
struct Module
{
	uint64_t handle = 0;
	std::string path;
	std::string directory; // of `path`, used to resolve this module's DT_NEEDED
	std::string soname;
	bool is_virtual = false;

	uint64_t base = 0;
	uint64_t span = 0;
	uint64_t align = 1;

	std::vector<ElfSegment> segments;
	std::vector<ElfSymbol> symbols;
	std::vector<std::string> needed;

	ElfTlsTemplate tls;
	bool has_tls = false;
	int64_t tls_offset = 0;        // relative to the thread pointer
	uint32_t tls_module_id = 0;    // 1-based, for DTPMOD64 / DTV
	bool tls_static = false;       // M4.5 treats every PT_TLS module as static

	GuestVA init_function;
	GuestVA init_array;
	uint64_t init_array_size = 0;
	bool initialized = false;

	ModuleState state = ModuleState::Loading;

	// Symbol name -> absolute guest address. Filled by the resolver; virtual
	// modules create thunks here, real modules cache lookups into themselves.
	std::unordered_map<std::string, GuestVA> resolved;

	[[nodiscard]] bool contains(uint64_t address) const
	{
		return !is_virtual && address >= base && address < base + span;
	}
};

} // namespace duckemu::runtime::loader
