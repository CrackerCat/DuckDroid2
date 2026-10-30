// SPDX-License-Identifier: GPL-2.0-only
// M8 decoder gate: capstone's explicit operand access flags + implicit
// registers produce the same read/write sets the QBDI trace records.
#include <array>
#include <cstdint>
#include <string>

#include "runtime/observer/trace/insn_decode.hpp"
#include "support/test_harness.hpp"

using duckemu::runtime::observer::InsnDecoder;
using duckemu::runtime::observer::InsnInfo;
using duckemu::runtime::observer::InsnReg;

namespace {

bool hasReg(const std::array<InsnReg, 8>& regs, uint8_t count, InsnReg reg) {
    for (uint8_t i = 0; i < count; ++i) {
        if (regs[i] == reg) {
            return true;
        }
    }
    return false;
}

void expectRead(const InsnInfo& info, InsnReg reg) {
    CHECK(hasReg(info.reads, info.read_count, reg));
}

void expectWrite(const InsnInfo& info, InsnReg reg) {
    CHECK(hasReg(info.writes, info.write_count, reg));
}

} // namespace

int main() {
    InsnDecoder decoder;

    // stp x29, x30, [sp, #-0x60]!
    const InsnInfo* stp = decoder.decode(0x1000, 0xa9ba7bfd);
    CHECK(stp != nullptr);
    CHECK(stp->text == "stp x29, x30, [sp, #-0x60]!");
    expectRead(*stp, InsnReg::Fp);
    expectRead(*stp, InsnReg::Lr);
    expectRead(*stp, InsnReg::Sp);
    expectWrite(*stp, InsnReg::Sp);
    CHECK_EQ(stp->read_count, 3);
    CHECK_EQ(stp->write_count, 1);

    // mov x29, sp
    const InsnInfo* mov = decoder.decode(0x1004, 0x910003fd);
    expectRead(*mov, InsnReg::Sp);
    expectWrite(*mov, InsnReg::Fp);
    CHECK_EQ(mov->read_count, 1);
    CHECK_EQ(mov->write_count, 1);

    // sub sp, sp, #0x2b0
    const InsnInfo* sub = decoder.decode(0x1008, 0xd10ac3ff);
    expectRead(*sub, InsnReg::Sp);
    expectWrite(*sub, InsnReg::Sp);

    // mov w10, #0xe905
    const InsnInfo* movw = decoder.decode(0x100c, 0x529d20aa);
    CHECK_EQ(movw->read_count, 0);
    CHECK_EQ(movw->write_count, 1);
    expectWrite(*movw, InsnReg::W10);

    // movk w10, #0xcdb8, lsl #16
    const InsnInfo* movk = decoder.decode(0x1010, 0x72b9b70a);
    expectRead(*movk, InsnReg::W10);
    expectWrite(*movk, InsnReg::W10);

    // ldrb w16, [x9, x16]
    const InsnInfo* ldrb = decoder.decode(0x1018, 0x38706930);
    expectRead(*ldrb, InsnReg::X9);
    expectRead(*ldrb, InsnReg::X16);
    expectWrite(*ldrb, InsnReg::W16);

    // strb w16, [x8]
    const InsnInfo* strb = decoder.decode(0x101c, 0x39000110);
    expectRead(*strb, InsnReg::W16);
    expectRead(*strb, InsnReg::X8);
    CHECK_EQ(strb->write_count, 0);

    // ldr x1, [x0], #8 : base is both read and written by writeback.
    const InsnInfo* ldr = decoder.decode(0x1020, 0xf8408401);
    expectRead(*ldr, InsnReg::X0);
    expectWrite(*ldr, InsnReg::X1);
    expectWrite(*ldr, InsnReg::X0);
    CHECK_EQ(ldr->read_count, 1);
    CHECK_EQ(ldr->write_count, 2);

    // blr x8
    const InsnInfo* blr = decoder.decode(0x1024, 0xd63f0100);
    CHECK(blr->is_call);
    CHECK_EQ(blr->call_reg, InsnReg::X8);
    expectRead(*blr, InsnReg::X8);
    expectWrite(*blr, InsnReg::Lr);

    // bl #0x103c
    const InsnInfo* bl = decoder.decode(0x1028, 0x94000005);
    CHECK(bl->is_call);
    expectWrite(*bl, InsnReg::Lr);
    CHECK_EQ(bl->call_target, 0x103c);

    // paciasp: reads sp and lr, writes lr.
    const InsnInfo* pac = decoder.decode(0x102c, 0xd503233f);
    expectRead(*pac, InsnReg::Sp);
    expectRead(*pac, InsnReg::Lr);
    expectWrite(*pac, InsnReg::Lr);

    // svc #0: no GPR writes, even though capstone reports LR.
    const InsnInfo* svc = decoder.decode(0x1034, 0xd4000001);
    CHECK(svc->is_svc);
    CHECK_EQ(svc->svc_imm, 0);
    CHECK_EQ(svc->write_count, 0);
    CHECK_EQ(svc->read_count, 0);

    // ret: implicit LR read.
    const InsnInfo* ret = decoder.decode(0x1038, 0xd65f03c0);
    CHECK(ret->is_return);
    expectRead(*ret, InsnReg::Lr);
    CHECK_EQ(ret->write_count, 0);

    // The cache is keyed by address and re-decodes when the word changes (SMC).
    const InsnInfo* nop = decoder.decode(0x2000, 0xd503201f);
    CHECK(nop->text == "nop");
    const InsnInfo* replaced = decoder.decode(0x2000, 0x94000000);
    CHECK(replaced->is_call);
    CHECK(replaced->text.rfind("bl", 0) == 0);

    return duckemu::test::finish("insn_decode_test");
}
