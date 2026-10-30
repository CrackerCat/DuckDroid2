// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/vfs/virtual_file_system.hpp"

#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

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

Result<VirtualFileSystem::HostPath> VirtualFileSystem::resolveHost(const std::string &guest_path) const
{
	namespace fs = std::filesystem;
	for (const std::string &root : config_.host_roots)
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

Result<uint64_t> VirtualFileSystem::open(uint64_t dirfd, std::string_view path, uint64_t flags)
{
	if ((flags & kAccModeMask) != 0)
	{
		return Error{ ErrorCode::Unsupported, "host roots are mounted read-only" };
	}
	if ((flags & (kOCreate | kOTruncate | kOAppend)) != 0)
	{
		return Error{ ErrorCode::Unsupported, "host roots are mounted read-only" };
	}

	auto resolved = resolvePath(dirfd, path);
	if (!resolved.ok())
	{
		return resolved.error();
	}
	auto file = openPath(resolved.value(), flags);
	if (!file.ok())
	{
		return file.error();
	}
	return fds_.insert(std::move(file.value()));
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
		return file.stat();
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
	return toFileStat(info);
}

Result<FileStat> VirtualFileSystem::statFd(uint64_t fd)
{
	File *file = fds_.find(fd);
	if (file == nullptr)
	{
		return Error{ ErrorCode::BadFileDescriptor, "fd is not open" };
	}
	return file->stat();
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

	if ((mode & (kAccessWrite | kAccessExecute)) != 0)
	{
		return Error{ ErrorCode::PermissionDenied, "host roots are mounted read-only" };
	}
	return {};
}

} // namespace duckemu::runtime::vfs
