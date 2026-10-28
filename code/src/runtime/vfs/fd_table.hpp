#pragma once

#include <cstdint>
#include <map>
#include <memory>

#include "common/error/result.hpp"
#include "runtime/vfs/file.hpp"

namespace duckemu::runtime::vfs
{

// Process-wide descriptor table. 0/1/2 are the host stdio and are not stored
// here; the first guest descriptor is 3, matching Linux.
class FdTable
{
    public:
	static constexpr uint64_t kFirstGuestFd = 3;

	[[nodiscard]] Result<uint64_t> insert(std::unique_ptr<File> file);
	[[nodiscard]] File *find(uint64_t fd) const;
	Result<void> remove(uint64_t fd);

	[[nodiscard]] size_t size() const
	{
		return files_.size();
	}

    private:
	std::map<uint64_t, std::unique_ptr<File> > files_;
};

} // namespace duckemu::runtime::vfs
