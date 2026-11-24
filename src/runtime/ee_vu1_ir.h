#pragma once

#include "ee_vu1.h"

#include <string>

namespace ee
{
// LLVM backend for straight-line and bounded branching integer/FMAC
// instructions. Empty IR means the entire program uses the interpreter.
// Entry ABI: VI[16], I, TPC, TOP, ITOP, VF[32][4], Q, MAC, STATUS,
// data[16384], cycles_out, last_pc_out, ACC[4], CLIP. Return 0 requests rollback/fallback.
// Stores require a staged data-memory copy before native execution.
// All pointer arguments are disjoint; Q is passed by value.
// Caller must supply initialized state with canonical VI0/VF0. The
// caller owns code identity, issue diagnostics and transactional commit.
// Native execution is opt-in; unsupported programs are interpreted.
struct s_ee_vu1_ir_program
{
    std::string m_ir;
    // Nonempty IR with zero steps denotes dynamic control flow.
    uint32_t m_steps = 0u;
    uint32_t m_cycles = 0u;
    uint32_t m_next_pc = 0u;
    bool m_writes_data = false;
};

s_ee_vu1_ir_program vu1_lift_straight_line_ir(
    const std::array<uint8_t, c_ee_vu1::k_memory_size>& code,
    uint32_t address);
s_ee_vu1_ir_program vu1_lift_program_ir(
    const std::array<uint8_t, c_ee_vu1::k_memory_size>& code,
    uint32_t address);
// Produce a self-describing module. Dynamic programs embed the full image;
// straight-line programs embed only reached instruction bytes.
// Empty means unsupported; no partial executable program is emitted.
std::string vu1_aot_module_ir(
    const std::array<uint8_t, c_ee_vu1::k_memory_size>& code,
    uint32_t address, bool windows_exports);
} // namespace ee
