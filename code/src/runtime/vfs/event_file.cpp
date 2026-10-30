// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/vfs/event_file.hpp"

#include <algorithm>
#include <cstring>
#include <sys/stat.h>

namespace duckemu::runtime::vfs
{
namespace
{

constexpr uint64_t kReadSize = 8;

Result<void> requireEightBytes(uint64_t count)
{
	if (count < kReadSize)
	{
		return Error{ ErrorCode::InvalidArgument, "an eventfd/timerfd transfer is 8 bytes" };
	}
	return {};
}

FileStat anonStat(const char *path, uint64_t size)
{
	FileStat result;
	result.inode = std::hash<std::string>{}(path);
	result.mode = S_IFREG | 0600;
	result.size = size;
	result.blocks = (size + 511) / 512;
	result.block_size = 4096;
	return result;
}

} // namespace

EventFile::EventFile(uint32_t initial, bool semaphore)
	: path_("/dev/eventfd")
	, counter_(initial)
	, semaphore_(semaphore)
{
}

uint32_t EventFile::pollEvents() const
{
	uint32_t events = 0;
	if (counter_ > 0)
	{
		events |= kPollIn;
	}
	if (counter_ < kMaxCount)
	{
		events |= kPollOut;
	}
	return events;
}

Result<FileStat> EventFile::stat()
{
	return anonStat(path_.c_str(), 0);
}

Result<uint64_t> EventFile::read(void *buffer, uint64_t count)
{
	if (auto enough = requireEightBytes(count); !enough.ok())
	{
		return enough.error();
	}
	if (counter_ == 0)
	{
		return Error{ ErrorCode::WouldBlock, "eventfd is empty" };
	}
	uint64_t value = counter_;
	if (semaphore_)
	{
		value = 1;
		counter_ -= 1;
	}
	else
	{
		counter_ = 0;
	}
	std::memcpy(buffer, &value, sizeof(value));
	return kReadSize;
}

Result<uint64_t> EventFile::readAt(uint64_t, void *, uint64_t)
{
	return Error{ ErrorCode::NotSeekable, "pread on an eventfd" };
}

Result<uint64_t> EventFile::write(const void *buffer, uint64_t count)
{
	if (auto enough = requireEightBytes(count); !enough.ok())
	{
		return enough.error();
	}
	uint64_t value = 0;
	std::memcpy(&value, buffer, sizeof(value));
	if (value == UINT64_MAX)
	{
		return Error{ ErrorCode::InvalidArgument, "eventfd write value -1 is reserved" };
	}
	if (value > kMaxCount - counter_)
	{
		return Error{ ErrorCode::WouldBlock, "eventfd counter overflow" };
	}
	counter_ += value;
	return kReadSize;
}

Result<uint64_t> EventFile::seek(int64_t, int)
{
	return Error{ ErrorCode::NotSeekable, "lseek on an eventfd" };
}

TimerFile::TimerFile()
	: path_("/dev/timerfd")
{
}

Result<void> TimerFile::setTimer(uint64_t value_seconds,
	uint64_t value_nanoseconds,
	uint64_t interval_seconds,
	uint64_t interval_nanoseconds,
	bool absolute)
{
	if (value_nanoseconds >= 1000000000ULL || interval_nanoseconds >= 1000000000ULL)
	{
		return Error{ ErrorCode::InvalidArgument, "timerfd nanosecond field out of range" };
	}
	const uint64_t value_ns = value_seconds * 1000000000ULL + value_nanoseconds;
	interval_ns_ = interval_seconds * 1000000000ULL + interval_nanoseconds;
	if (value_ns == 0)
	{
		armed_ = false;
		return {};
	}
	// Absolute deadlines are treated as offsets from "now" on the host steady
	// clock: the guest's injected wall clock is not a scheduling source.
	deadline_ = std::chrono::steady_clock::now() + std::chrono::nanoseconds(value_ns);
	armed_ = true;
	if (absolute)
	{
		// A guest absolute timestamp is meaningless to the host clock; the
		// relative distance is preserved by the caller's conversion.
	}
	return {};
}

uint64_t TimerFile::remainingNs() const
{
	refresh();
	if (!armed_)
	{
		return 0;
	}
	const auto now = std::chrono::steady_clock::now();
	if (deadline_ <= now)
	{
		return 0;
	}
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(deadline_ - now).count());
}

void TimerFile::getTimer(uint64_t &value_seconds,
	uint64_t &value_nanoseconds,
	uint64_t &interval_seconds,
	uint64_t &interval_nanoseconds) const
{
	refresh();
	uint64_t remaining_ns = 0;
	if (armed_)
	{
		const auto now = std::chrono::steady_clock::now();
		if (deadline_ > now)
		{
			remaining_ns = static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(deadline_ - now).count());
		}
	}
	value_seconds = remaining_ns / 1000000000ULL;
	value_nanoseconds = remaining_ns % 1000000000ULL;
	interval_seconds = interval_ns_ / 1000000000ULL;
	interval_nanoseconds = interval_ns_ % 1000000000ULL;
}

void TimerFile::refresh() const
{
	if (!armed_)
	{
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	if (now < deadline_)
	{
		return;
	}
	if (interval_ns_ == 0)
	{
		++expirations_;
		armed_ = false;
		return;
	}
	const uint64_t late_ns = static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(now - deadline_).count());
	const uint64_t periods = 1 + late_ns / interval_ns_;
	expirations_ += periods;
	deadline_ += std::chrono::nanoseconds(interval_ns_ * periods);
}

uint32_t TimerFile::pollEvents() const
{
	refresh();
	return expirations_ > 0 ? kPollIn : 0;
}

Result<FileStat> TimerFile::stat()
{
	return anonStat(path_.c_str(), 0);
}

Result<uint64_t> TimerFile::read(void *buffer, uint64_t count)
{
	if (auto enough = requireEightBytes(count); !enough.ok())
	{
		return enough.error();
	}
	refresh();
	if (expirations_ == 0)
	{
		return Error{ ErrorCode::WouldBlock, "timer has not expired" };
	}
	const uint64_t value = expirations_;
	expirations_ = 0;
	std::memcpy(buffer, &value, sizeof(value));
	return kReadSize;
}

Result<uint64_t> TimerFile::readAt(uint64_t, void *, uint64_t)
{
	return Error{ ErrorCode::NotSeekable, "pread on a timerfd" };
}

Result<uint64_t> TimerFile::write(const void *, uint64_t)
{
	return Error{ ErrorCode::InvalidArgument, "timerfd is not writable" };
}

Result<uint64_t> TimerFile::seek(int64_t, int)
{
	return Error{ ErrorCode::NotSeekable, "lseek on a timerfd" };
}

PipeFile::PipeFile(std::shared_ptr<PipeBuffer> buffer, bool readable_end)
	: path_(readable_end ? "/dev/pipe-read" : "/dev/pipe-write")
	, buffer_(std::move(buffer))
	, readable_end_(readable_end)
{
}

uint32_t PipeFile::pollEvents() const
{
	if (readable_end_)
	{
		return buffer_->data.empty() ? 0 : kPollIn;
	}
	return buffer_->data.size() < PipeBuffer::kCapacity ? kPollOut : 0;
}

Result<FileStat> PipeFile::stat()
{
	FileStat result;
	result.inode = std::hash<std::string>{}(path_);
	result.mode = S_IFIFO | 0600;
	result.block_size = 4096;
	return result;
}

Result<uint64_t> PipeFile::read(void *buffer, uint64_t count)
{
	if (!readable_end_)
	{
		return Error{ ErrorCode::BadFileDescriptor, "writing end of the pipe" };
	}
	if (buffer_->data.empty())
	{
		// A real blocking pipe would sleep here; poll(2) is the supported
		// way to wait, so an empty pipe reports EAGAIN.
		return Error{ ErrorCode::WouldBlock, "pipe is empty" };
	}
	const uint64_t take = std::min<uint64_t>(count, buffer_->data.size());
	std::memcpy(buffer, buffer_->data.data(), static_cast<size_t>(take));
	buffer_->data.erase(buffer_->data.begin(),
		buffer_->data.begin() + static_cast<std::ptrdiff_t>(take));
	return take;
}

Result<uint64_t> PipeFile::readAt(uint64_t, void *, uint64_t)
{
	return Error{ ErrorCode::NotSeekable, "pread on a pipe" };
}

Result<uint64_t> PipeFile::write(const void *buffer, uint64_t count)
{
	if (readable_end_)
	{
		return Error{ ErrorCode::BadFileDescriptor, "reading end of the pipe" };
	}
	const uint64_t space = PipeBuffer::kCapacity - buffer_->data.size();
	const uint64_t take = std::min<uint64_t>(count, space);
	if (take == 0)
	{
		return Error{ ErrorCode::WouldBlock, "pipe is full" };
	}
	const auto *bytes = static_cast<const uint8_t *>(buffer);
	buffer_->data.insert(buffer_->data.end(), bytes, bytes + take);
	return take;
}

Result<uint64_t> PipeFile::seek(int64_t, int)
{
	return Error{ ErrorCode::NotSeekable, "lseek on a pipe" };
}

EpollFile::ControlResult EpollFile::control(ControlOp op,
	uint64_t fd,
	uint32_t events,
	uint64_t data)
{
	auto position = std::find_if(registrations_.begin(),
		registrations_.end(),
		[fd](const EpollRegistration &entry) { return entry.fd == fd; });
	switch (op)
	{
	case ControlOp::Add:
		if (position != registrations_.end())
		{
			return ControlResult::Exists;
		}
		registrations_.push_back(EpollRegistration{ fd, events, data });
		return ControlResult::Ok;
	case ControlOp::Delete:
		if (position == registrations_.end())
		{
			return ControlResult::Missing;
		}
		registrations_.erase(position);
		return ControlResult::Ok;
	case ControlOp::Modify:
		if (position == registrations_.end())
		{
			return ControlResult::Missing;
		}
		position->events = events;
		position->data = data;
		return ControlResult::Ok;
	}
	return ControlResult::Missing;
}

Result<FileStat> EpollFile::stat()
{
	return anonStat(path_.c_str(), 0);
}

Result<uint64_t> EpollFile::read(void *, uint64_t)
{
	return Error{ ErrorCode::InvalidArgument, "an epoll fd cannot be read" };
}

Result<uint64_t> EpollFile::readAt(uint64_t, void *, uint64_t)
{
	return Error{ ErrorCode::InvalidArgument, "an epoll fd cannot be read" };
}

Result<uint64_t> EpollFile::write(const void *, uint64_t)
{
	return Error{ ErrorCode::InvalidArgument, "an epoll fd cannot be written" };
}

Result<uint64_t> EpollFile::seek(int64_t, int)
{
	return Error{ ErrorCode::NotSeekable, "epoll fds are not seekable" };
}

} // namespace duckemu::runtime::vfs
