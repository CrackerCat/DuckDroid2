// SPDX-License-Identifier: GPL-2.0-only
// TLS / DTV full semantics against the real ROM libc.
//
// Static half (new instance): a TLS module loaded in the startup graph, before
// linker_finalize_static_tls, gets a static slot. Bionic's pthread_create then
// copies its .tdata image from the guest TlsModules table into every new
// thread's block.
//
// Dynamic half (second instance): libc is loaded first, which finalizes static
// TLS; a TLS module loaded afterwards gets a DTV slot instead. The TLSDESC
// descriptor points at the dynamic resolver, and first access in each thread
// goes through libc's __tls_get_addr slow path. A second dynamic module loaded
// while a worker is parked bumps the generation, so the parked thread must
// grow its DTV on the next access.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#include "runtime/instance/runtime_instance.hpp"
#include "runtime/memory/region.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

namespace
{

constexpr uint32_t kPid = 1000;
constexpr uint32_t kTid = 1;

std::string soPath(std::string_view name)
{
	return std::string(DUCKEMU_SO_DIR) + "/" + std::string(name);
}

std::string staticPath(std::string_view name)
{
	return std::string(DUCKEMU_SO_STATIC_DIR) + "/" + std::string(name);
}

std::string romLibc()
{
	return std::string(DUCKEMU_ROM_BOOTSTRAP) + "/libc.so";
}

uint64_t call(RuntimeInstance &instance, GuestVA entry, const std::array<uint64_t, 8> &args = {})
{
	auto result = instance.callFunction(entry, args);
	CHECK_RESULT(result);
	return result.ok() ? result.value().return_value : 0;
}

GuestVA symbol(RuntimeInstance &instance, uint64_t handle, const char *name)
{
	auto found = instance.lookupSymbol(handle, name);
	CHECK_RESULT(found);
	return found.ok() ? found.value() : GuestVA{};
}

bool waitForState(RuntimeInstance &instance, GuestVA state_fn, uint64_t expected)
{
	for (int attempt = 0; attempt < 2000; ++attempt)
	{
		if (call(instance, state_fn) == expected)
		{
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return false;
}

void staticTlsScenario()
{
	RuntimeConfig config;
	config.pid = kPid;
	config.tid = kTid;
	// The fixture is addressed by absolute path; its DT_NEEDED libc.so has no
	// sibling in the static directory, so the ROM's real libc wins.
	config.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };

	auto created = RuntimeInstance::create(config);
	CHECK_RESULT(created);
	if (!created.ok())
	{
		return;
	}
	RuntimeInstance &instance = *created.value();

	auto fixture = instance.loadLibrary(staticPath("libtlsdtv_static.so"));
	CHECK_RESULT(fixture);
	if (!fixture.ok())
	{
		std::printf("  static fixture error: %s\n", fixture.error().message.c_str());
		return;
	}
	const uint64_t handle = fixture.value();

	// libc's __init_static_tls copied the module image into the main thread's
	// final block; a bionic-created thread gets its own copy.
	CHECK_EQ(call(instance, symbol(instance, handle, "dtv_get")), 7u);
	CHECK_EQ(call(instance, symbol(instance, handle, "dtv_thread_probe")), 7009u);
	CHECK_EQ(call(instance, symbol(instance, handle, "dtv_get")), 7u);

	CHECK_EQ(call(instance, symbol(instance, handle, "dtv_set"), { 11 }), 0u);
	CHECK_EQ(call(instance, symbol(instance, handle, "dtv_thread_probe")), 7009u);
	CHECK_EQ(call(instance, symbol(instance, handle, "dtv_get")), 11u);

	// dlsym(STT_TLS) returns the calling thread's copy.
	CHECK_EQ(call(instance, symbol(instance, handle, "dtv_dlsym_addr")), 0u);
	CHECK_EQ(call(instance, symbol(instance, handle, "dtv_dlsym_get")), 11u);
	CHECK_EQ(call(instance, symbol(instance, handle, "dtv_dlsym_set"), { 13 }), 0u);
	CHECK_EQ(call(instance, symbol(instance, handle, "dtv_get")), 13u);
}

void dynamicTlsScenario()
{
	RuntimeConfig config;
	config.pid = kPid;
	config.tid = kTid;
	config.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64, DUCKEMU_SO_DIR };

	auto created = RuntimeInstance::create(config);
	CHECK_RESULT(created);
	if (!created.ok())
	{
		return;
	}
	RuntimeInstance &instance = *created.value();

	// Loading libc finalizes static TLS before preinit runs.
	auto libc = instance.loadLibrary(romLibc());
	CHECK_RESULT(libc);
	if (!libc.ok())
	{
		std::printf("  libc error: %s\n", libc.error().message.c_str());
		return;
	}

	auto fixture = instance.loadLibrary(soPath("libtlsdtv.so"));
	CHECK_RESULT(fixture);
	if (!fixture.ok())
	{
		std::printf("  dynamic fixture error: %s\n", fixture.error().message.c_str());
		return;
	}
	const uint64_t handle = fixture.value();

	// First access in the main thread allocates its DTV block on demand.
	CHECK_EQ(call(instance, symbol(instance, handle, "dtv_get")), 7u);
	CHECK_EQ(call(instance, symbol(instance, handle, "dtv_set"), { 11 }), 0u);
	CHECK_EQ(call(instance, symbol(instance, handle, "dtv_get")), 11u);

	// A bionic-created thread starts from the module initializer and gets its
	// own dynamic block.
	CHECK_EQ(call(instance, symbol(instance, handle, "dtv_thread_probe")), 7009u);
	CHECK_EQ(call(instance, symbol(instance, handle, "dtv_get")), 11u);

	// dlsym(STT_TLS) resolves through this thread's DTV slot.
	CHECK_EQ(call(instance, symbol(instance, handle, "dtv_dlsym_addr")), 0u);
	CHECK_EQ(call(instance, symbol(instance, handle, "dtv_dlsym_get")), 11u);
	CHECK_EQ(call(instance, symbol(instance, handle, "dtv_dlsym_set"), { 13 }), 0u);
	CHECK_EQ(call(instance, symbol(instance, handle, "dtv_get")), 13u);
	CHECK_EQ(call(instance, symbol(instance, handle, "dtv_set"), { 11 }), 0u);

	// Park a worker, then load a second dynamic TLS module from the host. The
	// module registration bumps the generation; the worker's DTV is now stale.
	auto gate = instance.mapMemory(0x1000, PermReadWrite, "tls-dtv-gate");
	CHECK_RESULT(gate);
	if (!gate.ok())
	{
		return;
	}
	const uint32_t zero = 0;
	CHECK_RESULT(instance.writeMemory(gate.value(), &zero, sizeof(zero)));

	CHECK_EQ(call(instance, symbol(instance, handle, "start_gate_worker"), { gate.value().value() }), 0u);
	CHECK(waitForState(instance, symbol(instance, handle, "gate_worker_state"), 1));

	auto second = instance.loadLibrary(soPath("libtls.so"));
	CHECK_RESULT(second);
	if (!second.ok())
	{
		std::printf("  second TLS module error: %s\n", second.error().message.c_str());
		return;
	}

	CHECK_EQ(call(instance, symbol(instance, handle, "finish_gate_worker"), { gate.value().value() }), 0u);
	// The parked thread took the slow path and allocated the new module's
	// block lazily, seeing its initializer.
	CHECK(waitForState(instance, symbol(instance, handle, "gate_worker_state"), 2));
	CHECK_EQ(call(instance, symbol(instance, handle, "gate_worker_tls")), 7u);

	// Both dynamic modules are live with independent storage in the same
	// thread: libtlsdtv at 11, libtls at its initializer 7.
	CHECK_EQ(call(instance, symbol(instance, handle, "dtv_get")), 11u);
	CHECK_EQ(call(instance, symbol(instance, second.value(), "tls_get")), 7u);
}

} // namespace

int main()
{
	staticTlsScenario();
	dynamicTlsScenario();
	return test::finish("04n_tls_dtv");
}
