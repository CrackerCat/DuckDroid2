#include "runtime/observer/diagnostic/guest_stack.hpp"

#include <cstdio>

#include "runtime/memory/address_space.hpp"

namespace duckemu::runtime::observer
{

std::string describeGuestAddress(const RuntimeMemory &memory, uint64_t pc)
{
	char buffer[96];
	const auto region = memory.regionAt(GuestVA{ pc });
	if (region.has_value())
	{
		std::snprintf(buffer,
			sizeof(buffer),
			"%s!0x%llx",
			region->name.c_str(),
			static_cast<unsigned long long>(pc - region->base.value()));
		return buffer;
	}
	std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(pc));
	return buffer;
}

GuestStack unwindGuestStack(const RuntimeMemory &memory, uint64_t pc, uint64_t fp, uint64_t sp)
{
	GuestStack stack;
	stack.frames.push_back(GuestFrame{ pc, describeGuestAddress(memory, pc) });

	if (fp == 0)
	{
		stack.stop = "fp=0";
		return stack;
	}

	const auto stack_region = memory.regionAt(GuestVA{ sp });
	if (!stack_region.has_value())
	{
		stack.truncated = true;
		stack.stop = "sp unmapped";
		return stack;
	}

	uint64_t frame = fp;
	for (uint32_t depth = 1; depth < kMaxGuestFrames; ++depth)
	{
		if (frame == 0)
		{
			stack.stop = "fp=0";
			return stack;
		}
		if ((frame & 0xF) != 0)
		{
			stack.truncated = true;
			stack.stop = "unaligned fp";
			return stack;
		}
		if (!stack_region->contains(frame, 2 * sizeof(uint64_t)))
		{
			stack.truncated = true;
			stack.stop = "fp outside stack";
			return stack;
		}

		uint64_t record[2] = {};
		const auto read = memory.read(GuestVA{ frame }, record, sizeof(record));
		if (!read.ok())
		{
			stack.truncated = true;
			stack.stop = "unmapped frame";
			return stack;
		}
		if (record[1] == 0)
		{
			stack.truncated = true;
			stack.stop = "lr=0";
			return stack;
		}

		stack.frames.push_back(GuestFrame{ record[1], describeGuestAddress(memory, record[1]) });
		frame = record[0];
	}

	stack.truncated = true;
	stack.stop = "frame limit";
	return stack;
}

} // namespace duckemu::runtime::observer
