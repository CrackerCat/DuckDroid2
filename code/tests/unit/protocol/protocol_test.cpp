// S1: framing and message validation. The wire format is the contract between
// System, Worker and Instance, so the edge cases here (short reads, bad
// lengths, rejected fields) are the ones a malformed client will hit first.

#include <cstdint>
#include <string>

#include "common/uds/socket.hpp"
#include "protocol/channel.hpp"
#include "protocol/frame.hpp"
#include "protocol/instance_messages.hpp"
#include "protocol/messages.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;

namespace
{

void TestFrames()
{
	common::uds::Socket writer;
	common::uds::Socket reader;
	std::string error;
	CHECK(common::uds::make_pair(&writer, &reader, &error));

	CHECK(protocol::write_frame(writer, "hello", &error));
	std::string payload;
	CHECK(protocol::read_frame(reader, &payload, &error));
	CHECK(payload == "hello");

	CHECK(!protocol::write_frame(writer, "", &error));
	std::string too_large(protocol::kMaxFrameBytes + 1, 'x');
	CHECK(!protocol::write_frame(writer, too_large, &error));
}

void TestFrameReaderIncremental()
{
	protocol::FrameReader reader;
	std::string payload;
	std::string error;

	// One byte of a 4-byte header is not enough.
	const uint8_t first[1] = { 0 };
	CHECK(reader.consume(first, sizeof(first), &payload, &error) == protocol::FrameReader::Status::kNeedMore);
	CHECK_EQ(reader.buffered_bytes(), 1u);

	// Rest of the header (length 3) plus the payload, split after the header.
	const uint8_t rest_header[3] = { 0, 0, 3 };
	CHECK(reader.consume(rest_header, sizeof(rest_header), &payload, &error) ==
		protocol::FrameReader::Status::kNeedMore);
	const uint8_t body[3] = { 'a', 'b', 'c' };
	CHECK(reader.consume(body, sizeof(body), &payload, &error) == protocol::FrameReader::Status::kFrame);
	CHECK(payload == "abc");
	CHECK_EQ(reader.buffered_bytes(), 0u);
}

void TestFrameReaderMultipleFrames()
{
	protocol::FrameReader reader;
	std::string payload;
	std::string error;

	const uint8_t chunk[10] = { 0, 0, 0, 1, 'x', 0, 0, 0, 1, 'y' };
	CHECK(reader.consume(chunk, sizeof(chunk), &payload, &error) == protocol::FrameReader::Status::kFrame);
	CHECK(payload == "x");
	CHECK_EQ(reader.buffered_bytes(), 5u);
	CHECK(reader.consume(nullptr, 0, &payload, &error) == protocol::FrameReader::Status::kFrame);
	CHECK(payload == "y");
	CHECK_EQ(reader.buffered_bytes(), 0u);
}

void TestFrameReaderRejectsBadLength()
{
	protocol::FrameReader reader;
	std::string payload;
	std::string error;

	const uint8_t zero_length[4] = { 0, 0, 0, 0 };
	CHECK(reader.consume(zero_length, sizeof(zero_length), &payload, &error) ==
		protocol::FrameReader::Status::kError);
	// Sticky: the stream cannot be resynchronized.
	CHECK(reader.consume(nullptr, 0, &payload, &error) == protocol::FrameReader::Status::kError);

	protocol::FrameReader bounded(8);
	const uint8_t oversized[4] = { 0, 0, 0, 9 };
	CHECK(bounded.consume(oversized, sizeof(oversized), &payload, &error) ==
		protocol::FrameReader::Status::kError);
}

void TestRequestRoundTrip()
{
	protocol::Request request;
	request.id = "req-1";
	request.app = "libeleven";
	request.op = "sign";
	request.session.present = true;
	request.session.id = "session-1";
	request.session.seed = 42;
	request.params["value"] = "1";
	request.has_deadline = true;
	request.deadline_ms = 5000;

	std::string frame;
	std::string error;
	CHECK(protocol::serialize_request(request, &frame, &error));

	protocol::Request parsed;
	std::string message;
	std::string request_id;
	CHECK(protocol::parse_request(frame, &parsed, &message, &request_id) == protocol::ParseStatus::kOk);
	CHECK(parsed.id == "req-1");
	CHECK(parsed.app == "libeleven");
	CHECK(parsed.op == "sign");
	CHECK(parsed.session.present);
	CHECK(parsed.session.id == "session-1");
	CHECK_EQ(parsed.session.seed, 42u);
	CHECK_EQ(parsed.params.size(), 1u);
	CHECK(parsed.params["value"] == "1");
	CHECK(parsed.has_deadline);
	CHECK_EQ(parsed.deadline_ms, 5000u);
	CHECK(parsed.version == protocol::kVersion);
}

void TestRequestDefaultsAndRejections()
{
	protocol::Request parsed;
	std::string message;
	std::string request_id;

	CHECK(protocol::parse_request("{\"version\":1,\"id\":\"x\"}", &parsed, &message, &request_id) ==
		protocol::ParseStatus::kOk);
	CHECK(parsed.op == "sign");
	CHECK(!parsed.session.present);
	CHECK(parsed.params.empty());
	CHECK(!parsed.has_deadline);

	CHECK(protocol::parse_request("not json", &parsed, &message, &request_id) ==
		protocol::ParseStatus::kBadJson);
	CHECK(protocol::parse_request("[]", &parsed, &message, &request_id) ==
		protocol::ParseStatus::kBadJson);
	CHECK(protocol::parse_request("{\"id\":\"x\"}", &parsed, &message, &request_id) ==
		protocol::ParseStatus::kBadVersion);
	CHECK(protocol::parse_request("{\"version\":2,\"id\":\"x\"}", &parsed, &message, &request_id) ==
		protocol::ParseStatus::kBadVersion);

	CHECK(protocol::parse_request("{\"version\":1}", &parsed, &message, &request_id) ==
		protocol::ParseStatus::kBadRequest);
	CHECK(protocol::parse_request("{\"version\":1,\"id\":\"\"}", &parsed, &message, &request_id) ==
		protocol::ParseStatus::kBadRequest);
	CHECK(protocol::parse_request("{\"version\":1,\"id\":\"x\",\"session\":{}}", &parsed, &message,
			  &request_id) == protocol::ParseStatus::kBadRequest);
	CHECK(protocol::parse_request("{\"version\":1,\"id\":\"x\",\"params\":{\"a\":[1]}}", &parsed, &message,
			  &request_id) == protocol::ParseStatus::kBadRequest);
	CHECK(protocol::parse_request("{\"version\":1,\"id\":\"x\",\"deadline_ms\":999999999999}", &parsed,
			  &message, &request_id) == protocol::ParseStatus::kBadRequest);

	// A rejected request still reports its id so the caller can correlate the
	// error response.
	CHECK(protocol::parse_request("{\"version\":1,\"id\":\"bad\",\"session\":{}}", &parsed, &message,
			  &request_id) == protocol::ParseStatus::kBadRequest);
	CHECK(request_id == "bad");
}

void TestResponseRoundTrip()
{
	protocol::Response response;
	response.id = "req-1";
	response.app = "libeleven";
	response.worker = "worker-1";
	response.instance = "instance-1";
	response.ok = false;
	response.error.code = protocol::error_code::kGuestFault;
	response.error.message = "guest exited with 1";
	response.error.stage = protocol::stage::kInvoke;
	response.error.retryable = true;
	response.error.new_session = false;
	response.meta.queue_us = 10;
	response.meta.init_us = 20;
	response.meta.invoke_us = 30;
	response.meta.instance_requests = 4;
	response.meta.recycled = true;

	std::string frame;
	std::string error;
	CHECK(protocol::serialize_response(response, &frame, &error));

	protocol::Response parsed;
	std::string message;
	CHECK(protocol::parse_response(frame, &parsed, &message) == protocol::ParseStatus::kOk);
	CHECK(parsed.id == "req-1");
	CHECK(parsed.app == "libeleven");
	CHECK(parsed.worker == "worker-1");
	CHECK(parsed.instance == "instance-1");
	CHECK(!parsed.ok);
	CHECK(parsed.error.code == protocol::error_code::kGuestFault);
	CHECK(parsed.error.message == "guest exited with 1");
	CHECK(parsed.error.stage == protocol::stage::kInvoke);
	CHECK(parsed.error.retryable);
	CHECK(!parsed.error.new_session);
	CHECK_EQ(parsed.meta.queue_us, 10u);
	CHECK_EQ(parsed.meta.init_us, 20u);
	CHECK_EQ(parsed.meta.invoke_us, 30u);
	CHECK_EQ(parsed.meta.instance_requests, 4u);
	CHECK(parsed.meta.recycled);

	protocol::Response success;
	success.id = "req-2";
	success.ok = true;
	success.result["value"] = "1:0";
	std::string success_frame;
	CHECK(protocol::serialize_response(success, &success_frame, &error));
	protocol::Response parsed_success;
	CHECK(protocol::parse_response(success_frame, &parsed_success, &message) == protocol::ParseStatus::kOk);
	CHECK(parsed_success.ok);
	CHECK(parsed_success.result["value"] == "1:0");
	CHECK(parsed_success.error.code.empty());
}

void TestErrorResponse()
{
	const protocol::Response response = protocol::make_error_response("req-1", "libeleven", "w1", "",
		protocol::error_code::kBadJson, "invalid JSON", protocol::stage::kParse, false);
	CHECK(response.id == "req-1");
	CHECK(!response.ok);
	CHECK(response.error.code == protocol::error_code::kBadJson);
	CHECK(response.error.stage == protocol::stage::kParse);
	CHECK(!response.error.retryable);

	std::string frame;
	std::string error;
	CHECK(protocol::serialize_response(response, &frame, &error));
	protocol::Response parsed;
	std::string message;
	CHECK(protocol::parse_response(frame, &parsed, &message) == protocol::ParseStatus::kOk);
	CHECK(parsed.error.code == protocol::error_code::kBadJson);
}

void TestChannelEnvelope()
{
	common::uds::Socket writer;
	common::uds::Socket reader;
	std::string error;
	CHECK(common::uds::make_pair(&writer, &reader, &error));

	CHECK(protocol::write_channel(writer, 7, "{\"id\":\"x\"}", &error));
	uint32_t conn_id = 0;
	std::string payload;
	CHECK(protocol::read_channel(reader, &conn_id, &payload, &error));
	CHECK_EQ(conn_id, 7u);
	CHECK(payload == "{\"id\":\"x\"}");

	CHECK(!protocol::write_channel(writer, 1, "", &error));
}

void TestInstanceMessages()
{
	protocol::instance::Message ready;
	ready.type = protocol::instance::Type::kReady;
	ready.instance = "instance-1";
	ready.init_us = 123;
	std::string frame;
	std::string error;
	CHECK(protocol::instance::serialize(ready, &frame, &error));
	protocol::instance::Message parsed;
	CHECK(protocol::instance::parse(frame, &parsed, &error));
	CHECK(parsed.type == protocol::instance::Type::kReady);
	CHECK(parsed.instance == "instance-1");
	CHECK_EQ(parsed.init_us, 123u);

	protocol::instance::Message invoke;
	invoke.type = protocol::instance::Type::kInvoke;
	invoke.id = "req-1";
	invoke.op = "sign";
	invoke.params["value"] = "1";
	CHECK(protocol::instance::serialize(invoke, &frame, &error));
	CHECK(protocol::instance::parse(frame, &parsed, &error));
	CHECK(parsed.type == protocol::instance::Type::kInvoke);
	CHECK(parsed.id == "req-1");
	CHECK(parsed.op == "sign");
	CHECK(parsed.params["value"] == "1");

	protocol::instance::Message result;
	result.type = protocol::instance::Type::kResult;
	result.id = "req-1";
	result.ok = true;
	result.result["value"] = "1:0";
	result.invoke_us = 42;
	result.requests = 3;
	CHECK(protocol::instance::serialize(result, &frame, &error));
	CHECK(protocol::instance::parse(frame, &parsed, &error));
	CHECK(parsed.ok);
	CHECK(parsed.result["value"] == "1:0");
	CHECK_EQ(parsed.invoke_us, 42u);
	CHECK_EQ(parsed.requests, 3u);

	protocol::instance::Message failed_result;
	failed_result.type = protocol::instance::Type::kResult;
	failed_result.id = "req-2";
	failed_result.ok = false;
	failed_result.error.code = protocol::error_code::kAppError;
	failed_result.error.message = "boom";
	CHECK(protocol::instance::serialize(failed_result, &frame, &error));
	CHECK(protocol::instance::parse(frame, &parsed, &error));
	CHECK(!parsed.ok);
	CHECK(parsed.error.code == protocol::error_code::kAppError);
	CHECK(parsed.error.message == "boom");

	protocol::instance::Message missing_id;
	missing_id.type = protocol::instance::Type::kInvoke;
	CHECK(!protocol::instance::serialize(missing_id, &frame, &error));
	CHECK(!protocol::instance::parse("{\"type\":\"bogus\"}", &parsed, &error));
}

} // namespace

int main()
{
	TestFrames();
	TestFrameReaderIncremental();
	TestFrameReaderMultipleFrames();
	TestFrameReaderRejectsBadLength();
	TestRequestRoundTrip();
	TestRequestDefaultsAndRejections();
	TestResponseRoundTrip();
	TestErrorResponse();
	TestChannelEnvelope();
	TestInstanceMessages();
	return test::finish("protocol_test");
}
