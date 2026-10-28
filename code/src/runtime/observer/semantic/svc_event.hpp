#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace duckemu::runtime::observer
{

enum class SvcKind : uint8_t
{
	Syscall = 0,
	HostStub,
	UnknownTrap,
};

constexpr std::string_view svcKindName(SvcKind kind) noexcept
{
	switch (kind)
	{
	case SvcKind::Syscall:
		return "sys";
	case SvcKind::HostStub:
		return "host";
	case SvcKind::UnknownTrap:
		return "trap";
	}
	return "?";
}

// One frame of a guest call stack. `name` is "<region>!0x<offset>" when the
// address falls inside a known region, otherwise the bare "0x<address>".
struct GuestFrame
{
	uint64_t pc = 0;
	std::string name;
};

// One completed guest -> kernel dispatch.
//
// `seq` is assigned when the SVC is entered and the event is published when
// the dispatch completes; `id` is assigned at publication. A syscall is
// dispatched exactly once, so it is recorded exactly once (decision
// 2026-10-27-02 §3.1: no replay, no duplicate trace).
struct SvcEvent
{
	// Emission order: assigned right before the event is published. This is
	// the first column of svc.log and stays monotonic and gap-free even when
	// completions arrive out of call order.
	uint64_t id = 0;
	// Call order: assigned when the SVC is entered. Identical to `id` in M2;
	// the two diverge only once M3 lets a syscall complete asynchronously.
	uint64_t seq = 0;
	uint32_t tid = 0;
	// Address of the trapping `svc` instruction (PC - 4 inside the hook).
	uint64_t svc_address = 0;
	SvcKind kind = SvcKind::Syscall;
	// Syscall number, host stub index, or the raw SVC immediate for kind=trap.
	uint32_t number = 0;
	std::string name;
	std::array<uint64_t, 6> args{};
	// Value written back to x0.
	uint64_t result = 0;
	bool supported = true;
	// Filled only for unsupported dispatches (and when a stack could be read).
	std::vector<GuestFrame> stack;
	// Why the frame walk ended; empty for a clean walk or when it never ran.
	std::string stack_stop;
};

} // namespace duckemu::runtime::observer
