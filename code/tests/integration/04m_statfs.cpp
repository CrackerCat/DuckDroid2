// SPDX-License-Identifier: GPL-2.0-only
// statfs(43)/fstatfs(44): the mounts a target probes before installing or
// unpacking. /data capacity comes from the device profile; the rest are
// classified by mount point.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "runtime/instance/runtime_instance.hpp"
#include "runtime/memory/region.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

namespace
{

constexpr uint32_t kPid = 1000;
constexpr uint32_t kTid = 1;
constexpr uint64_t kDataTotal = 8ull * 1024 * 1024 * 1024;
constexpr uint64_t kDataFree = 3ull * 1024 * 1024 * 1024;
constexpr uint32_t kExt4Magic = 0xEF53;
constexpr uint64_t kErofsMagic = 0xE0F5E1E2;
constexpr uint64_t kProcMagic = 0x9fa0;
constexpr uint64_t kTmpfsMagic = 0x01021994;
constexpr uint64_t kPipefsMagic = 0x50495045;
constexpr uint64_t kBlockSize = 4096;
constexpr int kORdOnly = 0;

// arm64 struct statfs: eleven 64-bit words with fsid at 56.
constexpr size_t kStatfsTypeOffset = 0;
constexpr size_t kStatfsBlockSizeOffset = 8;
constexpr size_t kStatfsBlocksOffset = 16;
constexpr size_t kStatfsBlocksAvailableOffset = 32;
constexpr size_t kStatfsFlagsOffset = 80;

std::string romLibc()
{
	return std::string(DUCKEMU_ROM_BOOTSTRAP) + "/libc.so";
}

uint64_t callGuest(RuntimeInstance &instance, GuestVA entry,
	const std::array<uint64_t, 8> &args)
{
	auto call = instance.callFunction(entry, args);
	CHECK_RESULT(call);
	return call.ok() ? call.value().return_value : 0;
}

uint64_t readU64(RuntimeInstance &instance, uint64_t address)
{
	uint64_t value = 0;
	auto read = instance.readMemory(GuestVA{ address }, &value, sizeof(value));
	CHECK_RESULT(read);
	return value;
}

} // namespace

int main()
{
	RuntimeConfig config;
	config.pid = kPid;
	config.tid = kTid;
	config.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };
	config.device.data_file_system_type = kExt4Magic;
	config.device.data_total_bytes = kDataTotal;
	config.device.data_free_bytes = kDataFree;

	auto created = RuntimeInstance::create(config);
	CHECK_RESULT(created);
	if (!created.ok())
	{
		return test::finish("04m_statfs");
	}
	RuntimeInstance &instance = *created.value();

	auto libc = instance.loadLibrary(romLibc());
	CHECK_RESULT(libc);
	if (!libc.ok())
	{
		return test::finish("04m_statfs");
	}
	const uint64_t handle = libc.value();

	auto lookup = [&](const char *name) -> GuestVA {
		auto found = instance.lookupSymbol(handle, name);
		CHECK_RESULT(found);
		return found.ok() ? found.value() : GuestVA{};
	};
	const GuestVA statfs_fn = lookup("statfs");
	const GuestVA fstatfs_fn = lookup("fstatfs");
	const GuestVA open_fn = lookup("open");
	const GuestVA close_fn = lookup("close");
	const GuestVA pipe2_fn = lookup("pipe2");

	auto path_page = instance.mapMemory(0x1000, PermReadWrite, "test-statfs-path");
	auto stat_page = instance.mapMemory(0x1000, PermReadWrite, "test-statfs-buffer");
	auto pair_page = instance.mapMemory(0x1000, PermReadWrite, "test-statfs-pipe");
	CHECK_RESULT(path_page);
	CHECK_RESULT(stat_page);
	CHECK_RESULT(pair_page);
	if (!path_page.ok() || !stat_page.ok() || !pair_page.ok())
	{
		return test::finish("04m_statfs");
	}
	const uint64_t path = path_page.value().value();
	const uint64_t buffer = stat_page.value().value();
	const uint64_t pair = pair_page.value().value();

	auto setPath = [&](const char *value) {
		auto written = instance.writeMemory(GuestVA{ path }, value, std::strlen(value) + 1);
		CHECK_RESULT(written);
	};
	auto statfsPath = [&](const char *name) -> int64_t {
		setPath(name);
		return static_cast<int64_t>(callGuest(instance, statfs_fn, { path, buffer }));
	};

	// /data: the profile's filesystem type and byte counts.
	CHECK_EQ(statfsPath("/data"), 0);
	std::printf("  statfs(/data) type=0x%llx blocks=%llu bavail=%llu\n",
		static_cast<unsigned long long>(readU64(instance, buffer + kStatfsTypeOffset)),
		static_cast<unsigned long long>(readU64(instance, buffer + kStatfsBlocksOffset)),
		static_cast<unsigned long long>(readU64(instance, buffer + kStatfsBlocksAvailableOffset)));
	CHECK_EQ(readU64(instance, buffer + kStatfsTypeOffset), kExt4Magic);
	CHECK_EQ(readU64(instance, buffer + kStatfsBlockSizeOffset), kBlockSize);
	CHECK_EQ(readU64(instance, buffer + kStatfsBlocksOffset), kDataTotal / kBlockSize);
	CHECK_EQ(readU64(instance, buffer + kStatfsBlocksAvailableOffset), kDataFree / kBlockSize);

	// System images are read-only erofs.
	CHECK_EQ(statfsPath("/system"), 0);
	CHECK_EQ(readU64(instance, buffer + kStatfsTypeOffset), kErofsMagic);
	CHECK_EQ(readU64(instance, buffer + kStatfsFlagsOffset) & 1, 1);

	CHECK_EQ(statfsPath("/proc"), 0);
	CHECK_EQ(readU64(instance, buffer + kStatfsTypeOffset), kProcMagic);

	// fstatfs follows the described object.
	{
		setPath("/dev/null");
		const int64_t fd = static_cast<int64_t>(callGuest(instance, open_fn,
			{ path, static_cast<uint64_t>(kORdOnly), 0 }));
		CHECK(fd >= 3);
		if (fd >= 3)
		{
			CHECK_EQ(callGuest(instance, fstatfs_fn,
					 { static_cast<uint64_t>(fd), buffer }),
				0);
			CHECK_EQ(readU64(instance, buffer + kStatfsTypeOffset), kTmpfsMagic);
			callGuest(instance, close_fn, { static_cast<uint64_t>(fd) });
		}
	}

	// Pipes report pipefs.
	{
		CHECK_EQ(callGuest(instance, pipe2_fn, { pair, 0 }), 0);
		uint32_t read_end = 0;
		uint32_t write_end = 0;
		CHECK_RESULT(instance.readMemory(GuestVA{ pair }, &read_end, sizeof(read_end)));
		CHECK_RESULT(instance.readMemory(GuestVA{ pair + 4 }, &write_end, sizeof(write_end)));
		CHECK(read_end >= 3);
		CHECK(write_end >= 3);
		if (read_end >= 3 && write_end >= 3)
		{
			CHECK_EQ(callGuest(instance, fstatfs_fn,
					 { static_cast<uint64_t>(read_end), buffer }),
				0);
			CHECK_EQ(readU64(instance, buffer + kStatfsTypeOffset), kPipefsMagic);
			callGuest(instance, close_fn, { static_cast<uint64_t>(read_end) });
			callGuest(instance, close_fn, { static_cast<uint64_t>(write_end) });
		}
	}

	return test::finish("04m_statfs");
}
