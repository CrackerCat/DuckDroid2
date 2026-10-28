#pragma once

#include "common/error/result.hpp"
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

// Eager binding only: no lazy PLT, no symbol versions, no IFUNC. Every
// relocation is written straight into guest memory, so code pages go through
// the usual RuntimeMemory invalidation path.
class RelocApplier
{
    public:
	RelocApplier(RuntimeMemory &memory, const SymbolResolver &resolver, observer::EventBus *events);

	Result<void> apply(Module &module, const ElfImage &image);

    private:
	Result<void> applyOne(Module &module, const ElfRelocation &relocation);

	RuntimeMemory &memory_;
	const SymbolResolver &resolver_;
	observer::EventBus *events_;
};

} // namespace duckemu::runtime::loader
