#include "runtime/jni/abi/abi_adapter.hpp"

#include <cstddef>

#include "runtime/memory/address_space.hpp"

namespace duckemu::runtime::jni
{
namespace
{

// AAPCS64 va_list: the callee's save-area cursors. A variadic integer
// argument comes from the general-purpose save area while gr_offs is
// negative, then from the overflow area above stack.
struct Aarch64VaList
{
	uint64_t stack = 0;
	uint64_t gr_top = 0;
	uint64_t vr_top = 0;
	int32_t gr_offs = 0;
	int32_t vr_offs = 0;
};
static_assert(sizeof(Aarch64VaList) == 32, "AAPCS64 va_list layout");

} // namespace

Result<std::vector<uint64_t> > readCallArguments(const MethodSignature &signature,
	const std::array<uint64_t, 8> &registers,
	CallVariant variant,
	RuntimeMemory &memory)
{
	std::vector<uint64_t> args;
	args.reserve(signature.args.size());

	if (variant == CallVariant::VaList)
	{
		// x3 points at the guest va_list owned by the variadic caller. Consume
		// it in declaration order and write the advanced cursors back, exactly
		// as va_arg would.
		const uint64_t address = registers[3];
		if (address == 0)
		{
			return Error{ ErrorCode::InvalidArgument, "Call<Type>MethodV with a null va_list" };
		}
		Aarch64VaList list;
		auto read = memory.read(GuestVA{ address }, &list, sizeof(list));
		if (!read.ok())
		{
			return read.error();
		}
		for (ValueKind kind : signature.args)
		{
			if (kind == ValueKind::Float || kind == ValueKind::Double)
			{
				return Error{ ErrorCode::Unsupported,
					"Call<Type>MethodV with floating-point arguments is not implemented" };
			}
			uint64_t slot = 0;
			if (list.gr_offs < 0)
			{
				slot = list.gr_top + static_cast<uint64_t>(static_cast<int64_t>(list.gr_offs));
				list.gr_offs += 8;
			}
			else
			{
				slot = list.stack;
				list.stack += 8;
			}
			uint64_t value = 0;
			auto loaded = memory.read(GuestVA{ slot }, &value, sizeof(value));
			if (!loaded.ok())
			{
				return loaded.error();
			}
			args.push_back(value);
		}
		auto written = memory.write(GuestVA{ address }, &list.stack, sizeof(list.stack));
		if (!written.ok())
		{
			return written.error();
		}
		written = memory.write(GuestVA{ address + offsetof(Aarch64VaList, gr_offs) },
			&list.gr_offs, sizeof(list.gr_offs));
		if (!written.ok())
		{
			return written.error();
		}
		return args;
	}

	if (variant == CallVariant::Array)
	{
		if (signature.args.empty())
		{
			return args;
		}
		const uint64_t pointer = registers[3];
		if (pointer == 0)
		{
			return Error{ ErrorCode::InvalidArgument, "Call<Type>MethodA with a null jvalue array" };
		}
		args.resize(signature.args.size(), 0);
		auto read = memory.read(GuestVA{ pointer }, args.data(), args.size() * sizeof(uint64_t));
		if (!read.ok())
		{
			return read.error();
		}
		return args;
	}

	// Plain variadic: env, receiver/class and methodID occupy x0..x2.
	constexpr size_t kFirstArgumentRegister = 3;
	constexpr size_t kRegisterArgumentCount = 8 - kFirstArgumentRegister;
	if (signature.args.size() > kRegisterArgumentCount)
	{
		return Error{ ErrorCode::Unsupported,
			"Call<Type>Method with more than five arguments (guest stack arguments) is not implemented" };
	}
	for (size_t index = 0; index < signature.args.size(); ++index)
	{
		args.push_back(registers[kFirstArgumentRegister + index]);
	}
	return args;
}

} // namespace duckemu::runtime::jni
