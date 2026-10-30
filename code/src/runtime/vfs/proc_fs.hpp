// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "common/error/result.hpp"
#include "common/id/ids.hpp"

namespace duckemu::runtime
{
class RuntimeMemory;
namespace device
{
struct DeviceProfile;
}
}

namespace duckemu::runtime::vfs
{

// What the /proc generators need from the instance. Captured by value into
// each MemoryFile, so the file keeps working after the caller returns.
struct ProcContext
{
	uint32_t pid = 0;
	uint32_t tid = 0;
	std::string progname = "duckemu";
	RuntimeMemory *memory = nullptr;
	std::function<std::string(std::string_view)> resolve_region_name;
	std::function<uint64_t()> thread_count;
	std::function<Result<std::pair<GuestVA, uint64_t> >()> auxv;
	// Device identity; null keeps generic defaults.
	const device::DeviceProfile *device = nullptr;
};

[[nodiscard]] bool isProcPath(std::string_view path);

// A generator for a known /proc/self file, or nullopt when the path is under
// /proc but not emulated (the caller answers ENOENT).
[[nodiscard]] std::optional<std::function<std::string()> > makeGenerator(std::string_view path,
	const ProcContext &context);

} // namespace duckemu::runtime::vfs
