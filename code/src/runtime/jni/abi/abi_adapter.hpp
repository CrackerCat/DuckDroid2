#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "common/error/result.hpp"
#include "runtime/jni/jni_types.hpp"

namespace duckemu::runtime
{
class RuntimeMemory;
}

namespace duckemu::runtime::jni
{

enum class CallVariant : uint8_t
{
	// Call<Type>Method(env, receiver, methodID, ...): the first variadic
	// argument is in x3.
	Plain,
	// Call<Type>MethodV: va_list in x3. Integer arguments are consumed through
	// the AAPCS64 va_list cursors; floating-point va_list arguments are not
	// implemented.
	VaList,
	// Call<Type>MethodA(env, receiver, methodID, const jvalue*): x3 is a pointer.
	Array,
};

// Reads the call arguments in declaration order. Object arguments stay JNI
// handles; the caller resolves them to object addresses. Unsupported shapes
// (guest stack arguments, va_list) return ErrorCode::Unsupported instead of
// guessing a value.
Result<std::vector<uint64_t> > readCallArguments(const MethodSignature &signature,
	const std::array<uint64_t, 8> &registers,
	CallVariant variant,
	RuntimeMemory &memory);

} // namespace duckemu::runtime::jni
