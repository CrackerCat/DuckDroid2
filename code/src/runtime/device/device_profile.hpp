// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <string>

namespace duckemu::runtime::device
{

// Everything a target can observe about "the device" in one place: properties,
// uname, /proc synthesis, stat ownership and the auxiliary vector. The
// defaults describe a generic modern phone; a target pack overrides the fields
// it cares about (values can be sampled from a real device with adb).
//
// Settings.Secure/android_id lives on the Java side and is fed by the target's
// environment pack; this struct is the native half of the same picture.
struct DeviceProfile
{
	// Product identity.
	std::string model = "duckemu";
	std::string brand = "Duck";
	std::string manufacturer = "DuckDroid";
	std::string device = "duckemu";
	std::string fingerprint = "Duck/duckemu/duckemu:13/TQ3A.230805.001/1:user/release-keys";
	std::string serial = "duckemu0000000000";
	std::string hardware = "duckemu";
	std::string abi = "arm64-v8a";
	uint32_t sdk = 33;
	std::string release = "13";
	std::string incremental = "1";
	std::string build_type = "user";
	bool debuggable = false;

	// uname(2).
	std::string sysname = "Linux";
	std::string nodename = "localhost";
	std::string kernel_release = "6.1.0-duckemu";
	std::string kernel_version = "#1 SMP PREEMPT";
	std::string machine = "aarch64";

	// /proc/cpuinfo (one entry per core).
	std::string cpu_implementer = "0x41"; // ARM
	std::string cpu_architecture = "8";
	std::string cpu_part = "0xd0c";
	uint32_t cpu_revision = 1;
	uint32_t cpu_count = 8;
	std::string cpu_features =
		"fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm "
		"jscvt fcma lrcpc dcpop sha3 sm3 sm4 asimddp sha512";

	// Identity and ownership seen by syscalls and stat(2).
	uint32_t uid = 1000;
	uint32_t gid = 1000;
	// Process name: /proc/self/status Name, cmdline, prctl(PR_GET_NAME).
	std::string process_name = "duckemu";
	std::string android_id = "0000000000000000";

	// /proc/self/exe. App processes are forked from the zygote, so this is the
	// app_process binary rather than anything this runtime loaded.
	std::string executable_path = "/system/bin/app_process64";

	// /data storage picture (statfs): Android formats it f2fs or ext4. Install
	// and update flows probe the byte counts, so they are part of the profile.
	uint32_t data_file_system_type = 0xF2F52010; // F2FS_SUPER_MAGIC
	uint64_t data_total_bytes = 64ull * 1024 * 1024 * 1024;
	uint64_t data_free_bytes = 32ull * 1024 * 1024 * 1024;

	// Display, for the Java side and target packs.
	uint32_t display_width = 1080;
	uint32_t display_height = 2240;
	uint32_t display_density = 420;

	// AT_HWCAP / AT_HWCAP2. Zero is the baseline this backend truly
	// implements; a profile may raise bits the target probes for.
	uint64_t hwcap = 0;
	uint64_t hwcap2 = 0;
};

} // namespace duckemu::runtime::device
