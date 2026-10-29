#include <cstdio>
#include <string>
#include <utility>

#include "runtime/clock/runtime_clock.hpp"
#include "runtime/jni/envs/envs.hpp"
#include "runtime/jni/world/java_world.hpp"

namespace duckemu::runtime::jni::envs
{
namespace
{

uint64_t identityHash(uint64_t object)
{
	// Stable for the lifetime of the object, non-zero for non-null objects.
	return (object >> 4) & 0x7FFFFFFFu;
}

Result<void> declare(JavaWorld &world,
	uint64_t klass,
	const char *name,
	const char *signature,
	bool is_static,
	ShimHandler shim)
{
	auto declared = world.declareMethod(klass, name, signature, is_static, std::move(shim));
	if (!declared.ok())
	{
		return declared.error();
	}
	return {};
}

std::string objectToString(JavaWorld &world, uint64_t object)
{
	const uint64_t klass = world.classOf(object);
	std::string name = descriptorToClassName(world.descriptorOfClass(klass));
	name += '@';
	char address[32];
	std::snprintf(address, sizeof(address), "%llx", static_cast<unsigned long long>(object));
	name += address;
	return name;
}

Result<void> installObject(JavaWorld &world)
{
	const uint64_t object = world.findClass("java/lang/Object");
	if (object == 0)
	{
		return Error{ ErrorCode::Internal, "java/lang/Object is not bootstrapped" };
	}

	auto result = declare(world, object, "getClass", "()Ljava/lang/Class;", false,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			return java.classOf(call.receiver);
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, object, "hashCode", "()I", false,
		[](JavaWorld &, InvokeContext &call) -> Result<uint64_t> {
			return identityHash(call.receiver);
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, object, "equals", "(Ljava/lang/Object;)Z", false,
		[](JavaWorld &, InvokeContext &call) -> Result<uint64_t> {
			return call.args[0] == call.receiver ? uint64_t{ 1 } : uint64_t{ 0 };
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, object, "toString", "()Ljava/lang/String;", false,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			return java.newString(objectToString(java, call.receiver));
		});
	return result;
}

Result<void> installClass(JavaWorld &world)
{
	const uint64_t klass = world.findClass("java/lang/Class");
	if (klass == 0)
	{
		return Error{ ErrorCode::Internal, "java/lang/Class is not bootstrapped" };
	}
	return declare(world, klass, "getName", "()Ljava/lang/String;", false,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			return java.newString(descriptorToClassName(java.descriptorOfClass(call.receiver)));
		});
}

Result<void> installString(JavaWorld &world)
{
	auto declared = world.declareClass("java/lang/String", "java/lang/Object");
	if (!declared.ok())
	{
		return declared.error();
	}
	const uint64_t klass = declared.value();

	auto result = declare(world, klass, "length", "()I", false,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			return java.stringLength(call.receiver);
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, klass, "charAt", "(I)C", false,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			return java.stringCharAt(call.receiver, call.args[0]);
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, klass, "equals", "(Ljava/lang/Object;)Z", false,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args[0] == 0)
			{
				return uint64_t{ 0 };
			}
			auto equal = java.stringEquals(call.receiver, call.args[0]);
			if (!equal.ok())
			{
				// equals(null) / equals(non-String) is false, not an error.
				return uint64_t{ 0 };
			}
			return equal.value() ? uint64_t{ 1 } : uint64_t{ 0 };
		});
	if (!result.ok())
	{
		return result;
	}
	// java.lang.String.hashCode: s[0]*31^(n-1) + ... + s[n-1] over UTF-16.
	result = declare(world, klass, "hashCode", "()I", false,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			auto length = java.stringLength(call.receiver);
			if (!length.ok())
			{
				return length.error();
			}
			uint32_t hash = 0;
			for (uint64_t index = 0; index < length.value(); ++index)
			{
				auto character = java.stringCharAt(call.receiver, index);
				if (!character.ok())
				{
					return character.error();
				}
				hash = hash * 31u + static_cast<uint32_t>(character.value());
			}
			return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(hash)));
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, klass, "toString", "()Ljava/lang/String;", false,
		[](JavaWorld &, InvokeContext &call) -> Result<uint64_t> {
			return call.receiver;
		});
	return result;
}

Result<void> installSystem(JavaWorld &world)
{
	auto declared = world.declareClass("java/lang/System", "java/lang/Object");
	if (!declared.ok())
	{
		return declared.error();
	}
	const uint64_t klass = declared.value();

	auto result = declare(world, klass, "currentTimeMillis", "()J", true,
		[](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
			return java.clock().nextRealtimeNs() / 1000000ULL;
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, klass, "nanoTime", "()J", true,
		[](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
			return java.clock().nextMonotonicNs();
		});
	if (!result.ok())
	{
		return result;
	}
	return declare(world, klass, "identityHashCode", "(Ljava/lang/Object;)I", true,
		[](JavaWorld &, InvokeContext &call) -> Result<uint64_t> {
			return call.args[0] == 0 ? uint64_t{ 0 } : identityHash(call.args[0]);
		});
}

} // namespace

Result<void> installJavaLang(JavaWorld &world)
{
	const auto installers = { installObject, installClass, installString, installSystem };
	for (const auto installer : installers)
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
