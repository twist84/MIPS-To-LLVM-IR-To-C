#include "ee_dmac.h"
#include "ee_vif1.h"
#include "ee_runtime_profile.h"
#include "ee_graphics_batch.h"

#include <cstring>
#include "ee_exceptions.h"

#include <vector>
#include <set>
#include <tuple>

namespace ee
{
c_ee_dmac::s_gif_channel_state c_ee_dmac::gif_channel_state() const
{
    const s_spr_channel& channel = m_device_channels[2u];
    return {channel.m_chcr, channel.m_madr, channel.m_qwc, channel.m_tadr};
}

c_ee_dmac::s_gif_channel_state c_ee_dmac::vif1_channel_state() const
{
    const s_spr_channel& channel = m_device_channels[1u];
    return {channel.m_chcr, channel.m_madr, channel.m_qwc, channel.m_tadr};
}

uint64_t c_ee_dmac::gif_trace_count() const
{
    return m_gif_trace_count;
}

bool c_ee_dmac::gif_trace_newest(
    uint32_t offset, s_gif_dma_write* result) const
{
    if (result == nullptr || offset >= k_gif_trace_capacity ||
        offset >= m_gif_trace_count)
    {
        return false;
    }

    *result = m_gif_trace[
        (m_gif_trace_count - 1u - offset) % k_gif_trace_capacity];
    return true;
}

c_ee_dmac::e_gif_dma_reject c_ee_dmac::gif_last_reject() const
{
    return m_gif_last_reject;
}

c_ee_dmac::e_vif1_chain_reject
c_ee_dmac::vif1_chain_last_reject() const
{
    return m_vif1_chain_last_reject;
}

void c_ee_dmac::trace_gif_write(
    uint32_t offset, uint32_t value, uint32_t previous)
{
    s_gif_dma_write& event =
        m_gif_trace[m_gif_trace_count % k_gif_trace_capacity];
    event = {};
    event.m_sequence = ++m_gif_trace_count;
    event.m_offset = offset;
    event.m_value = value;
    event.m_previous = previous;
    event.m_chcr = m_device_channels[2u].m_chcr;
    event.m_madr = m_device_channels[2u].m_madr;
    event.m_qwc = m_device_channels[2u].m_qwc;
    event.m_tadr = m_device_channels[2u].m_tadr;
    event.m_reject = m_gif_last_reject;
}

void c_ee_dmac::sync_interrupt_line()
{
    if (m_cpu == nullptr)
    {
        return;
    }

    if (pending() != 0u)
    {
        ee_interrupt_request(m_cpu, INT_DMAC);
    }
    else
    {
        ee_interrupt_clear(m_cpu, INT_DMAC);
    }
}

void c_ee_dmac::bind_cpu(s_ee_cpu_state* cpu)
{
    if (m_cpu != nullptr && m_cpu != cpu)
    {
        ee_interrupt_clear(m_cpu, INT_DMAC);
    }

    m_cpu = cpu;
    sync_interrupt_line();
}

void c_ee_dmac::bind_memory(ee_memory* memory)
{
    m_memory = memory;
}

void c_ee_dmac::bind_sif0_start(
    void* user, t_sif0_start_callback callback)
{
    m_sif0_user = callback != nullptr ? user : nullptr;
    m_sif0_start = m_sif0_user != nullptr ? callback : nullptr;
}

bool c_ee_dmac::begin_sif0_packet(uint32_t tag, uint32_t destination)
{
    s_spr_channel& channel = m_sif0_channel;
    const uint32_t id = (tag >> 28u) & 7u;
    const uint32_t qwc = tag & 0xffffu;
    const uint32_t address = destination & ~15u;
    if (m_memory == nullptr ||
        (channel.m_chcr & k_chcr_str) == 0u ||
        ((channel.m_chcr >> 2u) & 3u) != 1u ||
        channel.m_qwc != 0u ||
        (id != 1u && id != 4u && id != 7u) ||
        address >= 0x02000000u ||
        static_cast<uint64_t>(address) +
            static_cast<uint64_t>(qwc) * 16u > 0x02000000ull ||
        !m_memory->contains(address, static_cast<size_t>(qwc) * 16u))
    {
        return false;
    }

    const bool end = id == 7u ||
        ((channel.m_chcr & 0x80u) != 0u &&
         (tag & 0x80000000u) != 0u);

    // The received tag supplies MADR/QWC. No IOP source data is
    // written yet, so invalid tags cannot mutate EE destination RAM.
    channel.m_chcr = (channel.m_chcr & 0x0000ffffu) |
                      (tag & 0xffff0000u);
    channel.m_madr = address;
    channel.m_qwc = qwc;
    m_sif0_chain_end = end;
    if (qwc == 0u && end)
    {
        channel.m_chcr &= ~k_chcr_str;
        return raise(5u);
    }
    return true;
}

bool c_ee_dmac::commit_sif0_qwords(uint32_t qwc)
{
    s_spr_channel& channel = m_sif0_channel;
    if ((channel.m_chcr & k_chcr_str) == 0u ||
        qwc == 0u || qwc > channel.m_qwc)
    {
        return false;
    }

    channel.m_madr += qwc * 16u;
    channel.m_qwc -= qwc;
    if (channel.m_qwc == 0u && m_sif0_chain_end)
    {
        channel.m_chcr &= ~k_chcr_str;
        return raise(5u);
    }
    return true;
}

void c_ee_dmac::bind_sif1_start(
    void* user, t_sif1_start_callback callback)
{
    m_sif1_user = callback != nullptr ? user : nullptr;
    m_sif1_start = m_sif1_user != nullptr ? callback : nullptr;
}

bool c_ee_dmac::commit_sif1_qwords(uint32_t qwc)
{
    s_spr_channel& channel = m_sif1_channel;
    if ((channel.m_chcr & k_chcr_str) == 0u ||
        qwc == 0u || qwc > channel.m_qwc)
    {
        return false;
    }

    channel.m_madr += qwc * 16u;
    channel.m_qwc -= qwc;
    if (channel.m_qwc == 0u)
    {
        const bool chain = ((channel.m_chcr >> 2u) & 3u) == 1u;
        if (!chain || m_sif1_chain_end)
        {
            channel.m_chcr &= ~k_chcr_str;
            return raise(6u);
        }

        // CNT stores its next tag after the last inline payload
        // qword; NEXT already selected its non-sequential successor.
        if (((channel.m_chcr >> 28u) & 7u) == 1u)
        {
            channel.m_tadr = channel.m_madr;
        }
    }
    return true;
}

bool c_ee_dmac::prepare_sif1_chain_tag()
{
    s_spr_channel& channel = m_sif1_channel;
    const uint32_t chcr = channel.m_chcr;
    if (m_memory == nullptr ||
        (chcr & k_chcr_str) == 0u ||
        ((chcr >> 2u) & 3u) != 1u ||
        channel.m_qwc != 0u || m_sif1_chain_end ||
        (chcr & 0x40u) != 0u || // TTE requires an extra FIFO tag header
        m_sif1_chain_tags >= 4096u)
    {
        return false;
    }

    const uint32_t tag_address = channel.m_tadr;
    if ((tag_address & 15u) != 0u ||
        tag_address >= 0x02000000u ||
        static_cast<uint64_t>(tag_address) + 16u > 0x02000000ull ||
        !m_memory->contains(tag_address, 16u))
    {
        return false;
    }

    const uint64_t tag = m_memory->read64(tag_address);
    const uint32_t low = static_cast<uint32_t>(tag);
    const uint32_t qwc = low & 0xffffu;
    const uint32_t id = (low >> 28u) & 7u;
    const uint32_t address = static_cast<uint32_t>(tag >> 32u);
    const uint64_t after_tag = static_cast<uint64_t>(tag_address) + 16u;
    const uint64_t after_data = after_tag +
        static_cast<uint64_t>(qwc) * 16u;

    uint32_t source = address;
    uint32_t next_tag = channel.m_tadr;
    bool end = false;
    switch (id)
    {
    case 0u: // REFE, referenced payload, then terminate
        next_tag = static_cast<uint32_t>(after_tag);
        end = true;
        break;
    case 1u: // CNT, inline payload, next tag after data
        source = static_cast<uint32_t>(after_tag);
        next_tag = static_cast<uint32_t>(after_data);
        break;
    case 2u: // NEXT, inline payload, tag at ADDR
        source = static_cast<uint32_t>(after_tag);
        next_tag = address;
        break;
    case 3u: // REF, referenced payload
    case 4u: // REFS, referenced payload with later stall control
        next_tag = static_cast<uint32_t>(after_tag);
        break;
    case 7u: // END, inline payload, terminate
        source = static_cast<uint32_t>(after_tag);
        end = true;
        break;
    default:
        // CALL/RET need a verified address-stack contract.
        return false;
    }

    if ((source & 15u) != 0u ||
        source >= 0x02000000u ||
        static_cast<uint64_t>(source) + uint64_t(qwc) * 16u >
            0x02000000ull ||
        !m_memory->contains(source, static_cast<size_t>(qwc) * 16u))
    {
        return false;
    }
    // Commit no register changes or payload until the entire tag and
    // its source span are known valid. TIE+IRQ ends this tag's chain.
    channel.m_chcr = (chcr & 0x0000ffffu) | (low & 0xffff0000u);
    channel.m_madr = source;
    channel.m_tadr = next_tag;
    channel.m_qwc = qwc;
    m_sif1_chain_end = end ||
        ((chcr & 0x80u) != 0u && (low & 0x80000000u) != 0u);
    ++m_sif1_chain_tags;
    if (qwc == 0u && m_sif1_chain_end)
    {
        channel.m_chcr &= ~k_chcr_str;
        return raise(6u);
    }
    return true;
}

void c_ee_dmac::bind_device_transfer(
    void* user, t_device_transfer_callback callback)
{
    m_device_user = user;
    m_device_transfer = callback;
}

void c_ee_dmac::bind_deferred_vif1(c_ee_vif1* vif1)
{
    cancel_deferred_vif1();
    m_deferred_vif1 = vif1;
}

c_ee_dmac::e_deferred_vif1 c_ee_dmac::submit_deferred_vif1(
    const uint8_t* data, uint32_t qwc, const s_spr_channel& next, bool chain)
{
    if (m_deferred_vif1 == nullptr || m_vif1_pending)
    {
        return e_deferred_vif1::rejected;
    }
    using result = c_ee_vif1::e_deferred_result;
    const auto accepted = m_deferred_vif1->begin_deferred(data, qwc);
    if (accepted == result::pending)
    {
        m_vif1_pending_next = next;
        m_vif1_pending_chain = chain;
        m_vif1_pending = true;
        return e_deferred_vif1::pending;
    }
    return accepted == result::completed ? e_deferred_vif1::completed :
        e_deferred_vif1::rejected;
}

c_ee_dmac::e_deferred_vif1 c_ee_dmac::finish_deferred_vif1(
    c_ee_vif1::e_deferred_result accepted)
{
    using result = c_ee_vif1::e_deferred_result;
    m_vif1_pending = false;
    if (accepted == result::completed)
    {
        m_device_channels[1u] = m_vif1_pending_next;
        (void)raise(1u);
        return e_deferred_vif1::completed;
    }
    if (m_vif1_pending_chain)
    {
        m_vif1_chain_last_reject = e_vif1_chain_reject::rejected_by_sink;
    }
    return e_deferred_vif1::rejected;
}

void c_ee_dmac::cancel_deferred_vif1()
{
    if (m_vif1_pending)
    {
        m_deferred_vif1->cancel_deferred();
        m_vif1_pending = false;
    }
    m_vif1_pending_chain = false;
    m_vif1_pending_next = {};
}

void c_ee_dmac::bind_device_download(
    void* user, t_device_download_callback callback)
{
    m_download_user = user;
    m_device_download = callback;
}

void c_ee_dmac::reset()
{
    cancel_deferred_vif1();
    m_status = 0u;
    m_sqwc = 0u;
    m_gif_trace_count = 0u;
    m_gif_last_reject = e_gif_dma_reject::none;
    m_vif1_chain_last_reject = e_vif1_chain_reject::none;
    for (s_gif_dma_write& event : m_gif_trace)
    {
        event = {};
    }
    for (s_spr_channel& channel : m_device_channels)
    {
        channel = {};
    }
    for (s_spr_channel& channel : m_spr_channels)
    {
        channel = {};
    }
    for (s_spr_channel& channel : m_ipu_channels)
    {
        channel = {};
    }
    m_to_ipu_tag_end = false;
    m_sif0_channel = {};
    m_sif0_chain_end = false;
    m_sif1_channel = {};
    m_sif1_chain_end = false;
    m_sif1_chain_tags = 0u;
    sync_interrupt_line();
}

uint32_t c_ee_dmac::ipu_dma_address(uint32_t address) const
{
    // MADR/TADR bit 31 selects scratchpad (SPR) instead of main RAM.
    return (address & 0x80000000u) != 0u
        ? ee_memory::k_scratchpad_base + (address & 0x3ff0u)
        : (address & 0x7ffffff0u);
}

void c_ee_dmac::finish_ipu(uint32_t channel)
{
    m_ipu_channels[channel - 3u].m_chcr &= ~k_chcr_str;
    (void)raise(channel);
}

bool c_ee_dmac::ipu_pull(uint8_t* qword)
{
    s_spr_channel& c = m_ipu_channels[1];
    if (m_memory == nullptr || (c.m_chcr & k_chcr_str) == 0u)
    {
        return false;
    }
    const bool chain = ((c.m_chcr >> 2u) & 3u) == 1u;
    while (c.m_qwc == 0u)
    {
        if (!chain || m_to_ipu_tag_end)
        {
            finish_ipu(4u);
            return false;
        }
        const uint64_t tag = m_memory->read64(ipu_dma_address(c.m_tadr));
        c.m_chcr = (c.m_chcr & 0xffffu) |
            (static_cast<uint32_t>(tag >> 16u) << 16u);
        const uint32_t qwc = static_cast<uint32_t>(tag) & 0xffffu;
        const uint32_t id = static_cast<uint32_t>(tag >> 28u) & 7u;
        const uint32_t addr =
            (static_cast<uint32_t>(tag >> 32u) & 0x7ffffff0u) |
            (static_cast<uint32_t>(tag >> 63u) << 31u);
        const bool irq = ((tag >> 31u) & 1u) != 0u;
        const bool tie = (c.m_chcr & 0x80u) != 0u;
        uint32_t data = c.m_tadr + 16u;
        switch (id)
        {
        case 0u: data = addr; c.m_tadr += 16u; m_to_ipu_tag_end = true; break; // refe
        case 1u: c.m_tadr = data + qwc * 16u; break;                            // cnt
        case 2u: c.m_tadr = addr; break;                                        // next
        case 3u: case 4u: data = addr; c.m_tadr += 16u; break;                  // ref, refs
        default: m_to_ipu_tag_end = true; break;                                // call/ret/end
        }
        if (tie && irq)
        {
            m_to_ipu_tag_end = true;
        }
        c.m_madr = data;
        c.m_qwc = qwc;
        if (qwc == 0u && m_to_ipu_tag_end)
        {
            finish_ipu(4u);
            return false;
        }
    }
    s_ee_u128 quad{};
    m_memory->read128(ipu_dma_address(c.m_madr), &quad);
    std::memcpy(qword, &quad, 16u);
    c.m_madr += 16u;
    --c.m_qwc;
    if (c.m_qwc == 0u && (!chain || m_to_ipu_tag_end))
    {
        finish_ipu(4u);
    }
    return true;
}

bool c_ee_dmac::ipu_push(const uint8_t* qword)
{
    s_spr_channel& c = m_ipu_channels[0];
    if (m_memory == nullptr || (c.m_chcr & k_chcr_str) == 0u || c.m_qwc == 0u)
    {
        return false;
    }
    s_ee_u128 quad{};
    std::memcpy(&quad, qword, 16u);
    m_memory->write128(ipu_dma_address(c.m_madr), &quad);
    c.m_madr += 16u;
    --c.m_qwc;
    if (c.m_qwc == 0u)
    {
        finish_ipu(3u);
    }
    return true;
}

bool c_ee_dmac::raise(uint32_t channel)
{
    if (channel >= k_channel_count)
    {
        return false;
    }

    m_status |= uint32_t{1} << channel;
    sync_interrupt_line();
    return true;
}

bool c_ee_dmac::acknowledge(uint32_t channel)
{
    if (channel >= k_channel_count)
    {
        return false;
    }

    m_status &= ~(uint32_t{1} << channel);
    sync_interrupt_line();
    return true;
}

uint32_t c_ee_dmac::status() const
{
    return m_status;
}

uint32_t c_ee_dmac::pending() const
{
    // Physical DMA channels 0..9: low status bits and high mask bits.
    // This is the same CIS & CIM expression used in ps2-web-recomp.
    return (m_status & 0x03ffu) & ((m_status >> 16u) & 0x03ffu);
}

bool c_ee_dmac::read_spr_channel(uint32_t address, uint32_t* value) const
{
    if (value == nullptr)
    {
        return false;
    }

    const s_spr_channel* channel = nullptr;
    uint32_t offset = 0;
    bool device_channel = false;
    if (address >= k_vif0_base && address < k_vif0_base + 0x100u)
    {
        channel = &m_device_channels[0];
        offset = address - k_vif0_base;
        device_channel = true;
    }
    else if (address >= k_vif1_base && address < k_vif1_base + 0x100u)
    {
        channel = &m_device_channels[1];
        offset = address - k_vif1_base;
        device_channel = true;
    }
    else if (address >= k_gif_base && address < k_gif_base + 0x100u)
    {
        channel = &m_device_channels[2];
        offset = address - k_gif_base;
        device_channel = true;
    }
    else if (address >= k_sif0_base && address < k_sif0_base + 0x100u)
    {
        channel = &m_sif0_channel;
        offset = address - k_sif0_base;
        device_channel = true;
    }
    else if (address >= k_sif1_base && address < k_sif1_base + 0x100u)
    {
        channel = &m_sif1_channel;
        offset = address - k_sif1_base;
        device_channel = true;
    }
    else if (address >= k_from_spr_base && address < k_from_spr_base + 0x100u)
    {
        channel = &m_spr_channels[0];
        offset = address - k_from_spr_base;
    }
    else if (address >= k_to_spr_base && address < k_to_spr_base + 0x100u)
    {
        channel = &m_spr_channels[1];
        offset = address - k_to_spr_base;
    }
    else if (address >= k_from_ipu_base && address < k_from_ipu_base + 0x100u)
    {
        channel = &m_ipu_channels[0];
        offset = address - k_from_ipu_base;
        device_channel = true;
    }
    else if (address >= k_to_ipu_base && address < k_to_ipu_base + 0x100u)
    {
        channel = &m_ipu_channels[1];
        offset = address - k_to_ipu_base;
        device_channel = true;
    }
    else
    {
        return false;
    }

    switch (offset)
    {
    case 0x00u: *value = channel->m_chcr; return true;
    case 0x10u: *value = channel->m_madr; return true;
    case 0x20u: *value = channel->m_qwc; return true;
    case 0x30u: *value = channel->m_tadr; return true;
    case 0x40u: *value = channel->m_asr[0]; return true;
    case 0x50u: *value = channel->m_asr[1]; return true;
    case 0x80u:
    {
        if (device_channel)
        {
            return false;
        }
        *value = channel->m_sadr;
        return true;
    }
    default: return false;
    }
}

bool c_ee_dmac::transfer_device_normal(uint32_t channel_index)
{
    ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::dmac_normal);
    if (channel_index > 2u || m_memory == nullptr)
    {
        return false;
    }

    s_spr_channel& channel = m_device_channels[channel_index];
    // Normal mode is supported for VIF0/VIF1/GIF. VIF1 CHCR.DIR=0
    // is a device-to-RAM image download, not a forward VIF stream.
    if (((channel.m_chcr >> 2u) & 3u) != 0u)
    {
        return false;
    }

    const size_t bytes = static_cast<size_t>(channel.m_qwc) * 16u;
    const uint64_t end = static_cast<uint64_t>(channel.m_madr) + bytes;
    if ((channel.m_madr & 0x0fu) != 0u ||
        channel.m_madr >= 0x02000000u ||
        end > 0x02000000ull ||
        !m_memory->contains(channel.m_madr, bytes))
    {
        return false;
    }

    // A zero-QWC DMA completes without invoking either device.
    if (channel.m_qwc != 0u)
    {
        std::vector<uint8_t> data(bytes);
        const bool reverse_vif1 =
            channel_index == 1u && (channel.m_chcr & 1u) == 0u;
        if (reverse_vif1)
        {
            if (m_device_download == nullptr ||
                !m_device_download(
                    m_download_user, channel_index,
                    data.data(), channel.m_qwc))
            {
                return false;
            }

            // All destination RAM spans were prevalidated. Do not
            // modify EE RAM unless the full device read succeeded.
            for (uint32_t i = 0; i < channel.m_qwc; ++i)
            {
                s_ee_u128 quad{};
                for (uint32_t b = 0u; b < 8u; ++b)
                {
                    quad.m_lo |= static_cast<uint64_t>(
                        data[static_cast<size_t>(i) * 16u + b]
                    ) << (b * 8u);
                    quad.m_hi |= static_cast<uint64_t>(
                        data[static_cast<size_t>(i) * 16u + 8u + b]
                    ) << (b * 8u);
                }
                m_memory->write128(
                    channel.m_madr + i * 16u, &quad
                );
            }
        }
        else
        {
            if (m_device_transfer == nullptr &&
                !(channel_index == 1u && m_deferred_vif1 != nullptr))
            {
                return false;
            }

            // Deliver the COMPLETE transfer as one contiguous
            // little-endian guest stream. A refusing consumer leaves
            // DMA registers and device state unchanged.
            for (uint32_t i = 0; i < channel.m_qwc; ++i)
            {
                s_ee_u128 quad{};
                m_memory->read128(channel.m_madr + i * 16u, &quad);
                for (uint32_t b = 0; b < 8u; ++b)
                {
                    data[static_cast<size_t>(i) * 16u + b] =
                        static_cast<uint8_t>(quad.m_lo >> (b * 8u));
                    data[static_cast<size_t>(i) * 16u + 8u + b] =
                        static_cast<uint8_t>(quad.m_hi >> (b * 8u));
                }
            }
            if (channel_index == 1u && m_deferred_vif1 != nullptr)
            {
                s_spr_channel next = channel;
                next.m_madr += static_cast<uint32_t>(bytes);
                next.m_qwc = 0u;
                next.m_chcr &= ~k_chcr_str;
                const auto submitted = submit_deferred_vif1(
                    data.data(), channel.m_qwc, next, false);
                if (submitted == e_deferred_vif1::pending)
                {
                    return true; // Started, not completed.
                }
                if (submitted != e_deferred_vif1::completed)
                {
                    return false;
                }
            }
            else if (!m_device_transfer(
                    m_device_user, channel_index,
                    data.data(), channel.m_qwc))
            {
                return false;
            }
        }
    }

    channel.m_madr += static_cast<uint32_t>(bytes);
    channel.m_qwc = 0u;
    channel.m_chcr &= ~k_chcr_str;
    return raise(channel_index);
}

// Walk a complete GIF PATH3 source chain before consuming any bytes.
// Supported: REFE, CNT, NEXT, REF and END, including TIE/IRQ termination.
// A DMA address with bit 31 set reads EE scratchpad at its low 14-bit
// offset, matching ps2-web-recomp/runtime/src/dmac.cpp::source().
// REFS stalls and MFIFO remain fail-closed.
// VIF1 source chain, matching the guest-visible tag/transfer ordering in
// ps2-web-recomp:SDBZ/runtime/src/dmac.cpp::run_chain(1).
//
// Host-side prevalidation is transactional: neither a partial tag walk,
// partial payload, nor a refusing VIF consumer may retire STR, advance
// MADR/TADR/ASR, or publish D_STAT channel 1. TTE delivers the high
// 64 tag bits as a qword with its first eight bytes zeroed.
bool c_ee_dmac::transfer_vif1_source_chain()
{
    ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::dmac_vif1_chain);
    using reject = e_vif1_chain_reject;
    m_vif1_chain_last_reject = reject::none;
    s_spr_channel& current = m_device_channels[1u];
    if (m_memory == nullptr ||
        (m_device_transfer == nullptr && m_deferred_vif1 == nullptr))
    {
        m_vif1_chain_last_reject = reject::missing_memory_or_sink;
        return false;
    }
    if ((current.m_chcr & k_chcr_str) == 0u ||
        ((current.m_chcr >> 2u) & 3u) != 1u ||
        (current.m_chcr & 1u) == 0u ||
        current.m_qwc != 0u ||
        ((current.m_chcr >> 4u) & 3u) != 0u)
    {
        m_vif1_chain_last_reject = reject::invalid_start_state;
        return false;
    }

    const auto resolve = [&](uint64_t address, uint32_t* resolved) -> bool
    {
        if (resolved == nullptr || address > UINT32_MAX ||
            (address & 15u) != 0u)
        {
            return false;
        }
        const uint32_t physical = static_cast<uint32_t>(address);
        uint32_t mapped = physical;
        if ((physical & 0x80000000u) != 0u)
        {
            mapped = ee_memory::k_scratchpad_base +
                (physical & static_cast<uint32_t>(
                    ee_memory::k_scratchpad_size - 1u));
        }
        else if (physical >= 0x02000000u)
        {
            return false;
        }
        if (!m_memory->contains(mapped, 16u))
        {
            return false;
        }
        *resolved = mapped;
        return true;
    };

    // Tag count is a host work budget, not an architectural DMA limit.
    // SDBZ's pre-battle chain exceeds 1024 tags. Bound preflight
    // independently of payload size (zero-QWC tags still cost work).
    constexpr uint32_t k_max_tags = 65536u;
    constexpr size_t k_max_packet_qwc = k_max_graphics_batch_qwc;
    s_spr_channel next = current;
    uint64_t tag_address = next.m_tadr;
    const bool tte = (next.m_chcr & 0x40u) != 0u;
    const bool tie = (next.m_chcr & 0x80u) != 0u;
    std::vector<uint8_t> packet;
    // Tags are immutable throughout preflight. A repeated tag address
    // with the same active return stack cannot reach a new terminator.
    // CALL/RET subroutines reused from different return addresses are valid.
    std::set<std::tuple<uint64_t, uint32_t, uint32_t, uint32_t>> visited;
    bool terminal = false;

    for (uint32_t tag_index = 0u;
         tag_index < k_max_tags && !terminal; ++tag_index)
    {
        uint32_t tag_host_address = 0u;
        if (!resolve(tag_address, &tag_host_address))
        {
            m_vif1_chain_last_reject = reject::invalid_tag_address;
            return false;
        }
        const uint32_t depth = (next.m_chcr >> 4u) & 3u;
        const auto key = std::make_tuple(tag_address, depth,
            depth >= 1u ? next.m_asr[0] : 0u,
            depth >= 2u ? next.m_asr[1] : 0u);
        if (!visited.insert(key).second)
        {
            m_vif1_chain_last_reject = reject::cyclic_chain;
            return false;
        }
        s_ee_u128 tag{};
        m_memory->read128(tag_host_address, &tag);
        const uint64_t low = tag.m_lo;
        const uint32_t qwc = static_cast<uint32_t>(low & 0xffffu);
        const uint32_t id = static_cast<uint32_t>((low >> 28u) & 7u);
        const bool tag_irq = (low & 0x80000000ull) != 0u;
        // DMA tag ADDR occupies bits 32..63 of the FIRST 64-bit word.
        // The second 64-bit word contains TTE VIF command payload, not ADDR.
        // Preserve bit 63 (SPR selector) and clear the low alignment nibble.
        // ps2-web-recomp:SDBZ runtime/src/dmac.cpp::run_chain().
        const uint64_t external = (low >> 32u) & 0xfffffff0ull;
        uint64_t data_address = tag_address + 16u;
        uint64_t next_tag_address = data_address +
            static_cast<uint64_t>(qwc) * 16u;
        bool tag_terminal = false;
        uint32_t asp = (next.m_chcr >> 4u) & 3u;

        switch (id)
        {
        case 0u: // REFE: external payload, end
            tag_terminal = true;
            [[fallthrough]];
        case 3u: // REF
            data_address = external;
            next_tag_address = tag_address + 16u;
            break;
        case 1u: // CNT: inline payload
            break;
        case 2u: // NEXT: inline payload then jump
            next_tag_address = external;
            break;
        case 4u: // REFS: STADR stall control is not modeled
            m_vif1_chain_last_reject = reject::unsupported_tag;
            return false;
        case 5u: // CALL: save the post-payload return tag address
            if (asp >= 2u)
            {
                m_vif1_chain_last_reject = reject::unsupported_tag;
                return false;
            }
            next.m_asr[asp] = static_cast<uint32_t>(next_tag_address);
            ++asp;
            next.m_chcr = (next.m_chcr & ~0x30u) | (asp << 4u);
            next_tag_address = external;
            break;
        case 6u: // RET: return to saved tag address
            if (asp == 0u)
            {
                tag_terminal = true;
            }
            else
            {
                --asp;
                next_tag_address = next.m_asr[asp];
                next.m_chcr = (next.m_chcr & ~0x30u) | (asp << 4u);
            }
            break;
        case 7u: // END
            tag_terminal = true;
            break;
        default:
            m_vif1_chain_last_reject = reject::unsupported_tag;
            return false;
        }

        // A TTE tag sends one additional VIF quadword before its payload.
        const size_t extra = tte ? 1u : 0u;
        if (packet.size() / 16u > k_max_packet_qwc ||
            static_cast<size_t>(qwc) + extra >
                k_max_packet_qwc - packet.size() / 16u)
        {
            m_vif1_chain_last_reject = reject::chain_qwc_limit;
            return false;
        }

        // The packet is private until the entire chain passes preflight.
        // Validate the address domain before borrowing contiguous backing
        // spans; a nonempty payload cannot cross the 32-bit address limit
        // or the physical 32 MiB RAM aperture.
        const uint64_t payload_bytes = static_cast<uint64_t>(qwc) * 16u;
        const uint64_t payload_end = data_address + payload_bytes;
        if (qwc != 0u &&
            ((data_address & 15u) != 0u ||
             payload_end > 0x100000000ull ||
             ((data_address & 0x80000000ull) == 0u &&
              payload_end > 0x02000000ull)))
        {
            m_vif1_chain_last_reject = reject::invalid_payload_span;
            return false;
        }
        if (tte)
        {
            const size_t offset = packet.size();
            packet.resize(offset + 16u, 0u);
            for (uint32_t b = 0u; b < 8u; ++b)
            {
                packet[offset + 8u + b] =
                    static_cast<uint8_t>(tag.m_hi >> (b * 8u));
            }
        }
        // RAM payloads are contiguous. SPR addresses wrap every 16 KiB,
        // so split only at scratchpad boundaries. The backing bytes are
        // already in guest little-endian order; no per-lane decoding is
        // required. Failed spans discard the entire private packet.
        for (uint64_t address = data_address; address < payload_end;)
        {
            uint32_t mapped = 0u;
            if (!resolve(address, &mapped))
            {
                m_vif1_chain_last_reject = reject::invalid_payload_span;
                return false;
            }
            size_t span = static_cast<size_t>(payload_end - address);
            if ((address & 0x80000000ull) != 0u)
            {
                const size_t to_wrap = ee_memory::k_scratchpad_size -
                    (mapped - ee_memory::k_scratchpad_base);
                if (span > to_wrap)
                {
                    span = to_wrap;
                }
            }
            const uint8_t* bytes = m_memory->read_span(mapped, span);
            if (bytes == nullptr)
            {
                m_vif1_chain_last_reject = reject::invalid_payload_span;
                return false;
            }
            packet.insert(packet.end(), bytes, bytes + span);
            address += span;
        }

        // Tag upper half is reflected into CHCR after processing.
        next.m_chcr = (next.m_chcr & 0x0000ffffu) |
            (static_cast<uint32_t>(low) & 0xffff0000u);
        next.m_madr = static_cast<uint32_t>(
            data_address + static_cast<uint64_t>(qwc) * 16u);
        next.m_tadr = static_cast<uint32_t>(next_tag_address);
        tag_address = next_tag_address;
        terminal = tag_terminal || (tie && tag_irq);
    }
    if (!terminal)
    {
        m_vif1_chain_last_reject = reject::chain_tag_limit;
        return false;
    }
    next.m_qwc = 0u;
    next.m_chcr &= ~k_chcr_str;
    if (!packet.empty())
    {
        if (m_deferred_vif1 != nullptr)
        {
            const auto submitted = submit_deferred_vif1(packet.data(),
                static_cast<uint32_t>(packet.size() / 16u), next, true);
            if (submitted == e_deferred_vif1::pending)
            {
                return true; // Started, not completed.
            }
            if (submitted != e_deferred_vif1::completed)
            {
                m_vif1_chain_last_reject = reject::rejected_by_sink;
                return false;
            }
        }
        else if (!m_device_transfer(m_device_user, 1u, packet.data(),
            static_cast<uint32_t>(packet.size() / 16u)))
        {
            m_vif1_chain_last_reject = reject::rejected_by_sink;
            return false;
        }
    }

    m_device_channels[1u] = next;
    return raise(1u);
}

bool c_ee_dmac::transfer_gif_source_chain()
{
    ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::dmac_gif_chain);
    s_spr_channel& channel = m_device_channels[2u];
    m_gif_last_reject = e_gif_dma_reject::none;
    if (m_memory == nullptr || m_device_transfer == nullptr)
    {
        m_gif_last_reject = e_gif_dma_reject::missing_memory_or_sink;
        return false;
    }
    if ((channel.m_chcr & k_chcr_str) == 0u ||
        ((channel.m_chcr >> 2u) & 3u) != 1u ||
        (channel.m_chcr & 1u) == 0u ||
        (channel.m_chcr & 0x40u) != 0u ||
        channel.m_qwc != 0u)
    {
        m_gif_last_reject = e_gif_dma_reject::invalid_start_state;
        return false;
    }

    // This resolves one physical DMA quadword, not an EE CPU virtual
    // address. The high selector bit means scratchpad and its low
    // 14 bits wrap within 16 KiB. RAM sources never silently wrap.
    const auto source_qword =
        [&](uint64_t dma_address, uint32_t* host_address) -> bool
    {
        if (host_address == nullptr || dma_address > 0xffffffffull ||
            (dma_address & 15u) != 0u)
        {
            return false;
        }

        const uint32_t address = static_cast<uint32_t>(dma_address);
        uint32_t mapped = 0u;
        if ((address & 0x80000000u) != 0u)
        {
            const uint32_t offset = address &
                static_cast<uint32_t>(ee_memory::k_scratchpad_size - 1u);
            mapped = ee_memory::k_scratchpad_base + offset;
        }
        else
        {
            if (address >= 0x02000000u ||
                static_cast<uint64_t>(address) + 16u > 0x02000000ull)
            {
                return false;
            }
            mapped = address;
        }

        if (!m_memory->contains(mapped, 16u))
        {
            return false;
        }
        *host_address = mapped;
        return true;
    };

    constexpr uint32_t k_max_chain_tags = 1024u;
    constexpr size_t k_max_chain_qwc = 0xffffu;
    uint64_t tag_address = channel.m_tadr;
    uint32_t final_tag = 0u;
    uint32_t final_madr = channel.m_madr;
    std::vector<uint8_t> packet;
    bool completed = false;

    for (uint32_t tag_index = 0u;
         tag_index < k_max_chain_tags && !completed; ++tag_index)
    {
        uint32_t tag_host_address = 0u;
        if (!source_qword(tag_address, &tag_host_address))
        {
            m_gif_last_reject = e_gif_dma_reject::invalid_tag_address;
            return false;
        }

        const uint64_t tag = m_memory->read64(tag_host_address);
        const uint32_t tag_id = static_cast<uint32_t>(
            (tag >> 28u) & 7u);
        const uint32_t qwc = static_cast<uint32_t>(tag & 0xffffu);
        // The tag's ADDR low nibble is not part of its DMA address.
        const uint64_t tag_data_address =
            static_cast<uint32_t>(tag >> 32u) & ~uint64_t{15};
        const uint64_t inline_data_address = tag_address + 16u;
        uint64_t data_address = inline_data_address;
        uint64_t next_tag_address = inline_data_address +
            static_cast<uint64_t>(qwc) * 16u;
        bool terminal = false;

        switch (tag_id)
        {
        case 0u: // REFE: external payload, terminate
            terminal = true;
            [[fallthrough]];
        case 3u: // REF: external payload, continue sequentially
            data_address = tag_data_address;
            next_tag_address = inline_data_address;
            break;
        case 1u: // CNT: inline payload, sequentially
            break;
        case 2u: // NEXT: inline payload, jump to ADDR
            next_tag_address = tag_data_address;
            break;
        case 7u: // END: inline payload, terminate
            terminal = true;
            break;
        default:
            // REFS, CALL and RET require additional architectural state.
            m_gif_last_reject = e_gif_dma_reject::unsupported_tag;
            return false;
        }

        if (packet.size() / 16u > k_max_chain_qwc ||
            static_cast<size_t>(qwc) >
                k_max_chain_qwc - packet.size() / 16u)
        {
            m_gif_last_reject = e_gif_dma_reject::chain_qwc_limit;
            return false;
        }

        // Prevalidate every source quadword before the sink sees any
        // bytes. Scratchpad may wrap at 0x3ff0; RAM must stay in range.
        std::vector<uint32_t> source_addresses(qwc);
        for (uint32_t i = 0u; i < qwc; ++i)
        {
            if (!source_qword(
                    data_address + static_cast<uint64_t>(i) * 16u,
                    &source_addresses[i]))
            {
                m_gif_last_reject = e_gif_dma_reject::invalid_payload_span;
                return false;
            }
        }

        const size_t previous_size = packet.size();
        packet.resize(previous_size + static_cast<size_t>(qwc) * 16u);
        for (uint32_t i = 0u; i < qwc; ++i)
        {
            s_ee_u128 quad{};
            m_memory->read128(source_addresses[i], &quad);
            for (uint32_t lane = 0u; lane < 8u; ++lane)
            {
                packet[previous_size + static_cast<size_t>(i) * 16u + lane] =
                    static_cast<uint8_t>(quad.m_lo >> (lane * 8u));
                packet[previous_size + static_cast<size_t>(i) * 16u + 8u + lane] =
                    static_cast<uint8_t>(quad.m_hi >> (lane * 8u));
            }
        }

        final_tag = static_cast<uint32_t>(tag);
        final_madr = static_cast<uint32_t>(
            data_address + static_cast<uint64_t>(qwc) * 16u);
        tag_address = next_tag_address;
        completed = terminal ||
            ((channel.m_chcr & 0x80u) != 0u &&
             (tag & 0x80000000ull) != 0u);
    }

    if (!completed)
    {
        m_gif_last_reject = e_gif_dma_reject::chain_tag_limit;
        return false;
    }
    if (!packet.empty() && !m_device_transfer(
            m_device_user, 2u, packet.data(),
            static_cast<uint32_t>(packet.size() / 16u)))
    {
        m_gif_last_reject = e_gif_dma_reject::rejected_by_sink;
        return false;
    }

    channel.m_chcr = ((channel.m_chcr & 0x0000ffffu) |
                       (final_tag & 0xffff0000u)) & ~k_chcr_str;
    channel.m_madr = final_madr;
    channel.m_tadr = static_cast<uint32_t>(tag_address);
    channel.m_qwc = 0u;
    return raise(2u);
}

bool c_ee_dmac::transfer_spr_normal(uint32_t channel_index)
{
    if (m_memory == nullptr || channel_index < 8u || channel_index > 9u)
    {
        return false;
    }

    s_spr_channel& channel = m_spr_channels[channel_index - 8u];
    // ps2-web-recomp falls back to contiguous normal DMA for MOD=2
    // when D_SQWC.TQWC is zero. The modulo operation must never
    // be executed with a zero transfer period.
    const uint32_t mode = (channel.m_chcr >> 2u) & 3u;
    if (mode != 0u && !(mode == 2u && ((m_sqwc >> 16u) & 0xffu) == 0u))
    {
        return false;
    }

    const size_t bytes = static_cast<size_t>(channel.m_qwc) * 16u;
    const uint64_t end = static_cast<uint64_t>(channel.m_madr) + bytes;

    // DMA addresses here are *physical main RAM*, not guest KSEG aliases
    // or a disguised pointer into SPR/MMIO. Validate the entire transfer
    // before any write to avoid partially completing a bad channel.
    if ((channel.m_madr & 0x0fu) != 0u ||
        channel.m_madr >= 0x02000000u ||
        end > 0x02000000ull ||
        !m_memory->contains(channel.m_madr, bytes))
    {
        return false;
    }

    for (uint32_t i = 0; i < channel.m_qwc; ++i)
    {
        const uint32_t ram_address = channel.m_madr + i * 16u;
        const uint32_t spr_offset = (channel.m_sadr + i * 16u) & 0x3ff0u;
        const uint32_t spr_address = ee_memory::k_scratchpad_base + spr_offset;
        s_ee_u128 quad{};

        if (channel_index == 8u) // fromSPR: SPR -> EE RAM
        {
            m_memory->read128(spr_address, &quad);
            m_memory->write128(ram_address, &quad);
        }
        else // toSPR: EE RAM -> SPR
        {
            m_memory->read128(ram_address, &quad);
            m_memory->write128(spr_address, &quad);
        }
    }

    channel.m_madr += static_cast<uint32_t>(bytes);
    channel.m_sadr = (channel.m_sadr + static_cast<uint32_t>(bytes)) & 0x3ff0u;
    channel.m_qwc = 0u;
    channel.m_chcr &= ~k_chcr_str;
    // The existing D_STAT model latches completion even if masked, and
    // asserts COP0 IP3 only when the matching mask bit is enabled.
    return raise(channel_index);
}

bool c_ee_dmac::transfer_spr_interleave(uint32_t channel_index)
{
    if (m_memory == nullptr || channel_index < 8u || channel_index > 9u)
    {
        return false;
    }

    s_spr_channel& channel = m_spr_channels[channel_index - 8u];
    const uint32_t transfer = (m_sqwc >> 16u) & 0xffu;
    const uint32_t skip = m_sqwc & 0xffu;

    if (((channel.m_chcr >> 2u) & 3u) != 2u || transfer == 0u)
    {
        return false;
    }

    // Match the reference's per-QWC D_SQWC pattern: one RAM qword at
    // MADR, then advance by 16 bytes; after each group of TQWC words,
    // also skip SQWC words in RAM. SADR always advances contiguously
    // through SPR, wrapping at the 16 KiB boundary.
    //
    // Preflight EVERY RAM qword before writing anything: a contiguous
    // bounds check is insufficient because the skipped RAM holes may
    // carry a later transfer beyond the allocated EE memory.
    uint64_t address = channel.m_madr;
    for (uint32_t i = 0; i < channel.m_qwc; ++i)
    {
        if ((address & 0x0fu) != 0u ||
            address >= 0x02000000ull ||
            address + 16u > 0x02000000ull ||
            !m_memory->contains(static_cast<uint32_t>(address), 16u))
        {
            return false;
        }

        address += 16u;
        if ((i + 1u) % transfer == 0u)
        {
            address += static_cast<uint64_t>(skip) * 16u;
        }
    }

    // The validated RAM addresses and SPR qwords are independent.
    // Copy only after the complete interleave pattern has passed.
    uint64_t current = channel.m_madr;
    for (uint32_t i = 0; i < channel.m_qwc; ++i)
    {
        const uint32_t ram_address = static_cast<uint32_t>(current);
        const uint32_t spr_address = ee_memory::k_scratchpad_base +
            ((channel.m_sadr + i * 16u) & 0x3ff0u);
        s_ee_u128 quad{};

        if (channel_index == 8u)
        {
            m_memory->read128(spr_address, &quad);
            m_memory->write128(ram_address, &quad);
        }
        else
        {
            m_memory->read128(ram_address, &quad);
            m_memory->write128(spr_address, &quad);
        }

        current += 16u;
        if ((i + 1u) % transfer == 0u)
        {
            current += static_cast<uint64_t>(skip) * 16u;
        }
    }

    channel.m_madr = static_cast<uint32_t>(current);
    channel.m_sadr = (channel.m_sadr + channel.m_qwc * 16u) & 0x3ff0u;
    channel.m_qwc = 0u;
    channel.m_chcr &= ~k_chcr_str;
    return raise(channel_index);
}

bool c_ee_dmac::transfer_from_spr_span(
    s_spr_channel& channel, uint32_t qwc, bool copy_data)
{
    // DMA MADR is a physical EE address. Match the normal-mode policy:
    // reject unbacked, unaligned, or non-RAM spans without any writes.
    const uint64_t bytes = static_cast<uint64_t>(qwc) * 16u;
    const uint64_t end = static_cast<uint64_t>(channel.m_madr) + bytes;

    if (qwc != 0u &&
        ((channel.m_madr & 0x0fu) != 0u ||
         channel.m_madr >= 0x02000000u ||
         end > 0x02000000ull ||
         !m_memory->contains(channel.m_madr, static_cast<size_t>(bytes))))
    {
        return false;
    }

    if (copy_data)
    {
        for (uint32_t i = 0; i < qwc; ++i)
        {
            const uint32_t spr_address =
                ee_memory::k_scratchpad_base +
                ((channel.m_sadr + i * 16u) & 0x3ff0u);
            const uint32_t ram_address = channel.m_madr + i * 16u;
            s_ee_u128 quad{};
            m_memory->read128(spr_address, &quad);
            m_memory->write128(ram_address, &quad);
        }
    }

    channel.m_sadr =
        (channel.m_sadr + static_cast<uint32_t>(bytes)) & 0x3ff0u;
    channel.m_madr += static_cast<uint32_t>(bytes);
    channel.m_qwc = 0u;
    return true;
}

bool c_ee_dmac::walk_from_spr_chain(
    s_spr_channel& channel, bool copy_data)
{
    if (m_memory == nullptr ||
        ((channel.m_chcr >> 2u) & 3u) != 1u)
    {
        return false;
    }

    // ps2-web-recomp runs a preloaded QWC segment first, if present,
    // using the existing CHCR tag ID/IRQ bits to determine termination.
    const bool tie = (channel.m_chcr & 0x80u) != 0u;
    bool done = false;
    if (channel.m_qwc != 0u)
    {
        done = ((channel.m_chcr >> 28u) & 7u) == 7u ||
            (tie && (channel.m_chcr & 0x80000000u) != 0u);
        if (!transfer_from_spr_span(
                channel, channel.m_qwc, copy_data))
        {
            return false;
        }
    }

    // The reference has a very large loop guard. This conservative
    // implementation caps the number of tags at 4096 and refuses to
    // complete a nonterminating chain. A preflight pass guarantees
    // invalid later tags cannot cause partial writes to EE RAM.
    constexpr uint32_t k_max_chain_tags = 4096u;
    for (uint32_t i = 0; !done && i < k_max_chain_tags; ++i)
    {
        const uint32_t tag_address =
            ee_memory::k_scratchpad_base + (channel.m_sadr & 0x3ff0u);
        const uint64_t tag = m_memory->read64(tag_address);
        channel.m_sadr = (channel.m_sadr + 16u) & 0x3ff0u;

        // DMA tag bits 16..31 update the CHCR upper half; the high
        // 32-bit tag word sets the RAM destination address.
        channel.m_chcr =
            (channel.m_chcr & 0x0000ffffu) |
            (static_cast<uint32_t>(tag) & 0xffff0000u);
        const uint32_t qwc = static_cast<uint32_t>(tag) & 0xffffu;
        const uint32_t tag_id =
            (static_cast<uint32_t>(tag) >> 28u) & 7u;
        channel.m_madr = static_cast<uint32_t>(tag >> 32u);

        if (!transfer_from_spr_span(channel, qwc, copy_data))
        {
            return false;
        }

        done = tag_id == 7u ||
            (tie && (tag & 0x80000000ull) != 0u);
    }

    return done;
}

bool c_ee_dmac::transfer_from_spr_chain()
{
    if (m_memory == nullptr)
    {
        return false;
    }

    // First pass: check every destination and verify termination without
    // modifying guest memory or visible channel registers.
    s_spr_channel candidate = m_spr_channels[0];
    if (!walk_from_spr_chain(candidate, false))
    {
        return false;
    }

    // Only validated chains are copied. The source tags/data live in
    // SPR and destination addresses are restricted to main EE RAM, so
    // the copy cannot modify tags consumed by the second pass.
    candidate = m_spr_channels[0];
    if (!walk_from_spr_chain(candidate, true))
    {
        return false;
    }

    candidate.m_chcr &= ~k_chcr_str;
    m_spr_channels[0] = candidate;
    return raise(8u);
}

bool c_ee_dmac::transfer_to_spr_span(
    s_spr_channel& channel, uint32_t qwc, bool copy_data)
{
    // Chain sources for channel 9 are physical EE RAM, never MMIO,
    // guest KSEG pointers, or VU memory. Validate a whole span before
    // copying, mirroring the strict normal/interleave RAM policy.
    const uint64_t bytes = static_cast<uint64_t>(qwc) * 16u;
    const uint64_t end = static_cast<uint64_t>(channel.m_madr) + bytes;
    if (qwc != 0u &&
        ((channel.m_madr & 0x0fu) != 0u ||
         channel.m_madr >= 0x02000000u ||
         end > 0x02000000ull ||
         !m_memory->contains(channel.m_madr, static_cast<size_t>(bytes))))
    {
        return false;
    }

    if (copy_data)
    {
        for (uint32_t i = 0; i < qwc; ++i)
        {
            const uint32_t ram_address = channel.m_madr + i * 16u;
            const uint32_t spr_address = ee_memory::k_scratchpad_base +
                ((channel.m_sadr + i * 16u) & 0x3ff0u);
            s_ee_u128 quad{};
            m_memory->read128(ram_address, &quad);
            m_memory->write128(spr_address, &quad);
        }
    }

    channel.m_sadr =
        (channel.m_sadr + static_cast<uint32_t>(bytes)) & 0x3ff0u;
    channel.m_madr += static_cast<uint32_t>(bytes);
    channel.m_qwc = 0u;
    return true;
}

bool c_ee_dmac::walk_to_spr_chain(
    s_spr_channel& channel, bool copy_data)
{
    if (m_memory == nullptr ||
        ((channel.m_chcr >> 2u) & 3u) != 1u)
    {
        return false;
    }

    const bool tie = (channel.m_chcr & 0x80u) != 0u;
    bool end = false;
    if (channel.m_qwc != 0u)
    {
        const uint32_t id = (channel.m_chcr >> 28u) & 7u;
        end = id == 0u || id == 7u ||
            (tie && (channel.m_chcr & 0x80000000u) != 0u);
        if (!transfer_to_spr_span(channel, channel.m_qwc, copy_data))
        {
            return false;
        }
    }

    // The reference implementation walks at most 1,000,000 tags.
    // Bound this experimental runtime to 4,096 to prevent untrusted,
    // cyclic or malformed DMA chains from stalling the native host.
    constexpr uint32_t k_max_chain_tags = 4096u;
    for (uint32_t i = 0; !end && i < k_max_chain_tags; ++i)
    {
        const uint64_t tag_address = channel.m_tadr;
        if ((tag_address & 0x0fu) != 0u ||
            tag_address >= 0x02000000ull ||
            tag_address + 16u > 0x02000000ull ||
            !m_memory->contains(static_cast<uint32_t>(tag_address), 16u))
        {
            return false;
        }

        const uint64_t tag = m_memory->read64(channel.m_tadr);
        const uint32_t low = static_cast<uint32_t>(tag);
        const uint32_t qwc = low & 0xffffu;
        const uint32_t id = (low >> 28u) & 7u;
        const bool irq = (low & 0x80000000u) != 0u;
        // The reference masks off low address bits but preserves bit31.
        const uint32_t addr =
            static_cast<uint32_t>(tag >> 32u) & 0xfffffff0u;
        const uint64_t after_tag = tag_address + 16u;
        const uint64_t after_data =
            after_tag + static_cast<uint64_t>(qwc) * 16u;
        channel.m_chcr = (channel.m_chcr & 0x0000ffffu) |
            (low & 0xffff0000u);
        uint32_t data_address = static_cast<uint32_t>(after_tag);

        switch (id)
        {
        case 0u: // REFE: pointed-to payload, then terminate
            data_address = addr;
            channel.m_tadr = static_cast<uint32_t>(after_tag);
            end = true;
            break;
        case 1u: // CNT: inline data, then next sequential tag
            if (after_data > 0xffffffffull)
            {
                return false;
            }
            channel.m_tadr = static_cast<uint32_t>(after_data);
            break;
        case 2u: // NEXT: inline data, then jump to the tag address
            channel.m_tadr = addr;
            break;
        case 3u: // REF: pointed-to payload, continue sequential tags
        case 4u: // REFS: equivalent source selection without stall events
            data_address = addr;
            channel.m_tadr = static_cast<uint32_t>(after_tag);
            break;
        case 5u: // CALL: inline payload and push next sequential tag
        {
            const uint32_t asp = (channel.m_chcr >> 4u) & 3u;
            if (after_data > 0xffffffffull)
            {
                return false;
            }
            if (asp < 2u)
            {
                channel.m_asr[asp] = static_cast<uint32_t>(after_data);
            }
            const uint32_t new_asp = asp < 2u ? asp + 1u : 2u;
            channel.m_chcr = (channel.m_chcr & ~0x30u) |
                (new_asp << 4u);
            channel.m_tadr = addr;
            break;
        }
        case 6u: // RET: return to most recent saved tag, or terminate
        {
            const uint32_t asp = (channel.m_chcr >> 4u) & 3u;
            if (asp != 0u && asp <= 2u)
            {
                channel.m_tadr = channel.m_asr[asp - 1u];
                channel.m_chcr = (channel.m_chcr & ~0x30u) |
                    ((asp - 1u) << 4u);
            }
            else if (asp == 0u)
            {
                end = true;
            }
            else
            {
                return false;
            }
            break;
        }
        case 7u: // END: inline data, terminate
            end = true;
            break;
        default:
            return false;
        }

        channel.m_madr = data_address;
        if (!transfer_to_spr_span(channel, qwc, copy_data))
        {
            return false;
        }

        if (tie && irq)
        {
            end = true;
        }
    }

    return end;
}

bool c_ee_dmac::transfer_to_spr_chain()
{
    if (m_memory == nullptr)
    {
        return false;
    }

    // Read-only dry run first: all tag/control flow and RAM spans must
    // be valid before the first scratchpad write. DMA source RAM cannot
    // be changed by writing SPR, so the second pass follows the same tags.
    s_spr_channel candidate = m_spr_channels[1];
    if (!walk_to_spr_chain(candidate, false))
    {
        return false;
    }

    candidate = m_spr_channels[1];
    if (!walk_to_spr_chain(candidate, true))
    {
        return false;
    }

    candidate.m_chcr &= ~k_chcr_str;
    m_spr_channels[1] = candidate;
    return raise(9u);
}

bool c_ee_dmac::write_spr_channel(uint32_t address, uint32_t value)
{
    s_spr_channel* channel = nullptr;
    uint32_t channel_index = 0;
    uint32_t offset = 0;
    bool device_channel = false;

    if (address >= k_vif0_base && address < k_vif0_base + 0x100u)
    {
        channel = &m_device_channels[0];
        channel_index = 0u;
        offset = address - k_vif0_base;
        device_channel = true;
    }
    else if (address >= k_vif1_base && address < k_vif1_base + 0x100u)
    {
        channel = &m_device_channels[1];
        channel_index = 1u;
        offset = address - k_vif1_base;
        device_channel = true;
    }
    else if (address >= k_gif_base && address < k_gif_base + 0x100u)
    {
        channel = &m_device_channels[2];
        channel_index = 2u;
        offset = address - k_gif_base;
        device_channel = true;
    }
    else if (address >= k_sif0_base && address < k_sif0_base + 0x100u)
    {
        channel = &m_sif0_channel;
        channel_index = 5u;
        offset = address - k_sif0_base;
        device_channel = true;
    }
    else if (address >= k_sif1_base && address < k_sif1_base + 0x100u)
    {
        channel = &m_sif1_channel;
        channel_index = 6u;
        offset = address - k_sif1_base;
        device_channel = true;
    }
    else if (address >= k_from_spr_base && address < k_from_spr_base + 0x100u)
    {
        channel = &m_spr_channels[0];
        channel_index = 8u;
        offset = address - k_from_spr_base;
    }
    else if (address >= k_to_spr_base && address < k_to_spr_base + 0x100u)
    {
        channel = &m_spr_channels[1];
        channel_index = 9u;
        offset = address - k_to_spr_base;
    }
    else if (address >= k_from_ipu_base && address < k_from_ipu_base + 0x100u)
    {
        channel = &m_ipu_channels[0];
        channel_index = 3u;
        offset = address - k_from_ipu_base;
        device_channel = true;
    }
    else if (address >= k_to_ipu_base && address < k_to_ipu_base + 0x100u)
    {
        channel = &m_ipu_channels[1];
        channel_index = 4u;
        offset = address - k_to_ipu_base;
        device_channel = true;
    }
    else
    {
        return false;
    }

    if (m_vif1_pending)
    {
        if (channel_index == 1u)
        {
            if (offset == 0u)
            {
                if ((value & k_chcr_str) == 0u)
                {
                    // Explicit stop abandons the staged transfer; do
                    // not raise a completion interrupt for discarded work.
                    cancel_deferred_vif1();
                }
                else
                {
                    // STR is already set. Do not start a second DMA or
                    // overwrite the registers of the outstanding job.
                    return true;
                }
            }
            else
            {
                // Conservative register-write fence: never overwrite
                // staged post-transfer registers behind the worker.
                (void)service_deferred_vif1(true);
            }
        }
        else if (channel_index == 2u && offset == 0u &&
                 (value & k_chcr_str) != 0u)
        {
            // PATH3 cannot overtake the earlier staged PATH1/PATH2
            // transaction. GS parsers remain EE-thread-owned.
            (void)service_deferred_vif1(true);
        }
    }

    switch (offset)
    {
    case 0x00u:
    {
        const uint32_t previous_chcr = channel->m_chcr;
        const bool was_running = (channel->m_chcr & k_chcr_str) != 0u;
        if (channel_index == 2u && !was_running &&
            (value & k_chcr_str) != 0u)
        {
            m_gif_last_reject = e_gif_dma_reject::none;
        }
        channel->m_chcr = value;
        if (!was_running && (value & k_chcr_str) != 0u)
        {
            const uint32_t mode = (channel->m_chcr >> 2u) & 3u;
            if (channel_index == 5u)
            {
                m_sif0_chain_end = false;
            }
            if (channel_index == 6u)
            {
                m_sif1_chain_end = false;
                m_sif1_chain_tags = 0u;
            }
            if (channel_index == 3u || channel_index == 4u)
            {
                if (channel_index == 4u)
                {
                    // A nonzero QWC continues the tag already latched in
                    // CHCR[31:16] (ps2-web-recomp chain_ends_after_current).
                    const uint32_t id = (channel->m_chcr >> 28u) & 7u;
                    m_to_ipu_tag_end = channel->m_qwc != 0u &&
                        (id == 0u || id == 7u ||
                         ((channel->m_chcr & 0x80u) != 0u &&
                          (channel->m_chcr & 0x80000000u) != 0u));
                }
                if (channel_index == 3u && channel->m_qwc == 0u)
                {
                    finish_ipu(3u);
                }
                else if (m_ipu_start != nullptr)
                {
                    m_ipu_start(m_ipu_user, channel_index);
                }
            }
            else if (channel_index == 5u || channel_index == 6u)
            {
                // Both channels notify their genuine FIFO engines.
                // Neither MMIO register write itself retires STR.
                if (channel_index == 5u && m_sif0_start != nullptr)
                {
                    m_sif0_start(m_sif0_user);
                }
                if (channel_index == 6u && m_sif1_start != nullptr)
                {
                    m_sif1_start(m_sif1_user);
                }
            }
            else if (device_channel)
            {
                if (channel_index == 2u && mode == 1u)
                {
                    (void)transfer_gif_source_chain();
                }
                else if (channel_index == 1u && mode == 1u)
                {
                    (void)transfer_vif1_source_chain();
                }
                else
                {
                    (void)transfer_device_normal(channel_index);
                }
            }
            else if (channel_index == 8u && mode == 1u)
            {
                (void)transfer_from_spr_chain();
            }
            else if (channel_index == 9u && mode == 1u)
            {
                (void)transfer_to_spr_chain();
            }
            else if (mode == 2u && ((m_sqwc >> 16u) & 0xffu) != 0u)
            {
                (void)transfer_spr_interleave(channel_index);
            }
            else
            {
                (void)transfer_spr_normal(channel_index);
            }
        }
        if (channel_index == 2u)
        {
            trace_gif_write(0x00u, value, previous_chcr);
        }
        return true;
    }
    case 0x10u:
    {
        const uint32_t previous = channel->m_madr;
        channel->m_madr = value;
        if (channel_index == 2u)
        {
            trace_gif_write(0x10u, value, previous);
        }
        return true;
    }
    case 0x20u:
    {
        const uint32_t previous = channel->m_qwc;
        channel->m_qwc = value & 0xffffu;
        if (channel_index == 2u)
        {
            trace_gif_write(0x20u, value, previous);
        }
        return true;
    }
    case 0x30u:
    {
        const uint32_t previous = channel->m_tadr;
        channel->m_tadr = value;
        if (channel_index == 2u)
        {
            trace_gif_write(0x30u, value, previous);
        }
        return true;
    }
    case 0x40u: channel->m_asr[0] = value; return true;
    case 0x50u: channel->m_asr[1] = value; return true;
    case 0x80u:
    {
        if (device_channel)
        {
            return false;
        }
        channel->m_sadr = value & 0x3ff0u;
        return true;
    }
    default: return false;
    }
}

bool c_ee_dmac::mmio_read(void* user, uint32_t address,
                           uint32_t width, s_ee_u128* value)
{
    if (user == nullptr || value == nullptr || width != 4u)
    {
        return false;
    }

    c_ee_dmac* controller = static_cast<c_ee_dmac*>(user);
    uint32_t word = 0u;
    if (address == k_stat_address)
    {
        word = controller->m_status;
    }
    else if (address == k_sqwc_address)
    {
        word = controller->m_sqwc;
    }
    else if (!controller->read_spr_channel(address, &word))
    {
        return false;
    }

    *value = {word, 0u};
    return true;
}

bool c_ee_dmac::mmio_write(void* user, uint32_t address,
                            uint32_t width, const s_ee_u128* value)
{
    if (user == nullptr || value == nullptr || width != 4u)
    {
        return false;
    }

    c_ee_dmac* controller = static_cast<c_ee_dmac*>(user);
    const uint32_t word = static_cast<uint32_t>(value->m_lo);
    if (address == k_sqwc_address)
    {
        controller->m_sqwc = word;
        return true;
    }

    if (address != k_stat_address)
    {
        return controller->write_spr_channel(address, word);
    }

    // D_STAT lower 16 bits are W1C, upper 16 bits W1-toggle.
    // Keep control/other D_STAT bits visible without pretending that
    // a transfer channel is implemented.
    controller->m_status &= ~(word & 0x0000ffffu);
    controller->m_status ^= word & 0xffff0000u;
    controller->sync_interrupt_line();
    return true;
}
} // namespace ee
