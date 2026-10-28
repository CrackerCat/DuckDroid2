#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "runtime/observer/semantic/svc_event.hpp"

namespace duckemu::runtime
{
class RuntimeMemory;
}

namespace duckemu::runtime::observer
{

inline constexpr uint32_t kMaxGuestFrames = 32;

struct GuestStack
{
	std::vector<GuestFrame> frames;
	bool truncated = false;
	// Why the walk ended: "fp=0" (clean) or the failed check.
	std::string stop;
};

// Walks the x29 frame chain. `pc` (the faulting SVC) becomes frame 0; `fp` is
// X29 at the faulting site and `sp` locates the stack region the walk must stay
// inside. This is a frame-pointer unwinder, not CFI: guest code built without
// frame records yields a short chain, and every failed check stops the walk
// with the reason recorded in `stop`.
[[nodiscard]] GuestStack unwindGuestStack(const RuntimeMemory &memory, uint64_t pc, uint64_t fp, uint64_t sp);

// "<region>!0x<offset>" when the address is inside a region, else "0x<addr>".
[[nodiscard]] std::string describeGuestAddress(const RuntimeMemory &memory, uint64_t pc);

} // namespace duckemu::runtime::observer
