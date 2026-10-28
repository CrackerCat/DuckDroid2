#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common/id/ids.hpp"

namespace duckemu::runtime
{

inline constexpr uint64_t kGuestPageSize = 0x1000;

// Guest anonymous mmap arena. Unicorn does not implement AArch64 TBI, while
// bionic tags scudo heap pointers in the top byte. Every anonymous mapping in
// this arena is mirrored at 1<<56 and 2<<56 aliases pointing at the same host
// memory, so tagged loads/stores land on the real bytes.
inline constexpr uint64_t kAnonymousArenaBase = 0x0000700000000000ULL;
inline constexpr uint64_t kAnonymousArenaSize = 0x0000000400000000ULL; // 16 GiB
inline constexpr uint64_t kAddressTagShift = 56;

[[nodiscard]] constexpr uint64_t untagAddress(uint64_t address) noexcept
{
	return address & ~(0xFFULL << kAddressTagShift);
}

[[nodiscard]] constexpr uint64_t tag0Address(uint64_t address) noexcept
{
	return address;
}

[[nodiscard]] constexpr uint64_t taggedAlias(uint64_t address, uint64_t tag) noexcept
{
	return address | (tag << kAddressTagShift);
}

[[nodiscard]] constexpr bool inAnonymousArena(uint64_t address) noexcept
{
	const uint64_t base = untagAddress(address);
	return base >= kAnonymousArenaBase && base < kAnonymousArenaBase + kAnonymousArenaSize;
}

// Guest permission bits. Numerically identical to Linux PROT_* and Unicorn
// UC_PROT_*; no translation table is needed.
enum GuestPerm : uint32_t
{
	PermNone = 0,
	PermRead = 1,
	PermWrite = 2,
	PermExec = 4,
	PermReadWrite = PermRead | PermWrite,
	PermReadExec = PermRead | PermExec,
	PermAll = PermRead | PermWrite | PermExec,
};

[[nodiscard]] constexpr uint64_t alignDown(uint64_t value, uint64_t alignment)
{
	return value & ~(alignment - 1);
}

[[nodiscard]] constexpr uint64_t alignUp(uint64_t value, uint64_t alignment)
{
	return alignDown(value + alignment - 1, alignment);
}

[[nodiscard]] constexpr uint64_t pageDown(uint64_t value)
{
	return alignDown(value, kGuestPageSize);
}

[[nodiscard]] constexpr uint64_t pageUp(uint64_t value)
{
	return alignUp(value, kGuestPageSize);
}

// Host backing for one or more guest regions. mmap'ed RW and owned by the
// MemoryManager; Unicorn never frees it (RAM_PREALLOC).
struct HostMapping
{
	void *base = nullptr;
	uint64_t size = 0;

	HostMapping() = default;
	HostMapping(const HostMapping &) = delete;
	HostMapping &operator=(const HostMapping &) = delete;
	~HostMapping();
};

// A contiguous guest VA range backed by [host_offset, host_offset + size) of
// HostMapping. Splitting a region only adjusts host_offset/size; the host
// mapping is shared until the last slice is gone.
struct Region
{
	GuestVA base;
	uint64_t size = 0;
	uint32_t perms = PermNone;
	std::shared_ptr<HostMapping> host;
	uint64_t host_offset = 0;
	std::string name;

	[[nodiscard]] uint64_t end() const
	{
		return base.value() + size;
	}

	[[nodiscard]] bool contains(uint64_t address, uint64_t length) const
	{
		if (address < base.value()) {
			return false;
		}
		const uint64_t offset = address - base.value();
		return offset <= size && length <= size - offset;
	}

	[[nodiscard]] uint8_t *hostPtr() const
	{
		return static_cast<uint8_t *>(host->base) + host_offset;
	}

	[[nodiscard]] bool executable() const
	{
		return (perms & PermExec) != 0;
	}
};

} // namespace duckemu::runtime
