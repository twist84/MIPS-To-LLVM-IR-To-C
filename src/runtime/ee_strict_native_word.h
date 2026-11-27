#pragma once

#include "ee_ram_interpreter.h"

namespace ee
{
// Separate experimental ABI: not s_ee_runtime or ee_entry. The caller
// owns the objects and the isolated execution boundary for the entire
// native call, including the transfer after successful preparation.
struct s_ee_strict_native_word_context
{
    ee_memory* m_memory = nullptr;
    bool* m_actual_delay_slot = nullptr;
    s_ee_strict_ram_evidence m_evidence{};
};

inline constexpr bool ee_strict_word_supported(uint32_t opcode)
{
    return (opcode >> 26u) == 0x23u || (opcode >> 26u) == 0x2bu;
}

// Returns the strict step result, not e_ee_exit_reason. COMMITTED (0)
// means preparation succeeded; the generated success edge must finish
// the transfer and advance PC synchronously. No pointer may escape.
// On fault/rejection the out pointer is null; no transfer is allowed.
extern "C" uint32_t ee_strict_native_prepare_word(
    s_ee_cpu_state* cpu, s_ee_strict_native_word_context* context,
    uint32_t expected_pc, uint32_t expected_opcode, uint8_t** out);

} // namespace ee
