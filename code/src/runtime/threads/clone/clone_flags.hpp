#pragma once

#include <cstdint>

// Linux clone(2) flag bits used by bionic's pthread_create. Kept as named
// constants instead of <sched.h> so the guest ABI is visible in one place.
namespace duckemu::runtime::threads::clone_flags
{

inline constexpr uint64_t kCloneVm = 0x00000100;
inline constexpr uint64_t kCloneFs = 0x00000200;
inline constexpr uint64_t kCloneFiles = 0x00000400;
inline constexpr uint64_t kCloneSighand = 0x00000800;
inline constexpr uint64_t kCloneThread = 0x00010000;
inline constexpr uint64_t kCloneSysVsem = 0x00040000;
inline constexpr uint64_t kCloneSetTls = 0x00080000;
inline constexpr uint64_t kCloneParentSetTid = 0x00100000;
inline constexpr uint64_t kCloneChildClearTid = 0x00200000;
inline constexpr uint64_t kCloneChildSetTid = 0x01000000;

} // namespace duckemu::runtime::threads::clone_flags
