#include "ee_gif.h"
#include "ee_runtime_profile.h"
#include "ee_graphics_batch.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace ee
{
namespace
{
void trace_rejection(const char* function, int line)
{
    const char* dump_path = std::getenv("EE_REJECTED_PATH2_DUMP");
    if (dump_path != nullptr && *dump_path != '\0')
    {
        std::fprintf(stderr, "GIF rejection in %s:%d\n", function, line);
    }
}

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
    ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::gif_consume, qwc);
    // A host GIF transaction may aggregate multiple 16-bit DMA tags.
    // Zero-length DMA has no GIF words to parse or forward.
    if (qwc == 0u)
    {
        return true;
    }
    if (data == nullptr || m_sink == nullptr || qwc > k_max_graphics_batch_qwc)
    {
        ++m_counters.m_rejected_batches;
        trace_rejection(__func__, __LINE__);
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
            trace_rejection(__func__, __LINE__);
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
        trace_rejection(__func__, __LINE__);
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
        {
            // Count the actual, GS-accepted IMAGE/IMAGE2 source bytes.
            // Do not count rejected or partially staged guest DMA input.
            // This does not imply the bytes were sampled as textures,
            // transformed into pixels, or selected by scanout.
            if (m_counters.m_image_events == 0u)
            {
                m_counters.m_image_payload_fnv1a64 =
                    0xcbf29ce484222325ull;
            }
            ++m_counters.m_image_events;
            m_counters.m_image_qwc += item.m_qwc;
            const size_t begin = static_cast<size_t>(item.m_byte_offset);
            const size_t end = begin + static_cast<size_t>(item.m_qwc) * 16u;
            for (size_t byte = begin; byte < end; ++byte)
            {
                const uint8_t value = data[byte];
                m_counters.m_image_nonzero_bytes += value != 0u ? 1u : 0u;
                m_counters.m_image_payload_fnv1a64 ^= value;
                m_counters.m_image_payload_fnv1a64 *=
                    0x100000001b3ull;
            }
            break;
        }
        }
    }
    return true;
}

bool c_ee_gif_path3::consume_ordered(
    c_ee_gif_path3& path1, c_ee_gif_path3& path2,
    const s_ee_gif_segment* segments, size_t count)
{
    ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::gif_ordered, count);
    if (&path1 == &path2 || path1.m_sink == nullptr ||
        path1.m_sink != path2.m_sink || path1.m_sink_user != path2.m_sink_user ||
        (count != 0u && segments == nullptr))
    {
        return false;
    }
    uint32_t total_qwc = 0u;
    for (size_t i = 0u; i < count; ++i)
    {
        const auto& segment = segments[i];
        if ((segment.m_path != 1u && segment.m_path != 2u) ||
            segment.m_data == nullptr || segment.m_qwc == 0u ||
            segment.m_qwc > k_max_graphics_batch_qwc - total_qwc)
        {
            return false;
        }
        total_qwc += segment.m_qwc;
    }
    if (total_qwc == 0u)
    {
        return true;
    }

    struct s_staged_events
    {
        std::vector<uint8_t> m_data;
        std::vector<s_ee_gif_event> m_events;
    } staged;
    staged.m_data.reserve(static_cast<size_t>(total_qwc) * 16u);
    const auto collect = [](void* user, const s_ee_gif_event* events,
                            size_t event_count, const uint8_t* data, uint32_t qwc)
    {
        auto& batch = *static_cast<s_staged_events*>(user);
        const uint32_t offset = static_cast<uint32_t>(batch.m_data.size());
        batch.m_data.insert(batch.m_data.end(), data,
            data + static_cast<size_t>(qwc) * 16u);
        for (size_t i = 0u; i < event_count; ++i)
        {
            auto event = events[i];
            event.m_byte_offset += offset;
            batch.m_events.push_back(event);
        }
        return true;
    };

    auto next1 = path1;
    auto next2 = path2;
    next1.bind_event_sink(&staged, collect);
    next2.bind_event_sink(&staged, collect);
    for (size_t i = 0u; i < count; ++i)
    {
        const auto& segment = segments[i];
        auto& next = segment.m_path == 1u ? next1 : next2;
        const auto& other = segment.m_path == 1u ? next2 : next1;
        // A partially received tag cannot be interleaved with a different
        // path in this synchronous model. Real arbitration is a later step.
        if (other.m_state.m_active || !next.consume(segment.m_data, segment.m_qwc))
        {
            return false;
        }
    }
    if (!path1.m_sink(path1.m_sink_user, staged.m_events.data(),
            staged.m_events.size(), staged.m_data.data(), total_qwc))
    {
        ++path1.m_counters.m_rejected_batches;
        ++path2.m_counters.m_rejected_batches;
        return false;
    }
    path1.m_state = next1.m_state;
    path1.m_counters = next1.m_counters;
    path2.m_state = next2.m_state;
    path2.m_counters = next2.m_counters;
    return true;
}

bool c_ee_gif_path3::dma_transfer(
    void* user, uint32_t channel, const uint8_t* data, uint32_t qwc)
{
    if (user == nullptr || channel != 2u)
    {
        trace_rejection(__func__, __LINE__);
        return false;
    }
    return static_cast<c_ee_gif_path3*>(user)->consume(data, qwc);
}
} // namespace ee
