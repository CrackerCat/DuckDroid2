// SPDX-License-Identifier: GPL-2.0-only
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#include "common/id/ids.hpp"
#include "runtime/clock/runtime_clock.hpp"
#include "runtime/jni/envs/envs.hpp"
#include "runtime/jni/world/java_world.hpp"
#include "runtime/memory/address_space.hpp"

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

// Number of parameters in a method signature ("(ILjava/lang/String;)V" -> 2).
size_t signatureParameterCount(std::string_view signature)
{
	if (signature.size() < 2 || signature.front() != '(')
	{
		return 0;
	}
	size_t index = 1;
	size_t count = 0;
	while (index < signature.size() && signature[index] != ')')
	{
		if (signature[index] == '[')
		{
			++index;
			continue;
		}
		if (signature[index] == 'L')
		{
			const size_t end = signature.find(';', index);
			if (end == std::string_view::npos)
			{
				break;
			}
			index = end + 1;
		}
		else
		{
			++index;
		}
		++count;
	}
	return count;
}

Result<uint64_t> makeMethodObject(JavaWorld &world, uint64_t method_id)
{
	const uint64_t method_class = world.findClass("java/lang/reflect/Method");
	if (method_class == 0)
	{
		return Error{ ErrorCode::NotFound, "java/lang/reflect/Method is not installed" };
	}
	const uint64_t id_field = world.findField(method_class, "__id", "J", false);
	auto object = world.allocateObject(method_class);
	if (!object.ok())
	{
		return object.error();
	}
	auto written = world.writeInstanceField(object.value(), id_field, method_id);
	if (!written.ok())
	{
		return written.error();
	}
	return object.value();
}

Result<uint64_t> makeFieldObject(JavaWorld &world, uint64_t field_id)
{
	const uint64_t field_class = world.findClass("java/lang/reflect/Field");
	if (field_class == 0)
	{
		return Error{ ErrorCode::NotFound, "java/lang/reflect/Field is not installed" };
	}
	const uint64_t id_field = world.findField(field_class, "__id", "J", false);
	auto object = world.allocateObject(field_class);
	if (!object.ok())
	{
		return object.error();
	}
	auto written = world.writeInstanceField(object.value(), id_field, field_id);
	if (!written.ok())
	{
		return written.error();
	}
	return object.value();
}

// Method.invoke/Field.get return Object; a primitive result is boxed the way
// the Java language requires.
Result<uint64_t> boxValue(JavaWorld &world, ValueKind kind, uint64_t raw)
{
	const char *wrapper = nullptr;
	const char *field_signature = nullptr;
	switch (kind)
	{
	case ValueKind::Int:
		wrapper = "java/lang/Integer";
		field_signature = "I";
		break;
	case ValueKind::Boolean:
		wrapper = "java/lang/Boolean";
		field_signature = "Z";
		break;
	case ValueKind::Long:
		wrapper = "java/lang/Long";
		field_signature = "J";
		break;
	default:
		return raw;
	}
	const uint64_t klass = world.findClass(wrapper);
	const uint64_t field = klass == 0 ? 0 : world.findField(klass, "value", field_signature, false);
	if (field == 0)
	{
		return raw;
	}
	auto object = world.allocateObject(klass);
	if (!object.ok())
	{
		return object.error();
	}
	auto written = world.writeInstanceField(object.value(), field, raw);
	if (!written.ok())
	{
		return written.error();
	}
	return object.value();
}

Result<uint64_t> unboxArgument(JavaWorld &world, ValueKind kind, uint64_t object)
{
	if (object == 0)
	{
		return uint64_t{ 0 };
	}
	switch (kind)
	{
	case ValueKind::Boolean:
	{
		if (!world.isInstanceOf(object, world.findClass("java/lang/Boolean")))
		{
			return object;
		}
		auto value = booleanValue(world, object);
		if (!value.ok())
		{
			return value.error();
		}
		return value.value() ? uint64_t{ 1 } : uint64_t{ 0 };
	}
	case ValueKind::Int:
	case ValueKind::Byte:
	case ValueKind::Short:
	case ValueKind::Char:
	{
		auto value = integerValue(world, object);
		if (!value.ok())
		{
			return object;
		}
		return static_cast<uint64_t>(static_cast<uint32_t>(value.value()));
	}
	case ValueKind::Long:
	{
		if (!world.isInstanceOf(object, world.findClass("java/lang/Long")))
		{
			return object;
		}
		return longValue(world, object);
	}
	default:
		return object;
	}
}

Result<void> installClass(JavaWorld &world)
{
	const uint64_t klass = world.findClass("java/lang/Class");
	if (klass == 0)
	{
		return Error{ ErrorCode::Internal, "java/lang/Class is not bootstrapped" };
	}
	auto named = declare(world, klass, "getName", "()Ljava/lang/String;", false,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			return java.newString(descriptorToClassName(java.descriptorOfClass(call.receiver)));
		});
	if (!named.ok())
	{
		return named.error();
	}
	// Hidden field libcore exposes; reflection reads it to test member
	// visibility. Not carrying per-class access flags yet, so it reads 0.
	auto access_flags = world.declareField(klass, "accessFlags", "I", false);
	if (!access_flags.ok())
	{
		return access_flags.error();
	}
	auto for_name = declare(world, klass, "forName",
		"(Ljava/lang/String;)Ljava/lang/Class;", true,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.empty() || call.args[0] == 0)
			{
				return uint64_t{ 0 };
			}
			auto name = java.stringUtf8(call.args[0]);
			if (!name.ok())
			{
				return name.error();
			}
			for (char &character : name.value())
			{
				if (character == '.')
				{
					character = '/';
				}
			}
			return java.findClass(name.value());
		});
	if (!for_name.ok())
	{
		return for_name;
	}

	// Reflection over members: the target asks a Class for its declared
	// methods/fields and inspects them by name. Exactly what a device returns,
	// with one wrapper object per declared member.
	auto methods = declare(world, klass, "getDeclaredMethods",
		"()[Ljava/lang/reflect/Method;", false,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			const uint64_t method_class = java.findClass("java/lang/reflect/Method");
			if (method_class == 0)
			{
				return Error{ ErrorCode::NotFound,
					"java/lang/reflect/Method is not installed" };
			}
			const std::vector<uint64_t> ids = java.declaredMethods(call.receiver);
			auto array = java.newArray(ArrayElementKind::Object, ids.size(), method_class);
			if (!array.ok())
			{
				return array.error();
			}
			for (size_t index = 0; index < ids.size(); ++index)
			{
				auto object = makeMethodObject(java, ids[index]);
				if (!object.ok())
				{
					return object.error();
				}
				auto stored = java.writeArrayElement(array.value(), index, object.value());
				if (!stored.ok())
				{
					return stored.error();
				}
			}
			return array.value();
		});
	if (!methods.ok())
	{
		return methods;
	}
	auto declared_method = declare(world, klass, "getDeclaredMethod",
		"(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;", false,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() < 2 || call.args[0] == 0)
			{
				return uint64_t{ 0 };
			}
			auto name = java.stringUtf8(call.args[0]);
			if (!name.ok())
			{
				return name.error();
			}
			size_t parameters = 0;
			if (call.args[1] != 0)
			{
				auto view = java.arrayView(call.args[1]);
				if (!view.ok())
				{
					return view.error();
				}
				parameters = view.value().length;
			}
			for (const uint64_t id : java.declaredMethods(call.receiver))
			{
				const MethodRecord *record = java.method(id);
				if (record == nullptr || record->name != name.value())
				{
					continue;
				}
				if (signatureParameterCount(record->signature) != parameters)
				{
					continue;
				}
				return makeMethodObject(java, id);
			}
			return uint64_t{ 0 };
		});
	if (!declared_method.ok())
	{
		return declared_method;
	}
	return declare(world, klass, "getDeclaredFields",
		"()[Ljava/lang/reflect/Field;", false,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			const uint64_t field_class = java.findClass("java/lang/reflect/Field");
			if (field_class == 0)
			{
				return Error{ ErrorCode::NotFound,
					"java/lang/reflect/Field is not installed" };
			}
			const std::vector<uint64_t> ids = java.declaredFields(call.receiver);
			auto array = java.newArray(ArrayElementKind::Object, ids.size(), field_class);
			if (!array.ok())
			{
				return array.error();
			}
			for (size_t index = 0; index < ids.size(); ++index)
			{
				auto object = makeFieldObject(java, ids[index]);
				if (!object.ok())
				{
					return object.error();
				}
				auto stored = java.writeArrayElement(array.value(), index, object.value());
				if (!stored.ok())
				{
					return stored.error();
				}
			}
			return array.value();
		});
}

// java.lang.reflect: Method/Field/AccessibleObject with a host-side id in a
// private field. Targets reflect over platform classes by name; the ids are
// engine method ids, so a name/declaring-class read is exact and invoke()
// flips through the same shim the JNI call would reach.
Result<void> installReflection(JavaWorld &world)
{
	auto accessible = world.declareClass("java/lang/reflect/AccessibleObject",
		"java/lang/Object");
	if (!accessible.ok())
	{
		return accessible.error();
	}
	auto accessible_set = declare(world, accessible.value(), "setAccessible",
		"(Z)V", false, [](JavaWorld &, InvokeContext &) -> Result<uint64_t> {
			return uint64_t{ 0 };
		});
	if (!accessible_set.ok())
	{
		return accessible_set;
	}
	auto accessible_get = declare(world, accessible.value(), "isAccessible", "()Z",
		false, [](JavaWorld &, InvokeContext &) -> Result<uint64_t> {
			return uint64_t{ 0 };
		});
	if (!accessible_get.ok())
	{
		return accessible_get;
	}

	auto method = world.declareClass("java/lang/reflect/Method",
		"java/lang/reflect/AccessibleObject");
	if (!method.ok())
	{
		return method.error();
	}
	auto method_id = world.declareField(method.value(), "__id", "J", false);
	if (!method_id.ok())
	{
		return method_id.error();
	}
	const uint64_t method_id_field = method_id.value();

	auto method_name = declare(world, method.value(), "getName",
		"()Ljava/lang/String;", false,
		[method_id_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			auto id = java.readInstanceField(call.receiver, method_id_field);
			if (!id.ok())
			{
				return id.error();
			}
			const MethodRecord *record = java.method(id.value());
			if (record == nullptr)
			{
				return uint64_t{ 0 };
			}
			return java.newString(record->name);
		});
	if (!method_name.ok())
	{
		return method_name;
	}
	auto method_declaring = declare(world, method.value(), "getDeclaringClass",
		"()Ljava/lang/Class;", false,
		[method_id_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			auto id = java.readInstanceField(call.receiver, method_id_field);
			if (!id.ok())
			{
				return id.error();
			}
			const MethodRecord *record = java.method(id.value());
			return record == nullptr ? uint64_t{ 0 } : record->class_object;
		});
	if (!method_declaring.ok())
	{
		return method_declaring;
	}
	auto method_parameters = declare(world, method.value(), "getParameterTypes",
		"()[Ljava/lang/Class;", false,
		[](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
			return java.newArray(ArrayElementKind::Object, 0, java.classClass());
		});
	if (!method_parameters.ok())
	{
		return method_parameters;
	}
	auto method_return = declare(world, method.value(), "getReturnType",
		"()Ljava/lang/Class;", false,
		[](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 0 }; });
	if (!method_return.ok())
	{
		return method_return;
	}
	auto method_modifiers = declare(world, method.value(), "getModifiers", "()I", false,
		[](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 0 }; });
	if (!method_modifiers.ok())
	{
		return method_modifiers;
	}
	// invoke() flips through the same shim a direct JNI call would reach, and
	// boxes a primitive result the way the Java language requires.
	auto method_invoke = declare(world, method.value(), "invoke",
		"(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;", false,
		[method_id_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			auto id = java.readInstanceField(call.receiver, method_id_field);
			if (!id.ok())
			{
				return id.error();
			}
			const MethodRecord *record = java.method(id.value());
			if (record == nullptr || !record->shim)
			{
				return uint64_t{ 0 };
			}
			InvokeContext inner;
			inner.receiver = record->is_static || call.args.empty() ? 0 : call.args[0];
			inner.tid = call.tid;
			const uint64_t array = call.args.size() > 1 ? call.args[1] : 0;
			if (array == 0)
			{
				if (!record->parsed.args.empty())
				{
					return uint64_t{ 0 };
				}
			}
			else
			{
				auto view = java.arrayView(array);
				if (!view.ok())
				{
					return view.error();
				}
				if (view.value().length != record->parsed.args.size())
				{
					return uint64_t{ 0 };
				}
				for (size_t index = 0; index < record->parsed.args.size(); ++index)
				{
					auto element = java.readArrayElement(array, index);
					if (!element.ok())
					{
						return element.error();
					}
					const ValueKind kind = record->parsed.args[index];
					if (kind == ValueKind::Object)
					{
						inner.args.push_back(element.value());
						continue;
					}
					auto value = unboxArgument(java, kind, element.value());
					if (!value.ok())
					{
						return value.error();
					}
					inner.args.push_back(value.value());
				}
			}
			auto called = record->shim(java, inner);
			if (!called.ok())
			{
				return called.error();
			}
			return boxValue(java, record->parsed.result, called.value());
		});
	if (!method_invoke.ok())
	{
		return method_invoke;
	}

	auto field = world.declareClass("java/lang/reflect/Field",
		"java/lang/reflect/AccessibleObject");
	if (!field.ok())
	{
		return field.error();
	}
	auto field_id = world.declareField(field.value(), "__id", "J", false);
	if (!field_id.ok())
	{
		return field_id.error();
	}
	const uint64_t field_id_field = field_id.value();
	auto field_name = declare(world, field.value(), "getName", "()Ljava/lang/String;",
		false, [field_id_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			auto id = java.readInstanceField(call.receiver, field_id_field);
			if (!id.ok())
			{
				return id.error();
			}
			const FieldRecord *record = java.field(id.value());
			if (record == nullptr)
			{
				return uint64_t{ 0 };
			}
			return java.newString(record->name);
		});
	if (!field_name.ok())
	{
		return field_name;
	}
	auto field_declaring = declare(world, field.value(), "getDeclaringClass",
		"()Ljava/lang/Class;", false,
		[field_id_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			auto id = java.readInstanceField(call.receiver, field_id_field);
			if (!id.ok())
			{
				return id.error();
			}
			const FieldRecord *record = java.field(id.value());
			return record == nullptr ? uint64_t{ 0 } : record->class_object;
		});
	if (!field_declaring.ok())
	{
		return field_declaring;
	}
	const auto read_field = [field_id_field](JavaWorld &java,
				   InvokeContext &call) -> Result<uint64_t> {
		auto id = java.readInstanceField(call.receiver, field_id_field);
		if (!id.ok())
		{
			return id.error();
		}
		const FieldRecord *record = java.field(id.value());
		if (record == nullptr)
		{
			return uint64_t{ 0 };
		}
		const uint64_t target = call.args.empty() ? 0 : call.args[0];
		return record->is_static ? java.readStaticField(record->class_object, record->id) :
					   java.readInstanceField(target, record->id);
	};
	auto field_get = declare(world, field.value(), "get",
		"(Ljava/lang/Object;)Ljava/lang/Object;", false,
		[field_id_field, read_field](JavaWorld &java,
					   InvokeContext &call) -> Result<uint64_t> {
			auto id = java.readInstanceField(call.receiver, field_id_field);
			if (!id.ok())
			{
				return id.error();
			}
			const FieldRecord *record = java.field(id.value());
			if (record == nullptr)
			{
				return uint64_t{ 0 };
			}
			auto value = read_field(java, call);
			if (!value.ok())
			{
				return value.error();
			}
			return boxValue(java, record->kind, value.value());
		});
	if (!field_get.ok())
	{
		return field_get;
	}
	const struct
	{
		const char *name;
		const char *signature;
	} raw_getters[] = {
		{ "getBoolean", "(Ljava/lang/Object;)Z" },
		{ "getByte", "(Ljava/lang/Object;)B" },
		{ "getChar", "(Ljava/lang/Object;)C" },
		{ "getShort", "(Ljava/lang/Object;)S" },
		{ "getInt", "(Ljava/lang/Object;)I" },
		{ "getLong", "(Ljava/lang/Object;)J" },
	};
	Result<void> result;
	for (const auto &getter : raw_getters)
	{
		result = declare(world, field.value(), getter.name, getter.signature, false,
			read_field);
		if (!result.ok())
		{
			return result;
		}
	}
	return result;
}

// java.lang.ClassLoader: the managed class loader chain. Targets that load a
// plugin dex through a PathClassLoader ask the Java side for the loader and
// then call findClass on it. The engine models classes itself, so the loader
// only has to exist, be stable across calls, and resolve a dotted name to the
// declared class object (unknown names stay 0, never a synthetic class).
Result<void> installClassLoader(JavaWorld &world)
{
	auto declared = world.declareClass("java/lang/ClassLoader", "java/lang/Object");
	if (!declared.ok())
	{
		return declared.error();
	}
	const uint64_t klass = declared.value();

	auto result = declare(world, klass, "<init>", "()V", false,
		[](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 0 }; });
	if (!result.ok())
	{
		return result;
	}
	// A boot-class loader's parent is null; targets only call getParent to
	// walk the chain and stop at the end.
	result = declare(world, klass, "getParent", "()Ljava/lang/ClassLoader;", false,
		[](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 0 }; });
	if (!result.ok())
	{
		return result;
	}
	const auto resolve = [](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
		if (call.args.empty() || call.args[0] == 0)
		{
			return uint64_t{ 0 };
		}
		auto name = java.stringUtf8(call.args[0]);
		if (!name.ok())
		{
			return name.error();
		}
		for (char &character : name.value())
		{
			if (character == '.')
			{
				character = '/';
			}
		}
		return java.findClass(name.value());
	};
	auto find = declare(world, klass, "findClass",
		"(Ljava/lang/String;)Ljava/lang/Class;", false, resolve);
	if (!find.ok())
	{
		return find;
	}
	// loadClass is the public entry; a device throws ClassNotFoundException on
	// a miss. The engine has no exception model here, so a miss is null and
	// the JNI layer reports it as a missing class like any other lookup.
	return declare(world, klass, "loadClass",
		"(Ljava/lang/String;)Ljava/lang/Class;", false, resolve);
}

Result<void> installString(JavaWorld &world)
{
	auto sequence = world.declareClass("java/lang/CharSequence", "");
	if (!sequence.ok())
	{
		return sequence.error();
	}
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
	if (!result.ok())
	{
		return result;
	}
	// String([B, String charsetName)V: native code commonly assembles text by
	// passing the byte[] and a charset name. The served targets pass UTF-8, so
	// the name is accepted but not interpreted.
	result = declare(world, klass, "<init>", "([BLjava/lang/String;)V", false,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() != 2 || call.args[0] == 0)
			{
				return Error{ ErrorCode::InvalidArgument,
					"String([B,String) needs a byte array" };
			}
			auto view = java.arrayView(call.args[0]);
			if (!view.ok())
			{
				return view.error();
			}
			if (view.value().kind != ArrayElementKind::Byte)
			{
				return Error{ ErrorCode::InvalidArgument,
					"String([B,String) expects a byte[]" };
			}
			std::string bytes(static_cast<size_t>(view.value().length), '\0');
			if (!bytes.empty())
			{
				auto read = java.memory().read(GuestVA{ view.value().storage },
					bytes.data(),
					bytes.size());
				if (!read.ok())
				{
					return read.error();
				}
			}
			auto set = java.setStringContent(call.receiver, bytes);
			if (!set.ok())
			{
				return set.error();
			}
			return uint64_t{ 0 };
		});
	if (!result.ok())
	{
		return result;
	}
	// String.getBytes(String charsetName)[B: the charset name is accepted but
	// the bytes are always UTF-8, matching String([B, String) above.
	return declare(world, klass, "getBytes", "(Ljava/lang/String;)[B", false,
		[](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			auto text = java.stringUtf8(call.receiver);
			if (!text.ok())
			{
				return text.error();
			}
			auto array = java.newArray(ArrayElementKind::Byte, text.value().size());
			if (!array.ok())
			{
				return array.error();
			}
			auto view = java.arrayView(array.value());
			if (!view.ok())
			{
				return view.error();
			}
			if (!text.value().empty())
			{
				auto written = java.memory().write(GuestVA{ view.value().storage },
					text.value().data(),
					text.value().size());
				if (!written.ok())
				{
					return written.error();
				}
			}
			return array.value();
		});
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
	result = declare(world, klass, "identityHashCode", "(Ljava/lang/Object;)I", true,
		[](JavaWorld &, InvokeContext &call) -> Result<uint64_t> {
			return call.args[0] == 0 ? uint64_t{ 0 } : identityHash(call.args[0]);
		});
	if (!result.ok())
	{
		return result;
	}
	// System.getProperty serves the JVM properties the Android runtime seeds
	// itself (libcore System.initProperties); unknown keys are null, not "".
	// These are platform constants, not device values: ro.* belongs to the
	// property service and is deliberately not served here.
	return declare(world, klass, "getProperty", "(Ljava/lang/String;)Ljava/lang/String;",
		true, [](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.empty() || call.args[0] == 0)
			{
				return uint64_t{ 0 };
			}
			auto key = java.stringUtf8(call.args[0]);
			if (!key.ok())
			{
				return key.error();
			}
			const char *value = nullptr;
			if (key.value() == "os.arch")
			{
				value = "aarch64";
			}
			else if (key.value() == "java.vm.version")
			{
				value = "2.1.0";
			}
			else if (key.value() == "java.vm.name")
			{
				value = "Dalvik";
			}
			else if (key.value() == "java.vendor")
			{
				value = "The Android Project";
			}
			else if (key.value() == "os.name")
			{
				value = "Linux";
			}
			if (value == nullptr)
			{
				return uint64_t{ 0 };
			}
			return java.newString(value);
		});
}

// java.lang.Thread: targets that label their signer output with the current
// thread need the object plus its name. One host thread serves the instance,
// so currentThread() always answers the same object.
Result<void> installThread(JavaWorld &world)
{
	auto declared = world.declareClass("java/lang/Thread", "java/lang/Object");
	if (!declared.ok())
	{
		return declared.error();
	}
	const uint64_t klass = declared.value();

	auto element = world.declareClass("java/lang/StackTraceElement", "java/lang/Object");
	if (!element.ok())
	{
		return element.error();
	}
	const uint64_t element_class = element.value();

	// StackTraceElement is a plain value holder; the platform's own trace is
	// empty, but targets resolve (and call) these members on elements they
	// receive from elsewhere.
	uint64_t class_name_field = 0;
	uint64_t method_name_field = 0;
	uint64_t file_name_field = 0;
	uint64_t line_number_field = 0;
	const auto declare_element_field = [&world, element_class](const char *name,
							       const char *signature,
							       uint64_t *slot) -> Result<void> {
		auto field = world.declareField(element_class, name, signature, false);
		if (!field.ok())
		{
			return field.error();
		}
		*slot = field.value();
		return {};
	};
	auto element_field = declare_element_field("className", "Ljava/lang/String;",
		&class_name_field);
	if (!element_field.ok())
	{
		return element_field;
	}
	element_field = declare_element_field("methodName", "Ljava/lang/String;",
		&method_name_field);
	if (!element_field.ok())
	{
		return element_field;
	}
	element_field = declare_element_field("fileName", "Ljava/lang/String;",
		&file_name_field);
	if (!element_field.ok())
	{
		return element_field;
	}
	element_field = declare_element_field("lineNumber", "I", &line_number_field);
	if (!element_field.ok())
	{
		return element_field;
	}
	auto element_result = declare(world, element_class, "<init>", "()V", false,
		[](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 0 }; });
	if (!element_result.ok())
	{
		return element_result;
	}
	const auto string_field_getter = [](uint64_t field) {
		return [field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			return java.readInstanceField(call.receiver, field);
		};
	};
	for (const auto &getter : { std::make_pair("getClassName", class_name_field),
		     std::make_pair("getMethodName", method_name_field),
		     std::make_pair("getFileName", file_name_field) })
	{
		element_result = declare(world, element_class, getter.first,
			"()Ljava/lang/String;", false, string_field_getter(getter.second));
		if (!element_result.ok())
		{
			return element_result;
		}
	}
	element_result = declare(world, element_class, "getLineNumber", "()I", false,
		[line_number_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			return java.readInstanceField(call.receiver, line_number_field);
		});
	if (!element_result.ok())
	{
		return element_result;
	}
	element_result = declare(world, element_class, "toString", "()Ljava/lang/String;", false,
		[class_name_field, method_name_field, file_name_field, line_number_field](
			JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			auto class_name = java.readInstanceField(call.receiver, class_name_field);
			auto method_name = java.readInstanceField(call.receiver, method_name_field);
			auto file_name = java.readInstanceField(call.receiver, file_name_field);
			auto line_number = java.readInstanceField(call.receiver, line_number_field);
			if (!class_name.ok() || !method_name.ok() || !file_name.ok() ||
				!line_number.ok())
			{
				return Error{ ErrorCode::Internal, "StackTraceElement is incomplete" };
			}
			auto text = [&java](uint64_t object) -> std::string {
				if (object == 0)
				{
					return {};
				}
				auto value = java.stringUtf8(object);
				return value.ok() ? value.value() : std::string{};
			};
			const std::string rendered = text(class_name.value()) + "." +
				text(method_name.value()) + "(" + text(file_name.value()) + ":" +
				std::to_string(static_cast<int32_t>(line_number.value())) + ")";
			return java.newString(rendered);
		});
	if (!element_result.ok())
	{
		return element_result;
	}

	auto thread = world.allocateObject(klass);
	if (!thread.ok())
	{
		return thread.error();
	}
	auto result = declare(world, klass, "<init>", "()V", false,
		[](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 0 }; });
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, klass, "currentThread", "()Ljava/lang/Thread;", true,
		[thread](JavaWorld &, InvokeContext &) -> Result<uint64_t> {
			return thread.value();
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, klass, "getName", "()Ljava/lang/String;", false,
		[](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
			return java.newString("main");
		});
	if (!result.ok())
	{
		return result;
	}
	// The host thread has no managed frames, so the honest stack trace is the
	// empty array (the class is declared so the element type is real). Targets
	// use this for hook detection and accept an empty trace.
	return declare(world, klass, "getStackTrace", "()[Ljava/lang/StackTraceElement;",
		false, [element_class](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
			return java.newArray(ArrayElementKind::Object, 0, element_class);
		});
}

// Boolean and Long are value holders like Integer: the native constructs them,
// stores a primitive through <init>, and reads it back through the getter or a
// host runner reads the field via booleanValue()/longValue() below.
Result<void> installBoolean(JavaWorld &world)
{
	auto declared = world.declareClass("java/lang/Boolean", "java/lang/Object");
	if (!declared.ok())
	{
		return declared.error();
	}
	const uint64_t klass = declared.value();
	auto value = world.declareField(klass, "value", "Z", false);
	if (!value.ok())
	{
		return value.error();
	}
	const uint64_t value_field = value.value();

	auto result = declare(world, klass, "<init>", "(Z)V", false,
		[value_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() != 1)
			{
				return Error{ ErrorCode::InvalidArgument, "Boolean.<init> expects one boolean" };
			}
			auto written = java.writeInstanceField(call.receiver, value_field, call.args[0]);
			if (!written.ok())
			{
				return written.error();
			}
			return uint64_t{ 0 };
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, klass, "valueOf", "(Z)Ljava/lang/Boolean;", true,
		[klass, value_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() != 1)
			{
				return Error{ ErrorCode::InvalidArgument, "Boolean.valueOf expects one boolean" };
			}
			auto object = java.allocateObject(klass);
			if (!object.ok())
			{
				return object.error();
			}
			auto written = java.writeInstanceField(object.value(), value_field, call.args[0]);
			if (!written.ok())
			{
				return written.error();
			}
			return object.value();
		});
	if (!result.ok())
	{
		return result;
	}
	return declare(world, klass, "booleanValue", "()Z", false,
		[value_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			return java.readInstanceField(call.receiver, value_field);
		});
}

Result<void> installLong(JavaWorld &world)
{
	auto declared = world.declareClass("java/lang/Long", "java/lang/Number");
	if (!declared.ok())
	{
		return declared.error();
	}
	const uint64_t klass = declared.value();
	auto value = world.declareField(klass, "value", "J", false);
	if (!value.ok())
	{
		return value.error();
	}
	const uint64_t value_field = value.value();

	auto result = declare(world, klass, "<init>", "(J)V", false,
		[value_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() != 1)
			{
				return Error{ ErrorCode::InvalidArgument, "Long.<init> expects one long" };
			}
			auto written = java.writeInstanceField(call.receiver, value_field, call.args[0]);
			if (!written.ok())
			{
				return written.error();
			}
			return uint64_t{ 0 };
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, klass, "valueOf", "(J)Ljava/lang/Long;", true,
		[klass, value_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() != 1)
			{
				return Error{ ErrorCode::InvalidArgument, "Long.valueOf expects one long" };
			}
			auto object = java.allocateObject(klass);
			if (!object.ok())
			{
				return object.error();
			}
			auto written = java.writeInstanceField(object.value(), value_field, call.args[0]);
			if (!written.ok())
			{
				return written.error();
			}
			return object.value();
		});
	if (!result.ok())
	{
		return result;
	}
	return declare(world, klass, "longValue", "()J", false,
		[value_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			return java.readInstanceField(call.receiver, value_field);
		});
}

// java.lang.Number exists so Integer and BigInteger can name their real
// superclass instead of inventing one. No methods: nothing calls them.
Result<void> installNumber(JavaWorld &world)
{
	auto declared = world.declareClass("java/lang/Number", "java/lang/Object");
	if (!declared.ok())
	{
		return declared.error();
	}
	return {};
}

// java.lang.Integer carries an int in the field the real class calls `value`.
// Targets that answer status codes with an Integer only need the constructor;
// host runners read it back through integerValue() below.
Result<void> installInteger(JavaWorld &world)
{
	auto declared = world.declareClass("java/lang/Integer", "java/lang/Number");
	if (!declared.ok())
	{
		return declared.error();
	}
	const uint64_t klass = declared.value();

	auto value = world.declareField(klass, "value", "I", false);
	if (!value.ok())
	{
		return value.error();
	}
	const uint64_t value_field = value.value();

	auto result = declare(world, klass, "<init>", "(I)V", false,
		[value_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() != 1)
			{
				return Error{ ErrorCode::InvalidArgument,
					"Integer.<init> expects one int" };
			}
			auto written =
				java.writeInstanceField(call.receiver, value_field, call.args[0]);
			if (!written.ok())
			{
				return written.error();
			}
			return uint64_t{ 0 };
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, klass, "valueOf", "(I)Ljava/lang/Integer;", true,
		[klass, value_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() != 1)
			{
				return Error{ ErrorCode::InvalidArgument,
					"Integer.valueOf expects one int" };
			}
			auto object = java.allocateObject(klass);
			if (!object.ok())
			{
				return object.error();
			}
			auto written =
				java.writeInstanceField(object.value(), value_field, call.args[0]);
			if (!written.ok())
			{
				return written.error();
			}
			return object.value();
		});
	if (!result.ok())
	{
		return result;
	}
	return declare(world, klass, "intValue", "()I", false,
		[value_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			return java.readInstanceField(call.receiver, value_field);
		});
}

// Float and Double carry the raw IEEE bits in the same `value` field the real
// classes use. SecurityGuard resolves java.lang.Float during JNI_OnLoad and
// reads it back through floatValue().
Result<void> installFloat(JavaWorld &world)
{
	auto declared = world.declareClass("java/lang/Float", "java/lang/Number");
	if (!declared.ok())
	{
		return declared.error();
	}
	const uint64_t klass = declared.value();
	auto value = world.declareField(klass, "value", "F", false);
	if (!value.ok())
	{
		return value.error();
	}
	const uint64_t value_field = value.value();

	auto result = declare(world, klass, "<init>", "(F)V", false,
		[value_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() != 1)
			{
				return Error{ ErrorCode::InvalidArgument, "Float.<init> expects one float" };
			}
			auto written = java.writeInstanceField(call.receiver, value_field, call.args[0]);
			if (!written.ok())
			{
				return written.error();
			}
			return uint64_t{ 0 };
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, klass, "valueOf", "(F)Ljava/lang/Float;", true,
		[klass, value_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() != 1)
			{
				return Error{ ErrorCode::InvalidArgument, "Float.valueOf expects one float" };
			}
			auto object = java.allocateObject(klass);
			if (!object.ok())
			{
				return object.error();
			}
			auto written =
				java.writeInstanceField(object.value(), value_field, call.args[0]);
			if (!written.ok())
			{
				return written.error();
			}
			return object.value();
		});
	if (!result.ok())
	{
		return result;
	}
	return declare(world, klass, "floatValue", "()F", false,
		[value_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			return java.readInstanceField(call.receiver, value_field);
		});
}

Result<void> installDouble(JavaWorld &world)
{
	auto declared = world.declareClass("java/lang/Double", "java/lang/Number");
	if (!declared.ok())
	{
		return declared.error();
	}
	const uint64_t klass = declared.value();
	auto value = world.declareField(klass, "value", "D", false);
	if (!value.ok())
	{
		return value.error();
	}
	const uint64_t value_field = value.value();

	auto result = declare(world, klass, "<init>", "(D)V", false,
		[value_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() != 1)
			{
				return Error{ ErrorCode::InvalidArgument, "Double.<init> expects one double" };
			}
			auto written = java.writeInstanceField(call.receiver, value_field, call.args[0]);
			if (!written.ok())
			{
				return written.error();
			}
			return uint64_t{ 0 };
		});
	if (!result.ok())
	{
		return result;
	}
	result = declare(world, klass, "valueOf", "(D)Ljava/lang/Double;", true,
		[klass, value_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() != 1)
			{
				return Error{ ErrorCode::InvalidArgument, "Double.valueOf expects one double" };
			}
			auto object = java.allocateObject(klass);
			if (!object.ok())
			{
				return object.error();
			}
			auto written =
				java.writeInstanceField(object.value(), value_field, call.args[0]);
			if (!written.ok())
			{
				return written.error();
			}
			return object.value();
		});
	if (!result.ok())
	{
		return result;
	}
	return declare(world, klass, "doubleValue", "()D", false,
		[value_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			return java.readInstanceField(call.receiver, value_field);
		});
}

} // namespace

Result<int32_t> integerValue(JavaWorld &world, uint64_t object)
{
	const uint64_t klass = world.findClass("java/lang/Integer");
	const uint64_t field = klass == 0 ? 0 : world.findField(klass, "value", "I", false);
	if (field == 0)
	{
		return Error{ ErrorCode::NotFound, "java/lang/Integer is not installed" };
	}
	if (!world.isInstanceOf(object, klass))
	{
		return Error{ ErrorCode::InvalidArgument, "the object is not a java.lang.Integer" };
	}
	auto value = world.readInstanceField(object, field);
	if (!value.ok())
	{
		return value.error();
	}
	return static_cast<int32_t>(value.value());
}

Result<bool> booleanValue(JavaWorld &world, uint64_t object)
{
	const uint64_t klass = world.findClass("java/lang/Boolean");
	const uint64_t field = klass == 0 ? 0 : world.findField(klass, "value", "Z", false);
	if (field == 0)
	{
		return Error{ ErrorCode::NotFound, "java/lang/Boolean is not installed" };
	}
	if (!world.isInstanceOf(object, klass))
	{
		return Error{ ErrorCode::InvalidArgument, "the object is not a java.lang.Boolean" };
	}
	auto value = world.readInstanceField(object, field);
	if (!value.ok())
	{
		return value.error();
	}
	return value.value() != 0;
}

Result<uint64_t> longValue(JavaWorld &world, uint64_t object)
{
	const uint64_t klass = world.findClass("java/lang/Long");
	const uint64_t field = klass == 0 ? 0 : world.findField(klass, "value", "J", false);
	if (field == 0)
	{
		return Error{ ErrorCode::NotFound, "java/lang/Long is not installed" };
	}
	if (!world.isInstanceOf(object, klass))
	{
		return Error{ ErrorCode::InvalidArgument, "the object is not a java.lang.Long" };
	}
	return world.readInstanceField(object, field);
}

// Primitive class objects: reflection compares parameter Types against the
// wrappers' TYPE fields, so `int.class` has to be a real Class object whose
// name is "int". The engine models it as a class with a primitive descriptor.
Result<void> installPrimitiveTypes(JavaWorld &world)
{
	const struct
	{
		const char *wrapper;
		const char *descriptor;
	} types[] = {
		{ "java/lang/Boolean", "boolean" },
		{ "java/lang/Byte", "byte" },
		{ "java/lang/Character", "char" },
		{ "java/lang/Short", "short" },
		{ "java/lang/Integer", "int" },
		{ "java/lang/Long", "long" },
		{ "java/lang/Float", "float" },
		{ "java/lang/Double", "double" },
		{ "java/lang/Void", "void" },
	};
	for (const auto &type : types)
	{
		uint64_t wrapper = world.findClass(type.wrapper);
		if (wrapper == 0)
		{
			auto declared = world.declareClass(type.wrapper, "java/lang/Object");
			if (!declared.ok())
			{
				return declared.error();
			}
			wrapper = declared.value();
		}
		auto primitive = world.declareClass(type.descriptor, "");
		if (!primitive.ok())
		{
			return primitive.error();
		}
		auto field = world.declareField(wrapper, "TYPE", "Ljava/lang/Class;", true);
		if (!field.ok())
		{
			return field.error();
		}
		auto written = world.writeStaticField(wrapper, field.value(), primitive.value());
		if (!written.ok())
		{
			return written.error();
		}
	}
	return {};
}

Result<void> installJavaLang(JavaWorld &world)
{
	const auto installers = {
		installObject,
		installClass,
		installReflection,
		installClassLoader,
		installString,
		installSystem,
		installThread,
		installNumber,
		installInteger,
		installBoolean,
		installLong,
		installFloat,
		installDouble,
		installPrimitiveTypes,
	};
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
