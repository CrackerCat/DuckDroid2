// libeleven project environment.
//
// Target-specific Java declarations belong to the project, not to the runtime:
//   src/runtime/jni/envs/        platform supplements (java.lang, android.*)
//   code/user_code/<case>/       one target project, installed by its main.cpp
//
// libeleven's JNI_OnLoad registers
//   ChallengeElevenFragment.data(I)Ljava/lang/String;
// The class hierarchy is intentionally not invented: the .so only needs the
// class to exist so FindClass succeeds, and the method to be declared so
// RegisterNatives can bind it. When M6 discovers which inherited members the
// native actually uses, the real superclass is added here.
#include "libeleven_case/libeleven_env.hpp"

#include "runtime/jni/world/java_world.hpp"

namespace duckemu::user::libeleven
{

Result<void> installEnvironment(runtime::jni::JavaWorld &world)
{
	auto target = world.declareClass(
		"com/yuanrenxue/challenge/fragment/challenge/ChallengeElevenFragment", "");
	if (!target.ok())
	{
		return target.error();
	}

	// Declared static: the runner calls registered natives with a jclass, and
	// RegisterNatives does not carry the static/instance bit.
	auto data = world.declareMethod(target.value(), "data", "(I)Ljava/lang/String;", true, {});
	if (!data.ok())
	{
		return data.error();
	}
	return {};
}

} // namespace duckemu::user::libeleven
