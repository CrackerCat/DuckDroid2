// Worker seam smoke test: the JD target behind worker::make_app_plugin, on the
// same RuntimeConfig the instance host builds (library search paths plus the
// guest file system root that holds the trimmed APK).
//
// This is the plugin path without the supervisor/IPC: init() installs the
// environment, loads libc/libjdg and runs both Bridge.main(103) calls; invoke()
// signs and returns the JSON in result.values["value"].

#include <string>

#include "runtime/instance/runtime_instance.hpp"
#include "support/test_harness.hpp"
#include "worker/app/app_plugin.hpp"

#ifndef DUCKEMU_ROM_BOOTSTRAP
#error "DUCKEMU_ROM_BOOTSTRAP must point at the ROM bootstrap lib directory"
#endif
#ifndef DUCKEMU_ROM_LIB64
#error "DUCKEMU_ROM_LIB64 must point at the ROM system/lib64 directory"
#endif
#ifndef DUCKEMU_JDGS_SO
#error "DUCKEMU_JDGS_SO must point at libjdg.so"
#endif
#ifndef DUCKEMU_JDGS_ROOT
#error "DUCKEMU_JDGS_ROOT must point at the guest file system root for this case"
#endif

using namespace duckemu;

int main()
{
	worker::AppContext context;
	context.app = "jdgs16";
	context.library = DUCKEMU_JDGS_SO;
	context.rom_libc = std::string(DUCKEMU_ROM_BOOTSTRAP) + "/libc.so";
	context.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };
	context.filesystem_roots = { DUCKEMU_JDGS_ROOT };

	runtime::RuntimeConfig config;
	config.library_search_paths = context.library_search_paths;
	config.filesystem_roots = context.filesystem_roots;
	config.default_timeout_us = 60ULL * 1000 * 1000;

	auto created = runtime::RuntimeInstance::create(config);
	CHECK_RESULT(created);
	if (!created.ok())
	{
		std::printf("  create failed: %s\n", created.error().message.c_str());
		return test::finish("07e_jdgs_plugin");
	}
	runtime::RuntimeInstance &instance = *created.value();

	auto plugin = worker::make_app_plugin(context);
	CHECK_RESULT(plugin);
	if (!plugin.ok())
	{
		std::printf("  make_app_plugin failed: %s\n", plugin.error().message.c_str());
		return test::finish("07e_jdgs_plugin");
	}

	std::string error;
	const bool initialized = plugin.value()->init(instance, &error);
	CHECK(initialized);
	if (!initialized)
	{
		std::printf("  plugin init failed: %s\n", error.c_str());
		return test::finish("07e_jdgs_plugin");
	}

	worker::AppRequest request;
	request.op = "sign";
	worker::AppResult result;
	worker::AppError app_error;
	const bool signed_once = plugin.value()->invoke(instance, request, &result, &app_error);
	CHECK(signed_once);
	if (!signed_once)
	{
		std::printf("  invoke failed: %s\n", app_error.message.c_str());
		return test::finish("07e_jdgs_plugin");
	}
	const auto value = result.values.find("value");
	CHECK(value != result.values.end());
	if (value != result.values.end())
	{
		// Deterministic field of the 16.1.50 sample; the rest carries time.
		CHECK(value->second.find("\"b5\":\"503299c6d0b6eedaf589ad49b6d67f12d3386c2f19c6e51bd58e1162c91252a9\"") !=
			std::string::npos);
	}

	// Every invoke must leave the instance usable: the runtime's local frame is
	// per call, so references from the first request must not leak into this one.
	worker::AppResult second;
	const bool signed_twice = plugin.value()->invoke(instance, request, &second, &app_error);
	CHECK(signed_twice);
	if (signed_twice)
	{
		const auto second_value = second.values.find("value");
		CHECK(second_value != second.values.end());
		if (second_value != second.values.end())
		{
			CHECK(second_value->second.find("\"b1\":\"68dd5803-07c3-44d7-8881-0b213bf81b51\"") !=
				std::string::npos);
		}
	}

	return test::finish("07e_jdgs_plugin");
}
