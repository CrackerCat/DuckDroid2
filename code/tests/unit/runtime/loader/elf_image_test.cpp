// SPDX-License-Identifier: GPL-2.0-only
// Unit: the ELF reader accepts a real fixture and rejects malformed input.
// The fixtures are the same ones the 04 gate loads (so-runtime view).

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "runtime/loader/elf/elf_image.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime::loader;

namespace {

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>());
}

bool hasSymbol(const ElfImage& image, std::string_view name) {
    for (const ElfSymbol& symbol : image.symbols()) {
        if (symbol.name == name && !symbol.undefined()) {
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    const std::string dir = DUCKEMU_SO_DIR;
    const std::vector<uint8_t> bytes = readFile(dir + "/liba.so");
    CHECK(!bytes.empty());

    auto image = ElfImage::parse(bytes, "liba.so");
    CHECK_RESULT(image);
    if (image.ok()) {
        const ElfImage& parsed = image.value();
        CHECK(parsed.soname() == "liba.so");
        CHECK(parsed.segments().size() >= 3);
        CHECK_EQ(parsed.needed().size(), 3u);
        CHECK(hasSymbol(parsed, "a_entry"));
        CHECK(hasSymbol(parsed, "g_ctor_ran"));
        CHECK(parsed.initArray().has_value());
        CHECK(!parsed.relocations().empty());
        CHECK(!parsed.pltRelocations().empty());

        // Every PT_LOAD keeps its file image inside the buffer.
        for (const ElfSegment& segment : parsed.segments()) {
            CHECK(segment.memsz >= segment.filesz);
            CHECK(segment.offset + segment.filesz <= bytes.size());
        }
    }

    // Not an ELF file at all.
    auto garbage = ElfImage::parse(std::vector<uint8_t>{ 1, 2, 3, 4 }, "garbage");
    CHECK(!garbage.ok());

    // A real header with the rest cut off.
    std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + 64);
    auto cut = ElfImage::parse(std::move(truncated), "truncated");
    CHECK(!cut.ok());

    // The loader must not be able to read the virtual modules' placeholders.
    const std::vector<uint8_t> placeholder = readFile(dir + "/liblog.so");
    CHECK(!placeholder.empty());
    auto notElf = ElfImage::parse(placeholder, "liblog.so");
    CHECK(!notElf.ok());

    return test::finish("elf_image_test");
}
