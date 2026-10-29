#include <algorithm>
#include <cstdint>
#include <vector>

#include "runtime/memory/address_space.hpp"
#include "runtime/svc/handler.hpp"
#include "runtime/svc/syscall_numbers.hpp"
#include "runtime/vfs/file.hpp"
#include "runtime/vfs/virtual_file_system.hpp"

namespace duckemu::runtime::svc
{
namespace
{

constexpr uint64_t kMapShared = 0x01;
constexpr uint64_t kMapPrivate = 0x02;
constexpr uint64_t kMapFixed = 0x10;
constexpr uint64_t kMapAnonymous = 0x20;

constexpr uint64_t kMaxFileMmapBytes = 1ULL << 20; // per read chunk while filling

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

// Copies a file's bytes into a freshly mapped guest range. MAP_PRIVATE
// semantics are exact (the guest gets a copy); a writable MAP_SHARED mapping
// would need write-back and is refused before we get here.
int64_t fillFileMapping(SyscallContext &ctx, vfs::File &file, GuestVA base,
	uint64_t length, uint64_t offset)
{
	const uint64_t span = pageUp(length);
	uint64_t cursor = 0;
	while (cursor < span) {
		const uint64_t chunk = std::min(span - cursor, kMaxFileMmapBytes);
		std::vector<uint8_t> buffer(static_cast<size_t>(chunk));
		auto read = file.readAt(offset + cursor, buffer.data(), chunk);
		if (!read.ok()) {
			(void)ctx.memory.unmap(base, span);
			return kError(kEio);
		}
		if (read.value() == 0) {
			break; // past EOF: the rest of the mapping stays zero
		}
		auto written = ctx.memory.write(base + cursor, buffer.data(), read.value());
		if (!written.ok()) {
			(void)ctx.memory.unmap(base, span);
			return kError(kEfault);
		}
		cursor += read.value();
		if (read.value() < chunk) {
			break;
		}
	}
	return 0;
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

	vfs::File *file = nullptr;
	uint64_t file_offset = 0;
	if ((flags & kMapAnonymous) == 0) {
		if (fd < 0) {
			return kError(kEbadf);
		}
		file = ctx.vfs.find(static_cast<uint64_t>(fd));
		if (file == nullptr) {
			return kError(kEbadf);
		}
		file_offset = ctx.args[5];
		if (file_offset % kGuestPageSize != 0) {
			return kError(kEinval);
		}
		if ((flags & kMapShared) != 0 && (prot & PermWrite) != 0) {
			// Write-back to the host file is not implemented; read-only and
			// private mappings are copies and need no backing coherency.
			return kError(kEacces);
		}
	}
	(void)kMapPrivate;

	const std::string name = file != nullptr ? "file-mmap" : "guest-mmap";
	Result<GuestVA> mapped = (flags & kMapFixed) != 0 ?
		ctx.memory.mapFixed(GuestVA{ address }, length, prot, name, true) :
		ctx.memory.mapAnonymous(length, prot, name);
	if (!mapped.ok()) {
		return toErrno(mapped.error());
	}
	if (file != nullptr) {
		const int64_t filled = fillFileMapping(ctx, *file, mapped.value(), length, file_offset);
		if (filled != 0) {
			return filled;
		}
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

// madvise is advisory: MADV_NORMAL/RANDOM/SEQUENTIAL/WILLNEED/DONTFORK/... do
// not change the bytes a later read returns, so answering success is exact.
// MADV_DONTNEED (and MADV_REMOVE, which allocators use for the same purpose)
// discards anonymous pages, and the guest is entitled to see zeroes
// afterwards, so the mapped range is cleared instead of left stale.
int64_t sysMadvise(SyscallContext &ctx)
{
	constexpr uint64_t kMadvDontNeed = 4;
	constexpr uint64_t kMadvRemove = 9;
	constexpr uint64_t kClearChunkBytes = 64 * 1024;

	const uint64_t address = untagged(ctx.args[0]);
	const uint64_t length = ctx.args[1];
	const uint64_t advice = ctx.args[2];
	if (length == 0) {
		return 0;
	}
	if (address % kGuestPageSize != 0) {
		return kError(kEinval);
	}
	const uint64_t span = pageUp(length);
	// A real kernel answers ENOMEM for a range that is not fully mapped.
	for (uint64_t offset = 0; offset < span; offset += kGuestPageSize) {
		if (!ctx.memory.isMapped(GuestVA{ address + offset })) {
			return kError(kEnomem);
		}
	}
	if (advice == kMadvDontNeed || advice == kMadvRemove) {
		const std::vector<uint8_t> zeros(
			static_cast<size_t>(std::min<uint64_t>(span, kClearChunkBytes)), 0);
		for (uint64_t offset = 0; offset < span; offset += zeros.size()) {
			const uint64_t chunk = std::min<uint64_t>(zeros.size(), span - offset);
			auto written = ctx.memory.write(GuestVA{ address + offset }, zeros.data(), chunk);
			if (!written.ok()) {
				return toErrno(written.error());
			}
		}
	}
	return 0;
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
	case kSysMadvise:
		return sysMadvise(ctx);
	default:
		return std::nullopt;
	}
}

} // namespace duckemu::runtime::svc
