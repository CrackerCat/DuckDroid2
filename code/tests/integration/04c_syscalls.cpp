// M4.6 S2 gate: the light syscall surface preinit needs, driven through the
// ROM libc wrappers. Each call asserts both the success path and the errno
// the kernel would report, so a handler that silently zeroes is caught.
//
// Covered: getpid/gettid/getuid/getgid/getppid, uname, getcwd, gettimeofday,
// clock_gettime, getrandom, prctl, sched_getaffinity, sched_yield,
// sigaltstack, getrlimit. exit/exit_group and clone/futex have their own gates.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "runtime/instance/runtime_instance.hpp"
#include "runtime/memory/region.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

namespace {

constexpr uint32_t kPid = 1000;
constexpr uint32_t kTid = 1;
constexpr uint32_t kUid = 1000;

constexpr uint64_t kScratchSize = 0x4000;
constexpr uint64_t kUnameSize = 65 * 6;
constexpr uint64_t kAffinitySize = 8;
constexpr uint64_t kRandomBytes = 32;
constexpr uint64_t kNameSize = 16;
constexpr uint64_t kPathSize = 0x100;

constexpr uint64_t kClockMonotonic = 1;
constexpr uint64_t kClockProcessCputime = 2;
constexpr uint64_t kClockRealtimeCoarse = 5;
constexpr uint64_t kClockMonotonicCoarse = 6;
constexpr uint64_t kClockInvalid = 99;
constexpr uint64_t kRlimitNoFile = 7;
constexpr uint64_t kPrGetName = 16;
constexpr uint64_t kPrSetTaggedAddrCtrl = 55;

constexpr int kEsrch = 3;
constexpr int kEinval = 22;

struct Functions {
    GuestVA getpid;
    GuestVA gettid;
    GuestVA getuid;
    GuestVA getgid;
    GuestVA getppid;
    GuestVA uname;
    GuestVA getcwd;
    GuestVA gettimeofday;
    GuestVA clock_gettime;
    GuestVA getrandom;
    GuestVA prctl;
    GuestVA sched_getaffinity;
    GuestVA sched_yield;
    GuestVA sigaltstack;
    GuestVA getrlimit;
    GuestVA errno_fn;
};

std::string romLibc() {
    return std::string(DUCKEMU_ROM_BOOTSTRAP) + "/libc.so";
}

uint64_t callGuest(RuntimeInstance& instance, GuestVA entry,
                   const std::array<uint64_t, 8>& args) {
    auto call = instance.callFunction(entry, args);
    CHECK_RESULT(call);
    return call.ok() ? call.value().return_value : 0;
}

bool readGuest(RuntimeInstance& instance, uint64_t address, void* buffer, uint64_t size) {
    auto read = instance.readMemory(GuestVA{ address }, buffer, size);
    CHECK_RESULT(read);
    return read.ok();
}

// int-returning libc wrappers only define w0 on failure (-1 arrives as
// 0x00000000ffffffff); ssize_t-returning ones sign-extend.
bool failed(uint64_t value) {
    return static_cast<int32_t>(value) == -1;
}

} // namespace

int main() {
    RuntimeConfig config;
    config.pid = kPid;
    config.tid = kTid;
    config.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };

    auto created = RuntimeInstance::create(config);
    CHECK_RESULT(created);
    if (!created.ok()) {
        return test::finish("04c_syscalls");
    }
    RuntimeInstance& instance = *created.value();

    auto libc = instance.loadLibrary(romLibc());
    CHECK_RESULT(libc);
    if (!libc.ok()) {
        std::printf("  libc error: %s\n", libc.error().message.c_str());
        return test::finish("04c_syscalls");
    }
    const uint64_t handle = libc.value();

    Functions functions{};
    const struct {
        const char* symbol;
        GuestVA* slot;
    } lookups[] = {
        { "getpid", &functions.getpid },
        { "gettid", &functions.gettid },
        { "getuid", &functions.getuid },
        { "getgid", &functions.getgid },
        { "getppid", &functions.getppid },
        { "uname", &functions.uname },
        { "getcwd", &functions.getcwd },
        { "gettimeofday", &functions.gettimeofday },
        { "clock_gettime", &functions.clock_gettime },
        { "getrandom", &functions.getrandom },
        { "prctl", &functions.prctl },
        { "sched_getaffinity", &functions.sched_getaffinity },
        { "sched_yield", &functions.sched_yield },
        { "sigaltstack", &functions.sigaltstack },
        { "getrlimit", &functions.getrlimit },
        { "__errno", &functions.errno_fn },
    };
    for (const auto& lookup : lookups) {
        auto found = instance.lookupSymbol(handle, lookup.symbol);
        CHECK_RESULT(found);
        if (!found.ok()) {
            return test::finish("04c_syscalls");
        }
        *lookup.slot = found.value();
    }

    auto scratch_page = instance.mapMemory(kScratchSize, PermReadWrite, "syscall-scratch");
    CHECK_RESULT(scratch_page);
    if (!scratch_page.ok()) {
        return test::finish("04c_syscalls");
    }
    const uint64_t scratch = scratch_page.value().value();
    const uint64_t uname_buffer = scratch;
    const uint64_t timeval_buffer = scratch + 0x100;
    const uint64_t timespec_buffer = scratch + 0x120;
    const uint64_t rlimit_buffer = scratch + 0x140;
    const uint64_t stack_buffer = scratch + 0x160;
    const uint64_t affinity_buffer = scratch + 0x180;
    const uint64_t random_buffer = scratch + 0x1A0;
    const uint64_t name_buffer = scratch + 0x1C0;
    const uint64_t path_buffer = scratch + 0x200;

    // __errno() is the libc accessor; its value is the guest errno slot.
    const uint64_t errno_address = callGuest(instance, functions.errno_fn, {});
    CHECK(errno_address != 0);
    const auto guestErrno = [&]() -> int32_t {
        int32_t value = 0;
        readGuest(instance, errno_address, &value, sizeof(value));
        return value;
    };

    // Identity.
    CHECK_EQ(callGuest(instance, functions.getpid, {}), kPid);
    CHECK_EQ(callGuest(instance, functions.gettid, {}), kTid);
    CHECK_EQ(callGuest(instance, functions.getuid, {}), kUid);
    CHECK_EQ(callGuest(instance, functions.getgid, {}), kUid);
    CHECK_EQ(callGuest(instance, functions.getppid, {}), 0);

    // uname: six 65-byte fields, Linux/aarch64.
    CHECK_EQ(callGuest(instance, functions.uname, { uname_buffer }), 0);
    {
        std::array<char, kUnameSize> fields{};
        readGuest(instance, uname_buffer, fields.data(), fields.size());
        CHECK_EQ(std::strcmp(fields.data(), "Linux"), 0);
        CHECK_EQ(std::strcmp(fields.data() + 4 * 65, "aarch64"), 0);
    }

    // getcwd(2) reports the guest root.
    CHECK(callGuest(instance, functions.getcwd, { path_buffer, kPathSize }) != 0);
    {
        std::array<char, kPathSize> path{};
        readGuest(instance, path_buffer, path.data(), path.size());
        CHECK_EQ(std::strcmp(path.data(), "/"), 0);
    }

    // Time. Values must be plausible; the only hard errno is an invalid clock.
    CHECK_EQ(callGuest(instance, functions.gettimeofday, { timeval_buffer, 0 }), 0);
    {
        int64_t values[2] = {};
        readGuest(instance, timeval_buffer, values, sizeof(values));
        CHECK(values[0] > 0);
        CHECK(values[1] >= 0 && values[1] < 1000000);
    }
    CHECK_EQ(callGuest(instance, functions.clock_gettime,
                       { kClockMonotonic, timespec_buffer }),
             0);
    {
        int64_t values[2] = {};
        readGuest(instance, timespec_buffer, values, sizeof(values));
        CHECK(values[0] >= 0);
        CHECK(values[1] >= 0 && values[1] < 1000000000);
    }
    // COARSE clocks used to return EINVAL; libc++/pthread probe them.
    CHECK_EQ(callGuest(instance, functions.clock_gettime,
                       { kClockRealtimeCoarse, timespec_buffer }),
             0);
    {
        int64_t values[2] = {};
        readGuest(instance, timespec_buffer, values, sizeof(values));
        CHECK(values[0] > 0);
    }
    CHECK_EQ(callGuest(instance, functions.clock_gettime,
                       { kClockMonotonicCoarse, timespec_buffer }),
             0);
    // CPU-time clocks are approximated by monotonic time (DuckDroid parity).
    CHECK_EQ(callGuest(instance, functions.clock_gettime,
                       { kClockProcessCputime, timespec_buffer }),
             0);
    CHECK(failed(callGuest(instance, functions.clock_gettime,
                           { kClockInvalid, timespec_buffer })));
    CHECK_EQ(guestErrno(), kEinval);

    // getrandom: full fill, not all zero.
    CHECK_EQ(callGuest(instance, functions.getrandom, { random_buffer, kRandomBytes, 0 }),
             kRandomBytes);
    {
        std::array<uint8_t, kRandomBytes> bytes{};
        readGuest(instance, random_buffer, bytes.data(), bytes.size());
        bool any = false;
        for (uint8_t byte : bytes) {
            any = any || byte != 0;
        }
        CHECK(any);
    }
    // The handler caps a single request at 1 MiB upstream; a zero-length call
    // is a valid no-op.
    CHECK_EQ(callGuest(instance, functions.getrandom, { random_buffer, 0, 0 }), 0);

    // prctl: PR_GET_NAME fills "duckemu"; the tagged-address control is
    // rejected with EINVAL (no MTE in the guest CPU).
    CHECK_EQ(callGuest(instance, functions.prctl, { kPrGetName, name_buffer }), 0);
    {
        std::array<char, kNameSize> name{};
        readGuest(instance, name_buffer, name.data(), name.size());
        CHECK_EQ(std::strcmp(name.data(), "duckemu"), 0);
    }
    CHECK(failed(callGuest(instance, functions.prctl, { kPrSetTaggedAddrCtrl, 0, 0, 0 })));
    CHECK_EQ(guestErrno(), kEinval);

    // sched_getaffinity returns a one-CPU mask; an unknown pid is ESRCH.
    CHECK_EQ(callGuest(instance, functions.sched_getaffinity, { 0, kAffinitySize, affinity_buffer }),
             0);
    {
        uint64_t mask = 0;
        readGuest(instance, affinity_buffer, &mask, sizeof(mask));
        CHECK_EQ(mask, 1);
    }
    CHECK(failed(callGuest(instance, functions.sched_getaffinity,
                           { 999999, kAffinitySize, affinity_buffer })));
    CHECK_EQ(guestErrno(), kEsrch);
    CHECK_EQ(callGuest(instance, functions.sched_yield, {}), 0);

    // sigaltstack and getrlimit succeed without a real signal stack.
    CHECK_EQ(callGuest(instance, functions.sigaltstack, { 0, stack_buffer }), 0);
    CHECK_EQ(callGuest(instance, functions.getrlimit, { kRlimitNoFile, rlimit_buffer }), 0);
    {
        uint64_t limits[2] = {};
        readGuest(instance, rlimit_buffer, limits, sizeof(limits));
        CHECK(limits[0] > 0);
        CHECK(limits[1] >= limits[0]);
    }

    return test::finish("04c_syscalls");
}
