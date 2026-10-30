// SPDX-License-Identifier: GPL-2.0-only
#include "runtime/observer/trace/insn_decode.hpp"

#include <capstone/capstone.h>

#include <cstdio>
#include <unordered_map>
#include <utility>

namespace duckemu::runtime::observer
{
namespace
{

constexpr uint8_t kMaxRegs = 8;

const char *const kXNames[29] = {
	"X0", "X1", "X2", "X3", "X4", "X5", "X6", "X7", "X8", "X9",
	"X10", "X11", "X12", "X13", "X14", "X15", "X16", "X17", "X18", "X19",
	"X20", "X21", "X22", "X23", "X24", "X25", "X26", "X27", "X28",
};

const char *const kWNames[31] = {
	"W0", "W1", "W2", "W3", "W4", "W5", "W6", "W7", "W8", "W9",
	"W10", "W11", "W12", "W13", "W14", "W15", "W16", "W17", "W18", "W19",
	"W20", "W21", "W22", "W23", "W24", "W25", "W26", "W27", "W28", "W29", "W30",
};

// Capstone's AArch64 register id -> trace register. Vector and system
// registers map to None: M8 does not record them.
InsnReg mapRegister(unsigned int reg) noexcept
{
	if (reg >= ARM64_REG_X0 && reg <= ARM64_REG_X28)
	{
		return static_cast<InsnReg>(static_cast<uint8_t>(InsnReg::X0) + (reg - ARM64_REG_X0));
	}
	if (reg >= ARM64_REG_W0 && reg <= ARM64_REG_W30)
	{
		return static_cast<InsnReg>(static_cast<uint8_t>(InsnReg::W0) + (reg - ARM64_REG_W0));
	}
	if (reg == ARM64_REG_SP || reg == ARM64_REG_WSP)
	{
		return InsnReg::Sp;
	}
	if (reg == ARM64_REG_FP)
	{
		return InsnReg::Fp;
	}
	if (reg == ARM64_REG_LR)
	{
		return InsnReg::Lr;
	}
	return InsnReg::None;
}

void addReg(std::array<InsnReg, kMaxRegs> &regs, uint8_t &count, InsnReg reg) noexcept
{
	if (reg == InsnReg::None)
	{
		return;
	}
	for (uint8_t i = 0; i < count; ++i)
	{
		if (regs[i] == reg)
		{
			return;
		}
	}
	if (count < kMaxRegs)
	{
		regs[count++] = reg;
	}
}

void removeReg(std::array<InsnReg, kMaxRegs> &regs, uint8_t &count, InsnReg reg) noexcept
{
	for (uint8_t i = 0; i < count; ++i)
	{
		if (regs[i] != reg)
		{
			continue;
		}
		for (uint8_t j = static_cast<uint8_t>(i + 1); j < count; ++j)
		{
			regs[j - 1] = regs[j];
		}
		--count;
		return;
	}
}

bool isCallInstruction(unsigned int id) noexcept
{
	switch (id)
	{
	case ARM64_INS_BL:
	case ARM64_INS_BLR:
	case ARM64_INS_BLRAA:
	case ARM64_INS_BLRAAZ:
	case ARM64_INS_BLRAB:
	case ARM64_INS_BLRABZ:
		return true;
	default:
		return false;
	}
}

bool isReturnInstruction(unsigned int id) noexcept
{
	return id == ARM64_INS_RET || id == ARM64_INS_RETAA || id == ARM64_INS_RETAB;
}

void replaceControlCharacters(std::string &text)
{
	for (char &ch : text)
	{
		if (ch == '\t' || ch == '\n' || ch == '\r')
		{
			ch = ' ';
		}
	}
}

void decodeInto(InsnInfo &out, const cs_insn &insn)
{
	out.text = insn.mnemonic;
	if (insn.op_str[0] != '\0')
	{
		out.text += ' ';
		out.text += insn.op_str;
	}
	replaceControlCharacters(out.text);

	const cs_arm64 &detail = insn.detail->arm64;

	// Explicit operands. Capstone's regs_read/regs_write hold only implicit
	// registers, so the explicit read/write set has to be derived here.
	for (uint8_t i = 0; i < detail.op_count; ++i)
	{
		const cs_arm64_op &op = detail.operands[i];
		if (op.type == ARM64_OP_REG)
		{
			const InsnReg reg = mapRegister(op.reg);
			if ((op.access & CS_AC_READ) != 0)
			{
				addReg(out.reads, out.read_count, reg);
			}
			if ((op.access & CS_AC_WRITE) != 0)
			{
				addReg(out.writes, out.write_count, reg);
			}
			if (isCallInstruction(insn.id) && op.access != 0 && out.call_reg == InsnReg::None)
			{
				out.call_reg = reg;
			}
			continue;
		}
		if (op.type == ARM64_OP_MEM)
		{
			const InsnReg base = mapRegister(op.mem.base);
			const InsnReg index = mapRegister(op.mem.index);
			addReg(out.reads, out.read_count, base);
			addReg(out.reads, out.read_count, index);
			if (detail.writeback)
			{
				// LDR/STR writeback updates the base register; STP's
				// pre-index form does too.
				addReg(out.writes, out.write_count, base);
			}
			continue;
		}
		if (op.type == ARM64_OP_IMM && insn.id == ARM64_INS_BL)
		{
			out.call_target = static_cast<uint64_t>(op.imm);
			continue;
		}
		if (op.type == ARM64_OP_IMM && insn.id == ARM64_INS_SVC)
		{
			out.svc_imm = static_cast<uint16_t>(op.imm);
		}
	}

	// Implicit registers (BL/BLR write LR, PACIA reads/writes LR, ...).
	for (uint8_t i = 0; i < insn.detail->regs_read_count; ++i)
	{
		addReg(out.reads, out.read_count, mapRegister(insn.detail->regs_read[i]));
	}
	for (uint8_t i = 0; i < insn.detail->regs_write_count; ++i)
	{
		addReg(out.writes, out.write_count, mapRegister(insn.detail->regs_write[i]));
	}

	if (isCallInstruction(insn.id))
	{
		out.is_call = true;
		addReg(out.writes, out.write_count, InsnReg::Lr);
		if (insn.id == ARM64_INS_BL)
		{
			// call_target was taken from the immediate operand above.
			out.call_reg = InsnReg::None;
		}
	}
	if (isReturnInstruction(insn.id))
	{
		out.is_return = true;
		addReg(out.reads, out.read_count, InsnReg::Lr);
	}
	if (insn.id == ARM64_INS_SVC)
	{
		out.is_svc = true;
		// SVC does not write any GPR. Capstone reports LR because it models
		// the exception; keep the trace faithful to the guest CPU.
		removeReg(out.writes, out.write_count, InsnReg::Lr);
	}
}

} // namespace

const char *insnRegName(InsnReg reg) noexcept
{
	const uint8_t value = static_cast<uint8_t>(reg);
	const uint8_t x0 = static_cast<uint8_t>(InsnReg::X0);
	const uint8_t w0 = static_cast<uint8_t>(InsnReg::W0);
	if (value >= x0 && value < x0 + 29)
	{
		return kXNames[value - x0];
	}
	if (value >= w0 && value < w0 + 31)
	{
		return kWNames[value - w0];
	}
	switch (reg)
	{
	case InsnReg::Sp:
		return "SP";
	case InsnReg::Fp:
		return "FP";
	case InsnReg::Lr:
		return "LR";
	default:
		return "?";
	}
}

bool insnRegIs32Bit(InsnReg reg) noexcept
{
	const uint8_t value = static_cast<uint8_t>(reg);
	const uint8_t w0 = static_cast<uint8_t>(InsnReg::W0);
	return value >= w0 && value < w0 + 31;
}

struct InsnDecoder::Impl
{
	struct Entry
	{
		uint32_t word = 0;
		InsnInfo info;
	};

	csh handle = 0;
	std::unordered_map<uint64_t, std::unique_ptr<Entry> > cache;

	Impl()
	{
		if (cs_open(CS_ARCH_ARM64, CS_MODE_ARM, &handle) == CS_ERR_OK)
		{
			(void)cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);
		}
		else
		{
			handle = 0;
		}
	}

	~Impl()
	{
		if (handle != 0)
		{
			(void)cs_close(&handle);
		}
	}
};

InsnDecoder::InsnDecoder()
	: impl_(std::make_unique<Impl>())
{
}

InsnDecoder::~InsnDecoder() = default;

const InsnInfo *InsnDecoder::decode(uint64_t address, uint32_t word)
{
	auto found = impl_->cache.find(address);
	if (found != impl_->cache.end() && found->second->word == word)
	{
		return &found->second->info;
	}

	InsnInfo info;
	if (impl_->handle != 0)
	{
		cs_insn *insn = nullptr;
		const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&word);
		const size_t count = cs_disasm(impl_->handle, bytes, sizeof(word), address, 0, &insn);
		if (count == 1)
		{
			decodeInto(info, insn[0]);
		}
		if (insn != nullptr)
		{
			cs_free(insn, count);
		}
	}
	if (info.text.empty())
	{
		char text[32];
		std::snprintf(text, sizeof(text), ".word 0x%08x", word);
		info.text = text;
	}

	auto entry = std::make_unique<Impl::Entry>();
	entry->word = word;
	entry->info = std::move(info);
	const InsnInfo *result = &entry->info;
	impl_->cache[address] = std::move(entry);
	return result;
}

} // namespace duckemu::runtime::observer
