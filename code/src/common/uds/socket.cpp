// SPDX-License-Identifier: GPL-2.0-only
#include "common/uds/socket.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace duckemu::common::uds
{
namespace
{

std::string SyscallError(const char *what)
{
	return std::string(what) + ": " + std::strerror(errno);
}

} // namespace

Socket::Socket(int fd) noexcept : fd_(fd) {}

Socket::~Socket()
{
	close();
}

Socket::Socket(Socket &&other) noexcept : fd_(other.fd_)
{
	other.fd_ = -1;
}

Socket &Socket::operator=(Socket &&other) noexcept
{
	if (this != &other)
	{
		close();
		fd_ = other.fd_;
		other.fd_ = -1;
	}
	return *this;
}

int Socket::release() noexcept
{
	const int fd = fd_;
	fd_ = -1;
	return fd;
}

void Socket::close() noexcept
{
	if (fd_ >= 0)
	{
		::close(fd_);
		fd_ = -1;
	}
}

bool Socket::set_nonblocking(bool enabled, std::string *error)
{
	const int flags = ::fcntl(fd_, F_GETFL, 0);
	if (flags < 0)
	{
		*error = SyscallError("fcntl(F_GETFL)");
		return false;
	}
	const int updated = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
	if (::fcntl(fd_, F_SETFL, updated) < 0)
	{
		*error = SyscallError("fcntl(F_SETFL)");
		return false;
	}
	return true;
}

bool Socket::set_cloexec(bool enabled, std::string *error)
{
	const int flags = ::fcntl(fd_, F_GETFD, 0);
	if (flags < 0)
	{
		*error = SyscallError("fcntl(F_GETFD)");
		return false;
	}
	const int updated = enabled ? (flags | FD_CLOEXEC) : (flags & ~FD_CLOEXEC);
	if (::fcntl(fd_, F_SETFD, updated) < 0)
	{
		*error = SyscallError("fcntl(F_SETFD)");
		return false;
	}
	return true;
}

bool Socket::listen_on(std::string_view path, uint32_t backlog, std::string *error)
{
	if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path))
	{
		*error = "AF_UNIX path is empty or too long";
		return false;
	}
	close();

	sockaddr_un address = {};
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path, path.data(), path.size());

	fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd_ < 0)
	{
		*error = SyscallError("socket");
		return false;
	}
	// A previous run may have left the socket file behind; binding would fail
	// with EADDRINUSE. Removing it is safe because the caller owns the path.
	::unlink(std::string(path).c_str());
	if (::bind(fd_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0)
	{
		*error = SyscallError("bind");
		close();
		return false;
	}
	if (::listen(fd_, static_cast<int>(backlog)) < 0)
	{
		*error = SyscallError("listen");
		close();
		return false;
	}
	return true;
}

bool Socket::connect_to(std::string_view path, std::string *error)
{
	if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path))
	{
		*error = "AF_UNIX path is empty or too long";
		return false;
	}
	close();

	sockaddr_un address = {};
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path, path.data(), path.size());

	fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd_ < 0)
	{
		*error = SyscallError("socket");
		return false;
	}
	if (::connect(fd_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0)
	{
		*error = SyscallError("connect");
		close();
		return false;
	}
	return true;
}

Socket Socket::accept(std::string *error) const
{
	while (true)
	{
		const int fd = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
		if (fd >= 0)
		{
			return Socket(fd);
		}
		if (errno == EINTR)
		{
			continue;
		}
		*error = SyscallError("accept4");
		return Socket();
	}
}

bool Socket::read_exact(void *buffer, size_t size, std::string *error) const
{
	auto *cursor = static_cast<uint8_t *>(buffer);
	size_t done = 0;
	while (done < size)
	{
		const ssize_t count = ::recv(fd_, cursor + done, size - done, 0);
		if (count == 0)
		{
			*error = done == 0 ? "peer closed the connection" : "peer closed mid-frame";
			return false;
		}
		if (count < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			*error = SyscallError("recv");
			return false;
		}
		done += static_cast<size_t>(count);
	}
	return true;
}

bool Socket::write_all(const void *buffer, size_t size, std::string *error) const
{
	const auto *cursor = static_cast<const uint8_t *>(buffer);
	size_t done = 0;
	while (done < size)
	{
		const ssize_t count = ::send(fd_, cursor + done, size - done, MSG_NOSIGNAL);
		if (count < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			*error = SyscallError("send");
			return false;
		}
		done += static_cast<size_t>(count);
	}
	return true;
}

bool make_pair(Socket *first, Socket *second, std::string *error)
{
	int fds[2] = { -1, -1 };
	if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) < 0)
	{
		*error = SyscallError("socketpair");
		return false;
	}
	*first = Socket(fds[0]);
	*second = Socket(fds[1]);
	return true;
}

} // namespace duckemu::common::uds
