#pragma once

#include "ee_state.h"
#include <cstdint>

namespace ee
{
class ee_memory;
struct s_ee_runtime;

// Conservatively execute code installed into EE RAM by the game. This is
// a bounded fallback for the static runner, not a full R5900 interpreter.
// Unsupported instructions and unbacked accesses stop with an error.
enum class e_ee_ram_result : uint32_t
{
    RETURNED,
    UNMAPPED,
    UNSUPPORTED,
    BRANCH_IN_DELAY_SLOT,
    INSTRUCTION_BUDGET,
    TIMING_OBSERVER_REJECTED,
    INVALID_TLB_STATE
};

struct s_ee_ram_execution
{
    e_ee_ram_result m_result = e_ee_ram_result::UNSUPPORTED;
    uint32_t m_pc = 0;
    uint32_t m_opcode = 0;
    uint32_t m_instructions = 0;
    uint32_t m_tlb_writes = 0;
};

s_ee_ram_execution ee_execute_ram_callback(
    s_ee_cpu_state* cpu,
    ee_memory* memory,
    uint32_t entry_pc,
    uint32_t return_pc,
    uint32_t max_instructions,
    s_ee_runtime* runtime = nullptr
);

// Opt-in, single ordinary scalar memory instruction. Caller owns the
// execution thread and must establish an ordinary (not delay-slot)
// dispatch boundary and isolated RAM: no pending cache/write-buffer,
// DMA/device effects, native code coherence or clock/interrupt work.
// This evidence is not inferred from PC or the memory attribute.
struct s_ee_strict_ram_evidence
{
    bool m_ordinary_boundary = false;
    bool m_isolated_ram = false;
};

enum class e_ee_strict_ram_step_result : uint8_t
{
    COMMITTED,
    FAULT_COMMITTED,
    UNSAFE_BOUNDARY,
    UNMODELED_ACCESS,
    UNSUPPORTED_INSTRUCTION
};

// No HLE alias fallback, clock updates or handler dispatch. On success
// advances PC by four. On a modeled fault enters Level 1 before any
// data write. All other outcomes leave CPU, RAM and diagnostic unchanged.
e_ee_strict_ram_step_result ee_execute_strict_ram_step(
    s_ee_cpu_state& cpu, ee_memory& memory, bool& actual_delay_slot,
    const s_ee_strict_ram_evidence& evidence);

const char* ee_ram_result_name(e_ee_ram_result result);

} // namespace ee
