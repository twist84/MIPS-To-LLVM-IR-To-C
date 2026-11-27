#pragma once

#include <cstdint>
#include <limits>

namespace ee
{

// Explicitly experimental issue-timing probe. This is NOT a calibrated
// R5900 clock and MUST NOT be connected to GS, INTC, DMA or guest timers.
// Inputs describe a committed guest instruction, not a host dispatch.
enum class e_ee_probe_pipe : uint8_t
{
    INTEGER,
    LOAD,
    STORE,
    BRANCH,
    MULT_DIV,     // MAC0: logical Pipe 0
    MULT_DIV_1,   // MAC1: logical Pipe 1
    COPROCESSOR_MOVE, // COP1 move: logical Pipe 1
    COPROCESSOR_OP,   // COP1 operation: logical Pipe 0
    COPROCESSOR = COPROCESSOR_OP, // compatibility with existing probes
    SYSTEM_CONTROL, // COP0 transfer: single-issue candidate until verified
    VU0_TRANSFER, // COP2 VF/VI transfer: single-issue candidate
    PACKED_MMI // Full-128-bit GPR SIMD; unverified pairing, single-issue
};

// Only a candidate functional-unit occupancy scoreboard. Real hardware
// throughput and resource sharing require independent validation.
enum class e_ee_probe_unit : uint8_t
{
    NONE,
    MAC0,
    MAC1,
    COP1_SCALAR
};

// Counterfactual WAW handling for offline comparisons. ORDERED_OVERLAP
// only permits a second write to issue before the previous write is ready
// when the second result will not overtake it. This is NOT a claim about
// actual EE register forwarding or retirement.
enum class e_ee_probe_waw_policy : uint8_t
{
    WAIT_FOR_RESULT,
    ORDERED_OVERLAP
};

struct s_ee_probe_instruction
{
    uint32_t m_gpr_read_mask = 0u;
    uint32_t m_gpr_write_mask = 0u;
    e_ee_probe_pipe m_pipe = e_ee_probe_pipe::INTEGER;
    // Experimental scoreboard reservation, supplied by a validated caller.
    // A latency of one allows a dependent instruction in the next probe cycle.
    uint32_t m_result_latency = 1u;

    // Resource identity is architectural; result availability is supplied by
    // the caller, never inferred from an unverified latency table.
    // HI/LO mask: bit0=HI0 bit1=LO0 bit2=HI1 bit3=LO1.
    uint32_t m_hilo_read_mask = 0u;
    uint32_t m_hilo_write_mask = 0u;
    uint32_t m_fpr_read_mask = 0u;
    uint32_t m_fpr_write_mask = 0u;
    // COP0 registers are independent of VU0 transfers. In macro mode,
    // VF[n] and VI[n] have distinct VALUES but a shared dependency
    // identity for equal n. VF00 and VI00 do not produce register hazards
    // (VU User's Manual v6.0, section 5.4.2).
    uint32_t m_cop0_read_mask = 0u;
    uint32_t m_cop0_write_mask = 0u;
    uint32_t m_vu0_vf_read_mask = 0u;
    uint32_t m_vu0_vf_write_mask = 0u;
    uint32_t m_vu0_vi_read_mask = 0u;
    uint32_t m_vu0_vi_write_mask = 0u;
    bool m_acc_read = false;
    bool m_acc_write = false;
    // FCR31 status/condition bits share an architectural control register.
    // This is a single conservative resource, not independently timed FCC bits.
    bool m_fcsr_read = false;
    bool m_fcsr_write = false;

    // Zero falls back to m_result_latency. Multiplication, division and
    // FPU operations may produce GPR and special-register results at
    // different times; caller must supply those when modeling such opcodes.
    uint32_t m_hilo_result_latency = 0u;
    uint32_t m_fpr_result_latency = 0u;
    uint32_t m_acc_result_latency = 0u;
    uint32_t m_fcsr_result_latency = 0u;
    uint32_t m_cop0_result_latency = 0u;
    uint32_t m_vu0_vf_result_latency = 0u;
    uint32_t m_vu0_vi_result_latency = 0u;

    // Throughput is distinct from result latency: the next instruction
    // using this unit may not issue until m_initiation_interval cycles
    // after the previous one. NONE requires interval zero; every named
    // unit requires an explicitly configured, nonzero interval.
    e_ee_probe_unit m_unit = e_ee_probe_unit::NONE;
    uint32_t m_initiation_interval = 0u;

    // Explicit full-128-bit operands. These must be subsets of the ordinary
    // GPR masks: the existing whole-register scoreboard already enforces
    // cross-class RAW/WAR/WAW, including scalar <-> packed aliases. Keeping
    // separate coverage masks prevents silently treating MMI as low-64-only.
    uint32_t m_gpr_128_read_mask = 0u;
    uint32_t m_gpr_128_write_mask = 0u;
};

// Disjoint candidate issue-window accounting. Result deadlines beyond the
// last issue are not included. These counts are not hardware stall samples.
struct s_ee_probe_cycle_breakdown
{
    uint64_t m_single_issue = 0u;
    uint64_t m_dual_issue = 0u;
    uint64_t m_annulled = 0u;
    uint64_t m_register_idle = 0u;
    uint64_t m_unit_idle = 0u;
    uint64_t m_hle = 0u;
};

// A conservative, deterministic two-wide issue *candidate* for evaluating
// instruction streams. Pairing checks documented logical pipe routing
// (EE Core User's Manual v6.0, Tables 1-2/1-3) and rejects RAW/WAR/WAW
// register overlap. The manual's Y resource-hazard pairs are not accepted
// until their one-cycle A-stage stall is represented explicitly. The caller supplies
// result latency explicitly; there are no invented hardware latency tables.
// It deliberately cannot change CPU or device state.
class c_ee_issue_timing_probe
{
public:
    void reset()
    {
        m_cycle = 0u;
        m_issued = 0u;
        m_annulled = 0u;
        m_hle_overhead = 0u;
        m_breakdown = {};
        m_waw_policy = e_ee_probe_waw_policy::WAIT_FOR_RESULT;
        m_last_completion_cycle = 0u;
        m_max_completion_cycle = 0u;
        m_pair_count = 0u;
        clear_pair();
        for (uint64_t& ready : m_gpr_ready)
        {
            ready = 0u;
        }
        for (uint64_t& ready : m_hilo_ready)
        {
            ready = 0u;
        }
        for (uint64_t& ready : m_fpr_ready)
        {
            ready = 0u;
        }
        for (uint64_t& ready : m_cop0_ready)
        {
            ready = 0u;
        }
        for (uint64_t& ready : m_vu0_ready)
        {
            ready = 0u;
        }
        m_acc_ready = 0u;
        m_fcsr_ready = 0u;
        for (uint64_t& ready : m_unit_next_issue)
        {
            ready = 0u;
        }
    }

    // The overlap policy can only be selected before observing events.
    // Reset restores conservative WAIT_FOR_RESULT behavior.
    bool configure_waw_policy(e_ee_probe_waw_policy policy)
    {
        if ((policy != e_ee_probe_waw_policy::WAIT_FOR_RESULT &&
             policy != e_ee_probe_waw_policy::ORDERED_OVERLAP) ||
            m_issued != 0u || m_annulled != 0u ||
            m_hle_overhead != 0u)
        {
            return false;
        }
        m_waw_policy = policy;
        return true;
    }

    e_ee_probe_waw_policy waw_policy() const
    {
        return m_waw_policy;
    }

    // Latest candidate result deadline; this is NOT a hardware retirement
    // timestamp. Earlier operations may have later deadlines.
    uint64_t last_completion_cycle() const
    {
        return m_last_completion_cycle;
    }

    uint64_t max_completion_cycle() const
    {
        return m_max_completion_cycle;
    }

    // Returns false without mutation on overflow or invalid input.
    bool issue(const s_ee_probe_instruction& instruction,
               uint64_t* issue_cycle = nullptr,
               uint64_t* completion_cycle = nullptr)
    {
        if (instruction.m_result_latency == 0u ||
            logical_pipes(instruction.m_pipe) == 0u ||
            static_cast<uint32_t>(instruction.m_unit) >
                static_cast<uint32_t>(e_ee_probe_unit::COP1_SCALAR) ||
            (instruction.m_unit == e_ee_probe_unit::NONE) !=
                (instruction.m_initiation_interval == 0u) ||
            ((instruction.m_hilo_read_mask | instruction.m_hilo_write_mask) &
                ~0x0fu) != 0u ||
            (instruction.m_gpr_128_read_mask &
                ~instruction.m_gpr_read_mask) != 0u ||
            (instruction.m_gpr_128_write_mask &
                ~instruction.m_gpr_write_mask) != 0u ||
            m_issued == std::numeric_limits<uint64_t>::max())
        {
            return false;
        }

        const uint32_t reads = instruction.m_gpr_read_mask & ~1u;
        const uint32_t writes = instruction.m_gpr_write_mask & ~1u;
        uint64_t candidate = m_cycle;

        const uint32_t hreads = instruction.m_hilo_read_mask;
        const uint32_t hwrites = instruction.m_hilo_write_mask;
        const uint32_t freads = instruction.m_fpr_read_mask;
        const uint32_t fwrites = instruction.m_fpr_write_mask;
        const uint32_t creads = instruction.m_cop0_read_mask;
        const uint32_t cwrites = instruction.m_cop0_write_mask;
        // Macro-mode dependency detection aliases VF[n] and VI[n] by
        // index; the actual vector and integer values remain independent.
        // Register zero is exempt from these dependencies.
        const uint32_t vfreads = instruction.m_vu0_vf_read_mask & ~1u;
        const uint32_t vfwrites = instruction.m_vu0_vf_write_mask & ~1u;
        const uint32_t vireads = instruction.m_vu0_vi_read_mask & ~1u;
        const uint32_t viwrites = instruction.m_vu0_vi_write_mask & ~1u;
        const uint32_t vu_reads = vfreads | vireads;
        const uint32_t vu_writes = vfwrites | viwrites;
        const bool independent =
            (reads & m_pair_writes) == 0u &&
            (writes & (m_pair_reads | m_pair_writes)) == 0u &&
            (hreads & m_pair_hilo_writes) == 0u &&
            (hwrites & (m_pair_hilo_reads | m_pair_hilo_writes)) == 0u &&
            (freads & m_pair_fpr_writes) == 0u &&
            (fwrites & (m_pair_fpr_reads | m_pair_fpr_writes)) == 0u &&
            (creads & m_pair_cop0_writes) == 0u &&
            (cwrites & (m_pair_cop0_reads | m_pair_cop0_writes)) == 0u &&
            (vu_reads & m_pair_vu0_writes) == 0u &&
            (vu_writes & (m_pair_vu0_reads | m_pair_vu0_writes)) == 0u &&
            (!instruction.m_acc_read || !m_pair_acc_write) &&
            (!instruction.m_acc_write ||
                (!m_pair_acc_read && !m_pair_acc_write)) &&
            (!instruction.m_fcsr_read || !m_pair_fcsr_write) &&
            (!instruction.m_fcsr_write ||
                (!m_pair_fcsr_read && !m_pair_fcsr_write));
        const bool may_pair =
            m_pair_count == 1u && !m_pair_annulled && independent &&
            compatible_pipes(m_pair_pipe, instruction.m_pipe);

        if (m_pair_count != 0u && !may_pair)
        {
            if (candidate == std::numeric_limits<uint64_t>::max())
            {
                return false;
            }
            ++candidate;
        }

        // RAW always waits for a producer. In conservative mode WAW waits
        // too. ORDERED_OVERLAP checks write-completion order separately
        // below and rejects overtaking rather than inventing a stall.
        const bool wait_for_waw =
            m_waw_policy == e_ee_probe_waw_policy::WAIT_FOR_RESULT;
        for (uint32_t reg = 1u; reg < 32u; ++reg)
        {
            if (((reads | (wait_for_waw ? writes : 0u)) &
                 (uint32_t{1} << reg)) != 0u &&
                candidate < m_gpr_ready[reg])
            {
                candidate = m_gpr_ready[reg];
            }
        }

        for (uint32_t reg = 0u; reg < 4u; ++reg)
        {
            if (((hreads | (wait_for_waw ? hwrites : 0u)) & (1u << reg)) != 0u &&
                candidate < m_hilo_ready[reg])
            {
                candidate = m_hilo_ready[reg];
            }
        }
        for (uint32_t reg = 0u; reg < 32u; ++reg)
        {
            if (((freads | (wait_for_waw ? fwrites : 0u)) & (1u << reg)) != 0u &&
                candidate < m_fpr_ready[reg])
            {
                candidate = m_fpr_ready[reg];
            }
        }
        for (uint32_t reg = 0u; reg < 32u; ++reg)
        {
            const uint32_t bit = 1u << reg;
            if (((creads | (wait_for_waw ? cwrites : 0u)) & bit) != 0u &&
                candidate < m_cop0_ready[reg])
            {
                candidate = m_cop0_ready[reg];
            }
            if (((vu_reads | (wait_for_waw ? vu_writes : 0u)) & bit) != 0u &&
                candidate < m_vu0_ready[reg])
            {
                candidate = m_vu0_ready[reg];
            }
        }
        if ((instruction.m_acc_read ||
             (wait_for_waw && instruction.m_acc_write)) &&
            candidate < m_acc_ready)
        {
            candidate = m_acc_ready;
        }
        if ((instruction.m_fcsr_read ||
             (wait_for_waw && instruction.m_fcsr_write)) &&
            candidate < m_fcsr_ready)
        {
            candidate = m_fcsr_ready;
        }

        // Idle time starts AFTER any previously occupied issue cycle.
        // Register readiness is charged first; only additional unit delay
        // is attributed to occupancy, avoiding double-counted stalls.
        const uint64_t next_empty_cycle = m_cycle + (m_pair_count != 0u ? 1u : 0u);
        const uint64_t register_idle = candidate > next_empty_cycle
            ? candidate - next_empty_cycle : 0u;
        const uint64_t after_registers = candidate > next_empty_cycle
            ? candidate : next_empty_cycle;
        const uint32_t unit_index =
            static_cast<uint32_t>(instruction.m_unit);
        if (unit_index != 0u && candidate < m_unit_next_issue[unit_index])
        {
            candidate = m_unit_next_issue[unit_index];
        }

        const uint64_t unit_idle = candidate > after_registers
            ? candidate - after_registers : 0u;

        const uint32_t hilo_latency = instruction.m_hilo_result_latency == 0u
            ? instruction.m_result_latency
            : instruction.m_hilo_result_latency;
        const uint32_t fpr_latency = instruction.m_fpr_result_latency == 0u
            ? instruction.m_result_latency
            : instruction.m_fpr_result_latency;
        const uint32_t acc_latency = instruction.m_acc_result_latency == 0u
            ? instruction.m_result_latency
            : instruction.m_acc_result_latency;
        const uint32_t fcsr_latency = instruction.m_fcsr_result_latency == 0u
            ? instruction.m_result_latency
            : instruction.m_fcsr_result_latency;
        const uint32_t cop0_latency = instruction.m_cop0_result_latency == 0u
            ? instruction.m_result_latency
            : instruction.m_cop0_result_latency;
        const uint32_t vu0_vf_latency =
            instruction.m_vu0_vf_result_latency == 0u
                ? instruction.m_result_latency
                : instruction.m_vu0_vf_result_latency;
        const uint32_t vu0_vi_latency =
            instruction.m_vu0_vi_result_latency == 0u
                ? instruction.m_result_latency
                : instruction.m_vu0_vi_result_latency;

        // A failed issue cannot change the scoreboard. Validate ALL result
        // domains before mutating any state or accepting a new issue.
        const uint64_t max_cycle = std::numeric_limits<uint64_t>::max();
        if (candidate >= max_cycle ||
            instruction.m_result_latency > max_cycle - candidate ||
            (hwrites != 0u && hilo_latency > max_cycle - candidate) ||
            (fwrites != 0u && fpr_latency > max_cycle - candidate) ||
            (instruction.m_acc_write &&
                acc_latency > max_cycle - candidate) ||
            (instruction.m_fcsr_write &&
                fcsr_latency > max_cycle - candidate) ||
            (cwrites != 0u && cop0_latency > max_cycle - candidate) ||
            (vfwrites != 0u && vu0_vf_latency > max_cycle - candidate) ||
            (viwrites != 0u && vu0_vi_latency > max_cycle - candidate) ||
            (unit_index != 0u &&
                instruction.m_initiation_interval > max_cycle - candidate))
        {
            return false;
        }

        // With explicitly selected ordered overlap, a later result must
        // never become visible before or simultaneously with an older write
        // to the same resource. Equal deadlines are ambiguous without a
        // verified same-cycle writeback priority, so reject them too.
        // No artificial retirement delay is introduced.
        if (m_waw_policy == e_ee_probe_waw_policy::ORDERED_OVERLAP)
        {
            for (uint32_t reg = 1u; reg < 32u; ++reg)
            {
                if ((writes & (uint32_t{1} << reg)) != 0u &&
                    candidate + instruction.m_result_latency <=
                        m_gpr_ready[reg])
                {
                    return false;
                }
            }
            for (uint32_t reg = 0u; reg < 4u; ++reg)
            {
                if ((hwrites & (1u << reg)) != 0u &&
                    candidate + hilo_latency <= m_hilo_ready[reg])
                {
                    return false;
                }
            }
            for (uint32_t reg = 0u; reg < 32u; ++reg)
            {
                if ((fwrites & (1u << reg)) != 0u &&
                    candidate + fpr_latency <= m_fpr_ready[reg])
                {
                    return false;
                }
            }
            for (uint32_t reg = 0u; reg < 32u; ++reg)
            {
                const uint32_t bit = 1u << reg;
                if (((cwrites & bit) != 0u &&
                     candidate + cop0_latency <= m_cop0_ready[reg]) ||
                    ((vfwrites & bit) != 0u &&
                     candidate + vu0_vf_latency <= m_vu0_ready[reg]) ||
                    ((viwrites & bit) != 0u &&
                     candidate + vu0_vi_latency <= m_vu0_ready[reg]))
                {
                    return false;
                }
            }
            if (instruction.m_acc_write &&
                candidate + acc_latency <= m_acc_ready)
            {
                return false;
            }
            if (instruction.m_fcsr_write &&
                candidate + fcsr_latency <= m_fcsr_ready)
            {
                return false;
            }
        }

        uint64_t completes_at = candidate + instruction.m_result_latency;
        if (hwrites != 0u && candidate + hilo_latency > completes_at)
        {
            completes_at = candidate + hilo_latency;
        }
        if (fwrites != 0u && candidate + fpr_latency > completes_at)
        {
            completes_at = candidate + fpr_latency;
        }
        if (instruction.m_acc_write && candidate + acc_latency > completes_at)
        {
            completes_at = candidate + acc_latency;
        }
        if (instruction.m_fcsr_write &&
            candidate + fcsr_latency > completes_at)
        {
            completes_at = candidate + fcsr_latency;
        }
        if (cwrites != 0u && candidate + cop0_latency > completes_at)
        {
            completes_at = candidate + cop0_latency;
        }
        if (vfwrites != 0u && candidate + vu0_vf_latency > completes_at)
        {
            completes_at = candidate + vu0_vf_latency;
        }
        if (viwrites != 0u && candidate + vu0_vi_latency > completes_at)
        {
            completes_at = candidate + vu0_vi_latency;
        }

        // Commit statistics only after every issue validity/overflow check.
        m_breakdown.m_register_idle += register_idle;
        m_breakdown.m_unit_idle += unit_idle;
        if (candidate == m_cycle && m_pair_count == 1u)
        {
            --m_breakdown.m_single_issue;
            ++m_breakdown.m_dual_issue;
        }
        else
        {
            ++m_breakdown.m_single_issue;
        }
        if (candidate != m_cycle)
        {
            m_cycle = candidate;
            clear_pair();
        }

        m_pair_reads |= reads;
        m_pair_writes |= writes;
        m_pair_hilo_reads |= hreads;
        m_pair_hilo_writes |= hwrites;
        m_pair_fpr_reads |= freads;
        m_pair_fpr_writes |= fwrites;
        m_pair_cop0_reads |= creads;
        m_pair_cop0_writes |= cwrites;
        m_pair_vu0_reads |= vu_reads;
        m_pair_vu0_writes |= vu_writes;
        m_pair_acc_read |= instruction.m_acc_read;
        m_pair_acc_write |= instruction.m_acc_write;
        m_pair_fcsr_read |= instruction.m_fcsr_read;
        m_pair_fcsr_write |= instruction.m_fcsr_write;
        m_pair_pipe = instruction.m_pipe;
        ++m_pair_count;
        for (uint32_t reg = 1u; reg < 32u; ++reg)
        {
            if ((writes & (uint32_t{1} << reg)) != 0u)
            {
                m_gpr_ready[reg] =
                    candidate + instruction.m_result_latency;
            }
        }
        for (uint32_t reg = 0u; reg < 4u; ++reg)
        {
            if ((hwrites & (1u << reg)) != 0u)
            {
                m_hilo_ready[reg] = candidate + hilo_latency;
            }
        }
        for (uint32_t reg = 0u; reg < 32u; ++reg)
        {
            if ((fwrites & (1u << reg)) != 0u)
            {
                m_fpr_ready[reg] = candidate + fpr_latency;
            }
        }
        for (uint32_t reg = 0u; reg < 32u; ++reg)
        {
            const uint32_t bit = 1u << reg;
            if ((cwrites & bit) != 0u)
            {
                m_cop0_ready[reg] = candidate + cop0_latency;
            }
            if ((vu_writes & bit) != 0u)
            {
                // If one instruction describes both kinds of write, the
                // numbered hazard clears only when both results are ready.
                uint64_t ready = 0u;
                if ((vfwrites & bit) != 0u)
                {
                    ready = candidate + vu0_vf_latency;
                }
                if ((viwrites & bit) != 0u &&
                    candidate + vu0_vi_latency > ready)
                {
                    ready = candidate + vu0_vi_latency;
                }
                m_vu0_ready[reg] = ready;
            }
        }
        if (instruction.m_acc_write)
        {
            m_acc_ready = candidate + acc_latency;
        }
        if (instruction.m_fcsr_write)
        {
            m_fcsr_ready = candidate + fcsr_latency;
        }
        if (unit_index != 0u)
        {
            m_unit_next_issue[unit_index] =
                candidate + instruction.m_initiation_interval;
        }
        ++m_issued;
        m_last_completion_cycle = completes_at;
        if (completes_at > m_max_completion_cycle)
        {
            m_max_completion_cycle = completes_at;
        }
        if (completion_cycle != nullptr)
        {
            *completion_cycle = completes_at;
        }
        if (issue_cycle != nullptr)
        {
            *issue_cycle = candidate;
        }
        return true;
    }

    // Not-taken branch-likely: count a reserved slot without executing its
    // instruction or creating a guest side effect. A previous branch alone
    // already occupied its issue cycle.
    bool reserve_annulled_delay_slot()
    {
        const uint64_t max_cycle = std::numeric_limits<uint64_t>::max();
        if (m_annulled == max_cycle ||
            m_cycle >= max_cycle - 1u)
        {
            return false;
        }
        if (m_pair_count != 0u)
        {
            ++m_cycle;
        }
        clear_pair();
        m_pair_count = 1u;
        m_pair_annulled = true;
        ++m_annulled;
        ++m_breakdown.m_annulled;
        return true;
    }

    // Explicit caller-configured HLE cost, separate from issue slots.
    // For example the reference runtime defaults to 300, but this number
    // has NOT been validated as an R5900 syscall latency.
    bool charge_hle_overhead(uint32_t cycles)
    {
        const uint64_t max_cycle = std::numeric_limits<uint64_t>::max();
        const uint64_t finish = m_cycle + (m_pair_count != 0u ? 1u : 0u);
        if (finish < m_cycle || cycles > max_cycle - finish ||
            cycles > max_cycle - m_hle_overhead)
        {
            return false;
        }
        m_cycle = finish + cycles;
        clear_pair();
        m_hle_overhead += cycles;
        m_breakdown.m_hle += cycles;
        return true;
    }

    uint64_t estimated_cycles() const
    {
        return m_cycle + (m_pair_count != 0u ? 1u : 0u);
    }

    const s_ee_probe_cycle_breakdown& cycle_breakdown() const
    {
        return m_breakdown;
    }

    uint64_t instructions_issued() const
    {
        return m_issued;
    }

    uint64_t annulled_slots_reserved() const
    {
        return m_annulled;
    }

    uint64_t configured_hle_overhead() const
    {
        return m_hle_overhead;
    }

    // A diagnostic boundary check only. It never fires a hardware event.
    bool probe_budget_reached(uint64_t budget) const
    {
        return budget != 0u && estimated_cycles() >= budget;
    }

private:
    // An O pairing in the manual requires one instruction to route to
    // logical Pipe 0 and the other to Pipe 1. This does not account for
    // fetch alignment, cache stalls or unmodeled pipeline occupancy.
    static uint32_t logical_pipes(e_ee_probe_pipe pipe)
    {
        switch (pipe)
        {
        case e_ee_probe_pipe::INTEGER:
        case e_ee_probe_pipe::BRANCH:
            return 0x3u;
        case e_ee_probe_pipe::LOAD:
        case e_ee_probe_pipe::STORE:
        case e_ee_probe_pipe::MULT_DIV_1:
        case e_ee_probe_pipe::COPROCESSOR_MOVE:
            return 0x2u;
        case e_ee_probe_pipe::MULT_DIV:
        case e_ee_probe_pipe::COPROCESSOR_OP:
            return 0x1u;
        case e_ee_probe_pipe::SYSTEM_CONTROL:
        case e_ee_probe_pipe::VU0_TRANSFER:
        case e_ee_probe_pipe::PACKED_MMI:
            return 0x3u; // Valid only as nonpairable conservative classes
        default:
            return 0u;
        }
    }

    static bool compatible_pipes(e_ee_probe_pipe first,
                                 e_ee_probe_pipe second)
    {
        if (first == e_ee_probe_pipe::SYSTEM_CONTROL ||
            second == e_ee_probe_pipe::SYSTEM_CONTROL ||
            first == e_ee_probe_pipe::VU0_TRANSFER ||
            second == e_ee_probe_pipe::VU0_TRANSFER ||
            first == e_ee_probe_pipe::PACKED_MMI ||
            second == e_ee_probe_pipe::PACKED_MMI)
        {
            // Their actual R5900 pairing/resource rules are not calibrated.
            return false;
        }
        if ((first == e_ee_probe_pipe::BRANCH &&
             second == e_ee_probe_pipe::BRANCH) ||
            (first == e_ee_probe_pipe::COPROCESSOR_OP &&
             second == e_ee_probe_pipe::COPROCESSOR_MOVE) ||
            (first == e_ee_probe_pipe::COPROCESSOR_MOVE &&
             second == e_ee_probe_pipe::COPROCESSOR_OP))
        {
            // Branch+branch is X; COP1 operation+move is Y (one A-stage
            // resource stall), not an unqualified same-cycle issue.
            return false;
        }

        const uint32_t a = logical_pipes(first);
        const uint32_t b = logical_pipes(second);
        return ((a & 0x1u) != 0u && (b & 0x2u) != 0u) ||
               ((a & 0x2u) != 0u && (b & 0x1u) != 0u);
    }

    void clear_pair()
    {
        m_pair_count = 0u;
        m_pair_annulled = false;
        m_pair_reads = 0u;
        m_pair_writes = 0u;
        m_pair_hilo_reads = 0u;
        m_pair_hilo_writes = 0u;
        m_pair_fpr_reads = 0u;
        m_pair_fpr_writes = 0u;
        m_pair_cop0_reads = 0u;
        m_pair_cop0_writes = 0u;
        m_pair_vu0_reads = 0u;
        m_pair_vu0_writes = 0u;
        m_pair_acc_read = false;
        m_pair_acc_write = false;
        m_pair_fcsr_read = false;
        m_pair_fcsr_write = false;
        m_pair_pipe = e_ee_probe_pipe::INTEGER;
    }

    uint64_t m_cycle = 0u;
    uint64_t m_issued = 0u;
    uint64_t m_annulled = 0u;
    uint64_t m_hle_overhead = 0u;
    s_ee_probe_cycle_breakdown m_breakdown{};
    e_ee_probe_waw_policy m_waw_policy =
        e_ee_probe_waw_policy::WAIT_FOR_RESULT;
    uint64_t m_last_completion_cycle = 0u;
    uint64_t m_max_completion_cycle = 0u;
    uint64_t m_gpr_ready[32]{};
    uint64_t m_hilo_ready[4]{};
    uint64_t m_fpr_ready[32]{};
    uint64_t m_cop0_ready[32]{};
    uint64_t m_vu0_ready[32]{};
    uint64_t m_acc_ready = 0u;
    uint64_t m_fcsr_ready = 0u;
    uint64_t m_unit_next_issue[4]{};
    uint32_t m_pair_count = 0u;
    bool m_pair_annulled = false;
    uint32_t m_pair_reads = 0u;
    uint32_t m_pair_writes = 0u;
    uint32_t m_pair_hilo_reads = 0u;
    uint32_t m_pair_hilo_writes = 0u;
    uint32_t m_pair_fpr_reads = 0u;
    uint32_t m_pair_fpr_writes = 0u;
    uint32_t m_pair_cop0_reads = 0u;
    uint32_t m_pair_cop0_writes = 0u;
    uint32_t m_pair_vu0_reads = 0u;
    uint32_t m_pair_vu0_writes = 0u;
    bool m_pair_acc_read = false;
    bool m_pair_acc_write = false;
    bool m_pair_fcsr_read = false;
    bool m_pair_fcsr_write = false;
    e_ee_probe_pipe m_pair_pipe = e_ee_probe_pipe::INTEGER;
};

} // namespace ee
