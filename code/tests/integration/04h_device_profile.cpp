// SPDX-License-Identifier: GPL-2.0-only
// Device profile: one struct feeds uname, getuid/getgid, prctl(PR_GET_NAME)
// and the /proc synthesis, so a target observes one consistent device.

#include <cstdio>
#include <string>
#include <string_view>

#include "runtime/device/device_profile.hpp"
#include "runtime/instance/runtime_instance.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

namespace
{

std::string soPath(std::string_view name)
{
	return std::string(DUCKEMU_SO_DIR) + "/" + std::string(name);
}

} // namespace

int main()
{
	RuntimeConfig config;
	config.pid = 1000;
	config.tid = 1;
	config.library_search_paths = { DUCKEMU_SO_DIR };

	// A distinctive profile: every assertion in the fixture is about one of
	// these values reaching the guest.
	config.device.process_name = "testproc";
	config.device.nodename = "testnode";
	config.device.kernel_release = "4.9.327-perf-test";
	config.device.cpu_implementer = "0x51";
	config.device.uid = 10042;
	config.device.gid = 20042;

	auto created = RuntimeInstance::create(config);
	CHECK_RESULT(created);
	if (!created.ok())
	{
		return test::finish("04h_device_profile");
	}
	RuntimeInstance &instance = *created.value();

	auto loaded = instance.loadLibrary(soPath("libdev.so"));
	CHECK_RESULT(loaded);
	if (!loaded.ok())
	{
		return test::finish("04h_device_profile");
	}
	auto entry = instance.lookupSymbol(loaded.value(), "device_checks");
	CHECK_RESULT(entry);
	if (!entry.ok())
	{
		return test::finish("04h_device_profile");
	}

	auto result = instance.callFunction(entry.value());
	CHECK_RESULT(result);
	if (result.ok())
	{
		CHECK_EQ(result.value().return_value, 0u);
		std::printf("  device_checks returned %lld\n",
			static_cast<long long>(result.value().return_value));
	}

	return test::finish("04h_device_profile");
}
