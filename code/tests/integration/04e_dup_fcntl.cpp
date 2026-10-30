// SPDX-License-Identifier: GPL-2.0-only
// Descriptor primitives: dup/dup2/dup3 and fcntl. The fixture drives the raw
// syscalls, so this covers the dispatcher and the descriptor table together
// (shared offsets, close semantics, stdio duplication).

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
		return test::finish("04e_dup_fcntl");
	}
	RuntimeInstance &instance = *created.value();

	auto loaded = instance.loadLibrary(soPath("libfd.so"));
	CHECK_RESULT(loaded);
	if (!loaded.ok())
	{
		return test::finish("04e_dup_fcntl");
	}

	auto checks = instance.lookupSymbol(loaded.value(), "fd_checks");
	CHECK_RESULT(checks);
	if (!checks.ok())
	{
		return test::finish("04e_dup_fcntl");
	}

	auto result = instance.callFunction(checks.value());
	CHECK_RESULT(result);
	if (result.ok())
	{
		// A negative return is the fixture's check id.
		CHECK_EQ(result.value().return_value, 0u);
		std::printf("  fd_checks returned %lld\n",
			static_cast<long long>(result.value().return_value));
	}

	return test::finish("04e_dup_fcntl");
}
