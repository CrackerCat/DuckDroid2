// SPDX-License-Identifier: GPL-2.0-only
#include <cstdint>
#include <utility>

#include "runtime/jni/envs/envs.hpp"
#include "runtime/jni/world/java_world.hpp"

namespace duckemu::runtime::jni::envs
{
namespace
{

Result<void> declare(JavaWorld &world,
	uint64_t klass,
	const char *name,
	const char *signature,
	bool is_static,
	ShimHandler shim)
{
	auto declared = world.declareMethod(klass, name, signature, is_static, std::move(shim));
	if (!declared.ok())
	{
		return declared.error();
	}
	return {};
}

Result<void> installInetAddress(JavaWorld &world)
{
	auto declared = world.declareClass("java/net/InetAddress", "java/lang/Object");
	if (!declared.ok())
	{
		return declared.error();
	}
	const uint64_t klass = declared.value();
	const auto null_string = [](JavaWorld &, InvokeContext &) -> Result<uint64_t> {
		return uint64_t{ 0 };
	};
	auto host = declare(world, klass, "getHostAddress", "()Ljava/lang/String;", false,
		null_string);
	if (!host.ok())
	{
		return host;
	}
	auto canonical = declare(world, klass, "getCanonicalHostName",
		"()Ljava/lang/String;", false, null_string);
	if (!canonical.ok())
	{
		return canonical;
	}
	return declare(world, klass, "getHostName", "()Ljava/lang/String;", false, null_string);
}

// java.net.NetworkInterface: device-info probes enumerate interfaces to read
// the hardware address. The emulated process is not on a network, so the
// enumeration is empty; the members exist because AOSP's own callers resolve
// them before they look at the result.
Result<void> installNetworkInterface(JavaWorld &world)
{
	auto declared = world.declareClass("java/net/NetworkInterface", "java/lang/Object");
	if (!declared.ok())
	{
		return declared.error();
	}
	const uint64_t klass = declared.value();

	auto interfaces = declare(world, klass, "getNetworkInterfaces",
		"()Ljava/util/Enumeration;", true,
		[](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
			const uint64_t enumeration = java.findClass("java/util/Enumeration");
			if (enumeration == 0)
			{
				return uint64_t{ 0 };
			}
			return java.allocateObject(enumeration);
		});
	if (!interfaces.ok())
	{
		return interfaces;
	}
	if (auto by_name = declare(world, klass, "getByName",
		    "(Ljava/lang/String;)Ljava/net/NetworkInterface;", true,
		    [](JavaWorld &, InvokeContext &) -> Result<uint64_t> {
			    return uint64_t{ 0 };
		    });
		!by_name.ok())
	{
		return by_name;
	}

	const struct
	{
		const char *name;
		const char *signature;
		ShimHandler shim;
	} methods[] = {
		{ "getName", "()Ljava/lang/String;",
			[](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 0 }; } },
		{ "getDisplayName", "()Ljava/lang/String;",
			[](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 0 }; } },
		{ "getHardwareAddress", "()[B",
			[](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 0 }; } },
		{ "getInetAddresses", "()Ljava/util/Enumeration;",
			[](JavaWorld &java, InvokeContext &) -> Result<uint64_t> {
				const uint64_t enumeration = java.findClass("java/util/Enumeration");
				if (enumeration == 0)
				{
					return uint64_t{ 0 };
				}
				return java.allocateObject(enumeration);
			} },
		{ "isUp", "()Z",
			[](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 0 }; } },
		{ "isLoopback", "()Z",
			[](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 0 }; } },
		{ "isPointToPoint", "()Z",
			[](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 0 }; } },
		{ "isVirtual", "()Z",
			[](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 0 }; } },
		{ "supportsMulticast", "()Z",
			[](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 0 }; } },
		{ "getMTU", "()I",
			[](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 0 }; } },
		{ "getIndex", "()I",
			[](JavaWorld &, InvokeContext &) -> Result<uint64_t> { return uint64_t{ 0 }; } },
	};
	for (const auto &method : methods)
	{
		auto result = declare(world, klass, method.name, method.signature, false,
			method.shim);
		if (!result.ok())
		{
			return result;
		}
	}
	return {};
}

} // namespace

Result<void> installJavaNet(JavaWorld &world)
{
	auto inet = installInetAddress(world);
	if (!inet.ok())
	{
		return inet;
	}
	return installNetworkInterface(world);
}

} // namespace duckemu::runtime::jni::envs
