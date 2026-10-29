#include "protocol/messages.hpp"

#include <exception>
#include <nlohmann/json.hpp>

namespace duckemu::protocol
{
namespace
{

using Json = nlohmann::json;

// Protocol parameters arrive as JSON scalars; stringifying them here keeps the
// guest-facing contract uniform (every value the plugin sees is a string).
bool JsonScalarToString(const Json &value, std::string *out)
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
	if (value.is_number_float())
	{
		*out = value.dump();
		return true;
	}
	return false;
}

bool JsonToU64(const Json &value, uint64_t *out)
{
	if (value.is_number_unsigned())
	{
		*out = value.get<uint64_t>();
		return true;
	}
	if (value.is_number_integer())
	{
		const int64_t signed_value = value.get<int64_t>();
		if (signed_value < 0)
		{
			return false;
		}
		*out = static_cast<uint64_t>(signed_value);
		return true;
	}
	return false;
}

bool ReadString(const Json &object, const char *key, std::string *out, size_t max_bytes, bool required,
	std::string *message)
{
	const auto found = object.find(key);
	if (found == object.end() || found->is_null())
	{
		if (required)
		{
			*message = std::string("missing '") + key + "'";
			return false;
		}
		return true;
	}
	if (!found->is_string())
	{
		*message = std::string("'") + key + "' must be a string";
		return false;
	}
	*out = found->get<std::string>();
	if (out->empty() && required)
	{
		*message = std::string("'") + key + "' must not be empty";
		return false;
	}
	if (out->size() > max_bytes)
	{
		*message = std::string("'") + key + "' is too long";
		return false;
	}
	return true;
}

bool ReadStringMap(const Json &object, const char *key, std::map<std::string, std::string> *params,
	std::string *message)
{
	const auto found = object.find(key);
	if (found == object.end() || found->is_null())
	{
		return true;
	}
	if (!found->is_object())
	{
		*message = std::string("'") + key + "' must be an object";
		return false;
	}
	if (found->size() > kMaxParamCount)
	{
		*message = "too many params";
		return false;
	}
	for (auto it = found->begin(); it != found->end(); ++it)
	{
		if (it.key().size() > kMaxParamKeyBytes)
		{
			*message = "param key is too long";
			return false;
		}
		std::string value;
		if (!JsonScalarToString(it.value(), &value))
		{
			*message = "param '" + it.key() + "' must be a scalar";
			return false;
		}
		if (value.size() > kMaxParamValueBytes)
		{
			*message = "param '" + it.key() + "' is too large";
			return false;
		}
		(*params)[it.key()] = std::move(value);
	}
	return true;
}

bool ReadVersion(const Json &object, uint32_t *version, std::string *message)
{
	const auto found = object.find("version");
	if (found == object.end() || !found->is_number_integer())
	{
		*message = "missing or invalid 'version'";
		return false;
	}
	uint64_t value = 0;
	if (!JsonToU64(*found, &value) || value != kVersion)
	{
		*message = "unsupported protocol version";
		return false;
	}
	*version = static_cast<uint32_t>(value);
	return true;
}

void WriteStringMap(Json *object, const char *key, const std::map<std::string, std::string> &params)
{
	Json values = Json::object();
	for (const auto &entry : params)
	{
		values[entry.first] = entry.second;
	}
	(*object)[key] = std::move(values);
}

} // namespace

ParseStatus parse_request(std::string_view frame, Request *request, std::string *message, std::string *request_id)
{
	Json root;
	try
	{
		root = Json::parse(frame.begin(), frame.end());
	}
	catch (const std::exception &error)
	{
		*message = std::string("invalid JSON: ") + error.what();
		return ParseStatus::kBadJson;
	}
	if (!root.is_object())
	{
		*message = "request must be a JSON object";
		return ParseStatus::kBadJson;
	}
	// Best effort: a correlated error response is more useful than a naked one,
	// even when the rest of the message is rejected.
	const auto id = root.find("id");
	if (id != root.end() && id->is_string())
	{
		*request_id = id->get<std::string>();
	}

	if (!ReadVersion(root, &request->version, message))
	{
		return ParseStatus::kBadVersion;
	}
	if (!ReadString(root, "id", &request->id, kMaxRequestIdBytes, true, message))
	{
		return ParseStatus::kBadRequest;
	}
	if (!ReadString(root, "app", &request->app, kMaxAppIdBytes, false, message))
	{
		return ParseStatus::kBadRequest;
	}
	if (!ReadString(root, "op", &request->op, kMaxOpBytes, false, message))
	{
		return ParseStatus::kBadRequest;
	}
	if (request->op.empty())
	{
		request->op = "sign";
	}

	const auto session = root.find("session");
	if (session != root.end() && !session->is_null())
	{
		if (!session->is_object())
		{
			*message = "'session' must be an object";
			return ParseStatus::kBadRequest;
		}
		if (!ReadString(*session, "id", &request->session.id, kMaxSessionBytes, true, message))
		{
			return ParseStatus::kBadRequest;
		}
		request->session.present = true;
		const auto seed = session->find("seed");
		if (seed != session->end() && !seed->is_null())
		{
			if (!JsonToU64(*seed, &request->session.seed))
			{
				*message = "'session.seed' must be an unsigned integer";
				return ParseStatus::kBadRequest;
			}
		}
	}

	if (!ReadStringMap(root, "params", &request->params, message))
	{
		return ParseStatus::kBadRequest;
	}

	const auto deadline = root.find("deadline_ms");
	if (deadline != root.end() && !deadline->is_null())
	{
		uint64_t value = 0;
		if (!JsonToU64(*deadline, &value) || value > kMaxDeadlineMs)
		{
			*message = "'deadline_ms' is out of range";
			return ParseStatus::kBadRequest;
		}
		request->has_deadline = true;
		request->deadline_ms = value;
	}
	return ParseStatus::kOk;
}

bool serialize_request(const Request &request, std::string *frame, std::string *error)
{
	if (request.id.empty() || request.id.size() > kMaxRequestIdBytes)
	{
		*error = "request id is empty or too long";
		return false;
	}
	Json root = Json::object();
	root["version"] = request.version;
	root["id"] = request.id;
	if (!request.app.empty())
	{
		root["app"] = request.app;
	}
	root["op"] = request.op;
	if (request.session.present)
	{
		Json session = Json::object();
		session["id"] = request.session.id;
		if (request.session.seed != 0)
		{
			session["seed"] = request.session.seed;
		}
		root["session"] = std::move(session);
	}
	WriteStringMap(&root, "params", request.params);
	if (request.has_deadline)
	{
		root["deadline_ms"] = request.deadline_ms;
	}
	*frame = root.dump();
	return true;
}

ParseStatus parse_response(std::string_view frame, Response *response, std::string *message)
{
	Json root;
	try
	{
		root = Json::parse(frame.begin(), frame.end());
	}
	catch (const std::exception &error)
	{
		*message = std::string("invalid JSON: ") + error.what();
		return ParseStatus::kBadJson;
	}
	if (!root.is_object())
	{
		*message = "response must be a JSON object";
		return ParseStatus::kBadJson;
	}
	if (!ReadVersion(root, &response->version, message))
	{
		return ParseStatus::kBadVersion;
	}
	if (!ReadString(root, "id", &response->id, kMaxRequestIdBytes, true, message))
	{
		return ParseStatus::kBadRequest;
	}
	if (!ReadString(root, "app", &response->app, kMaxAppIdBytes, false, message) ||
		!ReadString(root, "worker", &response->worker, kMaxAppIdBytes, false, message) ||
		!ReadString(root, "instance", &response->instance, kMaxAppIdBytes, false, message))
	{
		return ParseStatus::kBadRequest;
	}

	const auto ok = root.find("ok");
	if (ok != root.end() && !ok->is_null())
	{
		if (!ok->is_boolean())
		{
			*message = "'ok' must be a boolean";
			return ParseStatus::kBadRequest;
		}
		response->ok = ok->get<bool>();
	}

	if (!ReadStringMap(root, "result", &response->result, message))
	{
		return ParseStatus::kBadRequest;
	}

	const auto error = root.find("error");
	if (error != root.end() && !error->is_null())
	{
		if (!error->is_object())
		{
			*message = "'error' must be an object";
			return ParseStatus::kBadRequest;
		}
		if (!ReadString(*error, "code", &response->error.code, kMaxOpBytes, false, message) ||
			!ReadString(*error, "message", &response->error.message, kMaxParamValueBytes, false, message) ||
			!ReadString(*error, "stage", &response->error.stage, kMaxOpBytes, false, message))
		{
			return ParseStatus::kBadRequest;
		}
		const auto retryable = error->find("retryable");
		if (retryable != error->end() && retryable->is_boolean())
		{
			response->error.retryable = retryable->get<bool>();
		}
		const auto new_session = error->find("new_session");
		if (new_session != error->end() && new_session->is_boolean())
		{
			response->error.new_session = new_session->get<bool>();
		}
	}

	const auto meta = root.find("meta");
	if (meta != root.end() && !meta->is_null())
	{
		if (!meta->is_object())
		{
			*message = "'meta' must be an object";
			return ParseStatus::kBadRequest;
		}
		uint64_t value = 0;
		const auto queue_us = meta->find("queue_us");
		if (queue_us != meta->end() && JsonToU64(*queue_us, &value))
		{
			response->meta.queue_us = value;
		}
		const auto init_us = meta->find("init_us");
		if (init_us != meta->end() && JsonToU64(*init_us, &value))
		{
			response->meta.init_us = value;
		}
		const auto invoke_us = meta->find("invoke_us");
		if (invoke_us != meta->end() && JsonToU64(*invoke_us, &value))
		{
			response->meta.invoke_us = value;
		}
		const auto served = meta->find("instance_requests");
		if (served != meta->end() && JsonToU64(*served, &value))
		{
			response->meta.instance_requests = value;
		}
		const auto recycled = meta->find("recycled");
		if (recycled != meta->end() && recycled->is_boolean())
		{
			response->meta.recycled = recycled->get<bool>();
		}
	}
	return ParseStatus::kOk;
}

bool serialize_response(const Response &response, std::string *frame, std::string *error)
{
	if (response.version != kVersion)
	{
		*error = "response version is not supported";
		return false;
	}
	Json root = Json::object();
	root["version"] = response.version;
	root["id"] = response.id;
	if (!response.app.empty())
	{
		root["app"] = response.app;
	}
	if (!response.worker.empty())
	{
		root["worker"] = response.worker;
	}
	if (!response.instance.empty())
	{
		root["instance"] = response.instance;
	}
	root["ok"] = response.ok;
	WriteStringMap(&root, "result", response.result);
	if (!response.error.code.empty())
	{
		Json error_json = Json::object();
		error_json["code"] = response.error.code;
		error_json["message"] = response.error.message;
		error_json["stage"] = response.error.stage;
		error_json["retryable"] = response.error.retryable;
		error_json["new_session"] = response.error.new_session;
		root["error"] = std::move(error_json);
	}
	Json meta = Json::object();
	meta["queue_us"] = response.meta.queue_us;
	meta["init_us"] = response.meta.init_us;
	meta["invoke_us"] = response.meta.invoke_us;
	meta["instance_requests"] = response.meta.instance_requests;
	meta["recycled"] = response.meta.recycled;
	root["meta"] = std::move(meta);
	*frame = root.dump();
	return true;
}

Response make_error_response(std::string_view id,
	std::string_view app,
	std::string_view worker,
	std::string_view instance,
	std::string_view code,
	std::string_view message,
	std::string_view stage,
	bool retryable,
	bool new_session)
{
	Response response;
	response.id = std::string(id);
	response.app = std::string(app);
	response.worker = std::string(worker);
	response.instance = std::string(instance);
	response.ok = false;
	response.error.code = std::string(code);
	response.error.message = std::string(message);
	response.error.stage = std::string(stage);
	response.error.retryable = retryable;
	response.error.new_session = new_session;
	return response;
}

} // namespace duckemu::protocol
