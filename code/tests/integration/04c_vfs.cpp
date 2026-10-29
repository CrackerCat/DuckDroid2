// SPDX-License-Identifier: GPL-2.0-only
// M4.6 S3: VFS + /proc. The ROM libc runs its preinit through the VFS (the
// last three unsupported syscalls were openat/faccessat/newfstatat); this gate
// then drives the same syscalls from guest libc wrappers:
//   - open/read/close of generated /proc/self/{maps,status,auxv}
//   - stat/access, including the errno paths
//   - read-only file mmap from a mounted host root

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

constexpr uint64_t kO_RdOnly = 0;
constexpr uint64_t kO_WrOnly = 1;
constexpr uint64_t kProtRead = 1;
constexpr uint64_t kMapPrivate = 2;
constexpr uint64_t kF_OK = 0;
constexpr uint64_t kR_OK = 4;
constexpr uint64_t kInvalidResult = static_cast<uint64_t>(-1);

constexpr uint64_t kPathSlot = 0x100;
constexpr uint64_t kReadBufferSize = 0x2000;
constexpr uint64_t kStatSize = 128;
constexpr uint64_t kStatModeOffset = 16;
constexpr uint64_t kStatSizeOffset = 48;
constexpr uint32_t kModeTypeMask = 0170000;
constexpr uint32_t kModeRegular = 0100000;

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

// int-returning libc wrappers leave only w0 meaningful (-1 arrives as
// 0x00000000ffffffff); ssize_t-returning ones sign-extend. Both fail here.
bool failed(uint64_t value) {
    return static_cast<int32_t>(value) == -1;
}

} // namespace

int main() {
    RuntimeConfig config;
    config.pid = kPid;
    config.tid = kTid;
    config.library_search_paths = { DUCKEMU_ROM_BOOTSTRAP, DUCKEMU_ROM_LIB64 };
    config.filesystem_roots = { DUCKEMU_ROM_SYMBOLS };

    auto created = RuntimeInstance::create(config);
    CHECK_RESULT(created);
    if (!created.ok()) {
        return test::finish("04c_vfs");
    }
    RuntimeInstance& instance = *created.value();

    auto libc = instance.loadLibrary(romLibc());
    CHECK_RESULT(libc);
    if (!libc.ok()) {
        std::printf("  libc error: %s\n", libc.error().message.c_str());
        return test::finish("04c_vfs");
    }
    const uint64_t handle = libc.value();

    struct Functions {
        GuestVA open;
        GuestVA read;
        GuestVA close;
        GuestVA stat;
        GuestVA access;
        GuestVA mmap;
        GuestVA munmap;
    } functions{};

    const struct {
        const char* symbol;
        GuestVA* slot;
    } lookups[] = {
        { "open", &functions.open },
        { "read", &functions.read },
        { "close", &functions.close },
        { "stat", &functions.stat },
        { "access", &functions.access },
        { "mmap", &functions.mmap },
        { "munmap", &functions.munmap },
    };
    for (const auto& lookup : lookups) {
        auto found = instance.lookupSymbol(handle, lookup.symbol);
        CHECK_RESULT(found);
        if (!found.ok()) {
            return test::finish("04c_vfs");
        }
        *lookup.slot = found.value();
    }

    auto path_page = instance.mapMemory(kPathSlot * 8, PermReadWrite, "vfs-paths");
    auto buffer_page = instance.mapMemory(kReadBufferSize, PermReadWrite, "vfs-buffer");
    auto stat_page = instance.mapMemory(kStatSize, PermReadWrite, "vfs-stat");
    CHECK_RESULT(path_page);
    CHECK_RESULT(buffer_page);
    CHECK_RESULT(stat_page);
    if (!path_page.ok() || !buffer_page.ok() || !stat_page.ok()) {
        return test::finish("04c_vfs");
    }

    const uint64_t paths = path_page.value().value();
    const uint64_t buffer = buffer_page.value().value();
    const uint64_t stat_buffer = stat_page.value().value();

    uint64_t next_path = 0;
    const auto writePath = [&](const std::string& path) {
        const uint64_t address = paths + next_path;
        next_path += kPathSlot;
        CHECK_RESULT(instance.writeMemory(GuestVA{ address }, path.c_str(),
                                          path.size() + 1));
        return address;
    };

    const uint64_t maps_path = writePath("/proc/self/maps");
    const uint64_t status_path = writePath("/proc/self/status");
    const uint64_t auxv_path = writePath("/proc/self/auxv");
    const uint64_t missing_path = writePath("/proc/self/does-not-exist");
    const uint64_t libc_path = writePath("/system/lib64/bootstrap/libc.so");

    // 1. /proc/self/maps through open/read/close.
    const uint64_t maps_fd = callGuest(instance, functions.open,
                                       { maps_path, kO_RdOnly, 0 });
    std::printf("  open(/proc/self/maps)=%lld\n", static_cast<long long>(maps_fd));
    CHECK(maps_fd >= 3);
    if (maps_fd >= 3) {
        const uint64_t bytes = callGuest(instance, functions.read,
                                         { maps_fd, buffer, kReadBufferSize - 1 });
        CHECK(bytes > 0);
        std::array<char, kReadBufferSize> text{};
        if (bytes > 0 && bytes < text.size()) {
            readGuest(instance, buffer, text.data(), bytes);
            const std::string maps(text.data(), bytes);
            const std::string first_line = maps.substr(0, maps.find('\n'));
            std::printf("  maps first line: %s\n", first_line.c_str());
            CHECK(maps.find("libc.so") != std::string::npos);
            CHECK(maps.find("rw-p") != std::string::npos);
        }
        CHECK_EQ(callGuest(instance, functions.close, { maps_fd }), 0);
        // The descriptor is gone: a second read must fail with EBADF.
        CHECK(failed(callGuest(instance, functions.read, { maps_fd, buffer, 16 })));
    }

    // 2. stat / access.
    const uint64_t status_result = callGuest(instance, functions.stat,
                                             { status_path, stat_buffer });
    std::printf("  stat(/proc/self/status)=%lld\n", static_cast<long long>(status_result));
    CHECK_EQ(status_result, 0);
    if (status_result == 0) {
        uint32_t mode = 0;
        uint64_t size = 0;
        readGuest(instance, stat_buffer + kStatModeOffset, &mode, sizeof(mode));
        readGuest(instance, stat_buffer + kStatSizeOffset, &size, sizeof(size));
        std::printf("  status mode=0%o size=%llu\n", mode,
                    static_cast<unsigned long long>(size));
        CHECK_EQ(mode & kModeTypeMask, kModeRegular);
        CHECK(size > 0);
    }
    CHECK_EQ(callGuest(instance, functions.access, { maps_path, kR_OK }), 0);
    CHECK(failed(callGuest(instance, functions.access, { missing_path, kF_OK })));
    CHECK(failed(callGuest(instance, functions.stat, { missing_path, stat_buffer })));

    // 3. /proc/self/auxv: AT_PAGESZ must carry 0x1000.
    const uint64_t auxv_fd = callGuest(instance, functions.open,
                                       { auxv_path, kO_RdOnly, 0 });
    CHECK(auxv_fd >= 3);
    if (auxv_fd >= 3) {
        const uint64_t bytes = callGuest(instance, functions.read,
                                         { auxv_fd, buffer, 512 });
        CHECK(bytes >= 16);
        if (bytes >= 16) {
            uint64_t words[64] = {};
            const uint64_t count = bytes < sizeof(words) ? bytes : sizeof(words);
            readGuest(instance, buffer, words, count);
            bool pagesz_ok = false;
            for (uint64_t i = 0; i + 1 < count / 8; i += 2) {
                if (words[i] == 0) {
                    break;
                }
                if (words[i] == 6 /* AT_PAGESZ */) {
                    pagesz_ok = words[i + 1] == kGuestPageSize;
                }
            }
            CHECK(pagesz_ok);
        }
        CHECK_EQ(callGuest(instance, functions.close, { auxv_fd }), 0);
    }

    // 4. Read-only open + file mmap from the mounted root.
    const uint64_t libc_fd = callGuest(instance, functions.open,
                                       { libc_path, kO_RdOnly, 0 });
    std::printf("  open(/system/lib64/bootstrap/libc.so)=%lld\n",
                static_cast<long long>(libc_fd));
    CHECK(libc_fd >= 3);
    if (libc_fd >= 3) {
        const uint64_t mapped = callGuest(instance, functions.mmap,
                                          { 0, 0x1000, kProtRead, kMapPrivate, libc_fd, 0 });
        std::printf("  mmap(libc.so, 0x1000)=0x%llx\n",
                    static_cast<unsigned long long>(mapped));
        CHECK(mapped != 0 && mapped != kInvalidResult);
        if (mapped != 0 && mapped != kInvalidResult) {
            uint32_t magic = 0;
            readGuest(instance, mapped, &magic, sizeof(magic));
            CHECK_EQ(magic, 0x464c457fU); // "\x7fELF"
            CHECK_EQ(callGuest(instance, functions.munmap, { mapped, 0x1000 }), 0);
        }
        CHECK_EQ(callGuest(instance, functions.close, { libc_fd }), 0);
    }

    // 5. Write access is refused: the roots are read-only.
    CHECK(failed(callGuest(instance, functions.open, { maps_path, kO_WrOnly, 0 })));

    return test::finish("04c_vfs");
}
