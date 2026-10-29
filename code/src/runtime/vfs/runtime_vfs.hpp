// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/error/result.hpp"

namespace duckemu::runtime::vfs
{

// Host filesystem view used by the loader. M4 only reads whole files; an APK
// backend arrives with the north-star target (M6).
class RuntimeVfs
{
    public:
	RuntimeVfs() = default;
	explicit RuntimeVfs(std::vector<std::string> search_paths);

	RuntimeVfs(const RuntimeVfs &) = delete;
	RuntimeVfs &operator=(const RuntimeVfs &) = delete;

	void addSearchPath(std::string path);

	// Reads a file at an absolute or relative path.
	[[nodiscard]] Result<std::vector<uint8_t> > readFile(std::string_view path) const;

	// Resolves a DT_NEEDED name: the requesting module's directory first, then
	// the configured search paths, in order. Returns the existing path; the
	// caller reads it (the loader wants parse-once semantics).
	[[nodiscard]] Result<std::string> resolveModule(std::string_view name,
		std::string_view requesting_directory) const;

	[[nodiscard]] const std::vector<std::string> &searchPaths() const
	{
		return search_paths_;
	}

    private:
	std::vector<std::string> search_paths_;
};

} // namespace duckemu::runtime::vfs
