#pragma once

#include <cstdint>

namespace duckemu::runtime::observer
{

enum class DiagnosticKind : uint8_t
{
	MemoryRead = 0,
	MemoryWrite,
};

// High-frequency, lossy observation. Produced on the emulating thread and
// drained by the host; the ring drops when full and counts the drops.
struct DiagnosticEvent
{
	DiagnosticKind kind = DiagnosticKind::MemoryRead;
	uint32_t tid = 0;
	uint32_t size = 0;
	uint64_t pc = 0;
	uint64_t address = 0;
	// Only meaningful for writes of 1..8 bytes.
	uint64_t value = 0;
};

} // namespace duckemu::runtime::observer
