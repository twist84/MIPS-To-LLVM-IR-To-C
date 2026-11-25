#include "instruction_lifter.h"
#include "ee_opcode.h"

namespace ee {

s_lifted_op lift(const s_ee_instruction& insn)
{
    s_lifted_op op;
    op.pc = insn.pc;
    op.rt = insn.rt;
    op.rs = insn.rs;
    op.rd = insn.rd;
    op.sa = static_cast<uint8_t>(insn.sa);
    op.immediate = insn.immediate;
    op.uimmediate = insn.uimmediate;
    op.branch_target = insn.branch_target;
    op.jump_target = insn.jump_target;
    op.has_delay_slot = insn.has_delay_slot;
    op.branch_likely = insn.branch_likely;

    if (insn.valid != e_ee_valid::VALID) {
        op.kind = LOP_UNSUPPORTED;
        return op;
    }

    switch (insn.opcode)
    {
    case e_ee_opcode::OP_ADDIU: op.kind = LOP_ADDIU; break;
    case e_ee_opcode::OP_DADDIU: op.kind = LOP_DADDIU; break;
    case e_ee_opcode::OP_ANDI: op.kind = LOP_ANDI; break;
    case e_ee_opcode::OP_ORI: op.kind = LOP_ORI; break;
    case e_ee_opcode::OP_XORI: op.kind = LOP_XORI; break;
    case e_ee_opcode::OP_LUI: op.kind = LOP_LUI; break;
    case e_ee_opcode::OP_SLTI: op.kind = LOP_SLTI; break;
    case e_ee_opcode::OP_SLTIU: op.kind = LOP_SLTIU; break;
    case e_ee_opcode::OP_LW: op.kind = LOP_LW; break;
    case e_ee_opcode::OP_LWL: op.kind = LOP_LWL; break;
    case e_ee_opcode::OP_LWR: op.kind = LOP_LWR; break;
    case e_ee_opcode::OP_LBU: op.kind = LOP_LBU; break;
    case e_ee_opcode::OP_LHU: op.kind = LOP_LHU; break;
    case e_ee_opcode::OP_LB: op.kind = LOP_LB; break;
    case e_ee_opcode::OP_LH: op.kind = LOP_LH; break;
    case e_ee_opcode::OP_LWU: op.kind = LOP_LWU; break;
    case e_ee_opcode::OP_LD: op.kind = LOP_LD; break;
    case e_ee_opcode::OP_LQ: op.kind = LOP_LQ; break;
    case e_ee_opcode::OP_SQ: op.kind = LOP_SQ; break;
    case e_ee_opcode::OP_SW: op.kind = LOP_SW; break;
    case e_ee_opcode::OP_SWL: op.kind = LOP_SWL; break;
    case e_ee_opcode::OP_SDL: op.kind = LOP_SDL; break;
    case e_ee_opcode::OP_SB: op.kind = LOP_SB; break;
    case e_ee_opcode::OP_SH: op.kind = LOP_SH; break;
    case e_ee_opcode::OP_SD: op.kind = LOP_SD; break;
    case e_ee_opcode::OP_ADDI: op.kind = LOP_CHECKED_SIGNED; break;
    case e_ee_opcode::OP_BEQ: op.kind = LOP_BEQ; break;
    case e_ee_opcode::OP_BNE: op.kind = LOP_BNE; break;
    case e_ee_opcode::OP_BEQL: op.kind = LOP_BEQ; break;
    case e_ee_opcode::OP_BNEL: op.kind = LOP_BNE; break;
    case e_ee_opcode::OP_BLEZ: op.kind = LOP_BLTZ; break; // rs <= 0 (approx via BLTZ-class)
    case e_ee_opcode::OP_BGTZ: op.kind = LOP_BGEZ; break; // rs > 0 (approx)
    case e_ee_opcode::OP_BLEZL: op.kind = LOP_BLTZ; break;
    case e_ee_opcode::OP_BGTZL: op.kind = LOP_BGEZ; break;
    case e_ee_opcode::OP_DADDI: op.kind = LOP_CHECKED_SIGNED; break;
    case e_ee_opcode::OP_LDL: op.kind = LOP_LDL; break;
    case e_ee_opcode::OP_LDR: op.kind = LOP_LDR; break;
    case e_ee_opcode::OP_SWR: op.kind = LOP_SWR; break;
    case e_ee_opcode::OP_SDR: op.kind = LOP_SDR; break;
    case e_ee_opcode::OP_LQC2: op.kind = LOP_LQC2; break;
    case e_ee_opcode::OP_SQC2: op.kind = LOP_SQC2; break;
    case e_ee_opcode::OP_CACHE: op.kind = LOP_NOP; break;
    case e_ee_opcode::OP_PREF: op.kind = LOP_NOP; break;
    case e_ee_opcode::OP_J: op.kind = LOP_J; break;
    case e_ee_opcode::OP_JAL: op.kind = LOP_JAL; break;
    case e_ee_opcode::OP_SPECIAL:
        switch (static_cast<e_ee_opcode>(insn.funct))
        {
        case e_ee_opcode::FUNCT_JR: op.kind = LOP_JR; break;
        case e_ee_opcode::FUNCT_JALR: op.kind = LOP_JALR; break;
        case e_ee_opcode::FUNCT_SYSCALL: op.kind = LOP_SYSCALL; break;
        case e_ee_opcode::FUNCT_BREAK: op.kind = LOP_BREAK; break;
        case e_ee_opcode::FUNCT_SLL:
            if (insn.rd == 0 && insn.rt == 0 && insn.rs == 0) op.kind = LOP_NOP;
            else op.kind = LOP_SLL;
            break;
        case e_ee_opcode::FUNCT_SRL: op.kind = LOP_SRL; break;
        case e_ee_opcode::FUNCT_SRA: op.kind = LOP_SRA; break;
        // 0x04 SLLV / 0x06 SRLV (ROTR aliases share the same values — one case only).
        case e_ee_opcode::FUNCT_SLLV: op.kind = LOP_SLL; break;
        case e_ee_opcode::FUNCT_SRLV: op.kind = LOP_SRL; break;
        case e_ee_opcode::FUNCT_ADD:
        case e_ee_opcode::FUNCT_SUB:
        case e_ee_opcode::FUNCT_DADD:
        case e_ee_opcode::FUNCT_DSUB:
            op.kind = LOP_CHECKED_SIGNED; break;
        case e_ee_opcode::FUNCT_ADDU:
        case e_ee_opcode::FUNCT_DADDU: op.kind = LOP_ADDU; break;
        case e_ee_opcode::FUNCT_SUBU:
        case e_ee_opcode::FUNCT_DSUBU: op.kind = LOP_SUBU; break;
        case e_ee_opcode::FUNCT_AND: op.kind = LOP_AND; break;
        case e_ee_opcode::FUNCT_OR: op.kind = LOP_OR; break;
        case e_ee_opcode::FUNCT_XOR: op.kind = LOP_XOR; break;
        case e_ee_opcode::FUNCT_NOR: op.kind = LOP_NOR; break;
        case e_ee_opcode::FUNCT_SLT: op.kind = LOP_SLT; break;
        case e_ee_opcode::FUNCT_SLTU: op.kind = LOP_SLTU; break;
        case e_ee_opcode::FUNCT_MFSA: op.kind = LOP_MFSA; break;
        case e_ee_opcode::FUNCT_MTSA: op.kind = LOP_MTSA; break;
        case e_ee_opcode::FUNCT_MFHI: op.kind = LOP_MFHI; break;
        case e_ee_opcode::FUNCT_MTHI: op.kind = LOP_MTHI; break;
        case e_ee_opcode::FUNCT_MFLO: op.kind = LOP_MFLO; break;
        case e_ee_opcode::FUNCT_MTLO: op.kind = LOP_MTLO; break;
        case e_ee_opcode::FUNCT_MULT: op.kind = LOP_MULT; break;
        case e_ee_opcode::FUNCT_MULTU: op.kind = LOP_MULTU; break;
        case e_ee_opcode::FUNCT_DIV: op.kind = LOP_DIV; break;
        case e_ee_opcode::FUNCT_DIVU: op.kind = LOP_DIVU; break;
        case e_ee_opcode::FUNCT_MOVZ: op.kind = LOP_MOVZ; break;
        case e_ee_opcode::FUNCT_MOVN: op.kind = LOP_MOVN; break;
        case e_ee_opcode::FUNCT_SRAV: op.kind = LOP_SRA; break;
        case e_ee_opcode::FUNCT_DSLL:
        case e_ee_opcode::FUNCT_DSLL32:
        case e_ee_opcode::FUNCT_DSLLV:
            op.kind = LOP_SLL; break;
        case e_ee_opcode::FUNCT_DSRL:
        case e_ee_opcode::FUNCT_DSRL32:
        case e_ee_opcode::FUNCT_DSRLV:
            op.kind = LOP_SRL; break;
        case e_ee_opcode::FUNCT_DSRA:
        case e_ee_opcode::FUNCT_DSRA32:
        case e_ee_opcode::FUNCT_DSRAV:
            op.kind = LOP_SRA; break;
        case e_ee_opcode::FUNCT_SYNC:
            op.kind = LOP_NOP; break;
        case e_ee_opcode::FUNCT_TGE:
        case e_ee_opcode::FUNCT_TGEU:
        case e_ee_opcode::FUNCT_TLT:
        case e_ee_opcode::FUNCT_TLTU:
        case e_ee_opcode::FUNCT_TEQ:
        case e_ee_opcode::FUNCT_TNE:
            op.kind = LOP_CHECKED_TRAP; break;
        default:
            op.kind = LOP_UNSUPPORTED;
            break;
        }
        break;
    case e_ee_opcode::OP_MMI:
        // The decoder marks only exact lowerings VALID. Dispatch by the real
        // R5900 top-level funct or the class-specific sa sub-op.
        switch (insn.funct)
        {
        case 0x00: op.kind = LOP_MADD; break;
        case 0x01: op.kind = LOP_MADDU; break;
        case 0x18: op.kind = LOP_MULT1; break;
        case 0x19: op.kind = LOP_MULTU1; break;
        case 0x1a: op.kind = LOP_DIV1; break;
        case 0x1b: op.kind = LOP_DIVU1; break;
        case 0x20: op.kind = LOP_MADD1; break;
        case 0x21: op.kind = LOP_MADDU1; break;
        case 0x10: op.kind = LOP_MFHI1; break;
        case 0x11: op.kind = LOP_MTHI1; break;
        case 0x12: op.kind = LOP_MFLO1; break;
        case 0x13: op.kind = LOP_MTLO1; break;
        case 0x30: op.kind = LOP_MMI_PMFHL; break;
        case 0x31: op.kind = LOP_MMI_PMTHL; break;
        case 0x04: op.kind = LOP_MMI_PLZCW; break;
        case 0x34: case 0x36: case 0x37:
        case 0x3c: case 0x3e: case 0x3f:
            op.kind = LOP_MMI_SHIFT; break;
        case 0x08: // MMI0
            switch (insn.sa & 31)
            {
            case 0: op.kind = LOP_MMI_ADD_W; break;
            case 1: op.kind = LOP_MMI_SUB_W; break;
            case 3: op.kind = LOP_MMI_MAXS_W; break;
            case 4: op.kind = LOP_MMI_ADD_H; break;
            case 5: op.kind = LOP_MMI_SUB_H; break;
            case 7: op.kind = LOP_MMI_MAXS_H; break;
            case 8: op.kind = LOP_MMI_ADD_B; break;
            case 9: op.kind = LOP_MMI_SUB_B; break;
            case 0x02: case 0x06: case 0x0a:
            case 0x10: case 0x11: case 0x12: case 0x13:
            case 0x14: case 0x15: case 0x16: case 0x17:
            case 0x18: case 0x19: case 0x1a: case 0x1b:
            case 0x1e: case 0x1f:
                op.kind = LOP_MMI0_EXACT; break;
            default: op.kind = LOP_UNSUPPORTED; break;
            }
            break;
        case 0x28: // MMI1
            switch (insn.sa & 31)
            {
            case 0x01: op.kind = LOP_MMI_ABS_W; break;
            case 0x02: op.kind = LOP_MMI_CEQ_W; break;
            case 0x03: op.kind = LOP_MMI_MINS_W; break;
            case 0x05: op.kind = LOP_MMI_ABS_H; break;
            case 0x06: op.kind = LOP_MMI_CEQ_H; break;
            case 0x07: op.kind = LOP_MMI_MINS_H; break;
            case 0x04: op.kind = LOP_MMI_ADDSUB_H; break;
            case 0x0a: op.kind = LOP_MMI_CEQ_B; break;
            case 0x10: op.kind = LOP_MMI_ADDU_SAT_W; break;
            case 0x11: op.kind = LOP_MMI_SUBU_SAT_W; break;
            case 0x12: op.kind = LOP_MMI_EXTU_W; break;
            case 0x14: op.kind = LOP_MMI_ADDU_SAT_H; break;
            case 0x15: op.kind = LOP_MMI_SUBU_SAT_H; break;
            case 0x16: op.kind = LOP_MMI_EXTU_H; break;
            case 0x18: op.kind = LOP_MMI_ADDU_SAT_B; break;
            case 0x19: op.kind = LOP_MMI_SUBU_SAT_B; break;
            case 0x1a: op.kind = LOP_MMI_EXTU_B; break;
            case 0x1b: op.kind = LOP_MMI_QFSRV; break;
            default: op.kind = LOP_UNSUPPORTED; break;
            }
            break;
        case 0x09: // MMI2
            switch (insn.sa & 31)
            {
            case 0x00: case 0x04: op.kind = LOP_MMI_PACKED_MAC; break;
            case 0x02: case 0x03: op.kind = LOP_MMI_SHIFT; break;
            case 0x0a: op.kind = LOP_MMI_INTERLEAVE; break;
            case 0x10: case 0x11: case 0x14: case 0x15: case 0x1c:
                op.kind = LOP_MMI_PACKED_HALF; break;
            case 0x08: case 0x09: op.kind = LOP_MMI_HILO_PAIR_TRANSFER; break;
            case 0x0c: op.kind = LOP_MMI_PACKED_MULT; break;
            case 0x0d: case 0x1d: op.kind = LOP_MMI_PACKED_DIV; break;
            case 0x0e: op.kind = LOP_MMI_COPY_LOWER_D; break;
            case 0x12: op.kind = LOP_MMI_AND; break;
            case 0x13: op.kind = LOP_MMI_XOR; break;
            case 0x1a: case 0x1b: case 0x1e: case 0x1f:
                op.kind = LOP_MMI_PERMUTE; break;
            default: op.kind = LOP_UNSUPPORTED; break;
            }
            break;
        case 0x29: // MMI3
            switch (insn.sa & 31)
            {
            case 0x00: op.kind = LOP_MMI_PACKED_MAC; break;
            case 0x03: op.kind = LOP_MMI_SHIFT; break;
            case 0x0a: op.kind = LOP_MMI_INTERLEAVE; break;
            case 0x08: case 0x09: op.kind = LOP_MMI_HILO_PAIR_TRANSFER; break;
            case 0x0c: op.kind = LOP_MMI_PACKED_MULT; break;
            case 0x0d: op.kind = LOP_MMI_PACKED_DIV; break;
            case 0x0e: op.kind = LOP_MMI_COPY_UPPER_D; break;
            case 0x12: op.kind = LOP_MMI_OR; break;
            case 0x13: op.kind = LOP_MMI_NOR; break;
            case 0x1a: case 0x1e: op.kind = LOP_MMI_PERMUTE; break;
            case 0x1b: op.kind = LOP_MMI_COPY_H; break;
            default: op.kind = LOP_UNSUPPORTED; break;
            }
            break;
        default:
            op.kind = LOP_UNSUPPORTED;
            break;
        }
        break;
    case e_ee_opcode::OP_REGIMM:
        switch (static_cast<e_ee_opcode>(insn.rt))
        {
        case e_ee_opcode::REGIMM_BLTZ:
        case e_ee_opcode::REGIMM_BLTZL:
        case e_ee_opcode::REGIMM_BLTZAL:
        case e_ee_opcode::REGIMM_BLTZALL:
            op.kind = LOP_BLTZ; break;
        case e_ee_opcode::REGIMM_BGEZ:
        case e_ee_opcode::REGIMM_BGEZL:
        case e_ee_opcode::REGIMM_BGEZAL:
        case e_ee_opcode::REGIMM_BGEZALL:
            op.kind = LOP_BGEZ; break;
        case e_ee_opcode::REGIMM_TGEI:
        case e_ee_opcode::REGIMM_TGEIU:
        case e_ee_opcode::REGIMM_TLTI:
        case e_ee_opcode::REGIMM_TLTIU:
        case e_ee_opcode::REGIMM_TEQI:
        case e_ee_opcode::REGIMM_TNEI:
            op.kind = LOP_CHECKED_TRAP; break;
        case e_ee_opcode::REGIMM_MTSAB:
            op.kind = LOP_MTSAB; break;
        case e_ee_opcode::REGIMM_MTSAH:
            op.kind = LOP_MTSAH; break;
        default:
            op.kind = LOP_UNSUPPORTED; break;
        }
        break;
    case e_ee_opcode::OP_COP0:
        switch (static_cast<e_ee_opcode>(insn.rs))
        {
        case e_ee_opcode::COP0_MFC0:
            op.kind = insn.rd == 25u ? LOP_COP0_PERF : LOP_MFC0; break;
        case e_ee_opcode::COP0_MTC0:
            op.kind = insn.rd == 25u ? LOP_COP0_PERF : LOP_MTC0; break;
        case e_ee_opcode::COP0_BC0:  op.kind = LOP_BC0; break;
        case e_ee_opcode::COP0_CO:
            switch (static_cast<e_ee_opcode>(insn.funct))
            {
            case e_ee_opcode::COP0_ERET: op.kind = LOP_ERET; break;
            case e_ee_opcode::COP0_EI:
            case e_ee_opcode::COP0_DI:
            case e_ee_opcode::COP0_TLBR:
            case e_ee_opcode::COP0_TLBWI:
            case e_ee_opcode::COP0_TLBWR:
            case e_ee_opcode::COP0_TLBP:
                op.kind = LOP_MTC0; break;
            default:
                op.kind = LOP_MTC0; break; // other CO ops accepted
            }
            break;
        default: op.kind = LOP_UNSUPPORTED; break;
        }
        break;
    case e_ee_opcode::OP_LWC1: op.kind = LOP_LWC1; break;
    case e_ee_opcode::OP_SWC1: op.kind = LOP_SWC1; break;
    case e_ee_opcode::OP_COP1:
        switch (static_cast<e_ee_opcode>(insn.rs))
        {
        case e_ee_opcode::COP1_MFC1: op.kind = LOP_MFC1; break;
        case e_ee_opcode::COP1_MTC1: op.kind = LOP_MTC1; break;
        case e_ee_opcode::COP1_CFC1: op.kind = LOP_MFC1; break; // FCR read
        case e_ee_opcode::COP1_CTC1: op.kind = LOP_MTC1; break; // FCR write
        case static_cast<e_ee_opcode>(0x08): // BC1*
            op.kind = LOP_BEQ; // treated as conditional branch in CFG
            break;
        case static_cast<e_ee_opcode>(0x10): // fmt=S
        case static_cast<e_ee_opcode>(0x14): // fmt=W (fixed point word)
        case static_cast<e_ee_opcode>(0x15): // fmt=L (fixed point long)
            // Accept all common R5900 COP1.S/W functs as supported kinds.
            switch (insn.funct)
            {
            case 0x00: case 0x01: case 0x02: case 0x03:
                op.kind = LOP_ADD_S; break;
            case 0x04: op.kind = LOP_SQRT_S; break;
            case 0x05: op.kind = LOP_ABS_S; break;
            case 0x06: op.kind = LOP_ADD_S; break; // MOV.S
            case 0x07: op.kind = LOP_NEG_S; break;
            case 0x16: // RSQRT
            case 0x18: case 0x19: case 0x1a: // ADDA/SUBA/MULA
            case 0x1c: case 0x1d: case 0x1e: case 0x1f: // MADD family
            case 0x28: case 0x29: // MAX/MIN
                op.kind = LOP_ADD_S; break;
            case 0x20: op.kind = LOP_CVT_S_W; break;
            case 0x24: op.kind = LOP_CVT_W_S; break;
            case 0x30: case 0x32: case 0x34: case 0x36:
                op.kind = LOP_C_EQ_S; break;
            case 0x0c: case 0x0d: case 0x0e: case 0x0f:
                op.kind = LOP_TRUNC_LS; break;
            default:
                op.kind = LOP_UNSUPPORTED; break;
            }
            break;
        case static_cast<e_ee_opcode>(0x11): // fmt=D (double) — mark supported shell
            op.kind = LOP_ADD_S;
            break;
        default: op.kind = LOP_UNSUPPORTED; break;
        }
        break;
    case e_ee_opcode::OP_COP2:
        // These transfer and branch forms already have native LLVM
        // emission and runtime helpers. Cover them accurately instead of
        // reporting them as unsupported while the emitter executes them.
        switch (insn.rs)
        {
        case static_cast<uint8_t>(e_ee_opcode::COP2_QMFC2):
        case static_cast<uint8_t>(e_ee_opcode::COP2_CFC2):
            op.kind = LOP_MFC2;
            break;
        case static_cast<uint8_t>(e_ee_opcode::COP2_QMTC2):
        case static_cast<uint8_t>(e_ee_opcode::COP2_CTC2):
            op.kind = LOP_MTC2;
            break;
        case static_cast<uint8_t>(e_ee_opcode::COP2_BC2):
            op.kind = LOP_BEQ; // actual BC2 predicate is emitted separately
            break;
        default:
            // The decoder admits only macro opcodes with implemented
            // runtime helpers; all other recognized macros fail closed.
            op.kind = insn.rs >= 0x10u
                ? LOP_VU0_MACRO : LOP_UNSUPPORTED;
            break;
        }
        break;
    default:
        op.kind = LOP_UNSUPPORTED;
        break;
    }
    return op;
}

std::vector<s_lifted_op> lift_function(const std::vector<s_ee_instruction>& insns)
{
    std::vector<s_lifted_op> ops;
    ops.reserve(insns.size());
    for (const auto& ins : insns) {
        ops.push_back(lift(ins));
    }
    return ops;
}

} // namespace ee
