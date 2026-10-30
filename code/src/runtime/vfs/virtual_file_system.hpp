// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/error/result.hpp"
#include "common/id/ids.hpp"
#include "runtime/vfs/fd_table.hpp"
#include "runtime/vfs/file.hpp"
#include "runtime/vfs/proc_fs.hpp"
#include "runtime/vfs/synthetic_file_system.hpp"

namespace duckemu::runtime
{
class RuntimeMemory;
class RuntimeClock;
namespace device
{
struct DeviceProfile;
}
}

namespace duckemu::runtime::vfs
{

// statfs(2) view of one mount (arm64 `struct statfs`, all 64-bit words).
struct FileSystemStats
{
	uint64_t type = 0;
	uint64_t block_size = 4096;
	uint64_t blocks = 0;
	uint64_t blocks_free = 0;
	uint64_t blocks_available = 0;
	uint64_t files = 0;
	uint64_t files_free = 0;
	uint32_t fsid[2] = {};
	uint64_t name_length = 255;
	uint64_t fragment_size = 4096;
	uint64_t flags = 0;
};

// ST_RDONLY in f_flags.
inline constexpr uint64_t kMountReadOnly = 0x1;

struct VfsConfig
{
	uint32_t pid = 1000;
	uint32_t tid = 1;
	std::string progname = "duckemu";
	// Each root is a host directory that represents guest "/". Files are
	// exposed read-only, after canonicalising and checking the result stays
	// inside the root (symlinks may not escape).
	std::vector<std::string> host_roots;

	// Host directory representing guest "/" for writes. It overlays the
	// read-only roots: open/stat try it first. Empty means no writes (EROFS).
	std::string write_root;

	// Owner stamped on every stat result; the device profile supplies the
	// target's app uid.
	uint32_t uid = 1000;
	uint32_t gid = 1000;

	// When wall time is injected, stat timestamps come from it so a replay is
	// byte-identical. Null keeps the host's file times.
	RuntimeClock *clock = nullptr;

	// Device identity for /proc synthesis. Null keeps the built-in defaults.
	const device::DeviceProfile *device = nullptr;

	// Synthetic mounts outside the host roots (/dev, /dev/__properties__).
	// Owners outlive the VFS; later entries win on overlap.
	std::vector<const SyntheticFileSystem *> synthetic_mounts;
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

	// openat(2). Reads see the writable sandbox first, then the read-only
	// roots. Write access, O_CREAT, O_TRUNC and O_APPEND require a writable
	// sandbox; without one they answer EROFS.
	[[nodiscard]] Result<uint64_t> open(uint64_t dirfd,
		std::string_view path,
		uint64_t flags,
		uint64_t mode = 0666);

	// unlinkat(2)/renameat(2). Both only reach inside the writable sandbox;
	// a path that exists solely under a read-only root answers EROFS.
	Result<void> mkdir(uint64_t dirfd, std::string_view path, uint64_t mode);
	Result<void> unlink(uint64_t dirfd, std::string_view path, bool remove_directory);
	Result<void> rename(uint64_t old_dirfd,
		std::string_view old_path,
		uint64_t new_dirfd,
		std::string_view new_path);

	// ftruncate(2)/fsync(2) on an open description.
	Result<void> truncateFd(uint64_t fd, uint64_t size);
	Result<void> syncFd(uint64_t fd);
	// newfstatat(2) with a path. AT_EMPTY_PATH is handled by the caller.
	[[nodiscard]] Result<FileStat> stat(uint64_t dirfd, std::string_view path);
	[[nodiscard]] Result<FileStat> statFd(uint64_t fd);
	// faccessat(2). W_OK/X_OK are refused: the mounted roots are read-only.
	Result<void> access(uint64_t dirfd, std::string_view path, uint64_t mode);

	// readlinkat(2). Only /proc/self magic links are modelled (exe, cwd, root,
	// fd/N); anything else that exists answers InvalidArgument (EINVAL), like
	// the kernel for a non-symlink.
	[[nodiscard]] Result<std::string> readLink(uint64_t dirfd, std::string_view path);

	// statfs(2)/fstatfs(2). Paths are classified into the mounts the device
	// picture describes (/data capacity comes from DeviceProfile).
	[[nodiscard]] Result<FileSystemStats> statfs(uint64_t dirfd, std::string_view path);
	[[nodiscard]] Result<FileSystemStats> statfsFd(uint64_t fd);
	// The view a bare /proc, /dev or /data path would get; used by handlers
	// for descriptor kinds that carry no path.
	[[nodiscard]] FileSystemStats fileSystemForPath(const std::string &guest_path) const;

	[[nodiscard]] File *find(uint64_t fd) const
	{
		return fds_.find(fd);
	}
	Result<void> close(uint64_t fd)
	{
		return fds_.remove(fd);
	}

	// Inserts an already-built description (eventfd/timerfd/epoll instances).
	[[nodiscard]] Result<uint64_t> add(std::unique_ptr<File> file)
	{
		return fds_.insert(std::move(file));
	}

	// Wake source for blocking poll/epoll waits. Every guest-visible read or
	// write notifies the callbacks; each waiter re-evaluates its own readiness
	// and treats a wake without progress as spurious. Ids are used by
	// removeActivityWaiter.
	uint64_t addActivityWaiter(std::function<void()> callback);
	void removeActivityWaiter(uint64_t id);
	void notifyActivity();

	// dup(2) / F_DUPFD: lowest free descriptor >= `minimum` for the same open
	// file description. Standard streams are materialized on demand, so
	// dup(1) yields a descriptor whose write() still reaches the host.
	[[nodiscard]] Result<uint64_t> duplicate(uint64_t fd,
		uint64_t minimum = FdTable::kFirstGuestFd);
	// dup2(2)/dup3(2) onto an exact descriptor. Replacing 0/1/2 is refused:
	// they are the host's, and the syscall handlers address them by number.
	[[nodiscard]] Result<void> duplicateTo(uint64_t fd, uint64_t target);

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

	// Host path of `guest_path` inside the writable sandbox. Canonicalises the
	// parent (never the leaf, which may not exist yet) and refuses escapes.
	[[nodiscard]] Result<std::string> resolveWritePath(const std::string &guest_path,
		bool create_parents) const;
	// Stamps the configured owner and, when wall time is injected, the
	// replayed timestamps onto a stat result.
	void applyOwner(FileStat &stat) const;
	// Open file description behind `fd`. Standard streams get a persistent
	// StdioFile so repeated dups share it.
	[[nodiscard]] Result<std::shared_ptr<File> > describe(uint64_t fd);
	[[nodiscard]] Result<std::string> resolvePath(uint64_t dirfd, std::string_view path) const;
	[[nodiscard]] Result<HostPath> resolveHost(const std::string &guest_path) const;
	[[nodiscard]] Result<std::unique_ptr<File> > openPath(const std::string &guest_path,
		uint64_t flags) const;
	// First synthetic mount that answers for this path (later mounts win).
	[[nodiscard]] std::optional<SyntheticNode> findSynthetic(std::string_view guest_path) const;
	// Effective guest mode of a synthetic node.
	[[nodiscard]] static uint32_t syntheticMode(const SyntheticNode &node);
	// readlink target of an open descriptor (/proc/self/fd/N).
	[[nodiscard]] Result<std::string> fileLink(File &file);
	[[nodiscard]] ProcContext procContext() const;

	RuntimeMemory &memory_;
	VfsConfig config_;
	FdTable fds_;
	std::array<std::shared_ptr<StdioFile>, 3> stdio_;

	// Guarded: notifyActivity runs on the writing guest thread while a waiter
	// may be registering or being destroyed on its own thread.
	std::mutex waiters_mutex_;
	uint64_t next_waiter_id_ = 1;
	std::unordered_map<uint64_t, std::function<void()> > activity_waiters_;
	std::function<std::string(std::string_view)> region_names_;
	std::function<uint64_t()> thread_count_;
	std::function<Result<std::pair<GuestVA, uint64_t> >()> auxv_;
};

} // namespace duckemu::runtime::vfs
