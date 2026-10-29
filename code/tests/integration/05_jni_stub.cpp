// SPDX-License-Identifier: GPL-2.0-only
// M5 S4 gate: both directions through the JNI table.
//   host -> guest : callRegisteredNative enters a dynamically registered native
//   guest -> host : CallStaticIntMethod reaches a host shim (hostValue)
//   guest -> guest: CallStaticIntMethod on a registered native nests
//                   uc_emu_start on the same engine

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "fixtures/envs/fixture_env.hpp"
#include "runtime/instance/runtime_instance.hpp"
#include "runtime/observer/semantic/jni_event.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

namespace
{

constexpr const char *kFixtureClass = "env/Fixture";

uint64_t call(RuntimeInstance &instance,
	const char *name,
	const char *signature,
	const std::array<uint64_t, 8> &args = {})
{
	auto result = instance.callRegisteredNative(kFixtureClass, name, signature, args);
	CHECK_RESULT(result);
	if (!result.ok())
	{
		std::printf("  %s%s failed: %s\n", name, signature, result.error().message.c_str());
		return 0;
	}
	return result.value();
}

} // namespace

int main()
{
	RuntimeConfig config;
	auto created = RuntimeInstance::create(config);
	CHECK_RESULT(created);
	if (!created.ok())
	{
		return test::finish("05_jni_stub");
	}
	RuntimeInstance &instance = *created.value();

	std::vector<observer::JniEvent> events;
	instance.addJniListener([&events](const observer::JniEvent &event) {
		events.push_back(event);
	});

	auto installed = instance.installJavaEnvironment(&test::installFixtureEnvironment);
	CHECK_RESULT(installed);
	if (!installed.ok())
	{
		std::printf("  fixture environment install failed: %s\n",
			installed.error().message.c_str());
		return test::finish("05_jni_stub");
	}

	auto loaded = instance.loadLibrary(DUCKEMU_JNI_FIXTURE_SO);
	CHECK_RESULT(loaded);
	if (!loaded.ok())
	{
		std::printf("  fixture load failed: %s\n", loaded.error().message.c_str());
		return test::finish("05_jni_stub");
	}

	// host -> guest: the registered native itself.
	CHECK_EQ(call(instance, "nativeAdd", "(II)I", { 20, 22 }), 42);

	// guest -> host: the native asks the JNI table for a host shim.
	CHECK_EQ(call(instance, "callHost", "()I"), 42);

	// guest -> guest: the native calls another registered native through the
	// JNI table; that call nests on the same engine.
	CHECK_EQ(call(instance, "callNative", "()I"), 42);

	auto registered = instance.lookupRegisteredNative(kFixtureClass, "nativeAdd", "(II)I");
	CHECK_RESULT(registered);
	if (registered.ok())
	{
		CHECK(registered.value().value() != 0);
	}

	auto missing = instance.lookupRegisteredNative(kFixtureClass, "noSuchNative", "()I");
	CHECK(!missing.ok());

	// The happy path must not hit an unimplemented slot or a missing member.
	CHECK_EQ(events.size(), 0);

	return test::finish("05_jni_stub");
}
