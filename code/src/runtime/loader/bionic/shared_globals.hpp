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

    private:
	Result<void> writeU64(uint64_t offset, uint64_t value);

	RuntimeMemory &memory_;
	BionicGlobalsConfig config_;
	GuestVA block_;

	GuestVA auxv_address_;
	uint64_t auxv_size_ = 0;
	uint64_t tls_size_ = 0;
	uint64_t tls_alignment_ = 16;
	uint64_t tls_tcb_offset_ = 0;
	uint64_t tls_bionic_tls_offset_ = 0;
};

} // namespace duckemu::runtime::loader
