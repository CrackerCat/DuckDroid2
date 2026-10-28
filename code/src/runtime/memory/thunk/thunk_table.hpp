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
