#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

// JSON messages carried by protocol frames. This layer owns validation and
// serialization only: it knows nothing about workers, instances or scheduling.
//
// Shape (request):
//   {"version":1,"id":"...","app":"...","op":"sign",
//    "session":{"id":"...","seed":0},
//    "params":{"k":"v"},"deadline_ms":5000}
//
// Shape (response):
//   {"version":1,"id":"...","app":"...","worker":"...","instance":"...",
//    "ok":true,"result":{"k":"v"},
//    "error":{"code":"...","message":"...","stage":"...","retryable":false,
//             "new_session":false},
//    "meta":{"queue_us":0,"init_us":0,"invoke_us":0,"instance_requests":0,
//            "recycled":false}}

namespace duckemu::protocol
{

inline constexpr uint32_t kVersion = 1;

inline constexpr size_t kMaxRequestIdBytes = 256;
inline constexpr size_t kMaxAppIdBytes = 256;
inline constexpr size_t kMaxSessionBytes = 128;
inline constexpr size_t kMaxOpBytes = 64;
inline constexpr size_t kMaxParamCount = 256;
inline constexpr size_t kMaxParamKeyBytes = 256;
inline constexpr size_t kMaxParamValueBytes = 8u * 1024u * 1024u;
inline constexpr uint64_t kMaxDeadlineMs = 24ull * 60ull * 60ull * 1000ull;

namespace error_code
{
inline constexpr const char *kBadFrame = "BAD_FRAME";
inline constexpr const char *kBadJson = "BAD_JSON";
inline constexpr const char *kBadVersion = "BAD_VERSION";
inline constexpr const char *kBadRequest = "BAD_REQUEST";
inline constexpr const char *kUnknownOp = "UNKNOWN_OP";
inline constexpr const char *kAppMismatch = "APP_MISMATCH";
inline constexpr const char *kOverloaded = "OVERLOADED";
inline constexpr const char *kSessionNotFound = "SESSION_NOT_FOUND";
inline constexpr const char *kSessionGone = "SESSION_GONE";
inline constexpr const char *kInitializing = "INITIALIZING";
inline constexpr const char *kQueueTimeout = "QUEUE_TIMEOUT";
inline constexpr const char *kDeadlineExceeded = "DEADLINE_EXCEEDED";
inline constexpr const char *kInitFailed = "INIT_FAILED";
inline constexpr const char *kAppError = "APP_ERROR";
inline constexpr const char *kGuestFault = "GUEST_FAULT";
inline constexpr const char *kCallTimeout = "CALL_TIMEOUT";
inline constexpr const char *kStopRequested = "STOP_REQUESTED";
inline constexpr const char *kInternal = "INTERNAL";
} // namespace error_code

namespace stage
{
inline constexpr const char *kRead = "read";
inline constexpr const char *kParse = "parse";
inline constexpr const char *kQueue = "queue";
inline constexpr const char *kInit = "init";
inline constexpr const char *kInvoke = "invoke";
inline constexpr const char *kWrite = "write";
inline constexpr const char *kControl = "control";
} // namespace stage

struct SessionRef
{
	bool present = false;
	std::string id;
	// 0 asks the worker to derive one; a non-zero value makes the instance's
	// random stream reproducible.
	uint64_t seed = 0;
};

struct Request
{
	uint32_t version = kVersion;
	std::string id;
	std::string app;
	std::string op = "sign";
	SessionRef session;
	std::map<std::string, std::string> params;
	bool has_deadline = false;
	uint64_t deadline_ms = 0;
};

struct ErrorInfo
{
	std::string code;
	std::string message;
	std::string stage;
	bool retryable = false;
	bool new_session = false;
};

struct Meta
{
	uint64_t queue_us = 0;
	uint64_t init_us = 0;
	uint64_t invoke_us = 0;
	uint64_t instance_requests = 0;
	// The instance retires after this response; its session id is no longer
	// valid and the client should omit |session| on the next request.
	bool recycled = false;
};

struct Response
{
	uint32_t version = kVersion;
	std::string id;
	std::string app;
	std::string worker;
	std::string instance;
	bool ok = false;
	std::map<std::string, std::string> result;
	ErrorInfo error;
	Meta meta;
};

enum class ParseStatus
{
	kOk,
	kBadJson,
	kBadVersion,
	kBadRequest,
};

// Parses one request frame. On failure |message| explains why and |request_id|
// carries the id when it could be extracted, so the caller can produce a
// correlated error response.
ParseStatus parse_request(std::string_view frame, Request *request, std::string *message, std::string *request_id);
bool serialize_request(const Request &request, std::string *frame, std::string *error);

ParseStatus parse_response(std::string_view frame, Response *response, std::string *message);
bool serialize_response(const Response &response, std::string *frame, std::string *error);

// Builds a correlated error response for a request that failed before it
// reached the guest. |id| may be empty when the frame could not be parsed.
Response make_error_response(std::string_view id,
	std::string_view app,
	std::string_view worker,
	std::string_view instance,
	std::string_view code,
	std::string_view message,
	std::string_view stage,
	bool retryable,
	bool new_session = false);

} // namespace duckemu::protocol
