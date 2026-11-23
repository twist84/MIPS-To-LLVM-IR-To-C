#pragma once

#include "ee_decoder.h"
#include "ee_issue_timing_probe.h"
#include "ee_runtime.h"

#include <cstdint>

namespace ee
{
// Entirely experimental: zero means the caller has not supplied a latency.
// Supplying numbers does NOT validate them as R5900 hardware timing.
struct s_ee_issue_replay_profile
{
    uint32_t m_integer_latency = 0u;
    uint32_t m_load_latency = 0u;
    // All four fields default to unavailable. A caller must supply timing
    // evidence for each distinct HI/LO operation before it can be replayed.
    uint32_t m_hilo_multiply_latency = 0u;
    uint32_t m_hilo_divide_latency = 0u;
    uint32_t m_hilo_transfer_latency = 0u;
    uint32_t m_hilo_accumulate_latency = 0u;

    // All COP1 latencies must also be supplied individually. Zero keeps
    // that class unavailable, even when scalar GPR timing is configured.
    uint32_t m_fpu_transfer_latency = 0u;
    uint32_t m_fpu_arithmetic_latency = 0u;
    uint32_t m_fpu_divide_latency = 0u;
    uint32_t m_fpu_accumulate_latency = 0u;
    // Explicit FCR31 transfer/branch and comparison-result availability.
    // Still uncalibrated; zero means the operation is not replayable.
    uint32_t m_fpu_control_latency = 0u;
    uint32_t m_fpu_compare_latency = 0u;
    // EE manual Table 2-12 distinguishes SQRT.S and RSQRT.S from DIV.S.
    uint32_t m_fpu_sqrt_latency = 0u;
    uint32_t m_fpu_rsqrt_latency = 0u;
    // COP0 architectural register and VU0 VF/VI transfer candidates.
    // These classes are unavailable unless individually supplied and
    // remain nonpairable until hardware resource rules are established.
    uint32_t m_cop0_transfer_latency = 0u;
    uint32_t m_vu0_vf_transfer_latency = 0u;
    uint32_t m_vu0_vi_transfer_latency = 0u;

    // Optional *initiation intervals* (not result latencies); no implicit
    // defaults are permitted. The strict experimental mode rejects every
    // timed functional-unit operation lacking a configured interval.
    uint32_t m_mac_multiply_interval = 0u;
    uint32_t m_mac_divide_interval = 0u;
    uint32_t m_mac_accumulate_interval = 0u;
    uint32_t m_cop1_arithmetic_interval = 0u;
    uint32_t m_cop1_divide_interval = 0u;
    uint32_t m_cop1_sqrt_interval = 0u;
    uint32_t m_cop1_rsqrt_interval = 0u;
    bool m_require_throughput = false;
    // Diagnostic comparison only; never enabled implicitly. Strictly
    // preserve same-register completion order without inventing stalls.
    bool m_allow_ordered_waw_overlap = false;
    // Appended to preserve all existing profile aggregate initializer slots.
    // Packed MMI uses all 128 bits of each participating GPR. A separate
    // caller-provided result latency is mandatory; scalar integer latency
    // is not evidence of MMI pipeline timing. Pairing remains disabled.
    uint32_t m_packed_mmi_latency = 0u;
};

enum class e_ee_issue_replay_status : uint8_t
{
    ISSUED,
    ANNULLED,
    UNCONFIGURED,
    INCOMPLETE_STREAM,
    UNMODELED_INSTRUCTION,
    UNMODELED_HLE_BOUNDARY,
    INVALID_SLOT,
    HALTED
};

// Deterministic offline probe. The input must begin at reference slot 1
// and include every reached instruction and every annulled likely slot.
// This class cannot publish timestamps or install a hardware cycle source.
class c_ee_issue_trace_replay
{
public:
    bool configure(s_ee_issue_replay_profile profile)
    {
        if (profile.m_integer_latency == 0u || profile.m_load_latency == 0u)
        {
            return false;
        }
        m_probe.reset();
        if (!m_probe.configure_waw_policy(
                profile.m_allow_ordered_waw_overlap
                    ? e_ee_probe_waw_policy::ORDERED_OVERLAP
                    : e_ee_probe_waw_policy::WAIT_FOR_RESULT))
        {
            return false;
        }
        m_profile = profile;
        m_configured = true;
        m_failed = false;
        m_next_slot = 1u;
        m_next_attempt = 1u;
        m_pending_likely_slot = false;
        m_delay_pc = 0u;
        m_failure_pc = 0u;
        m_last_event_cycle = 0u;
        return true;
    }

    e_ee_issue_replay_status observe(const s_ee_instruction_trace_entry& entry)
    {
        if (!m_configured)
        {
            return e_ee_issue_replay_status::UNCONFIGURED;
        }
        if (m_failed)
        {
            return e_ee_issue_replay_status::HALTED;
        }
        // A host HLE transition has no issue/retirement cycle cost until
        // it is independently calibrated. It retains the preceding slot.
        if (entry.m_kind == 3u)
        {
            return fail(e_ee_issue_replay_status::UNMODELED_HLE_BOUNDARY,
                        entry.m_pc);
        }
        if (entry.m_hle_service != 0u)
        {
            return fail(e_ee_issue_replay_status::INVALID_SLOT, entry.m_pc);
        }
        if (entry.m_reference_slot != m_next_slot)
        {
            return fail(e_ee_issue_replay_status::INCOMPLETE_STREAM, entry.m_pc);
        }
        if (m_pending_likely_slot && entry.m_pc != m_delay_pc)
        {
            return fail(e_ee_issue_replay_status::INVALID_SLOT, entry.m_pc);
        }

        if (entry.m_kind == 1u)
        {
            if (!m_pending_likely_slot || entry.m_attempt != m_next_attempt - 1u ||
                !m_probe.reserve_annulled_delay_slot())
            {
                return fail(e_ee_issue_replay_status::INVALID_SLOT, entry.m_pc);
            }
            m_pending_likely_slot = false;
            m_last_event_cycle = m_probe.estimated_cycles() - 1u;
            ++m_next_slot;
            return e_ee_issue_replay_status::ANNULLED;
        }

        if ((entry.m_kind != 0u && entry.m_kind != 2u) ||
            entry.m_attempt != m_next_attempt)
        {
            return fail(e_ee_issue_replay_status::INVALID_SLOT, entry.m_pc);
        }

        const s_ee_instruction decoded = decode(entry.m_pc, entry.m_opcode_word);
        s_ee_probe_instruction instruction{};
        if (decoded.valid != e_ee_valid::VALID ||
            !classify(decoded, &instruction))
        {
            return fail(e_ee_issue_replay_status::UNMODELED_INSTRUCTION, entry.m_pc);
        }
        uint64_t issue_cycle = 0u;
        if (!m_probe.issue(instruction, &issue_cycle))
        {
            return fail(e_ee_issue_replay_status::INVALID_SLOT, entry.m_pc);
        }
        m_last_event_cycle = issue_cycle;

        m_pending_likely_slot = decoded.branch_likely;
        m_delay_pc = decoded.pc + 4u;
        ++m_next_slot;
        ++m_next_attempt;
        return e_ee_issue_replay_status::ISSUED;
    }

    bool usable_prefix() const { return m_configured && !m_failed; }
    uint64_t candidate_cycles() const { return m_probe.estimated_cycles(); }
    // Distinct from issue time and NOT hardware retirement.
    uint64_t last_candidate_completion_cycle() const
    {
        return m_probe.last_completion_cycle();
    }
    uint64_t max_candidate_completion_cycle() const
    {
        return m_probe.max_completion_cycle();
    }
    // An experimental per-event issue/reserved-slot time, not an EE clock.
    uint64_t last_event_candidate_cycle() const { return m_last_event_cycle; }
    uint64_t executed_instructions() const { return m_probe.instructions_issued(); }
    uint64_t annulled_slots() const { return m_probe.annulled_slots_reserved(); }
    uint32_t failure_pc() const { return m_failure_pc; }

private:
    static uint32_t mask(uint32_t register_index)
    {
        return register_index == 0u ? 0u : 1u << register_index;
    }

    static uint32_t fpr_mask(uint32_t register_index)
    {
        // Unlike GPR zero, FPR zero is a real writable scoreboard resource.
        return 1u << register_index;
    }

    bool set_unit(s_ee_probe_instruction* instruction,
                  e_ee_probe_unit unit, uint32_t interval) const
    {
        if (interval == 0u)
        {
            return !m_profile.m_require_throughput;
        }
        instruction->m_unit = unit;
        instruction->m_initiation_interval = interval;
        return true;
    }

    bool classify(const s_ee_instruction& decoded,
                  s_ee_probe_instruction* instruction) const
    {
        const uint32_t op = decoded.opcode_word >> 26u;
        const uint32_t rs = mask(decoded.rs);
        const uint32_t rt = mask(decoded.rt);
        const uint32_t rd = mask(decoded.rd);
        instruction->m_result_latency = m_profile.m_integer_latency;
        instruction->m_pipe = e_ee_probe_pipe::INTEGER;

        if (op == 0u)
        {
            switch (decoded.funct)
            {
            case 0x00u: case 0x02u: case 0x03u:
            case 0x38u: case 0x3au: case 0x3bu:
            case 0x3cu: case 0x3eu: case 0x3fu:
                instruction->m_gpr_read_mask = rt;
                instruction->m_gpr_write_mask = rd;
                return true;
            case 0x04u: case 0x06u: case 0x07u:
            case 0x14u: case 0x16u: case 0x17u:
            case 0x0au: case 0x0bu:
            case 0x21u: case 0x23u: case 0x24u:
            case 0x25u: case 0x26u: case 0x27u:
            case 0x2au: case 0x2bu: case 0x2du: case 0x2fu:
                instruction->m_gpr_read_mask = rs | rt;
                instruction->m_gpr_write_mask = rd;
                return true;
            case 0x10u: case 0x12u: // MFHI/MFLO (pipeline 0)
                if (m_profile.m_hilo_transfer_latency == 0u)
                {
                    return false;
                }
                instruction->m_pipe = e_ee_probe_pipe::MULT_DIV;
                instruction->m_hilo_read_mask =
                    decoded.funct == 0x10u ? 0x1u : 0x2u;
                instruction->m_gpr_write_mask = rd;
                instruction->m_result_latency =
                    m_profile.m_hilo_transfer_latency;
                return true;
            case 0x11u: case 0x13u: // MTHI/MTLO (pipeline 0)
                if (m_profile.m_hilo_transfer_latency == 0u)
                {
                    return false;
                }
                instruction->m_pipe = e_ee_probe_pipe::MULT_DIV;
                instruction->m_gpr_read_mask = rs;
                instruction->m_hilo_write_mask =
                    decoded.funct == 0x11u ? 0x1u : 0x2u;
                instruction->m_hilo_result_latency =
                    m_profile.m_hilo_transfer_latency;
                return true;
            case 0x18u: case 0x19u: // MULT/MULTU
            case 0x1au: case 0x1bu: // DIV/DIVU
            {
                const bool multiply = decoded.funct < 0x1au;
                const uint32_t special_latency = multiply
                    ? m_profile.m_hilo_multiply_latency
                    : m_profile.m_hilo_divide_latency;
                if (special_latency == 0u)
                {
                    return false;
                }
                instruction->m_pipe = e_ee_probe_pipe::MULT_DIV;
                instruction->m_gpr_read_mask = rs | rt;
                // EE MULT can also put the low product directly in rd.
                instruction->m_gpr_write_mask = multiply ? rd : 0u;
                // Do not infer an early GPR multiply result from generic ALU
                // timing; conservatively use the explicit multiply category.
                if (multiply)
                {
                    instruction->m_result_latency = special_latency;
                }
                instruction->m_hilo_write_mask = 0x3u;
                instruction->m_hilo_result_latency = special_latency;
                return set_unit(instruction, e_ee_probe_unit::MAC0,
                    multiply ? m_profile.m_mac_multiply_interval
                             : m_profile.m_mac_divide_interval);
            }
            case 0x08u: case 0x09u:
                instruction->m_pipe = e_ee_probe_pipe::BRANCH;
                instruction->m_gpr_read_mask = rs;
                instruction->m_gpr_write_mask =
                    decoded.funct == 0x09u ? rd : 0u;
                return true;
            default:
                return false; // HI/LO, trap, syscall and synchronization
            }
        }

        // MMI HI1/LO1 and multiply/accumulate have independent
        // architectural hazards from the scalar HI0/LO0 pipeline.
        if (op == 0x1cu)
        {
            const uint32_t operation = decoded.funct;
            instruction->m_pipe = e_ee_probe_pipe::MULT_DIV_1;
            if (operation == 0x00u || operation == 0x01u)
            {
                instruction->m_pipe = e_ee_probe_pipe::MULT_DIV;
            }
            switch (operation)
            {
            case 0x30u: // PMFHL: read both 128-bit HI/LO banks
                if (m_profile.m_hilo_transfer_latency == 0u)
                {
                    return false;
                }
                // Full GPR result is a packed transfer; both accumulators
                // supply words/halfwords and must be ready before issue.
                instruction->m_pipe = e_ee_probe_pipe::PACKED_MMI;
                instruction->m_hilo_read_mask = 0x0fu;
                instruction->m_gpr_write_mask = rd;
                instruction->m_gpr_128_write_mask = rd;
                instruction->m_result_latency =
                    m_profile.m_hilo_transfer_latency;
                return true;
            case 0x31u: // PMTHL: read rs, partially update all four halves
                if (m_profile.m_hilo_transfer_latency == 0u)
                {
                    return false;
                }
                // Conservatively reserve all four HI/LO halves; the
                // instruction preserves each upper 32-bit word.
                instruction->m_pipe = e_ee_probe_pipe::PACKED_MMI;
                instruction->m_gpr_read_mask = rs;
                instruction->m_gpr_128_read_mask = rs;
                instruction->m_hilo_write_mask = 0x0fu;
                instruction->m_hilo_result_latency =
                    m_profile.m_hilo_transfer_latency;
                return true;
            case 0x09u: case 0x29u: // MMI2 / MMI3 packed-word HI/LO
                switch (decoded.sa)
                {
                case 0x08u: case 0x09u: // PMFHI/PMFLO, PMTHI/PMTLO
                    if (m_profile.m_hilo_transfer_latency == 0u)
                    {
                        return false;
                    }
                    instruction->m_pipe = e_ee_probe_pipe::PACKED_MMI;
                    if (operation == 0x09u) // PMFHI / PMFLO
                    {
                        instruction->m_hilo_read_mask =
                            decoded.sa == 0x08u ? 0x05u : 0x0au;
                        instruction->m_gpr_write_mask = rd;
                        instruction->m_gpr_128_write_mask = rd;
                        instruction->m_result_latency =
                            m_profile.m_hilo_transfer_latency;
                    }
                    else // PMTHI / PMTLO
                    {
                        instruction->m_gpr_read_mask = rs;
                        instruction->m_gpr_128_read_mask = rs;
                        instruction->m_hilo_write_mask =
                            decoded.sa == 0x08u ? 0x05u : 0x0au;
                        instruction->m_hilo_result_latency =
                            m_profile.m_hilo_transfer_latency;
                    }
                    return true;
                case 0x00u: case 0x04u: // PMADDW, PMSUBW, PMADDUW
                    if (decoded.sa == 0x04u && operation != 0x09u)
                    {
                        return false;
                    }
                    if (m_profile.m_packed_mmi_latency == 0u ||
                        m_profile.m_hilo_accumulate_latency == 0u)
                    {
                        return false;
                    }
                    // Candidate hazards only, not measured PS2 cycles.
                    instruction->m_pipe = e_ee_probe_pipe::PACKED_MMI;
                    instruction->m_gpr_read_mask = rs | rt;
                    instruction->m_gpr_128_read_mask = rs | rt;
                    instruction->m_gpr_write_mask = rd;
                    instruction->m_gpr_128_write_mask = rd;
                    instruction->m_hilo_read_mask = 0x0fu;
                    instruction->m_hilo_write_mask = 0x0fu;
                    instruction->m_hilo_result_latency =
                        m_profile.m_hilo_accumulate_latency;
                    instruction->m_result_latency =
                        m_profile.m_hilo_accumulate_latency;
                    return true;
                case 0x10u: case 0x11u: // PMADDH, PHMADH
                case 0x14u: case 0x15u: // PMSUBH, PHMSBH
                case 0x1cu:             // PMULTH
                {
                    if (operation != 0x09u)
                    {
                        return false;
                    }
                    const bool accumulate =
                        decoded.sa == 0x10u || decoded.sa == 0x14u;
                    const uint32_t latency = accumulate
                        ? m_profile.m_hilo_accumulate_latency
                        : m_profile.m_hilo_multiply_latency;
                    // Only explicit packed and HI/LO profiles may produce
                    // a candidate issue trace. These are not EE cycle values.
                    if (m_profile.m_packed_mmi_latency == 0u ||
                        latency == 0u)
                    {
                        return false;
                    }
                    instruction->m_pipe = e_ee_probe_pipe::PACKED_MMI;
                    instruction->m_gpr_read_mask = rs | rt;
                    instruction->m_gpr_128_read_mask = rs | rt;
                    instruction->m_gpr_write_mask = rd;
                    instruction->m_gpr_128_write_mask = rd;
                    instruction->m_hilo_read_mask = accumulate ? 0x0fu : 0u;
                    instruction->m_hilo_write_mask = 0x0fu;
                    instruction->m_hilo_result_latency = latency;
                    instruction->m_result_latency = latency;
                    return true;
                }
                case 0x0cu: case 0x0du: // PMULTW/UW, PDIVW/UW
                {
                    const bool multiply = decoded.sa == 0x0cu;
                    const uint32_t latency = multiply
                        ? m_profile.m_hilo_multiply_latency
                        : m_profile.m_hilo_divide_latency;
                    // A scalar MAC latency alone is insufficient evidence
                    // for two-wide MMI2/MMI3 issue behaviour.
                    if (m_profile.m_packed_mmi_latency == 0u ||
                        latency == 0u)
                    {
                        return false;
                    }
                    instruction->m_pipe = e_ee_probe_pipe::PACKED_MMI;
                    instruction->m_gpr_read_mask = rs | rt;
                    instruction->m_gpr_128_read_mask = rs | rt;
                    instruction->m_hilo_write_mask = 0x0fu;
                    instruction->m_hilo_result_latency = latency;
                    if (multiply)
                    {
                        instruction->m_gpr_write_mask = rd;
                        instruction->m_gpr_128_write_mask = rd;
                        instruction->m_result_latency = latency;
                    }
                    return true;
                }
                default:
                    // Retain the previously modeled packed MMI2/MMI3
                    // operations instead of aliasing them to MFHI1/MFLO1.
                    if (m_profile.m_packed_mmi_latency == 0u ||
                        (decoded.sa != 0x0eu && decoded.sa != 0x12u &&
                         decoded.sa != 0x13u &&
                         !(operation == 0x29u && decoded.sa == 0x1bu)))
                    {
                        return false;
                    }
                    instruction->m_pipe = e_ee_probe_pipe::PACKED_MMI;
                    instruction->m_result_latency =
                        m_profile.m_packed_mmi_latency;
                    instruction->m_gpr_read_mask = rt |
                        (operation == 0x29u && decoded.sa == 0x1bu
                             ? 0u : rs);
                    instruction->m_gpr_write_mask = rd;
                    instruction->m_gpr_128_read_mask =
                        instruction->m_gpr_read_mask;
                    instruction->m_gpr_128_write_mask = rd;
                    return true;
                }
            case 0x10u: case 0x12u: // MFHI1 / MFLO1
                if (m_profile.m_hilo_transfer_latency == 0u)
                {
                    return false;
                }
                instruction->m_hilo_read_mask =
                    operation == 0x10u ? 0x4u : 0x8u;
                instruction->m_gpr_write_mask = rd;
                instruction->m_result_latency =
                    m_profile.m_hilo_transfer_latency;
                return true;
            case 0x11u: case 0x13u: // MTHI1 / MTLO1
                if (m_profile.m_hilo_transfer_latency == 0u)
                {
                    return false;
                }
                instruction->m_gpr_read_mask = rs;
                instruction->m_hilo_write_mask =
                    operation == 0x11u ? 0x4u : 0x8u;
                instruction->m_hilo_result_latency =
                    m_profile.m_hilo_transfer_latency;
                return true;
            case 0x18u: case 0x19u: // MULT1 / MULTU1
            case 0x1au: case 0x1bu: // DIV1 / DIVU1
            {
                const bool multiply = operation < 0x1au;
                const uint32_t special_latency = multiply
                    ? m_profile.m_hilo_multiply_latency
                    : m_profile.m_hilo_divide_latency;
                if (special_latency == 0u)
                {
                    return false;
                }
                instruction->m_gpr_read_mask = rs | rt;
                instruction->m_gpr_write_mask = multiply ? rd : 0u;
                // Do not infer an early GPR multiply result from generic ALU
                // timing; conservatively use the explicit multiply category.
                if (multiply)
                {
                    instruction->m_result_latency = special_latency;
                }
                instruction->m_hilo_write_mask = 0xcu;
                instruction->m_hilo_result_latency = special_latency;
                return set_unit(instruction, e_ee_probe_unit::MAC1,
                    multiply ? m_profile.m_mac_multiply_interval
                             : m_profile.m_mac_divide_interval);
            }
            case 0x00u: case 0x01u: // MADD / MADDU
            case 0x20u: case 0x21u: // MADD1 / MADDU1
                if (m_profile.m_hilo_accumulate_latency == 0u)
                {
                    return false;
                }
                instruction->m_gpr_read_mask = rs | rt;
                instruction->m_gpr_write_mask = rd;
                instruction->m_hilo_read_mask =
                    operation < 0x20u ? 0x3u : 0xcu;
                instruction->m_hilo_write_mask =
                    instruction->m_hilo_read_mask;
                instruction->m_hilo_result_latency =
                    m_profile.m_hilo_accumulate_latency;
                // MADD rd receives the new LO product/accumulator result.
                // Its availability is not generic one-cycle ALU timing.
                instruction->m_result_latency =
                    m_profile.m_hilo_accumulate_latency;
                return set_unit(instruction,
                    operation < 0x20u ? e_ee_probe_unit::MAC0
                                      : e_ee_probe_unit::MAC1,
                    m_profile.m_mac_accumulate_interval);
            default:
                // The decoder has already rejected unsupported encodings.
                // Only the exact-lowered, 128-bit GPR subset listed below
                // can be replayed with an independently supplied candidate
                // result latency. QFSRV (MMI1 sa=27) also reads the special
                // SA shift register: keep it unavailable until SA producer
                // (MTSAB/MTSAH) and consumer hazards are modeled.
                if (m_profile.m_packed_mmi_latency == 0u)
                {
                    return false;
                }
                {
                    bool unary_rt = false;
                    bool plzcw = false;
                    bool packed_supported = false;
                    switch (operation)
                    {
                    case 0x04u: // PLZCW: reads rs, writes low doubleword of rd
                        plzcw = true;
                        packed_supported = true;
                        break;
                    case 0x34u: case 0x36u: case 0x37u: // PS*H
                    case 0x3cu: case 0x3eu: case 0x3fu: // PS*W
                        unary_rt = true;
                        packed_supported = true;
                        break;
                    case 0x08u: // MMI0: all decoder-accepted packed lanes
                        unary_rt = decoded.sa == 30u || decoded.sa == 31u;
                        packed_supported = decoded.sa <= 10u ||
                            (decoded.sa >= 16u && decoded.sa <= 27u) ||
                            decoded.sa == 30u || decoded.sa == 31u;
                        break;
                    case 0x28u: // MMI1: parallel arithmetic and extensions
                        unary_rt = decoded.sa == 1u || decoded.sa == 5u;
                        switch (decoded.sa)
                        {
                        case 1u: case 2u: case 3u: case 4u:
                        case 5u: case 6u: case 7u: case 10u:
                        case 16u: case 17u: case 18u:
                        case 20u: case 21u: case 22u:
                        case 24u: case 25u: case 26u:
                            packed_supported = true;
                            break;
                        default:
                            break;
                        }
                        break;
                    case 0x09u: // MMI2: PCPYLD/PAND/PXOR
                        packed_supported = decoded.sa == 14u ||
                                           decoded.sa == 18u ||
                                           decoded.sa == 19u;
                        break;
                    case 0x29u: // MMI3: PCPYUD/POR/PNOR/PCPYH
                        unary_rt = decoded.sa == 27u;
                        packed_supported = decoded.sa == 14u ||
                                           decoded.sa == 18u ||
                                           decoded.sa == 19u ||
                                           decoded.sa == 27u;
                        break;
                    default:
                        break;
                    }
                    if (!packed_supported)
                    {
                        return false;
                    }
                    instruction->m_pipe = e_ee_probe_pipe::PACKED_MMI;
                    instruction->m_result_latency =
                        m_profile.m_packed_mmi_latency;
                    instruction->m_gpr_read_mask =
                        plzcw ? rs : (rt | (unary_rt ? 0u : rs));
                    instruction->m_gpr_write_mask = rd;
                    // PLZCW only touches words 0/1 of rd, preserving its
                    // upper 64 bits. Every other accepted packed op here
                    // reads/writes complete 128-bit GPRs.
                    instruction->m_gpr_128_read_mask =
                        plzcw ? 0u : instruction->m_gpr_read_mask;
                    instruction->m_gpr_128_write_mask =
                        plzcw ? 0u : instruction->m_gpr_write_mask;
                    return true;
                }
            }
        }

        if (op == 0x10u) // COP0 MFC0/MTC0, no system transitions
        {
            if (m_profile.m_cop0_transfer_latency == 0u)
            {
                return false;
            }
            instruction->m_pipe = e_ee_probe_pipe::SYSTEM_CONTROL;
            instruction->m_result_latency = m_profile.m_cop0_transfer_latency;
            const uint32_t control = fpr_mask(decoded.rd);
            if (decoded.rs == 0u) // MFC0
            {
                instruction->m_cop0_read_mask = control;
                instruction->m_gpr_write_mask = rt;
                return true;
            }
            if (decoded.rs == 4u) // MTC0
            {
                instruction->m_gpr_read_mask = rt;
                instruction->m_cop0_write_mask = control;
                instruction->m_cop0_result_latency =
                    m_profile.m_cop0_transfer_latency;
                return true;
            }
            // ERET, EI, DI, TLB changes and other COP0 forms require
            // distinct timing and externally observable kernel state.
            return false;
        }

        if (op == 0x12u) // COP2 VU0 VF/VI register transfers only
        {
            const bool vf = decoded.rs == 1u || decoded.rs == 5u;
            const bool vi = decoded.rs == 2u || decoded.rs == 6u;
            const bool read = decoded.rs == 1u || decoded.rs == 2u;
            if ((!vf && !vi) ||
                (vf && m_profile.m_vu0_vf_transfer_latency == 0u) ||
                (vi && m_profile.m_vu0_vi_transfer_latency == 0u))
            {
                // BC2 and VU0 macro FMAC need a validated flag/ACC model.
                return false;
            }
            const uint32_t latency = vf
                ? m_profile.m_vu0_vf_transfer_latency
                : m_profile.m_vu0_vi_transfer_latency;
            const uint32_t vu_register = fpr_mask(decoded.rd);
            instruction->m_pipe = e_ee_probe_pipe::VU0_TRANSFER;
            instruction->m_result_latency = latency;
            if (read) // QMFC2 / CFC2
            {
                instruction->m_gpr_write_mask = rt;
                if (vf)
                {
                    instruction->m_vu0_vf_read_mask = vu_register;
                }
                else
                {
                    instruction->m_vu0_vi_read_mask = vu_register;
                }
            }
            else // QMTC2 / CTC2
            {
                instruction->m_gpr_read_mask = rt;
                if (vf)
                {
                    instruction->m_vu0_vf_write_mask = vu_register;
                    instruction->m_vu0_vf_result_latency = latency;
                }
                else
                {
                    instruction->m_vu0_vi_write_mask = vu_register;
                    instruction->m_vu0_vi_result_latency = latency;
                }
            }
            return true;
        }

        if (op == 0x11u) // COP1 / EE scalar floating point
        {
            const uint32_t fmt = decoded.rs;
            const uint32_t ft = fpr_mask(decoded.rt);
            const uint32_t fs = fpr_mask(decoded.rd);
            const uint32_t fd = fpr_mask(decoded.sa);
            instruction->m_pipe = e_ee_probe_pipe::COPROCESSOR_OP;
            switch (fmt)
            {
            case 0x00u: // MFC1: FPR fs -> GPR rt
                instruction->m_pipe = e_ee_probe_pipe::COPROCESSOR_MOVE;
                if (m_profile.m_fpu_transfer_latency == 0u)
                {
                    return false;
                }
                instruction->m_fpr_read_mask = fs;
                instruction->m_gpr_write_mask = rt;
                instruction->m_result_latency = m_profile.m_fpu_transfer_latency;
                return true;
            case 0x04u: // MTC1: GPR rt -> FPR fs
                instruction->m_pipe = e_ee_probe_pipe::COPROCESSOR_MOVE;
                if (m_profile.m_fpu_transfer_latency == 0u)
                {
                    return false;
                }
                instruction->m_gpr_read_mask = rt;
                instruction->m_fpr_write_mask = fs;
                instruction->m_fpr_result_latency = m_profile.m_fpu_transfer_latency;
                return true;
            case 0x02u: // CFC1: FCR31 -> GPR rt
                instruction->m_pipe = e_ee_probe_pipe::COPROCESSOR_MOVE;
                if (decoded.rd != 31u || m_profile.m_fpu_control_latency == 0u)
                {
                    return false;
                }
                instruction->m_fcsr_read = true;
                instruction->m_gpr_write_mask = rt;
                instruction->m_result_latency = m_profile.m_fpu_control_latency;
                return true;
            case 0x06u: // CTC1: GPR rt -> FCR31
                instruction->m_pipe = e_ee_probe_pipe::COPROCESSOR_MOVE;
                if (decoded.rd != 31u || m_profile.m_fpu_control_latency == 0u)
                {
                    return false;
                }
                instruction->m_gpr_read_mask = rt;
                instruction->m_fcsr_write = true;
                instruction->m_fcsr_result_latency = m_profile.m_fpu_control_latency;
                return true;
            case 0x08u: // BC1F/BC1T/BC1FL/BC1TL
                if (decoded.rt > 3u || m_profile.m_fpu_control_latency == 0u)
                {
                    return false;
                }
                instruction->m_pipe = e_ee_probe_pipe::BRANCH;
                instruction->m_fcsr_read = true;
                return true;
            case 0x10u: // S format
            case 0x14u: // W format: CVT.S.W only
                break;
            default:
                // Other FCR registers and unmodeled COP1 forms fail closed.
                return false;
            }

            if (fmt == 0x14u)
            {
                if (decoded.funct != 0x20u ||
                    m_profile.m_fpu_arithmetic_latency == 0u)
                {
                    return false;
                }
                instruction->m_fpr_read_mask = fs;
                instruction->m_fpr_write_mask = fd;
                instruction->m_fpr_result_latency =
                    m_profile.m_fpu_arithmetic_latency;
                instruction->m_fcsr_write = true;
                instruction->m_fcsr_result_latency =
                    m_profile.m_fpu_arithmetic_latency;
                return set_unit(instruction, e_ee_probe_unit::COP1_SCALAR,
                    m_profile.m_cop1_arithmetic_interval);
            }

            const uint32_t function = decoded.funct;
            if (function == 0x30u || function == 0x32u ||
                function == 0x34u || function == 0x36u)
            {
                if (m_profile.m_fpu_compare_latency == 0u)
                {
                    return false;
                }
                // C.F.S has no register operands; the other comparisons
                // consume fs and ft and write the condition in FCR31.
                instruction->m_fpr_read_mask = function == 0x30u ? 0u : fs | ft;
                instruction->m_fcsr_write = true;
                instruction->m_fcsr_result_latency = m_profile.m_fpu_compare_latency;
                return set_unit(instruction, e_ee_probe_unit::COP1_SCALAR,
                    m_profile.m_cop1_arithmetic_interval);
            }
            const bool fpu_divide = function == 0x03u;
            const bool fpu_sqrt = function == 0x04u;
            const bool fpu_rsqrt = function == 0x16u;
            const bool acc_arithmetic =
                function == 0x18u || function == 0x19u || function == 0x1au ||
                function == 0x1cu || function == 0x1du ||
                function == 0x1eu || function == 0x1fu;
            const bool ordinary_arithmetic =
                function <= 0x02u ||
                (function >= 0x05u && function <= 0x07u) ||
                function == 0x24u || function == 0x28u ||
                function == 0x29u;

            const uint32_t latency = fpu_divide
                ? m_profile.m_fpu_divide_latency
                : fpu_sqrt ? m_profile.m_fpu_sqrt_latency
                : fpu_rsqrt ? m_profile.m_fpu_rsqrt_latency
                : acc_arithmetic ? m_profile.m_fpu_accumulate_latency
                : m_profile.m_fpu_arithmetic_latency;
            if (latency == 0u ||
                (!fpu_divide && !fpu_sqrt && !fpu_rsqrt &&
                 !acc_arithmetic && !ordinary_arithmetic))
            {
                return false;
            }

            const bool only_fs =
                function == 0x04u || function == 0x05u ||
                function == 0x06u || function == 0x07u ||
                function == 0x24u;
            instruction->m_fpr_read_mask = only_fs ? fs : fs | ft;
            const bool writes_acc =
                function == 0x18u || function == 0x19u ||
                function == 0x1au || function == 0x1eu || function == 0x1fu;
            const bool reads_acc =
                function == 0x1cu || function == 0x1du ||
                function == 0x1eu || function == 0x1fu;
            instruction->m_acc_read = reads_acc;
            instruction->m_acc_write = writes_acc;
            instruction->m_fpr_write_mask = writes_acc ? 0u : fd;
            instruction->m_fpr_result_latency = latency;
            instruction->m_acc_result_latency = latency;
            // Arithmetic can update FCR31 exception/status state, which
            // CFC1/CTC1 may subsequently observe or overwrite.
            instruction->m_fcsr_write = true;
            instruction->m_fcsr_result_latency = latency;
            return set_unit(instruction, e_ee_probe_unit::COP1_SCALAR,
                fpu_divide ? m_profile.m_cop1_divide_interval
                : fpu_sqrt ? m_profile.m_cop1_sqrt_interval
                : fpu_rsqrt ? m_profile.m_cop1_rsqrt_interval
                            : m_profile.m_cop1_arithmetic_interval);
        }

        switch (op)
        {
        case 0x02u: case 0x03u:
            instruction->m_pipe = e_ee_probe_pipe::BRANCH;
            instruction->m_gpr_write_mask = op == 0x03u ? mask(31u) : 0u;
            return true;
        case 0x01u:
            if (decoded.rt != 0u && decoded.rt != 1u &&
                decoded.rt != 2u && decoded.rt != 3u &&
                decoded.rt != 16u && decoded.rt != 17u &&
                decoded.rt != 18u && decoded.rt != 19u)
            {
                return false;
            }
            instruction->m_pipe = e_ee_probe_pipe::BRANCH;
            instruction->m_gpr_read_mask = rs;
            instruction->m_gpr_write_mask =
                decoded.rt >= 16u ? mask(31u) : 0u;
            return true;
        case 0x04u: case 0x05u: case 0x14u: case 0x15u:
            instruction->m_pipe = e_ee_probe_pipe::BRANCH;
            instruction->m_gpr_read_mask = rs | rt;
            return true;
        case 0x06u: case 0x07u: case 0x16u: case 0x17u:
            instruction->m_pipe = e_ee_probe_pipe::BRANCH;
            instruction->m_gpr_read_mask = rs;
            return true;
        case 0x09u: case 0x0au: case 0x0bu: case 0x0cu:
        case 0x0du: case 0x0eu: case 0x19u:
            instruction->m_gpr_read_mask = rs;
            instruction->m_gpr_write_mask = rt;
            return true;
        case 0x0fu:
            instruction->m_gpr_write_mask = rt;
            return true;
        case 0x20u: case 0x21u: case 0x23u:
        case 0x24u: case 0x25u: case 0x27u: case 0x37u:
            instruction->m_pipe = e_ee_probe_pipe::LOAD;
            instruction->m_result_latency = m_profile.m_load_latency;
            instruction->m_gpr_read_mask = rs;
            instruction->m_gpr_write_mask = rt;
            return true;
        case 0x28u: case 0x29u: case 0x2bu: case 0x3fu:
            instruction->m_pipe = e_ee_probe_pipe::STORE;
            instruction->m_gpr_read_mask = rs | rt;
            return true;
        default:
            return false; // Unknown pipeline or hazard behaviour
        }
    }

    e_ee_issue_replay_status fail(e_ee_issue_replay_status code, uint32_t pc)
    {
        m_failed = true;
        m_failure_pc = pc;
        return code;
    }

    c_ee_issue_timing_probe m_probe{};
    s_ee_issue_replay_profile m_profile{};
    bool m_configured = false;
    bool m_failed = false;
    bool m_pending_likely_slot = false;
    uint32_t m_delay_pc = 0u;
    uint32_t m_failure_pc = 0u;
    uint64_t m_next_slot = 1u;
    uint64_t m_next_attempt = 1u;
    uint64_t m_last_event_cycle = 0u;
};

} // namespace ee
