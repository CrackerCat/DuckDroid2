// M1 gate: arm64 bionic TLS layout.
//
// On arm64 tpidr_el0 points at slot 0 = DTV; there is no TLS_SLOT_SELF.
// slot 1 is pthread_internal_t*, and errno lives inside that struct.

#include <array>
#include <cstdint>
#include <cstdio>

#include "runtime/instance/runtime_instance.hpp"
#include "runtime/memory/region.hpp"
#include "support/blob_loader.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

namespace {

struct TlsSnapshot {
    uint64_t thread_pointer;
    uint64_t dtv;
    uint64_t pthread_internal;
    uint32_t errno_value;
    uint32_t padding;
    uint64_t stack_guard;
    uint64_t bionic_tls;
};
static_assert(sizeof(TlsSnapshot) == 48, "guest writes 48 bytes");

void runTlsRead() {
    auto blob = test::loadBlob("01_tls_read.bin");
    CHECK_RESULT(blob);
    if (!blob.ok()) {
        return;
    }

    auto instance = RuntimeInstance::create();
    CHECK_RESULT(instance);
    if (!instance.ok()) {
        return;
    }
    RuntimeInstance& guest = *instance.value();

    auto code = guest.mapMemory(blob.value().size(), PermRead | PermExec, "code");
    CHECK_RESULT(code);
    if (!code.ok()) {
        return;
    }
    CHECK_RESULT(guest.writeMemory(code.value(), blob.value().data(),
                                   blob.value().size()));

    auto out = guest.mapMemory(0x100, PermReadWrite, "out");
    CHECK_RESULT(out);
    if (!out.ok()) {
        return;
    }

    std::array<uint64_t, 8> args{};
    args[0] = out.value().value();

    auto call = guest.callFunction(code.value(), args);
    CHECK_RESULT(call);
    if (!call.ok()) {
        return;
    }
    CHECK_EQ(call.value().return_value, 0);
    CHECK(!call.value().exit_code.has_value());

    TlsSnapshot snapshot{};
    CHECK_RESULT(guest.readMemory(out.value(), &snapshot, sizeof(snapshot)));

    std::printf(
        "  tp=0x%llx dtv=0x%llx thread=0x%llx errno=%u guard=0x%llx bionic_tls=0x%llx\n",
        static_cast<unsigned long long>(snapshot.thread_pointer),
        static_cast<unsigned long long>(snapshot.dtv),
        static_cast<unsigned long long>(snapshot.pthread_internal),
        snapshot.errno_value,
        static_cast<unsigned long long>(snapshot.stack_guard),
        static_cast<unsigned long long>(snapshot.bionic_tls));

    CHECK(snapshot.thread_pointer != 0);
    CHECK(snapshot.dtv != 0);
    CHECK(snapshot.pthread_internal != 0);
    CHECK(snapshot.bionic_tls != 0);
    CHECK(snapshot.stack_guard != 0);
    CHECK_EQ(snapshot.errno_value, 0);
    CHECK_EQ(snapshot.thread_pointer % 16, 0);
    CHECK(snapshot.dtv != snapshot.thread_pointer);
    CHECK(snapshot.pthread_internal != snapshot.thread_pointer);

    // DTV[0] is the generation counter and starts at zero.
    uint64_t dtv_generation = 0;
    CHECK_RESULT(
        guest.readMemory(GuestVA{snapshot.dtv}, &dtv_generation, sizeof(uint64_t)));
    CHECK_EQ(dtv_generation, 0);

    // pthread_internal_t::bionic_tls (offset 760) must match slot -1.
    uint64_t thread_bionic_tls = 0;
    CHECK_RESULT(guest.readMemory(GuestVA{snapshot.pthread_internal + 760},
                                  &thread_bionic_tls, sizeof(uint64_t)));
    CHECK_EQ(thread_bionic_tls, snapshot.bionic_tls);

    // pthread_internal_t::errno_value (offset 768) must match the guest's read.
    uint32_t thread_errno = 0;
    CHECK_RESULT(guest.readMemory(GuestVA{snapshot.pthread_internal + 768},
                                  &thread_errno, sizeof(uint32_t)));
    CHECK_EQ(thread_errno, snapshot.errno_value);
}

} // namespace

int main() {
    runTlsRead();
    return test::finish("01_tls_read");
}
