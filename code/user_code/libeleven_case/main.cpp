// libeleven_case: load the real library, call the dynamically registered
// native and print the returned Java String.
//
//   ./duckemu_libeleven_case        # data(1)
//   ./duckemu_libeleven_case 1 100  # data(1)..data(100)
//   ./duckemu_libeleven_case --trace DIR 5
//
// The Java side of the project (the class and the native method declaration)
// is libeleven_env.cpp; main installs it explicitly before loading anything.
// The .so itself is in assets/ and is loaded through the runtime loader.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

std::string romLibc()
{
	return std::string(DUCKEMU_ROM_BOOTSTRAP) + "/libc.so";
}

} // namespace

int main(int argc, char **argv)
{
	std::string trace_path;
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

	RuntimeConfig config;
	config.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };
	config.trace_path = trace_path;

	auto created = RuntimeInstance::create(config);
	if (!created.ok())
	{
		std::fprintf(stderr, "RuntimeInstance::create failed: %s\n",
			created.error().message.c_str());
		return 1;
	}
	RuntimeInstance &instance = *created.value();

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

	return failures == 0 ? 0 : 1;
}
