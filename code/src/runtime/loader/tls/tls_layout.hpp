#pragma once

#include <cstdint>

#include "common/error/result.hpp"
#include "runtime/memory/region.hpp"
#include "runtime/threads/tls/tls_layout_android13.hpp"

namespace duckemu::runtime::loader
{

// Static TLS slot allocator shared by every module of one instance.
//
// The per-thread block is the M1 arm64 layout: TCB at offset 0, thread pointer
// at +0x10, pthread stub at 0x3200. Module TLS segments are appended from
// 0x4000 (see docs/devlogs/2026-10-28-13-M4.5设计.md §4.1). Offsets are never
// moved once reserved; M4.6 publishes this same layout to the real libc via
// __loader_shared_globals.
class TlsLayout
{
    public:
	static constexpr uint64_t kBlockSize = 0x10000;
	static constexpr uint64_t kModulesBase = 0x4000;
	static constexpr uint64_t kThreadPointerOffset = tls::android13::kThreadPointerOffset;

	// Reserves `size` bytes with `align` and returns the TP-relative offset.
	[[nodiscard]] Result<int64_t> reserve(uint64_t size, uint64_t align)
	{
		if (align == 0)
		{
			align = 1;
		}
		if ((align & (align - 1)) != 0)
		{
			return Error{ ErrorCode::InvalidArgument, "invalid TLS segment alignment" };
		}
		// Bionic honors static TLS alignment up to one page only.
		if (align > 0x1000)
		{
			align = 0x1000;
		}

		const uint64_t block_offset = alignUp(cursor_, align);
		if (block_offset < cursor_ || block_offset < kModulesBase)
		{
			return Error{ ErrorCode::InvalidArgument, "TLS layout overflow" };
		}
		if (block_offset > kBlockSize || size > kBlockSize - block_offset)
		{
			return Error{ ErrorCode::OutOfMemory, "static TLS capacity exceeded" };
		}

		cursor_ = block_offset + size;
		max_alignment_ = align > max_alignment_ ? align : max_alignment_;
		return static_cast<int64_t>(block_offset) - static_cast<int64_t>(kThreadPointerOffset);
	}

	[[nodiscard]] uint64_t size() const
	{
		return cursor_;
	}

	// What libc publishes as StaticTlsLayout::size(): bionic rounds the total
	// up to the layout alignment so that the block start stays aligned (and the
	// thread pointer, +16, with it).
	[[nodiscard]] uint64_t paddedSize() const
	{
		return alignUp(cursor_, max_alignment_);
	}

	[[nodiscard]] uint64_t alignment() const
	{
		return max_alignment_;
	}

    private:
	uint64_t cursor_ = kModulesBase;
	// bionic_tcb is 16-byte aligned, so the layout starts at 16.
	uint64_t max_alignment_ = 16;
};

} // namespace duckemu::runtime::loader
