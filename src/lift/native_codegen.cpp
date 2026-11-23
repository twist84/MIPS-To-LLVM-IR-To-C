#include "native_codegen.h"
#include "ee_opcode.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>

namespace ee {

namespace
{
std::string u64hex(uint64_t v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llxULL", (unsigned long long)v);
    return buf;
}

std::string u32hex(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08x", v);
    return buf;
}

std::string sext32_expr(const std::string& v)
{
    return "(int64_t)(int32_t)(uint32_t)(" + v + ")";
}
} // namespace

std::string native_codegen::fresh()
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "t%d", temp_++);
    return buf;
}

void native_codegen::touched_regs(const std::vector<s_lifted_op>& ops,
                                  std::vector<uint8_t>& out) const
{
    std::map<uint8_t, bool> seen;
    for (const auto& op : ops)
    {
        switch (op.kind)
        {
        case LOP_ADDIU:
        case LOP_DADDIU:
        case LOP_ANDI:
        case LOP_ORI:
        case LOP_XORI:
        case LOP_LUI:
        case LOP_SLTI:
        case LOP_SLTIU:
        case LOP_LB:
        case LOP_LH:
        case LOP_LBU:
        case LOP_LHU:
        case LOP_LW:
        case LOP_LWU:
        case LOP_LD:
        case LOP_LWL:
        case LOP_LWR:
            seen[op.rt] = true;
            if (op.rs != 0) seen[op.rs] = true;
            break;
        case LOP_ADDU:
        case LOP_SUBU:
        case LOP_AND:
        case LOP_OR:
        case LOP_XOR:
        case LOP_NOR:
        case LOP_SLT:
        case LOP_SLTU:
        case LOP_SLL:
        case LOP_SRL:
        case LOP_SRA:
        case LOP_ROTR:
        case LOP_ROTRV:
            seen[op.rd] = true;
            if (op.rs != 0) seen[op.rs] = true;
            seen[op.rt] = true;
            break;
        case LOP_SB:
        case LOP_SH:
        case LOP_SW:
        case LOP_SD:
        case LOP_SWL:
        case LOP_SDL:
            if (op.rs != 0) seen[op.rs] = true;
            seen[op.rt] = true;
            break;
        case LOP_MFHI:
        case LOP_MFLO:
        case LOP_MFHI1:
        case LOP_MFLO1:
            seen[op.rd] = true;
            break;
        case LOP_MTHI:
        case LOP_MTLO:
        case LOP_MTHI1:
        case LOP_MTLO1:
        case LOP_MULT:
        case LOP_MULTU:
        case LOP_DIV:
        case LOP_DIVU:
        case LOP_MTSAB:
        case LOP_MTSAH:
            if ((op.kind == LOP_MULT || op.kind == LOP_MULTU) &&
                op.rd != 0)
            {
                seen[op.rd] = true;
            }
            if (op.rs != 0) seen[op.rs] = true;
            if (op.kind != LOP_MTSAB && op.kind != LOP_MTSAH && op.rt != 0)
                seen[op.rt] = true;
            break;
        case LOP_MMI_AND:
        case LOP_MMI_OR:
        case LOP_MMI_XOR:
        case LOP_MMI_NOR:
        case LOP_MMI_ADD_B:
        case LOP_MMI_ADD_H:
        case LOP_MMI_ADD_W:
        case LOP_MMI_ADD_D:
        case LOP_MMI_SUB_B:
        case LOP_MMI_SUB_H:
        case LOP_MMI_SUB_W:
        case LOP_MMI_SUB_D:
        case LOP_MMI_ADDU_SAT_B:
        case LOP_MMI_ADDU_SAT_H:
        case LOP_MMI_ADDU_SAT_W:
        case LOP_MMI_SUBU_SAT_B:
        case LOP_MMI_SUBU_SAT_H:
        case LOP_MMI_SUBU_SAT_W:
        case LOP_MMI_SLL:
        case LOP_MMI_SRL:
        case LOP_MMI_SRA:
        case LOP_MMI_MAXU_B:
        case LOP_MMI_MAXU_H:
        case LOP_MMI_MAXU_W:
        case LOP_MMI_MAXS_B:
        case LOP_MMI_MAXS_H:
        case LOP_MMI_MAXS_W:
        case LOP_MMI_MINU_B:
        case LOP_MMI_MINU_H:
        case LOP_MMI_MINU_W:
        case LOP_MMI_MINS_B:
        case LOP_MMI_MINS_H:
        case LOP_MMI_MINS_W:
        case LOP_MMI_ABS_H:
        case LOP_MMI_ABS_W:
        case LOP_MMI_CEQ_B:
        case LOP_MMI_CEQ_H:
        case LOP_MMI_CEQ_W:
        case LOP_MMI_ADDSUB_H:
        case LOP_MMI_EXTU_B:
        case LOP_MMI_EXTU_H:
        case LOP_MMI_EXTU_W:
        case LOP_MMI_QFSRV:
        case LOP_MMI_SHUFFLE:
        case LOP_MMI_PACK:
            seen[op.rd] = true;
            if (op.rs != 0) seen[op.rs] = true;
            if (op.rt != 0) seen[op.rt] = true;
            break;
        case LOP_MFC1:
        case LOP_MTC1:
        case LOP_MFC0:
        case LOP_MTC0:
        case LOP_MFC2:
        case LOP_MTC2:
            if (op.rs != 0) seen[op.rs] = true;
            seen[op.rt] = true;
            seen[op.rd] = true;
            break;
        case LOP_LWC1:
        case LOP_LQC2:
            seen[op.rt] = true;
            if (op.rs != 0) seen[op.rs] = true;
            break;
        default:
            break;
        }
    }
    out.clear();
    for (auto& kv : seen) out.push_back(kv.first);
}

void native_codegen::emit_op_body(const s_lifted_op& op)
{
    using K = e_lift_op_kind;
    out_ += "    // pc=";
    out_ += u32hex(op.pc);
    out_ += "\n";

    switch (op.kind)
    {
    case K::LOP_ADDIU:
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_lo = ";
        out_ += "(uint64_t)(" + sext32_expr("g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo") + " + " +
                std::to_string((int)op.immediate) + ");\n";
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_hi = 0;\n";
        break;
    case K::LOP_DADDIU:
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_lo = ";
        out_ += "(uint64_t)((int64_t)g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo + " +
                std::to_string((int)op.immediate) + ");\n";
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_hi = 0;\n";
        break;
    case K::LOP_ADDU:
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rd) + "].m_lo = ";
        out_ += "g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo + g_cpu.m_gpr[" +
                std::to_string(op.rt) + "].m_lo;\n";
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rd) + "].m_hi = 0;\n";
        break;
    case K::LOP_SUBU:
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rd) + "].m_lo = ";
        out_ += "g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo - g_cpu.m_gpr[" +
                std::to_string(op.rt) + "].m_lo;\n";
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rd) + "].m_hi = 0;\n";
        break;
    case K::LOP_AND:
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rd) + "].m_lo = ";
        out_ += "g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo & g_cpu.m_gpr[" +
                std::to_string(op.rt) + "].m_lo;\n";
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rd) + "].m_hi = 0;\n";
        break;
    case K::LOP_OR:
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rd) + "].m_lo = ";
        out_ += "g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo | g_cpu.m_gpr[" +
                std::to_string(op.rt) + "].m_lo;\n";
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rd) + "].m_hi = 0;\n";
        break;
    case K::LOP_XOR:
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rd) + "].m_lo = ";
        out_ += "g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo ^ g_cpu.m_gpr[" +
                std::to_string(op.rt) + "].m_lo;\n";
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rd) + "].m_hi = 0;\n";
        break;
    case K::LOP_NOR:
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rd) + "].m_lo = ";
        out_ += "~(g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo | g_cpu.m_gpr[" +
                std::to_string(op.rt) + "].m_lo);\n";
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rd) + "].m_hi = 0;\n";
        break;
    case K::LOP_SLT:
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rd) + "].m_lo = ";
        out_ += "(" + sext32_expr("g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo") + " < " +
                sext32_expr("g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_lo") + ") ? 1 : 0;\n";
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rd) + "].m_hi = 0;\n";
        break;
    case K::LOP_SLTU:
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rd) + "].m_lo = ";
        out_ += "(g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo < g_cpu.m_gpr[" +
                std::to_string(op.rt) + "].m_lo) ? 1 : 0;\n";
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rd) + "].m_hi = 0;\n";
        break;
    case K::LOP_SLTI:
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_lo = ";
        out_ += "(" + sext32_expr("g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo") + " < " +
                std::to_string((int)op.immediate) + ") ? 1 : 0;\n";
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_hi = 0;\n";
        break;
    case K::LOP_SLTIU:
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_lo = ";
        out_ += "(g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo < " +
                std::to_string(op.uimmediate) + "ULL) ? 1 : 0;\n";
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_hi = 0;\n";
        break;
    case K::LOP_ANDI:
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_lo = ";
        out_ += "g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo & " +
                std::to_string(op.uimmediate) + "ULL;\n";
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_hi = 0;\n";
        break;
    case K::LOP_ORI:
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_lo = ";
        out_ += "g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo | " +
                std::to_string(op.uimmediate) + "ULL;\n";
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_hi = 0;\n";
        break;
    case K::LOP_XORI:
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_lo = ";
        out_ += "g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo ^ " +
                std::to_string(op.uimmediate) + "ULL;\n";
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_hi = 0;\n";
        break;
    case K::LOP_LUI:
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_lo = ";
        out_ += "((uint64_t)" + std::to_string(op.uimmediate) + " << 16);\n";
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_hi = 0;\n";
        break;
    case K::LOP_MFHI1:
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rd) + "].m_lo = g_cpu.m_hi1;\n";
        break;
    case K::LOP_MTHI1:
        out_ += "    g_cpu.m_hi1 = g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo;\n";
        break;
    case K::LOP_MTSAB:
    case K::LOP_MTSAH:
    {
        const bool is_byte = op.kind == K::LOP_MTSAB;
        const uint32_t lane_mask = is_byte ? 0x0fu : 0x07u;
        const uint32_t bit_shift = is_byte ? 3u : 4u;
        out_ += "    g_cpu.m_sa = ((((uint32_t)g_cpu.m_gpr[" +
                std::to_string(op.rs) + "].m_lo & " +
                std::to_string(lane_mask) + "u) ^ " +
                std::to_string(op.uimmediate & lane_mask) + "u) << " +
                std::to_string(bit_shift) + "u);\n";
        break;
    }
    case K::LOP_MFLO1:
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rd) + "].m_lo = g_cpu.m_lo1;\n";
        break;
    case K::LOP_MTLO1:
        out_ += "    g_cpu.m_lo1 = g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo;\n";
        break;
    case K::LOP_LW:
    {
        out_ += "    {\n";
        out_ += "        uint32_t __ea = (uint32_t)(" + sext32_expr("g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo") + " + " +
                std::to_string((int)op.immediate) + ");\n";
        out_ += "        uint32_t __v = ee_mem_read32(&g_rt, __ea);\n";
        out_ += "        g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_lo = (uint64_t)(int32_t)__v;\n";
        out_ += "        g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_hi = 0;\n";
        out_ += "    }\n";
        break;
    }
    case K::LOP_SW:
    {
        out_ += "    {\n";
        out_ += "        uint32_t __ea = (uint32_t)(" + sext32_expr("g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo") + " + " +
                std::to_string((int)op.immediate) + ");\n";
        out_ += "        ee_mem_write32(&g_rt, __ea, (uint32_t)g_cpu.m_gpr[" +
                std::to_string(op.rt) + "].m_lo);\n";
        out_ += "    }\n";
        break;
    }
    case K::LOP_MFC0:
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_lo = g_cpu.m_cop0[" +
                std::to_string(op.rd) + "];\n";
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_hi = 0;\n";
        break;
    case K::LOP_MTC0:
        out_ += "    ee_cop0_write(&g_cpu, " + std::to_string(op.rd) +
                ", (uint32_t)g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_lo);\n";
        break;
    case K::LOP_MFC1:
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_lo = (uint64_t)(uint32_t)g_cpu.m_fpr[" +
                std::to_string(op.rd) + "];\n";
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_hi = 0;\n";
        break;
    case K::LOP_MTC1:
        out_ += "    g_cpu.m_fpr[" + std::to_string(op.rd) + "] = (uint64_t)(uint32_t)g_cpu.m_gpr[" +
                std::to_string(op.rt) + "].m_lo;\n";
        break;
    case K::LOP_LWC1:
    {
        out_ += "    {\n";
        out_ += "        uint32_t __ea = (uint32_t)(" + sext32_expr("g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo") + " + " +
                std::to_string((int)op.immediate) + ");\n";
        out_ += "        g_cpu.m_fpr[" + std::to_string(op.rt) + "] = (uint64_t)ee_mem_read32(&g_rt, __ea);\n";
        out_ += "    }\n";
        break;
    }
    case K::LOP_SWC1:
    {
        out_ += "    {\n";
        out_ += "        uint32_t __ea = (uint32_t)(" + sext32_expr("g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo") + " + " +
                std::to_string((int)op.immediate) + ");\n";
        out_ += "        ee_mem_write32(&g_rt, __ea, (uint32_t)g_cpu.m_fpr[" +
                std::to_string(op.rt) + "]);\n";
        out_ += "    }\n";
        break;
    }
    case K::LOP_MFC2:
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_lo = (uint64_t)(uint32_t)g_cpu.m_vu0[" +
                std::to_string(op.rd) + "].m_lo;\n";
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_hi = 0;\n";
        break;
    case K::LOP_MTC2:
        out_ += "    g_cpu.m_vu0[" + std::to_string(op.rd) + "].m_lo = (uint64_t)(uint32_t)g_cpu.m_gpr[" +
                std::to_string(op.rt) + "].m_lo;\n";
        out_ += "    g_cpu.m_vu0[" + std::to_string(op.rd) + "].m_hi = 0;\n";
        break;
    case K::LOP_LQC2:
    {
        out_ += "    {\n";
        out_ += "        uint32_t __ea = (uint32_t)(" + sext32_expr("g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo") + " + " +
                std::to_string((int)op.immediate) + ");\n";
        out_ += "        g_cpu.m_vu0[" + std::to_string(op.rt) + "].m_lo = ee_mem_read64(&g_rt, __ea);\n";
        out_ += "        g_cpu.m_vu0[" + std::to_string(op.rt) + "].m_hi = ee_mem_read64(&g_rt, __ea + 8);\n";
        out_ += "    }\n";
        break;
    }
    case K::LOP_SQC2:
    {
        out_ += "    {\n";
        out_ += "        uint32_t __ea = (uint32_t)(" + sext32_expr("g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo") + " + " +
                std::to_string((int)op.immediate) + ");\n";
        out_ += "        ee_mem_write64(&g_rt, __ea, g_cpu.m_vu0[" + std::to_string(op.rt) + "].m_lo);\n";
        out_ += "        ee_mem_write64(&g_rt, __ea + 8, g_cpu.m_vu0[" + std::to_string(op.rt) + "].m_hi);\n";
        out_ += "    }\n";
        break;
    }
    case K::LOP_MULT:
    case K::LOP_MULTU:
    {
        const std::string a = "g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo";
        const std::string b = "g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_lo";
        out_ += "    {\n";
        if (op.kind == K::LOP_MULT)
        {
            out_ += "        int64_t __a = " + sext32_expr(a) + ";\n";
            out_ += "        int64_t __b = " + sext32_expr(b) + ";\n";
            out_ += "        uint64_t __p = (uint64_t)(__a * __b);\n";
        }
        else
        {
            out_ += "        uint64_t __a = (uint32_t)(" + a + ");\n";
            out_ += "        uint64_t __b = (uint32_t)(" + b + ");\n";
            out_ += "        uint64_t __p = __a * __b;\n";
        }
        out_ += "        g_cpu.m_lo = (uint64_t)(int64_t)(int32_t)(uint32_t)__p;\n";
        out_ += "        g_cpu.m_hi = (uint64_t)(int64_t)(int32_t)(uint32_t)(__p >> 32);\n";
        if (op.rd != 0)
        {
            out_ += "        g_cpu.m_gpr[" + std::to_string(op.rd) + "].m_lo = g_cpu.m_lo;\n";
        }
        out_ += "    }\n";
        break;
    }
    case K::LOP_CHECKED_SIGNED:
    case K::LOP_CHECKED_TRAP:
    case K::LOP_MMI0_EXACT:
    case K::LOP_MMI_SHIFT:
    case K::LOP_MMI_PLZCW:
        // The legacy source generator cannot implement precise exceptions
        // or the newly decoded packed MMI operations. Fail closed.
        out_ += "    #error Checked EE op requires LLVM lowering\n";
        break;
    case K::LOP_DIV:
    case K::LOP_DIVU:
    {
        const std::string a = "g_cpu.m_gpr[" + std::to_string(op.rs) + "].m_lo";
        const std::string b = "g_cpu.m_gpr[" + std::to_string(op.rt) + "].m_lo";
        out_ += "    {\n";
        if (op.kind == K::LOP_DIV)
        {
            // Use signed 64-bit intermediates for 32-bit operands. The
            // INT_MIN/-1 result is specified explicitly so it never depends
            // on a narrowing conversion outside int32_t's range.
            out_ += "        int64_t __a = " + sext32_expr(a) + ";\n";
            out_ += "        int64_t __b = " + sext32_expr(b) + ";\n";
            out_ += "        if (__b != 0)\n";
            out_ += "        {\n";
            out_ += "            if (__a == -2147483648LL && __b == -1)\n";
            out_ += "            {\n";
            out_ += "                g_cpu.m_lo = 0xffffffff80000000ULL;\n";
            out_ += "                g_cpu.m_hi = 0;\n";
            out_ += "            }\n";
            out_ += "            else\n";
            out_ += "            {\n";
            out_ += "                g_cpu.m_lo = (uint64_t)(int64_t)(int32_t)(__a / __b);\n";
            out_ += "                g_cpu.m_hi = (uint64_t)(int64_t)(int32_t)(__a % __b);\n";
            out_ += "            }\n";
            out_ += "        }\n";
        }
        else
        {
            // Unsigned word division still sign-extends its low 32-bit
            // quotient/remainder into the 64-bit EE HI/LO registers.
            out_ += "        uint64_t __a = (uint32_t)(" + a + ");\n";
            out_ += "        uint64_t __b = (uint32_t)(" + b + ");\n";
            out_ += "        if (__b != 0)\n";
            out_ += "        {\n";
            out_ += "            uint32_t __q = (uint32_t)(__a / __b);\n";
            out_ += "            uint32_t __r = (uint32_t)(__a % __b);\n";
            out_ += "            g_cpu.m_lo = (__q & 0x80000000u) ? (0xffffffff00000000ULL | __q) : __q;\n";
            out_ += "            g_cpu.m_hi = (__r & 0x80000000u) ? (0xffffffff00000000ULL | __r) : __r;\n";
            out_ += "        }\n";
        }
        // The manual leaves divide-by-zero outputs undefined; matching the
        // LLVM and DIV1 paths, preserve the prior HI/LO bank deterministically.
        out_ += "    }\n";
        break;
    }
    case K::LOP_MMI_AND:
    case K::LOP_MMI_OR:
    case K::LOP_MMI_XOR:
    case K::LOP_MMI_NOR:
    case K::LOP_MMI_ADD_B:
    case K::LOP_MMI_ADD_H:
    case K::LOP_MMI_ADD_W:
    case K::LOP_MMI_ADD_D:
    case K::LOP_MMI_SUB_B:
    case K::LOP_MMI_SUB_H:
    case K::LOP_MMI_SUB_W:
    case K::LOP_MMI_SUB_D:
    case K::LOP_MMI_ADDU_SAT_B:
    case K::LOP_MMI_ADDU_SAT_H:
    case K::LOP_MMI_ADDU_SAT_W:
    case K::LOP_MMI_SLL:
    case K::LOP_MMI_SRL:
    case K::LOP_MMI_SRA:
    case K::LOP_MMI_MAXU_B:
    case K::LOP_MMI_MAXU_H:
    case K::LOP_MMI_MAXU_W:
    case K::LOP_MMI_MAXS_B:
    case K::LOP_MMI_MAXS_H:
    case K::LOP_MMI_MAXS_W:
    case K::LOP_MMI_MINU_B:
    case K::LOP_MMI_MINU_H:
    case K::LOP_MMI_MINU_W:
    case K::LOP_MMI_MINS_B:
    case K::LOP_MMI_MINS_H:
    case K::LOP_MMI_MINS_W:
    case K::LOP_MMI_ABS_H:
    case K::LOP_MMI_ABS_W:
    case K::LOP_MMI_CEQ_B:
    case K::LOP_MMI_CEQ_H:
    case K::LOP_MMI_CEQ_W:
    case K::LOP_MMI_SUBU_SAT_B:
    case K::LOP_MMI_SUBU_SAT_H:
    case K::LOP_MMI_SUBU_SAT_W:
    case K::LOP_MMI_ADDSUB_H:
    case K::LOP_MMI_EXTU_B:
    case K::LOP_MMI_EXTU_H:
    case K::LOP_MMI_EXTU_W:
    case K::LOP_MMI_QFSRV:
    case K::LOP_MMI_SHUFFLE:
    case K::LOP_MMI_PACK:
    {
        out_ += "    g_cpu.m_gpr[" + std::to_string(op.rd) + "] = mmi_apply((e_lift_op_kind)" +
                std::to_string((int)op.kind) + ", g_cpu.m_gpr[" + std::to_string(op.rs) + "], " +
                "g_cpu.m_gpr[" + std::to_string(op.rt) + "], " + (op.kind == K::LOP_MMI_QFSRV ? "g_cpu.m_sa" : std::to_string(op.sa)) + "));\n";
        break;
    }
    default:
        // Control-transfer / unsupported: terminate straight-line region.
        out_ += "    // unsupported/control op at " + u32hex(op.pc) + ": stopping straight-line emission\n";
        break;
    }
}

std::string native_codegen::emit_program(
    const std::vector<s_lifted_op>& ops,
    uint32_t entry_pc,
    const char* func_name,
    const std::map<uint8_t, uint64_t>& init_gpr,
    const std::map<uint32_t, uint32_t>& init_mem)
{
    (void)func_name;
    std::vector<uint8_t> touched;
    touched_regs(ops, touched);

    std::ostringstream os;
    os << "// Auto-generated native translation of guest function at " << u32hex(entry_pc) << "\n";
    os << "#include <cstdint>\n";
    os << "#include <cstdio>\n";
    os << "#include \"ee_state.h\"\n";
    os << "#include \"ee_runtime.h\"\n";
    os << "#include \"ee_memory.h\"\n";
    os << "#include \"instruction_lifter.h\"\n";
    os << "#include \"mmi_ops.h\"\n";
    os << "\n";
    os << "using namespace ee;\n";
    os << "\n";
    os << "static s_ee_cpu_state g_cpu;\n";
    os << "static s_ee_runtime g_rt = { nullptr };\n";
    os << "static ee_memory g_mem(16u * 1024u * 1024u);\n";
    os << "\n";
    os << "static void print_state() {\n";
    os << "    std::printf(\"pc %08x\\n\", (unsigned)g_cpu.m_pc);\n";
    os << "    std::printf(\"exit %u\\n\", (unsigned)g_cpu.m_exit_reason);\n";
    os << "    std::printf(\"hi %016llx lo %016llx hi1 %016llx lo1 %016llx\\n\",\n";
    os << "        (unsigned long long)g_cpu.m_hi, (unsigned long long)g_cpu.m_lo,\n";
    os << "        (unsigned long long)g_cpu.m_hi1, (unsigned long long)g_cpu.m_lo1);\n";
    os << "    std::printf(\"mem200000 %08x\\n\", (unsigned)ee_mem_read32(&g_rt, 0x00200000u));\n";
    os << "    for (int r = 0; r < 32; ++r) {\n";
    os << "        std::printf(\"gpr%d %016llx %016llx\\n\", r,\n";
    os << "            (unsigned long long)g_cpu.m_gpr[r].m_lo,\n";
    os << "            (unsigned long long)g_cpu.m_gpr[r].m_hi);\n";
    os << "    }\n";
    os << "}\n";
    os << "\n";
    os << "int main() {\n";
    os << "    g_rt.m_user = &g_mem;\n";
    for (auto& kv : init_mem) {
        os << "    ee_mem_write32(&g_rt, " << u32hex(kv.first) << ", " << u32hex(kv.second) << "u);\n";
    }
    for (auto& kv : init_gpr) {
        os << "    g_cpu.m_gpr[" << (int)kv.first << "].m_lo = " << u64hex(kv.second) << ";\n";
        os << "    g_cpu.m_gpr[" << (int)kv.first << "].m_hi = 0;\n";
    }
    os << "    g_cpu.m_pc = " << u32hex(entry_pc) << ";\n";
    os << "\n";
    for (const auto& op : ops) {
        emit_op_body(op);
    }
    if (!ops.empty()) {
        os << "    g_cpu.m_pc = " << u32hex(ops.back().pc + 4) << ";\n";
    }
    os << out_;
    out_.clear();
    os << "    g_cpu.m_exit_reason = (uint32_t)e_ee_exit_reason::EE_EXIT_RETURN;\n";
    os << "    print_state();\n";
    os << "    return 0;\n";
    os << "}\n";
    return os.str();
}

std::string native_codegen::emit_function_body(
    const std::vector<s_lifted_op>& ops,
    uint32_t entry_pc,
    const char* func_name)
{
    (void)entry_pc;
    std::ostringstream os;
    os << "// Auto-generated native translation of guest function at " << u32hex(entry_pc) << "\n";
    os << "#include <cstdint>\n";
    os << "#include \"ee_state.h\"\n";
    os << "#include \"ee_runtime.h\"\n";
    os << "#include \"ee_memory.h\"\n";
    os << "#include \"instruction_lifter.h\"\n";
    os << "#include \"mmi_ops.h\"\n";
    os << "\n";
    os << "using namespace ee;\n";
    os << "\n";
    os << "extern \"C\" uint32_t " << func_name << "(ee::s_ee_cpu_state* cpu, ee::s_ee_runtime* rt) {\n";
    os << "\n";
    for (const auto& op : ops) {
        // Each op uses cpu-> and rt instead of g_cpu/g_rt globals.
        // Temporarily substitute: we rebuild the op body with the right prefixes.
        out_ = "";
        emit_op_body(op);
        // Replace g_cpu. -> cpu-> and &g_rt -> rt in the emitted body.
        std::string body = out_;
        out_.clear();
        // Replace all "g_cpu." with "cpu->"
        size_t pos = 0;
        while ((pos = body.find("g_cpu.", pos)) != std::string::npos) {
            body.replace(pos, 6, "cpu->");
            pos += 4;
        }
        // Replace address-of global CPU state when the emitted operation
        // calls a runtime helper; the callable ABI passes CPU by pointer.
        pos = 0;
        while ((pos = body.find("&g_cpu", pos)) != std::string::npos) {
            body.replace(pos, 6, "cpu");
            pos += 3;
        }
        // Replace all "&g_rt" with "rt"
        pos = 0;
        while ((pos = body.find("&g_rt", pos)) != std::string::npos) {
            body.replace(pos, 5, "rt");
            pos += 2;
        }
        os << body;
    }
    // The callable-function ABI must publish the same architectural exit state
    // as emit_program(); returning an enum alone leaves the guest PC stale.
    if (!ops.empty())
        os << "    cpu->m_pc = " << u32hex(ops.back().pc + 4) << ";\n";
    else
        os << "    cpu->m_pc = " << u32hex(entry_pc) << ";\n";
    os << "    cpu->m_exit_reason = (uint32_t)e_ee_exit_reason::EE_EXIT_RETURN;\n";
    os << "    return (uint32_t)e_ee_exit_reason::EE_EXIT_RETURN;\n";
    os << "}\n";
    return os.str();
}

} // namespace ee
