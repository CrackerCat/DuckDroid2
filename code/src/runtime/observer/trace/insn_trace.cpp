// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/observer/trace/insn_trace.hpp"

#include <unicorn/unicorn.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>

#include "common/log/logger.hpp"
#include "runtime/loader/module.hpp"
#include "runtime/loader/module_registry.hpp"
#include "runtime/memory/address_space.hpp"
#include "runtime/memory/region.hpp"
#include "runtime/memory/thunk/thunk_table.hpp"
#include "runtime/observer/trace/buffered_file.hpp"
#include "runtime/observer/trace/insn_decode.hpp"
#include "runtime/svc/syscall_names.hpp"

namespace duckemu::runtime::observer
{
namespace
{

constexpr uint64_t kAddressMask = 0xFFFFFFFFFFFFULL;
constexpr size_t kArgCount = 6;
constexpr size_t kMaxLine = 4096;

int unicornRegister(InsnReg reg) noexcept
{
	const int value = static_cast<int>(reg);
	const int x0 = static_cast<int>(InsnReg::X0);
	const int w0 = static_cast<int>(InsnReg::W0);
	if (value >= x0 && value < x0 + 29)
	{
		return static_cast<int>(UC_ARM64_REG_X0) + (value - x0);
	}
	if (value >= w0 && value < w0 + 31)
	{
		return static_cast<int>(UC_ARM64_REG_W0) + (value - w0);
	}
	switch (reg)
	{
	case InsnReg::Sp:
		return UC_ARM64_REG_SP;
	case InsnReg::Fp:
		return UC_ARM64_REG_FP;
	case InsnReg::Lr:
		return UC_ARM64_REG_LR;
	default:
		return -1;
	}
}

bool readRegister(uc_engine *engine, InsnReg reg, uint64_t &value) noexcept
{
	const int id = unicornRegister(reg);
	if (id < 0)
	{
		return false;
	}
	if (insnRegIs32Bit(reg))
	{
		uint32_t word = 0;
		if (uc_reg_read(engine, id, &word) != UC_ERR_OK)
		{
			return false;
		}
		value = word;
		return true;
	}
	uint64_t full = 0;
	if (uc_reg_read(engine, id, &full) != UC_ERR_OK)
	{
		return false;
	}
	value = full;
	return true;
}

std::string registersJson(uc_engine *engine, const std::array<InsnReg, 8> &regs, uint8_t count)
{
	if (count == 0)
	{
		return "{}";
	}
	std::string out;
	out.reserve(static_cast<size_t>(count) * 24);
	out += '{';
	for (uint8_t i = 0; i < count; ++i)
	{
		if (i != 0)
		{
			out += ',';
		}
		uint64_t value = 0;
		(void)readRegister(engine, regs[i], value);
		char pair[48];
		if (insnRegIs32Bit(regs[i]))
		{
			std::snprintf(pair,
				sizeof(pair),
				"\"%s\":\"0x%x\"",
				insnRegName(regs[i]),
				static_cast<unsigned int>(value));
		}
		else
		{
			std::snprintf(pair,
				sizeof(pair),
				"\"%s\":\"0x%llx\"",
				insnRegName(regs[i]),
				static_cast<unsigned long long>(value));
		}
		out += pair;
	}
	out += '}';
	return out;
}

// Mirrors process_vm_readv: copy as much of [address, address + size) as is
// mapped and readable, and stop at the first gap or unreadable region.
size_t readPartial(const RuntimeMemory &memory, uint64_t address, uint8_t *output, size_t size)
{
	uint64_t cursor = untagAddress(address);
	size_t done = 0;
	while (done < size)
	{
		const auto region = memory.regionAt(GuestVA{ cursor });
		if (!region.has_value() || (region->perms & PermRead) == 0)
		{
			break;
		}
		const uint64_t chunk = std::min<uint64_t>(size - done, region->end() - cursor);
		std::memcpy(output + done,
			region->hostPtr() + (cursor - region->base.value()),
			static_cast<size_t>(chunk));
		done += static_cast<size_t>(chunk);
		cursor += chunk;
	}
	return done;
}

void writeHexdump(BufferedFile &file, const RuntimeMemory &memory, uint64_t target)
{
	const uint64_t start = target >= 0x10 ? target - 0x10 : 0;
	uint8_t bytes_buffer[0x30];
	const size_t bytes = readPartial(memory, start, bytes_buffer, sizeof(bytes_buffer));
	if (bytes == 0)
	{
		return;
	}

	char row[256];
	for (size_t offset = 0; offset < bytes; offset += 16)
	{
		int used = std::snprintf(row,
			sizeof(row),
			"%08llx: ",
			static_cast<unsigned long long>(start + offset));
		for (int i = 0; i < 16; ++i)
		{
			if (offset + static_cast<size_t>(i) < bytes)
			{
				used += std::snprintf(row + used,
					sizeof(row) - static_cast<size_t>(used),
					"%02x ",
					bytes_buffer[offset + static_cast<size_t>(i)]);
			}
			else
			{
				used += std::snprintf(row + used,
					sizeof(row) - static_cast<size_t>(used),
					"   ");
			}
		}
		used += std::snprintf(row + used, sizeof(row) - static_cast<size_t>(used), "|");
		for (int i = 0; i < 16; ++i)
		{
			if (offset + static_cast<size_t>(i) < bytes)
			{
				const uint8_t value = bytes_buffer[offset + static_cast<size_t>(i)];
				row[used++] = (value >= 0x20 && value < 0x7f) ? static_cast<char>(value) : '.';
			}
			else
			{
				row[used++] = ' ';
			}
		}
		row[used++] = '|';
		row[used] = '\0';
		file.write(row, static_cast<size_t>(used));
		file.write("\n", 1);
	}
}

std::string moduleName(const loader::Module &module)
{
	// QBDI resolves bl.log names through dladdr().dli_fname, i.e. the basename
	// of the path the module was loaded from. Prefer that over the soname.
	const size_t slash = module.path.find_last_of('/');
	if (slash != std::string::npos)
	{
		return module.path.substr(slash + 1);
	}
	if (!module.path.empty())
	{
		return module.path;
	}
	return module.soname.empty() ? std::string("module") : module.soname;
}

std::string sanitizeSymbol(std::string text)
{
	for (char &ch : text)
	{
		if (ch == '\t' || ch == '\n' || ch == '\r')
		{
			ch = '_';
		}
	}
	return text;
}

// Dynamic-linker-style symbolization: a host thunk name, then the nearest
// preceding function symbol in the containing module, then "module!0xrva",
// then a bare address.
std::string describeSymbol(const loader::ModuleRegistry &modules,
	const ThunkTable *thunks,
	uint64_t address)
{
	const uint64_t clean = untagAddress(address);
	if (thunks != nullptr)
	{
		const uint64_t base = thunks->base().value();
		const uint64_t span = static_cast<uint64_t>(thunks->count()) * 8;
		if (clean >= base && clean < base + span && (clean - base) % 8 == 0)
		{
			const auto name = thunks->nameOf(static_cast<uint32_t>((clean - base) / 8));
			if (!name.empty())
			{
				return sanitizeSymbol(std::string(name));
			}
		}
	}
	const loader::Module *module = modules.findContaining(clean);
	if (module == nullptr || clean < module->base)
	{
		char text[32];
		std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(clean));
		return text;
	}

	const uint64_t rva = clean - module->base;
	const loader::ElfSymbol *best = nullptr;
	for (const loader::ElfSymbol &symbol : module->symbols)
	{
		if (symbol.undefined() || symbol.name.empty())
		{
			continue;
		}
		const uint8_t type = symbol.type();
		if (type != 2 /* STT_FUNC */ && type != 0 /* STT_NOTYPE */)
		{
			continue;
		}
		if (symbol.value > rva)
		{
			continue;
		}
		if (symbol.size != 0 && rva >= symbol.value + symbol.size)
		{
			continue;
		}
		if (best == nullptr || symbol.value > best->value)
		{
			best = &symbol;
		}
	}
	if (best != nullptr)
	{
		return sanitizeSymbol(best->name);
	}

	char text[320];
	std::snprintf(text,
		sizeof(text),
		"%s!0x%llx",
		moduleName(*module).c_str(),
		static_cast<unsigned long long>(rva));
	return text;
}

struct PendingInstruction
{
	bool valid = false;
	uint64_t line = 0;
	uint64_t address = 0;
	const InsnInfo *info = nullptr;
	std::string reads;
};

struct PendingAccess
{
	bool write = false;
	uint64_t address = 0;
};

struct CurrentInstruction
{
	bool valid = false;
	uint64_t line = 0;
	uint64_t pc = 0;
	std::array<PendingAccess, 8> accesses{};
	uint8_t access_count = 0;
};

struct EngineTraceState
{
	CurrentInstruction current;
	bool armed = false;
	bool finished = false;
	uint64_t entry_lr = 0;
	bool have_entry_lr = false;
};

class FileInsnTrace final : public InsnTraceSink
{
    public:
	FileInsnTrace(const InsnTraceOptions &options,
		RuntimeMemory &memory,
		const loader::ModuleRegistry &modules,
		const ThunkTable *thunks,
		uint64_t module_base,
		std::string module_name,
		std::unique_ptr<BufferedFile> code,
		std::unique_ptr<BufferedFile> bl,
		std::unique_ptr<BufferedFile> rw,
		std::unique_ptr<BufferedFile> threads)
		: memory_(memory)
		, modules_(modules)
		, thunks_(thunks)
		, begin_(options.begin.value())
		, end_(options.begin.value() + options.size)
		, module_base_(module_base)
		, module_name_(std::move(module_name))
		, max_instructions_(options.max_instructions)
		, code_(std::move(code))
		, bl_(std::move(bl))
		, rw_(std::move(rw))
		, threads_(std::move(threads))
		, stop_at_return_(options.stop_at_return)
	{
		DUCKEMU_INFO("insn trace: %s+0x%llx size=0x%llx -> %s",
			module_name_.c_str(),
			static_cast<unsigned long long>(begin_ - module_base_),
			static_cast<unsigned long long>(options.size),
			options.directory.c_str());
	}

	void onInstruction(void *engine_pointer, uint64_t engine_id, uint32_t tid, uint64_t address) override
	{
		auto *engine = static_cast<uc_engine *>(engine_pointer);
		EngineTraceState &state = states_[engine_id];
		if (stop_at_return_)
		{
			if (!have_armed_engine_)
			{
				// The first in-range instruction after startInsnTrace is the
				// called function's entry.
				have_armed_engine_ = true;
				armed_engine_id_ = engine_id;
				state.armed = true;
				uint64_t lr = 0;
				(void)uc_reg_read(engine, UC_ARM64_REG_LR, &lr);
				state.entry_lr = lr;
				state.have_entry_lr = true;
			}
			else if (engine_id != armed_engine_id_)
			{
				return;
			}
			if (state.finished)
			{
				return;
			}
			if (state.have_entry_lr && address == state.entry_lr)
			{
				// The function returned to its caller.
				flushPending(engine, engine_id);
				state.finished = true;
				return;
			}
		}

		flushPending(engine, engine_id);
		if (!recording_)
		{
			return;
		}
		if (max_instructions_ != 0 && next_line_ >= max_instructions_)
		{
			recording_ = false;
			DUCKEMU_WARN("insn trace: instruction budget %llu reached; recording stopped",
				static_cast<unsigned long long>(max_instructions_));
			return;
		}

		uint32_t word = 0;
		const auto read = memory_.readU32(GuestVA{ address });
		if (read.ok())
		{
			word = read.value();
		}
		const InsnInfo *info = decoder_.decode(address, word);
		const uint64_t line = next_line_++;
		if (!have_tid_ || tid != last_tid_)
		{
			char entry[64];
			std::snprintf(entry,
				sizeof(entry),
				"%llx\t0x%x\n",
				static_cast<unsigned long long>(line),
				tid);
			threads_->write(entry);
			last_tid_ = tid;
			have_tid_ = true;
		}

		const std::string reads = registersJson(engine, info->reads, info->read_count);
		CurrentInstruction &current = state.current;
		current.valid = true;
		current.line = line;
		current.pc = address;
		current.access_count = 0;

		if (info->is_svc)
		{
			const std::string note = svcNote(engine, *info);
			emitCodeLine(line, address, *info, reads, "{}", note);
			PendingInstruction &pending = pending_[engine_id];
			pending.valid = false;
			return;
		}

		PendingInstruction &pending = pending_[engine_id];
		pending.valid = true;
		pending.line = line;
		pending.address = address;
		pending.info = info;
		pending.reads = reads;

		if (info->is_call)
		{
			emitCall(engine, line, *info);
		}
	}

	// Unicorn calls the write hook before the store commits. Buffer the access
	// and emit rw.log when the instruction's POST is flushed, so a `w` entry
	// hexdump shows the written bytes (same as QBDI's getInstMemoryAccess).
	void onMemoryAccess(void *engine_pointer,
		uint64_t engine_id,
		uint32_t tid,
		bool write,
		uint64_t address,
		uint64_t size,
		uint64_t value) override
	{
		(void)tid;
		(void)size;
		(void)value;
		if (!recording_)
		{
			return;
		}
		auto *engine = static_cast<uc_engine *>(engine_pointer);
		auto state = states_.find(engine_id);
		if (state == states_.end() || !state->second.current.valid)
		{
			return;
		}
		if (stop_at_return_ &&
			(engine_id != armed_engine_id_ || !state->second.armed || state->second.finished))
		{
			return;
		}
		auto &current = state->second.current;
		uint64_t pc = 0;
		if (uc_reg_read(engine, UC_ARM64_REG_PC, &pc) != UC_ERR_OK || pc != current.pc)
		{
			return;
		}
		if (current.access_count < current.accesses.size())
		{
			current.accesses[current.access_count++] = PendingAccess{ write,
				address & kAddressMask };
		}
	}

	void onRunEnd(void *engine_pointer, uint64_t engine_id) override
	{
		flushPending(static_cast<uc_engine *>(engine_pointer), engine_id);
		auto state = states_.find(engine_id);
		if (state != states_.end())
		{
			state->second.current.valid = false;
		}
	}

    private:
	void emitMemoryAccess(uint64_t line, const PendingAccess &access)
	{
		char text[160];
		std::snprintf(text,
			sizeof(text),
			"%llx\t%s\t0x%llx+0x%llx\n",
			static_cast<unsigned long long>(line),
			access.write ? "w" : "r",
			static_cast<unsigned long long>(access.address),
			static_cast<unsigned long long>(0));
		rw_->write(text);
		writeHexdump(*rw_, memory_, access.address);
	}

	void flushPending(uc_engine *engine, uint64_t engine_id)
	{
		auto found = pending_.find(engine_id);
		if (found == pending_.end() || !found->second.valid)
		{
			return;
		}
		PendingInstruction pending = std::move(found->second);
		found->second.valid = false;
		const std::string writes = registersJson(engine, pending.info->writes, pending.info->write_count);
		emitCodeLine(pending.line, pending.address, *pending.info, pending.reads, writes, "{}");

		auto state = states_.find(engine_id);
		if (state != states_.end())
		{
			for (uint8_t i = 0; i < state->second.current.access_count; ++i)
			{
				emitMemoryAccess(pending.line, state->second.current.accesses[i]);
			}
			state->second.current.access_count = 0;
		}
	}

	void emitCodeLine(uint64_t line,
		uint64_t address,
		const InsnInfo &info,
		const std::string &reads,
		const std::string &writes,
		const std::string &note)
	{
		char buffer[kMaxLine];
		const int used = std::snprintf(buffer,
			sizeof(buffer),
			"%llx\t0x%-12llx\t0x%-8llx\t\"%s\"\t%-28s\t%-28s\t%s\n",
			static_cast<unsigned long long>(line),
			static_cast<unsigned long long>(address),
			static_cast<unsigned long long>(address - module_base_),
			info.text.c_str(),
			reads.c_str(),
			writes.c_str(),
			note.c_str());
		if (used <= 0)
		{
			return;
		}
		const size_t length = static_cast<size_t>(used) < sizeof(buffer) ? static_cast<size_t>(used) :
										      sizeof(buffer) - 1;
		code_->write(buffer, length);
	}

	void emitCall(uc_engine *engine, uint64_t line, const InsnInfo &info)
	{
		uint64_t target = info.call_target;
		if (target == 0 && info.call_reg != InsnReg::None)
		{
			(void)readRegister(engine, info.call_reg, target);
		}
		if (target == 0)
		{
			return;
		}
		const uint64_t clean = target & kAddressMask;
		if (clean >= begin_ && clean < end_)
		{
			return;
		}

		const std::string symbol = describeSymbol(modules_, thunks_, clean);
		char buffer[512];
		const int used = std::snprintf(buffer,
			sizeof(buffer),
			"%llx\t0x%llx\t%s\n",
			static_cast<unsigned long long>(line),
			static_cast<unsigned long long>(target),
			symbol.c_str());
		if (used > 0)
		{
			bl_->write(buffer,
				std::min(static_cast<size_t>(used), sizeof(buffer) - 1));
		}
		writeHexdump(*bl_, memory_, clean);
	}

	std::string svcNote(uc_engine *engine, const InsnInfo &info) const
	{
		uint64_t number = 0;
		(void)readRegister(engine, InsnReg::X8, number);
		std::array<uint64_t, kArgCount> args{};
		for (size_t i = 0; i < kArgCount; ++i)
		{
			(void)readRegister(engine,
				static_cast<InsnReg>(static_cast<int>(InsnReg::X0) + static_cast<int>(i)),
				args[i]);
		}

		std::string note;
		if (info.svc_imm >= 0x8000u)
		{
			char head[64];
			std::snprintf(head,
				sizeof(head),
				"{\"host\":\"0x%x\",\"args\":[",
				static_cast<unsigned int>(info.svc_imm & 0x7FFFu));
			note = head;
		}
		else
		{
			const std::string_view name = svc::syscallName(static_cast<uint32_t>(number));
			char head[96];
			if (!name.empty())
			{
				std::snprintf(head,
					sizeof(head),
					"{\"syscall\":\"%.*s\",\"args\":[",
					static_cast<int>(name.size()),
					name.data());
			}
			else
			{
				std::snprintf(head,
					sizeof(head),
					"{\"syscall\":\"syscall_%u\",\"args\":[",
					static_cast<unsigned int>(number));
			}
			note = head;
		}
		for (size_t i = 0; i < args.size(); ++i)
		{
			char value[32];
			std::snprintf(value,
				sizeof(value),
				"%s\"0x%llx\"",
				i == 0 ? "" : ",",
				static_cast<unsigned long long>(args[i]));
			note += value;
		}
		note += "]}";
		return note;
	}

	RuntimeMemory &memory_;
	const loader::ModuleRegistry &modules_;
	const ThunkTable *thunks_ = nullptr;
	uint64_t begin_ = 0;
	uint64_t end_ = 0;
	uint64_t module_base_ = 0;
	std::string module_name_;
	uint64_t max_instructions_ = 0;
	uint64_t next_line_ = 0;
	bool recording_ = true;
	bool have_tid_ = false;
	uint32_t last_tid_ = 0;

	std::unique_ptr<BufferedFile> code_;
	std::unique_ptr<BufferedFile> bl_;
	std::unique_ptr<BufferedFile> rw_;
	std::unique_ptr<BufferedFile> threads_;
	InsnDecoder decoder_;

	std::unordered_map<uint64_t, PendingInstruction> pending_;
	std::unordered_map<uint64_t, EngineTraceState> states_;
	bool stop_at_return_ = false;
	bool have_armed_engine_ = false;
	uint64_t armed_engine_id_ = 0;
};

} // namespace

Result<std::unique_ptr<InsnTraceSink> > createInsnTrace(
	const InsnTraceOptions &options,
	RuntimeMemory &memory,
	const loader::ModuleRegistry &modules,
	const ThunkTable *thunks)
{
	if (options.directory.empty())
	{
		return Error{ ErrorCode::InvalidArgument, "insn trace directory is empty" };
	}
	if (options.size == 0)
	{
		return Error{ ErrorCode::InvalidArgument, "insn trace size is zero" };
	}
	if (options.begin.value() % 4 != 0)
	{
		return Error{ ErrorCode::InvalidArgument, "insn trace begin is not 4-byte aligned" };
	}
	const uint64_t begin = options.begin.value();
	const uint64_t clean_begin = untagAddress(begin);
	if (clean_begin > UINT64_MAX - options.size)
	{
		return Error{ ErrorCode::InvalidArgument, "insn trace range overflows" };
	}
	const uint64_t clean_end = clean_begin + options.size;
	InsnTraceOptions effective = options;
	effective.begin = GuestVA{ clean_begin };
	uint64_t region_base = 0;
	std::string region_name;
	const loader::Module *module = modules.findContaining(clean_begin);
	if (module != nullptr)
	{
		if (!module->contains(clean_begin) || !module->contains(clean_end - 1))
		{
			return Error{ ErrorCode::InvalidArgument,
				"insn trace range must stay inside one module" };
		}
		region_base = module->base;
		region_name = moduleName(*module);
	}
	else
	{
		// Bare blobs and JIT regions have no Module. Use the containing
		// region as the base; `offset` then stays relative to that region.
		const auto region = memory.regionAt(GuestVA{ clean_begin });
		if (!region.has_value() || !region->contains(clean_begin, options.size))
		{
			return Error{ ErrorCode::InvalidArgument,
				"insn trace range must stay inside one mapped region" };
		}
		region_base = region->base.value();
		region_name = region->name.empty() ? "region" : region->name;
	}

	std::error_code directory_error;
	std::filesystem::create_directories(options.directory, directory_error);
	if (directory_error)
	{
		return Error{ ErrorCode::NotFound,
			"cannot create trace directory " + options.directory + ": " + directory_error.message() };
	}

	const std::string base = options.directory;
	auto code = BufferedFile::open(base + "/code.log");
	if (!code.ok())
	{
		return code.error();
	}
	auto bl = BufferedFile::open(base + "/bl.log");
	if (!bl.ok())
	{
		return bl.error();
	}
	auto rw = BufferedFile::open(base + "/rw.log");
	if (!rw.ok())
	{
		return rw.error();
	}
	auto threads = BufferedFile::open(base + "/threads.tsv");
	if (!threads.ok())
	{
		return threads.error();
	}

	return std::unique_ptr<InsnTraceSink>(new FileInsnTrace(effective,
		memory,
		modules,
		thunks,
		region_base,
		std::move(region_name),
		std::move(code).value(),
		std::move(bl).value(),
		std::move(rw).value(),
		std::move(threads).value()));
}

} // namespace duckemu::runtime::observer
