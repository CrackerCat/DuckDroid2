// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "common/error/result.hpp"

namespace duckemu::runtime
{
class RuntimeInstance;
}

namespace duckemu::runtime::jni
{
class JavaWorld;
}

namespace duckemu::user::szstone
{

// 得物 6.1.5.10 libszstone.so — the Java side lf() walks.
//
// lf([BZZZ)[B takes the device-profile blob (key\x1cvalue\x1d...) and returns
// the /call registration body. On the way it asks Android for the package
// name, the package code path, the APK signature certificate and
// Settings.Secure android_id, so this pack declares all of them.
Result<void> installEnvironment(runtime::jni::JavaWorld &world);

extern const char *const kSzSdkClass;
extern const char *const kLfSignature;

// Runs SzSdk.lf(blob, z0, z1, z2) on an instance whose environment is already
// installed and returns the [B result. The runner and the worker plugin share
// this so the argument layout is written once.
Result<std::vector<uint8_t> > invokeLf(runtime::RuntimeInstance &instance,
	std::string_view blob,
	const std::array<uint64_t, 3> &z);

} // namespace duckemu::user::szstone
