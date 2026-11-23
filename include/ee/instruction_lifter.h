#pragma once
#include "ee_instruction.h"
#include <vector>

namespace ee {

enum e_lift_op_kind
{
    LOP_NOP,
    LOP_ADDIU, LOP_DADDIU, LOP_ADDU, LOP_SUBU, LOP_AND, LOP_OR, LOP_XOR, LOP_NOR,
    LOP_SLT, LOP_SLTU, LOP_SLTI, LOP_SLTIU, LOP_ANDI, LOP_ORI, LOP_XORI, LOP_LUI,
    LOP_SLL, LOP_SRL, LOP_SRA, LOP_ROTR, LOP_ROTRV, LOP_SLLV, LOP_SRLV, LOP_SRAV,
    LOP_LB, LOP_LH, LOP_LBU, LOP_LHU, LOP_LW, LOP_LWL, LOP_LWR, LOP_LWU, LOP_LD, LOP_LDL, LOP_LDR, LOP_LQ,
    LOP_SB, LOP_SH, LOP_SW, LOP_SWL, LOP_SWR, LOP_SDL, LOP_SDR, LOP_SD, LOP_SQ,
    LOP_BEQ, LOP_BNE, LOP_BLTZ, LOP_BGEZ, LOP_J, LOP_JAL, LOP_JR, LOP_JALR,
    LOP_MFHI, LOP_MTHI, LOP_MFLO, LOP_MTLO,
    LOP_MADD, LOP_MADDU, LOP_MADD1, LOP_MADDU1,
    LOP_MULT1, LOP_MULTU1, LOP_DIV1, LOP_DIVU1,
    LOP_MFHI1, LOP_MTHI1, LOP_MFLO1, LOP_MTLO1,
    LOP_MULT, LOP_MULTU, LOP_DIV, LOP_DIVU, LOP_MOVZ, LOP_MOVN,
    LOP_MFC0, LOP_MTC0, LOP_ERET, LOP_SYSCALL, LOP_BREAK,
    LOP_MFC1, LOP_MTC1, LOP_LWC1, LOP_SWC1, LOP_CVT_S_W, LOP_CVT_W_S, LOP_ADD_S, LOP_SUB_S,
    LOP_MUL_S, LOP_DIV_S, LOP_C_LT_S, LOP_C_LE_S, LOP_C_EQ_S, LOP_SQRT_S, LOP_ABS_S, LOP_NEG_S,
    LOP_CVT_S_D, LOP_CVT_D_S, LOP_ROUND_LS, LOP_TRUNC_LS, LOP_CEIL_LS, LOP_FLOOR_LS,
    LOP_MFC2, LOP_MTC2, LOP_LQC2, LOP_SQC2,
    // A decoded COP2 macro operation lowered through the VU0 runtime helper.
    // The decoder admits only macro encodings for which that helper provides
    // architectural semantics.
    LOP_VU0_MACRO,
    LOP_VADD_XYZW, LOP_VSUB_XYZW, LOP_VMUL_XYZW, LOP_VMULA_XYZW,
    LOP_VADD_X, LOP_VSUB_X, LOP_VMUL_X, LOP_VMULA_X,
    LOP_VADD_Y, LOP_VSUB_Y, LOP_VMUL_Y, LOP_VMULA_Y,
    LOP_VADD_Z, LOP_VSUB_Z, LOP_VMUL_Z, LOP_VMULA_Z,
    LOP_VADD_W, LOP_VSUB_W, LOP_VMUL_W, LOP_VMULA_W,
    LOP_VBGTZ, LOP_VBLTZ, LOP_VBEQZ, LOP_VBNEZ,
    LOP_MMI_ADD_B, LOP_MMI_ADD_H, LOP_MMI_ADD_W, LOP_MMI_ADD_D,
    LOP_MMI_SUB_B, LOP_MMI_SUB_H, LOP_MMI_SUB_W, LOP_MMI_SUB_D,
    LOP_MMI_ADDU_SAT_B, LOP_MMI_ADDU_SAT_H, LOP_MMI_ADDU_SAT_W,
    LOP_MMI_AND, LOP_MMI_OR, LOP_MMI_XOR, LOP_MMI_NOR, LOP_MMI_SLL, LOP_MMI_SRL, LOP_MMI_SRA,
    LOP_MMI_MAXU_B, LOP_MMI_MAXU_H, LOP_MMI_MAXU_W,
    LOP_MMI_MAXS_B, LOP_MMI_MAXS_H, LOP_MMI_MAXS_W,
    LOP_MMI_MINU_B, LOP_MMI_MINU_H, LOP_MMI_MINU_W,
    LOP_MMI_MINS_B, LOP_MMI_MINS_H, LOP_MMI_MINS_W,
    LOP_MMI_ABS_H, LOP_MMI_ABS_W,
    LOP_MMI_CEQ_B, LOP_MMI_CEQ_H, LOP_MMI_CEQ_W,
    LOP_MMI_SUBU_SAT_B, LOP_MMI_SUBU_SAT_H, LOP_MMI_SUBU_SAT_W,
    LOP_MMI_ADDSUB_H, LOP_MMI_EXTU_B, LOP_MMI_EXTU_H, LOP_MMI_EXTU_W,
    LOP_MMI_QFSRV, LOP_MTSAB, LOP_MTSAH, LOP_MMI_SHUFFLE, LOP_MMI_PACK,
    LOP_MMI_COPY_H, LOP_MMI_COPY_LOWER_D, LOP_MMI_COPY_UPPER_D,
    LOP_EXIT_RETURN,
    LOP_UNSUPPORTED,
    // Added after UNSUPPORTED to preserve existing e_lift_op_kind values.
    // The LLVM IR builder (not the legacy straight-line C++ emitter)
    // implements precise EX_Ov and EX_Tr semantics for these classes.
    LOP_CHECKED_SIGNED,
    LOP_CHECKED_TRAP,
    LOP_MMI0_EXACT,
    LOP_MMI_SHIFT,
    LOP_MMI_PLZCW,
    LOP_MMI_PMFHL,
    LOP_MMI_PMTHL,
    LOP_MMI_HILO_PAIR_TRANSFER,
    LOP_MMI_PACKED_MULT,
    LOP_MMI_PACKED_DIV,
    LOP_MMI_PACKED_MAC,
    LOP_MMI_PACKED_HALF
};

struct s_lifted_op
{
    e_lift_op_kind kind = LOP_UNSUPPORTED;
    uint32_t pc = 0;
    uint8_t rt = 0;
    uint8_t rs = 0;
    uint8_t rd = 0;
    int16_t immediate = 0;
    uint16_t uimmediate = 0;
    uint8_t sa = 0;
    int32_t branch_target = 0;
    uint32_t jump_target = 0;
    bool has_delay_slot = false;
    bool branch_likely = false;
};

s_lifted_op lift(const s_ee_instruction& insn);
std::vector<s_lifted_op> lift_function(const std::vector<s_ee_instruction>& insns);

} // namespace ee
