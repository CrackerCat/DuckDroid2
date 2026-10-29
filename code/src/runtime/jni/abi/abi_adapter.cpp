#include "runtime/jni/abi/abi_adapter.hpp"

#include "runtime/memory/address_space.hpp"

namespace duckemu::runtime::jni
{

Result<std::vector<uint64_t> > readCallArguments(const MethodSignature &signature,
	const std::array<uint64_t, 8> &registers,
	CallVariant variant,
	RuntimeMemory &memory)
{
	std::vector<uint64_t> args;
	args.reserve(signature.args.size());

	if (variant == CallVariant::VaList)
	{
		return Error{ ErrorCode::Unsupported,
			"Call<Type>MethodV (va_list) is not implemented" };
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
