// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace duckemu::runtime::observer
{

// General-purpose registers the trace records. Vector registers are not
// included: M8 does not record or write NEON/Q/D/S registers.
enum class InsnReg : uint8_t
{
	None = 0,
	X0,
	X1,
	X2,
	X3,
	X4,
	X5,
	X6,
	X7,
	X8,
	X9,
	X10,
	X11,
	X12,
	X13,
	X14,
	X15,
	X16,
	X17,
	X18,
	X19,
	X20,
	X21,
	X22,
	X23,
	X24,
	X25,
	X26,
	X27,
	X28,
	W0,
	W1,
	W2,
	W3,
	W4,
	W5,
	W6,
	W7,
	W8,
	W9,
	W10,
	W11,
	W12,
	W13,
	W14,
	W15,
	W16,
	W17,
	W18,
	W19,
	W20,
	W21,
	W22,
	W23,
	W24,
	W25,
	W26,
	W27,
	W28,
	W29,
	W30,
	Sp,
	Fp,
	Lr,
};

// "X0" / "W10" / "SP" / "FP" / "LR", matching the names QBDI emits.
[[nodiscard]] const char *insnRegName(InsnReg reg) noexcept;
[[nodiscard]] bool insnRegIs32Bit(InsnReg reg) noexcept;

struct InsnInfo
{
	std::string text;
	std::array<InsnReg, 8> reads{};
	std::array<InsnReg, 8> writes{};
	uint8_t read_count = 0;
	uint8_t write_count = 0;

	bool is_call = false;
	bool is_return = false;
	bool is_svc = false;
	// bl/blra*: absolute immediate target. blr: the register holding the target.
	uint64_t call_target = 0;
	InsnReg call_reg = InsnReg::None;
	uint16_t svc_imm = 0;
};

// AArch64 decoder with a per-address cache. The cache stores the raw word; a
// code write that changes the bytes invalidates the entry on the next decode.
//
// Not thread-safe: one decoder belongs to one trace session, and the session
// runs under the instance execution lock.
class InsnDecoder
{
    public:
	InsnDecoder();
	~InsnDecoder();
	InsnDecoder(const InsnDecoder &) = delete;
	InsnDecoder &operator=(const InsnDecoder &) = delete;
	InsnDecoder(InsnDecoder &&) = delete;
	InsnDecoder &operator=(InsnDecoder &&) = delete;

	// Returns a stable InsnInfo for `address`. `word` is the current guest
	// word at that address (little-endian). The pointer stays valid until the
	// same address is decoded again with a different word.
	[[nodiscard]] const InsnInfo *decode(uint64_t address, uint32_t word);

    private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace duckemu::runtime::observer
