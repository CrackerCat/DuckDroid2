#pragma once

#include <cstdint>

#include "common/error/result.hpp"
#include "common/id/ids.hpp"

namespace duckemu::runtime
{
class RuntimeMemory;
}

namespace duckemu::runtime::tls
{

struct TlsInstance
{
	GuestVA region_base;
	GuestVA thread_pointer;
	GuestVA dtv;
	GuestVA pthread_internal;
	GuestVA bionic_tls;
	uint64_t stack_guard = 0;
};

// Allocates and initializes one bionic arm64 TLS block per guest thread.
// M1 has a single thread; M3 will create one instance per RuntimeThread.
class TlsManager
{
    public:
	explicit TlsManager(RuntimeMemory &memory);

	Result<TlsInstance> create(uint32_t tid, uint32_t pid, uint64_t stack_top);

	[[nodiscard]] const TlsInstance &mainThread() const
	{
		return main_;
	}
	[[nodiscard]] GuestVA threadPointer() const
	{
		return main_.thread_pointer;
	}

	// __errno() is &__get_thread()->errno_value on arm64.
	Result<void> setErrno(int32_t value);

    private:
	static uint64_t makeStackGuard();

	RuntimeMemory &memory_;
	TlsInstance main_{};
};

} // namespace duckemu::runtime::tls
