// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/error/result.hpp"
#include "common/id/ids.hpp"

namespace duckemu::runtime
{
class RandomSource;
class RuntimeMemory;
}

namespace duckemu::runtime::loader
{

struct BionicGlobalsConfig
{
	uint32_t pid = 1000;
	uint32_t uid = 1000;
	uint32_t gid = 1000;
	// AT_HWCAP / AT_HWCAP2; zero is the baseline this backend implements.
	uint64_t hwcap = 0;
	uint64_t hwcap2 = 0;
	std::string progname = "duckemu";
	std::vector<std::string> environment;
	// AT_RANDOM (also the stack canary seed) comes from the instance stream, so
	// a seeded replay sees identical canaries. Null keeps host entropy.
	RandomSource *random = nullptr;
};

// One entry of the guest-visible TlsModules table. Field names follow
// bionic's private/bionic_elf_tls.h; see the devlog for the layout proof.
struct TlsModuleSpec
{
	uint64_t segment_size = 0;
	uint64_t segment_alignment = 1;
	GuestVA init_ptr; // guest address of the .tdata initialization image
	uint64_t init_size = 0;
	// Block-relative offset for static TLS, SIZE_MAX for a dynamic module.
	uint64_t static_offset = ~uint64_t{ 0 };
	uint64_t soinfo = 0; // non-zero while the module is loaded
};

// The guest-side libc_shared_globals block that a real Android linker owns.
// libc's __libc_shared_globals() forwards to __loader_shared_globals(), which
// our virtual ld-android.so serves. Field offsets are verified against the
// selected ROM's libc; see docs/devlogs/2026-10-28-15-M4.6设计.md §2.
class BionicGlobals
{
    public:
	BionicGlobals(RuntimeMemory &memory, BionicGlobalsConfig config);

	Result<void> initialize();

	[[nodiscard]] GuestVA address() const
	{
		return block_;
	}

	// The auxv array written into the block; /proc/self/auxv serves these bytes.
	[[nodiscard]] GuestVA auxvAddress() const
	{
		return auxv_address_;
	}
	[[nodiscard]] uint64_t auxvSize() const
	{
		return auxv_size_;
	}

	// Published to StaticTlsLayout before preinit runs. The loader owns the
	// actual per-module layout; this method only mirrors its totals.
	void setStaticTlsLayout(uint64_t size, uint64_t alignment, uint64_t tcb_offset, uint64_t bionic_tls_offset);

	// Registers one ELF TLS module in the guest-visible TlsModules table and
	// returns the generation bionic assigned to it. libc's thread creation
	// copies the segment image from this table (__init_static_tls), and
	// __tls_get_addr uses the generation to grow each thread's DTV.
	//
	// Registration order must match module ids (bionic indexes the table with
	// module_id - 1). Static modules must all precede the first dynamic one;
	// finalizeStaticTls() closes the static prefix.
	Result<uint64_t> addTlsModule(uint32_t module_id, const TlsModuleSpec &spec, bool is_static);

	// Mirrors linker_finalize_static_tls(): from now on every module is
	// dynamic (dlopen), so it gets a DTV slot instead of a static slot.
	void finalizeStaticTls();

	// Load failure path: keeps the id reserved (static modules before it must
	// stay contiguous in the table) but turns the entry into a zero-sized
	// placeholder so libc neither copies an image nor allocates a block.
	void abandonTlsModule(uint32_t module_id);

	[[nodiscard]] uint32_t tlsModuleCount() const
	{
		return static_cast<uint32_t>(tls_module_count_);
	}
	[[nodiscard]] uint32_t tlsStaticModuleCount() const
	{
		return static_cast<uint32_t>(tls_static_module_count_);
	}
	[[nodiscard]] bool staticTlsFinalized() const
	{
		return static_tls_finished_;
	}

    private:
	Result<void> writeU64(uint64_t offset, uint64_t value);
	Result<uint64_t> readU64(uint64_t offset) const;
	Result<void> bumpTlsGeneration(uint64_t generation);

	RuntimeMemory &memory_;
	BionicGlobalsConfig config_;
	GuestVA block_;

	GuestVA auxv_address_;
	uint64_t auxv_size_ = 0;
	uint64_t tls_size_ = 0;
	uint64_t tls_alignment_ = 16;
	uint64_t tls_tcb_offset_ = 0;
	uint64_t tls_bionic_tls_offset_ = 0;

	// Guest-visible TlsModules table.
	GuestVA tls_module_table_;
	uint64_t tls_module_capacity_ = 0;
	uint64_t tls_module_count_ = 0;
	uint64_t tls_static_module_count_ = 0;
	uint64_t tls_generation_ = 1; // bionic's kTlsGenerationFirst
	bool static_tls_finished_ = false;
};

} // namespace duckemu::runtime::loader
