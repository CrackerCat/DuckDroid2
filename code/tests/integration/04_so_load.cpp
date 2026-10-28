// M4 gate: load a real ELF shared object with DT_NEEDED, apply relocations,
// resolve imports against the virtual bionic and call into the module.
//
// Fixture graph (code/tests/fixtures/so):
//   liba.so  NEEDED libb.so, liblog.so, libc.so
//   libb.so  leaf, one R_AARCH64_RELATIVE
//   liblog.so / libc.so exist on the search path as non-ELF garbage: the loader
//   must satisfy them from the virtual table without touching the filesystem.

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/instance/runtime_instance.hpp"
#include "runtime/observer/event_bus.hpp"
#include "runtime/observer/semantic/module_event.hpp"
#include "runtime/observer/trace/svc_trace.hpp"
#include "support/test_harness.hpp"
#include "support/trace_path.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

namespace {

constexpr uint32_t kPid = 1000;
constexpr uint32_t kTid = 1;

std::string soPath(std::string_view name) {
    return std::string(DUCKEMU_SO_DIR) + "/" + std::string(name);
}

uint32_t readU32(RuntimeInstance& instance, GuestVA address) {
    uint32_t value = 0;
    auto result = instance.readMemory(address, &value, sizeof(value));
    CHECK_RESULT(result);
    return value;
}

void printModuleEvents(const std::vector<observer::ModuleEvent>& events) {
    std::printf("  module events (%zu):\n", events.size());
    for (const auto& event : events) {
        std::printf("    %llu\t%s\t%s\t0x%llx\t%s\t%s\n",
                    static_cast<unsigned long long>(event.id),
                    std::string(observer::moduleEventKindName(event.kind)).c_str(),
                    event.soname.c_str(),
                    static_cast<unsigned long long>(event.base),
                    event.ok ? "ok" : "FAILED", event.detail.c_str());
    }
}

const observer::ModuleEvent* findEvent(
    const std::vector<observer::ModuleEvent>& events, observer::ModuleEventKind kind,
    std::string_view soname, std::string_view detail_contains) {
    for (const auto& event : events) {
        if (event.kind != kind || event.soname != soname) {
            continue;
        }
        if (!detail_contains.empty() &&
            event.detail.find(detail_contains) == std::string::npos) {
            continue;
        }
        return &event;
    }
    return nullptr;
}

size_t countLoads(const std::vector<observer::ModuleEvent>& events,
                  std::string_view soname) {
    size_t count = 0;
    for (const auto& event : events) {
        if (event.kind == observer::ModuleEventKind::Load && event.soname == soname) {
            ++count;
        }
    }
    return count;
}

std::vector<observer::SvcEvent> readTrace(const std::string& path) {
    std::ifstream input(path);
    std::vector<observer::SvcEvent> records;
    if (!input) {
        return records;
    }
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        auto record = observer::parseSvcTraceLine(line);
        if (record.has_value()) {
            records.push_back(std::move(record).value());
        }
    }
    return records;
}

size_t countHostStub(const std::vector<observer::SvcEvent>& records,
                     std::string_view name) {
    size_t count = 0;
    for (const auto& record : records) {
        if (record.kind == observer::SvcKind::HostStub && record.name == name) {
            ++count;
        }
    }
    return count;
}

} // namespace

int main() {
    RuntimeConfig config;
    config.pid = kPid;
    config.tid = kTid;
    config.trace_path = test::tracePath("04_so_load.log");
    config.library_search_paths = { DUCKEMU_SO_DIR };

    auto created = RuntimeInstance::create(config);
    CHECK_RESULT(created);
    if (!created.ok()) {
        return test::finish("04_so_load");
    }
    RuntimeInstance& instance = *created.value();

    std::vector<observer::ModuleEvent> module_events;
    instance.addModuleListener(
        [&module_events](const observer::ModuleEvent& event) { module_events.push_back(event); });

    // ------------------------------------------------------------------
    // Load the graph.
    // ------------------------------------------------------------------
    auto loaded = instance.loadLibrary(soPath("liba.so"));
    CHECK_RESULT(loaded);
    if (!loaded.ok()) {
        printModuleEvents(module_events);
        return test::finish("04_so_load");
    }
    const uint64_t handle = loaded.value();
    printModuleEvents(module_events);

    const auto* liba_load =
        findEvent(module_events, observer::ModuleEventKind::Load, "liba.so", "");
    const auto* libb_load =
        findEvent(module_events, observer::ModuleEventKind::Load, "libb.so", "");
    const auto* liblog_load =
        findEvent(module_events, observer::ModuleEventKind::Load, "liblog.so", "");
    const auto* libc_load =
        findEvent(module_events, observer::ModuleEventKind::Load, "libc.so", "");
    CHECK(liba_load != nullptr);
    CHECK(libb_load != nullptr);
    CHECK(liblog_load != nullptr);
    CHECK(libc_load != nullptr);

    if (liblog_load != nullptr) {
        CHECK_EQ(liblog_load->detail == "virtual", true);
    }
    if (libc_load != nullptr) {
        CHECK_EQ(libc_load->detail == "virtual", true);
    }
    if (liba_load != nullptr && libb_load != nullptr) {
        CHECK(liba_load->base != libb_load->base);
        CHECK_EQ(liba_load->base % 0x1000, 0);
        CHECK_EQ(libb_load->base % 0x1000, 0);
    }

    for (const std::string_view needed : { "libb.so", "liblog.so", "libc.so" }) {
        const auto* edge = findEvent(
            module_events, observer::ModuleEventKind::Needed, "liba.so", needed);
        CHECK(edge != nullptr);
        if (edge != nullptr) {
            CHECK(edge->ok);
        }
    }

    // Undefined functions must have been bound to thunks, in resolution order.
    for (const std::string_view symbol : { "pthread_once", "write", "malloc", "free",
                                           "__errno", "log_print" }) {
        const auto* resolved = findEvent(
            module_events, observer::ModuleEventKind::Resolve, "liba.so", symbol);
        CHECK(resolved != nullptr);
        if (resolved != nullptr) {
            CHECK(resolved->ok);
            CHECK(resolved->detail.find("virtual") != std::string::npos);
        }
    }

    // ------------------------------------------------------------------
    // Constructors ran at load time, in dependency order.
    // ------------------------------------------------------------------
    auto ctor_address = instance.lookupSymbol(handle, "g_ctor_ran");
    CHECK_RESULT(ctor_address);
    if (ctor_address.ok()) {
        // a_ctor() sets it to b_add(1) == 8.
        CHECK_EQ(readU32(instance, ctor_address.value()), 8u);
    }

    // ------------------------------------------------------------------
    // Call into the module. Twice, so pthread_once has to be idempotent.
    // ------------------------------------------------------------------
    auto entry = instance.lookupSymbol(handle, "a_entry");
    CHECK_RESULT(entry);
    if (!entry.ok()) {
        return test::finish("04_so_load");
    }

    auto first = instance.callFunction(entry.value(), { 5 });
    CHECK_RESULT(first);
    if (first.ok()) {
        // b_add(5) + g_once_ran == 12 + 1.
        CHECK_EQ(first.value().return_value, 13u);
    }

    auto second = instance.callFunction(entry.value(), { 7 });
    CHECK_RESULT(second);
    if (second.ok()) {
        CHECK_EQ(second.value().return_value, 15u);
    }

    auto once_address = instance.lookupSymbol(handle, "g_once_ran");
    CHECK_RESULT(once_address);
    if (once_address.ok()) {
        CHECK_EQ(readU32(instance, once_address.value()), 1u);
    }

    // ------------------------------------------------------------------
    // Trace: the imports went through the thunk page as host stubs.
    // ------------------------------------------------------------------
    const std::vector<observer::SvcEvent> trace = readTrace(config.trace_path);
    std::printf("  svc.log: %zu record(s)\n", trace.size());
    for (const auto& record : trace) {
        std::printf("    %s\n", observer::formatSvcTraceLine(record).c_str());
    }
    CHECK_EQ(countHostStub(trace, "write"), 2u);
    CHECK_EQ(countHostStub(trace, "pthread_once"), 2u);
    CHECK_EQ(countHostStub(trace, "malloc"), 2u);
    CHECK_EQ(countHostStub(trace, "free"), 2u);
    CHECK_EQ(countHostStub(trace, "log_print"), 2u);
    CHECK_EQ(countHostStub(trace, "__errno"), 2u);

    // ------------------------------------------------------------------
    // Loading the same module again is a no-op.
    // ------------------------------------------------------------------
    auto again = instance.loadLibrary(soPath("liba.so"));
    CHECK_RESULT(again);
    if (again.ok()) {
        CHECK_EQ(again.value(), handle);
    }
    CHECK_EQ(countLoads(module_events, "liba.so"), 1u);
    CHECK_EQ(countLoads(module_events, "libb.so"), 1u);

    // ------------------------------------------------------------------
    // An unresolvable object symbol fails the load and names the symbol.
    // ------------------------------------------------------------------
    auto missing = instance.loadLibrary(soPath("libmissing.so"));
    CHECK(!missing.ok());
    if (!missing.ok()) {
        std::printf("  libmissing error: %s\n", missing.error().message.c_str());
        CHECK(missing.error().message.find("nobody_provides_this_object") !=
              std::string::npos);
    }

    return test::finish("04_so_load");
}
