// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/jni/envs/envs.hpp"

#include "runtime/jni/world/java_world.hpp"

namespace duckemu::runtime::jni::envs
{

Result<void> installAll(JavaWorld &world)
{
	// Platform packs only. Target packs are installed by their project runner
	// (RuntimeInstance::installJavaEnvironment) and never referenced here.
	// Order matters: Number before BigInteger, and the byte stream and
	// BigInteger classes before the certificate pack reads their fields.
	constexpr Installer installers[] = {
		installJavaLang,
		installJavaIo,
		installJavaMath,
		installJavaSecurity,
	};
	for (const Installer installer : installers)
	{
		auto installed = installer(world);
		if (!installed.ok())
		{
			return installed.error();
		}
	}
	return {};
}

} // namespace duckemu::runtime::jni::envs
