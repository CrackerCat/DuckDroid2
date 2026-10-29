#include "fixtures/envs/fixture_env.hpp"

#include "runtime/jni/world/java_world.hpp"

namespace duckemu::test
{

Result<void> installFixtureEnvironment(runtime::jni::JavaWorld &world)
{
	auto klass = world.declareClass("env/Fixture", "java/lang/Object");
	if (!klass.ok())
	{
		return klass.error();
	}

	auto host = world.declareMethod(klass.value(), "hostValue", "()I", true,
		[](runtime::jni::JavaWorld &, runtime::jni::InvokeContext &) -> Result<uint64_t> {
			return uint64_t{ 42 };
		});
	if (!host.ok())
	{
		return host.error();
	}

	// Native methods must be declared by the environment (that is the Java
	// side); RegisterNatives then attaches the guest addresses.
	struct NativeDeclaration
	{
		const char *name;
		const char *signature;
	};
	static const NativeDeclaration kNatives[] = {
		{ "nativeAdd", "(II)I" },
		{ "callHost", "()I" },
		{ "callNative", "()I" },
		{ "findMissing", "()I" },
		{ "findMissingMethod", "()I" },
		{ "findDeclared", "()I" },
		{ "makeString", "()Ljava/lang/String;" },
		{ "stringLength", "()I" },
	};
	for (const NativeDeclaration &native : kNatives)
	{
		auto declared = world.declareMethod(klass.value(), native.name, native.signature, true, {});
		if (!declared.ok())
		{
			return declared.error();
		}
	}
	return {};
}

} // namespace duckemu::test
