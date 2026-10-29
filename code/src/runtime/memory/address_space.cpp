// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/memory/address_space.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include <sys/mman.h>

#include "runtime/observer/event_bus.hpp"

namespace duckemu::runtime
{

HostMapping::~HostMapping()
{
	if (base != nullptr && size != 0) {
		::munmap(base, static_cast<size_t>(size));
	}
}

namespace
{

void publishMemory(observer::EventBus *bus, const observer::MemoryEvent &event)
{
	if (bus != nullptr) {
		bus->publishMemory(event);
	}
}

Error invalidArgument(std::string message)
{
	return Error{ ErrorCode::InvalidArgument, std::move(message) };
}

Error unmapped(std::string message)
{
	return Error{ ErrorCode::Unmapped, std::move(message) };
}

} // namespace

Result<std::shared_ptr<HostMapping> > RuntimeMemory::allocateHost(uint64_t size)
{
	void *memory = ::mmap(nullptr,
		static_cast<size_t>(size),
		PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS,
		-1,
		0);
	if (memory == MAP_FAILED) {
		return Error{ ErrorCode::OutOfMemory, "host mmap failed" };
	}
	auto mapping = std::make_shared<HostMapping>();
	mapping->base = memory;
	mapping->size = size;
	return mapping;
}

RuntimeMemory::RegionMap::const_iterator RuntimeMemory::findRegion(uint64_t address) const
{
	address = untagAddress(address);
	auto it = regions_.upper_bound(address);
	if (it == regions_.begin()) {
		return regions_.end();
	}
	--it;
	return address < it->second.end() ? it : regions_.end();
}

RuntimeMemory::RegionMap::iterator RuntimeMemory::findRegion(uint64_t address)
{
	address = untagAddress(address);
	auto it = regions_.upper_bound(address);
	if (it == regions_.begin()) {
		return regions_.end();
	}
	--it;
	return address < it->second.end() ? it : regions_.end();
}

Result<GuestVA> RuntimeMemory::map(uint64_t size, uint32_t perms, std::string_view name)
{
	if (size == 0) {
		return invalidArgument("mmap size is zero");
	}
	size = pageUp(size);
	if (size > std::numeric_limits<uint64_t>::max() - next_free_) {
		return Error{ ErrorCode::OutOfMemory, "guest address space exhausted" };
	}

	uint64_t base = pageUp(next_free_);
	bool moved = true;
	while (moved) {
		moved = false;
		for (const auto &[key, region] : regions_) {
			(void)key;
			if (base < region.end() && base + size > region.base.value()) {
				base = pageUp(region.end());
				moved = true;
			}
		}
	}
	if (base > std::numeric_limits<uint64_t>::max() - size) {
		return Error{ ErrorCode::OutOfMemory, "guest address space exhausted" };
	}

	auto host = allocateHost(size);
	if (!host.ok()) {
		return host.error();
	}

	Region region;
	region.base = GuestVA{ base };
	region.size = size;
	region.perms = perms;
	region.host = std::move(host).value();
	region.name = std::string(name);

	if (observer_ != nullptr) {
		auto mirrored = observer_->onMapped(region);
		if (!mirrored.ok()) {
			return mirrored.error();
		}
	}

	regions_.emplace(base, region);
	next_free_ = base + size;
	publishMemory(event_bus_,
		observer::MemoryEvent{ observer::MemoryOp::Map, region.base.value(), region.size, region.perms, region.name, false });
	return region.base;
}

Result<GuestVA> RuntimeMemory::mapAnonymous(uint64_t size, uint32_t perms, std::string_view name)
{
	if (size == 0) {
		return invalidArgument("mmap size is zero");
	}
	size = pageUp(size);
	const uint64_t arena_end = kAnonymousArenaBase + kAnonymousArenaSize;
	if (size > kAnonymousArenaSize) {
		return Error{ ErrorCode::OutOfMemory, "anonymous mmap exceeds the arena" };
	}

	uint64_t base = kAnonymousArenaBase;
	bool moved = true;
	while (moved) {
		moved = false;
		for (const auto &[key, region] : regions_) {
			(void)key;
			if (base < region.end() && base + size > region.base.value()) {
				base = pageUp(region.end());
				moved = true;
			}
		}
	}
	if (base < kAnonymousArenaBase || base > arena_end || size > arena_end - base) {
		return Error{ ErrorCode::OutOfMemory, "anonymous arena is exhausted" };
	}

	auto host = allocateHost(size);
	if (!host.ok()) {
		return host.error();
	}

	Region region;
	region.base = GuestVA{ base };
	region.size = size;
	region.perms = perms;
	region.host = std::move(host).value();
	region.name = std::string(name);

	if (observer_ != nullptr) {
		auto mirrored = observer_->onMapped(region);
		if (!mirrored.ok()) {
			return mirrored.error();
		}
	}

	regions_.emplace(base, region);
	publishMemory(event_bus_,
		observer::MemoryEvent{ observer::MemoryOp::Map, region.base.value(), region.size, region.perms, region.name, false });
	return region.base;
}

Result<GuestVA>
RuntimeMemory::mapFixed(GuestVA address, uint64_t size, uint32_t perms, std::string_view name, bool replace)
{
	if (size == 0 || address.value() % kGuestPageSize != 0) {
		return invalidArgument("MAP_FIXED address must be page-aligned");
	}
	size = pageUp(size);
	if (address.value() > std::numeric_limits<uint64_t>::max() - size) {
		return invalidArgument("MAP_FIXED range overflows");
	}

	const uint64_t base = address.value();
	const uint64_t end = base + size;

	bool overlapping = false;
	for (const auto &[key, region] : regions_) {
		(void)key;
		if (region.base.value() < end && base < region.end()) {
			overlapping = true;
			break;
		}
	}
	if (overlapping) {
		if (!replace) {
			return Error{ ErrorCode::AlreadyExists, "MAP_FIXED range is occupied" };
		}
		auto removed = unmapRange(base, size);
		if (!removed.ok()) {
			return removed.error();
		}
	}

	auto host = allocateHost(size);
	if (!host.ok()) {
		return host.error();
	}

	Region region;
	region.base = address;
	region.size = size;
	region.perms = perms;
	region.host = std::move(host).value();
	region.name = std::string(name);

	if (observer_ != nullptr) {
		auto mirrored = observer_->onMapped(region);
		if (!mirrored.ok()) {
			return mirrored.error();
		}
	}

	regions_.emplace(base, region);
	publishMemory(event_bus_,
		observer::MemoryEvent{ observer::MemoryOp::Map, region.base.value(), region.size, region.perms, region.name, replace });
	return region.base;
}

void RuntimeMemory::splitAt(uint64_t address)
{
	auto it = findRegion(address);
	if (it == regions_.end()) {
		return;
	}
	Region &left = it->second;
	if (address <= left.base.value() || address >= left.end()) {
		return;
	}

	const uint64_t left_size = address - left.base.value();
	Region right = left;
	right.base = GuestVA{ address };
	right.size = left.size - left_size;
	right.host_offset = left.host_offset + left_size;
	left.size = left_size;
	regions_.emplace(address, std::move(right));
}

Result<void> RuntimeMemory::unmapRange(uint64_t address, uint64_t size)
{
	const uint64_t end = address + size;
	for (auto it = regions_.begin(); it != regions_.end();) {
		const Region original = it->second;
		const uint64_t region_start = original.base.value();
		const uint64_t region_end = original.end();

		if (region_end <= address || region_start >= end) {
			++it;
			continue;
		}

		const uint64_t overlap_start = std::max(region_start, address);
		const uint64_t overlap_end = std::min(region_end, end);

		it = regions_.erase(it);

		if (region_start < overlap_start) {
			Region left = original;
			left.size = overlap_start - region_start;
			regions_.emplace(left.base.value(), std::move(left));
		}
		if (overlap_end < region_end) {
			Region right = original;
			right.base = GuestVA{ overlap_end };
			right.size = region_end - overlap_end;
			right.host_offset = original.host_offset + (overlap_end - region_start);
			regions_.emplace(right.base.value(), std::move(right));
		}

		if (observer_ != nullptr) {
			auto mirrored = observer_->onUnmapped(GuestVA{ overlap_start }, overlap_end - overlap_start);
			if (!mirrored.ok()) {
				return mirrored.error();
			}
		}
	}
	return {};
}

Result<void> RuntimeMemory::unmap(GuestVA address, uint64_t size)
{
	if (address.value() % kGuestPageSize != 0) {
		return invalidArgument("munmap address must be page-aligned");
	}
	if (size == 0) {
		return invalidArgument("munmap size is zero");
	}
	size = pageUp(size);
	if (address.value() > std::numeric_limits<uint64_t>::max() - size) {
		return invalidArgument("munmap range overflows");
	}
	auto result = unmapRange(address.value(), size);
	if (result.ok()) {
		publishMemory(event_bus_, observer::MemoryEvent{ observer::MemoryOp::Unmap, address.value(), size, 0, {}, false });
	}
	return result;
}

Result<void> RuntimeMemory::verifyRangeMapped(uint64_t address, uint64_t size) const
{
	const uint64_t end = address + size;
	uint64_t cursor = address;
	auto it = regions_.upper_bound(address);
	if (it != regions_.begin()) {
		--it;
	}
	while (cursor < end) {
		if (it == regions_.end() || cursor < it->second.base.value() || cursor >= it->second.end()) {
			return unmapped("range is not fully mapped");
		}
		cursor = std::min(end, it->second.end());
		++it;
	}
	return {};
}

Result<void> RuntimeMemory::protect(GuestVA address, uint64_t size, uint32_t perms)
{
	if (address.value() % kGuestPageSize != 0) {
		return invalidArgument("mprotect address must be page-aligned");
	}
	if (size == 0) {
		return invalidArgument("mprotect size is zero");
	}
	size = pageUp(size);
	if (address.value() > std::numeric_limits<uint64_t>::max() - size) {
		return invalidArgument("mprotect range overflows");
	}

	auto covered = verifyRangeMapped(address.value(), size);
	if (!covered.ok()) {
		return covered.error();
	}

	const uint64_t base = address.value();
	const uint64_t end = base + size;
	splitAt(base);
	splitAt(end);

	for (auto &[key, region] : regions_) {
		(void)key;
		if (region.base.value() >= base && region.end() <= end) {
			region.perms = perms;
		}
	}

	if (observer_ != nullptr) {
		auto mirrored = observer_->onProtected(address, size, perms);
		if (!mirrored.ok()) {
			return mirrored.error();
		}
	}
	publishMemory(event_bus_, observer::MemoryEvent{ observer::MemoryOp::Protect, address.value(), size, perms, {}, false });
	return {};
}

Result<void> RuntimeMemory::write(GuestVA address, const void *data, uint64_t size)
{
	if (size == 0) {
		return {};
	}
	address = GuestVA{ untagAddress(address.value()) };
	const uint64_t end = address.value() + size;
	if (end < address.value()) {
		return invalidArgument("write range overflows");
	}

	uint64_t cursor = address.value();
	const auto *input = static_cast<const uint8_t *>(data);
	auto it = regions_.upper_bound(cursor);
	if (it != regions_.begin()) {
		--it;
	}

	while (cursor < end) {
		if (it == regions_.end() || cursor < it->second.base.value() || cursor >= it->second.end()) {
			return unmapped("write crosses an unmapped range");
		}
		Region &region = it->second;
		const uint64_t chunk = std::min(end, region.end()) - cursor;
		std::memcpy(region.hostPtr() + (cursor - region.base.value()), input, static_cast<size_t>(chunk));
		if (region.executable() && notifier_ != nullptr) {
			notifier_->onCodeWritten(GuestVA{ cursor }, chunk);
		}
		input += chunk;
		cursor += chunk;
		++it;
	}
	return {};
}

Result<void> RuntimeMemory::read(GuestVA address, void *data, uint64_t size) const
{
	if (size == 0) {
		return {};
	}
	address = GuestVA{ untagAddress(address.value()) };
	const uint64_t end = address.value() + size;
	if (end < address.value()) {
		return invalidArgument("read range overflows");
	}

	uint64_t cursor = address.value();
	auto *output = static_cast<uint8_t *>(data);
	auto it = regions_.upper_bound(cursor);
	if (it != regions_.begin()) {
		--it;
	}

	while (cursor < end) {
		if (it == regions_.end() || cursor < it->second.base.value() || cursor >= it->second.end()) {
			return unmapped("read crosses an unmapped range");
		}
		const Region &region = it->second;
		const uint64_t chunk = std::min(end, region.end()) - cursor;
		std::memcpy(output, region.hostPtr() + (cursor - region.base.value()), static_cast<size_t>(chunk));
		output += chunk;
		cursor += chunk;
		++it;
	}
	return {};
}

Result<uint32_t> RuntimeMemory::readU32(GuestVA address) const
{
	uint32_t value = 0;
	auto result = read(address, &value, sizeof(value));
	if (!result.ok()) {
		return result.error();
	}
	return value;
}

bool RuntimeMemory::isMapped(GuestVA address) const
{
	return findRegion(address.value()) != regions_.end();
}

uint32_t RuntimeMemory::permissionsAt(GuestVA address) const
{
	auto it = findRegion(address.value());
	return it == regions_.end() ? PermNone : it->second.perms;
}

bool RuntimeMemory::isExecutable(GuestVA address) const
{
	return (permissionsAt(address) & PermExec) != 0;
}

std::optional<Region> RuntimeMemory::regionAt(GuestVA address) const
{
	auto it = findRegion(address.value());
	if (it == regions_.end()) {
		return std::nullopt;
	}
	return it->second;
}

Result<GuestVA> RuntimeMemory::brk(GuestVA new_brk)
{
	if (!brk_initialized_) {
		auto mapped = mapFixed(GuestVA{ kHeapBase }, kHeapSize, PermReadWrite, "heap", false);
		if (!mapped.ok()) {
			return mapped.error();
		}
		brk_base_ = kHeapBase;
		brk_current_ = kHeapBase;
		brk_initialized_ = true;
	}

	if (new_brk.value() == 0) {
		return GuestVA{ brk_current_ };
	}
	if (new_brk.value() < brk_base_ || new_brk.value() > brk_base_ + kHeapSize) {
		// Linux returns the old break on failure; we do the same and do not
		// report ENOMEM here (the caller compares the result).
		return GuestVA{ brk_current_ };
	}
	brk_current_ = new_brk.value();
	publishMemory(event_bus_, observer::MemoryEvent{ observer::MemoryOp::Brk, brk_current_, 0, 0, {}, false });
	return GuestVA{ brk_current_ };
}

} // namespace duckemu::runtime
