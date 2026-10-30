// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/loader/bionic/shared_globals.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <random>
#include <utility>

#include "runtime/memory/address_space.hpp"
#include "runtime/random/random_source.hpp"

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

// libc_shared_globals::tls_modules lives right after StaticTlsLayout:
//   static_tls_layout at 0x450 (5 words, 0x28 bytes) -> tls_modules at 0x478.
// Fields on arm64 LP64 (private/bionic_globals.h): generation,
// generation_libc_so, pthread_rwlock_t (56 bytes), module_count,
// static_module_count, module_table, callbacks. The counts land at 0x4C0/0x4C8
// and the table pointer at 0x4D0, which also puts tls_allocator at 0x4F8 and
// set_target_sdk_version_hook at the already-verified 0x628.
constexpr uint64_t kTlsModuleCountOffset = 0x478 + 0x48;
constexpr uint64_t kTlsStaticModuleCountOffset = 0x478 + 0x50;
constexpr uint64_t kTlsModuleTableOffset = 0x478 + 0x58;

// struct TlsModule (private/bionic_elf_tls.h): TlsSegment {size, alignment,
// init_ptr, init_size}, static_offset, first_generation, soinfo_ptr.
constexpr uint64_t kTlsModuleSize = 56;
constexpr uint64_t kTlsModuleCapacity = 64;
constexpr uint64_t kTlsStaticSlotNone = ~uint64_t{ 0 };
constexpr uint64_t kTlsGenerationFirst = 1;

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

	// AT_RANDOM is also the source of __stack_chk_guard. It reads the instance
	// entropy stream so a seeded replay gets the same canary.
	uint8_t random_bytes[16] = {};
	if (config_.random != nullptr)
	{
		config_.random->fill(random_bytes, sizeof(random_bytes));
	}
	else
	{
		std::random_device device;
		uint64_t seed = (static_cast<uint64_t>(device()) << 32) ^ device();
		std::mt19937_64 generator(seed);
		for (size_t i = 0; i < sizeof(random_bytes); i += 8)
		{
			const uint64_t word = generator();
			std::memcpy(random_bytes + i, &word, sizeof(word));
		}
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
	add(kAtHwcap, config_.hwcap);
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
	add(kAtHwcap2, config_.hwcap2);
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
	// kTlsGenerationFirst, exactly like the linker's TlsModules constructor.
	// libc's own __libc_tls_generation_copy starts at SIZE_MAX and preinit
	// copies this value, so the first __tls_get_addr already takes the slow
	// path until the DTV generation matches.
	if (auto written = writeU64(kTlsGenerationOffset, kTlsGenerationFirst); !written.ok())
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

	// The TlsModule table. libc walks it in __init_static_tls (new threads)
	// and __tls_get_addr (dynamic TLS), so it must be guest-visible from the
	// moment libc's pthread code can run.
	auto module_table = memory_.map(kTlsModuleCapacity * kTlsModuleSize, PermReadWrite, "tls-modules");
	if (!module_table.ok())
	{
		return module_table.error();
	}
	tls_module_table_ = module_table.value();
	tls_module_capacity_ = kTlsModuleCapacity;
	if (auto written = writeU64(kTlsModuleTableOffset, tls_module_table_.value()); !written.ok())
	{
		return written;
	}
	if (auto written = writeU64(kTlsModuleCountOffset, 0); !written.ok())
	{
		return written;
	}
	if (auto written = writeU64(kTlsStaticModuleCountOffset, 0); !written.ok())
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

Result<uint64_t> BionicGlobals::readU64(uint64_t offset) const
{
	uint64_t value = 0;
	auto read = memory_.read(GuestVA{ block_.value() + offset }, &value, sizeof(value));
	if (!read.ok())
	{
		return read.error();
	}
	return value;
}

Result<void> BionicGlobals::bumpTlsGeneration(uint64_t generation)
{
	if (auto written = writeU64(kTlsGenerationOffset, generation); !written.ok())
	{
		return written;
	}
	// preinit stores &__libc_tls_generation_copy here. The linker updates both
	// copies on every registration; without this, __tls_get_addr's fast path
	// would keep comparing against the startup generation.
	auto libc_copy = readU64(kTlsGenerationLibcOffset);
	if (!libc_copy.ok())
	{
		return libc_copy.error();
	}
	if (libc_copy.value() == 0)
	{
		return {};
	}
	return memory_.write(GuestVA{ libc_copy.value() }, &generation, sizeof(generation));
}

Result<uint64_t> BionicGlobals::addTlsModule(uint32_t module_id, const TlsModuleSpec &spec, bool is_static)
{
	if (block_.value() == 0 || tls_module_table_.value() == 0)
	{
		return Error{ ErrorCode::Internal, "bionic globals are not initialized" };
	}
	if (module_id == 0 || module_id > tls_module_capacity_)
	{
		return Error{ ErrorCode::OutOfMemory, "TLS module table is full" };
	}
	// bionic indexes the table with module_id - 1 and requires a dense table.
	if (module_id != tls_module_count_ + 1)
	{
		return Error{ ErrorCode::InvalidArgument, "TLS modules must be registered in id order" };
	}
	if (is_static)
	{
		if (static_tls_finished_)
		{
			return Error{ ErrorCode::Internal, "static TLS is already finalized" };
		}
		if (tls_static_module_count_ != tls_module_count_ || spec.static_offset == kTlsStaticSlotNone)
		{
			return Error{ ErrorCode::InvalidArgument, "static TLS modules must precede dynamic ones" };
		}
	}
	else if (!static_tls_finished_)
	{
		return Error{ ErrorCode::Internal, "dynamic TLS module registered before finalize" };
	}

	const uint64_t generation = ++tls_generation_;
	const uint64_t entry =
		tls_module_table_.value() + static_cast<uint64_t>(module_id - 1) * kTlsModuleSize;
	const uint64_t values[7] = {
		spec.segment_size,
		spec.segment_alignment == 0 ? 1 : spec.segment_alignment,
		spec.init_ptr.value(),
		spec.init_size,
		is_static ? spec.static_offset : kTlsStaticSlotNone,
		generation,
		spec.soinfo,
	};
	if (auto written = memory_.write(GuestVA{ entry }, values, sizeof(values)); !written.ok())
	{
		return written.error();
	}

	tls_module_count_ = module_id;
	if (is_static)
	{
		tls_static_module_count_ = module_id;
	}
	// Static registration happens before preinit, so the libc copy is usually
	// still null here; dynamic registration must update it.
	if (auto written = bumpTlsGeneration(generation); !written.ok())
	{
		abandonTlsModule(module_id);
		return written.error();
	}
	if (auto written = writeU64(kTlsModuleCountOffset, tls_module_count_); !written.ok())
	{
		abandonTlsModule(module_id);
		return written.error();
	}
	if (auto written = writeU64(kTlsStaticModuleCountOffset, tls_static_module_count_); !written.ok())
	{
		abandonTlsModule(module_id);
		return written.error();
	}
	return generation;
}

void BionicGlobals::finalizeStaticTls()
{
	if (static_tls_finished_ || block_.value() == 0 || tls_module_table_.value() == 0)
	{
		return;
	}
	static_tls_finished_ = true;
	// linker_finalize_static_tls(): all modules registered so far are static.
	tls_static_module_count_ = tls_module_count_;
	(void)writeU64(kTlsStaticModuleCountOffset, tls_static_module_count_);
}

void BionicGlobals::abandonTlsModule(uint32_t module_id)
{
	if (block_.value() == 0 || tls_module_table_.value() == 0 || module_id == 0 ||
	    module_id > tls_module_count_)
	{
		return;
	}
	const uint64_t entry =
		tls_module_table_.value() + static_cast<uint64_t>(module_id - 1) * kTlsModuleSize;
	uint64_t static_offset = kTlsStaticSlotNone;
	(void)memory_.read(GuestVA{ entry + 4 * sizeof(uint64_t) }, &static_offset, sizeof(static_offset));

	// Keep static_offset intact so __init_static_tls still treats the slot as
	// part of the static prefix (it skips zero init_size); zero everything
	// else so no image is copied and no dynamic block is allocated.
	const uint64_t values[7] = {
		0, // segment.size
		1, // segment.alignment
		block_.value(), // segment.init_ptr stays non-null (never dereferenced)
		0, // segment.init_size
		static_offset,
		0, // first_generation: kTlsGenerationNone
		0, // soinfo_ptr
	};
	(void)memory_.write(GuestVA{ entry }, values, sizeof(values));
}

} // namespace duckemu::runtime::loader
