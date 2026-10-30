// SPDX-License-Identifier: GPL-2.0-only
// M8 gate: instruction trace across guest threads.
//
// 03_pthread_join.S hand-writes clone/futex/exit. Tracing its whole region
// exercises:
//   - a second engine created mid-run receives the trace hooks;
//   - the global line counter stays ordered while threads interleave;
//   - threads.tsv attributes lines to more than one tid;
//   - SVC lines carry syscall notes.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
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

} // namespace

int main() {
    auto blob = test::loadBlob("03_pthread_join.bin");
    CHECK_RESULT(blob);
    if (!blob.ok()) {
        return test::finish("08_insn_trace_threads");
    }

    RuntimeConfig config;
    config.tid = 1;
    auto created = RuntimeInstance::create(config);
    CHECK_RESULT(created);
    if (!created.ok()) {
        return test::finish("08_insn_trace_threads");
    }
    RuntimeInstance& guest = *created.value();

    auto code = guest.mapMemory(blob.value().size(), PermRead | PermExec, "join-code");
    auto output = guest.mapMemory(0x1000, PermReadWrite, "join-out");
    CHECK_RESULT(code);
    CHECK_RESULT(output);
    if (!code.ok() || !output.ok()) {
        return test::finish("08_insn_trace_threads");
    }
    CHECK_RESULT(guest.writeMemory(code.value(), blob.value().data(), blob.value().size()));

    const std::string directory = std::string(DUCKEMU_TRACE_DIR) + "/08_insn_trace_threads";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    CHECK(!error);

    observer::InsnTraceOptions options;
    options.directory = directory;
    options.begin = code.value();
    options.size = blob.value().size();
    CHECK_RESULT(guest.startInsnTrace(options));

    std::array<uint64_t, 8> args{};
    args[0] = output.value().value();
    auto call = guest.callFunction(code.value(), args);
    CHECK_RESULT(call);
    if (call.ok()) {
        CHECK_EQ(call.value().return_value, 0);
    }
    guest.stopInsnTrace();

    const std::vector<std::string> code_lines = splitLines(readFile(directory + "/code.log"));
    CHECK(!code_lines.empty());

    uint64_t expected_line = 0;
    bool saw_clone = false;
    for (const auto& line : code_lines) {
        const auto fields = splitTabs(line);
        CHECK_EQ(fields.size(), 7);
        if (fields.size() != 7) {
            continue;
        }
        CHECK_EQ(parseHex(fields[0]), expected_line++);
        const uint64_t address = parseHex(fields[1]);
        CHECK(address >= code.value().value());
        CHECK(address < code.value().value() + blob.value().size());
        if (contains(fields[6], "\"syscall\":\"clone\"")) {
            saw_clone = true;
        }
    }
    CHECK(saw_clone);

    const std::vector<std::string> threads = splitLines(readFile(directory + "/threads.tsv"));
    std::set<uint32_t> tids;
    for (const auto& line : threads) {
        const auto fields = splitTabs(line);
        CHECK_EQ(fields.size(), 2);
        if (fields.size() == 2) {
            const uint64_t tid = parseHex(fields[1]);
            CHECK(tid <= UINT32_MAX);
            tids.insert(static_cast<uint32_t>(tid));
        }
    }
    CHECK(tids.size() >= 2);
    CHECK(tids.count(1) == 1);

    CHECK(!readFile(directory + "/rw.log").empty());

    return test::finish("08_insn_trace_threads");
}
