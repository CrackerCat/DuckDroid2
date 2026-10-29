// M5 S2: JNIEnv function table, strict class/method resolution and strings.
// The fixture .so has no DT_NEEDED; every JNI call it makes goes through the
// table that JNI_OnLoad receives, so this test isolates the JNI layer from the
// real bionic.

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
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
		return test::finish("05_jni_env");
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
		return test::finish("05_jni_env");
	}

	auto loaded = instance.loadLibrary(DUCKEMU_JNI_FIXTURE_SO);
	CHECK_RESULT(loaded);
	if (!loaded.ok())
	{
		std::printf("  fixture load failed: %s\n", loaded.error().message.c_str());
		return test::finish("05_jni_env");
	}

	// JNI_OnLoad ran FindClass + RegisterNatives; both must be declared, so the
	// happy path publishes no strict-mode failures.
	CHECK_EQ(events.size(), 0);

	CHECK_EQ(call(instance, "findDeclared", "()I"), 1);
	CHECK_EQ(call(instance, "stringLength", "()I"), 5);

	auto text = instance.callRegisteredNative(kFixtureClass, "makeString", "()Ljava/lang/String;");
	CHECK_RESULT(text);
	if (text.ok())
	{
		CHECK(text.value() != 0);
		auto decoded = instance.jniStringUtf8(text.value());
		CHECK_RESULT(decoded);
		if (decoded.ok())
		{
			CHECK(decoded.value() == "value:42");
		}
	}

	// Strict misses: the JNI return is null, and the failure is observable.
	CHECK_EQ(call(instance, "findMissing", "()I"), 1);
	CHECK_EQ(call(instance, "findMissingMethod", "()I"), 1);

	bool saw_missing_class = false;
	bool saw_missing_method = false;
	for (const observer::JniEvent &event : events)
	{
		if (event.kind == observer::JniEventKind::MissingClass)
		{
			saw_missing_class = event.class_descriptor == "env/NotDeclared";
		}
		if (event.kind == observer::JniEventKind::MissingMethod)
		{
			saw_missing_method = event.name == "hostValue" && event.signature == "()J";
		}
	}
	CHECK(saw_missing_class);
	CHECK(saw_missing_method);

	// No call reached an unimplemented table slot.
	for (const observer::JniEvent &event : events)
	{
		CHECK(event.kind != observer::JniEventKind::UnsupportedFunction);
	}

	return test::finish("05_jni_env");
}
