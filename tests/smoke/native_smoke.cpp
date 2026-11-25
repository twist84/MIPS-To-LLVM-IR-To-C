#include <cstring>
#include "ee_state.h"
#include "ee_runtime.h"
#include "ee_memory.h"
#include "ee_exceptions.h"
#include "instruction_lifter.h"
#include <cstdint>
#include <cstdio>

namespace
{
// 128-bit GPR is stored as two 64-bit halves: m_lo = bits[0:63], m_hi = bits[64:127].
// Lane helpers treat the 128-bit value as little-endian packed lanes.
static inline uint8_t  get8 (const ee::s_ee_u128& v, int i){ return (uint8_t)((i<8?v.m_lo:v.m_hi)>>((i&7)*8)); }
static inline uint16_t get16(const ee::s_ee_u128& v, int i){ return (uint16_t)((i<4?v.m_lo:v.m_hi)>>((i&3)*16)); }
static inline uint32_t get32(const ee::s_ee_u128& v, int i){ return (uint32_t)((i<2?v.m_lo:v.m_hi)>>((i&1)*32)); }
static inline uint64_t get64(const ee::s_ee_u128& v, int i){ return i==0?v.m_lo:v.m_hi; }

static inline ee::s_ee_u128 put8 (ee::s_ee_u128 v, int i, uint8_t  x){ uint64_t& w = i<8?v.m_lo:v.m_hi; int s=(i&7)*8; w=(w&~(0xffULL<<s))|((uint64_t)x<<s); return v; }
static inline ee::s_ee_u128 put16(ee::s_ee_u128 v, int i, uint16_t x){ uint64_t& w = i<4?v.m_lo:v.m_hi; int s=(i&3)*16; w=(w&~(0xffffULL<<s))|((uint64_t)x<<s); return v; }
static inline ee::s_ee_u128 put32(ee::s_ee_u128 v, int i, uint32_t x){ uint64_t& w = i<2?v.m_lo:v.m_hi; int s=(i&1)*32; w=(w&~(0xffffffffULL<<s))|((uint64_t)x<<s); return v; }
static inline ee::s_ee_u128 put64(ee::s_ee_u128 v, int i, uint64_t x){ if(i==0)v.m_lo=x;else v.m_hi=x; return v; }

static inline uint8_t  sat8u (uint32_t v){ return v>0xff?0xff:(uint8_t)v; }
static inline uint16_t sat16u(uint32_t v){ return v>0xffff?0xffff:(uint16_t)v; }
static inline uint32_t sat32u(uint64_t v){ return v>0xffffffffULL?0xffffffffu:(uint32_t)v; }

// 128-bit logical shift right by n (n & 127).
static inline ee::s_ee_u128 shr128(const ee::s_ee_u128& v, int n)
{
    n &= 127;
    if (n == 0)
    {
        return v;
    }
    ee::s_ee_u128 r{};
    if (n < 64) { r.m_hi = v.m_hi >> n; r.m_lo = (v.m_lo >> n) | (v.m_hi << (64 - n)); }
    else { int m = n - 64; r.m_hi = 0; r.m_lo = (m < 64) ? (v.m_hi >> m) : 0; }
    return r;
}
// 128-bit logical shift left by n (n & 127).
static inline ee::s_ee_u128 shl128(const ee::s_ee_u128& v, int n)
{
    n &= 127;
    if (n == 0)
    {
        return v;
    }
    ee::s_ee_u128 r{};
    if (n < 64) { r.m_lo = v.m_lo << n; r.m_hi = (v.m_hi << n) | (v.m_lo >> (64 - n)); }
    else { int m = n - 64; r.m_lo = 0; r.m_hi = (m < 64) ? (v.m_lo << m) : 0; }
    return r;
}
// 128-bit arithmetic shift right by n (n & 127).
static inline ee::s_ee_u128 sar128(const ee::s_ee_u128& v, int n)
{
    n &= 127;
    if (n == 0)
    {
        return v;
    }
    ee::s_ee_u128 r{};
    uint64_t sign = (uint64_t)(int64_t)v.m_hi >> 63;
    if (n < 64) { r.m_hi = (uint64_t)(int64_t)v.m_hi >> n; r.m_lo = (v.m_lo >> n) | ((uint64_t)(int64_t)v.m_hi << (64 - n)); }
    else { int m = n - 64; r.m_hi = sign ? ~0ULL : 0; r.m_lo = (m < 64) ? (uint64_t)(int64_t)v.m_hi >> m : (sign ? ~0ULL : 0); }
    return r;
}
} // namespace

namespace ee {

// Native mirror of the lifted operations. Mirrors the semantics that the LLVM
// IR lifter would emit (target-independent i64/i32 + memory helpers). This is
// compiled to a native object and linked, simulating the IR -> object path.
// Executes a single lifted op (no control-flow transfer; branches handled by
// the caller via explicit pc update).
void exec_op(const s_lifted_op& op, s_ee_cpu_state& cpu,
             ee_memory& mem, s_ee_runtime* rt, uint32_t& next_pc)
{
    using ee::e_lift_op_kind;
    auto sext32 = [](uint64_t v) -> int64_t { return (int64_t)(int32_t)(uint32_t)v; };
    auto zext32 = [](uint64_t v) -> uint64_t { return (uint32_t)v; };

    next_pc = op.pc + 4;

    switch (op.kind)
    {
        case LOP_NOP:
            break;

        case LOP_ADDIU:
        {
            int64_t rs_v = sext32(cpu.m_gpr[op.rs].m_lo);
            int64_t imm = (int64_t)op.immediate;
            cpu.m_gpr[op.rt].m_lo = (uint64_t)(rs_v + imm);
            cpu.m_gpr[op.rt].m_hi = 0;
            break;
        }

        case LOP_DADDIU:
    {
        int64_t rs_v = (int64_t)cpu.m_gpr[op.rs].m_lo;
        int64_t imm = (int64_t)op.immediate;
        cpu.m_gpr[op.rt].m_lo = (uint64_t)(rs_v + imm);
        cpu.m_gpr[op.rt].m_hi = 0;
        break;
    }
    case LOP_ADDU:
        cpu.m_gpr[op.rd].m_lo = cpu.m_gpr[op.rs].m_lo + cpu.m_gpr[op.rt].m_lo;
        cpu.m_gpr[op.rd].m_hi = 0;
        break;
    case LOP_SUBU:
        cpu.m_gpr[op.rd].m_lo = cpu.m_gpr[op.rs].m_lo - cpu.m_gpr[op.rt].m_lo;
        cpu.m_gpr[op.rd].m_hi = 0;
        break;
    case LOP_AND:
        cpu.m_gpr[op.rd].m_lo = cpu.m_gpr[op.rs].m_lo & cpu.m_gpr[op.rt].m_lo;
        cpu.m_gpr[op.rd].m_hi = 0;
        break;
    case LOP_OR:
        cpu.m_gpr[op.rd].m_lo = cpu.m_gpr[op.rs].m_lo | cpu.m_gpr[op.rt].m_lo;
        cpu.m_gpr[op.rd].m_hi = 0;
        break;
    case LOP_XOR:
        cpu.m_gpr[op.rd].m_lo = cpu.m_gpr[op.rs].m_lo ^ cpu.m_gpr[op.rt].m_lo;
        cpu.m_gpr[op.rd].m_hi = 0;
        break;
    case LOP_NOR:
        cpu.m_gpr[op.rd].m_lo = ~(cpu.m_gpr[op.rs].m_lo | cpu.m_gpr[op.rt].m_lo);
        cpu.m_gpr[op.rd].m_hi = 0;
        break;
    case LOP_MOVZ:
        if (cpu.m_gpr[op.rt].m_lo != 0)
            cpu.m_gpr[op.rd].m_lo = cpu.m_gpr[op.rs].m_lo;
        cpu.m_gpr[op.rd].m_hi = 0;
        break;
    case LOP_MOVN:
        if (cpu.m_gpr[op.rt].m_lo == 0)
            cpu.m_gpr[op.rd].m_lo = cpu.m_gpr[op.rs].m_lo;
        cpu.m_gpr[op.rd].m_hi = 0;
        break;
    case LOP_SLT:
        cpu.m_gpr[op.rd].m_lo = (sext32(cpu.m_gpr[op.rs].m_lo) < sext32(cpu.m_gpr[op.rt].m_lo)) ? 1 : 0;
        cpu.m_gpr[op.rd].m_hi = 0;
        break;
    case LOP_SLTU:
        cpu.m_gpr[op.rd].m_lo = (cpu.m_gpr[op.rs].m_lo < cpu.m_gpr[op.rt].m_lo) ? 1 : 0;
        cpu.m_gpr[op.rd].m_hi = 0;
        break;
    case LOP_SLTI:
        cpu.m_gpr[op.rt].m_lo = (sext32(cpu.m_gpr[op.rs].m_lo) < (int64_t)op.immediate) ? 1 : 0;
        cpu.m_gpr[op.rt].m_hi = 0;
        break;
    case LOP_SLTIU:
        cpu.m_gpr[op.rt].m_lo = (cpu.m_gpr[op.rs].m_lo < (uint64_t)op.uimmediate) ? 1 : 0;
        cpu.m_gpr[op.rt].m_hi = 0;
        break;
    case LOP_ANDI:
        cpu.m_gpr[op.rt].m_lo = cpu.m_gpr[op.rs].m_lo & op.uimmediate;
        cpu.m_gpr[op.rt].m_hi = 0;
        break;
    case LOP_ORI:
        cpu.m_gpr[op.rt].m_lo = cpu.m_gpr[op.rs].m_lo | op.uimmediate;
        cpu.m_gpr[op.rt].m_hi = 0;
        break;
    case LOP_XORI:
        cpu.m_gpr[op.rt].m_lo = cpu.m_gpr[op.rs].m_lo ^ op.uimmediate;
        cpu.m_gpr[op.rt].m_hi = 0;
        break;
    case LOP_LUI:
        cpu.m_gpr[op.rt].m_lo = (uint64_t)op.uimmediate << 16;
        cpu.m_gpr[op.rt].m_hi = 0;
        break;

    case LOP_SLL:
        cpu.m_gpr[op.rd].m_lo = cpu.m_gpr[op.rt].m_lo << op.sa;
        cpu.m_gpr[op.rd].m_hi = 0;
        break;
    case LOP_SRL:
        cpu.m_gpr[op.rd].m_lo = cpu.m_gpr[op.rt].m_lo >> op.sa;
        cpu.m_gpr[op.rd].m_hi = 0;
        break;
    case LOP_SRA:
        cpu.m_gpr[op.rd].m_lo = (uint64_t)((int64_t)(int32_t)cpu.m_gpr[op.rt].m_lo >> op.sa);
        cpu.m_gpr[op.rd].m_hi = 0;
        break;
    case LOP_ROTR:
    {
        uint32_t v = (uint32_t)cpu.m_gpr[op.rt].m_lo;
        uint32_t s = op.sa & 31;
        cpu.m_gpr[op.rd].m_lo = (v >> s) | (v << (32 - s));
        cpu.m_gpr[op.rd].m_hi = 0;
        break;
    }
    case LOP_ROTRV:
    {
        uint32_t v = (uint32_t)cpu.m_gpr[op.rt].m_lo;
        uint32_t s = (uint32_t)cpu.m_gpr[op.rs].m_lo & 31;
        cpu.m_gpr[op.rd].m_lo = (v >> s) | (v << (32 - s));
        cpu.m_gpr[op.rd].m_hi = 0;
        break;
    }

    case LOP_LB:
    {
        uint32_t addr = (uint32_t)(sext32(cpu.m_gpr[op.rs].m_lo) + op.immediate);
        cpu.m_gpr[op.rt].m_lo = (uint64_t)(int8_t)ee_mem_read8(rt, addr);
        cpu.m_gpr[op.rt].m_hi = 0;
        break;
    }
    case LOP_LH:
    {
        uint32_t addr = (uint32_t)(sext32(cpu.m_gpr[op.rs].m_lo) + op.immediate);
        cpu.m_gpr[op.rt].m_lo = (uint64_t)(int16_t)ee_mem_read16(rt, addr);
        cpu.m_gpr[op.rt].m_hi = 0;
        break;
    }
    case LOP_LBU:
    {
        uint32_t addr = (uint32_t)(sext32(cpu.m_gpr[op.rs].m_lo) + op.immediate);
        cpu.m_gpr[op.rt].m_lo = ee_mem_read8(rt, addr);
        cpu.m_gpr[op.rt].m_hi = 0;
        break;
    }
    case LOP_LHU:
    {
        uint32_t addr = (uint32_t)(sext32(cpu.m_gpr[op.rs].m_lo) + op.immediate);
        cpu.m_gpr[op.rt].m_lo = ee_mem_read16(rt, addr);
        cpu.m_gpr[op.rt].m_hi = 0;
        break;
    }
    case LOP_LW:
    {
        uint32_t addr = (uint32_t)(sext32(cpu.m_gpr[op.rs].m_lo) + op.immediate);
        cpu.m_gpr[op.rt].m_lo = (uint64_t)(int32_t)ee_mem_read32(rt, addr);
        cpu.m_gpr[op.rt].m_hi = 0;
        break;
    }
    case LOP_LWL:
    {
        uint32_t ea = (uint32_t)(sext32(cpu.m_gpr[op.rs].m_lo) + op.immediate);
        uint32_t bo = ea & 3u;
        uint64_t cur = cpu.m_gpr[op.rt].m_lo;
        switch (bo)
        {
        case 0:
            cpu.m_gpr[op.rt].m_lo = (uint64_t)((uint32_t)ee_mem_read8(rt, ea) << 24 |
                                               (uint32_t)ee_mem_read8(rt, ea + 1) << 16 |
                                               (uint32_t)ee_mem_read8(rt, ea + 2) << 8 |
                                               (uint32_t)ee_mem_read8(rt, ea + 3));
            break;
        case 1:
            cpu.m_gpr[op.rt].m_lo = ((uint64_t)(uint32_t)ee_mem_read8(rt, ea) << 24) | (cur & 0x00FFFFFFULL);
            break;
        case 2:
            cpu.m_gpr[op.rt].m_lo = ((uint64_t)((uint32_t)ee_mem_read8(rt, ea) << 16 |
                                               (uint32_t)ee_mem_read8(rt, ea + 1) << 8)) | (cur & 0x0000FFFFULL);
            break;
        case 3:
            cpu.m_gpr[op.rt].m_lo = ((uint64_t)(uint32_t)ee_mem_read8(rt, ea) << 24) | (cur & 0x00FFFFFFULL);
            break;
        }
        cpu.m_gpr[op.rt].m_hi = 0;
        break;
    }
    case LOP_LWR:
    {
        uint32_t ea = (uint32_t)(sext32(cpu.m_gpr[op.rs].m_lo) + op.immediate);
        uint32_t bo = ea & 3u;
        uint64_t cur = cpu.m_gpr[op.rt].m_lo;
        switch (bo)
        {
        case 0:
            cpu.m_gpr[op.rt].m_lo = (uint64_t)(uint32_t)ee_mem_read8(rt, ea);
            break;
        case 1:
            cpu.m_gpr[op.rt].m_lo = ((uint64_t)(uint32_t)ee_mem_read8(rt, ea - 1) << 8 |
                                     (uint32_t)ee_mem_read8(rt, ea)) | (cur & 0xFFFF0000ULL);
            break;
        case 2:
            cpu.m_gpr[op.rt].m_lo = ((uint64_t)((uint32_t)ee_mem_read8(rt, ea - 2) << 16 |
                                               (uint32_t)ee_mem_read8(rt, ea - 1) << 8 |
                                               (uint32_t)ee_mem_read8(rt, ea))) | (cur & 0xFF000000ULL);
            break;
        case 3:
            cpu.m_gpr[op.rt].m_lo = (uint64_t)((uint32_t)ee_mem_read8(rt, ea - 3) << 24 |
                                               (uint32_t)ee_mem_read8(rt, ea - 2) << 16 |
                                               (uint32_t)ee_mem_read8(rt, ea - 1) << 8 |
                                               (uint32_t)ee_mem_read8(rt, ea));
            break;
        }
        cpu.m_gpr[op.rt].m_hi = 0;
        break;
    }
    case LOP_LWU:
    {
        uint32_t addr = (uint32_t)(sext32(cpu.m_gpr[op.rs].m_lo) + op.immediate);
        cpu.m_gpr[op.rt].m_lo = ee_mem_read32(rt, addr);
        cpu.m_gpr[op.rt].m_hi = 0;
        break;
    }
    case LOP_LD:
    {
        uint32_t addr = (uint32_t)(sext32(cpu.m_gpr[op.rs].m_lo) + op.immediate);
        cpu.m_gpr[op.rt].m_lo = ee_mem_read64(rt, addr);
        cpu.m_gpr[op.rt].m_hi = 0;
        break;
    }
    case LOP_SB:
    {
        uint32_t addr = (uint32_t)(sext32(cpu.m_gpr[op.rs].m_lo) + op.immediate);
        ee_mem_write8(rt, addr, (uint8_t)cpu.m_gpr[op.rt].m_lo);
        break;
    }
    case LOP_SH:
    {
        uint32_t addr = (uint32_t)(sext32(cpu.m_gpr[op.rs].m_lo) + op.immediate);
        ee_mem_write16(rt, addr, (uint16_t)cpu.m_gpr[op.rt].m_lo);
        break;
    }
    case LOP_SW:
    {
        uint32_t addr = (uint32_t)(sext32(cpu.m_gpr[op.rs].m_lo) + op.immediate);
        ee_mem_write32(rt, addr, (uint32_t)cpu.m_gpr[op.rt].m_lo);
        break;
    }
    case LOP_SWL:
    {
        uint32_t ea = (uint32_t)(sext32(cpu.m_gpr[op.rs].m_lo) + op.immediate);
        uint32_t bo = ea & 3u;
        uint32_t v = (uint32_t)cpu.m_gpr[op.rt].m_lo;
        uint32_t cur = ee_mem_read32(rt, ea & ~3u);
        uint32_t merged;
        switch (bo)
        {
        case 0: merged = v; break;
        case 1: merged = (v & 0xFF000000u) | (cur & 0x00FFFFFFu); break;
        case 2: merged = (v & 0xFFFF0000u) | (cur & 0x0000FFFFu); break;
        case 3: merged = (v & 0xFF000000u) | (cur & 0x00FFFFFFu); break;
        }
        ee_mem_write32(rt, ea & ~3u, merged);
        break;
    }
    case LOP_SDL:
    {
        uint32_t ea = (uint32_t)(sext32(cpu.m_gpr[op.rs].m_lo) + op.immediate);
        uint32_t bo = ea & 3u;
        uint32_t v = (uint32_t)cpu.m_gpr[op.rt].m_lo;
        uint32_t cur = ee_mem_read32(rt, ea & ~3u);
        uint32_t merged;
        switch (bo)
        {
        case 0: merged = v; break;
        case 1: merged = (cur & 0xFFFF0000u) | (v & 0x0000FFFFu); break;
        case 2: merged = (cur & 0xFF000000u) | (v & 0x00FFFFFFu); break;
        case 3: merged = v; break;
        }
        ee_mem_write32(rt, ea & ~3u, merged);
        break;
    }
    case LOP_SD:
    {
        uint32_t addr = (uint32_t)(sext32(cpu.m_gpr[op.rs].m_lo) + op.immediate);
        ee_mem_write64(rt, addr, cpu.m_gpr[op.rt].m_lo);
        break;
    }

    case LOP_MFHI:
        cpu.m_gpr[op.rd].m_lo = cpu.m_hi;
        cpu.m_gpr[op.rd].m_hi = 0;
        break;
    case LOP_MTHI:
        cpu.m_hi = cpu.m_gpr[op.rs].m_lo;
        break;
    case LOP_MFLO:
        cpu.m_gpr[op.rd].m_lo = cpu.m_lo;
        cpu.m_gpr[op.rd].m_hi = 0;
        break;
    case LOP_MTLO:
        cpu.m_lo = cpu.m_gpr[op.rs].m_lo;
        break;
    case LOP_MFHI1:
        cpu.m_gpr[op.rd].m_lo = cpu.m_hi1;
        break;
    case LOP_MTHI1:
        cpu.m_hi1 = cpu.m_gpr[op.rs].m_lo;
        break;
    case LOP_MFLO1:
        cpu.m_gpr[op.rd].m_lo = cpu.m_lo1;
        break;
    case LOP_MTLO1:
        cpu.m_lo1 = cpu.m_gpr[op.rs].m_lo;
        break;
    case LOP_MULT:
    {
        int64_t a = sext32(cpu.m_gpr[op.rs].m_lo);
        int64_t b = sext32(cpu.m_gpr[op.rt].m_lo);
        int64_t prod = a * b;
        cpu.m_lo = (uint64_t)(uint32_t)prod;
        cpu.m_hi = (uint64_t)(prod >> 32);
        break;
    }
    case LOP_MULTU:
    {
        uint64_t a = cpu.m_gpr[op.rs].m_lo;
        uint64_t b = cpu.m_gpr[op.rt].m_lo;
        uint64_t prod = a * b;
        cpu.m_lo = (uint32_t)prod;
        cpu.m_hi = prod >> 32;
        break;
    }
    case LOP_DIV:
    {
        const int64_t lhs = sext32(cpu.m_gpr[op.rs].m_lo);
        const int64_t rhs = sext32(cpu.m_gpr[op.rt].m_lo);
        // The R5900 does not define divide-by-zero outputs. Keep HI/LO
        // intact, matching the LLVM and generated-C++ execution paths.
        if (rhs != 0)
        {
            if (lhs == -2147483648LL && rhs == -1)
            {
                cpu.m_lo = 0xffffffff80000000ULL;
                cpu.m_hi = 0;
            }
            else
            {
                cpu.m_lo = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(lhs / rhs)));
                cpu.m_hi = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(lhs % rhs)));
            }
        }
        break;
    }
    case LOP_DIVU:
    {
        const uint64_t lhs = static_cast<uint32_t>(cpu.m_gpr[op.rs].m_lo);
        const uint64_t rhs = static_cast<uint32_t>(cpu.m_gpr[op.rt].m_lo);
        if (rhs != 0)
        {
            const uint32_t quotient = static_cast<uint32_t>(lhs / rhs);
            const uint32_t remainder = static_cast<uint32_t>(lhs % rhs);
            cpu.m_lo = (quotient & 0x80000000u) ?
                (0xffffffff00000000ULL | quotient) : quotient;
            cpu.m_hi = (remainder & 0x80000000u) ?
                (0xffffffff00000000ULL | remainder) : remainder;
        }
        break;
    }

    // ---- MMI (128-bit packed) ----
    case LOP_MMI_ADD_B:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        for (int i = 0; i < 16; ++i)
            r = put8(r, i, (uint8_t)((uint32_t)get8(a, i) + get8(b, i)));
        cpu.m_gpr[op.rd] = r;
        break;
    }
    case LOP_MMI_ADD_H:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        for (int i = 0; i < 8; ++i)
            r = put16(r, i, (uint16_t)((uint32_t)get16(a, i) + get16(b, i)));
        cpu.m_gpr[op.rd] = r;
        break;
    }
    case LOP_MMI_ADD_W:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        for (int i = 0; i < 4; ++i)
            r = put32(r, i, get32(a, i) + get32(b, i));
        cpu.m_gpr[op.rd] = r;
        break;
    }
    case LOP_MMI_ADD_D:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        r = put64(r, 0, get64(a, 0) + get64(b, 0));
        r = put64(r, 1, get64(a, 1) + get64(b, 1));
        cpu.m_gpr[op.rd] = r;
        break;
    }
    case LOP_MMI_SUB_B:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        for (int i = 0; i < 16; ++i)
            r = put8(r, i, (uint8_t)((uint32_t)get8(a, i) - get8(b, i)));
        cpu.m_gpr[op.rd] = r;
        break;
    }
    case LOP_MMI_SUB_H:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        for (int i = 0; i < 8; ++i)
            r = put16(r, i, (uint16_t)((uint32_t)get16(a, i) - get16(b, i)));
        cpu.m_gpr[op.rd] = r;
        break;
    }
    case LOP_MMI_SUB_W:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        for (int i = 0; i < 4; ++i)
            r = put32(r, i, get32(a, i) - get32(b, i));
        cpu.m_gpr[op.rd] = r;
        break;
    }
    case LOP_MMI_SUB_D:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        r = put64(r, 0, get64(a, 0) - get64(b, 0));
        r = put64(r, 1, get64(a, 1) - get64(b, 1));
        cpu.m_gpr[op.rd] = r;
        break;
    }
    case LOP_MMI_ADDU_SAT_B:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        for (int i = 0; i < 16; ++i)
            r = put8(r, i, sat8u((uint32_t)get8(a, i) + get8(b, i)));
        cpu.m_gpr[op.rd] = r;
        break;
    }
    case LOP_MMI_ADDU_SAT_H:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        for (int i = 0; i < 8; ++i)
            r = put16(r, i, sat16u((uint32_t)get16(a, i) + get16(b, i)));
        cpu.m_gpr[op.rd] = r;
        break;
    }
    case LOP_MMI_ADDU_SAT_W:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        for (int i = 0; i < 4; ++i)
            r = put32(r, i, sat32u((uint64_t)get32(a, i) + get32(b, i)));
        cpu.m_gpr[op.rd] = r;
        break;
    }
    case LOP_MMI_AND:
        cpu.m_gpr[op.rd].m_lo = cpu.m_gpr[op.rs].m_lo & cpu.m_gpr[op.rt].m_lo;
        cpu.m_gpr[op.rd].m_hi = cpu.m_gpr[op.rs].m_hi & cpu.m_gpr[op.rt].m_hi;
        break;
    case LOP_MMI_OR:
        cpu.m_gpr[op.rd].m_lo = cpu.m_gpr[op.rs].m_lo | cpu.m_gpr[op.rt].m_lo;
        cpu.m_gpr[op.rd].m_hi = cpu.m_gpr[op.rs].m_hi | cpu.m_gpr[op.rt].m_hi;
        break;
    case LOP_MMI_XOR:
        cpu.m_gpr[op.rd].m_lo = cpu.m_gpr[op.rs].m_lo ^ cpu.m_gpr[op.rt].m_lo;
        cpu.m_gpr[op.rd].m_hi = cpu.m_gpr[op.rs].m_hi ^ cpu.m_gpr[op.rt].m_hi;
        break;
    case LOP_MMI_NOR:
        cpu.m_gpr[op.rd].m_lo = ~(cpu.m_gpr[op.rs].m_lo | cpu.m_gpr[op.rt].m_lo);
        cpu.m_gpr[op.rd].m_hi = ~(cpu.m_gpr[op.rs].m_hi | cpu.m_gpr[op.rt].m_hi);
        break;
    case LOP_MMI_SLL:
        cpu.m_gpr[op.rd] = shl128(cpu.m_gpr[op.rt], (int)op.sa);
        break;
    case LOP_MMI_SRL:
        cpu.m_gpr[op.rd] = shr128(cpu.m_gpr[op.rt], (int)op.sa);
        break;
    case LOP_MMI_SRA:
        cpu.m_gpr[op.rd] = sar128(cpu.m_gpr[op.rt], (int)op.sa);
        break;
    case LOP_MMI_MAXU_B:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        for (int i = 0; i < 16; ++i)
            r = put8(r, i, get8(a, i) > get8(b, i) ? get8(a, i) : get8(b, i));
        cpu.m_gpr[op.rd] = r;
        break;
    }
    case LOP_MMI_MAXU_H:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        for (int i = 0; i < 8; ++i)
            r = put16(r, i, get16(a, i) > get16(b, i) ? get16(a, i) : get16(b, i));
        cpu.m_gpr[op.rd] = r;
        break;
    }
    case LOP_MMI_MAXU_W:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        for (int i = 0; i < 4; ++i)
            r = put32(r, i, get32(a, i) > get32(b, i) ? get32(a, i) : get32(b, i));
        cpu.m_gpr[op.rd] = r;
        break;
    }
    case LOP_MMI_MAXS_B:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        for (int i = 0; i < 16; ++i)
            r = put8(r, i, (int8_t)get8(a, i) > (int8_t)get8(b, i) ? get8(a, i) : get8(b, i));
        cpu.m_gpr[op.rd] = r;
        break;
    }
    case LOP_MMI_MAXS_H:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        for (int i = 0; i < 8; ++i)
            r = put16(r, i, (int16_t)get16(a, i) > (int16_t)get16(b, i) ? get16(a, i) : get16(b, i));
        cpu.m_gpr[op.rd] = r;
        break;
    }
    case LOP_MMI_MAXS_W:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        for (int i = 0; i < 4; ++i)
            r = put32(r, i, (int32_t)get32(a, i) > (int32_t)get32(b, i) ? get32(a, i) : get32(b, i));
        cpu.m_gpr[op.rd] = r;
        break;
    }
    case LOP_MMI_MINU_B:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        for (int i = 0; i < 16; ++i)
            r = put8(r, i, get8(a, i) < get8(b, i) ? get8(a, i) : get8(b, i));
        cpu.m_gpr[op.rd] = r;
        break;
    }
    case LOP_MMI_MINU_H:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        for (int i = 0; i < 8; ++i)
            r = put16(r, i, get16(a, i) < get16(b, i) ? get16(a, i) : get16(b, i));
        cpu.m_gpr[op.rd] = r;
        break;
    }
    case LOP_MMI_MINU_W:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        for (int i = 0; i < 4; ++i)
            r = put32(r, i, get32(a, i) < get32(b, i) ? get32(a, i) : get32(b, i));
        cpu.m_gpr[op.rd] = r;
        break;
    }
    case LOP_MMI_MINS_B:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        for (int i = 0; i < 16; ++i)
            r = put8(r, i, (int8_t)get8(a, i) < (int8_t)get8(b, i) ? get8(a, i) : get8(b, i));
        cpu.m_gpr[op.rd] = r;
        break;
    }
    case LOP_MMI_MINS_H:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        for (int i = 0; i < 8; ++i)
            r = put16(r, i, (int16_t)get16(a, i) < (int16_t)get16(b, i) ? get16(a, i) : get16(b, i));
        cpu.m_gpr[op.rd] = r;
        break;
    }
    case LOP_MMI_MINS_W:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        for (int i = 0; i < 4; ++i)
            r = put32(r, i, (int32_t)get32(a, i) < (int32_t)get32(b, i) ? get32(a, i) : get32(b, i));
        cpu.m_gpr[op.rd] = r;
        break;
    }
    case LOP_MMI_QFSRV:
        cpu.m_gpr[op.rd] = shr128(cpu.m_gpr[op.rt], (int)cpu.m_gpr[op.rs].m_lo);
        break;
    case LOP_MMI_SHUFFLE:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        for (int i = 0; i < 16; ++i)
            r = put8(r, i, get8(b, get8(a, i) & 0xF));
        cpu.m_gpr[op.rd] = r;
        break;
    }
    case LOP_MMI_PACK:
    {
        ee::s_ee_u128 a = cpu.m_gpr[op.rs], b = cpu.m_gpr[op.rt], r{};
        for (int i = 0; i < 8; ++i) {
            r = put8(r, i, sat8u(get16(b, i)));
            r = put8(r, 8 + i, sat8u(get16(a, i)));
        }
        cpu.m_gpr[op.rd] = r;
        break;
    }

    // ---- Coprocessor 0 (system control) ----
    case LOP_MFC0:
        cpu.m_gpr[op.rt].m_lo = cpu.m_cop0[op.rd];
        cpu.m_gpr[op.rt].m_hi = 0;
        break;
    case LOP_MTC0:
        cpu.m_cop0[op.rd] = (uint32_t)cpu.m_gpr[op.rt].m_lo;
        break;

    // ---- Coprocessor 1 (floating point) ----
    case LOP_MFC1:
        cpu.m_gpr[op.rt].m_lo = (uint64_t)(uint32_t)cpu.m_fpr[op.rd];
        cpu.m_gpr[op.rt].m_hi = 0;
        break;
    case LOP_MTC1:
        cpu.m_fpr[op.rd] = (uint64_t)(uint32_t)cpu.m_gpr[op.rt].m_lo;
        break;
    case LOP_LWC1:
    {
        uint32_t ea = (uint32_t)((int64_t)cpu.m_gpr[op.rs].m_lo + op.immediate);
        cpu.m_fpr[op.rt] = (uint64_t)ee_mem_read32(rt, ea);
        break;
    }
    case LOP_SWC1:
    {
        uint32_t ea = (uint32_t)((int64_t)cpu.m_gpr[op.rs].m_lo + op.immediate);
        ee_mem_write32(rt, ea, (uint32_t)cpu.m_fpr[op.rt]);
        break;
    }
    case LOP_CVT_S_W:
    {
        float f = static_cast<float>((int32_t)cpu.m_gpr[op.rt].m_lo);
        cpu.m_fpr[op.sa] = (uint64_t)(uint32_t)(*reinterpret_cast<uint32_t*>(&f));
        break;
    }
    case LOP_CVT_W_S:
    {
        float f = *reinterpret_cast<float*>(&cpu.m_fpr[op.rt]);
        cpu.m_gpr[op.sa].m_lo = (uint64_t)(int32_t)f;
        cpu.m_gpr[op.sa].m_hi = 0;
        break;
    }
    case LOP_ADD_S:
    {
        float a = *reinterpret_cast<float*>(&cpu.m_fpr[op.rd]);
        float b = *reinterpret_cast<float*>(&cpu.m_fpr[op.rt]);
        float r = a + b;
        cpu.m_fpr[op.sa] = (uint64_t)(uint32_t)(*reinterpret_cast<uint32_t*>(&r));
        break;
    }
    case LOP_SUB_S:
    {
        float a = *reinterpret_cast<float*>(&cpu.m_fpr[op.rd]);
        float b = *reinterpret_cast<float*>(&cpu.m_fpr[op.rt]);
        float r = a - b;
        cpu.m_fpr[op.sa] = (uint64_t)(uint32_t)(*reinterpret_cast<uint32_t*>(&r));
        break;
    }
    case LOP_MUL_S:
    {
        float a = *reinterpret_cast<float*>(&cpu.m_fpr[op.rd]);
        float b = *reinterpret_cast<float*>(&cpu.m_fpr[op.rt]);
        float r = a * b;
        cpu.m_fpr[op.sa] = (uint64_t)(uint32_t)(*reinterpret_cast<uint32_t*>(&r));
        break;
    }
    case LOP_DIV_S:
    {
        float a = *reinterpret_cast<float*>(&cpu.m_fpr[op.rd]);
        float b = *reinterpret_cast<float*>(&cpu.m_fpr[op.rt]);
        float r = a / b;
        cpu.m_fpr[op.sa] = (uint64_t)(uint32_t)(*reinterpret_cast<uint32_t*>(&r));
        break;
    }
    case LOP_C_LT_S:
    {
        float a = *reinterpret_cast<float*>(&cpu.m_fpr[op.rd]);
        float b = *reinterpret_cast<float*>(&cpu.m_fpr[op.rt]);
        cpu.m_cop0[31] = (a < b) ? 1 : 0;
        break;
    }
    case LOP_C_LE_S:
    {
        float a = *reinterpret_cast<float*>(&cpu.m_fpr[op.rd]);
        float b = *reinterpret_cast<float*>(&cpu.m_fpr[op.rt]);
        cpu.m_cop0[31] = (a <= b) ? 1 : 0;
        break;
    }
    case LOP_C_EQ_S:
    {
        float a = *reinterpret_cast<float*>(&cpu.m_fpr[op.rd]);
        float b = *reinterpret_cast<float*>(&cpu.m_fpr[op.rt]);
        cpu.m_cop0[31] = (a == b) ? 1 : 0;
        break;
    }

    // ---- Coprocessor 2 (VU0) ----
    case LOP_MFC2:
        cpu.m_gpr[op.rt].m_lo = (uint64_t)(uint32_t)cpu.m_vu0[op.rd].m_lo;
        cpu.m_gpr[op.rt].m_hi = 0;
        break;
    case LOP_MTC2:
        cpu.m_vu0[op.rd].m_lo = (uint64_t)(uint32_t)cpu.m_gpr[op.rt].m_lo;
        cpu.m_vu0[op.rd].m_hi = 0;
        break;
    case LOP_LQC2:
    {
        uint32_t ea = (uint32_t)((int64_t)cpu.m_gpr[op.rs].m_lo + op.immediate);
        cpu.m_vu0[op.rt].m_lo = ee_mem_read64(rt, ea);
        cpu.m_vu0[op.rt].m_hi = ee_mem_read64(rt, ea + 8);
        break;
    }
    case LOP_SQC2:
    {
        uint32_t ea = (uint32_t)((int64_t)cpu.m_gpr[op.rs].m_lo + op.immediate);
        ee_mem_write64(rt, ea, cpu.m_vu0[op.rt].m_lo);
        ee_mem_write64(rt, ea + 8, cpu.m_vu0[op.rt].m_hi);
        break;
    }

    // ---- COP2 VU0 vector arithmetic ----
    case LOP_VADD_XYZW:
    {
        float vs[4], vt[4], vd[4];
        memcpy(vs, &cpu.m_vu0[op.rs], 16);
        memcpy(vt, &cpu.m_vu0[op.rt], 16);
        vd[0] = vs[0] + vt[0];
        vd[1] = vs[1] + vt[1];
        vd[2] = vs[2] + vt[2];
        vd[3] = vs[3] + vt[3];
        memcpy(&cpu.m_vu0[op.rd], vd, 16);
        break;
    }
    case LOP_VSUB_XYZW:
    {
        float vs[4], vt[4], vd[4];
        memcpy(vs, &cpu.m_vu0[op.rs], 16);
        memcpy(vt, &cpu.m_vu0[op.rt], 16);
        vd[0] = vs[0] - vt[0];
        vd[1] = vs[1] - vt[1];
        vd[2] = vs[2] - vt[2];
        vd[3] = vs[3] - vt[3];
        memcpy(&cpu.m_vu0[op.rd], vd, 16);
        break;
    }
    case LOP_VMUL_XYZW:
    {
        float vs[4], vt[4], vd[4];
        memcpy(vs, &cpu.m_vu0[op.rs], 16);
        memcpy(vt, &cpu.m_vu0[op.rt], 16);
        vd[0] = vs[0] * vt[0];
        vd[1] = vs[1] * vt[1];
        vd[2] = vs[2] * vt[2];
        vd[3] = vs[3] * vt[3];
        memcpy(&cpu.m_vu0[op.rd], vd, 16);
        break;
    }
    case LOP_VMULA_XYZW:
    {
        float vs[4], vt[4], vd[4];
        memcpy(vs, &cpu.m_vu0[op.rs], 16);
        memcpy(vt, &cpu.m_vu0[op.rt], 16);
        memcpy(vd, &cpu.m_vu0[op.rd], 16);
        vd[0] = vs[0] * vt[0] + vd[0];
        vd[1] = vs[1] * vt[1] + vd[1];
        vd[2] = vs[2] * vt[2] + vd[2];
        vd[3] = vs[3] * vt[3] + vd[3];
        memcpy(&cpu.m_vu0[op.rd], vd, 16);
        break;
    }
    case LOP_VADD_X:
    {
        float vs[4], vt[4], vd[4];
        memcpy(vs, &cpu.m_vu0[op.rs], 16);
        memcpy(vt, &cpu.m_vu0[op.rt], 16);
        memcpy(vd, &cpu.m_vu0[op.rd], 16);
        vd[0] = vs[0] + vt[0];
        memcpy(&cpu.m_vu0[op.rd], vd, 16);
        break;
    }
    case LOP_VSUB_X:
    {
        float vs[4], vt[4], vd[4];
        memcpy(vs, &cpu.m_vu0[op.rs], 16);
        memcpy(vt, &cpu.m_vu0[op.rt], 16);
        memcpy(vd, &cpu.m_vu0[op.rd], 16);
        vd[0] = vs[0] - vt[0];
        memcpy(&cpu.m_vu0[op.rd], vd, 16);
        break;
    }
    case LOP_VMUL_X:
    {
        float vs[4], vt[4], vd[4];
        memcpy(vs, &cpu.m_vu0[op.rs], 16);
        memcpy(vt, &cpu.m_vu0[op.rt], 16);
        memcpy(vd, &cpu.m_vu0[op.rd], 16);
        vd[0] = vs[0] * vt[0];
        memcpy(&cpu.m_vu0[op.rd], vd, 16);
        break;
    }
    case LOP_VMULA_X:
    {
        float vs[4], vt[4], vd[4];
        memcpy(vs, &cpu.m_vu0[op.rs], 16);
        memcpy(vt, &cpu.m_vu0[op.rt], 16);
        memcpy(vd, &cpu.m_vu0[op.rd], 16);
        vd[0] = vs[0] * vt[0] + vd[0];
        memcpy(&cpu.m_vu0[op.rd], vd, 16);
        break;
    }
    case LOP_VADD_Y:
    {
        float vs[4], vt[4], vd[4];
        memcpy(vs, &cpu.m_vu0[op.rs], 16);
        memcpy(vt, &cpu.m_vu0[op.rt], 16);
        memcpy(vd, &cpu.m_vu0[op.rd], 16);
        vd[1] = vs[1] + vt[1];
        memcpy(&cpu.m_vu0[op.rd], vd, 16);
        break;
    }
    case LOP_VSUB_Y:
    {
        float vs[4], vt[4], vd[4];
        memcpy(vs, &cpu.m_vu0[op.rs], 16);
        memcpy(vt, &cpu.m_vu0[op.rt], 16);
        memcpy(vd, &cpu.m_vu0[op.rd], 16);
        vd[1] = vs[1] - vt[1];
        memcpy(&cpu.m_vu0[op.rd], vd, 16);
        break;
    }
    case LOP_VMUL_Y:
    {
        float vs[4], vt[4], vd[4];
        memcpy(vs, &cpu.m_vu0[op.rs], 16);
        memcpy(vt, &cpu.m_vu0[op.rt], 16);
        memcpy(vd, &cpu.m_vu0[op.rd], 16);
        vd[1] = vs[1] * vt[1];
        memcpy(&cpu.m_vu0[op.rd], vd, 16);
        break;
    }
    case LOP_VMULA_Y:
    {
        float vs[4], vt[4], vd[4];
        memcpy(vs, &cpu.m_vu0[op.rs], 16);
        memcpy(vt, &cpu.m_vu0[op.rt], 16);
        memcpy(vd, &cpu.m_vu0[op.rd], 16);
        vd[1] = vs[1] * vt[1] + vd[1];
        memcpy(&cpu.m_vu0[op.rd], vd, 16);
        break;
    }
    case LOP_VADD_Z:
    {
        float vs[4], vt[4], vd[4];
        memcpy(vs, &cpu.m_vu0[op.rs], 16);
        memcpy(vt, &cpu.m_vu0[op.rt], 16);
        memcpy(vd, &cpu.m_vu0[op.rd], 16);
        vd[2] = vs[2] + vt[2];
        memcpy(&cpu.m_vu0[op.rd], vd, 16);
        break;
    }
    case LOP_VSUB_Z:
    {
        float vs[4], vt[4], vd[4];
        memcpy(vs, &cpu.m_vu0[op.rs], 16);
        memcpy(vt, &cpu.m_vu0[op.rt], 16);
        memcpy(vd, &cpu.m_vu0[op.rd], 16);
        vd[2] = vs[2] - vt[2];
        memcpy(&cpu.m_vu0[op.rd], vd, 16);
        break;
    }
    case LOP_VMUL_Z:
    {
        float vs[4], vt[4], vd[4];
        memcpy(vs, &cpu.m_vu0[op.rs], 16);
        memcpy(vt, &cpu.m_vu0[op.rt], 16);
        memcpy(vd, &cpu.m_vu0[op.rd], 16);
        vd[2] = vs[2] * vt[2];
        memcpy(&cpu.m_vu0[op.rd], vd, 16);
        break;
    }
    case LOP_VMULA_Z:
    {
        float vs[4], vt[4], vd[4];
        memcpy(vs, &cpu.m_vu0[op.rs], 16);
        memcpy(vt, &cpu.m_vu0[op.rt], 16);
        memcpy(vd, &cpu.m_vu0[op.rd], 16);
        vd[2] = vs[2] * vt[2] + vd[2];
        memcpy(&cpu.m_vu0[op.rd], vd, 16);
        break;
    }
    case LOP_VADD_W:
    {
        float vs[4], vt[4], vd[4];
        memcpy(vs, &cpu.m_vu0[op.rs], 16);
        memcpy(vt, &cpu.m_vu0[op.rt], 16);
        memcpy(vd, &cpu.m_vu0[op.rd], 16);
        vd[3] = vs[3] + vt[3];
        memcpy(&cpu.m_vu0[op.rd], vd, 16);
        break;
    }
    case LOP_VSUB_W:
    {
        float vs[4], vt[4], vd[4];
        memcpy(vs, &cpu.m_vu0[op.rs], 16);
        memcpy(vt, &cpu.m_vu0[op.rt], 16);
        memcpy(vd, &cpu.m_vu0[op.rd], 16);
        vd[3] = vs[3] - vt[3];
        memcpy(&cpu.m_vu0[op.rd], vd, 16);
        break;
    }
    case LOP_VMUL_W:
    {
        float vs[4], vt[4], vd[4];
        memcpy(vs, &cpu.m_vu0[op.rs], 16);
        memcpy(vt, &cpu.m_vu0[op.rt], 16);
        memcpy(vd, &cpu.m_vu0[op.rd], 16);
        vd[3] = vs[3] * vt[3];
        memcpy(&cpu.m_vu0[op.rd], vd, 16);
        break;
    }
    case LOP_VMULA_W:
    {
        float vs[4], vt[4], vd[4];
        memcpy(vs, &cpu.m_vu0[op.rs], 16);
        memcpy(vt, &cpu.m_vu0[op.rt], 16);
        memcpy(vd, &cpu.m_vu0[op.rd], 16);
        vd[3] = vs[3] * vt[3] + vd[3];
        memcpy(&cpu.m_vu0[op.rd], vd, 16);
        break;
    }

    // ---- COP2 VU0 vector branches ----
    case LOP_VBGTZ:
    {
        float vx;
        memcpy(&vx, &cpu.m_vu0[op.rs], 4);
        if (vx > 0.0f) cpu.m_pc = op.jump_target;
        break;
    }
    case LOP_VBLTZ:
    {
        float vx;
        memcpy(&vx, &cpu.m_vu0[op.rs], 4);
        if (vx < 0.0f) cpu.m_pc = op.jump_target;
        break;
    }
    case LOP_VBEQZ:
    {
        float vx;
        memcpy(&vx, &cpu.m_vu0[op.rs], 4);
        if (vx == 0.0f) cpu.m_pc = op.jump_target;
        break;
    }
    case LOP_VBNEZ:
    {
        float vx;
        memcpy(&vx, &cpu.m_vu0[op.rs], 4);
        if (vx != 0.0f) cpu.m_pc = op.jump_target;
        break;
    }

    default:
        break;
    }
    cpu.m_pc = next_pc;

    // Exception ops override PC set above.
    if (op.kind == LOP_SYSCALL) {
        cpu.m_pc = ee::ee_raise_exception(&cpu, rt, ee::EX_Sys, op.pc, 0, 0);
    } else if (op.kind == LOP_BREAK) {
        cpu.m_pc = ee::ee_raise_exception(&cpu, rt, ee::EX_Bp, op.pc, 0, 0);
    } else if (op.kind == LOP_ERET) {
        cpu.m_pc = ee::ee_exception_return(&cpu);
    }
}

} // namespace ee

