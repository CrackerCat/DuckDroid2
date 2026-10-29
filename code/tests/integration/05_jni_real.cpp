// M5 S3: libeleven's real JNI_OnLoad.
//
// The library resolves and calls GetEnv(vm, &env, JNI_VERSION_1_6),
// FindClass("com/.../ChallengeElevenFragment") and RegisterNatives for
// data(I)Ljava/lang/String;. The environment declares the class and the
// method; RegisterNatives binds the guest address.
//
// The checked address comes from the .so itself: JNI_OnLoad is at 0x4139c and
// the registered native body at 0x40cb4, so module_base + 0x40cb4 must equal
// JNI_OnLoad_address - 0x4139c + 0x40cb4. If the asset changes, this test is
// expected to fail loudly rather than silently accept any non-zero address.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "libeleven_case/libeleven_env.hpp"
#include "runtime/instance/runtime_instance.hpp"
#include "runtime/observer/semantic/jni_event.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

namespace
{

constexpr const char *kTargetClass =
	"com/yuanrenxue/challenge/fragment/challenge/ChallengeElevenFragment";
constexpr uint64_t kJniOnLoadOffset = 0x4139c;
constexpr uint64_t kDataOffset = 0x40cb4;

std::string romLibc()
{
	return std::string(DUCKEMU_ROM_BOOTSTRAP) + "/libc.so";
}

} // namespace

int main()
{
	RuntimeConfig config;
	config.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };

	auto created = RuntimeInstance::create(config);
	CHECK_RESULT(created);
	if (!created.ok())
	{
		return test::finish("05_jni_real");
	}
	RuntimeInstance &instance = *created.value();

	std::vector<observer::JniEvent> events;
	instance.addJniListener([&events](const observer::JniEvent &event) {
		events.push_back(event);
	});

	auto installed = instance.installJavaEnvironment(&user::libeleven::installEnvironment);
	CHECK_RESULT(installed);
	if (!installed.ok())
	{
		std::printf("  libeleven environment install failed: %s\n",
			installed.error().message.c_str());
		return test::finish("05_jni_real");
	}

	auto libc = instance.loadLibrary(romLibc());
	CHECK_RESULT(libc);
	if (!libc.ok())
	{
		std::printf("  libc error: %s\n", libc.error().message.c_str());
		return test::finish("05_jni_real");
	}

	auto eleven = instance.loadLibrary(DUCKEMU_LIBELEVEN_CASE_SO);
	CHECK_RESULT(eleven);
	if (!eleven.ok())
	{
		std::printf("  libeleven error: %s\n", eleven.error().message.c_str());
		return test::finish("05_jni_real");
	}

	auto jni_on_load = instance.lookupSymbol(eleven.value(), "JNI_OnLoad");
	CHECK_RESULT(jni_on_load);
	auto registered = instance.lookupRegisteredNative(kTargetClass, "data", "(I)Ljava/lang/String;");
	CHECK_RESULT(registered);
	if (jni_on_load.ok() && registered.ok())
	{
		const uint64_t expected = jni_on_load.value().value() - kJniOnLoadOffset + kDataOffset;
		std::printf("  JNI_OnLoad=0x%llx data=0x%llx expected=0x%llx\n",
			static_cast<unsigned long long>(jni_on_load.value().value()),
			static_cast<unsigned long long>(registered.value().value()),
			static_cast<unsigned long long>(expected));
		CHECK_EQ(registered.value().value(), expected);
	}

	// Undeclared class / unregistered method stay strict.
	auto undeclared = instance.lookupRegisteredNative("env/NotDeclared", "data", "()V");
	CHECK(!undeclared.ok());

	// JNI_OnLoad's FindClass and RegisterNatives must both have been satisfied
	// by the environment; nothing may report a missing member.
	CHECK_EQ(events.size(), 0);
	for (const observer::JniEvent &event : events)
	{
		std::printf("  jni event kind=%s class=%s name=%s\n",
			std::string(observer::jniEventKindName(event.kind)).c_str(),
			event.class_descriptor.c_str(),
			event.name.c_str());
	}

	return test::finish("05_jni_real");
}
