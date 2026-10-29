// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/vfs/proc_fs.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#include "runtime/memory/address_space.hpp"
#include "runtime/memory/region.hpp"

namespace duckemu::runtime::vfs
{
namespace
{

constexpr std::string_view kProcPrefix = "/proc/";

std::string buildMaps(const ProcContext &context)
{
	std::string text;
	if (context.memory == nullptr)
	{
		return text;
	}

	context.memory->forEachRegion([&](const Region &region) {
		char header[128];
		std::snprintf(header, sizeof(header),
			"%08llx-%08llx %c%c%c%c %08llx %02x:%02x %llu ",
			static_cast<unsigned long long>(region.base.value()),
			static_cast<unsigned long long>(region.end()),
			(region.perms & PermRead) != 0 ? 'r' : '-',
			(region.perms & PermWrite) != 0 ? 'w' : '-',
			(region.perms & PermExec) != 0 ? 'x' : '-',
			'p',
			0ULL, 0, 0, 0ULL);
		text += header;

		std::string name;
		if (region.name.rfind("module:", 0) == 0 && context.resolve_region_name)
		{
			name = context.resolve_region_name(region.name);
		}
		else if (!region.name.empty())
		{
			name = "[anon:" + region.name + "]";
		}
		if (!name.empty())
		{
			text += name;
		}
		text += '\n';
	});
	return text;
}

std::string buildStatus(const ProcContext &context)
{
	const uint64_t threads = context.thread_count ? context.thread_count() : 1;
	char buffer[512];
	std::snprintf(buffer, sizeof(buffer),
		"Name:\t%s\n"
		"State:\tR (running)\n"
		"Tgid:\t%u\n"
		"Pid:\t%u\n"
		"PPid:\t0\n"
		"TracerPid:\t0\n"
		"Uid:\t1000\t1000\t1000\t1000\n"
		"Gid:\t1000\t1000\t1000\t1000\n"
		"FDSize:\t256\n"
		"Threads:\t%llu\n",
		context.progname.c_str(),
		context.pid,
		context.tid,
		static_cast<unsigned long long>(threads));
	return buffer;
}

std::string buildCmdline(const ProcContext &context)
{
	std::string text = context.progname;
	text.push_back('\0');
	return text;
}

std::string buildComm(const ProcContext &context)
{
	std::string text = context.progname;
	if (text.size() > 15)
	{
		text.resize(15);
	}
	text.push_back('\n');
	return text;
}

std::string buildAuxv(const ProcContext &context)
{
	if (!context.auxv || context.memory == nullptr)
	{
		return {};
	}
	auto entry = context.auxv();
	if (!entry.ok() || entry.value().second == 0)
	{
		return {};
	}
	const GuestVA address = entry.value().first;
	const uint64_t size = entry.value().second;
	std::vector<uint8_t> bytes(static_cast<size_t>(size));
	if (!context.memory->read(address, bytes.data(), size).ok())
	{
		return {};
	}
	return std::string(bytes.begin(), bytes.end());
}

bool pidComponentMatches(std::string_view component, const ProcContext &context)
{
	if (component == "self" || component == "thread-self")
	{
		return true;
	}
	uint64_t value = 0;
	for (char character : component)
	{
		if (character < '0' || character > '9')
		{
			return false;
		}
		value = value * 10 + static_cast<uint64_t>(character - '0');
	}
	return value == context.pid;
}

} // namespace

bool isProcPath(std::string_view path)
{
	return path.compare(0, kProcPrefix.size(), kProcPrefix) == 0;
}

std::optional<std::function<std::string()> > makeGenerator(std::string_view path,
	const ProcContext &context)
{
	const std::string_view rest = path.substr(kProcPrefix.size());
	const size_t slash = rest.find('/');
	if (slash == std::string_view::npos)
	{
		return std::nullopt;
	}
	if (!pidComponentMatches(rest.substr(0, slash), context))
	{
		return std::nullopt;
	}
	const std::string_view name = rest.substr(slash + 1);

	if (name == "maps")
	{
		return [context] {
			return buildMaps(context);
		};
	}
	if (name == "status")
	{
		return [context] {
			return buildStatus(context);
		};
	}
	if (name == "auxv")
	{
		return [context] {
			return buildAuxv(context);
		};
	}
	if (name == "cmdline")
	{
		return [context] {
			return buildCmdline(context);
		};
	}
	if (name == "comm")
	{
		return [context] {
			return buildComm(context);
		};
	}
	return std::nullopt;
}

} // namespace duckemu::runtime::vfs
