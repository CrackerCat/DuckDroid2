// SPDX-License-Identifier: GPL-2.0-only
// Event loop: eventfd, poll/ppoll, epoll and timerfd. The fixture blocks the
// guest thread in ppoll/epoll_pwait, so this also covers parking with a
// deadline and the timerfd-driven wake.

#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>

#include "runtime/instance/runtime_instance.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

namespace
{

std::string soPath(std::string_view name)
{
	return std::string(DUCKEMU_SO_DIR) + "/" + std::string(name);
}

} // namespace

int main()
{
	RuntimeConfig config;
	config.pid = 1000;
	config.tid = 1;
	config.library_search_paths = { DUCKEMU_SO_DIR };

	auto created = RuntimeInstance::create(config);
	CHECK_RESULT(created);
	if (!created.ok())
	{
		return test::finish("04g_event_loop");
	}
	RuntimeInstance &instance = *created.value();

	auto loaded = instance.loadLibrary(soPath("libev.so"));
	CHECK_RESULT(loaded);
	if (!loaded.ok())
	{
		return test::finish("04g_event_loop");
	}
	auto entry = instance.lookupSymbol(loaded.value(), "event_checks");
	CHECK_RESULT(entry);
	if (!entry.ok())
	{
		return test::finish("04g_event_loop");
	}

	const auto start = std::chrono::steady_clock::now();
	auto result = instance.callFunction(entry.value());
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - start);
	CHECK_RESULT(result);
	if (result.ok())
	{
		CHECK_EQ(result.value().return_value, 0u);
		std::printf("  event_checks returned %lld after %lld ms\n",
			static_cast<long long>(result.value().return_value),
			static_cast<long long>(elapsed.count()));
		// The two timer waits are 10 ms and 5 ms; the 1 s poll deadline must
		// never be reached.
		CHECK(elapsed < std::chrono::milliseconds(900));
	}

	return test::finish("04g_event_loop");
}
