#include "runtime/vfs/fd_table.hpp"

#include <utility>

namespace duckemu::runtime::vfs
{

Result<uint64_t> FdTable::insert(std::unique_ptr<File> file)
{
	uint64_t fd = kFirstGuestFd;
	while (files_.count(fd) != 0)
	{
		++fd;
	}
	files_.emplace(fd, std::move(file));
	return fd;
}

File *FdTable::find(uint64_t fd) const
{
	auto it = files_.find(fd);
	return it == files_.end() ? nullptr : it->second.get();
}

Result<void> FdTable::remove(uint64_t fd)
{
	auto it = files_.find(fd);
	if (it == files_.end())
	{
		return Error{ ErrorCode::BadFileDescriptor, "fd is not open" };
	}
	files_.erase(it);
	return {};
}

} // namespace duckemu::runtime::vfs
