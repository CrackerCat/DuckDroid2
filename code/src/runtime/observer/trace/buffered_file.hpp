// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

#include "common/error/result.hpp"

namespace duckemu::runtime::observer
{

// Append-only buffered writer. The trace path is hot: formatting writes into
// one fixed buffer and only calls write(2) when it fills or the session ends.
// Errors are latched (ok() turns false) instead of thrown out of a Unicorn
// callback.
class BufferedFile
{
    public:
	static Result<std::unique_ptr<BufferedFile> > open(std::string_view path);
	~BufferedFile();

	BufferedFile(const BufferedFile &) = delete;
	BufferedFile &operator=(const BufferedFile &) = delete;
	BufferedFile(BufferedFile &&) = delete;
	BufferedFile &operator=(BufferedFile &&) = delete;

	void write(std::string_view text);
	void write(const char *data, size_t size);
	void flush();
	void close();

	[[nodiscard]] bool ok() const noexcept
	{
		return !failed_ && fd_ >= 0;
	}

    private:
	explicit BufferedFile(int fd);

	static constexpr size_t kCapacity = 1u << 20;

	int fd_ = -1;
	std::unique_ptr<char[]> buffer_;
	size_t used_ = 0;
	bool failed_ = false;
};

} // namespace duckemu::runtime::observer
