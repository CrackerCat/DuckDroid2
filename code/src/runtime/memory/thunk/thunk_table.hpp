// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/error/result.hpp"
#include "common/id/ids.hpp"

namespace duckemu::runtime
{

class RuntimeMemory;

// Arguments as passed in x0..x7. The stub returns the value for x0.
struct HostCallArgs
{
	std::array<uint64_t, 8> x{};

	// Filled by the SVC dispatcher for stubs that must call back into guest
	// code (pthread_once). Empty when a stub is invoked outside an SVC trap.
	// Nested calls save/restore the engine context (M0 probe 6) and are capped
	// at 32 frames. M5 reuses this path for Call*Method.
	std::function<Result<int64_t>(GuestVA entry, const std::array<uint64_t, 8> &args)> call_guest;
	// TPIDR_EL0 of the thread that made the call. Lets __errno and friends find
	// the calling thread's bionic TLS, and the JNI layer find its per-thread
	// JNIEnv slot.
	uint64_t thread_pointer = 0;
	// Guest SP at the trap. Variadic JNI calls pass arguments beyond x0..x7 on
	// the stack; M5 does not read them yet, but the value is captured so the
	// ABI adapter has a single place to grow.
	uint64_t sp = 0;
	uint32_t tid = 0;
};

using HostStub = std::function<int64_t(HostCallArgs &)>;

// One read-only executable guest page. Each entry is
//   svc #(0x8000 | index)
//   ret
// The SVC trap dispatches to the registered host stub; the `ret` then returns
// to the guest caller's LR. No argument marshalling: AAPCS64 already put the
// arguments in x0..x7.
class ThunkTable
{
    public:
	static Result<std::unique_ptr<ThunkTable> > create(RuntimeMemory &memory);

	Result<GuestVA> add(std::string_view name, HostStub stub);
	Result<int64_t> invoke(uint32_t index, HostCallArgs &args) const;

	// Empty when the index was never registered.
	[[nodiscard]] std::string_view nameOf(uint32_t index) const
	{
		return index < count_ ? std::string_view(entries_[index].name) : std::string_view{};
	}

	[[nodiscard]] uint32_t count() const
	{
		return count_;
	}
	[[nodiscard]] GuestVA base() const
	{
		return base_;
	}

    private:
	struct Entry
	{
		std::string name;
		HostStub stub;
	};

	ThunkTable(RuntimeMemory &memory, GuestVA base);

	RuntimeMemory &memory_;
	GuestVA base_;
	uint32_t count_ = 0;
	std::vector<Entry> entries_;
};

} // namespace duckemu::runtime
