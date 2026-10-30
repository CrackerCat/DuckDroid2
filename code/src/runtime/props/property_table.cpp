// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/props/property_table.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace duckemu::runtime::props
{
namespace
{

std::string_view trim(std::string_view text)
{
	constexpr std::string_view whitespace = " \t\r\n";
	const size_t first = text.find_first_not_of(whitespace);
	if (first == std::string_view::npos)
	{
		return {};
	}
	const size_t last = text.find_last_not_of(whitespace);
	return text.substr(first, last - first + 1);
}

// `brand/product/device:release/id/incremental:type/tags`. ro.build.id has no
// field of its own in DeviceProfile, so it comes from the fingerprint; when
// the shape does not match the caller supplies it in the property file.
std::string buildIdFromFingerprint(const std::string &fingerprint)
{
	const size_t first = fingerprint.find(':');
	if (first == std::string::npos)
	{
		return {};
	}
	const size_t second = fingerprint.find(':', first + 1);
	if (second == std::string::npos)
	{
		return {};
	}
	const std::string_view middle(fingerprint.data() + first + 1, second - first - 1);
	const size_t slash = middle.find('/');
	if (slash == std::string::npos)
	{
		return {};
	}
	const size_t next = middle.find('/', slash + 1);
	if (next == std::string::npos)
	{
		return {};
	}
	return std::string(middle.substr(slash + 1, next - slash - 1));
}

} // namespace

PropertyTable PropertyTable::forDevice(const device::DeviceProfile &device)
{
	PropertyTable table;


	// Product identity. If the fingerprint does not name the same values, the
	// explicit fields are authoritative: everything else the guest sees
	// (uname, /proc, getuid) comes from them too.
	table.set("ro.product.model", device.model);
	table.set("ro.product.brand", device.brand);
	table.set("ro.product.manufacturer", device.manufacturer);
	table.set("ro.product.device", device.device);
	table.set("ro.product.name", device.device);
	table.set("ro.build.fingerprint", device.fingerprint);
	table.set("ro.build.version.release", device.release);
	table.set("ro.build.version.release_or_codename", device.release);
	table.set("ro.build.version.sdk", std::to_string(device.sdk));
	table.set("ro.build.version.incremental", device.incremental);
	table.set("ro.build.type", device.build_type);
	table.set("ro.build.tags", device.debuggable ? "test-keys" : "release-keys");
	table.set("ro.debuggable", device.debuggable ? "1" : "0");
	// Production and userdebug both keep the property service's checks on.
	table.set("ro.secure", "1");
	table.set("ro.allow.mock.location", "0");
	table.set("ro.hardware", device.hardware);
	table.set("ro.serialno", device.serial);
	const std::string build_id = buildIdFromFingerprint(device.fingerprint);
	if (!build_id.empty())
	{
		table.set("ro.build.id", build_id);
	}
	// ABI list, derived from the single ABI field.
	table.set("ro.product.cpu.abi", device.abi);
	if (device.abi == "arm64-v8a")
	{
		table.set("ro.product.cpu.abilist", "arm64-v8a,armeabi-v7a,armeabi");
		table.set("ro.product.cpu.abilist32", "armeabi-v7a,armeabi");
		table.set("ro.product.cpu.abilist64", "arm64-v8a");
	}
	else
	{
		table.set("ro.product.cpu.abilist", device.abi);
	}

	table.set("ro.sf.lcd_density", std::to_string(device.display_density));
	return table;
}

Result<void> PropertyTable::applyText(std::string_view text, std::string_view origin)
{
	size_t start = 0;
	size_t number = 0;
	while (start <= text.size())
	{
		const size_t newline = text.find('\n', start);
		const size_t end = newline == std::string_view::npos ? text.size() : newline;
		const std::string_view line = trim(text.substr(start, end - start));
		++number;
		if (!line.empty() && line.front() != '#')
		{
			const size_t equals = line.find('=');
			if (equals == std::string_view::npos)
			{
				return Error{ ErrorCode::InvalidArgument,
					std::string(origin) + ":" + std::to_string(number) +
						": expected name=value" };
			}
			const std::string_view name = trim(line.substr(0, equals));
			const std::string_view value = trim(line.substr(equals + 1));
			if (name.empty())
			{
				return Error{ ErrorCode::InvalidArgument,
					std::string(origin) + ":" + std::to_string(number) +
						": empty property name" };
			}
			if (name.find('\0') != std::string_view::npos ||
				value.find('\0') != std::string_view::npos)
			{
				return Error{ ErrorCode::InvalidArgument,
					std::string(origin) + ":" + std::to_string(number) +
						": property contains NUL" };
			}
			set(std::string(name), std::string(value));
		}
		if (newline == std::string_view::npos)
		{
			break;
		}
		start = newline + 1;
	}
	return {};
}

Result<void> PropertyTable::applyFile(const std::string &path)
{
	std::ifstream file(path);
	if (!file)
	{
		return Error{ ErrorCode::NotFound, "cannot read property file " + path };
	}
	std::ostringstream buffer;
	buffer << file.rdbuf();
	if (file.bad())
	{
		return Error{ ErrorCode::NotFound, "cannot read property file " + path };
	}
	return applyText(buffer.str(), path);
}

void PropertyTable::set(std::string name, std::string value)
{
	entries_[std::move(name)] = std::move(value);
}

} // namespace duckemu::runtime::props
