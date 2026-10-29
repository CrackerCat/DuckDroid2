#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "common/uds/socket.hpp"

namespace duckemu::protocol
{

// System <-> Worker envelope. System never parses the payload; the only thing
// it adds is the connection id used to route the response back to the client.
//
// Layout: [4B conn_id BE][4B payload length BE][payload bytes]
//
// The payload is one complete request/response frame body (JSON), so the
// worker reuses parse_request/serialize_response unchanged.
bool write_channel(const common::uds::Socket &socket, uint32_t conn_id, std::string_view payload,
	std::string *error);
bool read_channel(const common::uds::Socket &socket, uint32_t *conn_id, std::string *payload,
	std::string *error);

} // namespace duckemu::protocol
