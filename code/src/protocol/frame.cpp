#include "protocol/frame.hpp"

#include <cstring>

namespace duckemu::protocol
{
namespace
{

void StoreBigEndian(uint8_t *output, uint32_t value)
{
	output[0] = static_cast<uint8_t>((value >> 24) & 0xFFu);
	output[1] = static_cast<uint8_t>((value >> 16) & 0xFFu);
	output[2] = static_cast<uint8_t>((value >> 8) & 0xFFu);
	output[3] = static_cast<uint8_t>(value & 0xFFu);
}

uint32_t LoadBigEndian(const uint8_t *input)
{
	return (static_cast<uint32_t>(input[0]) << 24) | (static_cast<uint32_t>(input[1]) << 16) |
		(static_cast<uint32_t>(input[2]) << 8) | static_cast<uint32_t>(input[3]);
}

} // namespace

bool write_frame(const common::uds::Socket &socket, std::string_view payload, std::string *error)
{
	if (payload.empty())
	{
		*error = "frame payload is empty";
		return false;
	}
	if (payload.size() > kMaxFrameBytes)
	{
		*error = "frame payload exceeds the maximum size";
		return false;
	}
	uint8_t header[4] = {};
	StoreBigEndian(header, static_cast<uint32_t>(payload.size()));
	if (!socket.write_all(header, sizeof(header), error))
	{
		return false;
	}
	return socket.write_all(payload.data(), payload.size(), error);
}

bool read_frame(const common::uds::Socket &socket, std::string *payload, std::string *error)
{
	uint8_t header[4] = {};
	if (!socket.read_exact(header, sizeof(header), error))
	{
		return false;
	}
	const uint32_t length = LoadBigEndian(header);
	if (length == 0 || length > kMaxFrameBytes)
	{
		*error = "invalid frame length";
		return false;
	}
	payload->resize(length);
	return socket.read_exact(payload->data(), length, error);
}

FrameReader::FrameReader(uint32_t max_frame_bytes) : max_frame_bytes_(max_frame_bytes) {}

void FrameReader::reset()
{
	buffer_.clear();
	failed_ = false;
}

FrameReader::Status FrameReader::consume(const uint8_t *data, size_t size, std::string *payload, std::string *error)
{
	if (failed_)
	{
		*error = "frame stream is desynchronized";
		return Status::kError;
	}
	if (size != 0)
	{
		buffer_.append(reinterpret_cast<const char *>(data), size);
	}
	if (buffer_.size() < 4)
	{
		return Status::kNeedMore;
	}
	const uint32_t length = LoadBigEndian(reinterpret_cast<const uint8_t *>(buffer_.data()));
	if (length == 0 || length > max_frame_bytes_)
	{
		failed_ = true;
		*error = "invalid frame length";
		return Status::kError;
	}
	if (buffer_.size() < 4 + static_cast<size_t>(length))
	{
		return Status::kNeedMore;
	}
	payload->assign(buffer_.data() + 4, length);
	buffer_.erase(0, 4 + static_cast<size_t>(length));
	return Status::kFrame;
}

} // namespace duckemu::protocol
