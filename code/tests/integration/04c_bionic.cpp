// M4.6 S5 (main gate): real bionic pthreads. The ROM libc allocates its own
// thread stacks/TLS through the StaticTlsLayout we publish, then enters
// clone(2) exactly like a real pthread_create. This gate drives the libc
// entry points directly:
//   - N create/join cycles with the worker function incrementing a counter
//   - pthread_join's return_value comes from pthread_exit
//   - errno lives in each thread's pthread_internal_t, so the addresses differ

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "runtime/instance/runtime_instance.hpp"
#include "runtime/memory/region.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

namespace {

constexpr uint32_t kPid = 1000;
constexpr uint32_t kTid = 1;

constexpr uint32_t kThreadCount = 10;

constexpr uint64_t kCodePageSize = 0x1000;
constexpr uint64_t kDataPageSize = 0x1000;
constexpr uint64_t kCounterOffset = 0;
constexpr uint64_t kErrnoSlotOffset = 8;
constexpr uint64_t kPthreadSlotOffset = 16;
constexpr uint64_t kReturnValueOffset = 24;
constexpr uint64_t kErrnoSlot2Offset = 32;
constexpr uint64_t kPthreadSlot2Offset = 40;
constexpr uint64_t kPair1Offset = 0x40; // { errno_out, gate }
constexpr uint64_t kPair2Offset = 0x50;
constexpr uint64_t kGateOffset = 0x100;
constexpr uint64_t kStatusPathOffset = 0x200;
constexpr uint64_t kStatusTextOffset = 0x300;
constexpr uint64_t kStatusTextSize = 0x800;
constexpr uint64_t kO_RdOnly = 0;
constexpr uint64_t kFutexWake = 1;

// worker_increment(void* arg):
//   ldr w1,[x0]; add w1,w1,#1; str w1,[x0]; mov w0,#42; ret
constexpr uint32_t kWorkerIncrement[] = {
    0xB9400001, // ldr w1, [x0]
    0x11000421, // add w1, w1, #1
    0xB9000001, // str w1, [x0]
    0x52800540, // mov w0, #42
    0xD65F03C0, // ret
};

// worker_errno(void* arg), arg = { uint64_t* errno_out; uint32_t gate; }:
// records __errno() (thread + 768) and parks in futex(gate, WAIT, 0) until the
// parent releases it, so both workers are alive while /proc/self/status is read.
constexpr uint32_t kWorkerErrno[] = {
    0xD53BD041, // mrs x1, tpidr_el0
    0xF9400421, // ldr x1, [x1, #8]
    0x910C0021, // add x1, x1, #768
    0xF9400002, // ldr x2, [x0]        ; x2 = errno_out
    0xF9000041, // str x1, [x2]        ; *errno_out = __errno()
    0xF9400402, // ldr x2, [x0, #8]    ; x2 = gate
    0xAA0203E0, // mov x0, x2
    0xD2800001, // mov x1, #0          ; FUTEX_WAIT
    0x52800002, // mov w2, #0          ; expected 0
    0xD2800003, // mov x3, #0          ; no timeout
    0xD2800C48, // mov x8, #98         ; futex
    0xD4000001, // svc #0
    0x52800000, // mov w0, #0
    0xD65F03C0, // ret
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

} // namespace

int main() {
    RuntimeConfig config;
    config.pid = kPid;
    config.tid = kTid;
    config.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };

    auto created = RuntimeInstance::create(config);
    CHECK_RESULT(created);
    if (!created.ok()) {
        return test::finish("04c_bionic");
    }
    RuntimeInstance& instance = *created.value();

    auto libc = instance.loadLibrary(romLibc());
    CHECK_RESULT(libc);
    if (!libc.ok()) {
        std::printf("  libc error: %s\n", libc.error().message.c_str());
        return test::finish("04c_bionic");
    }
    const uint64_t handle = libc.value();

    auto lookup = [&](const char* name) -> GuestVA {
        auto found = instance.lookupSymbol(handle, name);
        CHECK_RESULT(found);
        return found.ok() ? found.value() : GuestVA{};
    };
    const GuestVA pthread_create = lookup("pthread_create");
    const GuestVA pthread_join = lookup("pthread_join");
    const GuestVA errno_fn = lookup("__errno");
    const GuestVA open_fn = lookup("open");
    const GuestVA read_fn = lookup("read");
    const GuestVA close_fn = lookup("close");
    const GuestVA syscall_fn = lookup("syscall");

    auto code_page = instance.mapMemory(kCodePageSize, PermRead | PermExec, "worker-code");
    auto data_page = instance.mapMemory(kDataPageSize, PermReadWrite, "worker-data");
    CHECK_RESULT(code_page);
    CHECK_RESULT(data_page);
    if (!code_page.ok() || !data_page.ok()) {
        return test::finish("04c_bionic");
    }
    const uint64_t code = code_page.value().value();
    const uint64_t data = data_page.value().value();
    const uint64_t increment_worker = code;
    const uint64_t errno_worker = code + 0x100;

    CHECK_RESULT(instance.writeMemory(GuestVA{ increment_worker }, kWorkerIncrement,
                                      sizeof(kWorkerIncrement)));
    CHECK_RESULT(instance.writeMemory(GuestVA{ errno_worker }, kWorkerErrno,
                                      sizeof(kWorkerErrno)));

    const uint64_t counter_address = data + kCounterOffset;
    const uint64_t errno_slot = data + kErrnoSlotOffset;
    const uint64_t pthread_slot = data + kPthreadSlotOffset;
    const uint64_t return_value = data + kReturnValueOffset;

    uint64_t failed_creates = 0;
    for (uint32_t i = 0; i < kThreadCount; ++i) {
        const uint64_t create_result =
            callGuest(instance, pthread_create, { pthread_slot, 0, increment_worker, counter_address });
        uint64_t thread_id = 0;
        readGuest(instance, pthread_slot, &thread_id, sizeof(thread_id));
        if (create_result != 0) {
            ++failed_creates;
            std::printf("  pthread_create failed: %lld\n", static_cast<long long>(create_result));
            break;
        }
        const uint64_t join_result = callGuest(instance, pthread_join, { thread_id, return_value });
        if (join_result != 0) {
            std::printf("  pthread_join failed: %lld\n", static_cast<long long>(join_result));
            ++failed_creates;
            break;
        }
    }
    CHECK_EQ(failed_creates, 0);

    uint64_t counter = 0;
    uint64_t joined_return = 0;
    readGuest(instance, counter_address, &counter, sizeof(counter));
    readGuest(instance, return_value, &joined_return, sizeof(joined_return));
    std::printf("  counter=%llu return_value=%llu\n",
                static_cast<unsigned long long>(counter),
                static_cast<unsigned long long>(joined_return));
    CHECK_EQ(counter, kThreadCount);
    CHECK_EQ(joined_return, 42);

    // errno is per-thread: __errno() points into pthread_internal_t. Two
    // workers are created before either is joined, so they are both alive
    // while the parent reads /proc/self/status: Threads must be 3 (main + 2).
    const uint64_t errno_slot2 = data + kErrnoSlot2Offset;
    const uint64_t pthread_slot2 = data + kPthreadSlot2Offset;
    const uint64_t gate = data + kGateOffset;
    const uint64_t pair1 = data + kPair1Offset;
    const uint64_t pair2 = data + kPair2Offset;
    const uint64_t pair_values1[2] = { errno_slot, gate };
    const uint64_t pair_values2[2] = { errno_slot2, gate };
    CHECK_RESULT(instance.writeMemory(GuestVA{ pair1 }, pair_values1, sizeof(pair_values1)));
    CHECK_RESULT(instance.writeMemory(GuestVA{ pair2 }, pair_values2, sizeof(pair_values2)));

    const uint64_t first_create =
        callGuest(instance, pthread_create, { pthread_slot, 0, errno_worker, pair1 });
    const uint64_t second_create =
        callGuest(instance, pthread_create, { pthread_slot2, 0, errno_worker, pair2 });
    CHECK_EQ(first_create, 0);
    CHECK_EQ(second_create, 0);
    if (first_create == 0 && second_create == 0) {
        uint64_t first_thread = 0;
        uint64_t second_thread = 0;
        readGuest(instance, pthread_slot, &first_thread, sizeof(first_thread));
        readGuest(instance, pthread_slot2, &second_thread, sizeof(second_thread));

        const uint64_t status_path = data + kStatusPathOffset;
        const uint64_t status_text = data + kStatusTextOffset;
        const char path_text[] = "/proc/self/status";
        CHECK_RESULT(instance.writeMemory(GuestVA{ status_path }, path_text, sizeof(path_text)));
        const uint64_t status_fd =
            callGuest(instance, open_fn, { status_path, kO_RdOnly, 0 });
        CHECK(status_fd >= 3);
        if (status_fd >= 3) {
            const uint64_t bytes = callGuest(instance, read_fn,
                                             { status_fd, status_text, kStatusTextSize - 1 });
            CHECK(bytes > 0);
            std::array<char, kStatusTextSize> text{};
            if (bytes > 0 && bytes < text.size()) {
                readGuest(instance, status_text, text.data(), bytes);
                const std::string status(text.data(), bytes);
                const size_t threads_at = status.find("Threads:");
                std::printf("  %s", threads_at == std::string::npos ? "Threads: <missing>\n" :
                            status.c_str() + threads_at);
                CHECK(threads_at != std::string::npos);
                if (threads_at != std::string::npos) {
                    const size_t value_at = status.find_first_of("0123456789", threads_at);
                    CHECK(value_at != std::string::npos);
                    if (value_at != std::string::npos) {
                        CHECK_EQ(std::strtoul(status.c_str() + value_at, nullptr, 10), 3);
                    }
                }
            }
            CHECK_EQ(callGuest(instance, close_fn, { status_fd }), 0);
        }

        // Release both parked workers, then reap them through pthread_join.
        const uint32_t released = 1;
        CHECK_RESULT(instance.writeMemory(GuestVA{ gate }, &released, sizeof(released)));
        callGuest(instance, syscall_fn, { 98, gate, kFutexWake, 0x7FFFFFFF });
        CHECK_EQ(callGuest(instance, pthread_join, { first_thread, 0 }), 0);
        CHECK_EQ(callGuest(instance, pthread_join, { second_thread, 0 }), 0);

        uint64_t first_errno = 0;
        uint64_t second_errno = 0;
        readGuest(instance, errno_slot, &first_errno, sizeof(first_errno));
        readGuest(instance, errno_slot2, &second_errno, sizeof(second_errno));
        const uint64_t main_errno = callGuest(instance, errno_fn, {});
        std::printf("  worker errno=0x%llx/0x%llx main errno=0x%llx\n",
                    static_cast<unsigned long long>(first_errno),
                    static_cast<unsigned long long>(second_errno),
                    static_cast<unsigned long long>(main_errno));
        CHECK(first_errno != 0);
        CHECK(second_errno != 0);
        CHECK(first_errno != second_errno);
        CHECK(first_errno != main_errno);
        CHECK(second_errno != main_errno);
    }

    // libeleven up to the JNI boundary: load the real closure (liblog/libm/
    // libdl/libc++ against the ROM libc), run every constructor, and resolve
    // JNI_OnLoad itself. Since M5 loadLibrary also calls it with a real
    // JavaVM; 05_jni_real checks the registration it performs.
    auto eleven = instance.loadLibrary(DUCKEMU_LIBELEVEN_SO);
    CHECK_RESULT(eleven);
    if (eleven.ok()) {
        auto jni_on_load = instance.lookupSymbol(eleven.value(), "JNI_OnLoad");
        CHECK_RESULT(jni_on_load);
        if (jni_on_load.ok()) {
            std::printf("  libeleven loaded, JNI_OnLoad=0x%llx\n",
                        static_cast<unsigned long long>(jni_on_load.value().value()));
            CHECK(jni_on_load.value().value() != 0);
        }
    }

    return test::finish("04c_bionic");
}
