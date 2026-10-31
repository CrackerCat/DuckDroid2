// SPDX-License-Identifier: GPL-2.0-only
#include <cstdint>
#include <string_view>
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

Result<void> installInputStream(JavaWorld &world)
{
	// The real superclass, with no members: the certificate pack reads the
	// concrete ByteArrayInputStream's buffer directly.
	auto input = world.declareClass("java/io/InputStream", "java/lang/Object");
	if (!input.ok())
	{
		return input.error();
	}

	auto declared = world.declareClass("java/io/ByteArrayInputStream", "java/io/InputStream");
	if (!declared.ok())
	{
		return declared.error();
	}
	const uint64_t klass = declared.value();

	auto buffer = world.declareField(klass, "buf", "[B", false);
	if (!buffer.ok())
	{
		return buffer.error();
	}
	const uint64_t buffer_field = buffer.value();

	// ByteArrayInputStream(byte[] buf): keep the array object itself; callers
	// that need the bytes (the certificate pack) read it back through the field.
	return declare(world, klass, "<init>", "([B)V", false,
		[buffer_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() != 1 || call.args[0] == 0)
			{
				return Error{ ErrorCode::InvalidArgument,
					"ByteArrayInputStream.<init> expects a byte array" };
			}
			auto view = java.arrayView(call.args[0]);
			if (!view.ok())
			{
				return view.error();
			}
			if (view.value().kind != ArrayElementKind::Byte)
			{
				return Error{ ErrorCode::InvalidArgument,
					"ByteArrayInputStream.<init> expects a byte array" };
			}
			auto written = java.writeInstanceField(call.receiver, buffer_field, call.args[0]);
			if (!written.ok())
			{
				return written.error();
			}
			return uint64_t{ 0 };
		});
}

Result<void> installFile(JavaWorld &world)
{
	auto declared = world.declareClass("java/io/File", "java/lang/Object");
	if (!declared.ok())
	{
		return declared.error();
	}
	const uint64_t klass = declared.value();

	auto path = world.declareField(klass, "path", "Ljava/lang/String;", false);
	if (!path.ok())
	{
		return path.error();
	}
	const uint64_t path_field = path.value();

	auto constructor = declare(world, klass, "<init>", "(Ljava/lang/String;)V", false,
		[path_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() != 1 || call.args[0] == 0)
			{
				return Error{ ErrorCode::InvalidArgument,
					"File.<init> expects a path string" };
			}
			auto written =
				java.writeInstanceField(call.receiver, path_field, call.args[0]);
			if (!written.ok())
			{
				return written.error();
			}
			return uint64_t{ 0 };
		});
	if (!constructor.ok())
	{
		return constructor;
	}

	// Targets build Files from absolute paths (Context.getFilesDir and the
	// like), so the stored path is what getAbsolutePath answers.
	return declare(world, klass, "getAbsolutePath", "()Ljava/lang/String;", false,
		[path_field](JavaWorld &java, InvokeContext &call) -> Result<uint64_t> {
			auto stored = java.readInstanceField(call.receiver, path_field);
			if (!stored.ok())
			{
				return stored.error();
			}
			if (stored.value() == 0)
			{
				return Error{ ErrorCode::Internal, "File has no path" };
			}
			return stored.value();
		});
}

} // namespace

Result<uint64_t> newFile(JavaWorld &world, std::string_view path)
{
	const uint64_t klass = world.findClass("java/io/File");
	const uint64_t field = klass == 0
		? 0
		: world.findField(klass, "path", "Ljava/lang/String;", false);
	if (field == 0)
	{
		return Error{ ErrorCode::NotFound, "java/io/File is not installed" };
	}
	auto object = world.allocateObject(klass);
	if (!object.ok())
	{
		return object.error();
	}
	auto text = world.newString(path);
	if (!text.ok())
	{
		return text.error();
	}
	auto written = world.writeInstanceField(object.value(), field, text.value());
	if (!written.ok())
	{
		return written.error();
	}
	return object.value();
}

Result<void> installJavaIo(JavaWorld &world)
{
	const auto installers = { installInputStream, installFile };
	for (const auto installer : installers)
	{
		auto installed = installer(world);
		if (!installed.ok())
		{
			return installed.error();
		}
	}
	return {};
}

} // namespace duckemu::runtime::jni::envs
