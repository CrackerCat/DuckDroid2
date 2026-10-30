// SPDX-License-Identifier: GPL-2.0-only
// /dev character nodes: the four that libc and crypto libraries open by path.
// Everything goes through the ROM libc's own open/read/write/fstat/stat/access,
// so the VFS (including st_rdev) is exercised exactly as a target sees it.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/instance/runtime_instance.hpp"
#include "runtime/memory/region.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

namespace
{

constexpr uint32_t kPid = 1000;
constexpr uint32_t kTid = 1;
constexpr uint64_t kDataSize = 0x1000;

constexpr size_t kStatModeOffset = 16;
constexpr size_t kStatUidOffset = 24;
constexpr size_t kStatGidOffset = 28;
constexpr size_t kStatRdevOffset = 32;

constexpr uint32_t kSIfmt = 0170000;
constexpr uint32_t kSIfchr = 0020000;
constexpr uint32_t kSIfdir = 0040000;

constexpr int kORdOnly = 0;
constexpr int kORdwr = 2;
constexpr int kFok = 0;
constexpr int kRok = 4;
constexpr int kXok = 1;

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

uint32_t readU32(RuntimeInstance &instance, uint64_t address)
{
	uint32_t value = 0;
	auto read = instance.readMemory(GuestVA{ address }, &value, sizeof(value));
	CHECK_RESULT(read);
	return value;
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

	auto created = RuntimeInstance::create(config);
	CHECK_RESULT(created);
	if (!created.ok())
	{
		return test::finish("04j_dev_nodes");
	}
	RuntimeInstance &instance = *created.value();

	auto libc = instance.loadLibrary(romLibc());
	CHECK_RESULT(libc);
	if (!libc.ok())
	{
		std::printf("  libc error: %s\n", libc.error().message.c_str());
		return test::finish("04j_dev_nodes");
	}
	const uint64_t handle = libc.value();

	auto lookup = [&](const char *name) -> GuestVA {
		auto found = instance.lookupSymbol(handle, name);
		CHECK_RESULT(found);
		return found.ok() ? found.value() : GuestVA{};
	};
	const GuestVA open_fn = lookup("open");
	const GuestVA read_fn = lookup("read");
	const GuestVA write_fn = lookup("write");
	const GuestVA close_fn = lookup("close");
	const GuestVA fstat_fn = lookup("fstat");
	const GuestVA stat_fn = lookup("stat");
	const GuestVA access_fn = lookup("access");
	const GuestVA lseek_fn = lookup("lseek");
	const GuestVA getrandom_fn = lookup("getrandom");

	auto path_page = instance.mapMemory(0x1000, PermReadWrite, "test-dev-path");
	auto data_page = instance.mapMemory(kDataSize, PermReadWrite, "test-dev-data");
	auto stat_page = instance.mapMemory(0x1000, PermReadWrite, "test-dev-stat");
	CHECK_RESULT(path_page);
	CHECK_RESULT(data_page);
	CHECK_RESULT(stat_page);
	if (!path_page.ok() || !data_page.ok() || !stat_page.ok())
	{
		return test::finish("04j_dev_nodes");
	}
	const uint64_t path = path_page.value().value();
	const uint64_t data = data_page.value().value();
	const uint64_t stat_buffer = stat_page.value().value();

	auto setPath = [&](const char *value) {
		auto written = instance.writeMemory(GuestVA{ path }, value, std::strlen(value) + 1);
		CHECK_RESULT(written);
	};
	auto openGuest = [&](const char *path_name, int flags) -> int64_t {
		setPath(path_name);
		return static_cast<int64_t>(callGuest(instance, open_fn,
			{ path, static_cast<uint64_t>(flags), 0 }));
	};
	auto readGuest = [&](int64_t fd, uint64_t count) -> int64_t {
		return static_cast<int64_t>(callGuest(instance, read_fn,
			{ static_cast<uint64_t>(fd), data, count }));
	};
	auto writeGuest = [&](int64_t fd, const char *bytes) -> int64_t {
		const size_t length = std::strlen(bytes);
		CHECK_RESULT(instance.writeMemory(GuestVA{ data }, bytes, length));
		return static_cast<int64_t>(callGuest(instance, write_fn,
			{ static_cast<uint64_t>(fd), data, length }));
	};
	auto fstatGuest = [&](int64_t fd) {
		return static_cast<int64_t>(callGuest(instance, fstat_fn,
			{ static_cast<uint64_t>(fd), stat_buffer }));
	};
	auto closeGuest = [&](int64_t fd) {
		return static_cast<int64_t>(callGuest(instance, close_fn, { static_cast<uint64_t>(fd) }));
	};

	// /dev/null: reads EOF, swallows writes, is a root-owned char device 1:3.
	{
		const int64_t fd = openGuest("/dev/null", kORdwr);
		CHECK(fd >= 3);
		std::printf("  open(/dev/null, O_RDWR) = %lld\n", static_cast<long long>(fd));
		if (fd >= 3)
		{
			CHECK_EQ(readGuest(fd, 32), 0);
			CHECK_EQ(writeGuest(fd, "abcdefgh"), 8);
			CHECK_EQ(callGuest(instance, lseek_fn, { static_cast<uint64_t>(fd), 0, 0 }), 0);
			CHECK_EQ(fstatGuest(fd), 0);
			const uint32_t mode = readU32(instance, stat_buffer + kStatModeOffset);
			const uint64_t rdev = readU64(instance, stat_buffer + kStatRdevOffset);
			std::printf("  /dev/null fstat mode=0%o rdev=0x%llx\n", mode & 07777,
				static_cast<unsigned long long>(rdev));
			CHECK_EQ(mode & kSIfmt, kSIfchr);
			CHECK_EQ(rdev, 0x103u);
			CHECK_EQ(readU32(instance, stat_buffer + kStatUidOffset), 0);
			CHECK_EQ(readU32(instance, stat_buffer + kStatGidOffset), 0);
			closeGuest(fd);
		}
	}

	// /dev/zero: every read returns the requested number of zero bytes.
	{
		const int64_t fd = openGuest("/dev/zero", kORdOnly);
		CHECK(fd >= 3);
		if (fd >= 3)
		{
			// Poison the buffer first so the zeros cannot be the initial map.
			const std::vector<uint8_t> poison(64, 0xa5);
			CHECK_RESULT(instance.writeMemory(GuestVA{ data }, poison.data(), poison.size()));
			CHECK_EQ(readGuest(fd, 64), 64);
			std::vector<uint8_t> bytes(64, 0);
			CHECK_RESULT(instance.readMemory(GuestVA{ data }, bytes.data(), bytes.size()));
			bool all_zero = true;
			for (uint8_t byte : bytes)
			{
				all_zero = all_zero && byte == 0;
			}
			CHECK(all_zero);
			// The description was opened O_RDONLY: a write is EBADF.
			CHECK_EQ(writeGuest(fd, "discarded"), -1);
			CHECK_EQ(fstatGuest(fd), 0);
			CHECK_EQ(readU64(instance, stat_buffer + kStatRdevOffset), 0x105u);
			closeGuest(fd);

			const int64_t rw = openGuest("/dev/zero", kORdwr);
			CHECK(rw >= 3);
			if (rw >= 3)
			{
				CHECK_EQ(writeGuest(rw, "discarded"), 9);
				closeGuest(rw);
			}
		}
	}

	// /dev/urandom and /dev/random: real bytes, from the instance stream.
	{
		const int64_t urandom_fd = openGuest("/dev/urandom", kORdOnly);
		CHECK(urandom_fd >= 3);
		if (urandom_fd >= 3)
		{
			CHECK_EQ(readGuest(urandom_fd, 64), 64);
			std::vector<uint8_t> first(64, 0);
			CHECK_RESULT(instance.readMemory(GuestVA{ data }, first.data(), first.size()));
			CHECK_EQ(readGuest(urandom_fd, 64), 64);
			std::vector<uint8_t> second(64, 0);
			CHECK_RESULT(instance.readMemory(GuestVA{ data }, second.data(), second.size()));
			const bool all_zero = std::all_of(first.begin(), first.end(),
				[](uint8_t byte) { return byte == 0; });
			CHECK(!all_zero);
			CHECK(first != second);
			CHECK_EQ(fstatGuest(urandom_fd), 0);
			CHECK_EQ(readU64(instance, stat_buffer + kStatRdevOffset), 0x109u);
			closeGuest(urandom_fd);
		}

		const int64_t random_fd = openGuest("/dev/random", kORdOnly);
		CHECK(random_fd >= 3);
		if (random_fd >= 3)
		{
			CHECK_EQ(readGuest(random_fd, 32), 32);
			closeGuest(random_fd);
		}

		// getrandom(2) still works and shares the stream's quality.
		CHECK_EQ(callGuest(instance, getrandom_fn, { data, 32, 0 }), 32);
	}

	// Metadata and access(2) for the nodes and the /dev directory.
	{
		setPath("/dev");
		CHECK_EQ(callGuest(instance, stat_fn, { path, stat_buffer }), 0);
		CHECK_EQ(readU32(instance, stat_buffer + kStatModeOffset) & kSIfmt, kSIfdir);

		setPath("/dev/null");
		CHECK_EQ(callGuest(instance, stat_fn, { path, stat_buffer }), 0);
		CHECK_EQ(readU32(instance, stat_buffer + kStatModeOffset) & kSIfmt, kSIfchr);
		CHECK_EQ(readU64(instance, stat_buffer + kStatRdevOffset), 0x103u);

		CHECK_EQ(callGuest(instance, access_fn, { path, kRok }), 0);
		CHECK_EQ(callGuest(instance, access_fn, { path, kFok }), 0);
		// 0444 property data stays read-only; 0666 device nodes accept writes.
		CHECK_EQ(callGuest(instance, access_fn, { path, kXok }), -1);
		setPath("/dev/urandom");
		CHECK_EQ(callGuest(instance, access_fn, { path, kRok }), 0);
	}

	return test::finish("04j_dev_nodes");
}
