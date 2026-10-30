// SPDX-License-Identifier: GPL-2.0-only
// Seed injection: getrandom(2), /dev/urandom and AT_RANDOM all read one
// instance stream, so two instances with the same seed observe the same bytes
// and a different seed diverges.

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
constexpr uint64_t kSampleBytes = 16;
constexpr int kORdOnly = 0;
constexpr uint64_t kAtRandom = 25;

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

struct Sample
{
	std::vector<uint8_t> urandom;
	std::vector<uint8_t> getrandom;
	std::vector<uint8_t> at_random;
};

// One instance, one fixed read order: AT_RANDOM is consumed during create(),
// then /dev/urandom, then getrandom.
bool sample(uint64_t seed, Sample *result)
{
	RuntimeConfig config;
	config.pid = kPid;
	config.tid = kTid;
	config.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };
	config.random_seed = seed;

	auto created = RuntimeInstance::create(config);
	CHECK_RESULT(created);
	if (!created.ok())
	{
		return false;
	}
	RuntimeInstance &instance = *created.value();

	auto libc = instance.loadLibrary(romLibc());
	CHECK_RESULT(libc);
	if (!libc.ok())
	{
		return false;
	}
	const uint64_t handle = libc.value();

	auto lookup = [&](const char *name) -> GuestVA {
		auto found = instance.lookupSymbol(handle, name);
		CHECK_RESULT(found);
		return found.ok() ? found.value() : GuestVA{};
	};
	const GuestVA open_fn = lookup("open");
	const GuestVA read_fn = lookup("read");
	const GuestVA close_fn = lookup("close");
	const GuestVA getrandom_fn = lookup("getrandom");

	auto path_page = instance.mapMemory(0x1000, PermReadWrite, "test-random-path");
	auto data_page = instance.mapMemory(0x1000, PermReadWrite, "test-random-data");
	CHECK_RESULT(path_page);
	CHECK_RESULT(data_page);
	if (!path_page.ok() || !data_page.ok())
	{
		return false;
	}
	const uint64_t path = path_page.value().value();
	const uint64_t data = data_page.value().value();

	result->urandom.assign(kSampleBytes, 0);
	result->getrandom.assign(kSampleBytes, 0);
	result->at_random.assign(kSampleBytes, 0);

	const char urandom_path[] = "/dev/urandom";
	CHECK_RESULT(instance.writeMemory(GuestVA{ path }, urandom_path, sizeof(urandom_path)));
	const int64_t fd = static_cast<int64_t>(callGuest(instance, open_fn,
		{ path, static_cast<uint64_t>(kORdOnly), 0 }));
	CHECK(fd >= 3);
	if (fd < 3)
	{
		return false;
	}
	const int64_t first = static_cast<int64_t>(callGuest(instance, read_fn,
		{ static_cast<uint64_t>(fd), data, kSampleBytes }));
	CHECK_EQ(first, kSampleBytes);
	CHECK_RESULT(instance.readMemory(GuestVA{ data }, result->urandom.data(), kSampleBytes));
	callGuest(instance, close_fn, { static_cast<uint64_t>(fd) });

	CHECK_EQ(callGuest(instance, getrandom_fn, { data, kSampleBytes, 0 }), kSampleBytes);
	CHECK_RESULT(instance.readMemory(GuestVA{ data }, result->getrandom.data(), kSampleBytes));

	// auxv is pairs of uint64; AT_RANDOM's value is the guest address of the
	// 16 bytes also used as the stack canary seed.
	const char auxv_path[] = "/proc/self/auxv";
	CHECK_RESULT(instance.writeMemory(GuestVA{ path }, auxv_path, sizeof(auxv_path)));
	const int64_t auxv_fd = static_cast<int64_t>(callGuest(instance, open_fn,
		{ path, static_cast<uint64_t>(kORdOnly), 0 }));
	CHECK(auxv_fd >= 3);
	if (auxv_fd >= 3)
	{
		const int64_t length = static_cast<int64_t>(callGuest(instance, read_fn,
			{ static_cast<uint64_t>(auxv_fd), data, 512 }));
		CHECK(length > 0);
		std::vector<uint8_t> auxv(length > 0 ? static_cast<size_t>(length) : 0, 0);
		CHECK_RESULT(instance.readMemory(GuestVA{ data }, auxv.data(), auxv.size()));
		uint64_t random_address = 0;
		for (size_t offset = 0; offset + 16 <= auxv.size(); offset += 16)
		{
			uint64_t type = 0;
			uint64_t value = 0;
			std::memcpy(&type, auxv.data() + offset, sizeof(type));
			std::memcpy(&value, auxv.data() + offset + 8, sizeof(value));
			if (type == 0)
			{
				break;
			}
			if (type == kAtRandom)
			{
				random_address = value;
			}
		}
		CHECK(random_address != 0);
		if (random_address != 0)
		{
			CHECK_RESULT(instance.readMemory(GuestVA{ random_address },
				result->at_random.data(), kSampleBytes));
		}
		callGuest(instance, close_fn, { static_cast<uint64_t>(auxv_fd) });
	}
	return true;
}

} // namespace

int main()
{
	Sample first;
	Sample second;
	Sample different;
	const bool sampled = sample(0x1122334455667788ULL, &first) &&
		sample(0x1122334455667788ULL, &second) && sample(1, &different);
	CHECK(sampled);
	if (!sampled)
	{
		return test::finish("04l_random_seed");
	}

	std::printf("  urandom   = %02x%02x%02x%02x...\n", first.urandom[0], first.urandom[1],
		first.urandom[2], first.urandom[3]);
	CHECK(first.urandom == second.urandom);
	CHECK(first.getrandom == second.getrandom);
	CHECK(first.at_random == second.at_random);
	CHECK(first.urandom != different.urandom);

	// The three surfaces draw from one stream, so one read cannot repeat.
	const bool surfaces_differ = first.urandom != first.getrandom;
	CHECK(surfaces_differ);

	return test::finish("04l_random_seed");
}
