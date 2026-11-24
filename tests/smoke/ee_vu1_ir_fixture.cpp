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
    if (!ee::vu1_lift_integer_ir(code, 0u).m_ir.empty() ||
        !ee::vu1_lift_integer_ir(code, 3u).m_ir.empty())
    {
        return false;
    }
    pair(code, 0u, k_nop, k_end);
    // Unsupported delay slot, not an early successful exit at E.
    pair(code, 8u, 0x40000000u, k_upper);
    if (!ee::vu1_lift_integer_ir(code, 0u).m_ir.empty())
    {
        return false;
    }
    pair(code, 8u, k_nop, k_upper | 0x10000000u);
    return ee::vu1_lift_integer_ir(code, 0u).m_ir.empty();
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
    const uint32_t start = std::strcmp(argv[2], "wrap") == 0 ? 0x3ff0u : 0x80u;
    t_code code{}, data{};
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
    append(k_nop, k_end);
    append(integer(0x30u, 12u, 10u, 11u)); // E delay slot
    const auto program = ee::vu1_lift_integer_ir(code, start);
    if (program.m_ir.empty())
    {
        return 1;
    }
    ee::s_ee_vu1_state state{};
    state.m_initialized = true;
    state.m_vf[0][3] = 0x3f800000u;
    state.m_vi[1] = 0xfffeu;
    state.m_vi[2] = 5u;
    state.m_i = 0x12345678u;
    ee::s_ee_vu1_issue issue{};
    std::vector<uint8_t> path1;
    if (!ee::c_ee_vu1::run(state, code, data, start, 0x12345u, 0x23456u,
            path1, &issue) || !path1.empty() ||
        issue.m_steps != program.m_steps || issue.m_cycles != program.m_cycles ||
        state.m_tpc != program.m_next_pc)
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
    std::printf("%u %u %u %u", issue.m_steps, issue.m_cycles, state.m_tpc, state.m_i);
    for (uint16_t value : state.m_vi)
    {
        std::printf(" %u", static_cast<unsigned>(value));
    }
    std::puts("");
    return 0;
}
