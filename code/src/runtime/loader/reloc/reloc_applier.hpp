#pragma once

#include <cstdint>
#include <vector>

#include "common/error/result.hpp"
#include "common/id/ids.hpp"
#include "runtime/loader/elf/elf_image.hpp"
#include "runtime/loader/module.hpp"
#include "runtime/loader/symbol_lookup.hpp"

namespace duckemu::runtime
{
class RuntimeMemory;
namespace observer
{
class EventBus;
}
} // namespace duckemu::runtime

namespace duckemu::runtime::loader
{

// An IFUNC (or IRELATIVE) relocation whose final value needs a guest call.
// The applier writes a placeholder so the relocation pass never runs guest
// code while the loader holds the execution lock; the facade resolves the
// fixup after releasing it.
struct IfuncFixup
{
	GuestVA place;
	GuestVA resolver;
};

// Eager binding only: no lazy PLT, no symbol versions. Every relocation is
// written straight into guest memory, so code pages go through the usual
// RuntimeMemory invalidation path. IFUNC and IRELATIVE are queued instead of
// executed here (see IfuncFixup).
class RelocApplier
{
    public:
	RelocApplier(RuntimeMemory &memory,
		const SymbolResolver &resolver,
		observer::EventBus *events,
		std::vector<IfuncFixup> &ifunc_fixups);

	Result<void> apply(Module &module, const ElfImage &image);

    private:
	struct TlsReference
	{
		const Module *owner = nullptr;
		uint64_t value = 0;
		bool weak = false;
	};

	Result<void> applyOne(Module &module, const ElfRelocation &relocation);
	Result<TlsReference> resolveTls(Module &module,
		uint32_t symbol_index,
		const ElfSymbol *symbol,
		bool weak_reference) const;
	Result<void> ensureTlsDescStubs();

	RuntimeMemory &memory_;
	const SymbolResolver &resolver_;
	observer::EventBus *events_;
	std::vector<IfuncFixup> &ifunc_fixups_;

	GuestVA tlsdesc_static_{};
	GuestVA tlsdesc_weak_{};
	bool tlsdesc_ready_ = false;
};

} // namespace duckemu::runtime::loader
