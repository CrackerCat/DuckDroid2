#include "runtime/jni/envs/envs.hpp"

#include "runtime/jni/world/java_world.hpp"

namespace duckemu::runtime::jni::envs
{

Result<void> installAll(JavaWorld &world)
{
	// Platform packs only. Target packs are installed by their project runner
	// (RuntimeInstance::installJavaEnvironment) and never referenced here.
	return installJavaLang(world);
}

} // namespace duckemu::runtime::jni::envs
