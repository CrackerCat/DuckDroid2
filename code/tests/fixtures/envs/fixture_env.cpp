// SPDX-License-Identifier: GPL-2.0-only
#include "fixtures/envs/fixture_env.hpp"

#include <string>

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

	// Variadic call target: "hi" + ":" + 7.
	auto join = world.declareMethod(klass.value(), "hostJoin",
		"(Ljava/lang/String;I)Ljava/lang/String;", true,
		[](runtime::jni::JavaWorld &java,
			runtime::jni::InvokeContext &call) -> Result<uint64_t> {
			auto text = java.stringUtf8(call.args[0]);
			if (!text.ok())
			{
				return text.error();
			}
			return java.newString(text.value() + ":" + std::to_string(call.args[1]));
		});
	if (!join.ok())
	{
		return join.error();
	}

	// env/Box: the NewObjectV fixture constructs it and reads the field back.
	auto box = world.declareClass("env/Box", "java/lang/Object");
	if (!box.ok())
	{
		return box.error();
	}
	auto value = world.declareField(box.value(), "value", "I", false);
	if (!value.ok())
	{
		return value.error();
	}
	auto ctor = world.declareMethod(box.value(), "<init>", "(I)V", false,
		[value_field = value.value()](runtime::jni::JavaWorld &java,
			runtime::jni::InvokeContext &call) -> Result<uint64_t> {
			auto written = java.writeInstanceField(call.receiver, value_field, call.args[0]);
			if (!written.ok())
			{
				return written.error();
			}
			return uint64_t{ 0 };
		});
	if (!ctor.ok())
	{
		return ctor.error();
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
		{ "objectArrayLength", "()I" },
		{ "newObjectValue", "()I" },
		{ "callHostV", "()I" },
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
