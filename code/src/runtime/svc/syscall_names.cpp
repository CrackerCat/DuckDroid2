#include "runtime/svc/syscall_names.hpp"

namespace duckemu::runtime::svc
{
namespace
{

struct Entry
{
	uint32_t number;
	const char *name;
};

// Generated from the NDK's aarch64-linux-android/asm/unistd_64.h and copied
// verbatim from the existing QBDI trace tooling:
//   /home/duck/NewCode/Android/Trace/code/QDBI/trace/syscall_names.inc
// Sorted by number; do not edit by hand.
constexpr Entry kSyscallNames[] = {
#include "runtime/svc/syscall_names.inc"
};

constexpr size_t kSyscallNameCount = sizeof(kSyscallNames) / sizeof(kSyscallNames[0]);

} // namespace

std::string_view syscallName(uint32_t number) noexcept
{
	size_t low = 0;
	size_t high = kSyscallNameCount;
	while (low < high)
	{
		const size_t middle = low + (high - low) / 2;
		if (kSyscallNames[middle].number < number)
		{
			low = middle + 1;
		}
		else
		{
			high = middle;
		}
	}
	if (low < kSyscallNameCount && kSyscallNames[low].number == number)
	{
		return kSyscallNames[low].name;
	}
	return {};
}

} // namespace duckemu::runtime::svc
