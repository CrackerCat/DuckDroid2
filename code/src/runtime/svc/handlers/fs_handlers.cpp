// SPDX-License-Identifier: GPL-2.0-only
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "common/log/logger.hpp"
#include "runtime/memory/address_space.hpp"
#include "runtime/svc/handler.hpp"
#include "runtime/svc/syscall_numbers.hpp"
#include "runtime/vfs/file.hpp"
#include "runtime/vfs/virtual_file_system.hpp"

namespace duckemu::runtime::svc
{
namespace
{

constexpr uint64_t kMaxIoBytes = 1ULL << 20; // 1 MiB per call
constexpr uint64_t kMaxPathBytes = 4096;
constexpr uint64_t kAtEmptyPath = 0x1000;

// arm64 kernel `struct stat` (arch/arm64/include/uapi/asm/stat.h). bionic
// passes the kernel buffer straight through, so fs_handlers writes this layout.
constexpr size_t kStatSize = 128;
constexpr size_t kStatModeOffset = 16;
constexpr size_t kStatSizeOffset = 48;
constexpr size_t kStatBlocksOffset = 64;
constexpr size_t kStatBlkSizeOffset = 56;
constexpr size_t kStatInodeOffset = 8;
constexpr size_t kStatDeviceOffset = 0;
constexpr size_t kStatNlinkOffset = 20;
constexpr size_t kStatUidOffset = 24;
constexpr size_t kStatGidOffset = 28;

constexpr uint32_t kModeCharacter = 0020000;

// fcntl commands and arm64 flag values bionic may pass.
constexpr int64_t kFcntlDupFd = 0;
constexpr int64_t kFcntlGetFd = 1;
constexpr int64_t kFcntlSetFd = 2;
constexpr int64_t kFcntlGetFl = 3;
constexpr int64_t kFcntlSetFl = 4;
constexpr int64_t kFcntlGetLk = 5;
constexpr int64_t kFcntlSetLk = 6;
constexpr int64_t kFcntlSetLkW = 7;
constexpr int64_t kFcntlDupFdCloexec = 1030;
constexpr uint64_t kOFdCloexec = 0x80000;
constexpr uint64_t kOWrOnly = 1;
// struct flock on arm64: l_type(2) l_whence(2) l_start(8) l_len(8) l_pid(4)
// plus padding. F_UNLCK == 2; F_RDLCK is 0, so a zeroed struct would claim a
// read lock.
constexpr size_t kFlockSize = 32;
constexpr uint8_t kFUnlck = 2;

int64_t toErrno(const Error &error)
{
	switch (error.code) {
	case ErrorCode::NotFound:
		return kError(kEnoent);
	case ErrorCode::PermissionDenied:
		return kError(kEacces);
	case ErrorCode::InvalidArgument:
		return kError(kEinval);
	case ErrorCode::AlreadyExists:
		return kError(kEexist);
	case ErrorCode::Unsupported:
		return kError(kErofs);
	case ErrorCode::BadFileDescriptor:
		return kError(kEbadf);
	case ErrorCode::IsDirectory:
		return kError(kEisdir);
	case ErrorCode::NotDirectory:
		return kError(kEnotdir);
	case ErrorCode::NotSeekable:
		return kError(kEspipe);
	case ErrorCode::OutOfMemory:
		return kError(kEnomem);
	default:
		return kError(kEio);
	}
}

// Reads a NUL-terminated guest string. Returns 0 or a negative errno.
int64_t readPath(SyscallContext &ctx, uint64_t address, std::string &path)
{
	path.clear();
	for (uint64_t i = 0; i < kMaxPathBytes; ++i) {
		char byte = 0;
		if (!ctx.memory.read(GuestVA{ address + i }, &byte, 1).ok()) {
			return kError(kEfault);
		}
		if (byte == '\0') {
			return 0;
		}
		path.push_back(byte);
	}
	return kError(kEnametoolong);
}

void putU32(std::array<uint8_t, kStatSize> &bytes, size_t offset, uint32_t value)
{
	std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void putU64(std::array<uint8_t, kStatSize> &bytes, size_t offset, uint64_t value)
{
	std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

int64_t writeStat(SyscallContext &ctx, uint64_t address, const vfs::FileStat &stat)
{
	std::array<uint8_t, kStatSize> bytes{};
	putU64(bytes, kStatDeviceOffset, stat.device);
	putU64(bytes, kStatInodeOffset, stat.inode);
	putU32(bytes, kStatModeOffset, stat.mode);
	putU32(bytes, kStatNlinkOffset, stat.link_count);
	putU32(bytes, kStatUidOffset, stat.uid);
	putU32(bytes, kStatGidOffset, stat.gid);
	putU64(bytes, kStatSizeOffset, stat.size);
	putU32(bytes, kStatBlkSizeOffset, stat.block_size);
	putU64(bytes, kStatBlocksOffset, stat.blocks);
	putU64(bytes, 72, static_cast<uint64_t>(stat.atime_seconds));
	putU64(bytes, 80, static_cast<uint64_t>(stat.atime_nanoseconds));
	putU64(bytes, 88, static_cast<uint64_t>(stat.mtime_seconds));
	putU64(bytes, 96, static_cast<uint64_t>(stat.mtime_nanoseconds));
	putU64(bytes, 104, static_cast<uint64_t>(stat.ctime_seconds));
	putU64(bytes, 112, static_cast<uint64_t>(stat.ctime_nanoseconds));

	auto written = ctx.memory.write(GuestVA{ address }, bytes.data(), bytes.size());
	return written.ok() ? 0 : kError(kEfault);
}

int64_t sysWrite(SyscallContext &ctx)
{
	const uint64_t fd = ctx.args[0];
	const uint64_t buffer_address = ctx.args[1];
	const uint64_t count = ctx.args[2];

	if (count > kMaxIoBytes) {
		return kError(kEinval);
	}

	std::vector<uint8_t> buffer(static_cast<size_t>(count));
	auto copied = ctx.memory.read(GuestVA{ buffer_address }, buffer.data(), count);
	if (!copied.ok()) {
		return kError(kEfault);
	}

	if (fd == 1 || fd == 2) {
		const ssize_t written = ::write(static_cast<int>(fd), buffer.data(), buffer.size());
		if (written < 0) {
			DUCKEMU_WARN("host write(fd=%llu) failed", static_cast<unsigned long long>(fd));
			return kError(kEio);
		}
		return static_cast<int64_t>(written);
	}

	vfs::File *file = ctx.vfs.find(fd);
	if (file == nullptr || !file->writable()) {
		return kError(kEbadf);
	}
	auto written = file->write(buffer.data(), count);
	return written.ok() ? static_cast<int64_t>(written.value()) : toErrno(written.error());
}

int64_t sysRead(SyscallContext &ctx)
{
	const uint64_t fd = ctx.args[0];
	const uint64_t buffer_address = ctx.args[1];
	const uint64_t count = ctx.args[2];

	if (count > kMaxIoBytes) {
		return kError(kEinval);
	}

	if (fd <= 2) {
		if (fd != 0) {
			return kError(kEbadf);
		}
		std::vector<uint8_t> buffer(static_cast<size_t>(count));
		const ssize_t received = ::read(0, buffer.data(), buffer.size());
		if (received < 0) {
			return kError(kEio);
		}
		auto copied = ctx.memory.write(GuestVA{ buffer_address }, buffer.data(),
			static_cast<uint64_t>(received));
		if (!copied.ok()) {
			return kError(kEfault);
		}
		return static_cast<int64_t>(received);
	}

	vfs::File *file = ctx.vfs.find(fd);
	if (file == nullptr) {
		return kError(kEbadf);
	}
	std::vector<uint8_t> buffer(static_cast<size_t>(count));
	auto read = file->read(buffer.data(), count);
	if (!read.ok()) {
		return toErrno(read.error());
	}
	auto copied = ctx.memory.write(GuestVA{ buffer_address }, buffer.data(), read.value());
	if (!copied.ok()) {
		return kError(kEfault);
	}
	return static_cast<int64_t>(read.value());
}

int64_t sysPread64(SyscallContext &ctx)
{
	const uint64_t fd = ctx.args[0];
	const uint64_t buffer_address = ctx.args[1];
	const uint64_t count = ctx.args[2];
	const uint64_t offset = ctx.args[3];

	if (fd <= 2) {
		return kError(kEspipe);
	}
	if (count > kMaxIoBytes) {
		return kError(kEinval);
	}
	vfs::File *file = ctx.vfs.find(fd);
	if (file == nullptr) {
		return kError(kEbadf);
	}
	std::vector<uint8_t> buffer(static_cast<size_t>(count));
	auto read = file->readAt(offset, buffer.data(), count);
	if (!read.ok()) {
		return toErrno(read.error());
	}
	auto copied = ctx.memory.write(GuestVA{ buffer_address }, buffer.data(), read.value());
	if (!copied.ok()) {
		return kError(kEfault);
	}
	return static_cast<int64_t>(read.value());
}

int64_t sysClose(SyscallContext &ctx)
{
	const uint64_t fd = ctx.args[0];
	// Do not close the host's stdio out from under the tests.
	if (fd <= 2) {
		return 0;
	}
	auto closed = ctx.vfs.close(fd);
	return closed.ok() ? 0 : kError(kEbadf);
}

int64_t sysLseek(SyscallContext &ctx)
{
	const uint64_t fd = ctx.args[0];
	if (fd <= 2) {
		return kError(kEspipe);
	}
	vfs::File *file = ctx.vfs.find(fd);
	if (file == nullptr) {
		return kError(kEbadf);
	}
	auto result = file->seek(static_cast<int64_t>(ctx.args[1]), static_cast<int>(ctx.args[2]));
	return result.ok() ? static_cast<int64_t>(result.value()) : toErrno(result.error());
}

int64_t sysOpenat(SyscallContext &ctx)
{
	std::string path;
	const int64_t path_error = readPath(ctx, ctx.args[1], path);
	if (path_error != 0) {
		return path_error;
	}
	auto opened = ctx.vfs.open(ctx.args[0], path, ctx.args[2]);
	if (!opened.ok()) {
		return toErrno(opened.error());
	}
	return static_cast<int64_t>(opened.value());
}

int64_t sysFaccessat(SyscallContext &ctx)
{
	std::string path;
	const int64_t path_error = readPath(ctx, ctx.args[1], path);
	if (path_error != 0) {
		return path_error;
	}
	auto result = ctx.vfs.access(ctx.args[0], path, ctx.args[2]);
	return result.ok() ? 0 : toErrno(result.error());
}

int64_t sysNewfstatat(SyscallContext &ctx)
{
	const uint64_t dirfd = ctx.args[0];
	const uint64_t path_address = ctx.args[1];
	const uint64_t stat_address = ctx.args[2];
	const uint64_t flags = ctx.args[3];

	std::string path;
	if (path_address == 0) {
		if ((flags & kAtEmptyPath) == 0) {
			return kError(kEfault);
		}
	}
	else {
		const int64_t path_error = readPath(ctx, path_address, path);
		if (path_error != 0) {
			return path_error;
		}
	}

	vfs::FileStat stat;
	if (path.empty() && (flags & kAtEmptyPath) != 0) {
		// fstatat(fd, "", &st, AT_EMPTY_PATH) is how bionic implements fstat.
		if (dirfd <= 2) {
			stat.mode = kModeCharacter | 0666;
		}
		else {
			auto result = ctx.vfs.statFd(dirfd);
			if (!result.ok()) {
				return toErrno(result.error());
			}
			stat = result.value();
		}
	}
	else if (path.empty()) {
		return kError(kEnoent);
	}
	else {
		auto result = ctx.vfs.stat(dirfd, path);
		if (!result.ok()) {
			return toErrno(result.error());
		}
		stat = result.value();
	}
	return writeStat(ctx, stat_address, stat);
}

// The guest file system is read-only: every host root is exposed RO and /proc
// is virtual. A real kernel answers EROFS for mkdirat on such a mount, so the
// guest gets that instead of an unimplemented-syscall warning.
int64_t sysMkdirat(SyscallContext &)
{
	return kError(kErofs);
}

int64_t sysDup(SyscallContext &ctx)
{
	auto duplicated = ctx.vfs.duplicate(ctx.args[0]);
	return duplicated.ok() ? static_cast<int64_t>(duplicated.value()) : toErrno(duplicated.error());
}

int64_t sysDup3(SyscallContext &ctx)
{
	const uint64_t old_fd = ctx.args[0];
	const uint64_t new_fd = ctx.args[1];
	const uint64_t flags = ctx.args[2];
	if ((flags & ~kOFdCloexec) != 0 || old_fd == new_fd)
	{
		// dup3 only accepts O_CLOEXEC, and refuses identical descriptors
		// (dup2 accepts them as a no-op).
		return kError(kEinval);
	}
	auto moved = ctx.vfs.duplicateTo(old_fd, new_fd);
	return moved.ok() ? static_cast<int64_t>(new_fd) : toErrno(moved.error());
}

bool fdIsOpen(SyscallContext &ctx, uint64_t fd)
{
	return fd <= 2 || ctx.vfs.find(fd) != nullptr;
}

int64_t sysFcntl(SyscallContext &ctx)
{
	const uint64_t fd = ctx.args[0];
	const int64_t command = static_cast<int64_t>(ctx.args[1]);

	switch (command)
	{
	case kFcntlDupFd:
	case kFcntlDupFdCloexec:
	{
		auto duplicated = ctx.vfs.duplicate(fd, ctx.args[2]);
		return duplicated.ok() ? static_cast<int64_t>(duplicated.value()) :
					 toErrno(duplicated.error());
	}
	case kFcntlGetFd:
	case kFcntlSetFd:
		// No CLOEXEC state is tracked: descriptors never survive an exec
		// here, and every guest descriptor is already close-on-exec from the
		// runtime's point of view.
		return fdIsOpen(ctx, fd) ? 0 : kError(kEbadf);
	case kFcntlGetFl:
	{
		if (fd <= 2)
		{
			return fd == 0 ? 0 : static_cast<int64_t>(kOWrOnly);
		}
		vfs::File *file = ctx.vfs.find(fd);
		if (file == nullptr)
		{
			return kError(kEbadf);
		}
		// openat only admits read-only opens, so the access mode is derived
		// from the description.
		return file->writable() ? static_cast<int64_t>(kOWrOnly) : 0;
	}
	case kFcntlSetFl:
		return fdIsOpen(ctx, fd) ? 0 : kError(kEbadf);
	case kFcntlGetLk:
	{
		if (!fdIsOpen(ctx, fd))
		{
			return kError(kEbadf);
		}
		std::array<uint8_t, kFlockSize> lock{};
		lock[0] = kFUnlck;
		auto written = ctx.memory.write(GuestVA{ ctx.args[2] }, lock.data(), lock.size());
		return written.ok() ? 0 : kError(kEfault);
	}
	case kFcntlSetLk:
	case kFcntlSetLkW:
		// No lock manager: report the lock as taken. A guest that depends on
		// cross-process exclusion would need a real one, but the runtime is
		// process-isolated per instance.
		return fdIsOpen(ctx, fd) ? 0 : kError(kEbadf);
	default:
		return kError(kEinval);
	}
}

int64_t sysFstat(SyscallContext &ctx)
{
	const uint64_t fd = ctx.args[0];
	vfs::FileStat stat;
	if (fd <= 2) {
		stat.mode = kModeCharacter | 0666;
	}
	else {
		auto result = ctx.vfs.statFd(fd);
		if (!result.ok()) {
			return toErrno(result.error());
		}
		stat = result.value();
	}
	return writeStat(ctx, ctx.args[1], stat);
}

} // namespace

std::optional<int64_t> handleFsSyscall(SyscallContext &ctx, uint32_t number)
{
	switch (number) {
	case kSysOpenat:
		return sysOpenat(ctx);
	case kSysClose:
		return sysClose(ctx);
	case kSysRead:
		return sysRead(ctx);
	case kSysWrite:
		return sysWrite(ctx);
	case kSysPread64:
		return sysPread64(ctx);
	case kSysLseek:
		return sysLseek(ctx);
	case kSysFaccessat:
	case kSysFaccessat2:
		return sysFaccessat(ctx);
	case kSysNewfstatat:
		return sysNewfstatat(ctx);
	case kSysFstat:
		return sysFstat(ctx);
	case kSysMkdirat:
		return sysMkdirat(ctx);
	case kSysDup:
		return sysDup(ctx);
	case kSysDup3:
		return sysDup3(ctx);
	case kSysFcntl:
		return sysFcntl(ctx);
	default:
		return std::nullopt;
	}
}

} // namespace duckemu::runtime::svc
