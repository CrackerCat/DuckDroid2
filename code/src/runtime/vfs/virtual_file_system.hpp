#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/error/result.hpp"
#include "common/id/ids.hpp"
#include "runtime/vfs/fd_table.hpp"
#include "runtime/vfs/file.hpp"
#include "runtime/vfs/proc_fs.hpp"

namespace duckemu::runtime
{
class RuntimeMemory;
}

namespace duckemu::runtime::vfs
{

struct VfsConfig
{
	uint32_t pid = 1000;
	uint32_t tid = 1;
	std::string progname = "duckemu";
	// Each root is a host directory that represents guest "/". Files are
	// exposed read-only, after canonicalising and checking the result stays
	// inside the root (symlinks may not escape).
	std::vector<std::string> host_roots;
};

// The process-level file system seen by guest syscalls: a descriptor table
// plus the path namespace (/proc/self generated on demand, host roots mounted
// read-only). Not thread-safe by itself; every access happens on the single
// guest CPU under the scheduler's execution lock.
class VirtualFileSystem
{
    public:
	VirtualFileSystem(RuntimeMemory &memory, VfsConfig config);

	VirtualFileSystem(const VirtualFileSystem &) = delete;
	VirtualFileSystem &operator=(const VirtualFileSystem &) = delete;

	void setAuxvProvider(std::function<Result<std::pair<GuestVA, uint64_t> >()> provider)
	{
		auxv_ = std::move(provider);
	}
	void setRegionNameResolver(std::function<std::string(std::string_view)> resolver)
	{
		region_names_ = std::move(resolver);
	}
	void setThreadCountProvider(std::function<uint64_t()> provider)
	{
		thread_count_ = std::move(provider);
	}

	// openat(2). Access is read-only; O_WRONLY/O_RDWR answer EROFS.
	[[nodiscard]] Result<uint64_t> open(uint64_t dirfd, std::string_view path, uint64_t flags);
	// newfstatat(2) with a path. AT_EMPTY_PATH is handled by the caller.
	[[nodiscard]] Result<FileStat> stat(uint64_t dirfd, std::string_view path);
	[[nodiscard]] Result<FileStat> statFd(uint64_t fd);
	// faccessat(2). W_OK/X_OK are refused: the mounted roots are read-only.
	Result<void> access(uint64_t dirfd, std::string_view path, uint64_t mode);

	[[nodiscard]] File *find(uint64_t fd) const
	{
		return fds_.find(fd);
	}
	Result<void> close(uint64_t fd)
	{
		return fds_.remove(fd);
	}

	[[nodiscard]] const VfsConfig &config() const
	{
		return config_;
	}

    private:
	struct HostPath
	{
		std::string path;
		bool directory = false;
	};

	[[nodiscard]] Result<std::string> resolvePath(uint64_t dirfd, std::string_view path) const;
	[[nodiscard]] Result<HostPath> resolveHost(const std::string &guest_path) const;
	[[nodiscard]] Result<std::unique_ptr<File> > openPath(const std::string &guest_path,
		uint64_t flags) const;
	[[nodiscard]] ProcContext procContext() const;

	RuntimeMemory &memory_;
	VfsConfig config_;
	FdTable fds_;
	std::function<std::string(std::string_view)> region_names_;
	std::function<uint64_t()> thread_count_;
	std::function<Result<std::pair<GuestVA, uint64_t> >()> auxv_;
};

} // namespace duckemu::runtime::vfs
