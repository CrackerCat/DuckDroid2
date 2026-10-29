// SPDX-License-Identifier: GPL-2.0-only
// M4.5 gate: map and relocate real bionic without executing it.
//
// Loads DuckEmu/bionic/{libc,libm,libdl}.so, then libz.so (APS2 + RELR +
// IFUNC imports) and libsgmainso (damaged section headers, full dependency
// closure through liblog -> libc++). Nothing is executed except IFUNC
// resolvers, which relocation itself requires.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/instance/runtime_instance.hpp"
#include "runtime/loader/elf/elf_image.hpp"
#include "runtime/observer/event_bus.hpp"
#include "runtime/observer/semantic/module_event.hpp"
#include "support/test_harness.hpp"
#include "support/trace_path.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

namespace {

constexpr uint32_t kPid = 1000;
constexpr uint32_t kTid = 1;

// AArch64 relocation types used by the assertions.
constexpr uint32_t kGlobDat = 1025;
constexpr uint32_t kJumpSlot = 1026;
constexpr uint32_t kRelative = 1027;
constexpr uint32_t kTlsTprel64 = 1030;

// First PT_TLS module of the instance (libc) lands at TlsLayout::kModulesBase
// with TP at +0x10: 0x4000 - 0x10.
constexpr uint64_t kExpectedLibcTpOff = 0x3ff0;

// libc's exported IFUNC resolver and the implementations it chooses when the
// guest advertises no optional HWCAP bits (this sample's memcpy_resolver).
constexpr uint64_t kLibcMemcpyResolver = 0xc3f78;
constexpr uint64_t kLibcMemcpyImpl = 0x54d00;

std::string bionicPath(std::string_view name) {
    return std::string(DUCKEMU_BIONIC_DIR) + "/" + std::string(name);
}

std::string romPath(std::string_view name) {
    return std::string(DUCKEMU_ROM_LIB64) + "/" + std::string(name);
}

bool fileExists(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return input.good();
}

std::optional<std::vector<uint8_t>> readFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>());
}

std::optional<loader::ElfImage> parseElf(const std::string& path) {
    auto bytes = readFile(path);
    if (!bytes.has_value()) {
        return std::nullopt;
    }
    auto image = loader::ElfImage::parse(std::move(bytes).value(), path);
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

const observer::ModuleEvent* findEvent(const std::vector<observer::ModuleEvent>& events,
                                       observer::ModuleEventKind kind,
                                       std::string_view soname,
                                       std::string_view detail_contains = {}) {
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

const observer::ModuleEvent* findLoad(const std::vector<observer::ModuleEvent>& events,
                                      std::string_view soname) {
    return findEvent(events, observer::ModuleEventKind::Load, soname);
}

uint64_t baseOf(const std::vector<observer::ModuleEvent>& events, std::string_view soname) {
    const auto* event = findLoad(events, soname);
    CHECK(event != nullptr);
    return event != nullptr ? event->base : 0;
}

// Finds the relocation slot for `symbol` in one of the given types.
std::optional<uint64_t> findSlot(const loader::ElfImage& image, std::string_view symbol,
                                 std::initializer_list<uint32_t> types) {
    auto matches = [&](const loader::ElfRelocation& reloc) {
        if (reloc.symbol >= image.symbols().size()) {
            return false;
        }
        if (image.symbols()[reloc.symbol].name != symbol) {
            return false;
        }
        for (uint32_t type : types) {
            if (reloc.type == type) {
                return true;
            }
        }
        return false;
    };

    for (const auto& reloc : image.relocations()) {
        if (matches(reloc)) {
            return reloc.offset;
        }
    }
    for (const auto& reloc : image.pltRelocations()) {
        if (matches(reloc)) {
            return reloc.offset;
        }
    }
    return std::nullopt;
}

std::optional<uint64_t> findRelocOffset(const loader::ElfImage& image, uint32_t type) {
    for (const auto& reloc : image.relocations()) {
        if (reloc.type == type) {
            return reloc.offset;
        }
    }
    for (const auto& reloc : image.pltRelocations()) {
        if (reloc.type == type) {
            return reloc.offset;
        }
    }
    return std::nullopt;
}

size_t countFailedEvents(const std::vector<observer::ModuleEvent>& events) {
    size_t failed = 0;
    for (const auto& event : events) {
        if (!event.ok) {
            ++failed;
            std::printf("  FAILED event: %s %s %s\n",
                        std::string(observer::moduleEventKindName(event.kind)).c_str(),
                        event.soname.c_str(), event.detail.c_str());
        }
    }
    return failed;
}

} // namespace

int main() {
    for (const char* asset : { "libc.so", "libm.so", "libdl.so" }) {
        if (!fileExists(bionicPath(asset))) {
            std::fprintf(stderr, "missing asset: %s\n", bionicPath(asset).c_str());
            return test::finish("04b_libc_map");
        }
    }
    if (!fileExists(romPath("libz.so")) || !fileExists(romPath("liblog.so")) ||
        !fileExists(romPath("libc++.so"))) {
        std::fprintf(stderr, "missing ROM dependency (libz/liblog/libc++)\n");
        return test::finish("04b_libc_map");
    }
    if (!fileExists(DUCKEMU_SGMAIN_SO)) {
        std::fprintf(stderr, "missing asset: %s\n", DUCKEMU_SGMAIN_SO);
        return test::finish("04b_libc_map");
    }

    RuntimeConfig config;
    config.pid = kPid;
    config.tid = kTid;
    config.trace_path = test::tracePath("04b_libc_map.log");
    config.library_search_paths = { DUCKEMU_BIONIC_DIR, DUCKEMU_ROM_LIB64,
                                    DUCKEMU_ROM_BOOTSTRAP };
    // M4.5 maps and relocates only. DT_INIT/init_array belong to M4.6.
    config.run_module_initializers = false;

    auto created = RuntimeInstance::create(config);
    CHECK_RESULT(created);
    if (!created.ok()) {
        return test::finish("04b_libc_map");
    }
    RuntimeInstance& instance = *created.value();

    std::vector<observer::ModuleEvent> events;
    instance.addModuleListener(
        [&events](const observer::ModuleEvent& event) { events.push_back(event); });

    // ------------------------------------------------------------------
    // libc: standard RELA + one TPREL64; NEEDED ld-android (virtual) + libdl.
    // ------------------------------------------------------------------
    auto libc = instance.loadLibrary(bionicPath("libc.so"));
    CHECK_RESULT(libc);
    if (!libc.ok()) {
        std::printf("  libc error: %s\n", libc.error().message.c_str());
        return test::finish("04b_libc_map");
    }
    const uint64_t libc_handle = libc.value();
    const uint64_t libc_base = baseOf(events, "libc.so");
    const auto* libc_load = findLoad(events, "libc.so");
    CHECK(libc_load != nullptr);
    if (libc_load != nullptr) {
        CHECK(libc_load->detail.find("virtual") == std::string::npos);
        CHECK(libc_load->detail.find("libc.so") != std::string::npos);
    }
    const auto* ld_android = findLoad(events, "ld-android.so");
    CHECK(ld_android != nullptr);
    if (ld_android != nullptr) {
        CHECK_EQ(ld_android->detail == "virtual", true);
    }
    const auto* libdl_load = findLoad(events, "libdl.so");
    CHECK(libdl_load != nullptr);
    if (libdl_load != nullptr) {
        CHECK(libdl_load->detail.find("virtual") == std::string::npos);
    }
    for (const std::string_view needed : { "ld-android.so", "libdl.so" }) {
        const auto* edge = findEvent(events, observer::ModuleEventKind::Needed, "libc.so", needed);
        CHECK(edge != nullptr);
        if (edge != nullptr) {
            CHECK(edge->ok);
        }
    }

    // TPREL64: libc's only TLS relocation (symbol 0, addend 0).
    if (const auto image = parseElf(bionicPath("libc.so"))) {
        const auto slot = findRelocOffset(*image, kTlsTprel64);
        CHECK(slot.has_value());
        if (slot.has_value()) {
            CHECK_EQ(readU64(instance, GuestVA{ libc_base + *slot }), kExpectedLibcTpOff);
        }
    }

    // ------------------------------------------------------------------
    // libm: APS2 (one GLOB_DAT) + RELR (27 words).
    // ------------------------------------------------------------------
    auto libm = instance.loadLibrary(bionicPath("libm.so"));
    CHECK_RESULT(libm);
    if (!libm.ok()) {
        std::printf("  libm error: %s\n", libm.error().message.c_str());
        return test::finish("04b_libc_map");
    }
    const uint64_t libm_handle = libm.value();
    const uint64_t libm_base = baseOf(events, "libm.so");

    const auto libm_image = parseElf(bionicPath("libm.so"));
    CHECK(libm_image.has_value());
    if (libm_image.has_value()) {
        // RELR relocated the first even entry at 0x37000; the file holds the
        // link-time value 0x37000, so the guest must now hold base + 0x37000.
        CHECK(findRelocOffset(*libm_image, kRelative).has_value());
        CHECK_EQ(readU64(instance, GuestVA{ libm_base + 0x37000 }), libm_base + 0x37000);

        const auto signgam_slot = findSlot(*libm_image, "signgam", { kGlobDat });
        CHECK(signgam_slot.has_value());
        if (signgam_slot.has_value()) {
            const auto signgam = instance.lookupSymbol(libm_handle, "signgam");
            CHECK_RESULT(signgam);
            if (signgam.ok()) {
                CHECK_EQ(readU64(instance, GuestVA{ libm_base + *signgam_slot }),
                         signgam.value().value());
            }
        }

        const auto memset_slot = findSlot(*libm_image, "memset", { kJumpSlot });
        CHECK(memset_slot.has_value());
        if (memset_slot.has_value()) {
            const auto memset = instance.lookupSymbol(libc_handle, "memset");
            CHECK_RESULT(memset);
            if (memset.ok()) {
                CHECK_EQ(readU64(instance, GuestVA{ libm_base + *memset_slot }),
                         memset.value().value());
            }
        }
    }

    // ------------------------------------------------------------------
    // libdl: 12 JUMP_SLOT entries into the virtual ld-android.so.
    // ------------------------------------------------------------------
    auto libdl = instance.loadLibrary(bionicPath("libdl.so"));
    CHECK_RESULT(libdl);

    // ------------------------------------------------------------------
    // libz: APS2 + RELR + IFUNC imports (memcpy/memmove).
    // ------------------------------------------------------------------
    auto libz = instance.loadLibrary(romPath("libz.so"));
    CHECK_RESULT(libz);
    if (!libz.ok()) {
        std::printf("  libz error: %s\n", libz.error().message.c_str());
        return test::finish("04b_libc_map");
    }
    const uint64_t libz_base = baseOf(events, "libz.so");

    const auto libz_image = parseElf(romPath("libz.so"));
    CHECK(libz_image.has_value());
    if (libz_image.has_value()) {
        for (const std::string_view symbol : { "memcpy", "memmove" }) {
            const auto slot = findSlot(*libz_image, symbol, { kGlobDat, kJumpSlot });
            CHECK(slot.has_value());
            if (!slot.has_value()) {
                continue;
            }
            const uint64_t value = readU64(instance, GuestVA{ libz_base + *slot });
            CHECK_EQ(value, libc_base + kLibcMemcpyImpl);
            CHECK(value != libc_base + kLibcMemcpyResolver);
        }
    }

    // ------------------------------------------------------------------
    // libsgmainso: damaged section headers, full closure through libc++.
    // ------------------------------------------------------------------
    auto sgmain = instance.loadLibrary(DUCKEMU_SGMAIN_SO);
    CHECK_RESULT(sgmain);
    if (!sgmain.ok()) {
        std::printf("  libsgmainso error: %s\n", sgmain.error().message.c_str());
        return test::finish("04b_libc_map");
    }

    for (const std::string_view soname : { "libmain.so", "liblog.so", "libc++.so", "libz.so" }) {
        CHECK(findLoad(events, soname) != nullptr);
    }
    for (const std::string_view needed :
         { "liblog.so", "libz.so", "libc.so", "libm.so", "libdl.so" }) {
        const auto* edge =
            findEvent(events, observer::ModuleEventKind::Needed, "libmain.so", needed);
        CHECK(edge != nullptr);
        if (edge != nullptr) {
            CHECK(edge->ok);
        }
    }
    const auto* libcxx_edge =
        findEvent(events, observer::ModuleEventKind::Needed, "liblog.so", "libc++.so");
    CHECK(libcxx_edge != nullptr);
    if (libcxx_edge != nullptr) {
        CHECK(libcxx_edge->ok);
    }

    const auto jni = instance.lookupSymbol(sgmain.value(), "JNI_OnLoad");
    CHECK_RESULT(jni);

    CHECK_EQ(countFailedEvents(events), 0u);

    std::printf("  modules (%zu events):\n", events.size());
    for (const auto& event : events) {
        std::printf("    %llu\t%s\t%s\t0x%llx\t%s\t%s\n",
                    static_cast<unsigned long long>(event.id),
                    std::string(observer::moduleEventKindName(event.kind)).c_str(),
                    event.soname.c_str(), static_cast<unsigned long long>(event.base),
                    event.ok ? "ok" : "FAILED", event.detail.c_str());
    }
    std::printf("  libc base=0x%llx tp_off=0x%llx\n",
                static_cast<unsigned long long>(libc_base),
                static_cast<unsigned long long>(kExpectedLibcTpOff));

    return test::finish("04b_libc_map");
}
