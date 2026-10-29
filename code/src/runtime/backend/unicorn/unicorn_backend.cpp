// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/backend/unicorn/unicorn_backend.hpp"

#include <optional>
#include <utility>

#include "runtime/memory/address_space.hpp"

namespace duckemu::runtime
{

UnicornBackend::UnicornBackend(RuntimeMemory &memory)
	: memory_(memory)
{
}

UnicornBackend::~UnicornBackend() = default;

Result<std::unique_ptr<UnicornBackend> > UnicornBackend::create(RuntimeMemory &memory)
{
	return std::unique_ptr<UnicornBackend>(new UnicornBackend(memory));
}

Result<std::unique_ptr<UnicornEngine> > UnicornBackend::createEngine(UnicornEngine::SvcTrapHandler handler,
	UnicornEngine::DiagnosticSink sink)
{
	auto created = UnicornEngine::create();
	if (!created.ok())
	{
		return created.error();
	}
	std::unique_ptr<UnicornEngine> engine = std::move(created).value();
	engine->setSvcTrapHandler(std::move(handler));
	engine->setDiagnosticSink(std::move(sink));
	engine->setCodeWriteSink([this](GuestVA address, uint64_t size) {
		onGuestCodeWrite(address, size);
	});

	// Replay the current address space, then register the engine while still
	// holding the lock so a concurrent map cannot slip in between.
	std::lock_guard<std::mutex> lock(mutex_);
	std::optional<Error> failed;
	memory_.forEachRegion([&](const Region &region) {
		if (failed.has_value())
		{
			return;
		}
		auto mapped = engine->mapRegion(region);
		if (!mapped.ok())
		{
			failed = mapped.error();
		}
	});
	if (failed.has_value())
	{
		return failed.value();
	}
	engines_.push_back(engine.get());
	return engine;
}

void UnicornBackend::destroyEngine(UnicornEngine *engine)
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto it = engines_.begin(); it != engines_.end(); ++it)
	{
		if (*it == engine)
		{
			engines_.erase(it);
			return;
		}
	}
}

Result<void> UnicornBackend::onMapped(const Region &region)
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::optional<Error> failed;
	for (UnicornEngine *engine : engines_)
	{
		auto mapped = engine->mapRegion(region);
		if (!mapped.ok())
		{
			failed = mapped.error();
		}
	}
	if (failed.has_value())
	{
		return failed.value();
	}
	return {};
}

Result<void> UnicornBackend::onUnmapped(GuestVA address, uint64_t size)
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::optional<Error> failed;
	for (UnicornEngine *engine : engines_)
	{
		auto unmapped = engine->unmapRegion(address, size);
		if (!unmapped.ok())
		{
			failed = unmapped.error();
		}
	}
	if (failed.has_value())
	{
		return failed.value();
	}
	return {};
}

Result<void> UnicornBackend::onProtected(GuestVA address, uint64_t size, uint32_t perms)
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::optional<Error> failed;
	for (UnicornEngine *engine : engines_)
	{
		auto protected_ = engine->protectRegion(address, size, perms);
		if (!protected_.ok())
		{
			failed = protected_.error();
		}
	}
	if (failed.has_value())
	{
		return failed.value();
	}
	return {};
}

void UnicornBackend::onCodeWritten(GuestVA address, uint64_t size)
{
	broadcastInvalidate(address, size);
}

size_t UnicornBackend::engineCount() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return engines_.size();
}

void UnicornBackend::onGuestCodeWrite(GuestVA address, uint64_t size)
{
	if (size == 0)
	{
		return;
	}
	const uint64_t first_page = pageDown(address.value());
	const uint64_t last_page = pageDown(address.value() + size - 1);
	for (uint64_t page = first_page;; page += kGuestPageSize)
	{
		if (memory_.isExecutable(GuestVA{ page }))
		{
			broadcastInvalidate(GuestVA{ page }, kGuestPageSize);
		}
		if (page >= last_page)
		{
			break;
		}
	}
}

void UnicornBackend::broadcastInvalidate(GuestVA address, uint64_t size)
{
	if (size == 0)
	{
		return;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	for (UnicornEngine *engine : engines_)
	{
		engine->invalidateCode(address, size);
	}
}

} // namespace duckemu::runtime
