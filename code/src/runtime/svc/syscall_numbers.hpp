#pragma once

#include <cstdint>

// Android arm64 uses the generic syscall table (asm-generic/unistd.h).
// Only the numbers M1 actually handles are listed here.
namespace duckemu::runtime::svc {

inline constexpr uint32_t kSysOpenat = 56;
inline constexpr uint32_t kSysClose = 57;
inline constexpr uint32_t kSysRead = 63;
inline constexpr uint32_t kSysWrite = 64;
inline constexpr uint32_t kSysExit = 93;
inline constexpr uint32_t kSysExitGroup = 94;
inline constexpr uint32_t kSysGetpid = 172;
inline constexpr uint32_t kSysGettid = 178;
inline constexpr uint32_t kSysBrk = 214;
inline constexpr uint32_t kSysMunmap = 215;
inline constexpr uint32_t kSysMmap = 222;
inline constexpr uint32_t kSysMprotect = 226;

// Linux/bionic errno values (identical on host and guest).
inline constexpr int64_t kEperm = 1;
inline constexpr int64_t kEnoent = 2;
inline constexpr int64_t kEbadf = 9;
inline constexpr int64_t kEagain = 11;
inline constexpr int64_t kEnomem = 12;
inline constexpr int64_t kEacces = 13;
inline constexpr int64_t kEfault = 14;
inline constexpr int64_t kEbusy = 16;
inline constexpr int64_t kEexist = 17;
inline constexpr int64_t kEnodev = 19;
inline constexpr int64_t kEinval = 22;
inline constexpr int64_t kEnosys = 38;
inline constexpr int64_t kEio = 5;

inline constexpr int64_t kError(int64_t errno_value) { return -errno_value; }

} // namespace duckemu::runtime::svc
