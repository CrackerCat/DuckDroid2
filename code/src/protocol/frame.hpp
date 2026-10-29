// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "common/uds/socket.hpp"

namespace duckemu::protocol
{

// Wire framing shared by every hop: a 4-byte big-endian payload length
// followed by exactly that many bytes of UTF-8 JSON. One connection may carry
// any number of frames in either direction.
inline constexpr uint32_t kMaxFrameBytes = 16u * 1024u * 1024u;

bool write_frame(const common::uds::Socket &socket, std::string_view payload, std::string *error);

// Blocking: waits for one whole frame. Used by the worker/instance loops where
// one thread is dedicated to the hop.
bool read_frame(const common::uds::Socket &socket, std::string *payload, std::string *error);

// Incremental parser for reactors that read arbitrary chunks. An invalid
// length is sticky: the stream cannot be resynchronized and the connection
// must be closed by the caller.
class FrameReader
{
    public:
	enum class Status
	{
		kNeedMore,
		kFrame,
		kError,
	};

	explicit FrameReader(uint32_t max_frame_bytes = kMaxFrameBytes);

	void reset();
	// Appends |size| bytes (|data| may be null when |size| is 0, which only
	// drains frames already buffered), then extracts at most one frame.
	Status consume(const uint8_t *data, size_t size, std::string *payload, std::string *error);
	[[nodiscard]] size_t buffered_bytes() const
	{
		return buffer_.size();
	}

    private:
	uint32_t max_frame_bytes_;
	std::string buffer_;
	bool failed_ = false;
};

} // namespace duckemu::protocol
