// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "common/error/result.hpp"

namespace duckemu::runtime::jni
{
class JavaWorld;
}

namespace duckemu::user::taobao
{

// Android device surface used by SecurityGuard's OneMain device collectors:
// Build/telephony/network/settings/binder probes plus the window/sensor/
// motion classes resolved when the security-body and middletier plugins load.
//
// Everything here is a *device reading*, not SDK logic: the values follow the
// reference-device picture in taobao.properties (Mi 8 / Android 12 / API 32),
// and calls that need data the emulator cannot have answer the null/0 the
// real framework answers when the service is absent.
Result<void> installDeviceEnvironment(runtime::jni::JavaWorld &world);

} // namespace duckemu::user::taobao
