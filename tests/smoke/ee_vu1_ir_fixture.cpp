#include "ee_vu1_ir.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace
{
constexpr uint32_t k_nop = 0x8000033cu;
constexpr uint32_t k_upper = 0x000002ffu;
constexpr uint32_t k_end = k_upper | 0x40000000u;

using t_code = std::array<uint8_t, ee::c_ee_vu1::k_memory_size>;

void pair(t_code& code, uint32_t pc, uint32_t lo, uint32_t hi)
{
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        code[pc + i] = static_cast<uint8_t>(lo >> (i * 8u));
        code[pc + 4u + i] = static_cast<uint8_t>(hi >> (i * 8u));
    }
}

uint32_t integer(uint32_t funct, uint32_t id, uint32_t is, uint32_t it)
{
    return 0x80000000u | (it << 16u) | (is << 11u) | (id << 6u) | funct;
}

bool rejection_tests()
{
    t_code code{};
    for (uint32_t pc = 0u; pc < code.size(); pc += 8u)
    {
        pair(code, pc, k_nop, k_upper);
    }
    if (!ee::vu1_lift_straight_line_ir(code, 0u).m_ir.empty() ||
        !ee::vu1_lift_straight_line_ir(code, 3u).m_ir.empty())
    {
        return false;
    }
    pair(code, 0u, k_nop, k_end);
    // Unsupported delay slot, not an early successful exit at E.
    pair(code, 8u, 0x40000000u, k_upper);
    if (!ee::vu1_lift_straight_line_ir(code, 0u).m_ir.empty())
    {
        return false;
    }
    pair(code, 8u, k_nop, k_upper | 0x10000000u);
    if (!ee::vu1_lift_straight_line_ir(code, 0u).m_ir.empty())
    {
        return false;
    }
    pair(code, 8u, 0x800006fdu, k_upper); // unknown lower index 0x6d
    return ee::vu1_lift_straight_line_ir(code, 0u).m_ir.empty();
}
} // namespace

int main(int argc, char** argv)
{
    if (!rejection_tests())
    {
        return 1;
    }
    if (argc == 1)
    {
        return 0;
    }
    if (argc != 3)
    {
        return 2;
    }
    const uint32_t start = std::strcmp(argv[2], "wrap") == 0 ||
        std::strcmp(argv[2], "branch-wrap") == 0 ? 0x3ff0u : 0x80u;
    t_code code{}, data{};
    for (size_t i = 0u; i < data.size(); ++i)
    {
        data[i] = static_cast<uint8_t>((i * 13u + (i >> 8u)) & 255u);
    }
    const bool conversion_case = std::strcmp(argv[2], "conversions") == 0;
    if (conversion_case)
    {
        constexpr uint32_t edges[] = {
            0u, 0x80000000u, 1u, 0x807fffffu, 0x00800000u, 0x80800000u,
            0x3f7fffffu, 0xbf7fffffu, 0x3f800000u, 0xbf800000u,
            0x3fc00000u, 0xbfc00000u, 0x4effffffu, 0xceffffffu,
            0x4f000000u, 0xcf000000u, 0x7fffffffu, 0xffffffffu,
            0x7f800000u, 0xff800000u, 0x00ffffffu, 0x01000001u,
            0x7ffffffeu, 0x80000001u, 0x01000000u, 0x01000001u,
            0x01ffffffu, 0x02000001u, 0xfeffffffu, 0x3f000000u,
            0x40000000u, 0xc0000000u
        };
        uint32_t random = 0x12345678u;
        for (uint32_t i = 0u; i < 128u; ++i)
        {
            random = random * 1664525u + 1013904223u;
            const uint32_t value = i < 32u ? edges[i] : random;
            for (uint32_t b = 0u; b < 4u; ++b)
            {
                data[0x3000u + i * 4u + b] = static_cast<uint8_t>(value >> (b * 8u));
            }
        }
    }
    uint32_t pc = start;
    const auto append = [&](uint32_t lo, uint32_t hi = k_upper)
    {
        pair(code, pc, lo, hi);
        pc = (pc + 8u) & 0x3fffu;
    };
    append(integer(0x30u, 3u, 1u, 2u));
    append(integer(0x31u, 4u, 3u, 2u));
    append(integer(0x32u, 16u, 4u, 5u)); // IADDI -16
    append(integer(0x34u, 6u, 4u, 5u));
    append(integer(0x35u, 7u, 6u, 2u));
    append(integer(0x30u, 0u, 1u, 2u)); // VI0 remains zero
    append((0x08u << 25u) | (8u << 16u) | (7u << 11u) | 0x7ffu);
    append((0x09u << 25u) | (9u << 16u) | (8u << 11u) | 0x7ffu);
    append(0x80000000u | (10u << 16u) | ((0x68u >> 2u) << 6u) | 0x3cu);
    append(0x80000000u | (11u << 16u) | ((0x69u >> 2u) << 6u) | 0x3du);
    append(0xdeadbeefu, k_upper | 0x80000000u);
    const auto upper = [](uint32_t op, uint32_t fd, uint32_t fs,
                          uint32_t ft, uint32_t mask)
    {
        return op | (fd << 6u) | (fs << 11u) | (ft << 16u) | (mask << 21u);
    };
    if (std::strcmp(argv[2], "integer") != 0)
    {
        append(k_nop, upper(0x28u, 4u, 2u, 3u, 15u));
        append(k_nop, upper(0x2au, 5u, 4u, 3u, 15u)); // RAW stall
        append(k_nop, upper(0x2cu, 6u, 5u, 4u, 10u));
        append(k_nop, upper(0x01u, 3u, 2u, 3u, 15u)); // broadcast alias
        append(k_nop, upper(0x2bu, 7u, 2u, 3u, 15u));
        append(k_nop, upper(0x2fu, 8u, 2u, 3u, 15u));
        append(k_nop, upper(0x3du, 7u, 2u, 9u, 15u)); // ABS special 0x1d
        append(0x40000000u, upper(0x22u, 10u, 2u, 0u, 15u) | 0x80000000u);
        append(k_nop, upper(0x20u, 11u, 2u, 0u, 15u)); // ADDq
        append(k_nop, upper(0x22u, 12u, 2u, 0u, 15u)); // new I
        append(k_nop, upper(0x2cu, 0u, 2u, 2u, 15u)); // flags, VF0 preserved
        append(k_nop, upper(0x2au, 13u, 2u, 3u, 5u));
        // Exact SDBZ LQ encoding and a negative offset wrapping VU RAM.
        append(0x01e10000u, k_upper);
        append((15u << 21u) | (14u << 16u) | (1u << 11u) | 0x7ffu, k_upper);
        // Upper reads the old VF2; its X write wins over same-pair LQ.xw.
        append((9u << 21u) | (2u << 16u) | 1u, upper(0x28u, 2u, 2u, 3u, 8u));
        append(0x81062bfeu, k_upper); // captured ILWR
        append((4u << 25u) | (15u << 21u) | (14u << 16u) | 3u, k_upper);
        append(integer(0x30u, 15u, 14u, 6u), k_upper); // VI load stall
        const auto special = [](uint32_t index, uint32_t ft, uint32_t fs, uint32_t mask)
        {
            return 0x80000000u | (mask << 21u) | (ft << 16u) | (fs << 11u) |
                ((index >> 2u) << 6u) | 0x3cu | (index & 3u);
        };
        append(special(0x34u, 16u, 1u, 15u), k_upper);
        append(special(0x36u, 17u, 1u, 10u), k_upper);
        append(special(0x31u, 16u, 16u, 15u), k_upper); // aliased MR32
        append(special(0x30u, 18u, 17u, 15u), k_upper);
        append(special(0x3cu, 5u, 18u, 1u), k_upper);
        append(special(0x3du, 19u, 5u, 15u), k_upper);
        if (std::strcmp(argv[2], "stores") == 0)
        {
            // SQ uses IT as the address, FS as the vector. Negative
            // offset wraps; unselected lanes must remain unchanged.
            append((1u << 25u) | (10u << 21u) | (1u << 16u) |
                (2u << 11u) | 0x7ffu, upper(0x28u, 2u, 2u, 3u, 15u));
            append(special(0x35u, 1u, 2u, 15u), k_upper);
            append(special(0x37u, 1u, 3u, 5u), k_upper);
            append(special(0x35u, 0u, 0u, 15u), k_upper);
            append(special(0x37u, 0u, 3u, 8u), k_upper);
            append((5u << 25u) | (9u << 21u) | (1u << 16u) |
                0x7ffu, k_upper); // ISW zero-extends 0xfffe
            append(special(0x3fu, 2u, 1u, 6u), k_upper);
            append((15u << 21u) | (20u << 16u) | (1u << 11u), k_upper);
        }
    }
    if (conversion_case)
    {
        // Eight conversion opcodes x 128 inputs. Save each result to
        // a separate quadword, so later conversions cannot hide errors.
        for (uint32_t op = 0x10u; op <= 0x17u; ++op)
        {
            const uint32_t index = op - 0x10u;
            for (uint32_t j = 0u; j < 32u; ++j)
            {
                append((15u << 21u) | (26u << 16u) | (0x300u + j), k_upper);
                const uint32_t target = (j & 1u) ? 26u : 27u;
                append(k_nop, upper(0x3cu | (op & 3u), op >> 2u,
                    26u, target, 15u));
                append((1u << 25u) | (15u << 21u) | (target << 11u) |
                    (0x100u + index * 32u + j), k_upper);
            }
        }
        // Partial lane write, overlapping LQ in the same pair, VF0
        // suppression, and unchanged accumulator/flags.
        append((15u << 21u) | (26u << 16u) | 0x300u, k_upper);
        append((15u << 21u) | (27u << 16u) | 0x301u,
            upper(0x3cu, 0x14u >> 2u, 26u, 27u, 10u));
        append(k_nop, upper(0x3cu, 0x10u >> 2u, 27u, 0u, 15u));
    }
    const bool acc_case = std::strcmp(argv[2], "acc") == 0 ||
        std::strcmp(argv[2], "acc-branches") == 0;
    if (acc_case)
    {
        const auto accumulator = [&](uint32_t index, uint32_t fs,
            uint32_t ft, uint32_t mask)
        {
            return upper(0x3cu | (index & 3u), index >> 2u, fs, ft, mask);
        };
        // Every supported ACC arithmetic opcode, interleaved with a
        // non-ACC MADD reading it into a distinct VF destination.
        // VU FP special values and partial masks are carried forward.
        constexpr uint32_t ops[] = {
            0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u,
            0x08u, 0x09u, 0x0au, 0x0bu, 0x0cu, 0x0du, 0x0eu, 0x0fu,
            0x18u, 0x19u, 0x1au, 0x1bu, 0x1cu, 0x1eu, 0x20u, 0x21u,
            0x22u, 0x23u, 0x24u, 0x25u, 0x26u, 0x27u, 0x28u, 0x29u,
            0x2au, 0x2cu, 0x2du, 0x2eu
        };
        // Store every result to an independent memory slot so later
        // arithmetic cannot hide an incorrect earlier result.
        for (uint32_t j = 0u; j < sizeof(ops) / sizeof(ops[0]); ++j)
        {
            append(k_nop, accumulator(ops[j], 21u, 22u, (j & 1u) ? 5u : 15u));
            append(k_nop, upper(0x29u, 23u, 0u, 0u, 15u));
            append((1u << 25u) | (15u << 21u) | (23u << 11u) |
                (0x100u + j), k_upper);
        }
        // All VF-target MADD/MSUB variants, including broadcast, I/Q,
        // zero destination, source aliases and cross-product XYZ mask.
        constexpr uint32_t mac_ops[] = {
            0x08u, 0x09u, 0x0au, 0x0bu, 0x0cu, 0x0du, 0x0eu, 0x0fu,
            0x21u, 0x23u, 0x25u, 0x27u, 0x29u, 0x2du, 0x2eu
        };
        for (uint32_t j = 0u; j < sizeof(mac_ops) / sizeof(mac_ops[0]); ++j)
        {
            append(k_nop, upper(mac_ops[j], 24u, 21u, 22u, 15u));
            append((1u << 25u) | (15u << 21u) | (24u << 11u) |
                (0x140u + j), k_upper);
        }
        append(k_nop, upper(0x29u, 22u, 21u, 22u, 10u));
        append(k_nop, upper(0x2du, 0u, 21u, 22u, 15u));
        append(k_nop, accumulator(0x2eu, 21u, 22u, 1u));
        append(k_nop, upper(0x2eu, 25u, 21u, 22u, 1u));
    }
    const bool flag_case = std::strcmp(argv[2], "flags") == 0;
    if (flag_case)
    {
        const auto lower_flag = [](uint32_t major, uint32_t it, uint32_t is, uint32_t imm)
        {
            return (major << 25u) | (it << 16u) | (is << 11u) |
                (imm & 0x7ffu) | ((imm & 0x800u) << 10u);
        };
        uint32_t slot = 0x200u;
        const auto save = [&](uint32_t vi)
        {
            append((5u << 25u) | (15u << 21u) | (vi << 16u) | slot++, k_upper);
        };
        const auto clip = upper(0x3fu, 0x1fu >> 2u, 21u, 3u, 0u);
        append((0x11u << 25u) | 0x123456u);
        append((0x10u << 25u) | 0x123456u); save(1u);
        append((0x12u << 25u) | 0x100000u); save(1u);
        append((0x13u << 25u) | 0xedcba9u); save(1u);
        append(lower_flag(0x1cu, 6u, 0u, 0u), clip); save(6u);
        append(lower_flag(0x1cu, 6u, 0u, 0u)); save(6u);
        append(lower_flag(0x1cu, 6u, 0u, 0u)); save(6u);
        // More than eight writes, including same-pair FCSET + CLIP.
        for (uint32_t j = 0u; j < 12u; ++j)
        {
            append(j & 1u ? (0x11u << 25u) | (j * 0x11111u) :
                lower_flag(0x1cu, 6u, 0u, 0u), clip);
        }
        append(lower_flag(0x1cu, 6u, 0u, 0u)); save(6u);
        // MAC/STATUS use issue count, not scoreboard-stalled cycles.
        append(k_nop, upper(0x2cu, 23u, 21u, 21u, 15u));
        append(lower_flag(0x16u, 7u, 0u, 0xfffu),
            upper(0x2au, 24u, 23u, 21u, 15u)); save(7u);
        append(lower_flag(0x17u, 7u, 0u, 0x800u)); save(7u);
        append(lower_flag(0x14u, 7u, 0u, 0x30u)); save(7u);
        append(lower_flag(0x15u, 0u, 0u, 0xac0u),
            upper(0x2au, 24u, 21u, 22u, 15u));
        append(lower_flag(0x16u, 7u, 0u, 0xfffu)); save(7u);
        append(lower_flag(0x18u, 7u, 2u, 0u)); save(7u);
        append(lower_flag(0x1au, 7u, 2u, 0u)); save(7u);
        append(lower_flag(0x1bu, 7u, 2u, 0u)); save(7u);
        // A flag-produced VI is exempt from previous-VI branch delay.
        append((0x11u << 25u) | 1u);
        append((0x12u << 25u) | 1u);
        append((0x29u << 25u) | (1u << 11u) | 2u);
        append(k_nop);
        append(integer(0x32u, 7u, 15u, 15u));
        append(k_nop);
    }
    const bool q_case = std::strcmp(argv[2], "q") == 0;
    if (q_case)
    {
        const auto scalar = [](uint32_t index, uint32_t fs, uint32_t ft,
            uint32_t fsf, uint32_t ftf)
        {
            return 0x80000000u | (fsf << 21u) | (ftf << 23u) |
                (ft << 16u) | (fs << 11u) | ((index >> 2u) << 6u) |
                0x3cu | (index & 3u);
        };
        const auto fsand = [](uint32_t it)
        {
            return (0x16u << 25u) | (it << 16u) | 0x200430u;
        };
        // Independent literal checks in the Python runner: old/current
        // FDIV causes and sticky flags, without a new FMAC history entry.
        append(0x15u << 25u);
        append(scalar(0x38u, 21u, 22u, 0u, 0u));
        append(scalar(0x3bu, 0u, 0u, 0u, 0u));
        append(fsand(10u));
        append(scalar(0x38u, 21u, 0u, 0u, 0u));
        append(fsand(11u));
        append(scalar(0x3bu, 0u, 0u, 0u, 0u));
        // VI12 is overwritten by the shared E delay slot; retain this
        // observation in VI9 instead.
        append(fsand(9u));
        append(scalar(0x38u, 21u, 22u, 0u, 0u));
        append(scalar(0x3bu, 0u, 0u, 0u, 0u));
        append(fsand(13u));
        append(0x15u << 25u);
        append(fsand(14u));
        // Q must be read before a same-pair producer/wait.
        append(scalar(0x38u, 21u, 22u, 0u, 0u),
            upper(0x20u, 26u, 0u, 0u, 15u));
        append(k_nop, upper(0x20u, 27u, 0u, 0u, 15u));
        append(scalar(0x3bu, 0u, 0u, 0u, 0u), upper(0x20u, 28u, 0u, 0u, 15u));
        append(k_nop, upper(0x20u, 29u, 0u, 0u, 15u));
        // Save every observation. A second producer stalls for the
        // earlier Q; zero divisors and negative sqrt operands use VU rules.
        for (uint32_t j = 0u; j < 4u; ++j)
        {
            append(scalar(0x38u, 21u, 22u, j, j));
            append(scalar(0x39u, 0u, 21u, 0u, j));
            append(scalar(0x3au, 21u, 22u, j, j),
                upper(0x20u, 30u, 0u, 0u, 15u));
            append(scalar(0x3bu, 0u, 0u, 0u, 0u));
            append(k_nop, upper(0x20u, 31u, 0u, 0u, 15u));
            append((1u << 25u) | (15u << 21u) | (30u << 11u) | (0x240u + j * 2u));
            append((1u << 25u) | (15u << 21u) | (31u << 11u) | (0x241u + j * 2u));
        }
        // CLIP commits after the lower's Q stall; flags retain their
        // different issue-count vs elapsed-cycle visibility.
        append(scalar(0x38u, 21u, 0u, 1u, 0u),
            upper(0x3fu, 0x1fu >> 2u, 21u, 3u, 0u));
        append(scalar(0x3bu, 0u, 0u, 0u, 0u),
            upper(0x3fu, 0x1fu >> 2u, 21u, 3u, 0u));
        append((0x1cu << 25u) | (8u << 16u));
        append(scalar(0x38u, 21u, 22u, 0u, 0u)); // E drain
    }
    const bool random_case = std::strcmp(argv[2], "random") == 0;
    if (random_case)
    {
        const auto special = [](uint32_t index, uint32_t fs, uint32_t ft, uint32_t mask)
        {
            return 0x80000000u | (mask << 21u) | (ft << 16u) |
                (fs << 11u) | ((index >> 2u) << 6u) | 0x3cu | (index & 3u);
        };
        append(special(0x42u, 21u, 0u, 0u)); // RINIT.x
        append(special(0x40u, 0u, 26u, 10u)); // RNEXT.xz
        append(special(0x41u, 0u, 27u, 15u)); // RGET
        append(special(0x43u, 22u, 0u, 2u)); // RXOR.z
        append(special(0x40u, 0u, 0u, 15u)); // advance R even with VF0
        append(special(0x41u, 0u, 28u, 15u));
    }
    const bool branches = std::strcmp(argv[2], "branches") == 0 ||
        std::strcmp(argv[2], "branch-wrap") == 0 ||
        std::strcmp(argv[2], "acc-branches") == 0;
    if (branches)
    {
        const auto branch = [](uint32_t major, uint32_t is, uint32_t it, int32_t imm)
        {
            return (major << 25u) | (it << 16u) | (is << 11u) |
                (static_cast<uint32_t>(imm) & 0x7ffu);
        };
        // VI13=3; IBNE observes the value before the immediately
        // preceding decrement. The loop therefore executes four times.
        append((0x08u << 25u) | (13u << 16u) | 3u);
        append(integer(0x32u, 31u, 13u, 13u)); // -1
        append(branch(0x29u, 13u, 0u, -2));
        append(integer(0x32u, 1u, 15u, 15u)); // branch delay slot
        // Exercise all conditional predicates, both branch outcomes,
        // and the independent NPC on an E-bit branch.
        append(branch(0x28u, 0u, 0u, 2));
        append(integer(0x32u, 1u, 15u, 15u));
        append(integer(0x32u, 7u, 15u, 15u)); // skipped
        append(branch(0x2cu, 1u, 0u, 2));
        append(k_nop);
        append(integer(0x32u, 7u, 15u, 15u)); // skipped
        append(branch(0x2du, 0u, 0u, 2));
        append(k_nop);
        append(integer(0x32u, 1u, 15u, 15u)); // not taken
        append(branch(0x2eu, 0u, 0u, 2));
        append(k_nop);
        append(integer(0x32u, 7u, 15u, 15u));
        append(branch(0x2fu, 1u, 0u, 2));
        append(k_nop);
        append(integer(0x32u, 1u, 15u, 15u));
        // Preserve only the VI written by the previous pair. The other
        // operand must be live, and a NOP must invalidate the saved value.
        append((0x08u << 25u) | (3u << 16u) | 7u); // VI3 = 7
        append((0x08u << 25u) | (4u << 16u) | 7u); // VI4 = 7
        append((0x08u << 25u) | (3u << 16u) | 9u); // VI3 = 9
        append(branch(0x28u, 3u, 4u, 2)); // old VI3 == live VI4
        append(k_nop);
        append(integer(0x32u, 7u, 15u, 15u)); // skipped
        append((0x08u << 25u) | (3u << 16u) | 7u); // VI3 = 7
        append(k_nop);
        append(branch(0x28u, 3u, 4u, 2)); // live VI3 == live VI4
        append(k_nop);
        append(integer(0x32u, 7u, 15u, 15u)); // skipped
        // ILW can overwrite the same VI for several selected lanes. Its
        // delayed value must be the original VI, not an intermediate lane.
        append((0x08u << 25u) | (3u << 16u) | 7u);
        append((0x04u << 25u) | (15u << 21u) | (3u << 16u));
        append(branch(0x28u, 3u, 4u, 2));
        append(k_nop);
        append(integer(0x32u, 7u, 15u, 15u)); // skipped
        // BAL calls forward; JR returns to the discovered continuation.
        append(branch(0x21u, 0u, 6u, 4));
        append(k_nop);
        append(branch(0x20u, 0u, 0u, 5));
        append(k_nop);
        append(k_nop);
        append(integer(0x32u, 1u, 15u, 15u));
        append(branch(0x24u, 6u, 0u, 0));
        append(k_nop);
        append(branch(0x20u, 0u, 0u, 3), k_end);
        append(integer(0x32u, 1u, 15u, 15u));
    }
    else
    {
        append(k_nop, k_end);
        append(integer(0x30u, 12u, 10u, 11u)); // E delay slot
    }
    const auto program = ee::vu1_lift_program_ir(code, start);
    if (program.m_ir.empty() || (program.m_steps == 0u) != (branches || flag_case || q_case) ||
        program.m_writes_data != (conversion_case || acc_case || flag_case || q_case ||
            std::strcmp(argv[2], "stores") == 0))
    {
        return 1;
    }
    if (branches && (program.m_ir.find("%previous_vi_mask") == std::string::npos ||
        program.m_ir.find("load <16 x i16>") != std::string::npos ||
        program.m_ir.find("store <16 x i16>") != std::string::npos ||
        program.m_ir.find("getelementptr i16, ptr %previous_vi, i32 15") != std::string::npos ||
        program.m_ir.find("getelementptr i16, ptr %previous_vi, i32 13") == std::string::npos))
    {
        return 1;
    }
    // The Q/flag pipeline can require dynamic IR without consuming
    // delayed conditional VI operands. It should have no snapshot state.
    if (q_case && program.m_ir.find("%previous_vi") != std::string::npos)
    {
        return 1;
    }
    ee::s_ee_vu1_state state{};
    state.m_initialized = true;
    state.m_vf[0][3] = 0x3f800000u;
    state.m_vi[1] = 0xfffeu;
    state.m_vi[2] = 5u;
    state.m_i = 0x12345678u;
    state.m_q = 0x3f800000u;
    state.m_acc = {0x3f000000u, 0xbf800000u, 0x80000000u, 0x7fffffffu};
    state.m_vf[21] = {0x3f800001u, 0xbf800001u, 0x7fffffffu, 0x00800000u};
    state.m_vf[22] = {0x40000000u, 0x3f800000u, 0x80000000u, 0x00800001u};
    state.m_vf[25][3] = 0x12345678u;
    state.m_mac = 0x1234u;
    state.m_status = 0x30u;
    state.m_clip = 0x345678u;
    state.m_p = 0x12345678u;
    state.m_r = 0x54321u;
    state.m_vf[2] = {0x3f800000u, 0x80000001u, 0x7fffffffu, 0xbf800000u};
    state.m_vf[3] = {0x40000000u, 0x80000000u, 0x7f7fffffu, 0x3f800000u};
    state.m_vf[6] = {11u, 22u, 33u, 44u};
    state.m_vf[13] = {55u, 66u, 77u, 88u};
    ee::s_ee_vu1_issue issue{};
    std::vector<uint8_t> path1;
    if (!ee::c_ee_vu1::run(state, code, data, start, 0x12345u, 0x23456u,
            path1, &issue) || !path1.empty() ||
        (!branches && !flag_case && !q_case && (issue.m_steps != program.m_steps ||
            issue.m_cycles != program.m_cycles || state.m_tpc != program.m_next_pc)))
    {
        return 1;
    }
    std::ofstream output(argv[1], std::ios::binary);
    output << program.m_ir;
    output.close();
    if (!output)
    {
        return 1;
    }
    std::printf("%u %u %u %u %u", issue.m_steps, issue.m_cycles, state.m_tpc,
        state.m_i, issue.m_pc);
    for (uint16_t value : state.m_vi)
    {
        std::printf(" %u", static_cast<unsigned>(value));
    }
    for (const auto& reg : state.m_vf)
    {
        for (uint32_t value : reg)
        {
            std::printf(" %u", value);
        }
    }
    std::printf(" %u %u %u %u %u %u", state.m_mac, state.m_status, state.m_clip,
        state.m_q, state.m_p, state.m_r);
    for (uint32_t value : state.m_acc)
    {
        std::printf(" %u", value);
    }
    std::printf(" %u", state.m_acc_overflow_mask);
    for (uint8_t value : data)
    {
        std::printf(" %u", static_cast<unsigned>(value));
    }
    std::puts("");
    return 0;
}
