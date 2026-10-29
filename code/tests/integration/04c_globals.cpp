// SPDX-License-Identifier: GPL-2.0-only
// M4.6 S1: the virtual ld-android.so exposes a real libc_shared_globals block.
// Calling libc's getauxval() walks that block's auxv, so it proves the offsets
// (0x418), the virtual __loader_shared_globals thunk, and the auxv contents
// without running preinit.

#include <cstdint>
#include <cstdio>
#include <string>

#include "runtime/instance/runtime_instance.hpp"
#include "runtime/memory/region.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

namespace {

constexpr uint32_t kPid = 1000;
constexpr uint32_t kTid = 1;

constexpr uint64_t kAtPagesz = 6;
constexpr uint64_t kAtUid = 11;
constexpr uint64_t kAtClktck = 17;
constexpr uint64_t kAtRandom = 25;
constexpr uint64_t kAtExecfn = 31;

std::string romLibc() {
    return std::string(DUCKEMU_ROM_BOOTSTRAP) + "/libc.so";
}

uint64_t callGetauxval(RuntimeInstance& instance, GuestVA entry, uint64_t type) {
    auto call = instance.callFunction(entry, { type });
    CHECK_RESULT(call);
    return call.ok() ? call.value().return_value : 0;
}

std::string readString(RuntimeInstance& instance, uint64_t address) {
    char buffer[64] = {};
    auto read = instance.readMemory(GuestVA{ address }, buffer, sizeof(buffer) - 1);
    CHECK_RESULT(read);
    return std::string(buffer);
}

} // namespace

int main() {
    RuntimeConfig config;
    config.pid = kPid;
    config.tid = kTid;
    config.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };
    config.run_module_initializers = false;

    auto created = RuntimeInstance::create(config);
    CHECK_RESULT(created);
    if (!created.ok()) {
        return test::finish("04c_globals");
    }
    RuntimeInstance& instance = *created.value();

    auto libc = instance.loadLibrary(romLibc());
    CHECK_RESULT(libc);
    if (!libc.ok()) {
        std::printf("  libc error: %s\n", libc.error().message.c_str());
        return test::finish("04c_globals");
    }

    auto getauxval = instance.lookupSymbol(libc.value(), "getauxval");
    CHECK_RESULT(getauxval);
    if (!getauxval.ok()) {
        return test::finish("04c_globals");
    }

    const GuestVA entry = getauxval.value();
    CHECK_EQ(callGetauxval(instance, entry, kAtPagesz), kGuestPageSize);
    CHECK_EQ(callGetauxval(instance, entry, kAtClktck), 100u);
    CHECK_EQ(callGetauxval(instance, entry, kAtUid), kPid);

    const uint64_t random_address = callGetauxval(instance, entry, kAtRandom);
    CHECK(random_address != 0);
    if (random_address != 0) {
        uint8_t bytes[16] = {};
        CHECK_RESULT(instance.readMemory(GuestVA{ random_address }, bytes, sizeof(bytes)));
        bool nonzero = false;
        for (uint8_t byte : bytes) {
            nonzero = nonzero || byte != 0;
        }
        CHECK(nonzero);
    }

    const uint64_t execfn = callGetauxval(instance, entry, kAtExecfn);
    CHECK(execfn != 0);
    if (execfn != 0) {
        const std::string progname = readString(instance, execfn);
        std::printf("  AT_EXECFN=\"%s\"\n", progname.c_str());
        CHECK_EQ(progname == "duckemu", true);
    }

    // Unknown auxiliary vector types resolve to 0, not a fault.
    CHECK_EQ(callGetauxval(instance, entry, 0x1234), 0u);

    return test::finish("04c_globals");
}
