#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace duckemu::runtime::observer
{

enum class MemoryOp : uint8_t
{
	Map = 0,
	Unmap,
	Protect,
	Brk,
};

constexpr std::string_view memoryOpName(MemoryOp op) noexcept
{
	switch (op)
	{
	case MemoryOp::Map:
		return "map";
	case MemoryOp::Unmap:
		return "unmap";
	case MemoryOp::Protect:
		return "protect";
	case MemoryOp::Brk:
		return "brk";
	}
	return "?";
}

// Published by RuntimeMemory after a successful public operation. Internal
// splits and the implicit unmaps of MAP_FIXED do not produce their own events.
struct MemoryEvent
{
	MemoryOp op = MemoryOp::Map;
	uint64_t address = 0;
	uint64_t size = 0;
	uint32_t perms = 0;
	std::string name;
	bool replaced = false;
};

} // namespace duckemu::runtime::observer
