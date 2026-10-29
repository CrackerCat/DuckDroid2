// SPDX-License-Identifier: GPL-2.0-only
#include "support/blob_loader.hpp"

#include <fstream>
#include <iterator>
#include <string>

#ifndef DUCKEMU_BLOB_DIR
#error "DUCKEMU_BLOB_DIR must point at the assembled guest blobs"
#endif

namespace duckemu::test {

Result<std::vector<uint8_t>> loadBlob(std::string_view name) {
    const std::string path = std::string(DUCKEMU_BLOB_DIR) + "/" + std::string(name);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Error{ErrorCode::NotFound, "cannot open blob " + path};
    }

    std::vector<uint8_t> data{std::istreambuf_iterator<char>(input),
                              std::istreambuf_iterator<char>()};
    if (data.empty()) {
        return Error{ErrorCode::InvalidArgument, "empty blob " + path};
    }
    return data;
}

} // namespace duckemu::test
