// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>

// Bionic arm64 TLS layout, copied from source rather than guessed.
//
// Sources (Android 13 / Android 14 share this field set):
//   bionic/libc/platform/bionic/tls_defines.h
//   bionic/libc/private/bionic_tls.h
//   bionic/libc/bionic/pthread_internal.h
//   bionic/libc/bionic/__errno.cpp
//   bionic/libc/platform/bionic/tls.h
//
// Important: arm64 has NO TLS_SLOT_SELF. The thread pointer (tpidr_el0)
// points at slot 0, which is the DTV, not at itself. `__get_thread()` reads
// slot 1 and `errno` lives inside pthread_internal_t.
namespace duckemu::runtime::tls::android13
{

inline constexpr uint64_t kPointerSize = 8;

// bionic_tcb: raw_slots_storage[BIONIC_TLS_SLOTS], BIONIC_TLS_SLOTS = 10.
inline constexpr int kMinTlsSlot = -2;
inline constexpr int kMaxTlsSlot = 7;
inline constexpr size_t kTcbSize = static_cast<size_t>(kMaxTlsSlot - kMinTlsSlot + 1) * kPointerSize;

// TP = tcb_base + 16, because slot 0 sits after slots -2 and -1.
inline constexpr uint64_t kThreadPointerOffset = 16;

inline constexpr int kSlotNativeBridgeGuestState = -2;
inline constexpr int kSlotBionicTls = -1;
inline constexpr int kSlotDtv = 0;
inline constexpr int kSlotThreadId = 1;
inline constexpr int kSlotApp = 2; // historically errno (Android P and earlier)
inline constexpr int kSlotOpenGL = 3;
inline constexpr int kSlotOpenGLApi = 4;
inline constexpr int kSlotStackGuard = 5;
inline constexpr int kSlotSanitizer = 6;
inline constexpr int kSlotArtThreadSelf = 7;

// pthread_internal_t offsets (LP64). Verified by mirroring the field list with
// equivalent host types; see docs/devlogs/2026-10-28-04-M1-TLS布局与门禁修正.md.
inline constexpr size_t kThreadNext = 0;
inline constexpr size_t kThreadPrev = 8;
inline constexpr size_t kThreadTid = 16;
inline constexpr size_t kThreadCachedPid = 20;
inline constexpr size_t kThreadAttr = 24;
inline constexpr size_t kThreadJoinState = 80;
inline constexpr size_t kThreadCleanupStack = 88;
inline constexpr size_t kThreadStartRoutine = 96;
inline constexpr size_t kThreadStartMask = 120;
inline constexpr size_t kThreadShadowCallStack = 136;
inline constexpr size_t kThreadStackTop = 144;
inline constexpr size_t kThreadTerminating = 152;
inline constexpr size_t kThreadMmapBase = 168;
inline constexpr size_t kThreadVmaName = 200;
inline constexpr size_t kThreadLocalDtors = 232;
inline constexpr size_t kThreadCurrentDlerror = 240;
inline constexpr size_t kThreadDlerrorBuffer = 248;
inline constexpr size_t kThreadBionicTls = 760;
inline constexpr size_t kThreadErrno = 768;
inline constexpr size_t kThreadSize = 776;

} // namespace duckemu::runtime::tls::android13
