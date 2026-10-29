// SPDX-License-Identifier: GPL-2.0-only
// M4.5 TLSDESC path: clang's Android target only emits R_AARCH64_TLSDESC for
// __thread access. The loader assigns a static TLS slot, writes a descriptor
// whose resolver is the guest stub page, and copies the PT_TLS image into the
// main thread's block. Calling into the module exercises all three.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "runtime/instance/runtime_instance.hpp"
#include "runtime/loader/elf/elf_image.hpp"
#include "runtime/observer/event_bus.hpp"
#include "runtime/observer/semantic/module_event.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

namespace {

constexpr uint32_t kPid = 1000;
constexpr uint32_t kTid = 1;
constexpr uint32_t kTlsDesc = 1031;

// The fixture is the first (and only) PT_TLS module: TlsLayout::kModulesBase
// (0x4000) minus the thread-pointer offset (0x10).
constexpr uint64_t kExpectedTpOff = 0x3ff0;

std::string soPath(std::string_view name) {
    return std::string(DUCKEMU_SO_DIR) + "/" + std::string(name);
}

std::optional<loader::ElfImage> parseElf(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    auto image = loader::ElfImage::parse(std::move(bytes), path);
    if (!image.ok()) {
        std::fprintf(stderr, "parse %s failed: %s\n", path.c_str(),
                     image.error().message.c_str());
        return std::nullopt;
    }
    return std::move(image).value();
}

uint64_t readU64(RuntimeInstance& instance, GuestVA address) {
    uint64_t value = 0;
    CHECK_RESULT(instance.readMemory(address, &value, sizeof(value)));
    return value;
}

const loader::ElfSymbol* findSymbol(const loader::ElfImage& image, std::string_view name) {
    for (const auto& symbol : image.symbols()) {
        if (!symbol.undefined() && symbol.name == name) {
            return &symbol;
        }
    }
    return nullptr;
}

} // namespace

int main() {
    RuntimeConfig config;
    config.pid = kPid;
    config.tid = kTid;
    config.library_search_paths = { DUCKEMU_SO_DIR };

    auto created = RuntimeInstance::create(config);
    CHECK_RESULT(created);
    if (!created.ok()) {
        return test::finish("04b_tlsdesc");
    }
    RuntimeInstance& instance = *created.value();

    uint64_t base = 0;
    instance.addModuleListener([&base](const observer::ModuleEvent& event) {
        if (event.kind == observer::ModuleEventKind::Load && event.soname == "libtls.so") {
            base = event.base;
        }
    });

    auto loaded = instance.loadLibrary(soPath("libtls.so"));
    CHECK_RESULT(loaded);
    if (!loaded.ok()) {
        std::printf("  libtls error: %s\n", loaded.error().message.c_str());
        return test::finish("04b_tlsdesc");
    }
    const uint64_t handle = loaded.value();
    CHECK(base != 0);

    auto image = parseElf(soPath("libtls.so"));
    CHECK(image.has_value());
    if (!image.has_value() || base == 0) {
        return test::finish("04b_tlsdesc");
    }

    std::optional<uint64_t> descriptor;
    for (const auto& reloc : image->relocations()) {
        if (reloc.type == kTlsDesc) {
            descriptor = reloc.offset;
            break;
        }
    }
    CHECK(descriptor.has_value());
    if (!descriptor.has_value()) {
        return test::finish("04b_tlsdesc");
    }

    // Descriptor layout on AArch64 is {resolver, argument}.
    const uint64_t resolver = readU64(instance, GuestVA{ base + *descriptor });
    const uint64_t argument = readU64(instance, GuestVA{ base + *descriptor + 8 });
    std::printf("  tlsdesc: func=0x%llx arg=0x%llx\n",
                static_cast<unsigned long long>(resolver),
                static_cast<unsigned long long>(argument));
    CHECK(resolver != 0);
    CHECK_EQ(argument, kExpectedTpOff);

    auto entry = instance.lookupSymbol(handle, "tls_get");
    CHECK_RESULT(entry);
    if (!entry.ok()) {
        return test::finish("04b_tlsdesc");
    }
    if (const auto* symbol = findSymbol(*image, "tls_get")) {
        CHECK_EQ(entry.value().value(), base + symbol->value);
    }

    // PT_TLS image was materialized: the initial value is 7.
    auto get = instance.callFunction(entry.value());
    CHECK_RESULT(get);
    if (get.ok()) {
        CHECK_EQ(get.value().return_value, 7u);
    }

    auto set = instance.lookupSymbol(handle, "tls_set");
    CHECK_RESULT(set);
    if (set.ok()) {
        auto wrote = instance.callFunction(set.value(), { 11 });
        CHECK_RESULT(wrote);
    }

    auto again = instance.callFunction(entry.value());
    CHECK_RESULT(again);
    if (again.ok()) {
        CHECK_EQ(again.value().return_value, 11u);
    }

    return test::finish("04b_tlsdesc");
}
