// SPDX-License-Identifier: GPL-2.0-only
// M7 S4: System gateway end to end.
//
// The test plays the Web API: it connects to <socket-dir>/libeleven.sock,
// sends framed JSON, and reads framed JSON back. System spawns the real
// duckemu_worker, which spawns real instance processes. Killing a worker must
// be detected, the connection dropped, and a replacement brought up.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "common/uds/socket.hpp"
#include "protocol/frame.hpp"
#include "protocol/messages.hpp"
#include "support/test_harness.hpp"
#include "system/system.hpp"

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

bool SendRequest(const common::uds::Socket &client, const std::string &id, int value)
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
		return false;
	}
	return write_frame(client, payload, &error);
}

bool ReceiveResponse(const common::uds::Socket &client, Response *response)
{
	std::string payload;
	std::string error;
	if (!read_frame(client, &payload, &error))
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

bool Connect(const std::string &path, common::uds::Socket *client)
{
	return WaitFor(
		[&]() {
			if (client->valid())
			{
				return true;
			}
			std::string error;
			return client->connect_to(path, &error);
		},
		10000);
}

// Requests issued while the worker's instances are still initializing get
// OVERLOADED; a real client retries with the same id.
bool SignWithRetry(const common::uds::Socket &client, const std::string &id, int value,
	Response *response, int timeout_ms)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (!SendRequest(client, id, value))
		{
			return false;
		}
		if (!ReceiveResponse(client, response))
		{
			return false;
		}
		if (response->ok || response->error.code != error_code::kOverloaded)
		{
			return response->ok;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	return false;
}

} // namespace

int main()
{
	const std::string socket_dir = "/tmp/duckemu-07c-" + std::to_string(::getpid());
	::mkdir(socket_dir.c_str(), 0700);

	system::SystemConfig config;
	config.socket_dir = socket_dir;
	system::AppConfig app;
	app.worker.app = "libeleven";
	app.worker.worker_executable = DUCKEMU_WORKER_EXEC;
	app.worker.library = DUCKEMU_LIBELEVEN_SO;
	app.worker.rom_libc = std::string(DUCKEMU_ROM_BOOTSTRAP) + "/libc.so";
	app.worker.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };
	app.worker.instances = 2;
	app.worker.workers = 1;
	app.worker.has_wall_time = true;
	app.worker.wall_time_ns = kBaseWallNs;
	config.apps.push_back(app);

	std::string error;
	auto system = system::System::create(config, &error);
	CHECK(system != nullptr);
	if (!system)
	{
		std::fprintf(stderr, "system create failed: %s\n", error.c_str());
		return test::finish("07c_system_gateway");
	}

	std::atomic<bool> run_ok{ false };
	std::thread runner([&]() { run_ok = system->run(&error); });

	const std::string path = system->socket_path("libeleven");
	CHECK(!path.empty());

	const bool worker_up = WaitFor(
		[&]() {
			const std::vector<int> pids = system->worker_pids("libeleven");
			return !pids.empty() && pids[0] > 0;
		},
		30000);
	CHECK(worker_up);

	common::uds::Socket client;
	CHECK(Connect(path, &client));

	Response response;
	const bool signed_once = SignWithRetry(client, "req-1", 1, &response, 60000);
	CHECK(signed_once);
	if (signed_once)
	{
		CHECK(response.id == "req-1");
		CHECK(response.app == "libeleven");
		CHECK(response.worker.rfind("libeleven-w0", 0) == 0);
		CHECK(response.instance.rfind("libeleven-w0", 0) == 0);
		const auto value = response.result.find("value");
		CHECK(value != response.result.end());
		if (value != response.result.end())
		{
			const std::string decoded = Decode(value->second);
			CHECK(decoded.rfind("1:", 0) == 0);
			if (decoded.rfind("1:", 0) == 0)
			{
				const long long ms = std::strtoll(decoded.c_str() + 2, nullptr, 10);
				CHECK(ms >= static_cast<long long>(kBaseWallMs));
				CHECK(ms <= static_cast<long long>(kBaseWallMs + 4));
			}
		}
	}

	// Kill the worker process. System must drop this connection and respawn.
	const std::vector<int> pids = system->worker_pids("libeleven");
	CHECK(!pids.empty());
	const int old_pid = pids.empty() ? -1 : pids[0];
	if (old_pid > 0)
	{
		::kill(old_pid, SIGKILL);
		client.close();
		const bool restarted = WaitFor(
			[&]() {
				const std::vector<int> now = system->worker_pids("libeleven");
				return !now.empty() && now[0] > 0 && now[0] != old_pid;
			},
			30000);
		CHECK(restarted);

		common::uds::Socket replacement;
		CHECK(Connect(path, &replacement));
		Response after_crash;
		const bool signed_again = SignWithRetry(replacement, "req-2", 2, &after_crash, 60000);
		CHECK(signed_again);
		if (signed_again)
		{
			CHECK(after_crash.id == "req-2");
			CHECK(after_crash.ok);
		}
	}

	system->stop();
	runner.join();
	CHECK(run_ok.load());

	::unlink(path.c_str());
	::rmdir(socket_dir.c_str());

	return test::finish("07c_system_gateway");
}
