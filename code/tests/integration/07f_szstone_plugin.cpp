// SPDX-License-Identifier: GPL-2.0-only
// Worker seam for the dewu target: SzSdk.lf() behind worker::make_app_plugin.
//
// The gate asserts the shape the case can actually guarantee:
//   - the strict Java world stays complete (JniEvent count is 0);
//   - the first call returns a non-empty body of the device-picture length;
//   - a second instance with the same injected clock and random seed produces
//     byte-identical output (lf's nonce becomes reproducible).
//
// The body itself is not compared with assets/lf_out.bin: that capture came
// from a device whose nonce differs, and byte equality needs the APK mount.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "runtime/instance/runtime_instance.hpp"
#include "runtime/observer/semantic/jni_event.hpp"
#include "szstone_case/szstone_codec.hpp"
#include "support/test_harness.hpp"
#include "worker/app/app_plugin.hpp"

#ifndef DUCKEMU_ROM_BOOTSTRAP
#error "DUCKEMU_ROM_BOOTSTRAP must point at the ROM bootstrap lib directory"
#endif
#ifndef DUCKEMU_ROM_LIB64
#error "DUCKEMU_ROM_LIB64 must point at the ROM system/lib64 directory"
#endif
#ifndef DUCKEMU_SZSTONE_SO
#error "DUCKEMU_SZSTONE_SO must point at libszstone.so"
#endif
#ifndef DUCKEMU_SZSTONE_BLOB
#error "DUCKEMU_SZSTONE_BLOB must point at the captured profile blob"
#endif
#ifndef DUCKEMU_SZSTONE_ROOT
#error "DUCKEMU_SZSTONE_ROOT must point at the device snapshot root"
#endif
#ifndef DUCKEMU_SZSTONE_PROPERTIES
#error "DUCKEMU_SZSTONE_PROPERTIES must point at the device property picture"
#endif

using namespace duckemu;

namespace
{

// With a pinned wall/monotonic clock and random source lf is byte-reproducible.
// Monotonic time matters too: one lf path includes an elapsed-time field, and
// leaving it on the host clock makes two otherwise identical runs differ.
constexpr uint64_t kWallTimeNs = 1792886400000000000ULL;
constexpr uint64_t kMonotonicTimeNs = 1000000000ULL;
constexpr uint64_t kRandomSeed = 7;
constexpr const char *kExpectedBodyLen = "3183";

bool readFile(const std::string &path, std::vector<uint8_t> *out, std::string *error)
{
	std::ifstream file(path, std::ios::binary);
	if (!file)
	{
		*error = "cannot open " + path;
		return false;
	}
	out->assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	return true;
}

struct Outcome
{
	std::string body_hex;
	std::string body_len;
	uint64_t jni_events = 0;
};

// One fresh instance, one lf() call. A second call inside the same instance is
// deliberately not part of the comparison: lf keeps internal state.
bool runOnce(const worker::AppContext &context,
	const std::string &blob_hex,
	Outcome *outcome,
	std::string *error)
{
	runtime::RuntimeConfig config;
	config.library_search_paths = context.library_search_paths;
	config.filesystem_roots = context.filesystem_roots;
	config.property_file = context.property_file;
	config.random_seed = context.random_seed;
	config.default_timeout_us = 60ULL * 1000 * 1000;

	auto created = runtime::RuntimeInstance::create(config);
	if (!created.ok())
	{
		*error = "RuntimeInstance::create failed: " + created.error().message;
		return false;
	}
	runtime::RuntimeInstance &instance = *created.value();
	instance.setWallTimeNs(kWallTimeNs);
	instance.setMonotonicTimeNs(kMonotonicTimeNs);

	uint64_t events = 0;
	instance.addJniListener([&events](const runtime::observer::JniEvent &event) {
		++events;
		std::fprintf(stderr, "jni %s class=%s name=%s sig=%s slot=%u tid=%u\n",
			std::string(runtime::observer::jniEventKindName(event.kind)).c_str(),
			event.class_descriptor.c_str(),
			event.name.c_str(),
			event.signature.c_str(),
			event.slot,
			event.tid);
	});

	auto plugin = worker::make_app_plugin(context);
	if (!plugin.ok())
	{
		*error = "make_app_plugin failed: " + plugin.error().message;
		return false;
	}
	if (!plugin.value()->init(instance, error))
	{
		return false;
	}

	worker::AppRequest request;
	request.op = "lf";
	request.params["blob"] = blob_hex;
	// The call the device capture used: lf(blob, true, true, true).
	request.params["z"] = "1,1,1";
	worker::AppResult result;
	worker::AppError app_error;
	if (!plugin.value()->invoke(instance, request, &result, &app_error))
	{
		*error = "invoke failed: " + app_error.message;
		return false;
	}

	const auto body = result.values.find("body_hex");
	const auto length = result.values.find("body_len");
	if (body == result.values.end() || length == result.values.end())
	{
		*error = "result is missing body_hex/body_len";
		return false;
	}
	outcome->body_hex = body->second;
	outcome->body_len = length->second;
	outcome->jni_events = events;
	return true;
}

} // namespace

int main()
{
	worker::AppContext context;
	context.app = "szstone";
	context.library = DUCKEMU_SZSTONE_SO;
	context.rom_libc = std::string(DUCKEMU_ROM_BOOTSTRAP) + "/libc.so";
	context.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };
	context.filesystem_roots = { DUCKEMU_SZSTONE_ROOT };
	context.property_file = DUCKEMU_SZSTONE_PROPERTIES;
	context.random_seed = kRandomSeed;

	std::vector<uint8_t> blob;
	std::string error;
	if (!readFile(DUCKEMU_SZSTONE_BLOB, &blob, &error))
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return test::finish("07f_szstone_plugin");
	}
	const std::string blob_hex = user::szstone::encodeHex(blob);

	Outcome first;
	const bool ran_once = runOnce(context, blob_hex, &first, &error);
	CHECK(ran_once);
	if (!ran_once)
	{
		std::fprintf(stderr, "  %s\n", error.c_str());
		return test::finish("07f_szstone_plugin");
	}
	CHECK(!first.body_hex.empty());
	CHECK(first.body_len == kExpectedBodyLen);
	CHECK_EQ(first.jni_events, 0);

	// Same clock + seed in a fresh process image: lf must answer identically.
	Outcome second;
	const bool ran_twice = runOnce(context, blob_hex, &second, &error);
	CHECK(ran_twice);
	if (ran_twice)
	{
		CHECK(second.body_hex == first.body_hex);
		CHECK_EQ(second.jni_events, 0);
	}
	else
	{
		std::fprintf(stderr, "  %s\n", error.c_str());
	}

	return test::finish("07f_szstone_plugin");
}
