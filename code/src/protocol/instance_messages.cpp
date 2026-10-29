// SPDX-License-Identifier: GPL-2.0-only
#include "protocol/instance_messages.hpp"

#include <exception>
#include <nlohmann/json.hpp>

namespace duckemu::protocol::instance
{
namespace
{

using Json = nlohmann::json;

constexpr const char *kTypeReady = "ready";
constexpr const char *kTypeFailed = "failed";
constexpr const char *kTypeInvoke = "invoke";
constexpr const char *kTypeResult = "result";
constexpr const char *kTypeShutdown = "shutdown";

const char *TypeName(Type type)
{
	switch (type)
	{
	case Type::kReady:
		return kTypeReady;
	case Type::kFailed:
		return kTypeFailed;
	case Type::kInvoke:
		return kTypeInvoke;
	case Type::kResult:
		return kTypeResult;
	case Type::kShutdown:
		return kTypeShutdown;
	}
	return "";
}

bool ParseType(std::string_view name, Type *type)
{
	if (name == kTypeReady)
	{
		*type = Type::kReady;
		return true;
	}
	if (name == kTypeFailed)
	{
		*type = Type::kFailed;
		return true;
	}
	if (name == kTypeInvoke)
	{
		*type = Type::kInvoke;
		return true;
	}
	if (name == kTypeResult)
	{
		*type = Type::kResult;
		return true;
	}
	if (name == kTypeShutdown)
	{
		*type = Type::kShutdown;
		return true;
	}
	return false;
}

bool ScalarToString(const Json &value, std::string *out)
{
	if (value.is_string())
	{
		*out = value.get<std::string>();
		return true;
	}
	if (value.is_boolean())
	{
		*out = value.get<bool>() ? "true" : "false";
		return true;
	}
	if (value.is_number_unsigned())
	{
		*out = std::to_string(value.get<uint64_t>());
		return true;
	}
	if (value.is_number_integer())
	{
		*out = std::to_string(value.get<int64_t>());
		return true;
	}
	return false;
}

bool ReadString(const Json &object, const char *key, std::string *out, size_t max_bytes, bool required,
	std::string *error)
{
	const auto found = object.find(key);
	if (found == object.end() || found->is_null())
	{
		if (required)
		{
			*error = std::string("missing '") + key + "'";
			return false;
		}
		return true;
	}
	if (!found->is_string())
	{
		*error = std::string("'") + key + "' must be a string";
		return false;
	}
	*out = found->get<std::string>();
	if (out->size() > max_bytes)
	{
		*error = std::string("'") + key + "' is too long";
		return false;
	}
	return true;
}

bool ReadStringMap(const Json &object, const char *key, std::map<std::string, std::string> *out,
	std::string *error)
{
	const auto found = object.find(key);
	if (found == object.end() || found->is_null())
	{
		return true;
	}
	if (!found->is_object())
	{
		*error = std::string("'") + key + "' must be an object";
		return false;
	}
	if (found->size() > kMaxParamCount)
	{
		*error = "too many entries in '" + std::string(key) + "'";
		return false;
	}
	for (auto it = found->begin(); it != found->end(); ++it)
	{
		std::string value;
		if (!ScalarToString(it.value(), &value))
		{
			*error = "entry '" + it.key() + "' must be a scalar";
			return false;
		}
		if (value.size() > kMaxParamValueBytes)
		{
			*error = "entry '" + it.key() + "' is too large";
			return false;
		}
		(*out)[it.key()] = std::move(value);
	}
	return true;
}

void WriteStringMap(Json *object, const char *key, const std::map<std::string, std::string> &values)
{
	Json json_values = Json::object();
	for (const auto &entry : values)
	{
		json_values[entry.first] = entry.second;
	}
	(*object)[key] = std::move(json_values);
}

bool ReadU64(const Json &object, const char *key, uint64_t *out, std::string *error)
{
	const auto found = object.find(key);
	if (found == object.end() || found->is_null())
	{
		return true;
	}
	if (found->is_number_unsigned())
	{
		*out = found->get<uint64_t>();
		return true;
	}
	if (found->is_number_integer())
	{
		const int64_t value = found->get<int64_t>();
		if (value < 0)
		{
			*error = std::string("'") + key + "' must not be negative";
			return false;
		}
		*out = static_cast<uint64_t>(value);
		return true;
	}
	*error = std::string("'") + key + "' must be an unsigned integer";
	return false;
}

} // namespace

bool serialize(const Message &message, std::string *frame, std::string *error)
{
	if ((message.type == Type::kInvoke || message.type == Type::kResult) && message.id.empty())
	{
		*error = "invoke/result messages require an id";
		return false;
	}
	if (message.type == Type::kReady && message.instance.empty())
	{
		*error = "ready requires an instance id";
		return false;
	}
	Json root = Json::object();
	root["type"] = TypeName(message.type);
	switch (message.type)
	{
	case Type::kReady:
		root["instance"] = message.instance;
		root["init_us"] = message.init_us;
		break;
	case Type::kFailed:
		root["code"] = message.error.code;
		root["message"] = message.error.message;
		break;
	case Type::kInvoke:
		root["id"] = message.id;
		if (!message.op.empty())
		{
			root["op"] = message.op;
		}
		WriteStringMap(&root, "params", message.params);
		if (message.has_deadline)
		{
			root["deadline_ms"] = message.deadline_ms;
		}
		break;
	case Type::kResult:
		root["id"] = message.id;
		root["ok"] = message.ok;
		if (message.ok)
		{
			WriteStringMap(&root, "result", message.result);
		}
		else
		{
			Json error_json = Json::object();
			error_json["code"] = message.error.code;
			error_json["message"] = message.error.message;
			root["error"] = std::move(error_json);
		}
		root["invoke_us"] = message.invoke_us;
		root["requests"] = message.requests;
		break;
	case Type::kShutdown:
		break;
	}
	*frame = root.dump();
	return true;
}

bool parse(std::string_view frame, Message *message, std::string *error)
{
	Json root;
	try
	{
		root = Json::parse(frame.begin(), frame.end());
	}
	catch (const std::exception &exception)
	{
		*error = std::string("invalid JSON: ") + exception.what();
		return false;
	}
	if (!root.is_object())
	{
		*error = "message must be a JSON object";
		return false;
	}
	std::string type_name;
	if (!ReadString(root, "type", &type_name, 32, true, error))
	{
		return false;
	}
	if (!ParseType(type_name, &message->type))
	{
		*error = "unknown message type '" + type_name + "'";
		return false;
	}
	switch (message->type)
	{
	case Type::kReady:
		return ReadString(root, "instance", &message->instance, kMaxAppIdBytes, true, error) &&
			ReadU64(root, "init_us", &message->init_us, error);
	case Type::kFailed:
		return ReadString(root, "code", &message->error.code, kMaxOpBytes, true, error) &&
			ReadString(root, "message", &message->error.message, kMaxParamValueBytes, false, error);
	case Type::kInvoke:
		if (!ReadString(root, "id", &message->id, kMaxRequestIdBytes, true, error) ||
			!ReadString(root, "op", &message->op, kMaxOpBytes, false, error) ||
			!ReadStringMap(root, "params", &message->params, error))
		{
			return false;
		}
		if (message->op.empty())
		{
			message->op = "sign";
		}
		{
			uint64_t deadline = 0;
			if (!ReadU64(root, "deadline_ms", &deadline, error))
			{
				return false;
			}
			if (deadline != 0)
			{
				message->has_deadline = true;
				message->deadline_ms = deadline;
			}
		}
		return true;
	case Type::kResult:
		if (!ReadString(root, "id", &message->id, kMaxRequestIdBytes, true, error))
		{
			return false;
		}
		{
			const auto ok = root.find("ok");
			if (ok != root.end() && ok->is_boolean())
			{
				message->ok = ok->get<bool>();
			}
		}
		if (message->ok)
		{
			if (!ReadStringMap(root, "result", &message->result, error))
			{
				return false;
			}
		}
		else
		{
			const auto error_object = root.find("error");
			if (error_object != root.end() && error_object->is_object())
			{
				if (!ReadString(*error_object, "code", &message->error.code, kMaxOpBytes, false, error) ||
					!ReadString(*error_object, "message", &message->error.message, kMaxParamValueBytes,
						false, error))
				{
					return false;
				}
			}
			if (message->error.code.empty())
			{
				message->error.code = error_code::kInternal;
			}
		}
		return ReadU64(root, "invoke_us", &message->invoke_us, error) &&
			ReadU64(root, "requests", &message->requests, error);
	case Type::kShutdown:
		return true;
	}
	*error = "unhandled message type";
	return false;
}

} // namespace duckemu::protocol::instance
