#pragma once

#include "common/error/result.hpp"
#include "common/id/ids.hpp"
#include "runtime/memory/region.hpp"

namespace duckemu::runtime
{

// Implemented by the execution backend. The MemoryManager is the single
// authority over the guest address space; the backend only mirrors it into
// each engine.
class MapObserver
{
    public:
	virtual ~MapObserver() = default;

	virtual Result<void> onMapped(const Region &region) = 0;
	virtual Result<void> onUnmapped(GuestVA address, uint64_t size) = 0;
	virtual Result<void> onProtected(GuestVA address, uint64_t size, uint32_t perms) = 0;
};

// Implemented by the execution backend; called when the host writes an
// executable region (SMC). Guest writes are caught by the backend's memory
// hooks directly.
class CodeWriteNotifier
{
    public:
	virtual ~CodeWriteNotifier() = default;

	virtual void onCodeWritten(GuestVA address, uint64_t size) = 0;
};

} // namespace duckemu::runtime
