// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace duckemu::runtime::observer
{

enum class JniEventKind : uint8_t
{
	MissingClass = 0,
	MissingMethod,
	MissingField,
	UnsupportedFunction,
};

constexpr std::string_view jniEventKindName(JniEventKind kind) noexcept
{
	switch (kind)
	{
	case JniEventKind::MissingClass:
		return "missing-class";
	case JniEventKind::MissingMethod:
		return "missing-method";
	case JniEventKind::MissingField:
		return "missing-field";
	case JniEventKind::UnsupportedFunction:
		return "unsupported-function";
	}
	return "?";
}

// One strict-mode failure. The JNI call itself returns null/JNI_ERR (the real
// JNI contract); this event makes the failure observable instead of silent.
// `slot` is set for UnsupportedFunction, `name`/`signature` for methods and
// fields, `class_descriptor` for every kind that has a class.
struct JniEvent
{
	uint64_t id = 0;
	JniEventKind kind = JniEventKind::MissingClass;
	std::string class_descriptor;
	std::string name;
	std::string signature;
	uint32_t slot = 0;
	uint32_t tid = 0;
};

} // namespace duckemu::runtime::observer
