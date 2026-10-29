#include "runtime/jni/world/java_world.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "runtime/memory/address_space.hpp"

namespace duckemu::runtime::jni
{
namespace
{

constexpr uint64_t kArenaChunkSize = 0x10000;
constexpr uint32_t kRecordKindClass = 0;
constexpr uint32_t kRecordKindObject = 1;
constexpr uint32_t kRecordKindMethod = 2;
constexpr uint32_t kRecordKindField = 3;
constexpr uint64_t kRecordSize = 16;

std::string methodKey(std::string_view name, std::string_view signature, bool is_static)
{
	std::string key;
	key.reserve(name.size() + signature.size() + 4);
	key.append(name);
	key.push_back('\n');
	key.append(signature);
	key.push_back('\n');
	key.push_back(is_static ? '1' : '0');
	return key;
}

std::string fieldKey(std::string_view name, std::string_view signature, bool is_static)
{
	return methodKey(name, signature, is_static);
}

// Standard UTF-8 <-> UTF-16. Modified UTF-8 (NUL = C0 80, surrogate pairs) is
// not distinguished; see java_world.hpp.
std::u16string utf8ToUtf16(std::string_view text)
{
	std::u16string out;
	out.reserve(text.size());
	size_t index = 0;
	while (index < text.size())
	{
		const uint8_t first = static_cast<uint8_t>(text[index]);
		uint32_t code = 0;
		size_t extra = 0;
		if (first < 0x80)
		{
			code = first;
		}
		else if ((first & 0xE0) == 0xC0)
		{
			code = first & 0x1F;
			extra = 1;
		}
		else if ((first & 0xF0) == 0xE0)
		{
			code = first & 0x0F;
			extra = 2;
		}
		else if ((first & 0xF8) == 0xF0)
		{
			code = first & 0x07;
			extra = 3;
		}
		else
		{
			// Invalid byte: keep it as a replacement character rather than
			// silently dropping data.
			code = 0xFFFD;
		}
		for (size_t i = 1; i <= extra && index + i < text.size(); ++i)
		{
			code = (code << 6) | (static_cast<uint8_t>(text[index + i]) & 0x3F);
		}
		index += 1 + extra;
		if (code <= 0xFFFF)
		{
			out.push_back(static_cast<char16_t>(code));
		}
		else
		{
			code -= 0x10000;
			out.push_back(static_cast<char16_t>(0xD800 + (code >> 10)));
			out.push_back(static_cast<char16_t>(0xDC00 + (code & 0x3FF)));
		}
	}
	return out;
}

std::string utf16ToUtf8(std::u16string_view text)
{
	std::string out;
	out.reserve(text.size());
	for (size_t index = 0; index < text.size(); ++index)
	{
		uint32_t code = text[index];
		if (code >= 0xD800 && code <= 0xDBFF && index + 1 < text.size())
		{
			const uint32_t low = text[index + 1];
			if (low >= 0xDC00 && low <= 0xDFFF)
			{
				code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
				++index;
			}
		}
		if (code < 0x80)
		{
			out.push_back(static_cast<char>(code));
		}
		else if (code < 0x800)
		{
			out.push_back(static_cast<char>(0xC0 | (code >> 6)));
			out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
		}
		else if (code < 0x10000)
		{
			out.push_back(static_cast<char>(0xE0 | (code >> 12)));
			out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
		}
		else
		{
			out.push_back(static_cast<char>(0xF0 | (code >> 18)));
			out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
		}
	}
	return out;
}

} // namespace

JniArena::JniArena(RuntimeMemory &memory)
	: memory_(memory)
{
}

Result<void> JniArena::initialize()
{
	auto mapped = memory_.map(kArenaChunkSize, PermReadWrite, "jni-arena");
	if (!mapped.ok())
	{
		return mapped.error();
	}
	cursor_ = mapped.value().value();
	end_ = cursor_ + kArenaChunkSize;
	return {};
}

Result<uint64_t> JniArena::allocate(uint64_t size, uint64_t alignment)
{
	if (size == 0)
	{
		size = 8;
	}
	if (alignment == 0)
	{
		alignment = 8;
	}
	uint64_t aligned = (cursor_ + alignment - 1) & ~(alignment - 1);
	if (aligned + size > end_)
	{
		const uint64_t chunk = std::max<uint64_t>(kArenaChunkSize, size);
		auto mapped = memory_.map(chunk, PermReadWrite, "jni-arena");
		if (!mapped.ok())
		{
			return mapped.error();
		}
		cursor_ = mapped.value().value();
		end_ = cursor_ + chunk;
		aligned = (cursor_ + alignment - 1) & ~(alignment - 1);
	}
	cursor_ = aligned + size;
	return aligned;
}

JavaWorld::JavaWorld(RuntimeMemory &memory)
	: memory_(memory)
	, arena_(memory)
{
}

Result<void> JavaWorld::initialize()
{
	auto arena = arena_.initialize();
	if (!arena.ok())
	{
		return arena.error();
	}

	auto class_record = allocateRecord(kRecordSize, kRecordKindClass);
	if (!class_record.ok())
	{
		return class_record.error();
	}
	class_class_ = class_record.value();
	{
		ClassRecord record;
		record.descriptor = "java/lang/Class";
		record.object = class_class_;
		classes_.push_back(std::move(record));
		class_by_object_[class_class_] = 0;
		class_by_descriptor_["java/lang/Class"] = 0;

		ObjectRecord object;
		object.class_object = class_class_;
		object.is_class = true;
		objects_[class_class_] = std::move(object);
	}

	auto object_record = allocateRecord(kRecordSize, kRecordKindClass);
	if (!object_record.ok())
	{
		return object_record.error();
	}
	object_class_ = object_record.value();
	{
		ClassRecord record;
		record.descriptor = "java/lang/Object";
		record.object = object_class_;
		classes_.push_back(std::move(record));
		class_by_object_[object_class_] = 1;
		class_by_descriptor_["java/lang/Object"] = 1;

		ObjectRecord object;
		object.class_object = class_class_;
		object.is_class = true;
		objects_[object_class_] = std::move(object);
	}
	return {};
}

Result<uint64_t> JavaWorld::allocateRecord(uint64_t size, uint32_t kind)
{
	auto address = arena_.allocate(size, 8);
	if (!address.ok())
	{
		return address.error();
	}
	const uint64_t header[2] = {
		(static_cast<uint64_t>(kind) << 32),
		static_cast<uint64_t>(method_by_id_.size() + field_by_id_.size()),
	};
	auto written = memory_.write(GuestVA{ address.value() }, header, sizeof(header));
	if (!written.ok())
	{
		return written.error();
	}
	return address.value();
}

Result<uint64_t> JavaWorld::declareClass(std::string_view descriptor, std::string_view super_descriptor)
{
	const auto existing = class_by_descriptor_.find(std::string(descriptor));
	if (existing != class_by_descriptor_.end())
	{
		ClassRecord &record = classes_[existing->second];
		if (record.super_descriptor != super_descriptor)
		{
			return Error{ ErrorCode::AlreadyExists,
				"class " + std::string(descriptor) + " already declared with a different superclass" };
		}
		return record.object;
	}

	if (descriptor == "java/lang/Object" || descriptor == "java/lang/Class")
	{
		return Error{ ErrorCode::AlreadyExists,
			"bootstrap class " + std::string(descriptor) + " is already declared" };
	}
	if (!super_descriptor.empty() &&
		class_by_descriptor_.find(std::string(super_descriptor)) == class_by_descriptor_.end())
	{
		return Error{ ErrorCode::NotFound,
			"superclass " + std::string(super_descriptor) + " is not declared" };
	}

	auto record_address = allocateRecord(kRecordSize, kRecordKindClass);
	if (!record_address.ok())
	{
		return record_address.error();
	}

	ClassRecord record;
	record.descriptor = std::string(descriptor);
	record.object = record_address.value();
	record.super_descriptor = std::string(super_descriptor);

	ObjectRecord object;
	object.class_object = class_class_;
	object.is_class = true;
	objects_[record.object] = std::move(object);

	const size_t index = classes_.size();
	classes_.push_back(std::move(record));
	class_by_object_[classes_.back().object] = index;
	class_by_descriptor_[std::string(descriptor)] = index;
	if (descriptor == "java/lang/String")
	{
		string_class_ = classes_.back().object;
	}
	return classes_.back().object;
}

Result<uint64_t> JavaWorld::declareMethod(uint64_t class_object,
	std::string_view name,
	std::string_view signature,
	bool is_static,
	ShimHandler shim)
{
	ClassRecord *klass = classRecord(class_object);
	if (klass == nullptr)
	{
		return Error{ ErrorCode::NotFound, "method declared on an unknown class" };
	}
	MethodSignature parsed;
	if (!parseMethodSignature(signature, &parsed))
	{
		return Error{ ErrorCode::InvalidArgument,
			"malformed method signature " + std::string(signature) };
	}
	const std::string key = methodKey(name, signature, is_static);
	if (klass->methods.find(key) != klass->methods.end())
	{
		return Error{ ErrorCode::AlreadyExists,
			"method " + std::string(name) + std::string(signature) + " is already declared" };
	}
	auto id_address = allocateRecord(kRecordSize, kRecordKindMethod);
	if (!id_address.ok())
	{
		return id_address.error();
	}

	MethodRecord record;
	record.id = id_address.value();
	record.class_object = class_object;
	record.name = std::string(name);
	record.signature = std::string(signature);
	record.parsed = std::move(parsed);
	record.is_static = is_static;
	record.shim = std::move(shim);

	const size_t index = methods_.size();
	methods_.push_back(std::move(record));
	method_by_id_[methods_.back().id] = index;
	klass->methods[key] = index;
	return methods_.back().id;
}

Result<uint64_t> JavaWorld::declareField(uint64_t class_object,
	std::string_view name,
	std::string_view signature,
	bool is_static)
{
	ClassRecord *klass = classRecord(class_object);
	if (klass == nullptr)
	{
		return Error{ ErrorCode::NotFound, "field declared on an unknown class" };
	}
	bool valid = false;
	const ValueKind kind = signature.empty() ? ValueKind::Void : valueKindOf(signature.front(), &valid);
	if (signature.size() != 1 || !valid || kind == ValueKind::Void)
	{
		return Error{ ErrorCode::InvalidArgument,
			"unsupported field signature " + std::string(signature) };
	}
	const std::string key = fieldKey(name, signature, is_static);
	if (klass->fields.find(key) != klass->fields.end())
	{
		return Error{ ErrorCode::AlreadyExists,
			"field " + std::string(name) + " is already declared" };
	}
	auto id_address = allocateRecord(kRecordSize, kRecordKindField);
	if (!id_address.ok())
	{
		return id_address.error();
	}

	FieldRecord record;
	record.id = id_address.value();
	record.class_object = class_object;
	record.name = std::string(name);
	record.signature = std::string(signature);
	record.kind = kind;
	record.is_static = is_static;

	const size_t index = fields_.size();
	fields_.push_back(std::move(record));
	field_by_id_[fields_.back().id] = index;
	klass->fields[key] = index;
	return fields_.back().id;
}

uint64_t JavaWorld::findClass(std::string_view descriptor) const
{
	const auto found = class_by_descriptor_.find(std::string(descriptor));
	return found == class_by_descriptor_.end() ? 0 : classes_[found->second].object;
}

uint64_t JavaWorld::findMethod(uint64_t class_object,
	std::string_view name,
	std::string_view signature,
	bool is_static) const
{
	const std::string key = methodKey(name, signature, is_static);
	for (uint64_t current = class_object; current != 0;)
	{
		const ClassRecord *klass = classRecord(current);
		if (klass == nullptr)
		{
			return 0;
		}
		const auto found = klass->methods.find(key);
		if (found != klass->methods.end())
		{
			return methods_[found->second].id;
		}
		current = findClass(klass->super_descriptor);
	}
	return 0;
}

uint64_t JavaWorld::findField(uint64_t class_object,
	std::string_view name,
	std::string_view signature,
	bool is_static) const
{
	const std::string key = fieldKey(name, signature, is_static);
	for (uint64_t current = class_object; current != 0;)
	{
		const ClassRecord *klass = classRecord(current);
		if (klass == nullptr)
		{
			return 0;
		}
		const auto found = klass->fields.find(key);
		if (found != klass->fields.end())
		{
			return fields_[found->second].id;
		}
		current = findClass(klass->super_descriptor);
	}
	return 0;
}

const MethodRecord *JavaWorld::method(uint64_t method_id) const
{
	const auto found = method_by_id_.find(method_id);
	return found == method_by_id_.end() ? nullptr : &methods_[found->second];
}

const FieldRecord *JavaWorld::field(uint64_t field_id) const
{
	const auto found = field_by_id_.find(field_id);
	return found == field_by_id_.end() ? nullptr : &fields_[found->second];
}

std::string_view JavaWorld::descriptorOfClass(uint64_t class_object) const
{
	const ClassRecord *klass = classRecord(class_object);
	return klass == nullptr ? std::string_view{} : std::string_view(klass->descriptor);
}

uint64_t JavaWorld::classOf(uint64_t object) const
{
	const ObjectRecord *record = objectRecord(object);
	return record == nullptr ? 0 : record->class_object;
}

bool JavaWorld::isClassObject(uint64_t object) const
{
	return object != 0 && classOf(object) == class_class_;
}

bool JavaWorld::isInstanceOf(uint64_t object, uint64_t class_object) const
{
	const uint64_t actual = classOf(object);
	return actual != 0 && isAssignableFrom(class_object, actual);
}

uint64_t JavaWorld::superclassOf(uint64_t class_object) const
{
	const ClassRecord *klass = classRecord(class_object);
	if (klass == nullptr || klass->super_descriptor.empty())
	{
		return 0;
	}
	return findClass(klass->super_descriptor);
}

bool JavaWorld::isAssignableFrom(uint64_t class_object, uint64_t source_class) const
{
	for (uint64_t current = source_class; current != 0;)
	{
		if (current == class_object)
		{
			return true;
		}
		current = superclassOf(current);
	}
	return false;
}

Result<uint64_t> JavaWorld::allocateObject(uint64_t class_object)
{
	if (classRecord(class_object) == nullptr)
	{
		return Error{ ErrorCode::NotFound, "object allocated for an unknown class" };
	}
	auto address = allocateRecord(kRecordSize, kRecordKindObject);
	if (!address.ok())
	{
		return address.error();
	}
	ObjectRecord record;
	record.class_object = class_object;
	objects_[address.value()] = std::move(record);
	return address.value();
}

Result<uint64_t> JavaWorld::readInstanceField(uint64_t object, uint64_t field_id) const
{
	const FieldRecord *record = field(field_id);
	if (record == nullptr || record->is_static)
	{
		return Error{ ErrorCode::NotFound, "instance field is not declared" };
	}
	const ObjectRecord *target = objectRecord(object);
	if (target == nullptr)
	{
		return Error{ ErrorCode::InvalidArgument, "field read on an unknown object" };
	}
	const auto found = target->fields.find(field_id);
	return found == target->fields.end() ? 0 : found->second;
}

Result<void> JavaWorld::writeInstanceField(uint64_t object, uint64_t field_id, uint64_t value)
{
	const FieldRecord *record = field(field_id);
	if (record == nullptr || record->is_static)
	{
		return Error{ ErrorCode::NotFound, "instance field is not declared" };
	}
	ObjectRecord *target = objectRecord(object);
	if (target == nullptr)
	{
		return Error{ ErrorCode::InvalidArgument, "field write on an unknown object" };
	}
	target->fields[field_id] = value;
	return {};
}

Result<uint64_t> JavaWorld::readStaticField(uint64_t, uint64_t field_id) const
{
	const FieldRecord *record = field(field_id);
	if (record == nullptr || !record->is_static)
	{
		return Error{ ErrorCode::NotFound, "static field is not declared" };
	}
	const ClassRecord *owner = classRecord(record->class_object);
	if (owner == nullptr)
	{
		return Error{ ErrorCode::Internal, "static field owner disappeared" };
	}
	const auto found = owner->static_values.find(field_id);
	return found == owner->static_values.end() ? 0 : found->second;
}

Result<void> JavaWorld::writeStaticField(uint64_t, uint64_t field_id, uint64_t value)
{
	const FieldRecord *record = field(field_id);
	if (record == nullptr || !record->is_static)
	{
		return Error{ ErrorCode::NotFound, "static field is not declared" };
	}
	ClassRecord *owner = classRecord(record->class_object);
	if (owner == nullptr)
	{
		return Error{ ErrorCode::Internal, "static field owner disappeared" };
	}
	owner->static_values[field_id] = value;
	return {};
}

Result<uint64_t> JavaWorld::newString(std::string_view utf8)
{
	if (string_class_ == 0)
	{
		return Error{ ErrorCode::NotFound, "java/lang/String is not declared" };
	}
	auto object = allocateObject(string_class_);
	if (!object.ok())
	{
		return object.error();
	}
	strings_[object.value()] = utf8ToUtf16(utf8);
	return object.value();
}

Result<std::string> JavaWorld::stringUtf8(uint64_t object) const
{
	const auto found = strings_.find(object);
	if (found == strings_.end())
	{
		return Error{ ErrorCode::InvalidArgument, "object is not a string" };
	}
	return utf16ToUtf8(found->second);
}

Result<uint64_t> JavaWorld::stringLength(uint64_t object) const
{
	const auto found = strings_.find(object);
	if (found == strings_.end())
	{
		return Error{ ErrorCode::InvalidArgument, "object is not a string" };
	}
	return found->second.size();
}

Result<uint64_t> JavaWorld::stringCharAt(uint64_t object, uint64_t index) const
{
	const auto found = strings_.find(object);
	if (found == strings_.end())
	{
		return Error{ ErrorCode::InvalidArgument, "object is not a string" };
	}
	if (index >= found->second.size())
	{
		return Error{ ErrorCode::InvalidArgument, "string index out of range" };
	}
	return found->second[index];
}

Result<bool> JavaWorld::stringEquals(uint64_t first, uint64_t second) const
{
	const auto first_text = strings_.find(first);
	if (first_text == strings_.end())
	{
		return Error{ ErrorCode::InvalidArgument, "left operand is not a string" };
	}
	if (first == second)
	{
		return true;
	}
	const auto second_text = strings_.find(second);
	if (second_text == strings_.end())
	{
		return false;
	}
	return first_text->second == second_text->second;
}

Result<void> JavaWorld::registerNative(uint64_t class_object,
	std::string_view name,
	std::string_view signature,
	uint64_t address)
{
	ClassRecord *klass = classRecord(class_object);
	if (klass == nullptr)
	{
		return Error{ ErrorCode::NotFound, "RegisterNatives on an unknown class" };
	}
	const std::string static_key = methodKey(name, signature, true);
	const std::string instance_key = methodKey(name, signature, false);
	auto found = klass->methods.find(static_key);
	if (found == klass->methods.end())
	{
		found = klass->methods.find(instance_key);
	}
	if (found == klass->methods.end())
	{
		return Error{ ErrorCode::NotFound,
			"native method " + std::string(name) + std::string(signature) +
				" is not declared by the environment" };
	}
	MethodRecord &record = methods_[found->second];
	if (record.shim)
	{
		return Error{ ErrorCode::AlreadyExists,
			"method " + std::string(name) + " already has a host implementation" };
	}
	record.native_address = address;
	return {};
}

const JavaWorld::ClassRecord *JavaWorld::classRecord(uint64_t class_object) const
{
	const auto found = class_by_object_.find(class_object);
	return found == class_by_object_.end() ? nullptr : &classes_[found->second];
}

JavaWorld::ClassRecord *JavaWorld::classRecord(uint64_t class_object)
{
	const auto found = class_by_object_.find(class_object);
	return found == class_by_object_.end() ? nullptr : &classes_[found->second];
}

const JavaWorld::ObjectRecord *JavaWorld::objectRecord(uint64_t object) const
{
	const auto found = objects_.find(object);
	return found == objects_.end() ? nullptr : &found->second;
}

JavaWorld::ObjectRecord *JavaWorld::objectRecord(uint64_t object)
{
	const auto found = objects_.find(object);
	return found == objects_.end() ? nullptr : &found->second;
}

} // namespace duckemu::runtime::jni
