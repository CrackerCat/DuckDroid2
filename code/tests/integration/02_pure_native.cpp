// M1 gate: a bare native blob runs to completion.
//
// Exercises the non-blocking SVC path (getpid/gettid/mmap/mprotect/munmap/
// write), a host stub through the thunk page, and exit_group.

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string_view>
#include <vector>

#include "runtime/instance/runtime_instance.hpp"
#include "runtime/memory/region.hpp"
#include "support/blob_loader.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

namespace {

constexpr uint32_t kPid = 1000;
constexpr uint32_t kTid = 1;

struct NativeOutput {
    uint64_t pid;
    uint64_t tid;
    uint64_t mmap_address;
    uint64_t loaded;
    uint64_t mprotect_return;
    uint64_t host_return;
    uint64_t write_return;
    uint64_t munmap_return;
};
static_assert(sizeof(NativeOutput) == 64, "guest writes 64 bytes");

Result<GuestVA> loadBlobInto(RuntimeInstance& guest,
                             const std::vector<uint8_t>& blob,
                             std::string_view name) {
    auto code = guest.mapMemory(blob.size(), PermRead | PermExec, name);
    if (!code.ok()) {
        return code.error();
    }
    auto written = guest.writeMemory(code.value(), blob.data(), blob.size());
    if (!written.ok()) {
        return written.error();
    }
    return code.value();
}

void runPureNative() {
    auto blob = test::loadBlob("02_pure_native.bin");
    CHECK_RESULT(blob);
    auto exit_blob = test::loadBlob("02_exit_group.bin");
    CHECK_RESULT(exit_blob);
    if (!blob.ok() || !exit_blob.ok()) {
        return;
    }

    RuntimeConfig config;
    config.pid = kPid;
    config.tid = kTid;

    auto instance = RuntimeInstance::create(config);
    CHECK_RESULT(instance);
    if (!instance.ok()) {
        return;
    }
    RuntimeInstance& guest = *instance.value();

    auto thunk = guest.registerHostFunction(
        "host_add", [](HostCallArgs& args) -> int64_t {
            return static_cast<int64_t>(args.x[0] + args.x[1]);
        });
    CHECK_RESULT(thunk);
    if (!thunk.ok()) {
        return;
    }

    auto code = loadBlobInto(guest, blob.value(), "code-native");
    CHECK_RESULT(code);
    auto exit_code_blob = loadBlobInto(guest, exit_blob.value(), "code-exit");
    CHECK_RESULT(exit_code_blob);
    if (!code.ok() || !exit_code_blob.ok()) {
        return;
    }

    auto out = guest.mapMemory(0x100, PermReadWrite, "native-out");
    CHECK_RESULT(out);
    if (!out.ok()) {
        return;
    }

    std::array<uint64_t, 8> args{};
    args[0] = out.value().value();
    args[1] = thunk.value().value();

    auto call = guest.callFunction(code.value(), args);
    CHECK_RESULT(call);
    if (!call.ok()) {
        return;
    }
    CHECK(!call.value().exit_code.has_value());

    NativeOutput output{};
    CHECK_RESULT(guest.readMemory(out.value(), &output, sizeof(output)));

    std::printf(
        "  pid=%llu tid=%llu mmap=0x%llx loaded=0x%llx mprotect=%llu "
        "host=%llu write=%llu munmap=%llu\n",
        static_cast<unsigned long long>(output.pid),
        static_cast<unsigned long long>(output.tid),
        static_cast<unsigned long long>(output.mmap_address),
        static_cast<unsigned long long>(output.loaded),
        static_cast<unsigned long long>(output.mprotect_return),
        static_cast<unsigned long long>(output.host_return),
        static_cast<unsigned long long>(output.write_return),
        static_cast<unsigned long long>(output.munmap_return));

    CHECK_EQ(output.pid, kPid);
    CHECK_EQ(output.tid, kTid);
    CHECK(output.mmap_address != 0);
    CHECK_EQ(output.mmap_address % kGuestPageSize, 0);
    CHECK_EQ(output.loaded, 0x1234);
    CHECK_EQ(output.mprotect_return, 0);
    CHECK_EQ(output.host_return, kPid + kTid);
    CHECK_EQ(output.write_return, 3);
    CHECK_EQ(output.munmap_return, 0);

    const uint64_t expected = output.loaded + output.host_return +
                              output.write_return + output.munmap_return +
                              output.mprotect_return;
    CHECK_EQ(call.value().return_value, expected);
    CHECK_EQ(expected, 0x1234 + kPid + kTid + 3);

    auto exit_call = guest.callFunction(exit_code_blob.value());
    CHECK_RESULT(exit_call);
    if (!exit_call.ok()) {
        return;
    }
    CHECK(exit_call.value().exit_code.has_value());
    if (exit_call.value().exit_code.has_value()) {
        CHECK_EQ(*exit_call.value().exit_code, 7);
    }
}

} // namespace

int main() {
    runPureNative();
    return test::finish("02_pure_native");
}
