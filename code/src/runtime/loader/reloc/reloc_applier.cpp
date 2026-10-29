// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/loader/reloc/reloc_applier.hpp"

#include <string>

#include "runtime/memory/address_space.hpp"
#include "runtime/observer/event_bus.hpp"

namespace duckemu::runtime::loader
{
namespace
{

// AArch64 ELF relocation types. Only the subset M4.5 needs: RELA arithmetic,
// the three TLS models and IFUNC.
constexpr uint32_t kRAarch64None = 0;
constexpr uint32_t kRAarch64Abs64 = 257;
constexpr uint32_t kRAarch64GlobDat = 1025;
constexpr uint32_t kRAarch64JumpSlot = 1026;
constexpr uint32_t kRAarch64Relative = 1027;
constexpr uint32_t kRAarch64TlsDtpmod64 = 1028;
constexpr uint32_t kRAarch64TlsDtprel64 = 1029;
constexpr uint32_t kRAarch64TlsTprel64 = 1030;
constexpr uint32_t kRAarch64TlsDesc = 1031;
constexpr uint32_t kRAarch64IRelative = 1032;

constexpr uint8_t kStbWeak = 2;
constexpr uint8_t kSttTls = 6;
constexpr uint8_t kSttGnuIfunc = 10;

// TLSDESC resolvers. AArch64's descriptor is {func, arg}; the resolver is
// called with x0 = &descriptor and returns the value the caller adds to TP.
//   static: return arg
//   weak:   return arg - TP (so "TP + result" == addend, normally 0)
constexpr uint32_t kTlsDescStatic[] = {
	0xF9400400u, // ldr x0, [x0, #8]
	0xD65F03C0u, // ret
};
constexpr uint32_t kTlsDescWeak[] = {
	0xF81F0FF3u, // str x19, [sp, #-16]!
	0xF9400413u, // ldr x19, [x0, #8]
	0xD53BD040u, // mrs x0, tpidr_el0
	0xCB000260u, // sub x0, x19, x0
	0xF84107F3u, // ldr x19, [sp], #16
	0xD65F03C0u, // ret
};
constexpr uint64_t kTlsDescWeakOffset = 0x20;

} // namespace

RelocApplier::RelocApplier(RuntimeMemory &memory,
	const SymbolResolver &resolver,
	observer::EventBus *events,
	std::vector<IfuncFixup> &ifunc_fixups)
	: memory_(memory)
	, resolver_(resolver)
	, events_(events)
	, ifunc_fixups_(ifunc_fixups)
{
}

Result<void> RelocApplier::apply(Module &module, const ElfImage &image)
{
	for (const ElfRelocation &relocation : image.relocations())
	{
		if (auto result = applyOne(module, relocation); !result.ok())
		{
			return result.error();
		}
	}
	for (const ElfRelocation &relocation : image.pltRelocations())
	{
		if (auto result = applyOne(module, relocation); !result.ok())
		{
			return result.error();
		}
	}
	return {};
}

Result<void> RelocApplier::applyOne(Module &module, const ElfRelocation &relocation)
{
	if (relocation.type == kRAarch64None)
	{
		return {};
	}

	const ElfSymbol *symbol = relocation.symbol < module.symbols.size() ?
						&module.symbols[relocation.symbol] :
						nullptr;
	const GuestVA place = GuestVA{ module.base + relocation.offset };

	auto writeValue = [&](uint64_t value) -> Result<void> {
		auto written = memory_.write(place, &value, sizeof(value));
		if (!written.ok())
		{
			return Error{ written.error().code,
				module.soname + " +0x" + std::to_string(relocation.offset) +
					(symbol != nullptr && !symbol->name.empty() ? " ('" + symbol->name + "')" :
										       std::string()) +
					": " + written.error().message };
		}
		return {};
	};

	switch (relocation.type)
	{
	case kRAarch64Relative:
	{
		const uint64_t value = module.base + static_cast<uint64_t>(relocation.addend);
		return writeValue(value);
	}
	case kRAarch64Abs64:
	case kRAarch64GlobDat:
	case kRAarch64JumpSlot:
	{
		if (symbol == nullptr)
		{
			return Error{ ErrorCode::InvalidArgument,
				module.soname + ": relocation has no symbol" };
		}

		GuestVA target;
		bool is_ifunc = false;
		if (relocation.symbol == 0)
		{
			// The null symbol means "no symbol": bionic resolves it to 0 and
			// still adds the addend for ABS64.
			target = GuestVA{};
		}
		else if (!symbol->undefined())
		{
			target = GuestVA{ module.base + symbol->value };
			is_ifunc = symbol->type() == kSttGnuIfunc;
		}
		else
		{
			const bool weak = symbol->bind() == kStbWeak;
			auto resolved = resolver_.resolve(module, symbol->name, weak);
			if (!resolved.ok())
			{
				if (events_ != nullptr)
				{
					observer::ModuleEvent event;
					event.kind = observer::ModuleEventKind::Resolve;
					event.soname = module.soname;
					event.base = module.base;
					event.ok = false;
					event.detail = symbol->name + " (" + resolved.error().message + ")";
					events_->publishModule(std::move(event));
				}
				return Error{ resolved.error().code, resolved.error().message };
			}

			target = resolved.value().address;
			is_ifunc = resolved.value().type == kSttGnuIfunc;

			if (resolved.value().from_virtual && events_ != nullptr)
			{
				observer::ModuleEvent event;
				event.kind = observer::ModuleEventKind::Resolve;
				event.soname = module.soname;
				event.base = module.base;
				event.ok = true;
				event.detail = symbol->name + " -> virtual";
				events_->publishModule(std::move(event));
			}
		}

		// IFUNC resolvers run in the guest, which the loader cannot do while
		// holding the execution lock. Defer: write a placeholder now and let the
		// facade patch the slot after releasing the lock.
		if (is_ifunc)
		{
			ifunc_fixups_.push_back(IfuncFixup{ place, target });
			return writeValue(0);
		}

		uint64_t value = target.value();
		if (relocation.type == kRAarch64Abs64)
		{
			value += static_cast<uint64_t>(relocation.addend);
		}
		return writeValue(value);
	}
	case kRAarch64IRelative:
	{
		const GuestVA resolver = GuestVA{ module.base + static_cast<uint64_t>(relocation.addend) };
		ifunc_fixups_.push_back(IfuncFixup{ place, resolver });
		return writeValue(0);
	}
	case kRAarch64TlsTprel64:
	case kRAarch64TlsDtpmod64:
	case kRAarch64TlsDtprel64:
	case kRAarch64TlsDesc:
	{
		const bool weak_ref = symbol != nullptr && symbol->bind() == kStbWeak;
		auto tls = resolveTls(module, relocation.symbol, symbol, weak_ref);
		if (!tls.ok())
		{
			return tls.error();
		}
		const TlsReference &reference = tls.value();

		if (relocation.type == kRAarch64TlsTprel64)
		{
			if (reference.weak)
			{
				return writeValue(0);
			}
			const int64_t tpoff = reference.owner->tls_offset +
					      static_cast<int64_t>(reference.value) + relocation.addend;
			return writeValue(static_cast<uint64_t>(tpoff));
		}
		if (relocation.type == kRAarch64TlsDtpmod64)
		{
			return writeValue(reference.weak ? 0 : reference.owner->tls_module_id);
		}
		if (relocation.type == kRAarch64TlsDtprel64)
		{
			const uint64_t value = reference.weak ? 0 :
							    reference.value +
								    static_cast<uint64_t>(relocation.addend);
			return writeValue(value);
		}

		// TLSDESC: {func, arg}. ARM64 static TLS only; every PT_TLS module gets
		// a static slot in M4.5 (see the design doc §4.2).
		if (auto stubs = ensureTlsDescStubs(); !stubs.ok())
		{
			return stubs.error();
		}
		uint64_t descriptor[2] = {};
		if (reference.weak)
		{
			descriptor[0] = tlsdesc_weak_.value();
			descriptor[1] = static_cast<uint64_t>(relocation.addend);
		}
		else
		{
			descriptor[0] = tlsdesc_static_.value();
			descriptor[1] = static_cast<uint64_t>(reference.owner->tls_offset +
				static_cast<int64_t>(reference.value) + relocation.addend);
		}
		auto written = memory_.write(place, descriptor, sizeof(descriptor));
		if (!written.ok())
		{
			return Error{ written.error().code,
				module.soname + " +0x" + std::to_string(relocation.offset) +
					": " + written.error().message };
		}
		return {};
	}
	default:
		return Error{ ErrorCode::Unsupported,
			module.soname + ": unsupported relocation type " +
				std::to_string(relocation.type) + " at +0x" +
				std::to_string(relocation.offset) +
				(symbol != nullptr && !symbol->name.empty() ? " (" + symbol->name + ")" : "") };
	}
}

Result<RelocApplier::TlsReference> RelocApplier::resolveTls(Module &module,
	uint32_t symbol_index,
	const ElfSymbol *symbol,
	bool weak_reference) const
{
	if (symbol_index == 0)
	{
		// Symbol 0 means "the current module" for TLS relocations.
		if (!module.has_tls)
		{
			return Error{ ErrorCode::InvalidArgument,
				module.soname + ": TLS relocation in a module without PT_TLS" };
		}
		return TlsReference{ &module, 0, false };
	}

	if (symbol == nullptr)
	{
		return Error{ ErrorCode::InvalidArgument,
			module.soname + ": TLS relocation references an invalid symbol" };
	}

	if (!symbol->undefined())
	{
		if (symbol->type() != kSttTls)
		{
			return Error{ ErrorCode::InvalidArgument,
				module.soname + ": TLS relocation references non-TLS symbol '" +
					symbol->name + "'" };
		}
		if (!module.has_tls)
		{
			return Error{ ErrorCode::InvalidArgument,
				module.soname + ": TLS symbol '" + symbol->name +
					"' is defined in a module without PT_TLS" };
		}
		return TlsReference{ &module, symbol->value, false };
	}

	auto resolved = resolver_.resolveTls(module, symbol->name, weak_reference);
	if (!resolved.ok())
	{
		return resolved.error();
	}
	return TlsReference{ resolved.value().owner, resolved.value().value, resolved.value().weak };
}

Result<void> RelocApplier::ensureTlsDescStubs()
{
	if (tlsdesc_ready_)
	{
		return {};
	}

	auto page = memory_.map(kGuestPageSize, PermRead | PermExec, "[loader] tlsdesc");
	if (!page.ok())
	{
		return page.error();
	}
	if (auto written = memory_.write(page.value(), kTlsDescStatic, sizeof(kTlsDescStatic));
		!written.ok())
	{
		return written.error();
	}
	if (auto written = memory_.write(GuestVA{ page.value().value() + kTlsDescWeakOffset },
		    kTlsDescWeak,
		    sizeof(kTlsDescWeak));
		!written.ok())
	{
		return written.error();
	}

	tlsdesc_static_ = page.value();
	tlsdesc_weak_ = GuestVA{ page.value().value() + kTlsDescWeakOffset };
	tlsdesc_ready_ = true;
	return {};
}

} // namespace duckemu::runtime::loader
