#include "ee_scalar_constant.h"
#include "ee_known_bits.h"
#include "ee_ram_interpreter.h"
#include "ee_memory.h"
#include "ee_state.h"

#include <cstdint>
#include <cstdio>
#include <random>

namespace
{
using namespace ee::scalar_constant;
using op = e_op;

static_assert(*evaluate(op::addu, 0x7fffffffu, 1) == 0xffffffff80000000ull);
static_assert(*evaluate(op::addu, 0x12345678ffffffffull, 1) == 0);
static_assert(*evaluate(op::subu, 0, 1) == UINT64_MAX);
static_assert(*evaluate(op::daddu, UINT64_MAX, 1) == 0);
static_assert(*evaluate(op::dsubu, 0, 1) == UINT64_MAX);
static_assert(*evaluate(op::slt, UINT64_MAX, 0) == 1);
static_assert(*evaluate(op::sltu, UINT64_MAX, 0) == 0);
static_assert(*evaluate(op::sltiu, 0, 0xffffu) == 1);
static_assert(*evaluate(op::slti, 0, 0xffffu) == 0);
static_assert(*evaluate(op::addiu, 0, 0x8000u) == 0xffffffffffff8000ull);
static_assert(*evaluate(op::daddiu, 0, 0xffffu) == UINT64_MAX);
static_assert(*evaluate(op::andi, UINT64_MAX, 0xffffu) == 0xffffu);
static_assert(*evaluate(op::ori, 0, 0xffffu) == 0xffffu);
static_assert(*evaluate(op::xori, UINT64_MAX, 0xffffu) == 0xffffffffffff0000ull);
static_assert(*evaluate(op::lui, 0, 0x8000u) == 0xffffffff80000000ull);
static_assert(*evaluate(op::srl, 0x80000000u, 0) == 0xffffffff80000000ull);
static_assert(*evaluate(op::srav, 0x80000000u, 63) == UINT64_MAX);
static_assert(*evaluate(op::sllv, 1, 32) == 1);
static_assert(*evaluate(op::dsllv, 1, 64) == 1);
static_assert(*evaluate(op::dsrav, 0x8000000000000000ull, 63) == UINT64_MAX);
static_assert(*evaluate(op::dsll32, 1, 31) == 0x8000000000000000ull);
static_assert(*evaluate(op::dsrl32, UINT64_MAX, 31) == 1);
static_assert(*evaluate(op::dsra32, 0x8000000000000000ull, 0) == 0xffffffff80000000ull);
static_assert(!evaluate(static_cast<op>(-1), 0, 0).has_value());
static_assert(read_gpr(0, {1, 2}).m_lo == 0 && read_gpr(0, {1, 2}).m_hi == 0);
static_assert(write_scalar(0, {0, 0}, 3).m_lo == 0);
static_assert(write_scalar(1, {1, 2}, 3).m_lo == 3 &&
              write_scalar(1, {1, 2}, 3).m_hi == 2);

static_assert(unsigned_bits(8).m_zero == 0xffffffffffffff00ull);
static_assert(*logical_bits(op::bit_and, unsigned_bits(8), exact_bits(0xff00)).constant() == 0);
static_assert(*less_bits(unsigned_bits(8), exact_bits(256), false) == 1);
static_assert(*less_bits(unsigned_bits(8), exact_bits(UINT64_MAX), true) == 0);
static_assert(!less_bits(unsigned_bits(8), exact_bits(128), false));
static_assert(*logical_bits(op::bit_or, unsigned_bits(8), exact_bits(31)).masked_constant(31) == 31);
static_assert(word_bits({}).m_word);
static_assert(add_low_bits({15, 0, false}, exact_bits(4)).m_zero == 11);
static_assert(add_low_bits({15, 0, false}, exact_bits(4)).m_one == 4);
static_assert(add_low_bits({15, 0, false}, exact_bits(4), true).m_one == 12);
static_assert(add_low_bits({0, 0, false}, exact_bits(4)).empty());
static_assert(*effective_address_bits(exact_bits(0x12345678fffffffcull), 0x44)
    .masked_constant(0xffffffffu) == 0x40);
static_assert(*effective_address_bits({15, 0, false}, -4).masked_constant(15) == 12);
static_assert(!effective_address_bits({3, 0, false}, 0).masked_constant(15));
static_assert(add_low_bits(exact_bits(UINT64_MAX), exact_bits(1)).m_zero == UINT64_MAX);
static_assert(!common_bits(word_bits({}), {}).m_word);
static_assert(*shift_bits(exact_bits(0x80000000), 31, true, false, true).constant() == UINT64_MAX);
static_assert(*shift_bits(exact_bits(0x80000000), 31, true, false, false).constant() == 1);
static_assert(*shift_bits(exact_bits(UINT64_MAX), 63, false, true, false).constant() == (uint64_t{1} << 63));
static_assert(shift_bits(unsigned_bits(8), 4, false, true, false).m_zero == 0xfffffffffffff00full);

enum class e_form { reg, immediate, shift, lui };
struct s_case
{
    op m_op;
    unsigned m_code;
    e_form m_form;
};

constexpr s_case k_interpreted[] = {
    {op::addu, 0x21, e_form::reg}, {op::subu, 0x23, e_form::reg},
    {op::daddu, 0x2d, e_form::reg}, {op::dsubu, 0x2f, e_form::reg},
    {op::bit_and, 0x24, e_form::reg}, {op::bit_or, 0x25, e_form::reg},
    {op::bit_xor, 0x26, e_form::reg}, {op::bit_nor, 0x27, e_form::reg},
    {op::slt, 0x2a, e_form::reg}, {op::sltu, 0x2b, e_form::reg},
    {op::addiu, 0x09, e_form::immediate}, {op::daddiu, 0x19, e_form::immediate},
    {op::andi, 0x0c, e_form::immediate}, {op::ori, 0x0d, e_form::immediate},
    {op::xori, 0x0e, e_form::immediate}, {op::slti, 0x0a, e_form::immediate},
    {op::sltiu, 0x0b, e_form::immediate}, {op::lui, 0x0f, e_form::lui},
    {op::sll, 0x00, e_form::shift}, {op::srl, 0x02, e_form::shift},
    {op::sra, 0x03, e_form::shift}
};

bool compare_interpreter(ee::ee_memory& memory, const s_case& item,
                         uint64_t a, uint64_t b, unsigned rs, unsigned rt,
                         unsigned rd)
{
    constexpr uint32_t pc = 0x80001000u;
    ee::s_ee_cpu_state cpu{};
    s_gpr_value expected[32]{};
    for (unsigned i = 1; i < 32; ++i)
    {
        expected[i] = {0x123456789abcdef0ull ^ i, 0xfedcba9876543210ull ^ i};
    }
    if (rs != 0)
    {
        expected[rs].m_lo = a;
    }
    if (rt != 0 && item.m_form != e_form::immediate && item.m_form != e_form::lui)
    {
        expected[rt].m_lo = b;
    }
    for (unsigned i = 0; i < 32; ++i)
    {
        cpu.m_gpr[i].m_lo = expected[i].m_lo;
        cpu.m_gpr[i].m_hi = expected[i].m_hi;
    }
    uint32_t instruction = 0;
    uint64_t lhs = read_gpr(rs, expected[rs]).m_lo;
    uint64_t rhs = read_gpr(rt, expected[rt]).m_lo;
    unsigned dest = rd;
    if (item.m_form == e_form::reg)
    {
        instruction = (rs << 21) | (rt << 16) | (rd << 11) | item.m_code;
    }
    else if (item.m_form == e_form::shift)
    {
        lhs = rhs;
        rhs = b & 31u;
        instruction = (rt << 16) | (rd << 11) |
            (static_cast<uint32_t>(rhs) << 6) | item.m_code;
    }
    else
    {
        dest = rd;
        rhs = b & 0xffffu;
        const unsigned source = item.m_form == e_form::lui ? 0 : rs;
        lhs = read_gpr(source, expected[source]).m_lo;
        instruction = (item.m_code << 26) | (source << 21) | (dest << 16) |
            static_cast<uint32_t>(rhs);
    }
    const auto value = evaluate(item.m_op, lhs, rhs);
    if (!value)
    {
        return false;
    }
    expected[dest] = write_scalar(dest, expected[dest], *value);
    memory.write32(pc, instruction);
    const auto result = ee::ee_execute_ram_callback(&cpu, &memory, pc, pc + 4, 2);
    if (result.m_result != ee::e_ee_ram_result::RETURNED ||
        result.m_instructions != 1)
    {
        std::fprintf(stderr, "interpreter rejected %08x\n", instruction);
        return false;
    }
    for (unsigned i = 0; i < 32; ++i)
    {
        if (cpu.m_gpr[i].m_lo != expected[i].m_lo ||
            cpu.m_gpr[i].m_hi != expected[i].m_hi)
        {
            std::fprintf(stderr, "scalar mismatch opcode=%08x register=%u\n", instruction, i);
            return false;
        }
    }
    return true;
}

// Independent bit-at-a-time shift oracle, including operations not supported
// by the bounded RAM interpreter. Does not use evaluator masks/sign helpers.
uint64_t reference_shift(uint64_t value, unsigned count, unsigned bits,
                         bool right, bool arithmetic)
{
    const uint64_t top = bits == 32 ? 0x80000000ull : 0x8000000000000000ull;
    const uint64_t mask = bits == 32 ? 0xffffffffull : UINT64_MAX;
    value &= mask;
    for (unsigned i = 0; i < count; ++i)
    {
        if (right)
        {
            const bool negative = (value & top) != 0;
            value /= 2;
            if (arithmetic && negative)
            {
                value |= top;
            }
        }
        else
        {
            value = (value + value) & mask;
        }
    }
    if (bits == 32 && (value & top) != 0)
    {
        value |= 0xffffffff00000000ull;
    }
    return value;
}

bool compare_shifts(uint64_t value, uint64_t count)
{
    const unsigned s32 = static_cast<unsigned>(count % 32);
    const unsigned s64 = static_cast<unsigned>(count % 64);
    const uint64_t l32 = reference_shift(value, s32, 32, false, false);
    const uint64_t r32 = reference_shift(value, s32, 32, true, false);
    const uint64_t a32 = reference_shift(value, s32, 32, true, true);
    const uint64_t l64 = reference_shift(value, s64, 64, false, false);
    const uint64_t r64 = reference_shift(value, s64, 64, true, false);
    const uint64_t a64 = reference_shift(value, s64, 64, true, true);
    return evaluate(op::sll, value, count) == l32 &&
        evaluate(op::sllv, value, count) == l32 &&
        evaluate(op::srl, value, count) == r32 &&
        evaluate(op::srlv, value, count) == r32 &&
        evaluate(op::sra, value, count) == a32 &&
        evaluate(op::srav, value, count) == a32 &&
        evaluate(op::dsll, value, count) == l64 &&
        evaluate(op::dsllv, value, count) == l64 &&
        evaluate(op::dsrl, value, count) == r64 &&
        evaluate(op::dsrlv, value, count) == r64 &&
        evaluate(op::dsra, value, count) == a64 &&
        evaluate(op::dsrav, value, count) == a64 &&
        evaluate(op::dsll32, value, count) == reference_shift(value, s32 + 32, 64, false, false) &&
        evaluate(op::dsrl32, value, count) == reference_shift(value, s32 + 32, 64, true, false) &&
        evaluate(op::dsra32, value, count) == reference_shift(value, s32 + 32, 64, true, true);
}
}

int main()
{
    constexpr uint64_t edges[] = {
        0, 1, 31, 32, 63, 64, 0x7fff, 0x8000, 0xffff,
        0x7fffffff, 0x80000000, 0xffffffff, 0x100000000ull,
        0x7fffffffffffffffull, 0x8000000000000000ull, UINT64_MAX
    };
    ee::ee_memory memory(2u * 1024u * 1024u);
    memory.set_kseg_ram_aliases(true);
    for (const auto a : edges)
    {
        for (const auto b : edges)
        {
            if (!compare_shifts(a, b))
            {
                std::fprintf(stderr, "edge shift mismatch\n");
                return 1;
            }
            for (const auto& item : k_interpreted)
            {
                if (!compare_interpreter(memory, item, a, b, 1, 2, 3) ||
                    !compare_interpreter(memory, item, a, b, 0, 2, 0) ||
                    !compare_interpreter(memory, item, a, b, 1, 1, 1) ||
                    !compare_interpreter(memory, item, a, b, 1, 0, 2))
                {
                    return 1;
                }
            }
        }
    }
    std::mt19937_64 rng(0x5900);
    for (unsigned i = 0; i < 256; ++i)
    {
        const uint64_t a = rng(), b = rng();
        if (!compare_shifts(a, b))
        {
            std::fprintf(stderr, "random shift mismatch\n");
            return 1;
        }
        for (const auto& item : k_interpreted)
        {
            if (!compare_interpreter(memory, item, a, b, 1, 2, 3))
            {
                return 1;
            }
        }
    }
    return 0;
}
