#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/error/result.hpp"
#include "runtime/jni/jni_types.hpp"

namespace duckemu::runtime
{
class RuntimeMemory;
class RuntimeClock;
}

namespace duckemu::runtime::jni
{

class JavaWorld;

// Arguments of one managed (Java) method invocation. Object arguments are
// object addresses, not JNI handles; the JNI layer resolves handles first.
struct InvokeContext
{
	uint64_t receiver = 0;
	std::vector<uint64_t> args;
	uint32_t tid = 0;
};

// A host implementation of a Java method declared by an environment module.
// Returns a primitive value, or an object address for reference results.
using ShimHandler = std::function<Result<uint64_t>(JavaWorld &, InvokeContext &)>;

// Element kind of a Java array. Object arrays carry the element class in
// ArrayView::element_class; primitive arrays never use it.
enum class ArrayElementKind : uint8_t
{
	Boolean = 0,
	Byte,
	Char,
	Short,
	Int,
	Long,
	Float,
	Double,
	Object,
};

// Size in bytes of one primitive array element; 8 for object arrays (the
// element is a guest object address). Void is not an array kind.
uint64_t arrayElementSize(ArrayElementKind kind);

struct ArrayView
{
	ArrayElementKind kind = ArrayElementKind::Object;
	uint64_t length = 0;
	uint64_t storage = 0;
	uint64_t element_class = 0;
};

struct MethodRecord
{
	uint64_t id = 0;
	uint64_t class_object = 0;
	std::string name;
	std::string signature;
	MethodSignature parsed;
	bool is_static = false;
	ShimHandler shim;
	// Non-zero once RegisterNatives (or Java_* binding, later) attached a guest
	// implementation. A method with neither is a declaration without a body.
	uint64_t native_address = 0;
};

struct FieldRecord
{
	uint64_t id = 0;
	uint64_t class_object = 0;
	std::string name;
	std::string signature;
	ValueKind kind = ValueKind::Object;
	bool is_static = false;
};

// Bump allocator for guest-visible JNI metadata (handles, method records,
// UTF-8 buffers). The host metadata tables are keyed by these guest addresses;
// no host pointer is ever written into guest memory.
class JniArena
{
    public:
	explicit JniArena(RuntimeMemory &memory);

	Result<void> initialize();
	Result<uint64_t> allocate(uint64_t size, uint64_t alignment = 8);

    private:
	RuntimeMemory &memory_;
	uint64_t cursor_ = 0;
	uint64_t end_ = 0;
};

// The per-instance Java object model. Classes, methods and fields exist only
// if an environment module declared them (or RegisterNatives explicitly bound
// a native method). Lookups never create anything: unknown means 0, and the
// JNI layer turns that into null plus a JniEvent.
class JavaWorld
{
    public:
	explicit JavaWorld(RuntimeMemory &memory, RuntimeClock &clock);

	// Bootstraps java/lang/Class and java/lang/Object. Must run before any
	// environment module is installed.
	Result<void> initialize();

	// -- Declaration (environment modules only) -----------------------------
	// Idempotent while the superclass matches; a conflicting declaration is an
	// error. An empty super means "no superclass": use it when the real
	// hierarchy is unknown rather than inventing one.
	Result<uint64_t> declareClass(std::string_view descriptor, std::string_view super_descriptor);
	Result<uint64_t> declareMethod(uint64_t class_object,
		std::string_view name,
		std::string_view signature,
		bool is_static,
		ShimHandler shim);
	Result<uint64_t> declareField(uint64_t class_object,
		std::string_view name,
		std::string_view signature,
		bool is_static);

	// -- Strict resolution (JNI) --------------------------------------------
	// Returns the class object address, or 0 when the descriptor was never
	// declared.
	[[nodiscard]] uint64_t findClass(std::string_view descriptor) const;
	// Returns the method id, or 0. Searches the class and its superclasses.
	[[nodiscard]] uint64_t findMethod(uint64_t class_object,
		std::string_view name,
		std::string_view signature,
		bool is_static) const;
	[[nodiscard]] uint64_t findField(uint64_t class_object,
		std::string_view name,
		std::string_view signature,
		bool is_static) const;

	[[nodiscard]] const MethodRecord *method(uint64_t method_id) const;
	[[nodiscard]] const FieldRecord *field(uint64_t field_id) const;
	[[nodiscard]] std::string_view descriptorOfClass(uint64_t class_object) const;
	[[nodiscard]] uint64_t classOf(uint64_t object) const;
	// True when the address is a class object (its class is java/lang/Class).
	[[nodiscard]] bool isClassObject(uint64_t object) const;
	[[nodiscard]] bool isInstanceOf(uint64_t object, uint64_t class_object) const;
	[[nodiscard]] uint64_t superclassOf(uint64_t class_object) const;
	[[nodiscard]] bool isAssignableFrom(uint64_t class_object, uint64_t source_class) const;

	// -- Objects and fields --------------------------------------------------
	Result<uint64_t> allocateObject(uint64_t class_object);
	Result<uint64_t> readInstanceField(uint64_t object, uint64_t field_id) const;
	Result<void> writeInstanceField(uint64_t object, uint64_t field_id, uint64_t value);
	Result<uint64_t> readStaticField(uint64_t class_object, uint64_t field_id) const;
	Result<void> writeStaticField(uint64_t class_object, uint64_t field_id, uint64_t value);

	// -- Arrays --------------------------------------------------------------
	// Arrays are values, not declared types: their class ("[B",
	// "[Ljava/lang/Object;") is created on demand and is deliberately not
	// reachable through FindClass. Element storage lives in the JNI arena.
	// |initial_element| pre-fills object arrays; 0 leaves them null.
	Result<uint64_t> newArray(ArrayElementKind kind,
		uint64_t length,
		uint64_t element_class = 0,
		uint64_t initial_element = 0);
	Result<ArrayView> arrayView(uint64_t object) const;
	Result<uint64_t> readArrayElement(uint64_t object, uint64_t index) const;
	Result<void> writeArrayElement(uint64_t object, uint64_t index, uint64_t value);

	// -- Strings -------------------------------------------------------------
	// M5 stores UTF-16 and converts through standard UTF-8. Modified UTF-8
	// (NUL as C0 80, surrogate pairs) is a known limitation; the current
	// consumers are ASCII-only.
	Result<uint64_t> newString(std::string_view utf8);
	// Attaches UTF-8 contents to an object that was allocated as java/lang/String
	// by NewObject, before its constructor shim runs.
	Result<void> setStringContent(uint64_t object, std::string_view utf8);
	Result<std::string> stringUtf8(uint64_t object) const;
	Result<uint64_t> stringLength(uint64_t object) const;
	// UTF-16 code unit at |index| (String.charAt).
	Result<uint64_t> stringCharAt(uint64_t object, uint64_t index) const;
	// True when both objects are strings with identical UTF-16 contents.
	Result<bool> stringEquals(uint64_t first, uint64_t second) const;

	// -- Native methods ------------------------------------------------------
	// Explicitly binds a guest implementation. Creates the method entry when the
	// environment did not declare it (RegisterNatives is the declaration).
	Result<void> registerNative(uint64_t class_object,
		std::string_view name,
		std::string_view signature,
		uint64_t address);

	// -- Pending exception ---------------------------------------------------
	[[nodiscard]] uint64_t pendingException() const
	{
		return pending_exception_;
	}
	void setPendingException(uint64_t object)
	{
		pending_exception_ = object;
	}
	void clearPendingException()
	{
		pending_exception_ = 0;
	}

	[[nodiscard]] JniArena &arena()
	{
		return arena_;
	}
	[[nodiscard]] RuntimeMemory &memory()
	{
		return memory_;
	}
	// Shared with the syscall layer: java.lang.System and clock_gettime must
	// observe the same injected/real clock.
	[[nodiscard]] RuntimeClock &clock()
	{
		return clock_;
	}
	[[nodiscard]] uint64_t classClass() const
	{
		return class_class_;
	}
	[[nodiscard]] uint64_t objectClass() const
	{
		return object_class_;
	}
	[[nodiscard]] uint64_t stringClass() const
	{
		return string_class_;
	}

    private:
	struct ClassRecord
	{
		std::string descriptor;
		uint64_t object = 0;
		std::string super_descriptor;
		std::unordered_map<std::string, size_t> methods; // name \n signature \n static
		std::unordered_map<std::string, size_t> fields;
		std::unordered_map<uint64_t, uint64_t> static_values; // field id -> value
	};

	struct ObjectRecord
	{
		uint64_t class_object = 0;
		bool is_class = false;
		std::unordered_map<uint64_t, uint64_t> fields; // field id -> value
		// Array payload; array_storage is a guest address in the JNI arena.
		bool is_array = false;
		ArrayElementKind array_kind = ArrayElementKind::Object;
		uint64_t array_length = 0;
		uint64_t array_storage = 0;
		uint64_t array_element_class = 0;
	};

	[[nodiscard]] const ClassRecord *classRecord(uint64_t class_object) const;
	[[nodiscard]] ClassRecord *classRecord(uint64_t class_object);
	[[nodiscard]] const ObjectRecord *objectRecord(uint64_t object) const;
	[[nodiscard]] ObjectRecord *objectRecord(uint64_t object);
	Result<uint64_t> allocateRecord(uint64_t size, uint32_t kind);

	RuntimeMemory &memory_;
	RuntimeClock &clock_;
	JniArena arena_;
	std::vector<ClassRecord> classes_;
	std::unordered_map<uint64_t, size_t> class_by_object_;
	std::unordered_map<std::string, size_t> class_by_descriptor_;
	std::unordered_map<uint64_t, ObjectRecord> objects_;
	std::vector<MethodRecord> methods_;
	std::unordered_map<uint64_t, size_t> method_by_id_;
	std::vector<FieldRecord> fields_;
	std::unordered_map<uint64_t, size_t> field_by_id_;
	std::unordered_map<uint64_t, std::u16string> strings_;
	uint64_t class_class_ = 0;
	uint64_t object_class_ = 0;
	uint64_t string_class_ = 0;
	uint64_t pending_exception_ = 0;
};

} // namespace duckemu::runtime::jni
