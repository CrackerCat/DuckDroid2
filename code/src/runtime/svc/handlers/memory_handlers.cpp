#include <cstdint>

#include "common/log/logger.hpp"
#include "runtime/memory/address_space.hpp"
#include "runtime/svc/handler.hpp"
#include "runtime/svc/syscall_numbers.hpp"

namespace duckemu::runtime::svc
{
namespace
{

constexpr uint64_t kMapShared = 0x01;
constexpr uint64_t kMapPrivate = 0x02;
constexpr uint64_t kMapFixed = 0x10;
constexpr uint64_t kMapAnonymous = 0x20;

int64_t toErrno(const Error &error)
{
	switch (error.code) {
	case ErrorCode::InvalidArgument:
		return kError(kEinval);
	case ErrorCode::OutOfMemory:
		return kError(kEnomem);
	case ErrorCode::Unmapped:
		return kError(kEfault);
	case ErrorCode::AlreadyExists:
		return kError(kEinval);
	case ErrorCode::Unsupported:
		return kError(kEnodev);
	default:
		return kError(kEnomem);
	}
}

// bionic passes scudo pointers with a TB I tag in the top byte; the kernel
// would ignore it. Unicorn does not, so strip it at the syscall boundary.
uint64_t untagged(uint64_t address)
{
	return address & ~(0xFFULL << 56);
}

int64_t sysMmap(SyscallContext &ctx)
{
	const uint64_t address = untagged(ctx.args[0]);
	const uint64_t length = ctx.args[1];
	const uint32_t prot = static_cast<uint32_t>(ctx.args[2] & PermAll);
	const uint64_t flags = ctx.args[3];
	const int64_t fd = static_cast<int64_t>(ctx.args[4]);

	if (length == 0) {
		return kError(kEinval);
	}
	if ((flags & kMapAnonymous) == 0) {
		// File-backed mappings need the VFS/loader; they arrive in M4.
		DUCKEMU_WARN("mmap(fd=%lld) is not supported yet", static_cast<long long>(fd));
		return kError(kEnodev);
	}
	(void)kMapShared;
	(void)kMapPrivate;

	Result<GuestVA> mapped = (flags & kMapFixed) != 0 ?
		ctx.memory.mapFixed(GuestVA{ address }, length, prot, "guest-mmap", true) :
		ctx.memory.mapAnonymous(length, prot, "guest-mmap");
	if (!mapped.ok()) {
		return toErrno(mapped.error());
	}
	return static_cast<int64_t>(mapped.value().value());
}

int64_t sysMunmap(SyscallContext &ctx)
{
	const uint64_t address = untagged(ctx.args[0]);
	const uint64_t length = ctx.args[1];
	if (length == 0 || address % kGuestPageSize != 0) {
		return kError(kEinval);
	}
	auto result = ctx.memory.unmap(GuestVA{ address }, length);
	return result.ok() ? 0 : toErrno(result.error());
}

int64_t sysMprotect(SyscallContext &ctx)
{
	const uint64_t address = untagged(ctx.args[0]);
	const uint64_t length = ctx.args[1];
	const uint32_t prot = static_cast<uint32_t>(ctx.args[2] & PermAll);
	if (length == 0 || address % kGuestPageSize != 0) {
		return kError(kEinval);
	}
	auto result = ctx.memory.protect(GuestVA{ address }, length, prot);
	if (!result.ok()) {
		return result.error().code == ErrorCode::Unmapped ? kError(kEnomem) : toErrno(result.error());
	}
	return 0;
}

int64_t sysBrk(SyscallContext &ctx)
{
	auto result = ctx.memory.brk(GuestVA{ ctx.args[0] });
	if (!result.ok()) {
		return kError(kEnomem);
	}
	return static_cast<int64_t>(result.value().value());
}

} // namespace

std::optional<int64_t> handleMemorySyscall(SyscallContext &ctx, uint32_t number)
{
	switch (number) {
	case kSysMmap:
		return sysMmap(ctx);
	case kSysMunmap:
		return sysMunmap(ctx);
	case kSysMprotect:
		return sysMprotect(ctx);
	case kSysBrk:
		return sysBrk(ctx);
	default:
		return std::nullopt;
	}
}

} // namespace duckemu::runtime::svc
