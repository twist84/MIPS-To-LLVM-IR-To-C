#pragma once

#include "ee_vu1.h"

#include <string>

namespace ee
{
// Initial LLVM backend: straight-line upper NOP + integer lower pairs.
// Empty IR means the entire program must use the interpreter. The
// generated entry takes separate VI[16], I and TPC pointers plus TOP/ITOP.
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

s_ee_vu1_ir_program vu1_lift_integer_ir(
    const std::array<uint8_t, c_ee_vu1::k_memory_size>& code,
    uint32_t address);
} // namespace ee
