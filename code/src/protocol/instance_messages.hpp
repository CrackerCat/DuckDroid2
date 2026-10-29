#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "protocol/messages.hpp"

namespace duckemu::protocol::instance
{

// Worker <-> instance control channel. Same framing as every other hop; the
// JSON body is discriminated by "type". The instance process is the only
// component that owns a RuntimeInstance, so this is the only message set that
// can carry guest results.
enum class Type
{
	kReady,
	kFailed,
	kInvoke,
	kResult,
	kShutdown,
};

struct Message
{
	Type type = Type::kReady;

	// ready
	std::string instance;
	uint64_t init_us = 0;

	// failed / result
	ErrorInfo error;

	// invoke / result
	std::string id;
	bool ok = false;
	std::string op;
	std::map<std::string, std::string> params;
	std::map<std::string, std::string> result;
	bool has_deadline = false;
	uint64_t deadline_ms = 0;

	// result
	uint64_t invoke_us = 0;
	uint64_t requests = 0;
};

bool serialize(const Message &message, std::string *frame, std::string *error);
bool parse(std::string_view frame, Message *message, std::string *error);

} // namespace duckemu::protocol::instance
