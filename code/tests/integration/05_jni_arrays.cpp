// M5 follow-up gate: the JNI surface arrays and variadic calls need.
//   guest -> host : NewObjectArray / GetArrayLength / GetObjectArrayElement
//                   build and read an Object[]
//   guest -> host : NewObjectV runs a host constructor shim with a va_list
//   guest -> host : CallStaticObjectMethodV decodes object and int arguments
//                   from a va_list and receives a new object

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
		return test::finish("05_jni_arrays");
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
		return test::finish("05_jni_arrays");
	}

	auto loaded = instance.loadLibrary(DUCKEMU_JNI_FIXTURE_SO);
	CHECK_RESULT(loaded);
	if (!loaded.ok())
	{
		std::printf("  fixture load failed: %s\n", loaded.error().message.c_str());
		return test::finish("05_jni_arrays");
	}

	// "hi" -> String[1] -> element -> GetStringLength == 2.
	CHECK_EQ(call(instance, "objectArrayLength", "()I"), 2);
	// NewObjectV(box, <init>(I), 7) -> GetIntField(value) == 7.
	CHECK_EQ(call(instance, "newObjectValue", "()I"), 7);
	// CallStaticObjectMethodV(hostJoin, "hi", 7) == "hi:7" (length 4).
	CHECK_EQ(call(instance, "callHostV", "()I"), 4);

	// The happy path must not hit an unimplemented slot or a missing member.
	CHECK_EQ(events.size(), 0);

	return test::finish("05_jni_arrays");
}
