// SPDX-License-Identifier: GPL-2.0-only
// M8 gate: address hooks.
//
// hook_entry (08_hook.S) stores x1 at [x0], loads it back, adds one and
// returns it. Hooks exercise register writes, memory reads/writes,
// forceWriteMemory, setPc, hit_count reset and the callFunction re-entry
// guard.

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "runtime/instance/runtime_instance.hpp"
#include "runtime/memory/region.hpp"
#include "runtime/observer/hook/hook_context.hpp"
#include "support/blob_loader.hpp"
#include "support/test_harness.hpp"

using namespace duckemu;
using namespace duckemu::runtime;

namespace {

constexpr uint64_t kMovX9 = 0x00;
constexpr uint64_t kLdr = 0x0c;
constexpr uint64_t kAdd = 0x10;
constexpr uint64_t kMovX0 = 0x14;

Result<uint64_t> runWithHook(RuntimeInstance& guest,
                             GuestVA entry,
                             const std::array<uint64_t, 8>& args,
                             uint64_t hook_offset,
                             observer::AddressHook hook) {
    auto id = guest.addAddressHook(GuestVA{entry.value() + hook_offset}, std::move(hook));
    if (!id.ok()) {
        return id.error();
    }
    auto call = guest.callFunction(entry, args);
    (void)guest.removeAddressHook(id.value());
    if (!call.ok()) {
        return call.error();
    }
    return call.value().return_value;
}

} // namespace

int main() {
    RuntimeConfig config;
    config.tid = 1;
    auto created = RuntimeInstance::create(config);
    CHECK_RESULT(created);
    if (!created.ok()) {
        return test::finish("08_address_hook");
    }
    RuntimeInstance& guest = *created.value();

    auto blob_bytes = test::loadBlob("08_hook.bin");
    CHECK_RESULT(blob_bytes);
    if (!blob_bytes.ok()) {
        return test::finish("08_address_hook");
    }
    auto blob = guest.mapMemory(blob_bytes.value().size(), PermRead | PermExec, "hook-blob");
    CHECK_RESULT(blob);
    auto data = guest.mapMemory(0x1000, PermReadWrite, "hook-data");
    CHECK_RESULT(data);
    auto read_only = guest.mapMemory(0x1000, PermRead, "hook-ro");
    CHECK_RESULT(read_only);
    if (!blob.ok() || !data.ok() || !read_only.ok()) {
        return test::finish("08_address_hook");
    }
    CHECK_RESULT(guest.writeMemory(blob.value(), blob_bytes.value().data(), blob_bytes.value().size()));
    const uint64_t zero = 0;
    CHECK_RESULT(guest.writeMemory(data.value(), &zero, sizeof(zero)));
    const uint64_t ro_initial = 0x11;
    CHECK_RESULT(guest.writeMemory(read_only.value(), &ro_initial, sizeof(ro_initial)));

    std::array<uint64_t, 8> args{};
    args[0] = data.value().value();
    args[1] = 2;

    // 1. Read the stored value, fault the memory, and let the load see it.
    auto run_read_write = runWithHook(guest, blob.value(), args, kLdr,
                                      [&](observer::HookContext& ctx) {
        CHECK_EQ(ctx.tid(), 1);
        CHECK_EQ(ctx.hitCount(), 1);
        CHECK_EQ(ctx.address(), blob.value().value() + kLdr);
        CHECK_EQ(ctx.x(0), data.value().value());
        CHECK_EQ(ctx.readX(0), data.value().value());
        CHECK(ctx.sp() != 0);
        CHECK_EQ(ctx.readSp(), ctx.sp());
        CHECK(ctx.lr() != 0);
        CHECK_EQ(ctx.readX(30), ctx.lr());

        auto memory = ctx.readMemory(data.value().value(), 8);
        CHECK(memory.ok());
        if (memory.ok()) {
            uint64_t stored = 0;
            std::memcpy(&stored, memory.value().data(), sizeof(stored));
            CHECK_EQ(stored, 2);
        }
        uint64_t fault = 0x99;
        auto written = ctx.writeMemory(data.value().value(), &fault, sizeof(fault));
        CHECK(written.ok());
    });
    CHECK_RESULT(run_read_write);
    if (run_read_write.ok()) {
        CHECK_EQ(run_read_write.value(), 0x9a); // 0x99 + 1
    }

    // 2. Write a register before the add.
    auto run_register = runWithHook(guest, blob.value(), args, kAdd,
                                    [](observer::HookContext& ctx) {
        ctx.writeX(2, 0x40);
    });
    CHECK_RESULT(run_register);
    if (run_register.ok()) {
        CHECK_EQ(run_register.value(), 0x41);
    }

    // 3. Redirect PC past the add.
    auto run_redirect = runWithHook(guest, blob.value(), args, kAdd,
                                    [&](observer::HookContext& ctx) {
        CHECK_RESULT(ctx.setPc(blob.value().value() + kMovX0));
    });
    CHECK_RESULT(run_redirect);
    if (run_redirect.ok()) {
        CHECK_EQ(run_redirect.value(), 2); // add skipped
    }

    // 4. hit_count restarts at 1 on every top-level call.
    std::vector<uint64_t> hits;
    auto counting = guest.addAddressHook(GuestVA{blob.value().value() + kMovX9}, [&](observer::HookContext& ctx) {
        hits.push_back(ctx.hitCount());
    });
    CHECK_RESULT(counting);
    if (counting.ok()) {
        for (int i = 0; i < 2; ++i) {
            CHECK_RESULT(guest.callFunction(blob.value(), args));
        }
        CHECK(guest.removeAddressHook(counting.value()));
    }
    CHECK_EQ(hits.size(), 2);
    if (hits.size() == 2) {
        CHECK_EQ(hits[0], 1);
        CHECK_EQ(hits[1], 1);
    }

    // 5. callFunction from inside a hook is refused, not nested.
    std::string reentry_error;
    auto reentry = guest.addAddressHook(GuestVA{blob.value().value() + kMovX9}, [&](observer::HookContext&) {
        auto nested = guest.callFunction(blob.value(), args);
        if (!nested.ok()) {
            reentry_error = nested.error().message;
        }
    });
    CHECK_RESULT(reentry);
    if (reentry.ok()) {
        CHECK_RESULT(guest.callFunction(blob.value(), args));
        CHECK(guest.removeAddressHook(reentry.value()));
    }
    CHECK(!reentry_error.empty());

    // 6. writeMemory honours PermWrite; forceWriteMemory bypasses it.
    bool denied = false;
    bool forced = false;
    auto force = guest.addAddressHook(GuestVA{blob.value().value() + kMovX9}, [&](observer::HookContext& ctx) {
        uint64_t fault = 0x22;
        auto checked = ctx.writeMemory(read_only.value().value(), &fault, sizeof(fault));
        denied = !checked.ok();
        auto bypass = ctx.forceWriteMemory(read_only.value().value(), &fault, sizeof(fault));
        forced = bypass.ok();
    });
    CHECK_RESULT(force);
    if (force.ok()) {
        CHECK_RESULT(guest.callFunction(blob.value(), args));
        CHECK(guest.removeAddressHook(force.value()));
    }
    CHECK(denied);
    CHECK(forced);
    uint64_t forced_value = 0;
    CHECK_RESULT(guest.readMemory(read_only.value(), &forced_value, sizeof(forced_value)));
    CHECK_EQ(forced_value, 0x22);

    return test::finish("08_address_hook");
}
