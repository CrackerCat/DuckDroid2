#include "runtime/vfs/runtime_vfs.hpp"

#include <filesystem>
#include <fstream>
#include <utility>

namespace duckemu::runtime::vfs
{
namespace
{

Result<std::vector<uint8_t> > readWholeFile(const std::string &path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
	{
		return Error{ ErrorCode::NotFound, "cannot open " + path };
	}

	input.seekg(0, std::ios::end);
	const std::streamoff length = input.tellg();
	if (length < 0)
	{
		return Error{ ErrorCode::Internal, "cannot size " + path };
	}
	input.seekg(0, std::ios::beg);

	std::vector<uint8_t> bytes(static_cast<size_t>(length));
	if (length > 0)
	{
		input.read(reinterpret_cast<char *>(bytes.data()), length);
		if (!input)
		{
			return Error{ ErrorCode::Internal, "cannot read " + path };
		}
	}
	return bytes;
}

} // namespace

RuntimeVfs::RuntimeVfs(std::vector<std::string> search_paths)
	: search_paths_(std::move(search_paths))
{
}

void RuntimeVfs::addSearchPath(std::string path)
{
	search_paths_.push_back(std::move(path));
}

Result<std::vector<uint8_t> > RuntimeVfs::readFile(std::string_view path) const
{
	return readWholeFile(std::string(path));
}

Result<std::string> RuntimeVfs::resolveModule(std::string_view name,
	std::string_view requesting_directory) const
{
	std::vector<std::string> candidates;
	if (!requesting_directory.empty())
	{
		candidates.push_back((std::filesystem::path(std::string(requesting_directory)) / std::string(name)).string());
	}
	for (const std::string &directory : search_paths_)
	{
		candidates.push_back((std::filesystem::path(directory) / std::string(name)).string());
	}

	for (const std::string &candidate : candidates)
	{
		if (std::filesystem::is_regular_file(candidate))
		{
			return candidate;
		}
	}
	return Error{ ErrorCode::NotFound, "cannot find " + std::string(name) };
}

} // namespace duckemu::runtime::vfs
