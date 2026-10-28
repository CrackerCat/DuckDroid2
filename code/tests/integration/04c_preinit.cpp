// M4.6 S4: real libc preinit runs. Loading the ROM libc with initializers
// enabled executes __libc_preinit (constructor priority 1), which initialises
// globals, scudo and the main thread. malloc/getenv then have to work through
// the real code paths.

#include <array>
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

std::string romLibc() {
    return std::string(DUCKEMU_ROM_BOOTSTRAP) + "/libc.so";
}

uint64_t callGuest(RuntimeInstance& instance, GuestVA entry, const std::array<uint64_t, 8>& args) {
    auto call = instance.callFunction(entry, args);
    CHECK_RESULT(call);
    return call.ok() ? call.value().return_value : 0;
}

std::string readString(RuntimeInstance& instance, uint64_t address) {
    char buffer[128] = {};
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
    // Deliberately left at the default: preinit and the init_array run here.

    auto created = RuntimeInstance::create(config);
    CHECK_RESULT(created);
    if (!created.ok()) {
        return test::finish("04c_preinit");
    }
    RuntimeInstance& instance = *created.value();

    auto libc = instance.loadLibrary(romLibc());
    CHECK_RESULT(libc);
    if (!libc.ok()) {
        std::printf("  libc error: %s\n", libc.error().message.c_str());
        return test::finish("04c_preinit");
    }
    const uint64_t handle = libc.value();

    // preinit initialises environ from __libc_shared_globals.
    auto getenv = instance.lookupSymbol(handle, "getenv");
    CHECK_RESULT(getenv);
    auto env_string = instance.mapMemory(0x1000, PermReadWrite, "test-env");
    CHECK_RESULT(env_string);
    if (getenv.ok() && env_string.ok()) {
        const char name[] = "ANDROID_ROOT";
        CHECK_RESULT(instance.writeMemory(env_string.value(), name, sizeof(name)));
        const uint64_t value = callGuest(instance, getenv.value(), { env_string.value().value() });
        CHECK(value != 0);
        if (value != 0) {
            const std::string text = readString(instance, value);
            std::printf("  getenv(ANDROID_ROOT)=\"%s\"\n", text.c_str());
            CHECK_EQ(text == "/system", true);
        }
    }

    // Real scudo malloc/free.
    auto malloc_fn = instance.lookupSymbol(handle, "malloc");
    auto free_fn = instance.lookupSymbol(handle, "free");
    auto calloc_fn = instance.lookupSymbol(handle, "calloc");
    CHECK_RESULT(malloc_fn);
    CHECK_RESULT(free_fn);
    CHECK_RESULT(calloc_fn);
    if (!malloc_fn.ok() || !free_fn.ok() || !calloc_fn.ok()) {
        return test::finish("04c_preinit");
    }

    const uint64_t pointer = callGuest(instance, malloc_fn.value(), { 64 });
    std::printf("  malloc(64)=0x%llx\n", static_cast<unsigned long long>(pointer));
    CHECK(pointer != 0);
    if (pointer != 0) {
        const uint64_t pattern = 0x1122334455667788ULL;
        CHECK_RESULT(instance.writeMemory(GuestVA{ pointer }, &pattern, sizeof(pattern)));
        uint64_t readback = 0;
        CHECK_RESULT(instance.readMemory(GuestVA{ pointer }, &readback, sizeof(readback)));
        CHECK_EQ(readback, pattern);
        // free returns void; only the absence of a fault matters.
        (void)callGuest(instance, free_fn.value(), { pointer });
    }

    const uint64_t calloc_pointer = callGuest(instance, calloc_fn.value(), { 4, 16 });
    CHECK(calloc_pointer != 0);
    if (calloc_pointer != 0) {
        uint8_t zeroes[64] = {};
        CHECK_RESULT(instance.readMemory(GuestVA{ calloc_pointer }, zeroes, sizeof(zeroes)));
        bool all_zero = true;
        for (uint8_t byte : zeroes) {
            all_zero = all_zero && byte == 0;
        }
        CHECK(all_zero);
        (void)callGuest(instance, free_fn.value(), { calloc_pointer });
    }

    return test::finish("04c_preinit");
}
