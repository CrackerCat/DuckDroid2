// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/vfs/proc_fs.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#include "runtime/device/device_profile.hpp"
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
	const device::DeviceProfile *profile = context.device;
	const uint32_t uid = profile != nullptr ? profile->uid : 1000;
	const uint32_t gid = profile != nullptr ? profile->gid : 1000;
	const std::string &name = profile != nullptr ? profile->process_name : context.progname;
	const uint64_t threads = context.thread_count ? context.thread_count() : 1;
	char buffer[2048];
	std::snprintf(buffer,
		sizeof(buffer),
		"Name:\t%s\n"
		"Umask:\t0077\n"
		"State:\tR (running)\n"
		"Tgid:\t%u\n"
		"Pid:\t%u\n"
		"PPid:\t0\n"
		"TracerPid:\t0\n"
		"Uid:\t%u\t%u\t%u\t%u\n"
		"Gid:\t%u\t%u\t%u\t%u\n"
		"FDSize:\t256\n"
		"Groups:\t%u\t%u\n"
		"VmPeak:\t 3400000 kB\n"
		"VmSize:\t 3358000 kB\n"
		"VmRSS:\t   68000 kB\n"
		"VmData:\t  210000 kB\n"
		"VmStk:\t     132 kB\n"
		"VmExe:\t     180 kB\n"
		"VmLib:\t   80000 kB\n"
		"Threads:\t%llu\n"
		"SigQ:\t0/16000\n"
		"SigPnd:\t0000000000000000\n"
		"CapInh:\t0000000000000000\n"
		"CapPrm:\t0000000000000000\n"
		"CapEff:\t0000000000000000\n"
		"CapBnd:\t0000000000000000\n"
		"Seccomp:\t2\n"
		"Seccomp_filters:\t1\n",
		name.c_str(),
		context.pid,
		context.pid,
		uid,
		uid,
		uid,
		uid,
		gid,
		gid,
		gid,
		gid,
		gid,
		gid,
		static_cast<unsigned long long>(threads));
	return buffer;
}

std::string buildVersion(const ProcContext &context)
{
	const device::DeviceProfile *profile = context.device;
	const std::string release = profile != nullptr ? profile->kernel_release : "6.1.0-duckemu";
	const std::string version = profile != nullptr ? profile->kernel_version : "#1 SMP";
	return "Linux version " + release + " (android-build@localhost) " + version + "\n";
}

std::string buildCpuinfo(const ProcContext &context)
{
	const device::DeviceProfile *profile = context.device;
	const std::string implementer = profile != nullptr ? profile->cpu_implementer : "0x41";
	const std::string architecture = profile != nullptr ? profile->cpu_architecture : "8";
	const std::string part = profile != nullptr ? profile->cpu_part : "0xd0c";
	const uint32_t revision = profile != nullptr ? profile->cpu_revision : 1;
	const uint32_t count = profile != nullptr ? profile->cpu_count : 8;
	const std::string features = profile != nullptr ? profile->cpu_features :
								 "fp asimd evtstrm aes pmull sha1 sha2 crc32";

	std::string text;
	char buffer[512];
	for (uint32_t core = 0; core < count; ++core)
	{
		std::snprintf(buffer,
			sizeof(buffer),
			"processor\t: %u\n"
			"BogoMIPS\t: 38.40\n"
			"Features\t: %s\n"
			"CPU implementer\t: %s\n"
			"CPU architecture: %s\n"
			"CPU variant\t: 0x7\n"
			"CPU part\t: %s\n"
			"CPU revision\t: %u\n"
			"\n",
			core,
			features.c_str(),
			implementer.c_str(),
			architecture.c_str(),
			part.c_str(),
			revision);
		text += buffer;
	}
	return text;
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
		// Process-independent /proc entries.
		if (rest == "version")
		{
			return [context] {
				return buildVersion(context);
			};
		}
		if (rest == "cpuinfo")
		{
			return [context] {
				return buildCpuinfo(context);
			};
		}
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
	if (name == "mem")
	{
		// The process can open its own mem on device. The emulator has no
		// ptrace-style byte-wise view, so the file is readable and empty.
		return [] {
			return std::string();
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
	if (name == "version")
	{
		return [context] {
			return buildVersion(context);
		};
	}
	if (name == "cpuinfo")
	{
		return [context] {
			return buildCpuinfo(context);
		};
	}
	return std::nullopt;
}

} // namespace duckemu::runtime::vfs
