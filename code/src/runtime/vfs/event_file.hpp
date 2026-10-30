// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/vfs/file.hpp"

namespace duckemu::runtime::vfs
{

// eventfd(2): a 64-bit counter. read returns it and clears it (or takes one
// with EFD_SEMAPHORE); write adds. A zero counter reports not-readable, which
// is what poll/epoll waits on.
class EventFile final : public File
{
    public:
	static constexpr uint64_t kMaxCount = 0xFFFFFFFFFFFFFFFEULL;

	EventFile(uint32_t initial, bool semaphore);

	[[nodiscard]] std::string_view path() const override
	{
		return path_;
	}
	[[nodiscard]] bool writable() const override
	{
		return true;
	}
	[[nodiscard]] FileKind kind() const override
	{
		return FileKind::Event;
	}
	[[nodiscard]] uint32_t pollEvents() const override;
	[[nodiscard]] Result<FileStat> stat() override;
	[[nodiscard]] Result<uint64_t> read(void *buffer, uint64_t count) override;
	[[nodiscard]] Result<uint64_t> readAt(uint64_t offset, void *buffer, uint64_t count) override;
	[[nodiscard]] Result<uint64_t> write(const void *buffer, uint64_t count) override;
	[[nodiscard]] Result<uint64_t> seek(int64_t offset, int whence) override;

    private:
	std::string path_;
	uint64_t counter_ = 0;
	bool semaphore_ = false;
};

// timerfd(2): expirations accumulate in the counter; read drains it. Timers
// run on the host steady clock, since they have to fire in real time to wake a
// blocked poll.
class TimerFile final : public File
{
    public:
	TimerFile();

	// timerfd_settime: a relative or absolute (`absolute`) deadline plus an
	// optional interval, in nanoseconds.
	Result<void> setTimer(uint64_t value_seconds,
		uint64_t value_nanoseconds,
		uint64_t interval_seconds,
		uint64_t interval_nanoseconds,
		bool absolute);
	// Nanoseconds until the next expiration; 0 when disarmed or already due.
	[[nodiscard]] uint64_t remainingNs() const;
	// timerfd_gettime.
	void getTimer(uint64_t &value_seconds,
		uint64_t &value_nanoseconds,
		uint64_t &interval_seconds,
		uint64_t &interval_nanoseconds) const;

	[[nodiscard]] std::string_view path() const override
	{
		return path_;
	}
	[[nodiscard]] bool writable() const override
	{
		return false;
	}
	[[nodiscard]] FileKind kind() const override
	{
		return FileKind::Timer;
	}
	[[nodiscard]] uint32_t pollEvents() const override;
	[[nodiscard]] Result<FileStat> stat() override;
	[[nodiscard]] Result<uint64_t> read(void *buffer, uint64_t count) override;
	[[nodiscard]] Result<uint64_t> readAt(uint64_t offset, void *buffer, uint64_t count) override;
	[[nodiscard]] Result<uint64_t> write(const void *buffer, uint64_t count) override;
	[[nodiscard]] Result<uint64_t> seek(int64_t offset, int whence) override;

    private:
	using Clock = std::chrono::steady_clock;

	// Folds elapsed periods into expirations_. Const because pollEvents is.
	void refresh() const;

	std::string path_;
	mutable bool armed_ = false;
	mutable Clock::time_point deadline_;
	mutable uint64_t interval_ns_ = 0;
	mutable uint64_t expirations_ = 0;
};

// Shared state of a pipe pair. The buffer is bounded; a full buffer clears
// POLLOUT on the write end.
struct PipeBuffer
{
	static constexpr size_t kCapacity = 64 * 1024;
	std::vector<uint8_t> data;
};

// One end of a pipe(fd). `readable_end` decides which direction works; the
// other direction answers EBADF, like a real descriptor.
class PipeFile final : public File
{
    public:
	PipeFile(std::shared_ptr<PipeBuffer> buffer, bool readable_end);

	[[nodiscard]] std::string_view path() const override
	{
		return path_;
	}
	[[nodiscard]] bool writable() const override
	{
		return !readable_end_;
	}
	[[nodiscard]] FileKind kind() const override
	{
		return FileKind::Pipe;
	}
	[[nodiscard]] uint32_t pollEvents() const override;
	[[nodiscard]] Result<FileStat> stat() override;
	[[nodiscard]] Result<uint64_t> read(void *buffer, uint64_t count) override;
	[[nodiscard]] Result<uint64_t> readAt(uint64_t offset, void *buffer, uint64_t count) override;
	[[nodiscard]] Result<uint64_t> write(const void *buffer, uint64_t count) override;
	[[nodiscard]] Result<uint64_t> seek(int64_t offset, int whence) override;

    private:
	std::string path_;
	std::shared_ptr<PipeBuffer> buffer_;
	bool readable_end_ = true;
};

struct EpollRegistration
{
	uint64_t fd = 0;
	uint32_t events = 0;
	uint64_t data = 0;
};

// epoll(7) instance. Registration only: readiness is evaluated by the
// syscall handler against the descriptor table, because the set may contain
// descriptors this object cannot see.
class EpollFile final : public File
{
    public:
	enum class ControlOp : int
	{
		Add = 1,
		Delete = 2,
		Modify = 3,
	};
	enum class ControlResult : uint8_t
	{
		Ok,
		Exists,
		Missing,
	};

	EpollFile() = default;

	ControlResult control(ControlOp op, uint64_t fd, uint32_t events, uint64_t data);

	[[nodiscard]] const std::vector<EpollRegistration> &registrations() const
	{
		return registrations_;
	}

	[[nodiscard]] std::string_view path() const override
	{
		return path_;
	}
	[[nodiscard]] bool writable() const override
	{
		return false;
	}
	[[nodiscard]] FileKind kind() const override
	{
		return FileKind::Epoll;
	}
	// An epoll descriptor is not pollable itself.
	[[nodiscard]] uint32_t pollEvents() const override
	{
		return 0;
	}
	[[nodiscard]] Result<FileStat> stat() override;
	[[nodiscard]] Result<uint64_t> read(void *buffer, uint64_t count) override;
	[[nodiscard]] Result<uint64_t> readAt(uint64_t offset, void *buffer, uint64_t count) override;
	[[nodiscard]] Result<uint64_t> write(const void *buffer, uint64_t count) override;
	[[nodiscard]] Result<uint64_t> seek(int64_t offset, int whence) override;

    private:
	std::string path_ = "/dev/epoll";
	std::vector<EpollRegistration> registrations_;
};

} // namespace duckemu::runtime::vfs
