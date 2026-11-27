#include "ee_scanout_clock.h"
#include "ee_intc.h"
#include "ee_vsync_flags.h"
#include "ee_memory.h"

#include <limits>

namespace ee
{
namespace
{
struct s_field_timing
{
    uint32_t m_whole;
    uint32_t m_remainder;
    uint32_t m_denominator;
    uint64_t m_vblank_duration;
};

template <uint32_t rate_numerator, uint32_t rate_denominator, uint32_t scanlines>
    requires (rate_numerator != 0u && rate_denominator != 0u && scanlines != 0u)
consteval s_field_timing make_field_timing()
{
    constexpr uint64_t numerator = c_ee_scanout_clock::k_ee_clock_hz *
        rate_denominator;
    constexpr uint64_t whole = numerator / rate_numerator;
    static_assert(whole > 0u && whole <= UINT32_MAX);
    return {static_cast<uint32_t>(whole),
        static_cast<uint32_t>(numerator % rate_numerator),
        rate_numerator, whole * 44u / scanlines};
}

constexpr auto k_ntsc = make_field_timing<60000u, 1001u, 525u>();
constexpr auto k_pal = make_field_timing<50u, 1u, 625u>();

static_assert(k_ntsc.m_whole == 4920115u && k_ntsc.m_remainder == 12000u);
static_assert(k_ntsc.m_vblank_duration == 4920115ull * 44u / 525u);
static_assert(k_pal.m_whole == 5898240u && k_pal.m_remainder == 0u);
static_assert(k_pal.m_vblank_duration == 5898240ull * 44u / 625u);

constexpr void queue_field(uint64_t& next, uint32_t& fraction,
                           uint32_t whole, uint32_t remainder,
                           uint32_t denominator)
{
    next += whole;
    fraction += remainder;
    if (fraction >= denominator)
    {
        ++next;
        fraction -= denominator;
    }
}

template <uint32_t fields>
consteval bool test_field_phase(s_field_timing timing)
{
    uint64_t cycle = 1000u;
    uint32_t fraction = 0u;
    const uint64_t numerator = static_cast<uint64_t>(timing.m_whole) *
        timing.m_denominator + timing.m_remainder;
    for (uint32_t field = 1u; field <= fields; ++field)
    {
        queue_field(cycle, fraction, timing.m_whole, timing.m_remainder,
                    timing.m_denominator);
        if (cycle != 1000u + numerator * field / timing.m_denominator ||
            fraction != numerator * field % timing.m_denominator)
        {
            return false;
        }
    }
    return true;
}

static_assert(test_field_phase<300u>(k_ntsc));
static_assert(test_field_phase<300u>(k_pal));
} // namespace

void c_ee_scanout_clock::queue_next_field()
{
    queue_field(m_next_field_cycle, m_fraction, m_field_whole,
                m_field_remainder, m_field_denominator);
}

bool c_ee_scanout_clock::configure(
    uint32_t interlace, uint32_t mode, uint32_t ffmd,
    uint64_t current_ee_cycle
)
{
    if (interlace > 1u || ffmd > 1u ||
        (mode != 0x02u && mode != 0x03u) ||
        current_ee_cycle > std::numeric_limits<uint64_t>::max() -
            2u * k_ee_clock_hz)
    {
        return false;
    }

    const auto& timing = mode == 0x02u ? k_ntsc : k_pal;

    m_enabled = true;
    m_interlaced = interlace != 0u;
    m_last_ee_cycle = current_ee_cycle;
    m_next_field_cycle = current_ee_cycle;
    m_field_whole = timing.m_whole;
    m_field_remainder = timing.m_remainder;
    m_field_denominator = timing.m_denominator;
    m_fraction = 0u;
    m_csr = 0u;
    m_fields_started = 0u;
    m_fields_ended = 0u;
    m_next_end_cycle = 0u;
    // Reference timing: VBlank end 22 scanlines after field start.
    // NTSC/PAL each have 525/625 scanlines per two fields.
    m_vblank_duration = timing.m_vblank_duration;
    m_delivered_edges.clear();
    queue_next_field();
    return true;
}

bool c_ee_scanout_clock::reconfigure(
    uint32_t interlace, uint32_t mode, uint32_t ffmd,
    uint64_t current_ee_cycle
)
{
    if (!m_enabled || interlace > 1u || ffmd > 1u ||
        (mode != 0x02u && mode != 0x03u) ||
        current_ee_cycle < m_last_ee_cycle ||
        current_ee_cycle > std::numeric_limits<uint64_t>::max() -
            2u * k_ee_clock_hz)
    {
        return false;
    }

    const auto& timing = mode == 0x02u ? k_ntsc : k_pal;
    m_interlaced = interlace != 0u;
    m_last_ee_cycle = current_ee_cycle;
    m_field_whole = timing.m_whole;
    m_field_remainder = timing.m_remainder;
    m_field_denominator = timing.m_denominator;
    m_fraction = 0u;
    m_next_field_cycle = current_ee_cycle;
    queue_next_field();
    m_vblank_duration = timing.m_vblank_duration;
    if (m_next_end_cycle <= current_ee_cycle)
    {
        m_next_end_cycle = 0u;
    }
    m_delivered_edges.clear();
    return true;
}

bool c_ee_scanout_clock::deliver_due_edges(
    uint64_t ee_cycle, c_ee_intc* intc,
    c_ee_vsync_flags* flags, ee_memory* memory
)
{
    // Reject impractically huge jumps without producing a partial series
    // of VBlank events. This protects against malformed cycle timestamps.
    constexpr uint64_t k_max_fields_per_advance = 1024ull;
    if (ee_cycle >= m_next_field_cycle &&
        (ee_cycle - m_next_field_cycle) /
            m_field_whole >= k_max_fields_per_advance)
    {
        return false;
    }

    m_delivered_edges.clear();
    while (ee_cycle >= m_next_field_cycle ||
           (m_next_end_cycle != 0u &&
            ee_cycle >= m_next_end_cycle))
    {
        if (m_next_end_cycle != 0u &&
            m_next_end_cycle <= m_next_field_cycle &&
            ee_cycle >= m_next_end_cycle)
        {
            ++m_fields_ended;
            m_next_end_cycle = 0u;
            m_delivered_edges.push_back(0u);
            continue;
        }
        const uint64_t field_boundary = m_next_field_cycle;
        uint64_t next_csr = m_csr | k_csr_vsint;
        if (m_interlaced)
        {
            next_csr ^= k_csr_field;
        }
        // Validate/deliver before changing our latch or INTC state. The
        // one-shot registered flag may be null, in which case no RAM write.
        if (!flags->deliver_actual_vsync(memory, next_csr))
        {
            return false;
        }
        if (!intc->raise(k_intc_vblank_start))
        {
            return false;
        }
        m_csr = next_csr;
        ++m_fields_started;
        m_next_end_cycle = field_boundary + m_vblank_duration;
        m_delivered_edges.push_back(1u);
        queue_next_field();
    }
    m_last_ee_cycle = ee_cycle;
    return true;
}

void c_ee_scanout_clock::acknowledge_csr(uint64_t write_one_to_clear)
{
    m_csr &= ~(write_one_to_clear & 0x1full);
}

uint64_t c_ee_scanout_clock::next_event_cycle() const
{
    if (!m_enabled)
    {
        return std::numeric_limits<uint64_t>::max();
    }
    return m_next_end_cycle != 0u && m_next_end_cycle < m_next_field_cycle
        ? m_next_end_cycle : m_next_field_cycle;
}

uint64_t c_ee_scanout_clock::last_ee_cycle() const
{
    return m_last_ee_cycle;
}

} // namespace ee
