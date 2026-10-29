// M7 gate: Web API -> System -> Worker -> RuntimeInstance, end to end.
//
// One instance, injected clock, data(1..100) over the real UDS protocol:
// every value must be exactly "n:<base+n-1>". When the DuckDroid reference
// runner is available it is executed too, and both sides must decode to the
// same "n:" payload.
//
// The DuckDroid runner cannot inject a clock (its runner has no such flag),
// so its timestamps come from the host clock. Parity here therefore compares
// the algorithm-dependent part; exact timestamp equality is asserted on our
// side, and is what the injected clock exists for.

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
#include <vector>

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
	return serialize_request(request, &payload, &error) &&
		write_frame(client, payload, &error);
}

bool ReceiveResponse(const common::uds::Socket &client, Response *response)
{
	std::string payload;
	std::string error;
	if (!read_frame(client, &payload, &error))
	{
		return false;
	}
	std::string message;
	return parse_response(payload, response, &message) == ParseStatus::kOk;
}

// Signs one value, retrying while the instance is still initializing.
bool Sign(const common::uds::Socket &client, const std::string &id, int value, std::string *decoded,
	int timeout_ms)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (!SendRequest(client, id, value))
		{
			return false;
		}
		Response response;
		if (!ReceiveResponse(client, &response))
		{
			return false;
		}
		if (response.ok)
		{
			const auto found = response.result.find("value");
			if (found == response.result.end())
			{
				return false;
			}
			*decoded = Decode(found->second);
			return true;
		}
		if (response.error.code != error_code::kOverloaded)
		{
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	return false;
}

#ifdef DUCKEMU_DUCK_DROID_ELEVEN_CASE
// Runs the DuckDroid reference runner and returns its decoded data(n) values.
bool RunReference(std::vector<std::string> *values)
{
	FILE *pipe = ::popen(DUCKEMU_DUCK_DROID_ELEVEN_CASE " 1 100", "r");
	if (pipe == nullptr)
	{
		return false;
	}
	char line[1024];
	while (std::fgets(line, sizeof(line), pipe) != nullptr)
	{
		const std::string text(line);
		const size_t marker = text.find(" = ");
		if (marker == std::string::npos)
		{
			continue;
		}
		std::string encoded = text.substr(marker + 3);
		while (!encoded.empty() && (encoded.back() == '\n' || encoded.back() == '\r'))
		{
			encoded.pop_back();
		}
		values->push_back(Decode(encoded));
	}
	return ::pclose(pipe) == 0 && values->size() == 100;
}
#endif

} // namespace

int main()
{
	const std::string socket_dir = "/tmp/duckemu-07e-" + std::to_string(::getpid());
	::mkdir(socket_dir.c_str(), 0700);

	system::SystemConfig config;
	config.socket_dir = socket_dir;
	system::AppConfig app;
	app.worker.app = "libeleven";
	app.worker.worker_executable = DUCKEMU_WORKER_EXEC;
	app.worker.library = DUCKEMU_LIBELEVEN_SO;
	app.worker.rom_libc = std::string(DUCKEMU_ROM_BOOTSTRAP) + "/libc.so";
	app.worker.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };
	app.worker.instances = 1;
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
		return test::finish("07_end_to_end");
	}

	std::atomic<bool> run_ok{ false };
	std::thread runner([&]() { run_ok = system->run(&error); });

	const std::string path = system->socket_path("libeleven");
	CHECK(!path.empty());

	// A connection accepted before any worker exists is dropped by System
	// (there is no healthy slot to own it), so wait for the first spawn.
	CHECK(WaitFor(
		[&]() {
			const std::vector<int> pids = system->worker_pids("libeleven");
			return !pids.empty() && pids[0] > 0;
		},
		30000));

	common::uds::Socket client;
	CHECK(WaitFor(
		[&]() {
			if (client.valid())
			{
				return true;
			}
			return client.connect_to(path, &error);
		},
		10000));

	// The deterministic sequence: one instance, one clock read per call.
	std::vector<std::string> ours;
	for (int value = 1; value <= 100; ++value)
	{
		std::string decoded;
		if (!Sign(client, "seq-" + std::to_string(value), value, &decoded, 60000))
		{
			std::fprintf(stderr, "sign(%d) failed\n", value);
			break;
		}
		ours.push_back(decoded);
	}
	CHECK_EQ(ours.size(), 100u);
	for (size_t index = 0; index < ours.size(); ++index)
	{
		const std::string expected =
			std::to_string(index + 1) + ":" + std::to_string(kBaseWallMs + index);
		CHECK(ours[index] == expected);
	}

#ifdef DUCKEMU_DUCK_DROID_ELEVEN_CASE
	std::vector<std::string> reference;
	const bool have_reference = RunReference(&reference);
	CHECK(have_reference);
	if (have_reference)
	{
		CHECK_EQ(reference.size(), 100u);
		for (size_t index = 0; index < reference.size() && index < ours.size(); ++index)
		{
			const std::string prefix = std::to_string(index + 1) + ":";
			CHECK(reference[index].rfind(prefix, 0) == 0);
			CHECK(ours[index].rfind(prefix, 0) == 0);
			// The timestamp half differs by construction (host clock vs injected
			// base); everything else must agree.
		}
	}
#endif

	// Replacing the instance must not change the answer: a new instance starts
	// from the injected base again.
	client.close();
	const std::vector<int> pids = system->worker_pids("libeleven");
	if (!pids.empty() && pids[0] > 0)
	{
		::kill(pids[0], SIGKILL);
		CHECK(WaitFor(
			[&]() {
				common::uds::Socket fresh;
				std::string connect_error;
				if (!fresh.connect_to(path, &connect_error))
				{
					return false;
				}
				std::string decoded;
				if (!Sign(fresh, "after-crash", 1, &decoded, 30000))
				{
					return false;
				}
				return decoded == "1:" + std::to_string(kBaseWallMs);
			},
			60000));
	}

	system->stop();
	runner.join();
	CHECK(run_ok.load());

	::unlink(path.c_str());
	::rmdir(socket_dir.c_str());

	return test::finish("07_end_to_end");
}
