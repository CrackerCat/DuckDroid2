// SPDX-License-Identifier: GPL-2.0-only
// jdgs_case: JD 16.1.50 libjdg.so Bridge.main.
//
//   ./duckemu_jdgs_case              # initialize, then sign once
//   JDGS_REPEAT=10 ./duckemu_jdgs_case
//   ./duckemu_jdgs_case --trace DIR
//
// The Java side (Bridge, the package/signature object graph and the
// Integer-based status) is jdgs_env.cpp; main installs it before loading
// anything. The target .so and the trimmed guest APK live in assets/.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>

#include "jdgs_case/jdgs_env.hpp"
#include "runtime/instance/runtime_instance.hpp"
#include "runtime/observer/semantic/jni_event.hpp"

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
using namespace duckemu::runtime;
using namespace duckemu::user;

namespace
{

constexpr uint64_t kSelectorInit = 103;
constexpr uint64_t kSelectorSign = 101;
// 16.1.50's JD setup is slower than the runtime's 5 s default; the reference
// runner also allowed 60 s per call.
constexpr uint64_t kCallTimeoutUs = 60ULL * 1000 * 1000;

std::string romLibc()
{
	return std::string(DUCKEMU_ROM_BOOTSTRAP) + "/libc.so";
}

bool callMain(RuntimeInstance &instance,
	uint64_t selector,
	uint64_t argument,
	uint64_t *result,
	std::string *error)
{
	const std::array<uint64_t, 8> arguments = { selector, argument };
	auto called = instance.callRegisteredNative(
		jdgs::kBridgeClass, "main", jdgs::kMainSignature, arguments);
	if (!called.ok())
	{
		*error = called.error().message;
		return false;
	}
	*result = called.value();
	return true;
}

// main(103) answers an Integer error code (or null on success). Anything that
// is not an Integer counts as success, matching the app-side status check.
bool reportInit(RuntimeInstance &instance, uint64_t handle, const char *phase, std::string *error)
{
	if (handle == 0)
	{
		return true;
	}
	auto object = instance.jniResolveHandle(handle);
	if (!object.ok())
	{
		*error = object.error().message;
		return false;
	}
	auto element = jdgs::readResultElement(object.value());
	if (!element.ok() || element.value() == 0)
	{
		return true;
	}
	auto descriptor = jdgs::classNameOf(element.value());
	if (!descriptor.ok() || descriptor.value() != "java/lang/Integer")
	{
		return true;
	}
	auto status = jdgs::readIntegerField(element.value());
	if (!status.ok())
	{
		return true;
	}
	if (std::getenv("JDGS_DEBUG") != nullptr)
	{
		std::fprintf(stderr, "  init(%s) status: %d\n", phase, status.value());
	}
	if (status.value() != 0)
	{
		*error = std::string("Bridge.main(103, \"") + phase + "\") returned " +
			std::to_string(status.value());
		return false;
	}
	return true;
}

bool sign(RuntimeInstance &instance,
	const jdgs::SignParams &params,
	std::string *signature,
	std::string *error)
{
	auto arguments = jdgs::buildSignArguments(params);
	if (!arguments.ok())
	{
		*error = "cannot build the argument array: " + arguments.error().message;
		return false;
	}
	uint64_t result = 0;
	if (!callMain(instance, kSelectorSign, arguments.value(), &result, error))
	{
		return false;
	}
	if (result == 0)
	{
		*error = "Bridge.main(101) returned null";
		return false;
	}
	auto object = instance.jniResolveHandle(result);
	if (!object.ok())
	{
		*error = "cannot resolve the result: " + object.error().message;
		return false;
	}
	auto element = jdgs::readResultElement(object.value());
	if (!element.ok())
	{
		*error = "cannot read the result array: " + element.error().message;
		return false;
	}
	if (element.value() == 0)
	{
		*error = "Bridge.main(101) returned a null element";
		return false;
	}
	auto descriptor = jdgs::classNameOf(element.value());
	if (descriptor.ok() && descriptor.value() == "java/lang/Integer")
	{
		auto status = jdgs::readIntegerField(element.value());
		*error = status.ok() ? "Bridge.main(101) returned errno " +
					   std::to_string(status.value())
				     : "Bridge.main(101) returned an Integer result";
		return false;
	}
	auto text = jdgs::readResultString(object.value());
	if (!text.ok())
	{
		*error = "cannot read the result string: " + text.error().message;
		return false;
	}
	*signature = std::move(text.value());
	return true;
}

} // namespace

int main(int argc, char **argv)
{
	std::string trace_path;
	std::optional<uint64_t> wall_time_ns;
	std::optional<uint64_t> monotonic_time_ns;
	for (int index = 1; index < argc; ++index)
	{
		const std::string argument = argv[index];
		if (argument == "--trace")
		{
			if (index + 1 >= argc)
			{
				std::fprintf(stderr, "--trace needs a directory\n");
				return 2;
			}
			trace_path = argv[++index];
			continue;
		}
		if (argument == "--wall-time-ns" || argument == "--monotonic-time-ns")
		{
			if (index + 1 >= argc)
			{
				std::fprintf(stderr, "%s needs a value\n", argument.c_str());
				return 2;
			}
			const uint64_t value = std::strtoull(argv[++index], nullptr, 10);
			if (argument == "--wall-time-ns")
			{
				wall_time_ns = value;
			}
			else
			{
				monotonic_time_ns = value;
			}
			continue;
		}
		std::fprintf(stderr, "usage: %s [--trace DIR] [--wall-time-ns N] [--monotonic-time-ns N]\n",
			argv[0]);
		return 2;
	}

	// DuckDroid parity: JDGS_WALL_TIME_MS / JDGS_MONOTONIC_NS may inject a
	// replay base instead of the explicit flags.
	if (!wall_time_ns.has_value())
	{
		if (const char *wall = std::getenv("JDGS_WALL_TIME_MS"))
		{
			wall_time_ns = std::strtoull(wall, nullptr, 10) * 1000000ULL;
		}
	}
	if (!monotonic_time_ns.has_value())
	{
		if (const char *monotonic = std::getenv("JDGS_MONOTONIC_NS"))
		{
			monotonic_time_ns = std::strtoull(monotonic, nullptr, 10);
		}
	}

	RuntimeConfig config;
	config.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };
	// The native opens its own APK through the package code path.
	config.filesystem_roots = { DUCKEMU_JDGS_ROOT };
	// The device picture the pack was validated against (Mi 8, SDK 32). The
	// signer takes a newer platform path at ro.build.version.sdk >= 33.
	config.property_file = DUCKEMU_JDGS_PROPERTIES;
	config.trace_path = trace_path;
	config.default_timeout_us = kCallTimeoutUs;

	auto created = RuntimeInstance::create(config);
	if (!created.ok())
	{
		std::fprintf(stderr, "RuntimeInstance::create failed: %s\n",
			created.error().message.c_str());
		return 1;
	}
	RuntimeInstance &instance = *created.value();

	if (wall_time_ns.has_value())
	{
		instance.setWallTimeNs(wall_time_ns.value());
	}
	if (monotonic_time_ns.has_value())
	{
		instance.setMonotonicTimeNs(monotonic_time_ns.value());
	}

	// The project's Java side, before any module load.
	auto installed = instance.installJavaEnvironment(&jdgs::installEnvironment);
	if (!installed.ok())
	{
		std::fprintf(stderr, "install Java environment failed: %s\n",
			installed.error().message.c_str());
		return 1;
	}

	// Strict Java world: anything libjdg asks for and the project did not
	// declare shows up here instead of silently returning a default.
	instance.addJniListener([](const observer::JniEvent &event) {
		std::fprintf(stderr, "jni %s class=%s name=%s sig=%s slot=%u tid=%u\n",
			std::string(observer::jniEventKindName(event.kind)).c_str(),
			event.class_descriptor.c_str(),
			event.name.c_str(),
			event.signature.c_str(),
			event.slot,
			event.tid);
	});

	auto libc = instance.loadLibrary(romLibc());
	if (!libc.ok())
	{
		std::fprintf(stderr, "load libc failed: %s\n", libc.error().message.c_str());
		return 1;
	}
	auto module = instance.loadLibrary(DUCKEMU_JDGS_SO);
	if (!module.ok())
	{
		std::fprintf(stderr, "load libjdg failed: %s\n", module.error().message.c_str());
		return 1;
	}

	std::string error;
	for (const auto &phase : { std::make_pair("0", false), std::make_pair("1", true) })
	{
		auto arguments = jdgs::buildInitArguments(phase.first, phase.second);
		if (!arguments.ok())
		{
			std::fprintf(stderr, "cannot build init(%s): %s\n", phase.first,
				arguments.error().message.c_str());
			return 1;
		}
		uint64_t result = 0;
		if (!callMain(instance, kSelectorInit, arguments.value(), &result, &error))
		{
			std::fprintf(stderr, "Bridge.main(103, \"%s\") failed: %s\n", phase.first,
				error.c_str());
			return 1;
		}
		if (!reportInit(instance, result, phase.first, &error))
		{
			std::fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
	}

	const jdgs::SignParams params = jdgs::signParamsFromEnvironment();
	const char *repeat_text = std::getenv("JDGS_REPEAT");
	const int repeat = repeat_text != nullptr ? std::atoi(repeat_text) : 1;
	for (int iteration = 0; iteration < repeat; ++iteration)
	{
		std::string signature;
		if (!sign(instance, params, &signature, &error))
		{
			std::fprintf(stderr, "sign failed: %s\n", error.c_str());
			return 1;
		}
		std::printf("%s\n", signature.c_str());
	}
	return 0;
}
