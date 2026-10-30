// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <map>
#include <string>
#include <string_view>

#include "common/error/result.hpp"
#include "runtime/device/device_profile.hpp"

namespace duckemu::runtime::props
{

// The property set a guest observes. Android resolves every property through
// the serialized property area under /dev/__properties__; this table is the
// only input the runtime needs. Identity values are derived from DeviceProfile
// (never duplicated), then an optional build.prop-format file layers over
// them, so a target pack can add anything an app probes without a code change.
class PropertyTable
{
    public:
	// The identity properties every Android process sees, computed from the
	// same profile that feeds uname(2), getuid(2) and /proc synthesis.
	[[nodiscard]] static PropertyTable forDevice(const device::DeviceProfile &device);

	// `name=value` per line. '#' starts a comment; blank lines are ignored.
	// Applies in file order and later assignments win, so a file may restate a
	// derived value. `origin` only appears in error messages.
	Result<void> applyText(std::string_view text, std::string_view origin);
	Result<void> applyFile(const std::string &path);

	void set(std::string name, std::string value);

	[[nodiscard]] const std::map<std::string, std::string> &entries() const
	{
		return entries_;
	}

    private:
	std::map<std::string, std::string> entries_;
};

} // namespace duckemu::runtime::props
