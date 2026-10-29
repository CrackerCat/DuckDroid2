// SPDX-License-Identifier: GPL-2.0-only
// M7 S2: one instance process, driven over its control channel.
//
// The child runs the same code path the supervisor will spawn: create the
// RuntimeInstance after fork, install the plugin, report ready, then answer
// invoke frames. No supervisor, no UDS gateway yet.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common/uds/socket.hpp"
#include "protocol/frame.hpp"
#include "protocol/instance_messages.hpp"
#include "support/test_harness.hpp"
#include "worker/instance_host.hpp"

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

namespace
{

constexpr uint64_t kBaseWallNs = 1792886400000000000ULL;
constexpr uint64_t kBaseWallMs = kBaseWallNs / 1000000ULL;
// data(n) returns custom_b64(f"{n}:{now_ms}"); the alphabet lives in the .so.
// Same decoder as 06_clock_injection, so both tests assert the same values.
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

bool Receive(const common::uds::Socket &channel, instance::Message *message)
{
	std::string frame;
	std::string error;
	if (!read_frame(channel, &frame, &error))
	{
		std::fprintf(stderr, "receive failed: %s\n", error.c_str());
		return false;
	}
	if (!instance::parse(frame, message, &error))
	{
		std::fprintf(stderr, "parse failed: %s\n", error.c_str());
		return false;
	}
	return true;
}

bool Send(const common::uds::Socket &channel, const instance::Message &message)
{
	std::string frame;
	std::string error;
	if (!instance::serialize(message, &frame, &error))
	{
		std::fprintf(stderr, "serialize failed: %s\n", error.c_str());
		return false;
	}
	if (!write_frame(channel, frame, &error))
	{
		std::fprintf(stderr, "send failed: %s\n", error.c_str());
		return false;
	}
	return true;
}

int RunChild(int fd)
{
	worker::InstanceHostConfig config;
	config.ipc_fd = fd;
	config.instance_id = "instance-test-1";
	config.app.app = "libeleven";
	config.app.library = DUCKEMU_LIBELEVEN_SO;
	config.app.rom_libc = std::string(DUCKEMU_ROM_BOOTSTRAP) + "/libc.so";
	config.app.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };
	config.has_wall_time = true;
	config.wall_time_ns = kBaseWallNs;
	config.has_monotonic_time = true;
	config.monotonic_time_ns = 1000000ULL;
	return worker::run_instance_host(config);
}

} // namespace

int main()
{
	int fds[2] = { -1, -1 };
	CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);

	const pid_t pid = ::fork();
	CHECK(pid >= 0);
	if (pid == 0)
	{
		::close(fds[0]);
		const int code = RunChild(fds[1]);
		::close(fds[1]);
		::_exit(code);
	}
	::close(fds[1]);
	common::uds::Socket channel(fds[0]);

	instance::Message ready;
	if (!Receive(channel, &ready))
	{
		std::fprintf(stderr, "instance did not report ready\n");
		::waitpid(pid, nullptr, 0);
		return test::finish("07a_instance_host");
	}
	CHECK(ready.type == instance::Type::kReady);
	CHECK(ready.instance == "instance-test-1");
	CHECK(ready.init_us > 0);

	// First request: deterministic clock base, one read.
	instance::Message invoke;
	invoke.type = instance::Type::kInvoke;
	invoke.id = "req-1";
	invoke.op = "sign";
	invoke.params["value"] = "1";
	CHECK(Send(channel, invoke));

	instance::Message first;
	CHECK(Receive(channel, &first));
	CHECK(first.type == instance::Type::kResult);
	CHECK(first.id == "req-1");
	CHECK(first.ok);
	CHECK_EQ(first.requests, 1u);
	CHECK(first.error.code.empty());
	const auto first_value = first.result.find("value");
	CHECK(first_value != first.result.end());
	const std::string expected_first = "1:" + std::to_string(kBaseWallMs);
	if (first_value != first.result.end())
	{
		CHECK(Decode(first_value->second) == expected_first);
	}

	// Second request: the injected clock advanced, the instance is reused.
	invoke.id = "req-2";
	CHECK(Send(channel, invoke));
	instance::Message second;
	CHECK(Receive(channel, &second));
	CHECK(second.ok);
	CHECK(second.id == "req-2");
	CHECK_EQ(second.requests, 2u);
	const auto second_value = second.result.find("value");
	CHECK(second_value != second.result.end());
	if (second_value != second.result.end())
	{
		CHECK(Decode(second_value->second) == "1:" + std::to_string(kBaseWallMs + 1));
	}

	// Plugin-level rejection comes back as a correlated error, not a crash.
	instance::Message bad;
	bad.type = instance::Type::kInvoke;
	bad.id = "req-3";
	bad.op = "sign";
	CHECK(Send(channel, bad));
	instance::Message rejected;
	CHECK(Receive(channel, &rejected));
	CHECK(!rejected.ok);
	CHECK(rejected.id == "req-3");
	CHECK(rejected.error.code == "BAD_REQUEST");

	instance::Message shutdown;
	shutdown.type = instance::Type::kShutdown;
	CHECK(Send(channel, shutdown));

	int status = 0;
	CHECK(::waitpid(pid, &status, 0) == pid);
	CHECK(WIFEXITED(status));
	CHECK_EQ(WEXITSTATUS(status), 0u);

	return test::finish("07a_instance_host");
}
