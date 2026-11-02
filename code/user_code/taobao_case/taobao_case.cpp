// SPDX-License-Identifier: GPL-2.0-only
#include "taobao_case/taobao_case.hpp"

#include <array>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/instance/runtime_instance.hpp"
#include "taobao_case/taobao_env.hpp"

namespace duckemu::user::taobao
{
namespace
{

// Device values captured from the reference phone (logs/hook_a.log).
constexpr const char *kVersionName = "10.68.0";
constexpr const char *kOsRelease = "12";
constexpr const char *kAppSgLibDir = "/data/user/0/com.taobao.taobao/app_SGLib";
constexpr const char *kDeviceSoPath =
	"/data/app/~~ZkuUNV2frSgeDqVY6u13sw==/"
	"com.taobao.taobao-ooDG-cQxwXBd0pNhPDtRMA==/lib/arm64/"
	"libsgmainso-6.8.260901.so";

Result<std::vector<uint64_t> > strings(const std::vector<const char *> &values)
{
	std::vector<uint64_t> objects;
	objects.reserve(values.size());
	for (const char *value : values)
	{
		auto object = makeString(value);
		if (!object.ok())
		{
			return object.error();
		}
		objects.push_back(object.value());
	}
	return objects;
}

Result<uint64_t> boxInt(int32_t value)
{
	return makeIntBox(value);
}

// Runs an int-returning doCommand (10102/10101): the boxed Integer is 0 on
// success, non-zero when the Java side would throw SecException.
Result<int32_t> callIntCommand(runtime::RuntimeInstance &instance,
	int32_t command,
	const std::vector<uint64_t> &elements)
{
	auto boxed = callCommand(instance, command, elements);
	if (!boxed.ok())
	{
		return boxed.error();
	}
	if (boxed.value() == 0)
	{
		return Error{ ErrorCode::Internal,
			std::to_string(command) + " returned null, expected an Integer" };
	}
	return readIntegerValue(boxed.value());
}

} // namespace

Result<uint64_t> callCommand(runtime::RuntimeInstance &instance,
	int32_t command,
	const std::vector<uint64_t> &elements)
{
	auto array = newObjectArray(elements);
	if (!array.ok())
	{
		return array.error();
	}
	const std::array<uint64_t, 8> arguments = {
		static_cast<uint64_t>(static_cast<uint32_t>(command)),
		array.value(),
	};
	auto called = instance.callRegisteredNative(
		kNativeClass, kNativeMethod, kNativeSignature, arguments);
	if (!called.ok())
	{
		return Error{ called.error().code,
			std::to_string(command) + ": " + called.error().message };
	}
	if (called.value() == 0)
	{
		return uint64_t{ 0 };
	}
	auto object = instance.jniResolveHandle(called.value());
	if (!object.ok())
	{
		return Error{ object.error().code,
			std::to_string(command) + ": cannot resolve the result handle: " +
				object.error().message };
	}
	return object.value();
}

Result<void> init10102(runtime::RuntimeInstance &instance)
{
// 10102 runs before 10101 on the device: the plugin announces its name,
// SDK version and the SO path it was loaded from.
{
	auto values = strings({ "main", kSdkVersion, kDeviceSoPath });
	if (!values.ok())
	{
		return values.error();
	}
	std::fprintf(stderr, "call 10102...\n");
	auto called = callIntCommand(instance, 10102, values.value());
	if (!called.ok())
	{
		return called.error();
	}
	if (called.value() != 0)
	{
		return Error{ ErrorCode::Internal,
			"10102 returned " + std::to_string(called.value()) };
	}
	std::fprintf(stderr, "10102 -> 0\n");
}

	return {};
}

Result<void> init10101(runtime::RuntimeInstance &instance)
{
	auto registered = init10102(instance);
	if (!registered.ok())
	{
		return registered.error();
	}
	// 10101 — the eleven arguments exactly as captured (flags=3, dataDir=
	// app_SGLib, M24=1; these were the three the decompiled guess got wrong).
	auto context = newContextObject();
	if (!context.ok())
	{
		return context.error();
	}
	auto plugin = newMainPluginObject();
	if (!plugin.ok())
	{
		return plugin.error();
	}
	auto flags = boxInt(3);
	auto os_release = makeString(kOsRelease);
	auto package = makeString(kPackage);
	auto version = makeString(kVersionName);
	auto process = makeString(kPackage);
	auto app_sg_lib = makeString(kAppSgLibDir);
	auto empty_a = makeString("");
	auto empty_b = makeString("");
	auto m24 = boxInt(1);
	for (const auto &result : { flags, os_release, package, version, process, app_sg_lib,
		     empty_a, empty_b, m24 })
	{
		if (!result.ok())
		{
			return result.error();
		}
	}
	const std::vector<uint64_t> arguments = {
		context.value(),
		flags.value(),
		empty_a.value(),
		app_sg_lib.value(),
		empty_b.value(),
		package.value(),
		version.value(),
		os_release.value(),
		process.value(),
		m24.value(),
		plugin.value(),
	};
	std::fprintf(stderr, "call 10101...\n");
	auto status = callIntCommand(instance, 10101, arguments);
	if (!status.ok())
	{
		return status.error();
	}
	if (status.value() != 0)
	{
		return Error{ ErrorCode::Internal,
			"10101 returned " + std::to_string(status.value()) +
				" (the Java side would throw SecException)" };
	}
	std::fprintf(stderr, "10101 -> 0\n");
	return {};
}

Result<void> init(runtime::RuntimeInstance &instance)
{
	auto registered = init10101(instance);
	if (!registered.ok())
	{
		return registered.error();
	}

	// 10103 / 10104: "plugin loaded" notifications. Both return null on the
	// reference device and the Java side ignores the value.
	for (const int32_t command : { 10103, 10104 })
	{
		std::fprintf(stderr, "call %d...\n", command);
		auto ignored = callCommand(instance, command, {});
		if (!ignored.ok())
		{
			return ignored.error();
		}
	}

	// 10107: the instance handle. The device answers a Long; the Java side
	// only records it, so the value itself is not an input to signing.
	std::fprintf(stderr, "call 10107...\n");
	auto handle = callCommand(instance, 10107, {});
	if (!handle.ok())
	{
		return handle.error();
	}
	std::fprintf(stderr, "10107 done\n");
	return {};
}

} // namespace duckemu::user::taobao
