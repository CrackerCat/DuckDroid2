// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace duckemu::common::uds
{

// One AF_UNIX descriptor, move-only. Every syscall wrapper is blocking and
// reports failure through |error|; no exceptions cross this boundary.
//
// Descriptors that a forked child must inherit (the worker/instance control
// pipes) are created with FD_CLOEXEC cleared; everything else stays close-on-
// exec so a respawned process starts with a clean fd table.
class Socket
{
    public:
	Socket() noexcept = default;
	explicit Socket(int fd) noexcept;
	~Socket();

	Socket(Socket &&other) noexcept;
	Socket &operator=(Socket &&other) noexcept;
	Socket(const Socket &) = delete;
	Socket &operator=(const Socket &) = delete;

	[[nodiscard]] bool valid() const noexcept
	{
		return fd_ >= 0;
	}
	[[nodiscard]] int fd() const noexcept
	{
		return fd_;
	}

	// Gives up ownership; the caller closes the descriptor.
	int release() noexcept;
	void close() noexcept;

	bool set_nonblocking(bool enabled, std::string *error);
	bool set_cloexec(bool enabled, std::string *error);

	// Binds and listens on an AF_UNIX path. A stale socket file at |path| is
	// removed first; the caller owns unlinking it on shutdown.
	bool listen_on(std::string_view path, uint32_t backlog, std::string *error);
	bool connect_to(std::string_view path, std::string *error);

	// Invalid socket on failure. Blocking accept.
	Socket accept(std::string *error) const;

	// Returns false on EOF or error. |error| is "peer closed" for a clean EOF
	// and the strerror text otherwise.
	bool read_exact(void *buffer, size_t size, std::string *error) const;
	bool write_all(const void *buffer, size_t size, std::string *error) const;

    private:
	int fd_ = -1;
};

// socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ...).
bool make_pair(Socket *first, Socket *second, std::string *error);

} // namespace duckemu::common::uds
