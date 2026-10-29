// SPDX-License-Identifier: GPL-2.0-only
// M6: deterministic wall clock. With setWallTimeNs the guest's
// clock_gettime(CLOCK_REALTIME) starts at the injected base and advances
// exactly 1 ms per read (DuckDroid parity), so libeleven's data(int) — whose
// result embeds now_ms — becomes reproducible.
//
// data(n) is custom_b64(f"{n}:{now_ms}") with the alphabet stored in the .so at
// file offset 0xa1c29. The test decodes the returned Java String and checks the
// exact timestamps.

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "libeleven_case/libeleven_env.hpp"
#include "runtime/instance/runtime_instance.hpp"
#include "runtime/observer/semantic/jni_event.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

namespace
{

constexpr const char *kTargetClass =
	"com/yuanrenxue/challenge/fragment/challenge/ChallengeElevenFragment";
// 2026-10-01T00:00:00Z. Same base DuckDroid's libeleven README uses.
constexpr uint64_t kWallTimeNs = 1792886400000000000ULL;
constexpr uint64_t kWallTimeMs = kWallTimeNs / 1000000ULL;
constexpr std::string_view kAlphabet =
	"45AogfZw/9+FcYrSVH1TKM7k3BebEtqCIGPJyQD60lUXpNmhWuRLds8jainzv2Ox";

std::string romLibc()
{
	return std::string(DUCKEMU_ROM_BOOTSTRAP) + "/libc.so";
}

std::string decode(std::string_view encoded)
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

std::string callAndDecode(RuntimeInstance &instance, int value)
{
	const std::array<uint64_t, 8> arguments = {
		static_cast<uint64_t>(static_cast<uint32_t>(value))
	};
	auto called = instance.callRegisteredNative(kTargetClass, "data", "(I)Ljava/lang/String;",
		arguments);
	CHECK_RESULT(called);
	if (!called.ok() || called.value() == 0)
	{
		return {};
	}
	auto text = instance.jniStringUtf8(called.value());
	CHECK_RESULT(text);
	return text.ok() ? decode(text.value()) : std::string{};
}

} // namespace

int main()
{
	RuntimeConfig config;
	config.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };

	auto created = RuntimeInstance::create(config);
	CHECK_RESULT(created);
	if (!created.ok())
	{
		return test::finish("06_clock_injection");
	}
	RuntimeInstance &instance = *created.value();

	std::vector<observer::JniEvent> events;
	instance.addJniListener([&events](const observer::JniEvent &event) {
		events.push_back(event);
	});

	auto installed = instance.installJavaEnvironment(&user::libeleven::installEnvironment);
	CHECK_RESULT(installed);
	if (!installed.ok())
	{
		return test::finish("06_clock_injection");
	}

	// Inject before loading anything: every subsequent read is deterministic.
	instance.setWallTimeNs(kWallTimeNs);

	auto libc = instance.loadLibrary(romLibc());
	CHECK_RESULT(libc);
	auto eleven = instance.loadLibrary(DUCKEMU_LIBELEVEN_SO);
	CHECK_RESULT(eleven);
	if (!libc.ok() || !eleven.ok())
	{
		std::printf("  load failed: libc=%s libeleven=%s\n",
			libc.ok() ? "ok" : libc.error().message.c_str(),
			eleven.ok() ? "ok" : eleven.error().message.c_str());
		return test::finish("06_clock_injection");
	}

	// Each data() call reads CLOCK_REALTIME exactly once.
	const std::string first = callAndDecode(instance, 1);
	const std::string second = callAndDecode(instance, 1);
	const std::string third = callAndDecode(instance, 100);
	std::printf("  first=%s second=%s third=%s\n", first.c_str(), second.c_str(),
		third.c_str());

	const std::string expected_first = "1:" + std::to_string(kWallTimeMs);
	const std::string expected_second = "1:" + std::to_string(kWallTimeMs + 1);
	const std::string expected_third = "100:" + std::to_string(kWallTimeMs + 2);
	CHECK(first == expected_first);
	CHECK(second == expected_second);
	CHECK(third == expected_third);

	// Host mode still returns real time.
	instance.useHostWallClock();
	const std::string host = callAndDecode(instance, 1);
	const size_t separator = host.find(':');
	CHECK(separator != std::string::npos);
	if (separator != std::string::npos)
	{
		const uint64_t host_ms = std::stoull(host.substr(separator + 1));
		std::printf("  host=%s\n", host.c_str());
		CHECK(host_ms > 1500000000000ULL);
		CHECK(host_ms != kWallTimeMs + 3);
	}

	CHECK_EQ(events.size(), 0);
	for (const observer::JniEvent &event : events)
	{
		std::printf("  jni event kind=%s class=%s name=%s\n",
			std::string(observer::jniEventKindName(event.kind)).c_str(),
			event.class_descriptor.c_str(),
			event.name.c_str());
	}

	return test::finish("06_clock_injection");
}
