// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace duckemu::runtime::jni
{

// Values that cross the JNI boundary are guest addresses or plain integers;
// host pointers never enter guest memory. A null reference is 0.
using JniHandle = uint64_t;
using JniMethodId = uint64_t;
using JniFieldId = uint64_t;

inline constexpr int32_t kJniOk = 0;
inline constexpr int32_t kJniErr = -1;
inline constexpr int32_t kJniEDetached = -2;
inline constexpr int32_t kJniEVersion = -3;
inline constexpr int32_t kJniENoMem = -4;
inline constexpr int32_t kJniEEXist = -5;
inline constexpr int32_t kJniEInval = -6;

inline constexpr int32_t kJniVersion1_1 = 0x00010001;
inline constexpr int32_t kJniVersion1_2 = 0x00010002;
inline constexpr int32_t kJniVersion1_4 = 0x00010004;
inline constexpr int32_t kJniVersion1_6 = 0x00010006;

enum class ValueKind : uint8_t
{
	Void = 0,
	Boolean,
	Byte,
	Char,
	Short,
	Int,
	Long,
	Float,
	Double,
	Object,
};

// Java type descriptor -> ValueKind. Object descriptors (L...;) and arrays map
// to Object; an array descriptor is not sized anywhere in M5, so callers must
// treat Object arrays as unsupported until arrays land.
ValueKind valueKindOf(char descriptor, bool *ok);

struct MethodSignature
{
	ValueKind result = ValueKind::Void;
	std::vector<ValueKind> args;
};

// Parses "(...)<ret>". Returns false on a malformed descriptor.
bool parseMethodSignature(std::string_view signature, MethodSignature *out);

// Abstract JVM descriptor -> dotted class name (java/lang/String -> java.lang.String).
std::string descriptorToClassName(std::string_view descriptor);

} // namespace duckemu::runtime::jni
