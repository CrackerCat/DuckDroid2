// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace duckemu::test {

// Path of a trace file under DUCKEMU_TRACE_DIR (defined by tests/CMakeLists).
// Creates the directory on first use so tests can run from any cwd.
inline std::string tracePath(std::string_view name) {
    std::error_code error;
    std::filesystem::create_directories(DUCKEMU_TRACE_DIR, error);
    return std::string(DUCKEMU_TRACE_DIR) + "/" + std::string(name);
}

} // namespace duckemu::test
