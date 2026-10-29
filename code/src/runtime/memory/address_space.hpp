// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string_view>

#include "common/error/result.hpp"
#include "common/id/ids.hpp"
#include "runtime/memory/code_notifier.hpp"
#include "runtime/memory/region.hpp"

namespace duckemu::runtime
{

namespace observer
{
class EventBus;
}

// Single authority over the guest address space.
//
// M1 is single-threaded; no locking yet. M3 adds the instance-level lock.
// Guest regions are non-overlapping and backed by host mmap memory; the
// execution backend mirrors every change into its engine(s).
class RuntimeMemory
{
    public:
	RuntimeMemory() = default;
	~RuntimeMemory() = default;
	RuntimeMemory(const RuntimeMemory &) = delete;
	RuntimeMemory &operator=(const RuntimeMemory &) = delete;
	RuntimeMemory(RuntimeMemory &&) = delete;
	RuntimeMemory &operator=(RuntimeMemory &&) = delete;

	// Allocate at an arbitrary address (mmap without MAP_FIXED).
	Result<GuestVA> map(uint64_t size, uint32_t perms, std::string_view name);

	// Guest mmap: allocate inside the anonymous arena so the backend can
	// mirror the mapping at the TBI tag aliases. Runtime-internal allocations
	// (stack, TLS, shared globals) keep using map().
	Result<GuestVA> mapAnonymous(uint64_t size, uint32_t perms, std::string_view name);

	// Allocate at an exact address. With replace=true this is MAP_FIXED:
	// overlapping regions are unmapped first.
	Result<GuestVA> mapFixed(GuestVA address, uint64_t size, uint32_t perms, std::string_view name, bool replace);

	Result<void> unmap(GuestVA address, uint64_t size);
	Result<void> protect(GuestVA address, uint64_t size, uint32_t perms);

	// Host-side access. Writes to executable regions notify the backend so
	// translated code is dropped.
	Result<void> write(GuestVA address, const void *data, uint64_t size);
	Result<void> read(GuestVA address, void *data, uint64_t size) const;
	Result<uint32_t> readU32(GuestVA address) const;

	[[nodiscard]] bool isMapped(GuestVA address) const;
	[[nodiscard]] bool isExecutable(GuestVA address) const;
	[[nodiscard]] uint32_t permissionsAt(GuestVA address) const;
	[[nodiscard]] std::optional<Region> regionAt(GuestVA address) const;

	void setMapObserver(MapObserver *observer)
	{
		observer_ = observer;
	}
	void setCodeNotifier(CodeWriteNotifier *notifier)
	{
		notifier_ = notifier;
	}
	// Optional observation sink: every successful public map/unmap/protect/brk
	// is published as a MemoryEvent. Internal splits do not publish.
	void setEventBus(observer::EventBus *bus)
	{
		event_bus_ = bus;
	}

	// Minimal brk(): a fixed 1 MiB heap, mapped lazily.
	Result<GuestVA> brk(GuestVA new_brk);

	// Read-only visit of every current region. The callback must not call back
	// into RuntimeMemory (it runs while the region table is being walked).
	// Used by the backend to replay the address space into a new engine.
	template <typename Function>
	void forEachRegion(Function &&function) const
	{
		for (const auto &entry : regions_)
		{
			function(entry.second);
		}
	}

    private:
	using RegionMap = std::map<uint64_t, Region>;

	[[nodiscard]] RegionMap::const_iterator findRegion(uint64_t address) const;
	RegionMap::iterator findRegion(uint64_t address);

	// Split a region at `address` if it falls strictly inside one.
	void splitAt(uint64_t address);

	// Remove [address, address + size) from the table and tell the backend.
	Result<void> unmapRange(uint64_t address, uint64_t size);
	Result<void> verifyRangeMapped(uint64_t address, uint64_t size) const;

	static Result<std::shared_ptr<HostMapping> > allocateHost(uint64_t size);

	RegionMap regions_;
	uint64_t next_free_ = 0x40000000; MapObserver *observer_ = nullptr;
	CodeWriteNotifier *notifier_ = nullptr;
	observer::EventBus *event_bus_ = nullptr;

	static constexpr uint64_t kHeapBase = 0x30000000; static constexpr uint64_t
		kHeapSize = 0x100000; uint64_t brk_base_ = 0;
	uint64_t brk_current_ = 0;
	bool brk_initialized_ = false;
};

} // namespace duckemu::runtime
