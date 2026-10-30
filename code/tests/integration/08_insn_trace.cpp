// SPDX-License-Identifier: GPL-2.0-only
// M8 gate: instruction-level trace over one region.
//
// trace_main calls trace_ext (mapped in its own region). The trace range is
// trace_main only, so:
//   - code.log has one line per trace_main instruction, no trace_ext lines;
//   - bl.log has the blr target;
//   - rw.log is empty (the fixture touches no data memory);
//   - threads.tsv records the main tid.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/instance/runtime_instance.hpp"
#include "runtime/memory/region.hpp"
#include "support/blob_loader.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

namespace {

std::string readFile(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        CHECK(false);
        return {};
    }
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    return lines;
}

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> fields;
    size_t start = 0;
    while (true) {
        const size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            fields.push_back(line.substr(start));
            return fields;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
}

uint64_t parseHex(std::string_view text) {
    const std::string value(text);
    return std::strtoull(value.c_str(), nullptr, 16);
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

Result<GuestVA> loadBlob(RuntimeInstance& guest, std::string_view name,
                         std::string_view region_name, std::vector<uint8_t>& bytes) {
    auto blob = test::loadBlob(name);
    if (!blob.ok()) {
        return blob.error();
    }
    bytes = std::move(blob).value();
    auto region = guest.mapMemory(bytes.size(), PermRead | PermExec, region_name);
    if (!region.ok()) {
        return region.error();
    }
    auto written = guest.writeMemory(region.value(), bytes.data(), bytes.size());
    if (!written.ok()) {
        return written.error();
    }
    return region.value();
}

} // namespace

int main() {
    RuntimeConfig config;
    config.tid = 1;
    auto created = RuntimeInstance::create(config);
    CHECK_RESULT(created);
    if (!created.ok()) {
        return test::finish("08_insn_trace");
    }
    RuntimeInstance& guest = *created.value();

    std::vector<uint8_t> main_bytes;
    std::vector<uint8_t> ext_bytes;
    auto main_region = loadBlob(guest, "08_trace_main.bin", "trace-main", main_bytes);
    auto ext_region = loadBlob(guest, "08_trace_ext.bin", "trace-ext", ext_bytes);
    CHECK_RESULT(main_region);
    CHECK_RESULT(ext_region);
    if (!main_region.ok() || !ext_region.ok()) {
        return test::finish("08_insn_trace");
    }

    const std::string directory = std::string(DUCKEMU_TRACE_DIR) + "/08_insn_trace";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    CHECK(!error);

    observer::InsnTraceOptions options;
    options.directory = directory;
    options.begin = main_region.value();
    options.size = main_bytes.size();
    CHECK_RESULT(guest.startInsnTrace(options));
    CHECK(guest.insnTraceActive());

    std::array<uint64_t, 8> args{};
    args[0] = ext_region.value().value();
    auto call = guest.callFunction(main_region.value(), args);
    CHECK_RESULT(call);
    if (call.ok()) {
        CHECK_EQ(call.value().return_value, 8);
    }
    guest.stopInsnTrace();
    CHECK(!guest.insnTraceActive());

    const std::vector<std::string> code = splitLines(readFile(directory + "/code.log"));
    const std::vector<std::string> bl = splitLines(readFile(directory + "/bl.log"));
    const std::string rw = readFile(directory + "/rw.log");
    const std::vector<std::string> threads = splitLines(readFile(directory + "/threads.tsv"));

    CHECK_EQ(code.size(), 7);
    if (code.size() == 7) {
        for (size_t i = 0; i < code.size(); ++i) {
            const auto fields = splitTabs(code[i]);
            CHECK_EQ(fields.size(), 7);
            if (fields.size() != 7) {
                continue;
            }
            CHECK_EQ(parseHex(fields[0]), i);
            CHECK_EQ(parseHex(fields[1]), main_region.value().value() + i * 4);
            CHECK_EQ(parseHex(fields[2]), i * 4);
            // No trace_ext instruction may leak into code.log.
            CHECK(parseHex(fields[1]) != ext_region.value().value());
        }
    }

    // blr x8: reads X8, writes LR, and leaves the traced range.
    if (code.size() == 7) {
        const auto blr = splitTabs(code[3]);
        CHECK_EQ(parseHex(blr[1]), main_region.value().value() + 12);
        CHECK(contains(blr[3], "blr"));
        CHECK(contains(blr[4], "\"X8\""));
        CHECK(contains(blr[5], "\"LR\""));
    }

    // add x0, x0, #1 after trace_ext returned: PRE X0 is the callee's 7.
    if (code.size() == 7) {
        const auto add = splitTabs(code[4]);
        CHECK_EQ(parseHex(add[1]), main_region.value().value() + 16);
        CHECK(contains(add[3], "add"));
        CHECK(contains(add[4], "\"X0\":\"0x7\""));
        CHECK(contains(add[5], "\"X0\":\"0x8\""));
    }

    // bl.log: one index line for the out-of-range call, then its hexdump.
    CHECK(!bl.empty());
    if (!bl.empty()) {
        const auto fields = splitTabs(bl[0]);
        CHECK_EQ(fields.size(), 3);
        if (fields.size() == 3) {
            CHECK_EQ(parseHex(fields[0]), 3);
            CHECK_EQ(parseHex(fields[1]), ext_region.value().value());
            CHECK(fields[2].rfind("0x", 0) == 0);
        }
    }

    CHECK(rw.empty());
    CHECK_EQ(threads.size(), 1);
    if (threads.size() == 1) {
        const auto fields = splitTabs(threads[0]);
        CHECK_EQ(fields.size(), 2);
        if (fields.size() == 2) {
            CHECK_EQ(parseHex(fields[0]), 0);
            CHECK(contains(fields[1], "0x1"));
        }
    }

    // ------------------------------------------------------------------
    // A hook inside the traced range runs before the PRE sample, so the
    // injected value must appear in code.log's reads column.
    // ------------------------------------------------------------------
    const std::string hooked_directory = std::string(DUCKEMU_TRACE_DIR) + "/08_insn_trace_hooked";
    std::filesystem::create_directories(hooked_directory, error);
    options.directory = hooked_directory;
    options.begin = main_region.value();
    options.size = main_bytes.size();
    CHECK_RESULT(guest.startInsnTrace(options));

    int hook_hits = 0;
    auto hook = guest.addAddressHook(GuestVA{main_region.value().value() + 16},
                                     [&](observer::HookContext& ctx) {
        ++hook_hits;
        CHECK_EQ(ctx.hitCount(), 1);
        ctx.writeX(0, 0x20);
    });
    CHECK_RESULT(hook);
    if (hook.ok()) {
        auto hooked_call = guest.callFunction(main_region.value(), args);
        CHECK_RESULT(hooked_call);
        if (hooked_call.ok()) {
            CHECK_EQ(hooked_call.value().return_value, 0x21);
        }
        CHECK(guest.removeAddressHook(hook.value()));
    }
    guest.stopInsnTrace();
    CHECK_EQ(hook_hits, 1);

    const std::vector<std::string> hooked_code =
        splitLines(readFile(hooked_directory + "/code.log"));
    CHECK_EQ(hooked_code.size(), 7);
    if (hooked_code.size() == 7) {
        const auto add = splitTabs(hooked_code[4]);
        CHECK(contains(add[4], "\"X0\":\"0x20\""));
        CHECK(contains(add[5], "\"X0\":\"0x21\""));
    }

    // ------------------------------------------------------------------
    // rw.log: the hook fixture stores and loads 8 bytes at the data pointer.
    // ------------------------------------------------------------------
    std::vector<uint8_t> rw_bytes;
    auto rw_blob = loadBlob(guest, "08_hook.bin", "hook-blob", rw_bytes);
    auto data_region = guest.mapMemory(0x1000, PermReadWrite, "hook-data");
    CHECK_RESULT(rw_blob);
    CHECK_RESULT(data_region);
    if (rw_blob.ok() && data_region.ok()) {
        const std::string rw_directory = std::string(DUCKEMU_TRACE_DIR) + "/08_insn_trace_rw";
        std::filesystem::create_directories(rw_directory, error);
        options.directory = rw_directory;
        options.begin = rw_blob.value();
        options.size = rw_bytes.size();
        CHECK_RESULT(guest.startInsnTrace(options));

        const uint64_t data_address = data_region.value().value() + 0x100;
        std::array<uint64_t, 8> rw_args{};
        rw_args[0] = data_address;
        rw_args[1] = 2;
        auto rw_call = guest.callFunction(rw_blob.value(), rw_args);
        CHECK_RESULT(rw_call);
        if (rw_call.ok()) {
            CHECK_EQ(rw_call.value().return_value, 3);
        }
        guest.stopInsnTrace();

        const std::vector<std::string> rw_lines = splitLines(readFile(rw_directory + "/rw.log"));
        std::vector<std::vector<std::string>> indexes;
        for (const auto& line : rw_lines) {
            const auto fields = splitTabs(line);
            if (fields.size() == 3 && (fields[1] == "r" || fields[1] == "w")) {
                indexes.push_back(fields);
            }
        }
        CHECK_EQ(indexes.size(), 2);
        if (indexes.size() == 2) {
            // str is instruction 2, ldr is instruction 3.
            CHECK_EQ(parseHex(indexes[0][0]), 2);
            CHECK(indexes[0][1] == "w");
            CHECK_EQ(parseHex(indexes[0][2]), data_address);
            CHECK_EQ(parseHex(indexes[1][0]), 3);
            CHECK(indexes[1][1] == "r");
            CHECK_EQ(parseHex(indexes[1][2]), data_address);
        }
        // Two index lines + three hexdump lines each.
        CHECK_EQ(rw_lines.size(), 8);
    }

    return test::finish("08_insn_trace");
}
