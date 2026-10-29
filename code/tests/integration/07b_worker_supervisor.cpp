// M7 S3: the worker supervisor.
//
// Two real instance processes (the built duckemu_worker, exec'd with
// --instance). The test plays System over the channel envelope: concurrent
// requests must land on different instances, and killing one instance must be
// detected and replaced without breaking later requests.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <set>
#include <signal.h>
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

constexpr uint64_t kBaseWallNs = 1792886400000000000ULL;
constexpr uint64_t kBaseWallMs = kBaseWallNs / 1000000ULL;

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

bool SendRequest(const common::uds::Socket &channel, uint32_t conn_id, const std::string &id, int value)
{
	Request request;
	request.id = id;
	request.app = "libeleven";
	request.op = "sign";
	request.params["value"] = std::to_string(value);

	std::string payload;
	std::string error;
	if (!serialize_request(request, &payload, &error))
	{
		std::fprintf(stderr, "serialize request failed: %s\n", error.c_str());
		return false;
	}
	if (!write_channel(channel, conn_id, payload, &error))
	{
		std::fprintf(stderr, "write request failed: %s\n", error.c_str());
		return false;
	}
	return true;
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

} // namespace

int main()
{
	int fds[2] = { -1, -1 };
	CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);

	worker::WorkerConfig config;
	config.worker_id = "w-test";
	config.app.app = "libeleven";
	config.app.library = DUCKEMU_LIBELEVEN_SO;
	config.app.rom_libc = std::string(DUCKEMU_ROM_BOOTSTRAP) + "/libc.so";
	config.app.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };
	config.instance_executable = DUCKEMU_WORKER_EXEC;
	config.instances = 2;
	config.has_wall_time = true;
	config.wall_time_ns = kBaseWallNs;

	std::string error;
	auto worker = worker::WorkerProcess::create(config, &error);
	CHECK(worker != nullptr);
	if (!worker)
	{
		std::fprintf(stderr, "worker create failed: %s\n", error.c_str());
		return test::finish("07b_worker_supervisor");
	}

	std::atomic<bool> run_ok{ false };
	std::string run_error;
	std::thread runner([&]() { run_ok = worker->run(fds[0], &run_error); });
	common::uds::Socket channel(fds[1]);

	const bool ready = WaitFor([&]() { return worker->ready_instances() == 2; }, 60000);
	CHECK(ready);
	if (!ready)
	{
		channel.close();
		runner.join();
		return test::finish("07b_worker_supervisor");
	}

	// Four requests: they must be spread over both instances.
	for (int value = 1; value <= 4; ++value)
	{
		CHECK(SendRequest(channel, static_cast<uint32_t>(value), "req-" + std::to_string(value), value));
	}
	std::set<std::string> instances;
	int received = 0;
	for (int index = 0; index < 4; ++index)
	{
		uint32_t conn_id = 0;
		Response response;
		if (!ReceiveResponse(channel, &conn_id, &response))
		{
			break;
		}
		++received;
		CHECK(response.ok);
		CHECK(response.id == "req-" + std::to_string(conn_id));
		CHECK(response.worker == "w-test");
		CHECK(response.instance.rfind("w-test-i", 0) == 0);
		CHECK(response.meta.instance_requests >= 1);
		instances.insert(response.instance);

		const auto value = response.result.find("value");
		CHECK(value != response.result.end());
		if (value == response.result.end())
		{
			continue;
		}
		const std::string expected_prefix = std::to_string(conn_id) + ":";
		const std::string decoded = Decode(value->second);
		CHECK(decoded.rfind(expected_prefix, 0) == 0);
		if (decoded.rfind(expected_prefix, 0) == 0)
		{
			const long long ms = std::strtoll(decoded.c_str() + expected_prefix.size(), nullptr, 10);
			CHECK(ms >= static_cast<long long>(kBaseWallMs));
			CHECK(ms <= static_cast<long long>(kBaseWallMs + 8));
		}
	}
	CHECK_EQ(received, 4u);
	CHECK_EQ(instances.size(), 2u);

	// Kill one instance. The slot must detect it and start a replacement.
	const std::vector<int> before = worker->instance_pids();
	CHECK_EQ(before.size(), 2u);
	if (before.size() == 2 && before[0] > 0)
	{
		::kill(before[0], SIGKILL);
		const bool respawned = WaitFor(
			[&]() {
				const std::vector<int> now = worker->instance_pids();
				return now.size() == 2 && now[0] > 0 && now[0] != before[0] &&
					worker->ready_instances() == 2;
			},
			60000);
		CHECK(respawned);
	}

	// Requests after the crash must succeed on the replacement.
	for (int value = 5; value <= 6; ++value)
	{
		CHECK(SendRequest(channel, static_cast<uint32_t>(value), "req-" + std::to_string(value), value));
	}
	for (int index = 0; index < 2; ++index)
	{
		uint32_t conn_id = 0;
		Response response;
		if (!ReceiveResponse(channel, &conn_id, &response))
		{
			break;
		}
		CHECK(response.ok);
		CHECK(response.id == "req-" + std::to_string(conn_id));
	}

	channel.close();
	runner.join();
	CHECK(run_ok.load());

	return test::finish("07b_worker_supervisor");
}
