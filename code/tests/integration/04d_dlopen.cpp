// SPDX-License-Identifier: GPL-2.0-only
// Dynamic linking: libc's dlopen()/dlsym()/dlerror() forward to the virtual
// ld-android.so entry points, so a module that loads a sub-library at runtime
// must work end to end. The plugin's constructor runs inside the dlopen stub,
// through the nested guest-call path (the caller already holds the engine).

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/instance/runtime_instance.hpp"
#include "runtime/observer/event_bus.hpp"
#include "runtime/observer/semantic/module_event.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

namespace {

std::string soPath(std::string_view name)
{
	return std::string(DUCKEMU_SO_DIR) + "/" + std::string(name);
}

size_t countLoads(const std::vector<observer::ModuleEvent> &events, std::string_view soname)
{
	size_t count = 0;
	for (const auto &event : events)
	{
		if (event.kind == observer::ModuleEventKind::Load && event.soname == soname)
		{
			++count;
		}
	}
	return count;
}

uint64_t call(RuntimeInstance &instance, GuestVA entry)
{
	auto result = instance.callFunction(entry);
	CHECK_RESULT(result);
	return result.ok() ? result.value().return_value : 0xDEAD;
}

} // namespace

int main()
{
	RuntimeConfig config;
	config.pid = 1000;
	config.tid = 1;
	// dlopen("libplug.so") resolves through the same search path DT_NEEDED
	// names use.
	config.library_search_paths = { DUCKEMU_SO_DIR };

	auto created = RuntimeInstance::create(config);
	CHECK_RESULT(created);
	if (!created.ok())
	{
		return test::finish("04d_dlopen");
	}
	RuntimeInstance &instance = *created.value();

	std::vector<observer::ModuleEvent> module_events;
	instance.addModuleListener(
		[&module_events](const observer::ModuleEvent &event) { module_events.push_back(event); });

	auto loaded = instance.loadLibrary(soPath("libdlcaller.so"));
	CHECK_RESULT(loaded);
	if (!loaded.ok())
	{
		return test::finish("04d_dlopen");
	}
	const uint64_t handle = loaded.value();
	CHECK_EQ(countLoads(module_events, "libplug.so"), 0u);

	auto load_plugin = instance.lookupSymbol(handle, "load_plugin");
	auto call_plugin_value = instance.lookupSymbol(handle, "call_plugin_value");
	auto lookup_missing = instance.lookupSymbol(handle, "lookup_missing");
	auto open_missing = instance.lookupSymbol(handle, "open_missing");
	auto reload = instance.lookupSymbol(handle, "reload_same_handle");
	CHECK_RESULT(load_plugin);
	CHECK_RESULT(call_plugin_value);
	CHECK_RESULT(lookup_missing);
	CHECK_RESULT(open_missing);
	CHECK_RESULT(reload);
	if (!load_plugin.ok() || !call_plugin_value.ok() || !lookup_missing.ok() || !open_missing.ok() ||
	    !reload.ok())
	{
		return test::finish("04d_dlopen");
	}

	// dlopen from guest code: one new module, handle non-null.
	CHECK_EQ(call(instance, load_plugin.value()), 1u);
	CHECK_EQ(countLoads(module_events, "libplug.so"), 1u);

	// The constructor ran before dlopen returned (through the nested call),
	// and the plugin's frame returned normally.
	CHECK_EQ(call(instance, call_plugin_value.value()), 42u);
	CHECK_EQ(call(instance, call_plugin_value.value()), 42u);

	// Reopening the already loaded soname is idempotent and dlclose is a
	// no-op for the module's lifetime.
	CHECK_EQ(call(instance, reload.value()), 1u);
	CHECK_EQ(countLoads(module_events, "libplug.so"), 1u);
	CHECK_EQ(call(instance, call_plugin_value.value()), 42u);

	// Failures report through NULL plus a one-shot dlerror message.
	CHECK_EQ(call(instance, lookup_missing.value()), 1u);
	CHECK_EQ(call(instance, open_missing.value()), 1u);

	// Nothing leaked into the module table: still just the caller + plugin
	// (plus the virtual ld-android.so module).
	size_t real_modules = 0;
	for (const auto &event : module_events)
	{
		if (event.kind == observer::ModuleEventKind::Load && event.detail != "virtual")
		{
			++real_modules;
		}
	}
	CHECK_EQ(real_modules, 2u);

	return test::finish("04d_dlopen");
}
