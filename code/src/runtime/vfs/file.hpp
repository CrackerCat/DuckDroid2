#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "common/error/result.hpp"

namespace duckemu::runtime::vfs
{

// File metadata in guest-neutral terms. fs_handlers encodes it into the arm64
// kernel `struct stat` layout.
struct FileStat
{
	uint64_t device = 0;
	uint64_t inode = 0;
	uint32_t mode = 0; // S_IF* | permission bits
	uint32_t link_count = 1;
	uint32_t uid = 1000;
	uint32_t gid = 1000;
	uint64_t size = 0;
	uint64_t blocks = 0;
	uint32_t block_size = 4096;
	int64_t atime_seconds = 0;
	int64_t atime_nanoseconds = 0;
	int64_t mtime_seconds = 0;
	int64_t mtime_nanoseconds = 0;
	int64_t ctime_seconds = 0;
	int64_t ctime_nanoseconds = 0;
};

// One open file description. The read offset lives here, so it will be shared
// by dup'ed descriptors once dup exists.
class File
{
    public:
	virtual ~File() = default;

	[[nodiscard]] virtual std::string_view path() const = 0;
	[[nodiscard]] virtual bool writable() const = 0;

	[[nodiscard]] virtual Result<FileStat> stat() = 0;
	[[nodiscard]] virtual Result<uint64_t> read(void *buffer, uint64_t count) = 0;
	[[nodiscard]] virtual Result<uint64_t> readAt(uint64_t offset, void *buffer, uint64_t count) = 0;
	[[nodiscard]] virtual Result<uint64_t> write(const void *buffer, uint64_t count) = 0;
	[[nodiscard]] virtual Result<uint64_t> seek(int64_t offset, int whence) = 0;
};

// Generated read-only file (/proc). The content is produced on first use and
// then frozen for the lifetime of the open description, like a seq_file that
// captured its snapshot when reading started.
class MemoryFile final : public File
{
    public:
	using Generator = std::function<std::string()>;

	MemoryFile(std::string path, Generator generator);

	[[nodiscard]] std::string_view path() const override
	{
		return path_;
	}
	[[nodiscard]] bool writable() const override
	{
		return false;
	}
	[[nodiscard]] Result<FileStat> stat() override;
	[[nodiscard]] Result<uint64_t> read(void *buffer, uint64_t count) override;
	[[nodiscard]] Result<uint64_t> readAt(uint64_t offset, void *buffer, uint64_t count) override;
	[[nodiscard]] Result<uint64_t> write(const void *buffer, uint64_t count) override;
	[[nodiscard]] Result<uint64_t> seek(int64_t offset, int whence) override;

    private:
	const std::string &materialize();

	std::string path_;
	Generator generator_;
	std::optional<std::string> content_;
	uint64_t offset_ = 0;
};

// Read-only view of one host file. M4.6 exposes host directories read-only;
// writable files arrive with the north-star target (M6).
class HostFile final : public File
{
    public:
	static Result<std::unique_ptr<HostFile> > open(std::string guest_path, std::string host_path);

	~HostFile() override;
	HostFile(const HostFile &) = delete;
	HostFile &operator=(const HostFile &) = delete;

	[[nodiscard]] std::string_view path() const override
	{
		return guest_path_;
	}
	[[nodiscard]] bool writable() const override
	{
		return false;
	}
	[[nodiscard]] Result<FileStat> stat() override;
	[[nodiscard]] Result<uint64_t> read(void *buffer, uint64_t count) override;
	[[nodiscard]] Result<uint64_t> readAt(uint64_t offset, void *buffer, uint64_t count) override;
	[[nodiscard]] Result<uint64_t> write(const void *buffer, uint64_t count) override;
	[[nodiscard]] Result<uint64_t> seek(int64_t offset, int whence) override;

    private:
	HostFile(std::string guest_path, int fd, uint64_t size);

	std::string guest_path_;
	int fd_ = -1;
	uint64_t size_ = 0;
	uint64_t offset_ = 0;
};

} // namespace duckemu::runtime::vfs
