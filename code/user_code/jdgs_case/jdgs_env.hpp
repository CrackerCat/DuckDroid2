#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "common/error/result.hpp"

namespace duckemu::runtime::jni
{
class JavaWorld;
}

namespace duckemu::user::jdgs
{

// Parameters of one Bridge.main(101) call. Defaults are the values the
// 16.1.50 app assembles on the Java side; each field can be overridden with a
// JDGS_* environment variable.
struct SignParams
{
	std::string request;
	std::string device_payload;
	std::string dfp_eid;
	std::string pay_version;
	std::string openudid;
	std::string sdk_config;
};

// The target's Java side: Bridge.main and the Android package/signature
// objects libjdg reads. Installed explicitly by the runner before any module
// is loaded.
Result<void> installEnvironment(runtime::jni::JavaWorld &world);

// Arguments of Bridge.main. The environment keeps the instance's JavaWorld so
// the runner can build the Object[] the native expects; the runtime gives host
// callers no other handle on it. Call only while the guest is not running.
SignParams signParamsFromEnvironment();
Result<uint64_t> buildInitArguments(std::string_view phase, bool with_elapsed);
Result<uint64_t> buildSignArguments(const SignParams &params);

// Inspection of the [Ljava/lang/Object; returned by Bridge.main. The argument
// is an object address (RuntimeInstance::jniResolveHandle), not a JNI handle.
Result<std::string> classNameOf(uint64_t object);
Result<uint64_t> readResultElement(uint64_t object);
Result<std::string> readResultString(uint64_t object);
Result<int32_t> readIntegerField(uint64_t object);

// Constants shared by the runner and the worker plugin.
extern const char *const kBridgeClass;
extern const char *const kMainSignature;
extern const char *const kPackageName;
extern const char *const kApkPath;

} // namespace duckemu::user::jdgs
