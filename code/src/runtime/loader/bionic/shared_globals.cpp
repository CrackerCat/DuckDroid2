// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/loader/bionic/shared_globals.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <random>
#include <utility>

#include "runtime/memory/address_space.hpp"

namespace duckemu::runtime::loader
{
namespace
{

// libc_shared_globals offsets for the selected ROM (lemonade bootstrap libc).
// Verified from the ROM's libc disassembly: getauxval [x0,#0x418],
// __libc_preinit_impl [x0,#0x478]/[x0,#0x480]/[x0,#0x628],
// __libc_init_common [x0,#0x630]/[x0,#0x638]. See the M4.6 design doc §2.
constexpr uint64_t kAuxvOffset = 0x418;
constexpr uint64_t kStaticTlsLayoutOffset = 0x450;
constexpr uint64_t kTlsGenerationOffset = 0x478;
constexpr uint64_t kTlsGenerationLibcOffset = 0x480;
constexpr uint64_t kTargetSdkHookOffset = 0x628;
constexpr uint64_t kPrognameOffset = 0x630;
constexpr uint64_t kEnvironOffset = 0x638;

constexpr uint64_t kBlockSize = 0x1000;
constexpr uint64_t kStringsSize = 0x1000;

// M1/M4.5 TLS block layout, published through StaticTlsLayout.
constexpr uint64_t kDefaultTlsSize = 0x4000;
constexpr uint64_t kBionicTlsOffset = 0x200;

// Kernel auxv types (asm-generic).
constexpr uint64_t kAtNull = 0;
constexpr uint64_t kAtPhdr = 3;
constexpr uint64_t kAtPhent = 4;
constexpr uint64_t kAtPhnum = 5;
constexpr uint64_t kAtPagesz = 6;
constexpr uint64_t kAtBase = 7;
constexpr uint64_t kAtEntry = 9;
constexpr uint64_t kAtUid = 11;
constexpr uint64_t kAtEuid = 12;
constexpr uint64_t kAtGid = 13;
constexpr uint64_t kAtEgid = 14;
constexpr uint64_t kAtHwcap = 16;
constexpr uint64_t kAtClktck = 17;
constexpr uint64_t kAtSecure = 23;
constexpr uint64_t kAtRandom = 25;
constexpr uint64_t kAtHwcap2 = 26;
constexpr uint64_t kAtExecfn = 31;
constexpr uint64_t kAtSysinfoEhdr = 33;

} // namespace

BionicGlobals::BionicGlobals(RuntimeMemory &memory, BionicGlobalsConfig config)
	: memory_(memory)
	, config_(std::move(config))
	, tls_size_(kDefaultTlsSize)
	, tls_alignment_(16)
	, tls_tcb_offset_(0)
	, tls_bionic_tls_offset_(kBionicTlsOffset)
{
}

Result<void> BionicGlobals::writeU64(uint64_t offset, uint64_t value)
{
	return memory_.write(GuestVA{ block_.value() + offset }, &value, sizeof(value));
}

Result<void> BionicGlobals::initialize()
{
	auto block = memory_.map(kBlockSize, PermReadWrite, "bionic-globals");
	if (!block.ok())
	{
		return block.error();
	}
	block_ = block.value();

	auto strings = memory_.map(kStringsSize, PermReadWrite, "bionic-strings");
	if (!strings.ok())
	{
		return strings.error();
	}
	const uint64_t strings_base = strings.value().value();
	uint64_t cursor = 0;
	auto reserve = [&](uint64_t size, uint64_t alignment) -> Result<uint64_t> {
		const uint64_t aligned = (cursor + alignment - 1) & ~(alignment - 1);
		if (aligned < cursor || aligned > kStringsSize || size > kStringsSize - aligned)
		{
			return Error{ ErrorCode::OutOfMemory, "bionic globals strings page overflow" };
		}
		cursor = aligned + size;
		return strings_base + aligned;
	};
	auto writeBytes = [&](uint64_t address, const void *data, uint64_t size) -> Result<void> {
		return memory_.write(GuestVA{ address }, data, size);
	};

	// AT_RANDOM is also the source of __stack_chk_guard.
	uint8_t random_bytes[16] = {};
	std::random_device device;
	uint64_t seed = (static_cast<uint64_t>(device()) << 32) ^ device();
	std::mt19937_64 generator(seed);
	for (size_t i = 0; i < sizeof(random_bytes); i += 8)
	{
		const uint64_t word = generator();
		std::memcpy(random_bytes + i, &word, sizeof(word));
	}
	if (std::all_of(std::begin(random_bytes), std::end(random_bytes), [](uint8_t b) { return b == 0; }))
	{
		random_bytes[0] = 1;
	}
	auto random_address = reserve(sizeof(random_bytes), 16);
	if (!random_address.ok())
	{
		return random_address.error();
	}
	if (auto written = writeBytes(random_address.value(), random_bytes, sizeof(random_bytes)); !written.ok())
	{
		return written.error();
	}

	auto writeString = [&](const std::string &value) -> Result<uint64_t> {
		const uint64_t size = value.size() + 1;
		auto address = reserve(size, 1);
		if (!address.ok())
		{
			return address.error();
		}
		if (auto written = writeBytes(address.value(), value.data(), value.size()); !written.ok())
		{
			return written.error();
		}
		const char nul = '\0';
		if (auto written = writeBytes(address.value() + value.size(), &nul, 1); !written.ok())
		{
			return written.error();
		}
		return address.value();
	};

	auto progname_address = writeString(config_.progname.empty() ? "duckemu" : config_.progname);
	if (!progname_address.ok())
	{
		return progname_address.error();
	}

	std::vector<uint64_t> environment;
	environment.reserve(config_.environment.size() + 1);
	for (const std::string &entry : config_.environment)
	{
		auto address = writeString(entry);
		if (!address.ok())
		{
			return address.error();
		}
		environment.push_back(address.value());
	}
	environment.push_back(0);

	auto environ_address = reserve(environment.size() * sizeof(uint64_t), 8);
	if (!environ_address.ok())
	{
		return environ_address.error();
	}
	if (auto written = writeBytes(environ_address.value(), environment.data(),
		    environment.size() * sizeof(uint64_t));
		!written.ok())
	{
		return written.error();
	}

	std::vector<uint64_t> auxv;
	auxv.reserve(20 * 2);
	auto add = [&](uint64_t type, uint64_t value) {
		auxv.push_back(type);
		auxv.push_back(value);
	};
	add(kAtSysinfoEhdr, 0);
	add(kAtHwcap, 0);
	add(kAtPagesz, kGuestPageSize);
	add(kAtClktck, 100);
	add(kAtPhdr, 0);
	add(kAtPhent, 56);
	add(kAtPhnum, 0);
	add(kAtBase, 0);
	add(kAtEntry, 0);
	add(kAtUid, config_.uid);
	add(kAtEuid, config_.uid);
	add(kAtGid, config_.gid);
	add(kAtEgid, config_.gid);
	add(kAtSecure, 0);
	add(kAtRandom, random_address.value());
	add(kAtHwcap2, 0);
	add(kAtExecfn, progname_address.value());
	add(kAtNull, 0);

	auto auxv_address = reserve(auxv.size() * sizeof(uint64_t), 8);
	if (!auxv_address.ok())
	{
		return auxv_address.error();
	}
	if (auto written = writeBytes(auxv_address.value(), auxv.data(),
		    auxv.size() * sizeof(uint64_t));
		!written.ok())
	{
		return written.error();
	}

	auxv_address_ = GuestVA{ auxv_address.value() };
	auxv_size_ = auxv.size() * sizeof(uint64_t);

	if (auto written = writeU64(kAuxvOffset, auxv_address.value()); !written.ok())
	{
		return written;
	}
	if (auto written = writeU64(kTlsGenerationOffset, ~0ULL); !written.ok())
	{
		return written;
	}
	if (auto written = writeU64(kTlsGenerationLibcOffset, 0); !written.ok())
	{
		return written;
	}
	if (auto written = writeU64(kTargetSdkHookOffset, 0); !written.ok())
	{
		return written;
	}
	if (auto written = writeU64(kPrognameOffset, progname_address.value()); !written.ok())
	{
		return written;
	}
	if (auto written = writeU64(kEnvironOffset, environ_address.value()); !written.ok())
	{
		return written;
	}

	setStaticTlsLayout(tls_size_, tls_alignment_, tls_tcb_offset_, tls_bionic_tls_offset_);
	return {};
}

void BionicGlobals::setStaticTlsLayout(uint64_t size,
	uint64_t alignment,
	uint64_t tcb_offset,
	uint64_t bionic_tls_offset)
{
	tls_size_ = size;
	tls_alignment_ = alignment == 0 ? 1 : alignment;
	tls_tcb_offset_ = tcb_offset;
	tls_bionic_tls_offset_ = bionic_tls_offset;

	if (block_.value() == 0)
	{
		return;
	}
	const uint64_t base = block_.value() + kStaticTlsLayoutOffset;
	const uint64_t values[5] = {
		tls_size_,
		tls_alignment_,
		0, // overflowed_
		tls_tcb_offset_,
		tls_bionic_tls_offset_,
	};
	(void)memory_.write(GuestVA{ base }, values, sizeof(values));
}

} // namespace duckemu::runtime::loader
