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
    pair(code, 8u, k_nop, 0x29u); // unsupported MADD
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
    const bool branches = std::strcmp(argv[2], "branches") == 0 ||
        std::strcmp(argv[2], "branch-wrap") == 0;
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
    if (program.m_ir.empty() || (program.m_steps == 0u) != branches ||
        program.m_writes_data != (std::strcmp(argv[2], "stores") == 0))
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
    state.m_mac = 0x1234u;
    state.m_status = 0x30u;
    state.m_vf[2] = {0x3f800000u, 0x80000001u, 0x7fffffffu, 0xbf800000u};
    state.m_vf[3] = {0x40000000u, 0x80000000u, 0x7f7fffffu, 0x3f800000u};
    state.m_vf[6] = {11u, 22u, 33u, 44u};
    state.m_vf[13] = {55u, 66u, 77u, 88u};
    ee::s_ee_vu1_issue issue{};
    std::vector<uint8_t> path1;
    if (!ee::c_ee_vu1::run(state, code, data, start, 0x12345u, 0x23456u,
            path1, &issue) || !path1.empty() ||
        (!branches && (issue.m_steps != program.m_steps ||
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
    std::printf(" %u %u", state.m_mac, state.m_status);
    for (uint8_t value : data)
    {
        std::printf(" %u", static_cast<unsigned>(value));
    }
    std::puts("");
    return 0;
}
