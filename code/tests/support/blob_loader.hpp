// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "common/error/result.hpp"

namespace duckemu::test {

// Loads a flat guest blob produced by tests/CMakeLists.txt (clang + objcopy).
Result<std::vector<uint8_t>> loadBlob(std::string_view name);

} // namespace duckemu::test
