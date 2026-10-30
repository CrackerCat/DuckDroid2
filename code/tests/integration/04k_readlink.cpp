// SPDX-License-Identifier: GPL-2.0-only
// readlinkat(78): the /proc/self magic links a target resolves at startup.
// Everything goes through the ROM libc, including errno checks.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/instance/runtime_instance.hpp"
#include "runtime/memory/region.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

namespace
{

constexpr uint32_t kPid = 4242;
constexpr uint32_t kTid = 1;
constexpr int kORdOnly = 0;
constexpr int kEinval = 22;
constexpr int kEnoent = 2;

std::string romLibc()
{
	return std::string(DUCKEMU_ROM_BOOTSTRAP) + "/libc.so";
}

uint64_t callGuest(RuntimeInstance &instance, GuestVA entry,
	const std::array<uint64_t, 8> &args)
{
	auto call = instance.callFunction(entry, args);
	CHECK_RESULT(call);
	return call.ok() ? call.value().return_value : 0;
}

} // namespace

int main()
{
	RuntimeConfig config;
	config.pid = kPid;
	config.tid = kTid;
	config.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };
	// Distinctive so the assertion proves the profile reaches readlink.
	config.device.executable_path = "/system/bin/test_process";

	auto created = RuntimeInstance::create(config);
	CHECK_RESULT(created);
	if (!created.ok())
	{
		return test::finish("04k_readlink");
	}
	RuntimeInstance &instance = *created.value();

	auto libc = instance.loadLibrary(romLibc());
	CHECK_RESULT(libc);
	if (!libc.ok())
	{
		return test::finish("04k_readlink");
	}
	const uint64_t handle = libc.value();

	auto lookup = [&](const char *name) -> GuestVA {
		auto found = instance.lookupSymbol(handle, name);
		CHECK_RESULT(found);
		return found.ok() ? found.value() : GuestVA{};
	};
	const GuestVA readlink_fn = lookup("readlink");
	const GuestVA errno_fn = lookup("__errno");
	const GuestVA open_fn = lookup("open");
	const GuestVA close_fn = lookup("close");

	auto path_page = instance.mapMemory(0x1000, PermReadWrite, "test-link-path");
	auto data_page = instance.mapMemory(0x1000, PermReadWrite, "test-link-data");
	CHECK_RESULT(path_page);
	CHECK_RESULT(data_page);
	if (!path_page.ok() || !data_page.ok())
	{
		return test::finish("04k_readlink");
	}
	const uint64_t path = path_page.value().value();
	const uint64_t data = data_page.value().value();

	auto setPath = [&](const std::string &value) {
		auto written = instance.writeMemory(GuestVA{ path }, value.c_str(), value.size() + 1);
		CHECK_RESULT(written);
	};
	auto guestErrno = [&]() -> int {
		const uint64_t pointer = callGuest(instance, errno_fn, {});
		int value = 0;
		if (pointer != 0)
		{
			CHECK_RESULT(instance.readMemory(GuestVA{ pointer }, &value, sizeof(value)));
		}
		return value;
	};
	auto readLink = [&](const std::string &name, uint64_t size) -> int64_t {
		setPath(name);
		return static_cast<int64_t>(callGuest(instance, readlink_fn, { path, data, size }));
	};
	auto dataString = [&](uint64_t size) {
		std::vector<char> buffer(size + 1, 0);
		CHECK_RESULT(instance.readMemory(GuestVA{ data }, buffer.data(), size));
		return std::string(buffer.data());
	};

	struct Case
	{
		const char *path;
		const char *expected;
	};
	const std::vector<Case> cases = {
		{ "/proc/self/exe", "/system/bin/test_process" },
		{ "/proc/4242/exe", "/system/bin/test_process" },
		{ "/proc/self/cwd", "/" },
		{ "/proc/self/root", "/" },
		{ "/proc/self", "4242" },
	};
	for (const Case &pending : cases)
	{
		const int64_t length = readLink(pending.path, 256);
		const std::string value = dataString(length > 0 ? static_cast<uint64_t>(length) : 0);
		std::printf("  readlink(%s) = %lld \"%s\"\n", pending.path,
			static_cast<long long>(length), value.c_str());
		CHECK_EQ(length, static_cast<int64_t>(std::strlen(pending.expected)));
		CHECK(value == pending.expected);
	}

	// /proc/self/fd/N reports the open description's path.
	{
		setPath("/dev/urandom");
		const int64_t fd = static_cast<int64_t>(callGuest(instance, open_fn,
			{ path, static_cast<uint64_t>(kORdOnly), 0 }));
		CHECK(fd >= 3);
		if (fd >= 3)
		{
			const std::string fd_path = "/proc/self/fd/" + std::to_string(fd);
			const int64_t length = readLink(fd_path, 256);
			const std::string value = dataString(length > 0 ? static_cast<uint64_t>(length) : 0);
			std::printf("  readlink(%s) = \"%s\"\n", fd_path.c_str(), value.c_str());
			CHECK(value == "/dev/urandom");
			callGuest(instance, close_fn, { static_cast<uint64_t>(fd) });

			const int64_t closed_length = readLink(fd_path, 256);
			CHECK_EQ(closed_length, -1);
			CHECK_EQ(guestErrno(), kEnoent);
		}
	}

	// Exists but is not a link -> EINVAL; missing -> ENOENT.
	CHECK_EQ(readLink("/dev/null", 256), -1);
	CHECK_EQ(guestErrno(), kEinval);
	CHECK_EQ(readLink("/no/such/path", 256), -1);
	CHECK_EQ(guestErrno(), kEnoent);
	// Zero-length buffer is EINVAL (Linux since 2.6.39).
	CHECK_EQ(readLink("/proc/self/cwd", 0), -1);
	CHECK_EQ(guestErrno(), kEinval);

	// Truncates silently: five bytes, no terminator.
	const int64_t truncated = readLink("/proc/self/exe", 5);
	CHECK_EQ(truncated, 5);
	CHECK(dataString(5) == "/syst");

	return test::finish("04k_readlink");
}
