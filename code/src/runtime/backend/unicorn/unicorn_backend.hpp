// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <memory>
#include <mutex>
#include <vector>

#include "common/error/result.hpp"
#include "runtime/backend/unicorn/unicorn_engine.hpp"
#include "runtime/memory/code_notifier.hpp"

namespace duckemu::runtime
{

class RuntimeMemory;

// Owns every engine of one RuntimeInstance and keeps their address-space view
// and translated-code caches in sync. Engines themselves are owned by their
// RuntimeThread; the backend only keeps non-owning pointers and is told when
// one is destroyed.
class UnicornBackend final : public MapObserver, public CodeWriteNotifier
{
    public:
	static Result<std::unique_ptr<UnicornBackend> > create(RuntimeMemory &memory);
	~UnicornBackend();

	UnicornBackend(const UnicornBackend &) = delete;
	UnicornBackend &operator=(const UnicornBackend &) = delete;
	UnicornBackend(UnicornBackend &&) = delete;
	UnicornBackend &operator=(UnicornBackend &&) = delete;

	// Creates an engine, mirrors every existing region into it and registers
	// it for future broadcasts. The caller must hold the instance execution
	// lock (or otherwise guarantee no engine is running).
	Result<std::unique_ptr<UnicornEngine> > createEngine(UnicornEngine::SvcTrapHandler handler,
		UnicornEngine::DiagnosticSink sink);

	// Must be called before the engine is destroyed.
	void destroyEngine(UnicornEngine *engine);

	// MapObserver: broadcast to every engine.
	Result<void> onMapped(const Region &region) override;
	Result<void> onUnmapped(GuestVA address, uint64_t size) override;
	Result<void> onProtected(GuestVA address, uint64_t size, uint32_t perms) override;

	// CodeWriteNotifier: invalidate translated code in every engine.
	void onCodeWritten(GuestVA address, uint64_t size) override;

	[[nodiscard]] size_t engineCount() const;

    private:
	explicit UnicornBackend(RuntimeMemory &memory);

	void broadcastInvalidate(GuestVA address, uint64_t size);
	// Guest-side SMC: pages written by guest code must drop stale TBs, but only
	// where the region is executable.
	void onGuestCodeWrite(GuestVA address, uint64_t size);

	RuntimeMemory &memory_;
	mutable std::mutex mutex_;
	std::vector<UnicornEngine *> engines_;
};

} // namespace duckemu::runtime
