// SPDX-License-Identifier: GPL-2.0-only
// System properties through the real thing: the ROM libc's own
// __system_properties_init runs from its constructor (preinit) and reads the
// /dev/__properties__ images the runtime synthesizes. Every assertion below
// goes through libc's parser, so a wrong prop_area/property_info layout fails
// here rather than in a hand-rolled lookup.

#include <array>
#include <cstdint>
#include <cstdio>
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
constexpr uint64_t kLongValueSize = 100;

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

std::string readCString(RuntimeInstance &instance, uint64_t address, uint64_t max = 256)
{
	std::vector<char> buffer(max, 0);
	auto read = instance.readMemory(GuestVA{ address }, buffer.data(), max - 1);
	CHECK_RESULT(read);
	if (!read.ok())
	{
		return {};
	}
	return std::string(buffer.data());
}

} // namespace

int main()
{
	RuntimeConfig config;
	config.pid = kPid;
	config.tid = kTid;
	config.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };
	// Identity values the property file then overrides in part.
	config.device.model = "Mi8Test";
	config.device.brand = "TestBrand";
	config.device.device = "testdevice";
	config.device.fingerprint =
		"TestBrand/testdevice/testdevice:14/AP2A.240905.003/eng.test.1:userdebug/test-keys";
	config.device.sdk = 33;
	config.device.release = "13";
	config.property_file = DUCKEMU_PROPERTY_FIXTURE;

	auto created = RuntimeInstance::create(config);
	CHECK_RESULT(created);
	if (!created.ok())
	{
		return test::finish("04i_properties");
	}
	RuntimeInstance &instance = *created.value();

	auto libc = instance.loadLibrary(romLibc());
	CHECK_RESULT(libc);
	if (!libc.ok())
	{
		std::printf("  libc error: %s\n", libc.error().message.c_str());
		return test::finish("04i_properties");
	}
	const uint64_t handle = libc.value();

	auto lookup = [&](const char *name) -> GuestVA {
		auto found = instance.lookupSymbol(handle, name);
		CHECK_RESULT(found);
		return found.ok() ? found.value() : GuestVA{};
	};
	const GuestVA get_fn = lookup("__system_property_get");
	const GuestVA find_fn = lookup("__system_property_find");
	const GuestVA read_cb_fn = lookup("__system_property_read_callback");
	const GuestVA serial_fn = lookup("__system_property_area_serial");
	const GuestVA foreach_fn = lookup("__system_property_foreach");
	const GuestVA read_fn = lookup("__system_property_read");

	// A string page each for names/values; libc copies out, never in.
	auto name_page = instance.mapMemory(0x1000, PermReadWrite, "test-prop-name");
	auto value_page = instance.mapMemory(0x1000, PermReadWrite, "test-prop-value");
	CHECK_RESULT(name_page);
	CHECK_RESULT(value_page);
	if (!name_page.ok() || !value_page.ok())
	{
		return test::finish("04i_properties");
	}
	const uint64_t name_buffer = name_page.value().value();
	const uint64_t value_buffer = value_page.value().value();

	auto writeName = [&](const std::string &name) {
		auto written = instance.writeMemory(GuestVA{ name_buffer }, name.c_str(), name.size() + 1);
		CHECK_RESULT(written);
	};
	auto readValue = [&]() -> std::string {
		return readCString(instance, value_buffer, 256);
	};

	// preinit must have found a usable area: uninitialised answers 0xffffffff.
	auto area_serial = static_cast<uint32_t>(callGuest(instance, serial_fn, {}));
	std::printf("  property area serial = %u\n", area_serial);
	CHECK(area_serial != 0xffffffffu);

	struct Case
	{
		const char *name;
		const char *expected; // nullptr: property must be absent
	};

	const std::vector<Case> cases = {
		// Derived from DeviceProfile.
		{ "ro.product.model", "Mi8Test" },
		{ "ro.product.brand", "TestBrand" },
		{ "ro.product.device", "testdevice" },
		{ "ro.build.fingerprint",
			"TestBrand/testdevice/testdevice:14/AP2A.240905.003/eng.test.1:userdebug/test-keys" },
		{ "ro.build.id", "AP2A.240905.003" }, // parsed out of the fingerprint
		{ "ro.build.type", "user" },
		{ "ro.product.cpu.abi", "arm64-v8a" },
		// Overridden / extended by the property file.
		{ "ro.build.version.sdk", "34" },
		{ "ro.build.version.release", "14" },
		{ "ro.test.custom", "hello" },
		{ "ro.test.empty", "" },
		{ "debug.fdsan", nullptr },
	};

	for (const Case &pending : cases)
	{
		writeName(pending.name);
		auto written = instance.writeMemory(GuestVA{ value_buffer }, "\x7f", 1);
		CHECK_RESULT(written);
		const int64_t length = static_cast<int64_t>(
			callGuest(instance, get_fn, { name_buffer, value_buffer }));
		const std::string value = readValue();
		std::printf("  get(%s) = %lld \"%s\"\n", pending.name,
			static_cast<long long>(length), value.c_str());
		if (pending.expected == nullptr)
		{
			CHECK_EQ(length, 0);
			CHECK(value.empty());
		}
		else
		{
			CHECK_EQ(length, static_cast<int64_t>(std::string(pending.expected).size()));
			CHECK(value == pending.expected);
		}

		// __system_property_find agrees about existence.
		const uint64_t info = callGuest(instance, find_fn, { name_buffer });
		CHECK_EQ(info != 0, pending.expected != nullptr);
	}

	// Long values only decode through the callback form.
	struct CallbackResult
	{
		std::string name;
		std::string value;
		uint32_t serial = 0;
		bool called = false;
	} callback;

	auto callback_thunk = instance.registerHostFunction("property_read_callback",
		[&](HostCallArgs &args) -> int64_t {
			callback.called = true;
			callback.name = readCString(instance, args.x[1], 128);
			callback.value = readCString(instance, args.x[2], 256);
			callback.serial = static_cast<uint32_t>(args.x[3]);
			return 0;
		});
	CHECK_RESULT(callback_thunk);
	if (!callback_thunk.ok())
	{
		return test::finish("04i_properties");
	}

	std::string long_value(kLongValueSize, 'x');
	for (size_t index = 0; index < long_value.size(); ++index)
	{
		long_value[index] = static_cast<char>('a' + (index % 26));
	}

	for (const char *name : { "ro.test.long", "ro.build.version.sdk" })
	{
		writeName(name);
		const uint64_t info = callGuest(instance, find_fn, { name_buffer });
		CHECK(info != 0);
		if (info == 0)
		{
			continue;
		}
		callback = CallbackResult{};
		callGuest(instance, read_cb_fn, { info, callback_thunk.value().value(), 0 });
		CHECK(callback.called);
		std::printf("  read_callback(%s) = \"%s\" serial=%u\n", name,
			callback.value.c_str(), callback.serial);
		CHECK(callback.name == name);
		if (std::string(name) == "ro.test.long")
		{
			// Long properties carry the kLongFlag bit and the legacy error
			// message length in the serial; the value lives past the struct.
			CHECK((callback.serial & (1u << 16)) != 0);
			CHECK(callback.value == long_value);
		}
		else
		{
			CHECK_EQ(callback.serial >> 24, callback.value.size());
			CHECK(callback.value == "34");
		}
	}

	// __system_property_foreach walks the whole area; read each entry through
	// libc's __system_property_read so names and values come from prop_info.
	std::vector<std::string> seen_names;
	std::vector<std::string> seen_values;
	const uint64_t foreach_name = name_buffer;
	const uint64_t foreach_value = value_buffer;
	auto foreach_thunk = instance.registerHostFunction("property_foreach",
		[&](HostCallArgs &args) -> int64_t {
			const uint64_t info = args.x[0];
			if (!args.call_guest)
			{
				return 0;
			}
			// Re-enter the guest through the dispatcher: the instance API
			// refuses a second callFunction while one is running.
			auto read = args.call_guest(read_fn, { info, foreach_name, foreach_value });
			CHECK_RESULT(read);
			if (read.ok() && read.value() > 0)
			{
				seen_names.push_back(readCString(instance, foreach_name, 128));
				seen_values.push_back(readCString(instance, foreach_value, 256));
			}
			return 0;
		});
	CHECK_RESULT(foreach_thunk);
	if (foreach_thunk.ok())
	{
		callGuest(instance, foreach_fn, { foreach_thunk.value().value(), 0 });
		std::printf("  foreach saw %zu properties\n", seen_names.size());
		bool found_custom = false;
		bool found_model = false;
		for (size_t index = 0; index < seen_names.size(); ++index)
		{
			found_custom = found_custom || (seen_names[index] == "ro.test.custom" &&
							 seen_values[index] == "hello");
			found_model = found_model ||
				(seen_names[index] == "ro.product.model" &&
					seen_values[index] == "Mi8Test");
		}
		CHECK(found_custom);
		CHECK(found_model);
		CHECK(seen_names.size() >= cases.size() - 1);
	}

	return test::finish("04i_properties");
}
