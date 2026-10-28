#pragma once

#include <cstdint>

#include "common/id/ids.hpp"

namespace duckemu::runtime::threads
{

class RuntimeThread;

// clone(2) arguments after the dispatcher has read x0..x4. The arm64 kernel
// order differs from x86: (flags, stack, parent_tid, child_tid, tls).
struct CloneRequest
{
	uint64_t flags = 0;
	GuestVA stack_top;
	GuestVA parent_tid;
	GuestVA child_tid;
	GuestVA tls;
};

// The part of the scheduler the SVC handlers are allowed to see. Handlers run
// inside the dispatcher, under the instance execution lock.
class ThreadSystem
{
    public:
	virtual ~ThreadSystem() = default;

	// Returns the new tid in the parent, or a negative errno.
	virtual int64_t cloneThread(RuntimeThread &parent, const CloneRequest &request) = 0;

	// Returns 0 / -EAGAIN / -ENOSYS / -EFAULT. Blocks the thread on success by
	// setting its state and letting the dispatcher stop the engine.
	virtual int64_t futexWait(RuntimeThread &thread, GuestVA uaddr, uint32_t expected, uint64_t timeout_address) = 0;
	virtual int64_t futexWake(GuestVA uaddr, int32_t count) = 0;

	virtual void exitThread(RuntimeThread &thread, int32_t code) = 0;
	virtual void exitGroup(RuntimeThread &thread, int32_t code) = 0;
	virtual void setClearChildTid(RuntimeThread &thread, GuestVA address) = 0;

	[[nodiscard]] virtual bool stopping() const = 0;
};

} // namespace duckemu::runtime::threads
