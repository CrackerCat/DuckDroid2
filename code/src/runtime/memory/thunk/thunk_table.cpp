#include "runtime/memory/thunk/thunk_table.hpp"

#include <utility>

#include "runtime/memory/address_space.hpp"

namespace duckemu::runtime
{
namespace
{

constexpr uint64_t kThunkPageBase = 0x70000000; constexpr uint32_t kThunksPerPage = kGuestPageSize / 8;
// The SVC immediate reserves 15 bits for the index (0x8000 | index).
constexpr uint32_t kMaxThunks = 0x7FFF;
constexpr uint32_t kHostStubImmBase = 0x8000;
constexpr uint32_t kSvcOpcode = 0xD4000001u;
constexpr uint32_t kRetOpcode = 0xD65F03C0u;

} // namespace

ThunkTable::ThunkTable(RuntimeMemory &memory, GuestVA base)
	: memory_(memory)
	, base_(base)
{
}

Result<std::unique_ptr<ThunkTable> > ThunkTable::create(RuntimeMemory &memory)
{
	auto mapped = memory.mapFixed(GuestVA{ kThunkPageBase },
		kGuestPageSize,
		PermRead | PermExec,
		"thunk-page",
		false);
	if (!mapped.ok()) {
		return mapped.error();
	}
	return std::unique_ptr<ThunkTable>(new ThunkTable(memory, mapped.value()));
}

Result<GuestVA> ThunkTable::add(std::string_view name, HostStub stub)
{
	if (count_ >= kMaxThunks) {
		return Error{ ErrorCode::OutOfMemory, "thunk space is full" };
	}
	if (count_ != 0 && count_ % kThunksPerPage == 0)
	{
		// Grow by one page, contiguously: entry address = base + index * 8.
		auto page = memory_.mapFixed(GuestVA{ base_.value() + static_cast<uint64_t>(count_) * 8 },
			kGuestPageSize,
			PermRead | PermExec,
			"thunk-page",
			false);
		if (!page.ok())
		{
			return page.error();
		}
	}

	const uint32_t index = count_;
	const GuestVA entry = GuestVA{ base_.value() + static_cast<uint64_t>(index) * 8 };

	const uint32_t svc = kSvcOpcode | ((kHostStubImmBase | index) << 5);
	const uint32_t words[2] = { svc, kRetOpcode };
	auto written = memory_.write(entry, words, sizeof(words));
	if (!written.ok()) {
		return written.error();
	}

	entries_.push_back(Entry{ std::string(name), std::move(stub) });
	++count_;
	return entry;
}

Result<int64_t> ThunkTable::invoke(uint32_t index, HostCallArgs &args) const
{
	if (index >= count_) {
		return Error{ ErrorCode::NotFound, "unknown thunk index" };
	}
	return entries_[index].stub(args);
}

} // namespace duckemu::runtime
