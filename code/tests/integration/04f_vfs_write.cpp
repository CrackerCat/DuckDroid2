// SPDX-License-Identifier: GPL-2.0-only
// Writable sandbox: one host directory overlays the read-only roots for guest
// "/", so open(O_CREAT|O_TRUNC|O_APPEND), write, fsync, ftruncate, stat,
// mkdirat, unlinkat and renameat all work on a normal Android path. Without a
// sandbox the same calls answer EROFS.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <unistd.h>

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

uint64_t callFunction(RuntimeInstance &instance, uint64_t handle, std::string_view name)
{
	auto entry = instance.lookupSymbol(handle, name);
	CHECK_RESULT(entry);
	if (!entry.ok())
	{
		return 0xDEAD;
	}
	auto result = instance.callFunction(entry.value());
	CHECK_RESULT(result);
	return result.ok() ? result.value().return_value : 0xDEAD;
}

} // namespace

int main()
{
	namespace fs = std::filesystem;
	std::error_code error;
	const fs::path sandbox = fs::temp_directory_path(error) /
				 ("duckemu-vfs-write-" + std::to_string(::getpid()));
	if (error)
	{
		std::fprintf(stderr, "cannot locate a temp directory\n");
		return 1;
	}
	fs::remove_all(sandbox, error);
	fs::create_directories(sandbox / "data", error);
	{
		std::ofstream seed(sandbox / "data" / "preloaded.txt", std::ios::binary);
		seed << "seed";
	}
	CHECK(!error);

	RuntimeConfig config;
	config.pid = 1000;
	config.tid = 1;
	config.library_search_paths = { DUCKEMU_SO_DIR };
	config.filesystem_write_root = sandbox.string();

	auto created = RuntimeInstance::create(config);
	CHECK_RESULT(created);
	if (!created.ok())
	{
		fs::remove_all(sandbox);
		return test::finish("04f_vfs_write");
	}
	RuntimeInstance &instance = *created.value();

	auto loaded = instance.loadLibrary(soPath("libwr.so"));
	CHECK_RESULT(loaded);
	if (loaded.ok())
	{
		const uint64_t handle = loaded.value();

		const uint64_t result = callFunction(instance, handle, "write_checks");
		CHECK_EQ(result, 0u);
		std::printf("  write_checks returned %lld\n", static_cast<long long>(result));

		// Host-side view of the same sandbox: the fixture cleaned up after
		// itself and never touched the pre-seeded file.
		CHECK(!fs::exists(sandbox / "data" / "local" / "tmp" / "wr2.txt"));
		CHECK(!fs::exists(sandbox / "data" / "local" / "tmp" / "wdir"));
		CHECK(fs::exists(sandbox / "data" / "preloaded.txt"));
	}

	// ------------------------------------------------------------------
	// No sandbox: the namespace stays read-only and mutating calls fail with
	// EROFS instead of disappearing into an unimplemented syscall.
	// ------------------------------------------------------------------
	RuntimeConfig read_only = config;
	read_only.filesystem_write_root.clear();
	auto created_read_only = RuntimeInstance::create(read_only);
	CHECK_RESULT(created_read_only);
	if (created_read_only.ok())
	{
		RuntimeInstance &plain = *created_read_only.value();
		auto plain_loaded = plain.loadLibrary(soPath("libwr.so"));
		CHECK_RESULT(plain_loaded);
		if (plain_loaded.ok())
		{
			const uint64_t refused = callFunction(plain, plain_loaded.value(), "write_refused");
			CHECK_EQ(refused, 0u);
			std::printf("  write_refused returned %lld\n", static_cast<long long>(refused));
		}
	}

	fs::remove_all(sandbox, error);
	return test::finish("04f_vfs_write");
}
