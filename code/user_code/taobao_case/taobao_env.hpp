// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/error/result.hpp"

namespace duckemu::runtime::jni
{
class JavaWorld;
}

namespace duckemu::user::taobao
{

// 淘宝 10.68.0 / SecurityGuard 6.8.260901 (com.taobao.taobao).
//
// libsgmainso is AVMP-protected: its Java surface is discovered from the
// reference-device capture in the old DuckDroid case (logs/hook_a.log) and
// from runtime JNI events, not from readable class-name strings.
extern const char *const kNativeClass;
extern const char *const kNativeMethod;
extern const char *const kNativeSignature;

extern const char *const kPackage;
extern const char *const kSdkVersion;

Result<void> installEnvironment(runtime::jni::JavaWorld &world);

// Host-side access to the installed world, used by the runner/plugin to build
// doCommand arguments and read boxed results. One install per instance.
Result<uint64_t> makeString(std::string_view text);
Result<uint64_t> makeIntBox(int32_t value);
Result<uint64_t> makeBooleanBox(bool value);
Result<uint64_t> newContextObject();
Result<uint64_t> newMainPluginObject();
Result<uint64_t> newObjectArray(const std::vector<uint64_t> &elements);

Result<std::string> readJavaString(uint64_t object);
Result<int32_t> readIntegerValue(uint64_t object);
Result<bool> readBooleanValue(uint64_t object);

} // namespace duckemu::user::taobao
