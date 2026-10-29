// M7 S5: session affinity and instance recycling (M6 warm-pool reset).
//
// One instance, max_requests_per_instance=2:
//   req1 (no session)  -> instance I, requests=1
//   req2 (session I)   -> same instance, requests=2, recycled=true
//   [slot replaces the process]
//   req3 (session I)   -> SESSION_NOT_FOUND + new_session
//   req4 (no session)  -> new instance, same deterministic output as req1

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "common/uds/socket.hpp"
#include "protocol/channel.hpp"
#include "protocol/messages.hpp"
#include "support/test_harness.hpp"
#include "worker/worker_process.hpp"

using namespace duckemu;
using namespace duckemu::protocol;

#ifndef DUCKEMU_ROM_BOOTSTRAP
#error "DUCKEMU_ROM_BOOTSTRAP must point at the ROM bootstrap lib directory"
#endif
#ifndef DUCKEMU_ROM_LIB64
#error "DUCKEMU_ROM_LIB64 must point at the ROM system/lib64 directory"
#endif
#ifndef DUCKEMU_LIBELEVEN_SO
#error "DUCKEMU_LIBELEVEN_SO must point at libeleven.so"
#endif
#ifndef DUCKEMU_WORKER_EXEC
#error "DUCKEMU_WORKER_EXEC must point at the built duckemu_worker"
#endif

namespace
{

constexpr std::string_view kAlphabet =
	"45AogfZw/9+FcYrSVH1TKM7k3BebEtqCIGPJyQD60lUXpNmhWuRLds8jainzv2Ox";

std::string Decode(std::string_view encoded)
{
	uint32_t bits = 0;
	int bit_count = 0;
	std::string plain;
	for (const char character : encoded)
	{
		if (character == '=')
		{
			break;
		}
		const size_t index = kAlphabet.find(character);
		if (index == std::string_view::npos)
		{
			return {};
		}
		bits = (bits << 6) | static_cast<uint32_t>(index);
		bit_count += 6;
		if (bit_count >= 8)
		{
			bit_count -= 8;
			plain.push_back(static_cast<char>((bits >> bit_count) & 0xFFu));
		}
	}
	return plain;
}

bool WaitFor(const std::function<bool()> &predicate, int timeout_ms)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (predicate())
		{
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	return predicate();
}

bool SendRequest(const common::uds::Socket &channel, uint32_t conn_id, const std::string &id, int value,
	std::string_view session)
{
	Request request;
	request.id = id;
	request.app = "libeleven";
	request.op = "sign";
	request.params["value"] = std::to_string(value);
	if (!session.empty())
	{
		request.session.present = true;
		request.session.id = std::string(session);
	}

	std::string payload;
	std::string error;
	if (!serialize_request(request, &payload, &error))
	{
		return false;
	}
	return write_channel(channel, conn_id, payload, &error);
}

bool ReceiveResponse(const common::uds::Socket &channel, uint32_t *conn_id, Response *response)
{
	std::string payload;
	std::string error;
	if (!read_channel(channel, conn_id, &payload, &error))
	{
		std::fprintf(stderr, "read response failed: %s\n", error.c_str());
		return false;
	}
	std::string message;
	if (parse_response(payload, response, &message) != ParseStatus::kOk)
	{
		std::fprintf(stderr, "parse response failed: %s\n", message.c_str());
		return false;
	}
	return true;
}

std::string ResultValue(const Response &response)
{
	const auto value = response.result.find("value");
	return value == response.result.end() ? std::string() : Decode(value->second);
}

} // namespace

int main()
{
	int fds[2] = { -1, -1 };
	CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);

	worker::WorkerConfig config;
	config.worker_id = "w-reset";
	config.app.app = "libeleven";
	config.app.library = DUCKEMU_LIBELEVEN_SO;
	config.app.rom_libc = std::string(DUCKEMU_ROM_BOOTSTRAP) + "/libc.so";
	config.app.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };
	config.instance_executable = DUCKEMU_WORKER_EXEC;
	config.instances = 1;
	config.max_requests_per_instance = 2;
	config.has_wall_time = true;
	config.wall_time_ns = 1792886400000000000ULL;

	std::string error;
	auto worker = worker::WorkerProcess::create(config, &error);
	CHECK(worker != nullptr);
	if (!worker)
	{
		std::fprintf(stderr, "worker create failed: %s\n", error.c_str());
		return test::finish("07d_session_reset");
	}

	std::atomic<bool> run_ok{ false };
	std::string run_error;
	std::thread runner([&]() { run_ok = worker->run(fds[0], &run_error); });
	common::uds::Socket channel(fds[1]);

	CHECK(WaitFor([&]() { return worker->ready_instances() == 1; }, 60000));

	// Capture the original process now: recycling reaps it before the
	// replacement is spawned, so reading pids later may legitimately see -1.
	const std::vector<int> initial = worker->instance_pids();
	CHECK_EQ(initial.size(), 1u);
	const int old_pid = initial.empty() ? -1 : initial[0];
	CHECK(old_pid > 0);

	// req1: a new session is bound to the only instance.
	Response first;
	CHECK(SendRequest(channel, 1, "req-1", 1, {}));
	uint32_t conn_id = 0;
	CHECK(ReceiveResponse(channel, &conn_id, &first));
	CHECK(first.ok);
	CHECK_EQ(conn_id, 1u);
	CHECK_EQ(first.meta.instance_requests, 1u);
	CHECK(!first.meta.recycled);
	const std::string first_instance = first.instance;
	const std::string first_value = ResultValue(first);
	CHECK(first_value.rfind("1:", 0) == 0);

	// req2: the session goes back to the same instance, which then retires.
	Response second;
	CHECK(SendRequest(channel, 2, "req-2", 1, first_instance));
	CHECK(ReceiveResponse(channel, &conn_id, &second));
	CHECK(second.ok);
	CHECK_EQ(conn_id, 2u);
	CHECK(second.instance == first_instance);
	CHECK_EQ(second.meta.instance_requests, 2u);
	CHECK(second.meta.recycled);

	// The slot replaces the process after the recycling response.
	const bool replaced = WaitFor(
		[&]() {
			const std::vector<int> now = worker->instance_pids();
			return now.size() == 1 && now[0] > 0 && now[0] != old_pid &&
				worker->ready_instances() == 1;
		},
		60000);
	CHECK(replaced);

	// req3: the old session id is gone.
	Response stale;
	CHECK(SendRequest(channel, 3, "req-3", 1, first_instance));
	CHECK(ReceiveResponse(channel, &conn_id, &stale));
	CHECK(!stale.ok);
	CHECK(stale.error.code == error_code::kSessionNotFound);
	CHECK(stale.error.new_session);

	// req4: a fresh instance; the injected clock makes the output identical.
	Response fourth;
	CHECK(SendRequest(channel, 4, "req-4", 1, {}));
	CHECK(ReceiveResponse(channel, &conn_id, &fourth));
	CHECK(fourth.ok);
	CHECK_EQ(conn_id, 4u);
	CHECK(fourth.instance != first_instance);
	CHECK(ResultValue(fourth) == first_value);

	channel.close();
	runner.join();
	CHECK(run_ok.load());

	return test::finish("07d_session_reset");
}
