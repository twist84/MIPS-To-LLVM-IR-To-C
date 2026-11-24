#pragma once

#include "ee_vu1.h"

#include <string>

namespace ee
{
// Straight-line LLVM backend for integer lower and selected FMAC upper
// instructions. Empty IR means the entire program uses the interpreter.
// Entry ABI: VI[16], I, TPC, TOP, ITOP, VF[32][4], Q, MAC, STATUS.
// All pointer arguments are disjoint; Q is passed by value.
// Caller must supply initialized state with canonical VI0/VF0. The
// caller owns code identity, issue diagnostics and transactional commit.
// No native execution is selected by the retail runtime yet.
struct s_ee_vu1_ir_program
{
    std::string m_ir;
    uint32_t m_steps = 0u;
    uint32_t m_cycles = 0u;
    uint32_t m_next_pc = 0u;
};

s_ee_vu1_ir_program vu1_lift_straight_line_ir(
    const std::array<uint8_t, c_ee_vu1::k_memory_size>& code,
    uint32_t address);
} // namespace ee
