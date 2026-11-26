#pragma once

#include <cstdint>
#include <optional>

namespace ee::scalar_constant
{
// Guest properties only. No assumptions about the host compilation target.
struct s_r5900_traits
{
    static constexpr unsigned k_word_bits = 32;
    static constexpr unsigned k_scalar_bits = 64;
    static constexpr unsigned k_gpr_bits = 128;
    static constexpr unsigned k_gpr_count = 32;
};

enum class e_op
{
    addu, subu, daddu, dsubu,
    bit_and, bit_or, bit_xor, bit_nor, slt, sltu,
    addiu, daddiu, andi, ori, xori, slti, sltiu, lui,
    sll, srl, sra, sllv, srlv, srav,
    dsll, dsrl, dsra, dsllv, dsrlv, dsrav,
    dsll32, dsrl32, dsra32
};

template<unsigned bits>
constexpr uint64_t low_mask()
{
    static_assert(bits > 0 && bits <= 64);
    if constexpr (bits == 64)
    {
        return UINT64_MAX;
    }
    else
    {
        return (uint64_t{1} << bits) - 1;
    }
}

template<unsigned bits>
constexpr uint64_t sign_extend(uint64_t value)
{
    constexpr uint64_t sign = uint64_t{1} << (bits - 1);
    return ((value & low_mask<bits>()) ^ sign) - sign;
}

constexpr bool signed_less(uint64_t lhs, uint64_t rhs)
{
    constexpr uint64_t sign = uint64_t{1} << 63;
    return (lhs ^ sign) < (rhs ^ sign);
}

template<unsigned bits>
constexpr uint64_t arithmetic_right(uint64_t value, uint64_t count)
{
    static_assert(bits == 32 || bits == 64);
    constexpr uint64_t mask = low_mask<bits>();
    constexpr uint64_t sign = uint64_t{1} << (bits - 1);
    const unsigned shift = static_cast<unsigned>(count & (bits - 1));
    value &= mask;
    return (value >> shift) | ((value & sign) ? (mask ^ (mask >> shift)) : 0);
}

// Operands are known guest bit patterns, not necessarily C++ compile-time
// values. For immediate operations rhs is the encoded 16-bit field; for
// shifts lhs is the value (rt), rhs is the count (sa or rs). DS*32 adds 32
// to the masked five-bit encoded count. No trapping or side-effecting
// operations belong in this evaluator.
constexpr std::optional<uint64_t> evaluate(e_op op, uint64_t lhs, uint64_t rhs)
{
    const uint64_t imm = rhs & 0xffffu;
    const uint64_t simm = sign_extend<16>(imm);
    const uint64_t word = lhs & low_mask<32>();
    const unsigned shift32 = static_cast<unsigned>(rhs & 31u);
    const unsigned shift64 = static_cast<unsigned>(rhs & 63u);
    switch (op)
    {
    case e_op::addu: return sign_extend<32>(lhs + rhs);
    case e_op::subu: return sign_extend<32>(lhs - rhs);
    case e_op::daddu: return lhs + rhs;
    case e_op::dsubu: return lhs - rhs;
    case e_op::bit_and: return lhs & rhs;
    case e_op::bit_or: return lhs | rhs;
    case e_op::bit_xor: return lhs ^ rhs;
    case e_op::bit_nor: return ~(lhs | rhs);
    case e_op::slt: return signed_less(lhs, rhs) ? 1u : 0u;
    case e_op::sltu: return lhs < rhs ? 1u : 0u;
    case e_op::addiu: return sign_extend<32>(lhs + simm);
    case e_op::daddiu: return lhs + simm;
    case e_op::andi: return lhs & imm;
    case e_op::ori: return lhs | imm;
    case e_op::xori: return lhs ^ imm;
    case e_op::slti: return signed_less(lhs, simm) ? 1u : 0u;
    case e_op::sltiu: return lhs < simm ? 1u : 0u;
    case e_op::lui: return sign_extend<32>(imm << 16);
    case e_op::sll:
    case e_op::sllv: return sign_extend<32>(word << shift32);
    case e_op::srl:
    case e_op::srlv: return sign_extend<32>(word >> shift32);
    case e_op::sra:
    case e_op::srav: return sign_extend<32>(arithmetic_right<32>(word, rhs));
    case e_op::dsll:
    case e_op::dsllv: return lhs << shift64;
    case e_op::dsrl:
    case e_op::dsrlv: return lhs >> shift64;
    case e_op::dsra:
    case e_op::dsrav: return arithmetic_right<64>(lhs, rhs);
    case e_op::dsll32: return lhs << (shift32 + 32u);
    case e_op::dsrl32: return lhs >> (shift32 + 32u);
    case e_op::dsra32: return arithmetic_right<64>(lhs, shift32 + 32u);
    }
    return std::nullopt;
}

// Pure value-level model of the scalar GPR contract, independent of runtime
// state, SSA tracking and dirty-half publication. A zero-register write is
// ignored; a scalar write to any other register leaves its high half intact.
struct s_gpr_value
{
    uint64_t m_lo;
    uint64_t m_hi;
};

constexpr s_gpr_value read_gpr(unsigned reg, s_gpr_value stored)
{
    return reg == 0 ? s_gpr_value{0, 0} : stored;
}

constexpr s_gpr_value write_scalar(unsigned reg, s_gpr_value stored, uint64_t lo)
{
    if (reg != 0)
    {
        stored.m_lo = lo;
    }
    return stored;
}
}
