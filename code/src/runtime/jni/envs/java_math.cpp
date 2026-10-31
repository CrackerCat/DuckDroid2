// SPDX-License-Identifier: GPL-2.0-only
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "common/id/ids.hpp"
#include "runtime/jni/envs/envs.hpp"
#include "runtime/jni/world/java_world.hpp"
#include "runtime/memory/address_space.hpp"

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

// Converts a big-endian unsigned magnitude to lower-case text in |radix|,
// which is what BigInteger.toString() produces (radix 16 is lower-case).
std::string magnitudeToText(std::vector<uint8_t> magnitude, uint32_t radix)
{
	static constexpr char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
	size_t begin = 0;
	while (begin < magnitude.size() && magnitude[begin] == 0)
	{
		++begin;
	}
	if (begin == magnitude.size())
	{
		return "0";
	}
	std::vector<uint8_t> work(
		magnitude.begin() + static_cast<std::ptrdiff_t>(begin), magnitude.end());
	std::string text;
	while (!work.empty())
	{
		uint32_t remainder = 0;
		for (uint8_t &byte : work)
		{
			const uint32_t current = remainder * 256u + byte;
			byte = static_cast<uint8_t>(current / radix);
			remainder = current % radix;
		}
		text.push_back(kDigits[remainder]);
		while (!work.empty() && work.front() == 0)
		{
			work.erase(work.begin());
		}
	}
	std::reverse(text.begin(), text.end());
	return text;
}

} // namespace

// java.math.BigInteger, with the magnitude/negative representation the
// certificate pack writes and the decimal/hex text a target reads back.
Result<void> installJavaMath(JavaWorld &world)
{
	auto declared = world.declareClass("java/math/BigInteger", "java/lang/Number");
	if (!declared.ok())
	{
		return declared.error();
	}
	const uint64_t klass = declared.value();

	auto magnitude = world.declareField(klass, "mag", "[B", false);
	if (!magnitude.ok())
	{
		return magnitude.error();
	}
	auto negative = world.declareField(klass, "negative", "Z", false);
	if (!negative.ok())
	{
		return negative.error();
	}
	const uint64_t magnitude_field = magnitude.value();
	const uint64_t negative_field = negative.value();

	return declare(world, klass, "toString", "(I)Ljava/lang/String;", false,
		[magnitude_field, negative_field](JavaWorld &java,
			InvokeContext &call) -> Result<uint64_t> {
			if (call.args.size() != 1)
			{
				return Error{ ErrorCode::InvalidArgument,
					"BigInteger.toString expects one radix" };
			}
			const uint64_t radix = call.args[0];
			if (radix < 2 || radix > 36)
			{
				return Error{ ErrorCode::InvalidArgument,
					"BigInteger.toString: radix out of range" };
			}
			auto stored = java.readInstanceField(call.receiver, magnitude_field);
			if (!stored.ok())
			{
				return stored.error();
			}
			if (stored.value() == 0)
			{
				return Error{ ErrorCode::Internal, "BigInteger has no magnitude" };
			}
			auto view = java.arrayView(stored.value());
			if (!view.ok())
			{
				return view.error();
			}
			if (view.value().kind != ArrayElementKind::Byte)
			{
				return Error{ ErrorCode::Internal, "BigInteger magnitude is not a byte array" };
			}
			std::vector<uint8_t> bytes(static_cast<size_t>(view.value().length));
			if (!bytes.empty())
			{
				auto read = java.memory().read(
					GuestVA{ view.value().storage }, bytes.data(), bytes.size());
				if (!read.ok())
				{
					return read.error();
				}
			}
			auto sign = java.readInstanceField(call.receiver, negative_field);
			if (!sign.ok())
			{
				return sign.error();
			}
			std::string text = magnitudeToText(std::move(bytes), static_cast<uint32_t>(radix));
			if (sign.value() != 0 && text != "0")
			{
				text.insert(text.begin(), '-');
			}
			return java.newString(text);
		});
}

} // namespace duckemu::runtime::jni::envs
