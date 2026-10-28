#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <random>
#include <string_view>

#include "runtime/memory/address_space.hpp"
#include "runtime/svc/handler.hpp"
#include "runtime/svc/syscall_numbers.hpp"

namespace duckemu::runtime::svc
{
namespace
{

constexpr uint64_t kMaxRandomBytes = 1ULL << 20;

int64_t sysGetpid(SyscallContext &ctx)
{
	return static_cast<int64_t>(ctx.pid);
}

int64_t sysGettid(SyscallContext &ctx)
{
	return static_cast<int64_t>(ctx.tid);
}

int64_t sysGetppid(SyscallContext &)
{
	return 0;
}

int64_t sysGetUid(SyscallContext &)
{
	return 1000;
}

int64_t writeArray(SyscallContext &ctx, uint64_t address, const uint64_t *values, size_t count)
{
	if (address == 0)
	{
		return 0;
	}
	auto written = ctx.memory.write(GuestVA{ address }, values, count * sizeof(uint64_t));
	return written.ok() ? 0 : -kEfault;
}

int64_t sysPrctl(SyscallContext &ctx)
{
	constexpr uint64_t kPrGetName = 16;
	constexpr uint64_t kPrSetTaggedAddrCtrl = 55;
	constexpr uint64_t kPrGetTaggedAddrCtrl = 56;

	const uint64_t option = ctx.args[0];
	if (option == kPrSetTaggedAddrCtrl || option == kPrGetTaggedAddrCtrl)
	{
		// The guest has no MTE; bionic checks for ENOTSUP via EINVAL.
		return -kEinval;
	}
	if (option == kPrGetName)
	{
		const char name[16] = "duckemu";
		auto written = ctx.memory.write(GuestVA{ ctx.args[1] }, name, sizeof(name));
		return written.ok() ? 0 : -kEfault;
	}
	return 0;
}

int64_t sysSchedSetaffinity(SyscallContext &ctx)
{
	const uint64_t pid = ctx.args[0];
	return (pid == 0 || pid == ctx.pid) ? 0 : -kEsrch;
}

int64_t sysSchedGetaffinity(SyscallContext &ctx)
{
	const uint64_t pid = ctx.args[0];
	const uint64_t size = ctx.args[1];
	const uint64_t mask_address = ctx.args[2];
	if (pid != 0 && pid != ctx.pid)
	{
		return -kEsrch;
	}
	if (size < sizeof(uint64_t))
	{
		return -kEinval;
	}
	const uint64_t mask = 1; // one CPU
	auto written = ctx.memory.write(GuestVA{ mask_address }, &mask, sizeof(mask));
	return written.ok() ? static_cast<int64_t>(sizeof(mask)) : -kEfault;
}

int64_t sysSigaltstack(SyscallContext &ctx)
{
	const uint64_t old_stack = ctx.args[1];
	const uint64_t empty[3] = { 0, 0, 0 };
	return writeArray(ctx, old_stack, empty, 3);
}

int64_t sysRtSigprocmask(SyscallContext &ctx)
{
	const uint64_t old_set = ctx.args[2];
	const uint64_t empty = 0;
	return writeArray(ctx, old_set, &empty, 1);
}

int64_t sysGetrlimit(SyscallContext &ctx)
{
	const uint64_t limits[2] = { ~0ULL, ~0ULL };
	return writeArray(ctx, ctx.args[1], limits, 2);
}

int64_t sysPrlimit64(SyscallContext &ctx)
{
	const uint64_t pid = ctx.args[0];
	if (pid != 0 && pid != ctx.pid)
	{
		return -kEsrch;
	}
	const uint64_t limits[2] = { ~0ULL, ~0ULL };
	return writeArray(ctx, ctx.args[3], limits, 2);
}

int64_t sysGetrandom(SyscallContext &ctx)
{
	uint64_t address = ctx.args[0];
	const uint64_t count = ctx.args[1];
	if (count > kMaxRandomBytes)
	{
		return -kEinval;
	}
	if (count == 0)
	{
		return 0;
	}

	// Host RNG; deterministic profiles can replace this later.
	static thread_local std::mt19937_64 generator([] {
		std::random_device device;
		return (static_cast<uint64_t>(device()) << 32) ^ device();
	}());
	std::array<uint8_t, 256> chunk{};
	uint64_t remaining = count;
	while (remaining > 0)
	{
		const uint64_t take = remaining < chunk.size() ? remaining : chunk.size();
		for (uint64_t i = 0; i < take; i += sizeof(uint64_t))
		{
			const uint64_t word = generator();
			std::memcpy(chunk.data() + i, &word, sizeof(word));
		}
		auto written = ctx.memory.write(GuestVA{ address }, chunk.data(), take);
		if (!written.ok())
		{
			return -kEfault;
		}
		address += take;
		remaining -= take;
	}
	return static_cast<int64_t>(count);
}

int64_t sysClockGettime(SyscallContext &ctx)
{
	const uint64_t clock_id = ctx.args[0];
	const uint64_t timespec = ctx.args[1];

	constexpr uint64_t kClockRealtime = 0;
	constexpr uint64_t kClockMonotonic = 1;
	constexpr uint64_t kClockProcessCputime = 2;
	constexpr uint64_t kClockThreadCputime = 3;
	constexpr uint64_t kClockMonotonicRaw = 4;
	constexpr uint64_t kClockBoottime = 7;

	int64_t seconds = 0;
	int64_t nanoseconds = 0;
	switch (clock_id)
	{
	case kClockRealtime:
	{
		const auto now = std::chrono::system_clock::now().time_since_epoch();
		const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
		seconds = ns / 1000000000;
		nanoseconds = ns % 1000000000;
		break;
	}
	case kClockMonotonic:
	case kClockMonotonicRaw:
	case kClockBoottime:
	{
		const auto now = std::chrono::steady_clock::now().time_since_epoch();
		const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
		seconds = ns / 1000000000;
		nanoseconds = ns % 1000000000;
		break;
	}
	case kClockProcessCputime:
	case kClockThreadCputime:
		break;
	default:
		return -kEinval;
	}

	const int64_t values[2] = { seconds, nanoseconds };
	auto written = ctx.memory.write(GuestVA{ timespec }, values, sizeof(values));
	return written.ok() ? 0 : -kEfault;
}

int64_t sysGettimeofday(SyscallContext &ctx)
{
	const uint64_t timeval = ctx.args[0];
	const uint64_t timezone = ctx.args[1];
	if (timeval != 0)
	{
		const auto now = std::chrono::system_clock::now().time_since_epoch();
		const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
		const int64_t values[2] = { us / 1000000, us % 1000000 };
		auto written = ctx.memory.write(GuestVA{ timeval }, values, sizeof(values));
		if (!written.ok())
		{
			return -kEfault;
		}
	}
	const uint64_t zero[2] = { 0, 0 };
	return writeArray(ctx, timezone, zero, 2);
}

int64_t sysUname(SyscallContext &ctx)
{
	constexpr uint64_t kFieldSize = 65;
	constexpr uint64_t kFieldCount = 6;
	const std::string_view fields[kFieldCount] = {
		"Linux",
		"duckemu",
		"6.1.0-duckemu",
		"#1 SMP",
		"aarch64",
		"",
	};
	std::array<char, kFieldSize * kFieldCount> buffer{};
	for (uint64_t i = 0; i < kFieldCount; ++i)
	{
		const uint64_t copy = fields[i].size() < kFieldSize - 1 ? fields[i].size() : kFieldSize - 1;
		std::memcpy(buffer.data() + i * kFieldSize, fields[i].data(), copy);
	}
	auto written = ctx.memory.write(GuestVA{ ctx.args[0] }, buffer.data(), buffer.size());
	return written.ok() ? 0 : -kEfault;
}

int64_t sysGetcwd(SyscallContext &ctx)
{
	const uint64_t buffer = ctx.args[0];
	const uint64_t size = ctx.args[1];
	if (size < 2)
	{
		return -kEinval;
	}
	const char root[2] = { '/', '\0' };
	auto written = ctx.memory.write(GuestVA{ buffer }, root, sizeof(root));
	return written.ok() ? static_cast<int64_t>(buffer) : -kEfault;
}

} // namespace

// exit/exit_group moved to thread_handlers.cpp: they act on the calling
// RuntimeThread and, for exit_group, on the whole instance.
std::optional<int64_t> handleMiscSyscall(SyscallContext &ctx, uint32_t number)
{
	switch (number)
	{
	case kSysGetpid:
		return sysGetpid(ctx);
	case kSysGettid:
		return sysGettid(ctx);
	case kSysGetppid:
		return sysGetppid(ctx);
	case kSysGetuid:
	case kSysGeteuid:
	case kSysGetgid:
	case kSysGetegid:
		return sysGetUid(ctx);
	case kSysPrctl:
		return sysPrctl(ctx);
	case kSysSchedSetaffinity:
		return sysSchedSetaffinity(ctx);
	case kSysSchedGetaffinity:
		return sysSchedGetaffinity(ctx);
	case kSysSchedYield:
		return 0;
	case kSysSigaltstack:
		return sysSigaltstack(ctx);
	case kSysRtSigaction:
		return 0;
	case kSysRtSigprocmask:
		return sysRtSigprocmask(ctx);
	case kSysGetrlimit:
		return sysGetrlimit(ctx);
	case kSysPrlimit64:
		return sysPrlimit64(ctx);
	case kSysSetRobustList:
	case kSysMembarrier:
		return 0;
	case kSysGetrandom:
		return sysGetrandom(ctx);
	case kSysClockGettime:
		return sysClockGettime(ctx);
	case kSysGettimeofday:
		return sysGettimeofday(ctx);
	case kSysUname:
		return sysUname(ctx);
	case kSysGetcwd:
		return sysGetcwd(ctx);
	default:
		return std::nullopt;
	}
}

} // namespace duckemu::runtime::svc
