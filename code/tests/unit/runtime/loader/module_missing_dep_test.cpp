// SPDX-License-Identifier: GPL-2.0-only
// Regression: a module whose DT_NEEDED cannot be resolved must return an
// error. The failure path used to erase the Module from the registry and then
// build the message from `registered.soname` / `needed` (a reference into the
// destroyed module), which ASan reports as heap-use-after-free.
//
// The fixture is liba.so with "libb.so" patched to "libx.so" in .dynstr (same
// length), copied to a private directory so it never disturbs other tests.

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "runtime/instance/runtime_instance.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

namespace
{

std::string ReadFile(const std::string &path)
{
	std::ifstream input(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool WriteFile(const std::string &path, const std::string &bytes)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	return output.good();
}

} // namespace

int main()
{
	const std::string source = std::string(DUCKEMU_SO_DIR) + "/liba.so";
	std::string bytes = ReadFile(source);
	CHECK(!bytes.empty());

	size_t patched = 0;
	size_t found = bytes.find("libb.so");
	while (found != std::string::npos)
	{
		bytes.replace(found, 7, "libx.so");
		++patched;
		found = bytes.find("libb.so", found + 7);
	}
	CHECK_EQ(patched, 1u);

	const std::string directory = "/tmp/duckemu-missing-dep-" + std::to_string(::getpid());
	::mkdir(directory.c_str(), 0700);
	const std::string broken = directory + "/liba.so";
	CHECK(WriteFile(broken, bytes));

	RuntimeConfig config;
	config.library_search_paths = { DUCKEMU_SO_DIR };
	config.enable_test_import_stubs = true;

	auto created = RuntimeInstance::create(config);
	CHECK_RESULT(created);
	if (!created.ok())
	{
		return test::finish("module_missing_dep_test");
	}
	RuntimeInstance &instance = *created.value();

	auto loaded = instance.loadLibrary(broken);
	CHECK(!loaded.ok());
	if (!loaded.ok())
	{
		CHECK(loaded.error().message.find("libx.so") != std::string::npos);
		std::printf("  error: %s\n", loaded.error().message.c_str());
	}

	::unlink(broken.c_str());
	::rmdir(directory.c_str());
	return test::finish("module_missing_dep_test");
}
