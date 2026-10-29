// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "common/error/result.hpp"
#include "runtime/jni/envs/envs.hpp"

namespace duckemu::runtime::jni
{
class JavaWorld;
}

namespace duckemu::user::libeleven
{

// The target project's Java side: the class libeleven's JNI_OnLoad looks up and
// the native method it registers. Installed explicitly by main.cpp before any
// module is loaded.
Result<void> installEnvironment(runtime::jni::JavaWorld &world);

} // namespace duckemu::user::libeleven
