#pragma once

#include <cstdint>
#include <limits>
#include <vector>

namespace ee
{
class c_ee_intc;
class c_ee_vsync_flags;
class ee_memory;

// Cycle-driven GS field-start events. No host wall-clock, synthetic IOP
// response or fabricated frame-completion is involved. The caller must
// provide an authoritative, monotonically increasing EE cycle timestamp;
// native dispatch count and translated loop-guard visits are NOT clocks.
class c_ee_scanout_clock
{
public:
    static constexpr uint64_t k_ee_clock_hz = 294912000ull;
    static constexpr uint32_t k_intc_vblank_start = 2u;
    static constexpr uint64_t k_csr_vsint = 1ull << 3;
    static constexpr uint64_t k_csr_field = 1ull << 13;

    // Official SetGsCrt mode 2 (NTSC 60000/1001 fields/sec) or mode 3
    // (PAL 50 fields/sec). Others have no calibrated event schedule yet.
    // A failed request leaves the previous configuration untouched.
    bool configure(uint32_t interlace, uint32_t mode, uint32_t ffmd,
                   uint64_t current_ee_cycle);

    // Runs due GS field-start transitions in timestamp order. This must
    // NEVER be fed a count of host/native dispatches or loop guards.
    // The INTC cause is asserted regardless of INTC_MASK; the controller
    // decides whether to raise the CPU's interrupt pending line.
    bool advance_to(uint64_t ee_cycle, c_ee_intc* intc,
                    c_ee_vsync_flags* flags, ee_memory* memory)
    {
        // Reject invalid calls before touching the edge list or clock.
        if (!m_enabled || ee_cycle < m_last_ee_cycle ||
            ee_cycle > std::numeric_limits<uint64_t>::max() -
                2u * k_ee_clock_hz ||
            intc == nullptr || flags == nullptr || memory == nullptr)
        {
            return false;
        }

        // Between edges, the large-jump check cannot apply.
        // Advance the same timestamp and clear previously delivered edges.
        if (ee_cycle < m_next_field_cycle &&
            (m_next_end_cycle == 0u || ee_cycle < m_next_end_cycle))
        {
            m_delivered_edges.clear();
            m_last_ee_cycle = ee_cycle;
            return true;
        }
        return deliver_due_edges(ee_cycle, intc, flags, memory);
    }

    // CSR.VSINT is sticky, cleared only by the guest write-one-to-clear.
    void acknowledge_csr(uint64_t write_one_to_clear);
    bool enabled() const
    {
        return m_enabled;
    }
    uint64_t next_field_cycle() const
    {
        return m_next_field_cycle;
    }
    // Both start and end are guest-visible edges. Disabled scanout has
    // no deadline. This query never publishes an event.
    uint64_t next_event_cycle() const;
    uint64_t last_ee_cycle() const;
    uint64_t csr() const
    {
        return m_csr;
    }
    uint64_t fields_started() const
    {
        return m_fields_started;
    }
    uint64_t fields_ended() const
    {
        return m_fields_ended;
    }
    // Ordered edges since the most recent guest-cycle advance.
    // 1 = start, 0 = end; the caller emits only these real events.
    const std::vector<uint8_t>& delivered_vblank_edges() const
    {
        return m_delivered_edges;
    }

private:
    void queue_next_field();
    bool deliver_due_edges(uint64_t ee_cycle, c_ee_intc* intc,
                           c_ee_vsync_flags* flags, ee_memory* memory);

    uint64_t m_last_ee_cycle = 0u;
    uint64_t m_next_field_cycle = 0u;
    uint64_t m_field_numerator = 0u;
    uint32_t m_field_denominator = 1u;
    uint32_t m_fraction = 0u;
    uint64_t m_csr = 0u;
    uint64_t m_fields_started = 0u;
    uint64_t m_fields_ended = 0u;
    uint64_t m_next_end_cycle = 0u;
    uint64_t m_vblank_duration = 0u;
    std::vector<uint8_t> m_delivered_edges;
    bool m_enabled = false;
    bool m_interlaced = false;
};
} // namespace ee
