// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/observer/trace/buffered_file.hpp"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

namespace duckemu::runtime::observer
{

BufferedFile::BufferedFile(int fd)
	: fd_(fd)
	, buffer_(std::make_unique<char[]>(kCapacity))
{
}

BufferedFile::~BufferedFile()
{
	close();
}

Result<std::unique_ptr<BufferedFile> > BufferedFile::open(std::string_view path)
{
	const std::string file_path(path);
	const int fd = ::open(file_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
	{
		return Error{ ErrorCode::NotFound,
			"cannot open trace file " + file_path + ": " + std::strerror(errno) };
	}
	return std::unique_ptr<BufferedFile>(new BufferedFile(fd));
}

void BufferedFile::write(std::string_view text)
{
	write(text.data(), text.size());
}

void BufferedFile::write(const char *data, size_t size)
{
	if (failed_ || fd_ < 0 || size == 0)
	{
		return;
	}
	if (size >= kCapacity)
	{
		flush();
		const char *cursor = data;
		size_t remaining = size;
		while (remaining > 0)
		{
			const ssize_t written = ::write(fd_, cursor, remaining);
			if (written < 0)
			{
				if (errno == EINTR)
				{
					continue;
				}
				failed_ = true;
				return;
			}
			if (written == 0)
			{
				failed_ = true;
				return;
			}
			cursor += written;
			remaining -= static_cast<size_t>(written);
		}
		return;
	}
	if (used_ + size > kCapacity)
	{
		flush();
	}
	std::memcpy(buffer_.get() + used_, data, size);
	used_ += size;
}

void BufferedFile::flush()
{
	if (fd_ < 0 || used_ == 0)
	{
		return;
	}
	const char *cursor = buffer_.get();
	size_t remaining = used_;
	while (remaining > 0)
	{
		const ssize_t written = ::write(fd_, cursor, remaining);
		if (written < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			failed_ = true;
			break;
		}
		if (written == 0)
		{
			failed_ = true;
			break;
		}
		cursor += written;
		remaining -= static_cast<size_t>(written);
	}
	used_ = 0;
}

void BufferedFile::close()
{
	if (fd_ < 0)
	{
		return;
	}
	flush();
	(void)::close(fd_);
	fd_ = -1;
}

} // namespace duckemu::runtime::observer
