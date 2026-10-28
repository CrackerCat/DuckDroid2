#include "runtime/loader/reloc/reloc_applier.hpp"

#include <string>

#include "runtime/memory/address_space.hpp"
#include "runtime/observer/event_bus.hpp"

namespace duckemu::runtime::loader
{
namespace
{

// AArch64 ELF relocation types used by M4.
constexpr uint32_t kRAarch64None = 0;
constexpr uint32_t kRAarch64Abs64 = 257;
constexpr uint32_t kRAarch64GlobDat = 1025;
constexpr uint32_t kRAarch64JumpSlot = 1026;
constexpr uint32_t kRAarch64Relative = 1027;
constexpr uint32_t kRAarch64TlsDtpmod64 = 1028;
constexpr uint32_t kRAarch64TlsDtpoff64 = 1029;
constexpr uint32_t kRAarch64TlsTprel64 = 1031;
constexpr uint32_t kRAarch64TlsDesc = 1032;

constexpr uint8_t kStbWeak = 2;

} // namespace

RelocApplier::RelocApplier(RuntimeMemory &memory, const SymbolResolver &resolver, observer::EventBus *events)
	: memory_(memory)
	, resolver_(resolver)
	, events_(events)
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

	switch (relocation.type)
	{
	case kRAarch64Relative:
	{
		const uint64_t value = module.base +
				       static_cast<uint64_t>(relocation.addend);
		return memory_.write(GuestVA{ module.base + relocation.offset }, &value, sizeof(value));
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

		uint64_t value = 0;
		if (!symbol->undefined())
		{
			value = module.base + symbol->value;
			if (relocation.type == kRAarch64Abs64)
			{
				value += static_cast<uint64_t>(relocation.addend);
			}
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

			value = resolved.value().address.value();
			if (relocation.type == kRAarch64Abs64)
			{
				value += static_cast<uint64_t>(relocation.addend);
			}

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

		auto written = memory_.write(GuestVA{ module.base + relocation.offset }, &value, sizeof(value));
		if (!written.ok())
		{
			return Error{ written.error().code,
				module.soname + " +0x" + std::to_string(relocation.offset) + " ('" +
					symbol->name + "'): " + written.error().message };
		}
		return {};
	}
	case kRAarch64TlsTprel64:
	case kRAarch64TlsDesc:
		return Error{ ErrorCode::Unsupported,
			module.soname + ": TLS relocations are not implemented yet (need " +
				(relocation.type == kRAarch64TlsDesc ? "TLSDESC descriptors" : "static TLS slots") +
				")" };
	case kRAarch64TlsDtpmod64:
	case kRAarch64TlsDtpoff64:
		return Error{ ErrorCode::Unsupported,
			module.soname + ": dynamic TLS (__tls_get_addr path) is not implemented yet" };
	default:
		return Error{ ErrorCode::Unsupported,
			module.soname + ": unsupported relocation type " +
				std::to_string(relocation.type) + " at +0x" +
				std::to_string(relocation.offset) +
				(symbol != nullptr && !symbol->name.empty() ? " (" + symbol->name + ")" : "") };
	}
}

} // namespace duckemu::runtime::loader
