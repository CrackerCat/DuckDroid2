// SPDX-License-Identifier: GPL-2.0-only
// libeleven_case: load the real library, call the dynamically registered
// native and print the returned Java String.
//
//   ./duckemu_libeleven_case        # data(1)
//   ./duckemu_libeleven_case 1 100  # data(1)..data(100)
//   ./duckemu_libeleven_case --trace DIR 5
//     # svc.log at DIR/svc.log and the instruction trace at DIR/insn/.
//     # The range is the native function's byte range defined below
//     # (kNativeTraceSize), not a CLI argument; requires
//     # -DDUCKEMU_ENABLE_TRACE=ON.
//
// The Java side of the project (the class and the native method declaration)
// is libeleven_env.cpp; main installs it explicitly before loading anything.
// The .so itself is in assets/ and is loaded through the runtime loader.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

#include "libeleven_case/libeleven_env.hpp"
#include "runtime/instance/runtime_instance.hpp"
#include "runtime/observer/semantic/jni_event.hpp"

#ifndef DUCKEMU_ROM_BOOTSTRAP
#error "DUCKEMU_ROM_BOOTSTRAP must point at the ROM bootstrap lib directory"
#endif
#ifndef DUCKEMU_ROM_LIB64
#error "DUCKEMU_ROM_LIB64 must point at the ROM system/lib64 directory"
#endif
#ifndef DUCKEMU_LIBELEVEN_SO
#error "DUCKEMU_LIBELEVEN_SO must point at libeleven.so"
#endif

using namespace duckemu;
using namespace duckemu::runtime;

namespace
{

constexpr const char *kClassName =
	"com/yuanrenxue/challenge/fragment/challenge/ChallengeElevenFragment";
constexpr const char *kMethodName = "data";
constexpr const char *kSignature = "(I)Ljava/lang/String;";

// The registered native starts at libeleven.so + 0x40cb4. The next function
// prologue begins at 0x40f78, so the function's own code range is 0x2c4 bytes.
// This is intentionally a code constant, not a CLI knob: the analysis target
// owns its trace range.
constexpr uint64_t kNativeTraceSize = 0x2c4;

std::string romLibc()
{
	return std::string(DUCKEMU_ROM_BOOTSTRAP) + "/libc.so";
}

} // namespace

int main(int argc, char **argv)
{
	std::string trace_path;
	std::optional<uint64_t> wall_time_ns;
	std::optional<uint64_t> monotonic_time_ns;
	int first = 1;
	int last = 1;
	int values = 0;
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
		const int value = std::atoi(argument.c_str());
		if (values == 0)
		{
			first = value;
		}
		last = value;
		++values;
	}
	if (values == 0)
	{
		first = last = 1;
	}

	if (!trace_path.empty())
	{
		std::error_code directory_error;
		std::filesystem::create_directories(trace_path, directory_error);
		if (directory_error)
		{
			std::fprintf(stderr, "cannot create trace directory %s: %s\n",
				trace_path.c_str(),
				directory_error.message().c_str());
			return 1;
		}
	}

	RuntimeConfig config;
	config.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };
	config.trace_path = trace_path.empty() ? std::string{} : trace_path + "/svc.log";

	auto created = RuntimeInstance::create(config);
	if (!created.ok())
	{
		std::fprintf(stderr, "RuntimeInstance::create failed: %s\n",
			created.error().message.c_str());
		return 1;
	}
	RuntimeInstance &instance = *created.value();

	// Deterministic replay inputs (DuckDroid parity): injected clocks start at
	// the given value and advance 1 ms per read.
	if (wall_time_ns.has_value())
	{
		instance.setWallTimeNs(wall_time_ns.value());
	}
	if (monotonic_time_ns.has_value())
	{
		instance.setMonotonicTimeNs(monotonic_time_ns.value());
	}

	// The project's Java side. Explicit and before any module is loaded: this
	// is the single wiring point, no static registration anywhere.
	auto installed = instance.installJavaEnvironment(&user::libeleven::installEnvironment);
	if (!installed.ok())
	{
		std::fprintf(stderr, "install Java environment failed: %s\n",
			installed.error().message.c_str());
		return 1;
	}

	// Strict Java world: anything the target asks for and the project did not
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
	auto module = instance.loadLibrary(DUCKEMU_LIBELEVEN_SO);
	if (!module.ok())
	{
		std::fprintf(stderr, "load libeleven failed: %s\n", module.error().message.c_str());
		return 1;
	}

	if (!trace_path.empty())
	{
		auto native = instance.lookupRegisteredNative(kClassName, kMethodName, kSignature);
		if (!native.ok())
		{
			std::fprintf(stderr, "lookup native for trace failed: %s\n",
				native.error().message.c_str());
			return 1;
		}
		observer::InsnTraceOptions options;
		options.directory = trace_path + "/insn";
		options.begin = native.value();
		options.size = kNativeTraceSize;
		auto started = instance.startInsnTrace(options);
		if (!started.ok())
		{
			std::fprintf(stderr, "start instruction trace failed: %s\n",
				started.error().message.c_str());
			return 1;
		}
	}

	// The native is static: x0 = JNIEnv*, x1 = jclass, x2 = the int argument.
	int failures = 0;
	for (int value = first; value <= last; ++value)
	{
		const std::array<uint64_t, 8> arguments = {
			static_cast<uint64_t>(static_cast<uint32_t>(value))
		};
		auto called =
			instance.callRegisteredNative(kClassName, kMethodName, kSignature, arguments);
		if (!called.ok())
		{
			std::fprintf(stderr, "data(%d) failed: %s\n", value, called.error().message.c_str());
			++failures;
			continue;
		}
		if (called.value() == 0)
		{
			std::fprintf(stderr, "data(%d) returned null\n", value);
			++failures;
			continue;
		}
		auto text = instance.jniStringUtf8(called.value());
		if (!text.ok())
		{
			std::fprintf(stderr, "data(%d) string decode failed: %s\n", value,
				text.error().message.c_str());
			++failures;
			continue;
		}
		std::printf("data(%d) = %s\n", value, text.value().c_str());
	}

	instance.stopInsnTrace();
	return failures == 0 ? 0 : 1;
}
