// Unit tests for the strict Java world and the java.lang environment: no guest
// code, no engine. Declaration, lookup, strings, fields, native binding.

#include <cstdint>

#include "runtime/clock/runtime_clock.hpp"
#include "runtime/jni/envs/envs.hpp"
#include "runtime/jni/jni_types.hpp"
#include "runtime/jni/world/java_world.hpp"
#include "runtime/memory/address_space.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime;
using namespace duckemu::runtime::jni;

namespace
{

uint64_t addShim(JavaWorld &, InvokeContext &call)
{
	return call.args[0] + call.args[1];
}

} // namespace

int main()
{
	RuntimeMemory memory;
	RuntimeClock clock;
	JavaWorld world(memory, clock);
	CHECK_RESULT(world.initialize());

	auto java_lang = envs::installJavaLang(world);
	CHECK_RESULT(java_lang);

	// Bootstrap classes exist; unknown classes do not (and are not created).
	CHECK(world.findClass("java/lang/Object") != 0);
	CHECK(world.findClass("java/lang/String") != 0);
	CHECK(world.findClass("java/lang/Class") != 0);
	CHECK_EQ(world.findClass("does/not/Exist"), 0);
	CHECK_EQ(world.findClass("does/not/Exist"), 0);

	// java.lang.System uses the instance clock: injected time advances 1 ms
	// per read, so a replay is deterministic.
	const uint64_t system_class = world.findClass("java/lang/System");
	CHECK(system_class != 0);
	const uint64_t millis_id = world.findMethod(system_class, "currentTimeMillis", "()J", true);
	const uint64_t nanotime_id = world.findMethod(system_class, "nanoTime", "()J", true);
	CHECK(millis_id != 0);
	CHECK(nanotime_id != 0);
	const MethodRecord *millis = world.method(millis_id);
	const MethodRecord *nanotime = world.method(nanotime_id);
	CHECK(millis != nullptr && millis->shim);
	CHECK(nanotime != nullptr && nanotime->shim);
	if (millis != nullptr && millis->shim && nanotime != nullptr && nanotime->shim)
	{
		InvokeContext time_call;
		time_call.receiver = system_class;

		clock.setWallTimeNs(1792886400000000000ULL);
		auto first = millis->shim(world, time_call);
		auto second = millis->shim(world, time_call);
		CHECK_RESULT(first);
		CHECK_RESULT(second);
		if (first.ok() && second.ok())
		{
			CHECK_EQ(first.value(), 1792886400000ULL);
			CHECK_EQ(second.value(), 1792886400001ULL);
		}

		clock.setMonotonicTimeNs(5000000ULL);
		auto nano_first = nanotime->shim(world, time_call);
		auto nano_second = nanotime->shim(world, time_call);
		CHECK_RESULT(nano_first);
		CHECK_RESULT(nano_second);
		if (nano_first.ok() && nano_second.ok())
		{
			CHECK_EQ(nano_first.value(), 5000000ULL);
			CHECK_EQ(nano_second.value(), 6000000ULL);
		}
	}

	// Method lookup walks the superclass chain but never creates.
	const uint64_t object = world.findClass("java/lang/Object");
	const uint64_t string = world.findClass("java/lang/String");
	CHECK(world.findMethod(string, "length", "()I", false) != 0);
	CHECK_EQ(world.findMethod(string, "noSuchMethod", "()I", false), 0);
	CHECK_EQ(world.findMethod(string, "noSuchMethod", "()I", false), 0);

	const uint64_t derived = world.declareClass("test/Derived", "java/lang/Object").value();
	const uint64_t base = world.declareClass("test/Base", "java/lang/Object").value();
	(void)derived;
	auto base_method = world.declareMethod(base, "value", "()I", true,
		[](JavaWorld &, InvokeContext &) -> Result<uint64_t> {
			return uint64_t{ 7 };
		});
	CHECK_RESULT(base_method);
	const uint64_t derived2 = world.declareClass("test/Derived2", "test/Base").value();
	CHECK(world.findMethod(derived2, "value", "()I", true) == base_method.value());

	// Conflicting redeclaration is an error; the same declaration is idempotent.
	CHECK(!world.declareClass("test/Base", "java/lang/String").ok());
	CHECK(world.declareClass("test/Base", "java/lang/Object").ok());
	// An explicitly unknown hierarchy is allowed; nothing is invented.
	CHECK(world.declareClass("test/Lone", "").ok());

	// Methods and fields.
	auto add = world.declareMethod(object, "testAdd", "(II)I", true,
		addShim);
	CHECK_RESULT(add);
	const MethodRecord *add_record = world.method(add.value());
	CHECK(add_record != nullptr);
	if (add_record != nullptr)
	{
		CHECK(add_record->parsed.result == ValueKind::Int);
		CHECK_EQ(add_record->parsed.args.size(), 2);
		InvokeContext call;
		call.args = { 20, 22 };
		auto result = add_record->shim(world, call);
		CHECK_RESULT(result);
		if (result.ok())
		{
			CHECK_EQ(result.value(), 42);
		}
	}
	CHECK_EQ(world.findMethod(object, "testAdd", "(II)I", true), add.value());
	CHECK_EQ(world.findMethod(object, "testAdd", "(II)I", false), 0);

	auto field = world.declareField(object, "testCounter", "I", true);
	CHECK_RESULT(field);
	CHECK(world.findField(object, "testCounter", "I", true) == field.value());
	CHECK_EQ(world.findField(object, "testCounter", "I", false), 0);
	CHECK_RESULT(world.writeStaticField(object, field.value(), 11));
	auto value = world.readStaticField(object, field.value());
	CHECK_RESULT(value);
	if (value.ok())
	{
		CHECK_EQ(value.value(), 11);
	}

	// Objects and instance fields.
	auto box = world.declareClass("test/Box", "java/lang/Object").value();
	auto instance_field = world.declareField(box, "value", "I", false);
	CHECK_RESULT(instance_field);
	auto object_address = world.allocateObject(box);
	CHECK_RESULT(object_address);
	CHECK_EQ(world.classOf(object_address.value()), box);
	CHECK_RESULT(world.writeInstanceField(object_address.value(), instance_field.value(), 9));
	auto instance_value = world.readInstanceField(object_address.value(), instance_field.value());
	CHECK_RESULT(instance_value);
	if (instance_value.ok())
	{
		CHECK_EQ(instance_value.value(), 9);
	}

	// Strings: UTF-8 in, UTF-16 semantics inside, UTF-8 out.
	auto text = world.newString("h\xC3\xA9llo");
	CHECK_RESULT(text);
	if (text.ok())
	{
		auto decoded = world.stringUtf8(text.value());
		CHECK_RESULT(decoded);
		if (decoded.ok())
		{
			CHECK(decoded.value() == "h\xC3\xA9llo");
		}
		auto length = world.stringLength(text.value());
		CHECK_RESULT(length);
		if (length.ok())
		{
			CHECK_EQ(length.value(), 5);
		}
		auto character = world.stringCharAt(text.value(), 1);
		CHECK_RESULT(character);
		if (character.ok())
		{
			CHECK_EQ(character.value(), 0xE9);
		}
	}

	// RegisterNatives binds only to declared methods.
	CHECK(!world.registerNative(box, "noSuchNative", "()I", 0x1234).ok());
	auto native = world.declareMethod(box, "nativeOne", "()I", true, {});
	CHECK_RESULT(native);
	CHECK_RESULT(world.registerNative(box, "nativeOne", "()I", 0x1234));
	const MethodRecord *native_record = world.method(native.value());
	CHECK(native_record != nullptr && native_record->native_address == 0x1234);
	CHECK(!world.registerNative(box, "testAdd", "(II)I", 0x1234).ok());
	CHECK_EQ(world.findMethod(object, "nativeOne", "()I", true), 0);

	// Signature parsing.
	MethodSignature parsed;
	CHECK(parseMethodSignature("(ILjava/lang/String;[B)J", &parsed));
	CHECK(parsed.result == ValueKind::Long);
	CHECK_EQ(parsed.args.size(), 3);
	CHECK(parsed.args[0] == ValueKind::Int);
	CHECK(parsed.args[1] == ValueKind::Object);
	CHECK(parsed.args[2] == ValueKind::Object);
	CHECK(!parseMethodSignature("(I", &parsed));
	CHECK(!parseMethodSignature("I)V", &parsed));

	return test::finish("jni_world_test");
}
