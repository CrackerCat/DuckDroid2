// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/vfs/virtual_file_system.hpp"

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "runtime/clock/runtime_clock.hpp"

namespace duckemu::runtime::vfs
{
namespace
{

// Guest-visible constants (Linux arm64). The host's values match, but spelling
// them out keeps the guest ABI independent of the build host.
constexpr int64_t kAtFdcwd = -100;
constexpr uint64_t kAccModeMask = 3;
constexpr uint64_t kODirectory = 0x10000;
constexpr uint64_t kOCreate = 0x40;
constexpr uint64_t kOExclusive = 0x80;
constexpr uint64_t kOTruncate = 0x200;
constexpr uint64_t kOAppend = 0x400;
constexpr uint64_t kAccessWrite = 2;
constexpr uint64_t kAccessExecute = 1;
constexpr uint64_t kAccessRead = 4;

Result<std::string> normalizePath(std::string_view path)
{
	std::vector<std::string_view> parts;
	size_t begin = 0;
	while (begin <= path.size())
	{
		size_t end = path.find('/', begin);
		if (end == std::string_view::npos)
		{
			end = path.size();
		}
		const std::string_view part = path.substr(begin, end - begin);
		if (!part.empty() && part != ".")
		{
			if (part == "..")
			{
				// The parent of "/" is "/"; above-root components are dropped.
				if (!parts.empty())
				{
					parts.pop_back();
				}
			}
			else
			{
				parts.push_back(part);
			}
		}
		if (end == path.size())
		{
			break;
		}
		begin = end + 1;
	}

	std::string result;
	for (const std::string_view part : parts)
	{
		result += '/';
		result += part;
	}
	return result.empty() ? std::string("/") : result;
}

bool isWithin(const std::filesystem::path &path, const std::filesystem::path &root)
{
	const std::string candidate = path.string();
	const std::string parent = root.string();
	if (candidate == parent)
	{
		return true;
	}
	if (candidate.size() <= parent.size() || candidate.compare(0, parent.size(), parent) != 0)
	{
		return false;
	}
	return candidate[parent.size()] == '/' || parent.back() == '/';
}

Error errnoToError(int error_number, std::string message)
{
	switch (error_number)
	{
	case ENOENT:
		return Error{ ErrorCode::NotFound, std::move(message) };
	case EACCES:
	case EPERM:
	case EROFS:
		return Error{ ErrorCode::PermissionDenied, std::move(message) };
	case EISDIR:
		return Error{ ErrorCode::IsDirectory, std::move(message) };
	case ENOTDIR:
		return Error{ ErrorCode::NotDirectory, std::move(message) };
	case EEXIST:
	case ENOTEMPTY:
		return Error{ ErrorCode::AlreadyExists, std::move(message) };
	default:
		return Error{ ErrorCode::Internal, std::move(message) };
	}
}

FileStat toFileStat(const struct stat &info)
{
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

} // namespace

VirtualFileSystem::VirtualFileSystem(RuntimeMemory &memory, VfsConfig config)
	: memory_(memory)
	, config_(std::move(config))
{
}

Result<std::string> VirtualFileSystem::resolvePath(uint64_t dirfd, std::string_view path) const
{
	if (path.empty())
	{
		return Error{ ErrorCode::NotFound, "empty path" };
	}
	if (path.front() == '/')
	{
		return normalizePath(path);
	}
	if (static_cast<int64_t>(dirfd) != kAtFdcwd)
	{
		// Directory descriptors are not modelled yet; kernels answer EBADF for
		// a dirfd that is not open.
		return Error{ ErrorCode::BadFileDescriptor, "relative path against a dirfd" };
	}
	std::string combined = "/";
	combined.append(path);
	return normalizePath(combined);
}

Result<std::string> VirtualFileSystem::resolveWritePath(const std::string &guest_path,
	bool create_parents) const
{
	namespace fs = std::filesystem;
	if (config_.write_root.empty())
	{
		return Error{ ErrorCode::Unsupported, "no writable sandbox" };
	}
	std::error_code error;
	const fs::path root = fs::weakly_canonical(fs::path(config_.write_root), error);
	if (error)
	{
		return Error{ ErrorCode::Internal, "writable sandbox is unreadable" };
	}
	const fs::path candidate = root / guest_path.substr(1);
	if (create_parents)
	{
		fs::create_directories(candidate.parent_path(), error);
		if (error)
		{
			return Error{ ErrorCode::Internal, "cannot create the parent of " + guest_path };
		}
	}
	// Only the parent is canonicalised: the leaf may not exist yet, but its
	// directory must still resolve inside the sandbox after symlinks.
	const fs::path parent = fs::weakly_canonical(candidate.parent_path(), error);
	if (error)
	{
		return Error{ ErrorCode::NotFound, "cannot resolve the parent of " + guest_path };
	}
	const fs::path leaf = candidate.filename();
	if (leaf.empty() || leaf == "." || leaf == "..")
	{
		return Error{ ErrorCode::IsDirectory, guest_path + " is a directory" };
	}
	const fs::path full = parent / leaf;
	if (!isWithin(full, root))
	{
		return Error{ ErrorCode::PermissionDenied, guest_path + " escapes the sandbox" };
	}
	return full.string();
}

void VirtualFileSystem::applyOwner(FileStat &stat) const
{
	stat.uid = config_.uid;
	stat.gid = config_.gid;
	// An injected wall clock makes every timestamp replay-identical; host mode
	// keeps the real file times.
	if (config_.clock != nullptr && config_.clock->wallTimeInjected())
	{
		const uint64_t ns = config_.clock->wallTimeNs();
		const int64_t seconds = static_cast<int64_t>(ns / 1000000000ULL);
		const int64_t nanoseconds = static_cast<int64_t>(ns % 1000000000ULL);
		stat.atime_seconds = seconds;
		stat.atime_nanoseconds = nanoseconds;
		stat.mtime_seconds = seconds;
		stat.mtime_nanoseconds = nanoseconds;
		stat.ctime_seconds = seconds;
		stat.ctime_nanoseconds = nanoseconds;
	}
}

Result<VirtualFileSystem::HostPath> VirtualFileSystem::resolveHost(const std::string &guest_path) const
{
	namespace fs = std::filesystem;
	std::vector<std::string> roots;
	if (!config_.write_root.empty())
	{
		// The sandbox overlays the read-only roots, so a file created through
		// open(O_CREAT) is visible to every later read and stat.
		roots.push_back(config_.write_root);
	}
	for (const std::string &root : config_.host_roots)
	{
		roots.push_back(root);
	}
	for (const std::string &root : roots)
	{
		if (root.empty())
		{
			continue;
		}
		std::error_code error;
		const fs::path root_path = fs::weakly_canonical(fs::path(root), error);
		if (error)
		{
			continue;
		}
		const fs::path candidate = root_path / guest_path.substr(1);
		const fs::file_status status = fs::status(candidate, error);
		if (error || !fs::exists(status))
		{
			continue;
		}
		const fs::path canonical = fs::weakly_canonical(candidate, error);
		if (error || !isWithin(canonical, root_path))
		{
			continue;
		}
		return HostPath{ canonical.string(), fs::is_directory(status) };
	}
	return Error{ ErrorCode::NotFound, "no such file: " + guest_path };
}

ProcContext VirtualFileSystem::procContext() const
{
	ProcContext context;
	context.pid = config_.pid;
	context.tid = config_.tid;
	context.progname = config_.progname;
	context.memory = &memory_;
	context.resolve_region_name = region_names_;
	context.thread_count = thread_count_;
	context.auxv = auxv_;
	return context;
}

Result<std::unique_ptr<File> > VirtualFileSystem::openPath(const std::string &guest_path,
	uint64_t flags) const
{
	if (isProcPath(guest_path))
	{
		auto generator = makeGenerator(guest_path, procContext());
		if (!generator.has_value())
		{
			return Error{ ErrorCode::NotFound, "no such proc file: " + guest_path };
		}
		if ((flags & kODirectory) != 0)
		{
			return Error{ ErrorCode::NotDirectory, guest_path + " is not a directory" };
		}
		return std::unique_ptr<File>(new MemoryFile(guest_path, std::move(generator.value())));
	}

	auto host = resolveHost(guest_path);
	if (!host.ok())
	{
		return host.error();
	}
	if (host.value().directory)
	{
		return Error{ ErrorCode::IsDirectory, guest_path + " is a directory" };
	}
	if ((flags & kODirectory) != 0)
	{
		return Error{ ErrorCode::NotDirectory, guest_path + " is not a directory" };
	}
	auto opened = HostFile::open(guest_path, host.value().path);
	if (!opened.ok())
	{
		return opened.error();
	}
	return std::unique_ptr<File>(std::move(opened.value()));
}

Result<uint64_t> VirtualFileSystem::open(uint64_t dirfd,
	std::string_view path,
	uint64_t flags,
	uint64_t mode)
{
	auto resolved = resolvePath(dirfd, path);
	if (!resolved.ok())
	{
		return resolved.error();
	}
	const std::string &guest_path = resolved.value();

	const uint64_t access = flags & kAccModeMask;
	const bool writes = access != 0 || (flags & (kOCreate | kOTruncate | kOAppend)) != 0;

	if (!writes)
	{
		auto file = openPath(guest_path, flags);
		if (!file.ok())
		{
			return file.error();
		}
		return fds_.insert(std::move(file.value()));
	}

	if (isProcPath(guest_path))
	{
		return Error{ ErrorCode::Unsupported, "proc files are read-only" };
	}
	if ((flags & kODirectory) != 0)
	{
		return Error{ ErrorCode::NotDirectory, guest_path + " is not a directory" };
	}
	auto host_path = resolveWritePath(guest_path, (flags & kOCreate) != 0);
	if (!host_path.ok())
	{
		return host_path.error();
	}

	int host_flags = (access == 2) ? O_RDWR : (access == 1 ? O_WRONLY : O_RDONLY);
	if ((flags & kOCreate) != 0)
	{
		host_flags |= O_CREAT;
	}
	if ((flags & kOExclusive) != 0)
	{
		host_flags |= O_EXCL;
	}
	if ((flags & kOTruncate) != 0)
	{
		host_flags |= O_TRUNC;
	}
	if ((flags & kOAppend) != 0)
	{
		host_flags |= O_APPEND;
	}

	auto opened = HostFile::openWritable(guest_path,
		host_path.value(),
		host_flags,
		access,
		(flags & kOAppend) != 0,
		mode == 0 ? 0666 : mode);
	if (!opened.ok())
	{
		// The only host-side failures that reach here are permission and
		// missing-directory; the guest sees the correspondents.
		return Error{ ErrorCode::PermissionDenied, "cannot open " + guest_path };
	}
	return fds_.insert(std::move(opened.value()));
}

Result<void> VirtualFileSystem::mkdir(uint64_t dirfd, std::string_view path, uint64_t mode)
{
	auto resolved = resolvePath(dirfd, path);
	if (!resolved.ok())
	{
		return resolved.error();
	}
	// The sandbox is a host directory tree, so creating one level needs the
	// parent to exist; mkdirat(2) has the same rule.
	auto host = resolveWritePath(resolved.value(), false);
	if (!host.ok())
	{
		return host.error();
	}
	if (::mkdir(host.value().c_str(), static_cast<mode_t>(mode == 0 ? 0777 : mode)) != 0)
	{
		return errnoToError(errno, "cannot create " + resolved.value());
	}
	return {};
}

Result<void> VirtualFileSystem::unlink(uint64_t dirfd,
	std::string_view path,
	bool remove_directory)
{
	auto resolved = resolvePath(dirfd, path);
	if (!resolved.ok())
	{
		return resolved.error();
	}
	auto host = resolveWritePath(resolved.value(), false);
	if (!host.ok())
	{
		return host.error();
	}
	std::error_code error;
	const bool directory = std::filesystem::is_directory(host.value(), error);
	if (!error && remove_directory && !directory)
	{
		return Error{ ErrorCode::NotDirectory, resolved.value() + " is not a directory" };
	}
	if (!error && !remove_directory && directory)
	{
		return Error{ ErrorCode::IsDirectory, resolved.value() + " is a directory" };
	}
	const int result = remove_directory ? ::rmdir(host.value().c_str()) :
					      ::unlink(host.value().c_str());
	if (result != 0)
	{
		return errnoToError(errno, "cannot remove " + resolved.value());
	}
	return {};
}

Result<void> VirtualFileSystem::rename(uint64_t old_dirfd,
	std::string_view old_path,
	uint64_t new_dirfd,
	std::string_view new_path)
{
	auto old_resolved = resolvePath(old_dirfd, old_path);
	if (!old_resolved.ok())
	{
		return old_resolved.error();
	}
	auto new_resolved = resolvePath(new_dirfd, new_path);
	if (!new_resolved.ok())
	{
		return new_resolved.error();
	}
	auto old_host = resolveWritePath(old_resolved.value(), false);
	if (!old_host.ok())
	{
		return old_host.error();
	}
	auto new_host = resolveWritePath(new_resolved.value(), false);
	if (!new_host.ok())
	{
		return new_host.error();
	}
	if (::rename(old_host.value().c_str(), new_host.value().c_str()) != 0)
	{
		return errnoToError(errno,
			"cannot rename " + old_resolved.value() + " to " + new_resolved.value());
	}
	return {};
}

Result<void> VirtualFileSystem::truncateFd(uint64_t fd, uint64_t size)
{
	std::shared_ptr<File> file = fds_.shared(fd);
	if (!file)
	{
		return Error{ ErrorCode::BadFileDescriptor, "fd is not open" };
	}
	return file->truncate(size);
}

Result<void> VirtualFileSystem::syncFd(uint64_t fd)
{
	// Host stdio is shared with the runner; flushing it is harmless but not
	// ours to report on.
	if (fd <= 2)
	{
		return {};
	}
	std::shared_ptr<File> file = fds_.shared(fd);
	if (!file)
	{
		return Error{ ErrorCode::BadFileDescriptor, "fd is not open" };
	}
	return file->sync();
}

Result<FileStat> VirtualFileSystem::stat(uint64_t dirfd, std::string_view path)
{
	auto resolved = resolvePath(dirfd, path);
	if (!resolved.ok())
	{
		return resolved.error();
	}
	const std::string &guest_path = resolved.value();

	if (isProcPath(guest_path))
	{
		auto generator = makeGenerator(guest_path, procContext());
		if (!generator.has_value())
		{
			return Error{ ErrorCode::NotFound, "no such proc file: " + guest_path };
		}
		MemoryFile file(guest_path, std::move(generator.value()));
		auto stat = file.stat();
		if (stat.ok())
		{
			applyOwner(stat.value());
		}
		return stat;
	}

	auto host = resolveHost(guest_path);
	if (!host.ok())
	{
		return host.error();
	}
	struct stat info = {};
	if (::stat(host.value().path.c_str(), &info) != 0)
	{
		return Error{ ErrorCode::NotFound, "cannot stat " + guest_path };
	}
	FileStat result = toFileStat(info);
	applyOwner(result);
	return result;
}

Result<FileStat> VirtualFileSystem::statFd(uint64_t fd)
{
	File *file = fds_.find(fd);
	if (file == nullptr)
	{
		return Error{ ErrorCode::BadFileDescriptor, "fd is not open" };
	}
	auto stat = file->stat();
	if (stat.ok())
	{
		applyOwner(stat.value());
	}
	return stat;
}

Result<std::shared_ptr<File> > VirtualFileSystem::describe(uint64_t fd)
{
	if (fd <= 2)
	{
		std::shared_ptr<StdioFile> &slot = stdio_[fd];
		if (!slot)
		{
			slot = std::make_shared<StdioFile>(static_cast<int>(fd));
		}
		return std::static_pointer_cast<File>(slot);
	}
	std::shared_ptr<File> file = fds_.shared(fd);
	if (!file)
	{
		return Error{ ErrorCode::BadFileDescriptor, "fd is not open" };
	}
	return file;
}

uint64_t VirtualFileSystem::addActivityWaiter(std::function<void()> callback)
{
	std::lock_guard<std::mutex> lock(waiters_mutex_);
	const uint64_t id = next_waiter_id_++;
	activity_waiters_.emplace(id, std::move(callback));
	return id;
}

void VirtualFileSystem::removeActivityWaiter(uint64_t id)
{
	std::lock_guard<std::mutex> lock(waiters_mutex_);
	activity_waiters_.erase(id);
}

void VirtualFileSystem::notifyActivity()
{
	// Callbacks may complete a parked syscall, which can in turn remove
	// themselves; copy first so the map is not mutated mid-iteration.
	std::vector<std::function<void()> > callbacks;
	{
		std::lock_guard<std::mutex> lock(waiters_mutex_);
		callbacks.reserve(activity_waiters_.size());
		for (const auto &entry : activity_waiters_)
		{
			callbacks.push_back(entry.second);
		}
	}
	for (const auto &callback : callbacks)
	{
		if (callback)
		{
			callback();
		}
	}
}

Result<uint64_t> VirtualFileSystem::duplicate(uint64_t fd, uint64_t minimum)
{
	auto file = describe(fd);
	if (!file.ok())
	{
		return file.error();
	}
	return fds_.insertShared(file.value(), minimum);
}

Result<void> VirtualFileSystem::duplicateTo(uint64_t fd, uint64_t target)
{
	if (fd == target)
	{
		// dup2 on an open descriptor is a successful no-op; validate the
		// source so a closed fd still reports EBADF.
		auto source = describe(fd);
		if (!source.ok())
		{
			return source.error();
		}
		return {};
	}
	if (target <= 2)
	{
		return Error{ ErrorCode::BadFileDescriptor, "stdin/stdout/stderr are fixed" };
	}
	auto file = describe(fd);
	if (!file.ok())
	{
		return file.error();
	}
	return fds_.assign(target, file.value());
}

Result<void> VirtualFileSystem::access(uint64_t dirfd, std::string_view path, uint64_t mode)
{
	auto resolved = resolvePath(dirfd, path);
	if (!resolved.ok())
	{
		return resolved.error();
	}
	const std::string &guest_path = resolved.value();

	if (isProcPath(guest_path))
	{
		if (!makeGenerator(guest_path, procContext()).has_value())
		{
			return Error{ ErrorCode::NotFound, "no such proc file: " + guest_path };
		}
	}
	else
	{
		auto host = resolveHost(guest_path);
		if (!host.ok())
		{
			return host.error();
		}
		if ((mode & kAccessRead) != 0 && ::access(host.value().path.c_str(), R_OK) != 0)
		{
			return Error{ ErrorCode::PermissionDenied, "cannot read " + guest_path };
		}
	}

	if ((mode & kAccessWrite) != 0)
	{
		// W_OK only holds inside the writable sandbox: the read-only roots may
		// be host-writable, but the guest must not see that.
		auto write_path = resolveWritePath(guest_path, false);
		if (!write_path.ok())
		{
			return Error{ ErrorCode::PermissionDenied, guest_path + " is read-only" };
		}
		if (::access(write_path.value().c_str(), W_OK) != 0)
		{
			return Error{ ErrorCode::PermissionDenied, "cannot write " + guest_path };
		}
	}
	if ((mode & kAccessExecute) != 0)
	{
		// Execution is decided by the mapping permissions, not by access(2).
		return Error{ ErrorCode::PermissionDenied, "execute access is not modelled" };
	}
	return {};
}

} // namespace duckemu::runtime::vfs
