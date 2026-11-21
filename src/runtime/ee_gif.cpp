#include "ee_gif.h"

#include <algorithm>
#include <vector>

namespace ee
{
namespace
{
uint64_t read_le64(const uint8_t* data)
{
    uint64_t word = 0;
    for (uint32_t i = 0; i < 8u; ++i)
    {
        word |= static_cast<uint64_t>(data[i]) << (i * 8u);
    }
    return word;
}

s_ee_gif_event event(
    e_ee_gif_event_type type, uint32_t reg, uint32_t offset,
    uint32_t qwc, uint64_t low, uint64_t high)
{
    s_ee_gif_event out{};
    out.m_type = type;
    out.m_register = reg;
    out.m_byte_offset = offset;
    out.m_qwc = qwc;
    out.m_word0 = low;
    out.m_word1 = high;
    return out;
}
} // namespace

void c_ee_gif_path3::bind_event_sink(
    void* user, t_event_callback callback)
{
    m_sink_user = user;
    m_sink = callback;
}

void c_ee_gif_path3::reset()
{
    m_state = {};
    m_counters = {};
}

const s_ee_gif_path3_state& c_ee_gif_path3::state() const
{
    return m_state;
}

const s_ee_gif_path_counters& c_ee_gif_path3::counters() const
{
    return m_counters;
}

bool c_ee_gif_path3::consume(const uint8_t* data, uint32_t qwc)
{
    // DMA QWC is 16 bits; reject out-of-contract direct callers.
    // Zero-length DMA has no GIF words to parse or forward.
    if (qwc == 0u)
    {
        return true;
    }
    if (data == nullptr || m_sink == nullptr || qwc > 0xffffu)
    {
        ++m_counters.m_rejected_batches;
        return false;
    }

    s_ee_gif_path3_state next = m_state;
    std::vector<s_ee_gif_event> events;
    // One reglist quad can generate two events; the tag may additionally
    // synthesize a PRE/PRIM event. Do not permanently queue events.
    events.reserve(static_cast<size_t>(qwc) * 2u);

    uint32_t i = 0;
    while (i < qwc)
    {
        const uint32_t offset = i * 16u;
        const uint8_t* at = data + static_cast<size_t>(offset);

        if (!next.m_active)
        {
            const uint64_t low = read_le64(at);
            const uint64_t high = read_le64(at + 8u);
            next.m_nloop = static_cast<uint32_t>(low) & 0x7fffu;
            next.m_eop = ((low >> 15u) & 1u) != 0u;
            next.m_flg = static_cast<uint32_t>((low >> 58u) & 3u);
            next.m_nreg = static_cast<uint32_t>((low >> 60u) & 0xfu);
            if (next.m_nreg == 0u)
            {
                next.m_nreg = 16u;
            }
            next.m_regs = high;
            next.m_loop = 0u;
            next.m_reg = 0u;
            events.push_back(event(
                e_ee_gif_event_type::tag, 0u, offset, 1u, low, high
            ));
            // PRE allows the GIF tag's PRIM field to select the GS
            // primitive before its PACKED registers are processed.
            if (next.m_flg == 0u && ((low >> 46u) & 1u) != 0u)
            {
                events.push_back(event(
                    e_ee_gif_event_type::primitive, 0u, offset, 0u,
                    (low >> 47u) & 0x7ffu, 0u
                ));
            }
            next.m_active = next.m_nloop != 0u;
            ++i;
            continue;
        }

        if (next.m_flg == 0u)
        {
            const uint32_t reg = static_cast<uint32_t>(
                (next.m_regs >> (next.m_reg * 4u)) & 0xfu
            );
            events.push_back(event(
                e_ee_gif_event_type::packed, reg, offset, 1u,
                read_le64(at), read_le64(at + 8u)
            ));
            if (++next.m_reg == next.m_nreg)
            {
                next.m_reg = 0u;
                if (++next.m_loop == next.m_nloop)
                {
                    next.m_active = false;
                }
            }
            ++i;
            continue;
        }

        if (next.m_flg == 1u)
        {
            for (uint32_t half = 0; half < 2u && next.m_active; ++half)
            {
                const uint32_t reg = static_cast<uint32_t>(
                    (next.m_regs >> (next.m_reg * 4u)) & 0xfu
                );
                events.push_back(event(
                    e_ee_gif_event_type::reglist, reg,
                    offset + half * 8u, 0u,
                    read_le64(at + half * 8u), 0u
                ));
                if (++next.m_reg == next.m_nreg)
                {
                    next.m_reg = 0u;
                    if (++next.m_loop == next.m_nloop)
                    {
                        next.m_active = false;
                    }
                }
            }
            // If the final reglist item occupied the low half, the high
            // half is padding. The next GIFtag begins at the NEXT qword.
            ++i;
            continue;
        }

        // FLG=2 IMAGE and FLG=3 IMAGE2 both consume NLOOP qwords;
        // NREG/REGS do not affect the image payload length.
        const uint32_t remaining = next.m_nloop - next.m_loop;
        const uint32_t run = std::min(remaining, qwc - i);
        if (run == 0u)
        {
            ++m_counters.m_rejected_batches;
            return false;
        }
        events.push_back(event(
            e_ee_gif_event_type::image, 0u, offset, run, 0u, 0u
        ));
        next.m_loop += run;
        i += run;
        if (next.m_loop == next.m_nloop)
        {
            next.m_active = false;
        }
    }

    // The event sink must treat the event array as one transaction.
    // Failure does NOT advance state or consume GIF/DMA input.
    if (!m_sink(m_sink_user, events.data(), events.size(), data, qwc))
    {
        ++m_counters.m_rejected_batches;
        return false;
    }
    m_state = next;
    ++m_counters.m_accepted_batches;
    m_counters.m_input_qwc += qwc;
    for (const s_ee_gif_event& item : events)
    {
        switch (item.m_type)
        {
        case e_ee_gif_event_type::tag:
            ++m_counters.m_tags;
            break;
        case e_ee_gif_event_type::primitive:
            ++m_counters.m_primitive_events;
            break;
        case e_ee_gif_event_type::packed:
            ++m_counters.m_packed_events;
            break;
        case e_ee_gif_event_type::reglist:
            ++m_counters.m_reglist_events;
            break;
        case e_ee_gif_event_type::image:
            ++m_counters.m_image_events;
            m_counters.m_image_qwc += item.m_qwc;
            break;
        }
    }
    return true;
}

bool c_ee_gif_path3::dma_transfer(
    void* user, uint32_t channel, const uint8_t* data, uint32_t qwc)
{
    if (user == nullptr || channel != 2u)
    {
        return false;
    }
    return static_cast<c_ee_gif_path3*>(user)->consume(data, qwc);
}
} // namespace ee
