// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/loader/module_loader.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <utility>

#include <elf.h>

#include "common/log/logger.hpp"
#include "runtime/memory/address_space.hpp"
#include "runtime/memory/thunk/thunk_table.hpp"
#include "runtime/observer/event_bus.hpp"

namespace duckemu::runtime::loader
{
namespace
{

constexpr uint64_t kModuleAreaBase = 0x60000000;
constexpr uint64_t kModuleSlotSize = 0x1000000; // 16 MiB between module bases
constexpr uint64_t kMaxModuleAlign = 0x10000;
constexpr uint64_t kMaxSegments = 16;

uint32_t guestPermissions(uint32_t elf_flags)
{
	uint32_t perms = 0;
	if ((elf_flags & PF_R) != 0)
	{
		perms |= PermRead;
	}
	if ((elf_flags & PF_W) != 0)
	{
		perms |= PermWrite;
	}
	if ((elf_flags & PF_X) != 0)
	{
		perms |= PermExec;
	}
	return perms;
}

} // namespace

ModuleLoader::ModuleLoader(RuntimeMemory &memory,
	ThunkTable &thunks,
	observer::EventBus *events,
	std::vector<std::string> search_paths,
	bool enable_test_import_stubs)
	: memory_(memory)
	, events_(events)
	, vfs_(std::move(search_paths))
	, virtuals_(memory, thunks, enable_test_import_stubs)
	, resolver_(registry_, virtuals_)
	, relocations_(memory, resolver_, events, ifunc_fixups_)
	, next_base_(kModuleAreaBase)
{
}

Result<uint64_t> ModuleLoader::load(std::string_view path)
{
	return loadCanonical(std::string(path));
}

Result<uint64_t> ModuleLoader::loadByName(std::string_view name, std::string_view requester_directory)
{
	if (name.empty())
	{
		return Error{ ErrorCode::InvalidArgument, "empty library name" };
	}
	if (name.find('/') != std::string_view::npos)
	{
		return loadCanonical(std::string(name));
	}
	if (Module *existing = registry_.findBySoname(name))
	{
		return existing->handle;
	}
	auto resolved = vfs_.resolveModule(name, requester_directory);
	if (!resolved.ok())
	{
		return resolved.error();
	}
	return loadCanonical(resolved.value());
}

Result<uint64_t> ModuleLoader::loadCanonical(const std::string &path)
{
	std::error_code error;
	const std::filesystem::path canonical = std::filesystem::weakly_canonical(std::filesystem::path(path), error);
	const std::string resolved = error ? path : canonical.string();

	return loadResolvedPath(resolved);
}

Result<uint64_t> ModuleLoader::loadResolvedPath(const std::string &path)
{
	if (Module *existing = registry_.findByPath(path))
	{
		return existing->handle;
	}

	auto bytes = vfs_.readFile(path);
	if (!bytes.ok())
	{
		return bytes.error();
	}

	auto image = ElfImage::parse(std::move(bytes).value(), path);
	if (!image.ok())
	{
		return image.error();
	}

	const std::string soname = image.value().soname().empty() ?
					   std::filesystem::path(path).filename().string() :
					   std::string(image.value().soname());

	if (Module *existing = registry_.findBySoname(soname))
	{
		return existing->handle;
	}

	auto module = loadImage(path, std::move(image).value());
	if (!module.ok())
	{
		return module.error();
	}
	return module.value()->handle;
}

Result<Module *> ModuleLoader::loadImage(const std::string &path, ElfImage parsed)
{
	const ElfImage &elf = parsed;

	Module module;
	module.path = path;
	module.directory = std::filesystem::path(path).parent_path().string();
	module.soname = elf.soname().empty() ? std::filesystem::path(path).filename().string() :
					       std::string(elf.soname());

	for (const ElfSegment &segment : elf.segments())
	{
		module.align = std::max(module.align, std::min(segment.align, kMaxModuleAlign));
		module.span = std::max(module.span, alignUp(segment.vaddr + segment.memsz, kGuestPageSize));
	}
	if (module.align == 0)
	{
		module.align = kGuestPageSize;
	}

	auto base = allocateBase(module.span, module.align);
	if (!base.ok())
	{
		return base.error();
	}
	module.base = base.value();
	module.segments = elf.segments();
	module.needed = elf.needed();
	module.symbols = elf.symbols();
	module.tls = elf.tls();
	module.has_tls = elf.tls().present;

	if (module.has_tls && module.tls.memsz != 0)
	{
		auto offset = tls_layout_.reserve(module.tls.memsz, module.tls.align);
		if (!offset.ok())
		{
			return offset.error();
		}
		module.tls_offset = offset.value();
		module.tls_module_id = next_tls_module_id_++;
		module.tls_static = true;
	}

	if (auto mapped = mapSegments(module, elf); !mapped.ok())
	{
		return mapped.error();
	}

	Module &registered = registry_.add(std::make_unique<Module>(std::move(module)));

	if (events_ != nullptr)
	{
		observer::ModuleEvent event;
		event.kind = observer::ModuleEventKind::Load;
		event.soname = registered.soname;
		event.base = registered.base;
		event.detail = registered.path;
		events_->publishModule(std::move(event));
	}

	// M4.5 does not execute guest code, but the initial thread block must
	// already carry the module's TLS image for any later call.
	if (registered.has_tls && tls_sink_ && registered.tls.filesz != 0)
	{
		const ElfTlsTemplate &tls = registered.tls;
		if (tls.offset > elf.bytes().size() || tls.filesz > elf.bytes().size() - tls.offset)
		{
			// erase() destroys the Module: copy what the error message needs
			// before it is gone.
			const std::string soname = registered.soname;
			unmapSegments(registered);
			registry_.erase(registered.handle);
			return Error{ ErrorCode::InvalidArgument, soname + ": PT_TLS image is out of bounds" };
		}
		auto materialized = tls_sink_(registered.tls_offset,
			elf.bytes().data() + tls.offset,
			tls.filesz,
			tls.memsz);
		if (!materialized.ok())
		{
			unmapSegments(registered);
			registry_.erase(registered.handle);
			return materialized.error();
		}
	}

	for (const std::string &needed : registered.needed)
	{
		bool ok = false;
		std::string detail;
		if (virtuals_.isVirtualModule(needed))
		{
			auto virtual_module = loadVirtual(needed);
			ok = virtual_module.ok();
			if (!virtual_module.ok())
			{
				detail = virtual_module.error().message;
			}
		}
		else
		{
			auto resolved_path = vfs_.resolveModule(needed, registered.directory);
			if (!resolved_path.ok())
			{
				detail = resolved_path.error().message;
			}
			else
			{
				auto dependency = loadResolvedPath(resolved_path.value());
				ok = dependency.ok();
				if (!dependency.ok())
				{
					detail = dependency.error().message;
				}
			}
		}

		if (events_ != nullptr)
		{
			observer::ModuleEvent event;
			event.kind = observer::ModuleEventKind::Needed;
			event.soname = registered.soname;
			event.base = registered.base;
			event.ok = ok;
			event.detail = ok ? needed : needed + " (" + detail + ")";
			events_->publishModule(std::move(event));
		}

		if (!ok)
		{
			// `needed` is a reference into registered.needed: build the message
			// before erase() destroys the Module.
			const std::string message =
				registered.soname + ": cannot load dependency " + needed + ": " + detail;
			unmapSegments(registered);
			registry_.erase(registered.handle);
			return Error{ ErrorCode::NotFound, message };
		}
	}

	if (auto applied = relocations_.apply(registered, elf); !applied.ok())
	{
		unmapSegments(registered);
		registry_.erase(registered.handle);
		return applied.error();
	}

	// dl_iterate_phdr hands the guest the raw table (link-time entries, like
	// bionic). Kept outside the module's PT_LOAD range; a failure here is a
	// real allocation failure, not a load-time nicety.
	if (!elf.phdrBytes().empty())
	{
		const uint64_t phdr_size = alignUp(elf.phdrBytes().size(), kGuestPageSize);
		auto phdr = memory_.map(phdr_size, PermRead, "phdr:" + registered.soname);
		if (!phdr.ok())
		{
			unmapSegments(registered);
			registry_.erase(registered.handle);
			return phdr.error();
		}
		auto written = memory_.write(phdr.value(),
			elf.phdrBytes().data(),
			elf.phdrBytes().size());
		if (!written.ok())
		{
			memory_.unmap(phdr.value(), phdr_size);
			unmapSegments(registered);
			registry_.erase(registered.handle);
			return written.error();
		}
		registered.phdr = phdr.value();
		registered.phnum = elf.phnum();
	}

	registered.init_function = GuestVA{ elf.initFunction().has_value() ?
						    registered.base + *elf.initFunction() :
						    0 };
	registered.init_array = GuestVA{ elf.initArray().has_value() ? registered.base + *elf.initArray() : 0 };
	registered.init_array_size = elf.initArraySize();
	registered.state = ModuleState::Loaded;

	collectInitializers(registered);
	return &registered;
}

Result<Module *> ModuleLoader::loadVirtual(std::string_view soname)
{
	if (Module *existing = registry_.findBySoname(soname))
	{
		return existing;
	}

	auto module = std::make_unique<Module>();
	module->soname = std::string(soname);
	module->is_virtual = true;
	module->state = ModuleState::Loaded;
	Module &registered = registry_.add(std::move(module));

	if (events_ != nullptr)
	{
		observer::ModuleEvent event;
		event.kind = observer::ModuleEventKind::Load;
		event.soname = registered.soname;
		event.detail = "virtual";
		events_->publishModule(std::move(event));
	}
	return &registered;
}

Result<uint64_t> ModuleLoader::allocateBase(uint64_t span, uint64_t align)
{
	if (align == 0)
	{
		align = kGuestPageSize;
	}

	uint64_t base = alignUp(next_base_, align);
	for (int attempt = 0; attempt < 4096; ++attempt)
	{
		bool free = true;
		for (uint64_t address = base; address < base + span; address += kGuestPageSize)
		{
			if (memory_.isMapped(GuestVA{ address }))
			{
				free = false;
				break;
			}
		}
		if (free)
		{
			next_base_ = base + span;
			return base;
		}
		base = alignUp(base + kModuleSlotSize, align);
	}
	return Error{ ErrorCode::OutOfMemory, "no free guest range for a module" };
}

Result<void> ModuleLoader::mapSegments(Module &module, const ElfImage &image)
{
	std::vector<std::pair<uint64_t, uint64_t> > mapped;
	auto rollback = [&]() {
		for (const auto &[start, end] : mapped)
		{
			memory_.unmap(GuestVA{ start }, end - start);
		}
	};

	if (module.segments.size() > kMaxSegments)
	{
		return Error{ ErrorCode::InvalidArgument, module.soname + ": too many PT_LOAD segments" };
	}

	for (const ElfSegment &segment : module.segments)
	{
		if (segment.memsz == 0)
		{
			continue;
		}

		const uint64_t start = module.base + pageDown(segment.vaddr);
		const uint64_t end = module.base + pageUp(segment.vaddr + segment.memsz);
		if (end <= start)
		{
			rollback();
			return Error{ ErrorCode::InvalidArgument, module.soname + ": segment range is empty" };
		}
		for (const auto &[other_start, other_end] : mapped)
		{
			if (start < other_end && other_start < end)
			{
				rollback();
				return Error{ ErrorCode::InvalidArgument,
					module.soname + ": PT_LOAD segments overlap after page rounding" };
			}
		}

		auto result = memory_.mapFixed(GuestVA{ start },
			end - start,
			guestPermissions(segment.flags),
			"module:" + module.soname,
			false);
		if (!result.ok())
		{
			rollback();
			return result.error();
		}
		mapped.emplace_back(start, end);

		if (segment.filesz != 0)
		{
			if (segment.offset > image.bytes().size() ||
			    segment.filesz > image.bytes().size() - segment.offset)
			{
				rollback();
				return Error{ ErrorCode::InvalidArgument, module.soname + ": segment exceeds the file" };
			}
			auto written = memory_.write(GuestVA{ module.base + segment.vaddr },
				image.bytes().data() + segment.offset,
				segment.filesz);
			if (!written.ok())
			{
				rollback();
				return written.error();
			}
		}
	}
	return {};
}

void ModuleLoader::unmapSegments(const Module &module)
{
	for (const ElfSegment &segment : module.segments)
	{
		if (segment.memsz == 0)
		{
			continue;
		}
		const uint64_t start = module.base + pageDown(segment.vaddr);
		const uint64_t end = module.base + pageUp(segment.vaddr + segment.memsz);
		if (end > start)
		{
			memory_.unmap(GuestVA{ start }, end - start);
		}
	}
}

void ModuleLoader::collectInitializers(Module &module)
{
	if (module.init_function.value() != 0)
	{
		initializers_.push_back(Initializer{ module.handle, module.init_function });
	}

	if (module.init_array.value() == 0 || module.init_array_size == 0)
	{
		return;
	}

	const uint64_t count = module.init_array_size / sizeof(uint64_t);
	for (uint64_t i = 0; i < count; ++i)
	{
		uint64_t entry = 0;
		const GuestVA slot = GuestVA{ module.init_array.value() + i * sizeof(uint64_t) };
		if (!memory_.read(slot, &entry, sizeof(entry)).ok() || entry == 0)
		{
			continue;
		}
		initializers_.push_back(Initializer{ module.handle, GuestVA{ entry } });
	}
}

std::vector<Initializer> ModuleLoader::takeInitializers()
{
	std::vector<Initializer> drained;
	drained.swap(initializers_);
	return drained;
}

std::vector<IfuncFixup> ModuleLoader::takeIfuncFixups()
{
	std::vector<IfuncFixup> drained;
	drained.swap(ifunc_fixups_);
	return drained;
}

Result<GuestVA> ModuleLoader::lookup(uint64_t handle, std::string_view symbol) const
{
	const Module *module = registry_.findByHandle(handle);
	if (module == nullptr)
	{
		return Error{ ErrorCode::NotFound, "unknown module handle" };
	}
	return resolver_.lookupExported(*module, symbol);
}

Result<GuestVA> ModuleLoader::lookupGlobal(std::string_view symbol) const
{
	GuestVA address;
	for (const auto &module : registry_.modules())
	{
		if (!module->is_virtual && SymbolResolver::findDefined(*module, symbol, address))
		{
			return address;
		}
	}
	return Error{ ErrorCode::NotFound, "no loaded module exports '" + std::string(symbol) + "'" };
}

} // namespace duckemu::runtime::loader
