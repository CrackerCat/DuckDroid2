#include "protocol/channel.hpp"

#include "protocol/frame.hpp"

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

bool write_channel(const common::uds::Socket &socket, uint32_t conn_id, std::string_view payload,
	std::string *error)
{
	if (payload.empty())
	{
		*error = "channel payload is empty";
		return false;
	}
	if (payload.size() > kMaxFrameBytes)
	{
		*error = "channel payload exceeds the maximum size";
		return false;
	}
	uint8_t header[8] = {};
	StoreBigEndian(header, conn_id);
	StoreBigEndian(header + 4, static_cast<uint32_t>(payload.size()));
	if (!socket.write_all(header, sizeof(header), error))
	{
		return false;
	}
	return socket.write_all(payload.data(), payload.size(), error);
}

bool read_channel(const common::uds::Socket &socket, uint32_t *conn_id, std::string *payload,
	std::string *error)
{
	uint8_t header[8] = {};
	if (!socket.read_exact(header, sizeof(header), error))
	{
		return false;
	}
	const uint32_t length = LoadBigEndian(header + 4);
	if (length == 0 || length > kMaxFrameBytes)
	{
		*error = "invalid channel frame length";
		return false;
	}
	*conn_id = LoadBigEndian(header);
	payload->resize(length);
	return socket.read_exact(payload->data(), length, error);
}

} // namespace duckemu::protocol
