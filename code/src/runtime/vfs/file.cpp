// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/vfs/file.hpp"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace duckemu::runtime::vfs
{
namespace
{

Error ioError(std::string message)
{
	return Error{ ErrorCode::Internal, std::move(message) };
}

} // namespace

MemoryFile::MemoryFile(std::string path, Generator generator)
	: path_(std::move(path))
	, generator_(std::move(generator))
{
}

const std::string &MemoryFile::materialize()
{
	if (!content_.has_value())
	{
		content_ = generator_ ? generator_() : std::string();
	}
	return *content_;
}

Result<FileStat> MemoryFile::stat()
{
	FileStat result;
	result.inode = std::hash<std::string>{}(path_);
	result.mode = S_IFREG | 0444;
	result.size = materialize().size();
	result.blocks = (result.size + 511) / 512;
	return result;
}

Result<uint64_t> MemoryFile::read(void *buffer, uint64_t count)
{
	auto result = readAt(offset_, buffer, count);
	if (result.ok())
	{
		offset_ += result.value();
	}
	return result;
}

Result<uint64_t> MemoryFile::readAt(uint64_t offset, void *buffer, uint64_t count)
{
	const std::string &content = materialize();
	if (offset >= content.size() || count == 0)
	{
		return uint64_t{ 0 };
	}
	const uint64_t available = content.size() - offset;
	const uint64_t take = std::min(count, available);
	std::memcpy(buffer, content.data() + offset, static_cast<size_t>(take));
	return take;
}

Result<uint64_t> MemoryFile::write(const void *, uint64_t)
{
	return Error{ ErrorCode::Unsupported, path_ + " is read-only" };
}

Result<uint64_t> MemoryFile::seek(int64_t offset, int whence)
{
	int64_t base = 0;
	switch (whence)
	{
	case 0: // SEEK_SET
		break;
	case 1: // SEEK_CUR
		base = static_cast<int64_t>(offset_);
		break;
	case 2: // SEEK_END
		base = static_cast<int64_t>(materialize().size());
		break;
	default:
		return Error{ ErrorCode::InvalidArgument, "invalid whence" };
	}
	const int64_t target = base + offset;
	if (target < 0)
	{
		return Error{ ErrorCode::InvalidArgument, "negative file offset" };
	}
	offset_ = static_cast<uint64_t>(target);
	return offset_;
}

HostFile::HostFile(std::string guest_path, int fd, uint64_t size)
	: guest_path_(std::move(guest_path))
	, fd_(fd)
	, size_(size)
{
}

HostFile::~HostFile()
{
	if (fd_ >= 0)
	{
		::close(fd_);
	}
}

Result<std::unique_ptr<HostFile> > HostFile::open(std::string guest_path, std::string host_path)
{
	const int fd = ::open(host_path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0)
	{
		return Error{ ErrorCode::Internal, "cannot open " + host_path };
	}
	struct stat info = {};
	if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode))
	{
		::close(fd);
		return Error{ ErrorCode::Internal, "cannot stat " + host_path };
	}
	return std::unique_ptr<HostFile>(new HostFile(std::move(guest_path), fd,
		static_cast<uint64_t>(info.st_size)));
}

Result<FileStat> HostFile::stat()
{
	struct stat info = {};
	if (::fstat(fd_, &info) != 0)
	{
		return ioError("fstat failed");
	}
	FileStat result;
	result.device = static_cast<uint64_t>(info.st_dev);
	result.inode = static_cast<uint64_t>(info.st_ino);
	result.mode = static_cast<uint32_t>(info.st_mode);
	result.link_count = static_cast<uint32_t>(info.st_nlink);
	result.uid = static_cast<uint32_t>(info.st_uid);
	result.gid = static_cast<uint32_t>(info.st_gid);
	result.size = static_cast<uint64_t>(info.st_size);
	result.blocks = static_cast<uint64_t>(info.st_blocks);
	result.block_size = static_cast<uint32_t>(info.st_blksize);
	result.atime_seconds = info.st_atim.tv_sec;
	result.atime_nanoseconds = info.st_atim.tv_nsec;
	result.mtime_seconds = info.st_mtim.tv_sec;
	result.mtime_nanoseconds = info.st_mtim.tv_nsec;
	result.ctime_seconds = info.st_ctim.tv_sec;
	result.ctime_nanoseconds = info.st_ctim.tv_nsec;
	return result;
}

Result<uint64_t> HostFile::read(void *buffer, uint64_t count)
{
	auto result = readAt(offset_, buffer, count);
	if (result.ok())
	{
		offset_ += result.value();
	}
	return result;
}

Result<uint64_t> HostFile::readAt(uint64_t offset, void *buffer, uint64_t count)
{
	if (count == 0)
	{
		return uint64_t{ 0 };
	}
	const ssize_t received = ::pread(fd_, buffer, static_cast<size_t>(count),
		static_cast<off_t>(offset));
	if (received < 0)
	{
		return ioError("pread failed");
	}
	return static_cast<uint64_t>(received);
}

Result<uint64_t> HostFile::write(const void *, uint64_t)
{
	return Error{ ErrorCode::Unsupported, guest_path_ + " is read-only" };
}

Result<uint64_t> HostFile::seek(int64_t offset, int whence)
{
	int64_t base = 0;
	switch (whence)
	{
	case 0: // SEEK_SET
		break;
	case 1: // SEEK_CUR
		base = static_cast<int64_t>(offset_);
		break;
	case 2: // SEEK_END
		base = static_cast<int64_t>(size_);
		break;
	default:
		return Error{ ErrorCode::InvalidArgument, "invalid whence" };
	}
	const int64_t target = base + offset;
	if (target < 0)
	{
		return Error{ ErrorCode::InvalidArgument, "negative file offset" };
	}
	offset_ = static_cast<uint64_t>(target);
	return offset_;
}

} // namespace duckemu::runtime::vfs
