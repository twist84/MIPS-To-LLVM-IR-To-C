#include "iop_hw.h"
#include "iop_cpu.h"
#include "iop_memory_card.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace iop
{
c_hw::c_hw()
{
    reset();
}

void c_hw::reset()
{
    std::fill(
        m_scratchpad,
        m_scratchpad + k_scratchpad_size,
        static_cast<uint8_t>(0u)
    );
    m_cdvd_i_stat = 0u;
    m_cdvd_dec_set = 0u;
    m_cdvd_ncmd_command = 0u;
    std::fill(
        m_cdvd_ncmd_params,
        m_cdvd_ncmd_params + k_cdvd_ncmd_param_capacity,
        static_cast<uint8_t>(0u)
    );
    m_cdvd_ncmd_param_count = 0u;
    m_cdvd_ncmd_pending = false;
    m_i_stat = 0u;
    m_i_mask = 0u;
    m_i_ctrl = 1u;
    m_cache_control = 0u;
    m_spu2.reset();
    m_ssbus_spu2_addr = 0u;
    m_ssbus_spu2_addr1 = 0u;
    m_ssbus_spu2_delay = 0u;
    m_ssbus_spu2_delay1 = 0u;
    m_sio2_ctrl = 0u;
    m_sio2_stat = 0u;
    m_sio2_intr = 0u;
    m_sio2_index = 0u;
    if (m_memory_card != nullptr)
    {
        m_memory_card->reset_protocol();
    }
    std::fill(m_sio2_send3, m_sio2_send3 + 16u, 0u);
    std::fill(m_sio2_port, m_sio2_port + 8u, 0u);
    m_sio2_pad[0] = s_sio2_pad{};
    m_sio2_pad[1] = s_sio2_pad{};
    // Primary ps2-web-recomp/SDBZ reference has a neutral pad in port 0.
    // This models a connected emulated peripheral, not host input.
    m_sio2_pad[0].m_connected = true;
    m_sio2_pending_tx.clear();
    m_sio2_rx.clear();
    m_sio2_rx_cross_tags.clear();
    m_sio2_empty_rx_read_count = 0u;
    m_pad_trace_dispatch = 0u;
    m_sio2_pad0_poll_count = 0u;
    m_sio2_pad0_cross_pressed_polls = 0u;
    m_sio2_pad0_cross_bytes_pio_read = 0u;
    m_sio2_pad0_cross_bytes_dma_written = 0u;
    m_sio2_pad0_cross_bytes_fifo_discarded = 0u;
    m_sio2_pad0_cross_first_poll = 0u;
    m_sio2_pad0_cross_poll_count = 0u;
    m_sio2_transfer_requested = false;
    m_sio2_ctrl_start_count = 0u;
    m_sio2_packet_count = 0u;
    m_sio2_transfer_count = 0u;
    m_sio2_last_start_guest_pc = 0u;
    m_sio2_last_start_guest_ra = 0u;
    m_sio2_packets_at_last_start = 0u;
    m_sio2_tx_bytes_at_last_start = 0u;
    m_sio2_rx_bytes_at_last_start = 0u;
    for (auto& packet : m_sio2_first_packets)
    {
        packet = s_sio2_packet_observation{};
    }
    m_sio2_last_rejection = e_sio2_rejection::none;
    m_sio2_rejected_start_count = 0u;
    m_timer5_count = 0u;
    m_timer5_mode = 0u;
    m_timer5_target = 0u;
    m_timer5_remainder = 0u;
    m_timer5_irq_armed = true;
    std::fill(m_cdrom_dma, m_cdrom_dma + 3u, 0u);
    for (auto& core : m_spu2_dma)
    {
        for (uint32_t& reg : core)
        {
            reg = 0u;
        }
    }
    m_spu2_dma_remaining[0] = 0u;
    m_spu2_dma_remaining[1] = 0u;
    m_spu2_auto[0] = {};
    m_spu2_auto[1] = {};
    m_spu2_auto_started = 0u;
    m_spu2_auto_completed = 0u;
    m_spu2_auto_bytes = 0u;
    m_dma_cpu = nullptr;
    m_dma_pcr = 0u;
    m_dma_pcr2 = 0u;
    for (auto& bank : m_sio2_dma)
    {
        for (uint32_t& register_value : bank)
        {
            register_value = 0u;
        }
    }
    m_dma_global_enable = 0u;
    m_sif_mscom = 0u;
    m_sif_smcom = 0u;
    m_sif_msflag = 0u;
    m_sif_smflag = 0u;
    m_sif_mmio_user = nullptr;
    m_sif0_user = nullptr;
    m_sif0_start = nullptr;
    m_sif1_user = nullptr;
    m_sif1_start = nullptr;
    m_sif_mmio_read = nullptr;
    m_sif_mmio_write = nullptr;
    m_cdvd_user = nullptr;
    m_cdvd_start = nullptr;
    for (uint32_t channel = 0u; channel < 2u; ++channel)
    {
        for (uint32_t reg = 0u; reg < 4u; ++reg)
        {
            m_sif_dma[channel][reg] = 0u;
        }
    }
    m_dma_icr = 0u;
    m_dma_icr2 = 0u;
    m_debug_console.clear();
    m_debug_console_truncated = false;
}

void c_hw::bind_sif_mmio(
    void* user, t_sif_mmio_read read, t_sif_mmio_write write
)
{
    m_sif_mmio_user = (user != nullptr && read != nullptr &&
                       write != nullptr) ? user : nullptr;
    m_sif_mmio_read = m_sif_mmio_user != nullptr ? read : nullptr;
    m_sif_mmio_write = m_sif_mmio_user != nullptr ? write : nullptr;
}

void c_hw::bind_sif0_start(void* user, t_sif0_start_callback callback)
{
    m_sif0_user = callback != nullptr ? user : nullptr;
    m_sif0_start = m_sif0_user != nullptr ? callback : nullptr;
}

void c_hw::bind_sif1_start(void* user, t_sif1_start_callback callback)
{
    m_sif1_user = callback != nullptr ? user : nullptr;
    m_sif1_start = m_sif1_user != nullptr ? callback : nullptr;
}

void c_hw::bind_cdvd_start(void* user, t_cdvd_start_callback callback)
{
    m_cdvd_user = callback != nullptr ? user : nullptr;
    m_cdvd_start = m_cdvd_user != nullptr ? callback : nullptr;
}

void c_hw::bind_dma_cpu(c_cpu* cpu)
{
    m_dma_cpu = cpu;
}

bool c_hw::complete_memory_card_auth_reset(uint32_t port, uint32_t slot)
{
    return port == 2u && slot == 0u && m_memory_card != nullptr &&
        m_memory_card->complete_auth_reset();
}

void c_hw::set_sio2_pad0_input(uint16_t buttons, uint8_t right_x,
    uint8_t right_y, uint8_t left_x, uint8_t left_y)
{
    m_pad0_buttons = buttons;
    m_pad0_axes[0] = right_x;
    m_pad0_axes[1] = right_y;
    m_pad0_axes[2] = left_x;
    m_pad0_axes[3] = left_y;
}

void c_hw::configure_sio2_pad0_cross_pulse(
    uint32_t first_poll, uint32_t poll_count)
{
    // A test pulse is explicitly opt-in and only modifies the bytes
    // returned to a real guest pad read; it never queues a new packet.
    m_sio2_pad0_cross_first_poll =
        first_poll != 0u && poll_count != 0u ? first_poll : 0u;
    m_sio2_pad0_cross_poll_count =
        m_sio2_pad0_cross_first_poll != 0u ? poll_count : 0u;
    m_sio2_pad0_poll_count = 0u;
    m_sio2_pad0_cross_pressed_polls = 0u;
    m_sio2_pad0_cross_bytes_pio_read = 0u;
    m_sio2_pad0_cross_bytes_dma_written = 0u;
    m_sio2_pad0_cross_bytes_fifo_discarded = 0u;
}

bool c_hw::pad_trace_window() const
{
    const uint64_t poll = m_sio2_pad0_poll_count;
    const uint64_t begin = m_sio2_pad0_cross_first_poll;
    const uint64_t end = begin + m_sio2_pad0_cross_poll_count;
    // Include one preceding poll and two release polls. Limit output
    // during intentionally long holds to the press and release edges.
    return begin != 0u && poll != 0u && poll + 1u >= begin &&
        poll < end + 2u && (poll < begin + 8u || poll + 1u >= end);
}

// SIO2 controller exchange from ps2-web-recomp/SDBZ's sio2.cpp:
// SEND3 supplies the input and output sizes; the guest's actual TX bytes
// select the device command. Never make a response to a write-only setup.
bool c_hw::process_sio2_packet()
{
    if (m_dma_cpu == nullptr || m_sio2_index >= 16u)
    {
        return false;
    }
    const uint32_t send = m_sio2_send3[m_sio2_index];
    const uint32_t in_size = (send >> 8u) & 0x1ffu;
    const uint32_t out_size = (send >> 18u) & 0x1ffu;
    if (in_size == 0u || out_size == 0u ||
        in_size > k_sio2_tx_limit || out_size > k_sio2_tx_limit ||
        m_sio2_pending_tx.size() < in_size ||
        m_sio2_rx.size() > k_sio2_tx_limit - out_size)
    {
        return false;
    }

    const uint32_t port = send & 3u;
    std::vector<uint8_t> reply(out_size, 0xffu);
    const auto put = [&reply](uint32_t index, uint8_t value)
    {
        if (index < reply.size())
        {
            reply[index] = value;
        }
    };
    if (m_sio2_index == 0u)
    {
        m_sio2_stat = 0x1100u;
    }
    const bool card_present = port == 2u && m_memory_card != nullptr &&
        m_memory_card->exchange(m_sio2_pending_tx.data(), in_size, reply);
    if (m_sio2_pending_tx[0u] == 0x81u && std::getenv("SDBZ_TRACE_MEMCARD") != nullptr)
    {
        std::fprintf(stderr, "[mc] port=%u cmd=%02x in=%u out=%u response=%u\n",
            port, in_size >= 2u ? m_sio2_pending_tx[1u] : 0u,
            in_size, out_size, card_present ? 1u : 0u);
    }
    const bool pad_present = port < 2u &&
        m_sio2_pad[port].m_connected &&
        m_sio2_port[port * 2u + 1u] != 0x5ffffu &&
        m_sio2_port[port * 2u + 1u] != 0x00030064u &&
        m_sio2_pending_tx[0u] == 0x01u;
    bool cross_reply = false;
    if (!pad_present && !card_present)
    {
        m_sio2_stat |= 0xc000u |
            (0x10000u << (m_sio2_index & 15u));
    }
    else if (pad_present)
    {
        s_sio2_pad& pad = m_sio2_pad[port];
        const uint8_t command = in_size >= 2u ?
            m_sio2_pending_tx[1u] : 0u;
        put(0u, 0xffu);
        put(1u, pad.m_config ? 0xf3u : pad.m_mode);
        put(2u, 0x5au);
        const auto neutral_data = [&]()
        {
            put(3u, 0xffu);
            put(4u, 0xffu);
            if (pad.m_mode != 0x41u)
            {
                for (uint32_t index = 5u; index < 9u; ++index)
                {
                    put(index, 0x80u);
                }
            }
            if (pad.m_mode == 0x79u)
            {
                for (uint32_t index = 9u; index < 21u; ++index)
                {
                    put(index, 0u);
                }
            }
        };
        switch (command)
        {
        case 0x40u:
            put(3u, 0u); put(4u, 0u); put(5u, 2u);
            put(6u, 0u); put(7u, 0u); put(8u, 0x5au);
            break;
        case 0x41u:
            if (pad.m_mode == 0x41u)
            {
                for (uint32_t index = 3u; index < 9u; ++index)
                {
                    put(index, 0u);
                }
            }
            else
            {
                put(3u, pad.m_poll_mask[0u]);
                put(4u, pad.m_poll_mask[1u]);
                put(5u, pad.m_poll_mask[2u]);
                put(6u, 0u); put(7u, 0u); put(8u, 0x5au);
            }
            break;
        case 0x42u:
            neutral_data();
            if (port == 0u)
            {
                // PS2 pad button bits are active-low: Cross is bit 14,
                // the upper byte's bit 6. Only a genuine guest 0x42
                // read on a connected port-0 pad consumes the probe.
                ++m_sio2_pad0_poll_count;
                s_pad0_poll input{};
                input.m_poll = m_sio2_pad0_poll_count;
                input.m_mode = pad.m_mode | (pad.m_config ? 0x100u : 0u);
                input.m_input_size = in_size;
                input.m_output_size = out_size;
                input.m_buttons = m_pad0_buttons;
                std::copy_n(m_pad0_axes, 4u, input.m_axes);
                if (m_pad0_poll != nullptr &&
                    !m_pad0_poll(m_pad0_poll_user, input))
                {
                    return false;
                }
                uint16_t buttons = input.m_buttons;
                if (m_sio2_pad0_cross_poll_count != 0u &&
                    m_sio2_pad0_poll_count >=
                        m_sio2_pad0_cross_first_poll &&
                    m_sio2_pad0_poll_count -
                        m_sio2_pad0_cross_first_poll <
                        m_sio2_pad0_cross_poll_count)
                {
                    buttons |= 1u << 14u;
                    cross_reply = true;
                    ++m_sio2_pad0_cross_pressed_polls;
                }
                bool scripted = false;
                for (const s_pad0_script_step& step : m_sio2_pad0_script)
                {
                    if (m_sio2_pad0_poll_count >= step.m_first_poll &&
                        m_sio2_pad0_poll_count - step.m_first_poll <
                            step.m_poll_count)
                    {
                        buttons |= step.m_buttons;
                        scripted = true;
                    }
                }
                if (scripted)
                {
                    ++m_sio2_pad0_script_pressed_polls;
                }
                const uint16_t wire_buttons = static_cast<uint16_t>(~buttons);
                put(3u, static_cast<uint8_t>(wire_buttons));
                put(4u, static_cast<uint8_t>(wire_buttons >> 8u));
                if (pad.m_mode != 0x41u)
                {
                    for (uint32_t index = 0u; index < 4u; ++index)
                    {
                        put(5u + index, input.m_axes[index]);
                    }
                }
                if (pad.m_mode == 0x79u)
                {
                    constexpr uint8_t pressure_bits[] = {
                        5u, 7u, 4u, 6u, 12u, 13u, 14u, 15u, 10u, 11u, 8u, 9u
                    };
                    for (uint32_t index = 0u; index < sizeof(pressure_bits); ++index)
                    {
                        put(9u + index,
                            (input.m_buttons & (1u << pressure_bits[index])) != 0u ?
                                0xffu : 0u);
                    }
                }
                if (pad_trace_window() && reply.size() >= 5u)
                {
                    std::fprintf(stderr,
                        "PAD poll=%u ee_completed=%llu iop_cpu_cycle=%llu "
                        "wire=%02x %02x %02x %02x %02x\n",
                        pad0_polls(), static_cast<unsigned long long>(m_pad_trace_dispatch),
                        static_cast<unsigned long long>(m_dma_cpu->cycles()),
                        reply[0], reply[1], reply[2], reply[3], reply[4]);
                }
            }
            break;
        case 0x43u:
            if (!pad.m_config)
            {
                neutral_data();
            }
            else
            {
                for (uint32_t index = 3u; index < 9u; ++index)
                {
                    put(index, 0u);
                }
            }
            if (in_size > 3u)
            {
                pad.m_config = m_sio2_pending_tx[3u] == 1u;
            }
            break;
        case 0x44u:
            if (in_size > 4u)
            {
                pad.m_mode = m_sio2_pending_tx[3u] != 0u ? 0x73u : 0x41u;
                pad.m_locked = m_sio2_pending_tx[4u] == 3u;
            }
            for (uint32_t index = 3u; index < 9u; ++index)
            {
                put(index, 0u);
            }
            break;
        case 0x45u:
            put(3u, 3u); put(4u, 2u);
            put(5u, pad.m_mode == 0x41u ? 0u : 1u);
            put(6u, 2u); put(7u, 1u); put(8u, 0u);
            break;
        case 0x46u:
            put(3u, 0u); put(4u, 0u); put(5u, 1u);
            put(6u, m_sio2_pending_tx.size() > 3u &&
                m_sio2_pending_tx[3u] == 0u ? 2u : 1u);
            put(7u, m_sio2_pending_tx.size() > 3u &&
                m_sio2_pending_tx[3u] == 0u ? 0u : 1u);
            put(8u, m_sio2_pending_tx.size() > 3u &&
                m_sio2_pending_tx[3u] == 0u ? 0x0au : 0x14u);
            break;
        case 0x47u:
            put(3u, 0u); put(4u, 0u); put(5u, 2u);
            put(6u, 0u); put(7u, 1u); put(8u, 0u);
            break;
        case 0x4cu:
            put(3u, 0u); put(4u, 0u); put(5u, 0u);
            put(6u, m_sio2_pending_tx.size() > 3u &&
                m_sio2_pending_tx[3u] == 0u ? 4u : 7u);
            put(7u, 0u); put(8u, 0u);
            break;
        case 0x4du:
            for (uint32_t index = 3u; index < 9u; ++index)
            {
                put(index, 0xffu);
            }
            for (uint32_t index = 0u;
                 index < 6u && 3u + index < in_size; ++index)
            {
                pad.m_motors[index] = m_sio2_pending_tx[3u + index];
            }
            break;
        case 0x4fu:
            if (in_size > 5u)
            {
                pad.m_poll_mask[0u] = m_sio2_pending_tx[3u];
                pad.m_poll_mask[1u] = m_sio2_pending_tx[4u];
                pad.m_poll_mask[2u] = m_sio2_pending_tx[5u];
            }
            pad.m_mode = 0x79u;
            for (uint32_t index = 3u; index < 9u; ++index)
            {
                put(index, 0u);
            }
            put(8u, 0x5au);
            break;
        default:
            neutral_data();
            break;
        }
    }
    // A mere packet count cannot distinguish controller port 0/1
    // from memory-card port 2. Preserve the first four *real* decoded
    // requests without changing any guest-visible SIO2 response bytes.
    if (m_sio2_packet_count < 4u)
    {
        s_sio2_packet_observation& packet =
            m_sio2_first_packets[m_sio2_packet_count];
        packet.m_port = port;
        packet.m_send3 = send;
        packet.m_input_size = in_size;
        packet.m_output_size = out_size;
        packet.m_first_tx_byte = m_sio2_pending_tx[0u];
        packet.m_command = in_size >= 2u ? m_sio2_pending_tx[1u] : 0u;
        packet.m_pad_present = pad_present;
    }
    for (size_t index = 0u; index < reply.size(); ++index)
    {
        m_sio2_rx.push_back(reply[index]);
        m_sio2_rx_cross_tags.push_back(
            cross_reply && index == 4u ? 1u : 0u);
    }
    m_sio2_pending_tx.clear();
    ++m_sio2_index;
    ++m_sio2_packet_count;
    return true;
}

// CTRL.start is the only SIO2 transaction boundary. Transfer actual IOP
// RAM bytes for armed DMA11/12 and acknowledge precisely those channels.
// An absent guest buffer or incomplete packet keeps STR armed and raises no
// IRQ; setup writes alone never generate successful completion.
bool c_hw::run_sio2_transfer()
{
    // Record why a genuine guest CTRL.start was not completed. Rejection
    // attribution is host-only and never acknowledges DMA11/12 or IRQ17.
    m_sio2_last_rejection = e_sio2_rejection::none;
    const auto reject = [this](e_sio2_rejection reason)
    {
        m_sio2_last_rejection = reason;
        ++m_sio2_rejected_start_count;
        return false;
    };
    if (m_dma_cpu == nullptr)
    {
        return reject(e_sio2_rejection::cpu_unavailable);
    }
    const bool input_dma =
        (m_sio2_dma[0u][2u] & k_spu2_dma_str_bit) != 0u;
    const bool output_dma =
        (m_sio2_dma[1u][2u] & k_spu2_dma_str_bit) != 0u;
    std::vector<uint8_t> inbound;
    uint32_t input_words = 0u;
    uint32_t input_blocks = 0u;
    uint32_t output_words = 0u;
    uint32_t output_blocks = 0u;
    if (input_dma)
    {
        const uint32_t bcr = m_sio2_dma[0u][1u];
        input_words = bcr & 0xffffu;
        input_blocks = bcr >> 16u;
        const uint64_t total = static_cast<uint64_t>(
            input_words) * input_blocks * 4u;
        if (input_words == 0u || input_blocks == 0u ||
            input_blocks > 16u || total > k_sio2_tx_limit ||
            m_sio2_index + input_blocks > 16u)
        {
            return reject(e_sio2_rejection::dma11_geometry);
        }
        inbound.resize(static_cast<size_t>(total));
        if (!m_dma_cpu->dma_read(
                m_sio2_dma[0u][0u], inbound.data(),
                static_cast<uint32_t>(total)))
        {
            return reject(e_sio2_rejection::dma11_source);
        }
    }
    if (output_dma)
    {
        const uint32_t bcr = m_sio2_dma[1u][1u];
        output_words = bcr & 0xffffu;
        output_blocks = bcr >> 16u;
        const uint64_t total = static_cast<uint64_t>(
            output_words) * output_blocks * 4u;
        if (output_words == 0u || output_blocks == 0u ||
            output_blocks > 16u || total > k_sio2_tx_limit)
        {
            return reject(e_sio2_rejection::dma12_geometry);
        }
        // Validate the write range before processing the device command.
        // A failed target must never perform partial peripheral writes.
        uint8_t check = 0u;
        if (!m_dma_cpu->dma_read(m_sio2_dma[1u][0u], &check, 1u) ||
            !m_dma_cpu->dma_read(
                m_sio2_dma[1u][0u] +
                    static_cast<uint32_t>(total) - 1u, &check, 1u))
        {
            return reject(e_sio2_rejection::dma12_destination);
        }
    }
    bool processed = false;
    if (input_dma)
    {
        const uint32_t block_bytes = input_words * 4u;
        for (uint32_t block = 0u; block < input_blocks; ++block)
        {
            const uint8_t* begin = inbound.data() + block * block_bytes;
            m_sio2_pending_tx.insert(
                m_sio2_pending_tx.end(), begin, begin + block_bytes);
            if (!process_sio2_packet())
            {
                return reject(e_sio2_rejection::packet_incomplete);
            }
            processed = true;
        }
    }
    else if (!m_sio2_pending_tx.empty())
    {
        processed = process_sio2_packet();
    }
    else if (!m_sio2_rx.empty())
    {
        processed = true;
    }
    if (!processed)
    {
        // Original SDBZ SIO2MAN issues CTRL=0x3bd after sending its
        // two PIO pad polls. The combined FIFO reset/start clears both
        // queues *before* entering here. The primary ps2-web-recomp
        // SIO2 run_transfer() still acknowledges that actual START
        // through IRQ17, even with no additional data to transfer.
        //
        // An empty PIO START is therefore a real control completion,
        // not a missing device reply. Incomplete PIO packets and armed
        // DMA11/12 requests must still pass their normal preflight.
        if (!input_dma && !output_dma && m_sio2_pending_tx.empty())
        {
            processed = true;
        }
        else
        {
            return reject(m_sio2_pending_tx.empty()
                ? e_sio2_rejection::no_processed_packet
                : e_sio2_rejection::packet_incomplete);
        }
    }
    if (output_dma)
    {
        const uint32_t block_bytes = output_words * 4u;
        for (uint32_t block = 0u; block < output_blocks; ++block)
        {
            if (m_sio2_index < output_blocks)
            {
                return reject(e_sio2_rejection::dma12_packet_count);
            }
            if (m_sio2_rx.size() <
                ((m_sio2_send3[block] >> 18u) & 0x1ffu))
            {
                return reject(e_sio2_rejection::dma12_reply_underflow);
            }
            const uint32_t count =
                (m_sio2_send3[block] >> 18u) & 0x1ffu;
            std::vector<uint8_t> bytes(count);
            uint32_t cross_bytes = 0u;
            for (uint32_t index = 0u; index < count; ++index)
            {
                bytes[index] = m_sio2_rx.front();
                m_sio2_rx.pop_front();
                cross_bytes += m_sio2_rx_cross_tags.front();
                m_sio2_rx_cross_tags.pop_front();
            }
            if (count > block_bytes)
            {
                return reject(e_sio2_rejection::dma12_block_too_small);
            }
            if (!m_dma_cpu->dma_write(
                    m_sio2_dma[1u][0u] + block * block_bytes,
                    bytes.data(), count))
            {
                return reject(e_sio2_rejection::dma12_write);
            }
            m_sio2_pad0_cross_bytes_dma_written += cross_bytes;
        }
    }
    if (input_dma)
    {
        m_sio2_dma[0u][2u] &= ~k_spu2_dma_str_bit;
        (void)complete_dma_channel(11u);
    }
    if (output_dma)
    {
        m_sio2_dma[1u][2u] &= ~k_spu2_dma_str_bit;
        (void)complete_dma_channel(12u);
    }
    m_sio2_transfer_requested = false;
    ++m_sio2_transfer_count;
    // The SIO2 device's own completion latch is separate from the IOP
    // interrupt controller. The SIO2MAN callback at 0xbf808280 reads
    // this bit and writes the same value to clear it (W1C).
    m_sio2_intr |= 0x1u;
    (void)raise_interrupt(17u);
    return true;
}

bool c_hw::spu2_ram_word(uint32_t address, uint16_t* value) const
{
    return m_spu2.ram_word(address, value);
}

void c_hw::start_spu2_dma(uint32_t core)
{
    if (core >= 2u || m_dma_cpu == nullptr ||
        m_spu2_dma_remaining[core] != 0u || m_spu2_auto[core].m_active)
    {
        return;
    }
    const uint32_t madr = m_spu2_dma[core][0u];
    const uint32_t bcr = m_spu2_dma[core][1u];
    const uint32_t chcr = m_spu2_dma[core][2u];
    const uint64_t blocks = (bcr >> 16u) != 0u ?
        (bcr >> 16u) : 1u;
    const uint64_t words = static_cast<uint64_t>(bcr & 0xffffu) * blocks;
    const uint64_t bytes = words * 4u;
    const uint32_t physical = madr & 0x1fffffffu;
    const bool to_spu = (chcr & 1u) != 0u;
    const uint32_t channel = core == 0u ? 4u : 7u;

    // Preflight the complete guest range for either transfer mode.
    if (words == 0u || bytes > c_cpu::k_ram_size ||
        physical > c_cpu::k_ram_size ||
        bytes > c_cpu::k_ram_size - physical)
    {
        std::printf(
            "IOP SPU2 DMA deferred: channel=%u madr=0x%08x "
            "bcr=0x%08x chcr=0x%08x auto=%u "
            "(no data or completion fabricated)\n",
            channel, madr, bcr, chcr,
            (to_spu && m_spu2.auto_dma_enabled(core)) ? 1u : 0u);
        return;
    }

    if (to_spu && m_spu2.auto_dma_enabled(core))
    {
        m_spu2_auto[core] = {madr, static_cast<uint32_t>(bytes), true};
        ++m_spu2_auto_started;
        std::printf("IOP SPU2 AutoDMA started: channel=%u madr=0x%08x bytes=%u\n",
            channel, madr, static_cast<uint32_t>(bytes));
        feed_spu2_input(core);
        return;
    }

    std::vector<uint8_t> data(static_cast<size_t>(bytes), 0u);
    if (to_spu && !m_dma_cpu->dma_read(
            madr, data.data(), static_cast<uint32_t>(bytes)))
    {
        return;
    }
    const uint32_t prior_irq = m_spu2.irq_latches();
    if (!m_spu2.copy_dma(core, data, to_spu))
    {
        return;
    }
    if (!to_spu && !m_dma_cpu->dma_write(
            madr, data.data(), static_cast<uint32_t>(bytes)))
    {
        // The complete IOP RAM range was preflight above.
        return;
    }
    if ((m_spu2.irq_latches() & ~prior_irq) != 0u)
    {
        (void)raise_interrupt(c_spu2::k_irq_line);
    }

    // ps2-web-recomp/SDBZ completes ordinary SPU2 DMA after
    // 64 + words/4 IOP clocks. This is scheduled only once real
    // guest bytes have moved in the guest-requested direction.
    m_spu2_dma_remaining[core] =
        64u + static_cast<uint32_t>(words / 4u);
    std::printf(
        "IOP SPU2 DMA started: channel=%u madr=0x%08x "
        "bcr=0x%08x chcr=0x%08x bytes=%u delay=%u\n",
        channel, madr, bcr, chcr, static_cast<uint32_t>(bytes),
        m_spu2_dma_remaining[core]);
}

uint32_t c_hw::spu2_dma_refill_cycles_remaining() const
{
    // Ordinary DMA uses only two countdowns. The refill query is
    // needed solely for an active AutoDMA transfer with bytes left.
    uint32_t first = m_spu2_dma_remaining[0u];
    uint32_t second = m_spu2_dma_remaining[1u];
    if (m_spu2_auto[0u].m_active && m_spu2_auto[0u].m_bytes != 0u)
    {
        first = m_spu2.input_refill_cycles(0u);
    }
    if (m_spu2_auto[1u].m_active && m_spu2_auto[1u].m_bytes != 0u)
    {
        second = m_spu2.input_refill_cycles(1u);
    }
    return first == 0u ? second :
        (second == 0u ? first : std::min(first, second));
}

void c_hw::feed_spu2_input(uint32_t core)
{
    s_spu2_auto_transfer& transfer = m_spu2_auto[core];
    if (!transfer.m_active || transfer.m_bytes == 0u)
    {
        return;
    }
    while (transfer.m_bytes != 0u && m_spu2.input_frames(core) <= 256u)
    {
        uint8_t data[c_spu2::k_input_chunk_bytes];
        const uint32_t bytes = std::min(transfer.m_bytes, c_spu2::k_input_chunk_bytes);
        if (m_dma_cpu == nullptr || !m_dma_cpu->dma_read(transfer.m_address, data, bytes) ||
            !m_spu2.push_input(core, data, bytes))
        {
            // No fabricated completion if a real source read fails.
            transfer = {};
            return;
        }
        transfer.m_address += bytes;
        transfer.m_bytes -= bytes;
        m_spu2_auto_bytes += bytes;
    }
    if (transfer.m_bytes == 0u)
    {
        // The reference schedules DMA completion 32 IOP cycles after
        // the final input chunk is copied, not after playback drains.
        m_spu2_dma_remaining[core] = 32u;
    }
}

void c_hw::advance_spu2_devices(uint32_t cycles)
{
    // Split only while a real DMA/refill deadline exists. New
    // completions must not be charged cycles before the final refill.
    while (cycles != 0u)
    {
        const uint32_t deadline = spu2_dma_cycles_remaining();
        const uint32_t step = deadline != 0u ? std::min(cycles, deadline) : cycles;
        const uint64_t before = m_spu2.elapsed_samples();
        advance_spu2_dma(step);
        if (m_spu2.advance_cycles(step) != 0u)
        {
            (void)raise_interrupt(c_spu2::k_irq_line);
        }
        cycles -= step;
        if (m_spu2.elapsed_samples() / c_spu2::k_input_batch !=
            before / c_spu2::k_input_batch)
        {
            feed_spu2_input(0u);
            feed_spu2_input(1u);
        }
    }
}

void c_hw::advance_spu2_dma(uint32_t cycles)
{
    for (uint32_t core = 0u; core < 2u; ++core)
    {
        uint32_t& remaining = m_spu2_dma_remaining[core];
        if (remaining == 0u)
        {
            continue;
        }
        if ((m_spu2_dma[core][2u] & k_spu2_dma_str_bit) == 0u)
        {
            remaining = 0u;
            continue;
        }
        if (cycles < remaining)
        {
            remaining -= cycles;
            continue;
        }
        remaining = 0u;

        // A real sound-RAM copy completed and was not cancelled.
        // Only now may the guest see STR fall and the matching
        // DICR/DICR2 channel completion flag with shared IOP IRQ3.
        m_spu2_dma[core][2u] &= ~k_spu2_dma_str_bit;
        if (m_spu2_auto[core].m_active)
        {
            ++m_spu2_auto_completed;
            m_spu2_auto[core] = {};
        }
        const uint32_t channel = core == 0u ? 4u : 7u;
        const uint32_t index = core == 0u ? 4u : 0u;
        uint32_t& control = core == 0u ? m_dma_icr : m_dma_icr2;
        control |= uint32_t{1} << (24u + index);
        assert_dma_irq(control, false);
        (void)raise_interrupt(k_dma_irq);
        std::printf(
            "IOP SPU2 DMA completed: channel=%u chcr=0x%08x "
            "dicr=0x%08x i_stat=0x%08x i_mask=0x%08x "
            "i_ctrl=%u next_irq=%d\n",
            channel, m_spu2_dma[core][2u], control, m_i_stat,
            m_i_mask, m_i_ctrl, next_pending_interrupt());
    }
}

bool c_hw::scratchpad_range(
    uint32_t address, uint32_t width
) const
{
    if (width != 1u && width != 2u && width != 4u)
    {
        return false;
    }
    return address >= k_scratchpad_base &&
           static_cast<uint64_t>(address - k_scratchpad_base) +
               width <= k_scratchpad_size &&
           (address & (width - 1u)) == 0u;
}

bool c_hw::sif_dma_register(uint32_t address, uint32_t* channel,
                            uint32_t* reg) const
{
    if (channel == nullptr || reg == nullptr)
    {
        return false;
    }
    if (address >= k_sif0_dma_base &&
        address < k_sif0_dma_base + 16u &&
        (address & 3u) == 0u)
    {
        *channel = 0u;
        *reg = (address - k_sif0_dma_base) / 4u;
        return true;
    }
    if (address >= k_sif1_dma_base &&
        address < k_sif1_dma_base + 16u &&
        (address & 3u) == 0u)
    {
        *channel = 1u;
        *reg = (address - k_sif1_dma_base) / 4u;
        return true;
    }
    return false;
}

bool c_hw::spu2_dma_register(
    uint32_t address, uint32_t width, uint32_t* core,
    uint32_t* reg, uint32_t* shift
) const
{
    if (core == nullptr || reg == nullptr || shift == nullptr ||
        (width != 1u && width != 2u && width != 4u) ||
        (address & (width - 1u)) != 0u)
    {
        return false;
    }
    const uint32_t bases[2] = {
        k_spu2_dma4_base, k_spu2_dma7_base
    };
    for (uint32_t index = 0u; index < 2u; ++index)
    {
        if (address >= bases[index])
        {
            const uint32_t offset = address - bases[index];
            if (offset < k_spu2_dma_register_size &&
                width <= k_spu2_dma_register_size - offset &&
                width <= 4u - (offset & 3u))
            {
                *core = index;
                *reg = offset / 4u;
                *shift = (offset & 3u) * 8u;
                return true;
            }
        }
    }
    return false;
}

bool c_hw::read(
    uint32_t address, uint32_t width, uint32_t* value
)
{
    if (value == nullptr ||
        (width != 1u && width != 2u && width != 4u))
    {
        return false;
    }

    if (scratchpad_range(address, width))
    {
        const uint32_t offset = address - k_scratchpad_base;
        uint32_t data = 0u;
        for (uint32_t i = 0u; i < width; ++i)
        {
            data |= static_cast<uint32_t>(
                m_scratchpad[offset + i]
            ) << (8u * i);
        }
        *value = data;
        return true;
    }
    if (address >= c_spu2::k_base &&
        address < c_spu2::k_base + c_spu2::k_size)
    {
        return m_spu2.read(address, width, value);
    }
    // A physical CDVD drive with no media reports 0x00. The ROM0
    // diagnostic runner does not mount a disc. This deliberately does
    // not claim a successful CD read, device initialization, or SIF.
    // The physical controller is idle and ready to receive a command
    // after reset, independently of whether media is present.
    // Parameter and command writes are accepted; no disc producer
    // is attached, so commands remain pending without a fake IRQ.
    // CDVD interrupt reasons are distinct from the IOP I_STAT register.
    // Firmware can acknowledge only bits previously raised by genuine
    // CDVD hardware; this model has no CDVD event producer yet.
    if (address == k_cdvd_i_stat && width == 1u)
    {
        *value = m_cdvd_i_stat;
        return true;
    }
    if (address == k_cdvd_drive_status && width == 1u)
    {
        *value = k_cdvd_stopped;
        return true;
    }
    if (address == k_cdvd_dec_set && width == 1u)
    {
        *value = m_cdvd_dec_set;
        return true;
    }
    if (address == k_cdvd_ncmd_command && width == 1u)
    {
        *value = m_cdvd_ncmd_command;
        return true;
    }
    if (address == k_cdvd_ncmd_status && width == 1u)
    {
        *value = m_cdvd_ncmd_pending ?
            k_cdvd_ncmd_busy : k_cdvd_ncmd_ready;
        return true;
    }
    if (address == k_cdvd_disc_type && width == 1u)
    {
        *value = k_cdvd_no_disc;
        return true;
    }
    if (address == k_sio2_rx_fifo &&
        (width == 1u || width == 4u))
    {
        // Match ps2-web-recomp/SDBZ SIO2 read(0x1f808264):
        // an empty RX FIFO reads as 0xff, not an unsupported MMIO
        // operation. This is open-bus/empty-FIFO data, not a controller
        // response: no bytes are queued and no IRQ/DMA is triggered.
        if (m_sio2_rx.empty())
        {
            ++m_sio2_empty_rx_read_count;
            *value = 0xffu;
        }
        else
        {
            *value = m_sio2_rx.front();
            if (m_sio2_rx_cross_tags.front() != 0u && pad_trace_window() &&
                m_dma_cpu != nullptr)
            {
                std::fprintf(stderr,
                    "PAD PIO ee_completed=%llu iop_cpu_cycle=%llu pc=%08x ra=%08x byte=%02x\n",
                    static_cast<unsigned long long>(m_pad_trace_dispatch),
                    static_cast<unsigned long long>(m_dma_cpu->cycles()),
                    m_dma_cpu->state().m_pc, m_dma_cpu->state().m_gpr[31], *value);
            }
            m_sio2_rx.pop_front();
            m_sio2_pad0_cross_bytes_pio_read +=
                m_sio2_rx_cross_tags.front();
            m_sio2_rx_cross_tags.pop_front();
        }
        return true;
    }

    // The actual LIBSD DMA4.BCR path uses SH, and the
    // ps2-web-recomp/SDBZ hardware model extracts subword lanes.
    // Only these two known DMA4/DMA7 register banks are relaxed.
    uint32_t spu2_core = 0u;
    uint32_t spu2_reg = 0u;
    uint32_t spu2_shift = 0u;
    if (spu2_dma_register(
            address, width, &spu2_core, &spu2_reg, &spu2_shift))
    {
        const uint32_t mask = width == 4u ?
            0xffffffffu : ((uint32_t{1} << (width * 8u)) - 1u);
        *value = (m_spu2_dma[spu2_core][spu2_reg] >>
                  spu2_shift) & mask;
        return true;
    }

    if (width != 4u || (address & 3u) != 0u)
    {
        return false;
    }
    if (address >= k_sio2_send3_base &&
        address < k_sio2_send3_base + 0x40u)
    {
        *value = m_sio2_send3[(address - k_sio2_send3_base) / 4u];
        return true;
    }
    if (address >= k_sio2_port_base &&
        address < k_sio2_port_base + 0x20u)
    {
        *value = m_sio2_port[(address - k_sio2_port_base) / 4u];
        return true;
    }

    if (address >= k_cdrom_dma_base &&
        address < k_cdrom_dma_base + 12u)
    {
        *value = m_cdrom_dma[(address - k_cdrom_dma_base) / 4u];
        return true;
    }


    for (uint32_t index = 0u; index < 2u; ++index)
    {
        const uint32_t base = index == 0u
            ? k_sio2_dma_rx_base : k_sio2_dma_tx_base;
        if (address >= base && address < base + 12u)
        {
            *value = m_sio2_dma[index][(address - base) / 4u];
            return true;
        }
    }

    uint32_t channel = 0u;
    uint32_t reg = 0u;
    if (sif_dma_register(address, &channel, &reg))
    {
        *value = m_sif_dma[channel][reg];
        return true;
    }

    if (m_sif_mmio_user != nullptr &&
        (address == k_sif_mscom || address == k_sif_smcom ||
         address == k_sif_msflag || address == k_sif_smflag))
    {
        return m_sif_mmio_read(m_sif_mmio_user, address, width, value);
    }
    switch (address)
    {
    case k_ssbus_spu2_addr:
        *value = m_ssbus_spu2_addr;
        return true;
    case k_ssbus_spu2_addr1:
        *value = m_ssbus_spu2_addr1;
        return true;
    case k_ssbus_spu2_delay:
        *value = m_ssbus_spu2_delay;
        return true;
    case k_ssbus_spu2_delay1:
        *value = m_ssbus_spu2_delay1;
        return true;
    case k_sif_mscom:
        *value = m_sif_mscom;
        return true;
    case k_sif_smcom:
        *value = m_sif_smcom;
        return true;
    case k_sif_msflag:
        *value = m_sif_msflag;
        return true;
    case k_sif_smflag:
        *value = m_sif_smflag;
        return true;
    case k_dma_pcr:
        *value = m_dma_pcr;
        return true;
    case k_dma_pcr2:
        *value = m_dma_pcr2;
        return true;
    case k_dma_global_enable:
        *value = m_dma_global_enable;
        return true;
    case k_dma_icr:
        *value = m_dma_icr;
        return true;
    case k_dma_icr2:
        *value = m_dma_icr2;
        return true;
    case k_timer5_count:
        *value = m_timer5_count;
        return true;
    case k_timer5_mode:
        *value = m_timer5_mode | (m_timer5_irq_armed ? 0x400u : 0u);
        // Counter target/overflow status bits are cleared on read.
        m_timer5_mode &= ~0x1800u;
        return true;
    case k_timer5_target:
        *value = m_timer5_target;
        return true;
    case k_i_stat:
        *value = m_i_stat;
        return true;
    case k_i_mask:
        *value = m_i_mask;
        return true;
    case k_i_ctrl:
        // On the IOP, reading I_CTRL disables the global interrupt
        // enable until the guest writes it back.
        *value = m_i_ctrl;
        m_i_ctrl = 0u;
        return true;
    case k_cache_control:
        // Register storage only. The emulator does not yet model
        // I-cache/D-cache isolation or instruction fetch coherence.
        *value = m_cache_control;
        return true;
    case k_sio2_ctrl:
        // Match ps2-web-recomp/SDBZ: CTRL reads return zero rather
        // than echoing the last write. Original SIO2MAN performs
        // reset (CTRL | 0x0c), sends PIO data, then START (CTRL | 1).
        // Echoing the reset write turns that START into 0x3bd,
        // discarding the real pad replies immediately before recv_td.
        // Keep m_sio2_ctrl for host-only last-write diagnostics.
        *value = 0u;
        return true;
    case k_sio2_stat:
        *value = m_sio2_stat;
        return true;
    case k_sio2_port_status:
        *value = 0x0fu; // Port register mask, not pad readiness.
        return true;
    case k_sio2_aux_status:
        *value = 0u;
        return true;
    case k_sio2_intr:
        *value = m_sio2_intr;
        return true;
    default:
        return false;
    }
}

bool c_hw::write(
    uint32_t address, uint32_t width, uint32_t value
)
{
    if (width != 1u && width != 2u && width != 4u)
    {
        return false;
    }
    if (scratchpad_range(address, width))
    {
        const uint32_t offset = address - k_scratchpad_base;
        for (uint32_t i = 0u; i < width; ++i)
        {
            m_scratchpad[offset + i] =
                static_cast<uint8_t>(value >> (8u * i));
        }
        return true;
    }
    if (address >= c_spu2::k_base &&
        address < c_spu2::k_base + c_spu2::k_size)
    {
        const uint32_t prior_irq = m_spu2.irq_latches();
        if (!m_spu2.write(address, width, value))
        {
            return false;
        }
        // Only a real data-port access to the guest-selected IRQA
        // can assert IRQ9. The SPU2 IRQ latch is edge-triggered here:
        // an unrelated register write must not reassert an IRQ the
        // guest already acknowledged at I_STAT.
        if ((m_spu2.irq_latches() & ~prior_irq) != 0u)
        {
            (void)raise_interrupt(c_spu2::k_irq_line);
        }
        return true;
    }
    // OpenPS2BIOS writes individual characters here. This address is
    // a debug-output convention in emulators, not a PS2 readiness flag.
    // Accept only the observed byte-store operation; fail other MMIO.
    if (address == k_cdvd_i_stat && width == 1u)
    {
        // CDVD I_STAT is W1C. An ack does not generate an IOP IRQ
        // or mark any CDVD operation as completed.
        m_cdvd_i_stat &= static_cast<uint8_t>(~value);
        return true;
    }
    if (address == k_cdvd_dec_set && width == 1u)
    {
        // Firmware configures the CDVD decryption mode here. Retain
        // the byte until reset; no sector processing is implied.
        m_cdvd_dec_set = static_cast<uint8_t>(value);
        return true;
    }
    if (address == k_cdvd_ncmd_status && width == 1u)
    {
        // This physical address is N_READY when read, N_DATAIN when
        // written. Keep the original ordered parameters for CDVD.
        if (m_cdvd_ncmd_pending ||
            m_cdvd_ncmd_param_count >= k_cdvd_ncmd_param_capacity)
        {
            return false;
        }
        m_cdvd_ncmd_params[m_cdvd_ncmd_param_count++] =
            static_cast<uint8_t>(value);
        return true;
    }
    if (address == k_cdvd_ncmd_command && width == 1u)
    {
        if (m_cdvd_ncmd_pending)
        {
            return false;
        }
        m_cdvd_ncmd_command = static_cast<uint8_t>(value);
        m_cdvd_ncmd_pending = true;
        // An actual sector provider must consume/complete this request.
        // Do not supply empty sector data or clear DMA3 STR here.
        if (m_cdvd_start != nullptr)
        {
            m_cdvd_start(m_cdvd_user);
        }
        return true;
    }
    if (address == k_debug_console && width == 1u)
    {
        if (m_debug_console.size() < k_debug_console_capacity)
        {
            m_debug_console.push_back(static_cast<char>(value & 0xffu));
        }
        else
        {
            m_debug_console_truncated = true;
        }
        return true;
    }
    // A transmitted byte can finish the guest's SEND3 packet. The
    // neutral controller responds only when a live HLE CPU is attached.
    if (address == k_sio2_tx_fifo &&
        (width == 1u || width == 4u))
    {
        if (m_sio2_pending_tx.size() >= k_sio2_tx_limit)
        {
            return false;
        }
        m_sio2_pending_tx.push_back(static_cast<uint8_t>(value));
        (void)process_sio2_packet();
        return true;
    }

    uint32_t spu2_core = 0u;
    uint32_t spu2_reg = 0u;
    uint32_t spu2_shift = 0u;
    if (spu2_dma_register(
            address, width, &spu2_core, &spu2_reg, &spu2_shift))
    {
        // Retain the untouched bits of MADR, BCR, CHCR or TADR.
        // This is the primary reference's little-endian subword
        // merge, including 0xBF8010C4 and its upper half at 0xC6.
        const uint32_t lane_mask = width == 4u ?
            0xffffffffu : ((uint32_t{1} << (width * 8u)) - 1u);
        const uint32_t mask = lane_mask << spu2_shift;
        uint32_t& word = m_spu2_dma[spu2_core][spu2_reg];
        const uint32_t previous = word;
        word = (word & ~mask) | ((value << spu2_shift) & mask);
        if (spu2_reg == 2u && (mask & k_spu2_dma_str_bit) != 0u)
        {
            if ((word & k_spu2_dma_str_bit) == 0u)
            {
                // Driver cancellation must never complete a stopped
                // DMA or set a phantom DICR interrupt flag.
                m_spu2_dma_remaining[spu2_core] = 0u;
                m_spu2_auto[spu2_core] = {};
            }
            else if ((previous & k_spu2_dma_str_bit) == 0u)
            {
                start_spu2_dma(spu2_core);
            }
        }
        return true;
    }

    if (width != 4u || (address & 3u) != 0u)
    {
        return false;
    }
    if (address >= k_sio2_send3_base &&
        address < k_sio2_send3_base + 0x40u)
    {
        m_sio2_send3[(address - k_sio2_send3_base) / 4u] = value;
        return true;
    }
    if (address >= k_sio2_port_base &&
        address < k_sio2_port_base + 0x20u)
    {
        m_sio2_port[(address - k_sio2_port_base) / 4u] = value;
        return true;
    }

    if (address >= k_cdrom_dma_base &&
        address < k_cdrom_dma_base + 12u)
    {
        const uint32_t reg = (address - k_cdrom_dma_base) / 4u;
        // Programming CHCR.STR requests CD-ROM DMA3. In the
        // absence of a disc/data producer, retain the requested
        // state (including STR) without transferring bytes, clearing
        // STR, asserting an IRQ or claiming command completion.
        // A future disc transport must explicitly complete it.
        m_cdrom_dma[reg] = value;
        if (reg == 2u && (value & 0x01000000u) != 0u &&
            m_cdvd_start != nullptr)
        {
            m_cdvd_start(m_cdvd_user);
        }
        return true;
    }

    for (uint32_t index = 0u; index < 2u; ++index)
    {
        const uint32_t base = index == 0u
            ? k_sio2_dma_rx_base : k_sio2_dma_tx_base;
        if (address >= base && address < base + 12u)
        {
            // DMA11 and DMA12 are armed by CHCR.STR but can only
            // complete through a real SIO2 CTRL.start transaction.
            m_sio2_dma[index][(address - base) / 4u] = value;
            return true;
        }
    }

    uint32_t channel = 0u;
    uint32_t reg = 0u;
    if (sif_dma_register(address, &channel, &reg))
    {
        // CHCR.STR is guest-controlled when starting/stopping DMA, and
        // is NOT cleared merely because the register write succeeded.
        // The transfer backend will clear STR on real completion.
        const bool was_started =
            (m_sif_dma[channel][2u] & 0x01000000u) != 0u;
        m_sif_dma[channel][reg] = value;
        if (reg == 2u && !was_started &&
            (value & 0x01000000u) != 0u)
        {
            if (channel == 0u && m_sif0_start != nullptr)
            {
                m_sif0_start(m_sif0_user);
            }
            else if (channel == 1u && m_sif1_start != nullptr)
            {
                m_sif1_start(m_sif1_user);
            }
        }
        return true;
    }

    if (m_sif_mmio_user != nullptr &&
        (address == k_sif_mscom || address == k_sif_smcom ||
         address == k_sif_msflag || address == k_sif_smflag))
    {
        return m_sif_mmio_write(m_sif_mmio_user, address, width, value);
    }
    switch (address)
    {
    case k_ssbus_spu2_addr:
        m_ssbus_spu2_addr = value;
        return true;
    case k_ssbus_spu2_addr1:
        m_ssbus_spu2_addr1 = value;
        return true;
    case k_ssbus_spu2_delay:
        m_ssbus_spu2_delay = value;
        return true;
    case k_ssbus_spu2_delay1:
        m_ssbus_spu2_delay1 = value;
        return true;
    case k_sif_smcom:
        m_sif_smcom = value;
        return true;
    case k_sif_msflag:
        // IOP writes acknowledge bits asserted by the EE.
        m_sif_msflag &= ~value;
        return true;
    case k_sif_smflag:
        // Only the guest IOP announces its own readiness. The EE
        // independently acknowledges flags when it processes them.
        m_sif_smflag |= value;
        return true;
    case k_dma_pcr:
        // Each DMA channel's priority and enable nibble is writable,
        // but programming it does not launch or complete a transfer.
        m_dma_pcr = value;
        return true;
    case k_dma_pcr2:
        m_dma_pcr2 = value;
        return true;
    case k_dma_global_enable:
        // This permits an explicitly started secondary DMA controller,
        // but never asserts a DMA completion or a SIF handshake.
        m_dma_global_enable = value;
        return true;
    case k_dma_icr:
        m_dma_icr = update_dma_icr(m_dma_icr, value);
        return true;
    case k_dma_icr2:
        m_dma_icr2 = update_dma_icr(m_dma_icr2, value);
        return true;
    case k_timer5_count:
        m_timer5_count = value;
        m_timer5_remainder = 0u;
        return true;
    case k_timer5_target:
        m_timer5_target = value;
        return true;
    case k_timer5_mode:
        // Only accept the supported counter-clock, target-reset/IRQ,
        // repeat, non-toggle mode, plus the disabled interrupt mode.
        // Unknown gate/source/overflow configurations must not run
        // with invented counter behaviour.
        if ((value & ~0x6058u) != 0u ||
            (value & 0x0058u) != 0x0058u)
        {
            // Also permit mode 0 to reset an unconfigured counter.
            if (value != 0u)
            {
                return false;
            }
        }
        m_timer5_mode = value & 0x6058u;
        m_timer5_count = 0u;
        m_timer5_remainder = 0u;
        m_timer5_irq_armed = true;
        return true;
    case k_i_stat:
        // Write-one keeps the IRQ; write-zero acknowledges it.
        m_i_stat &= value;
        return true;
    case k_i_mask:
        m_i_mask = value;
        return true;
    case k_i_ctrl:
        m_i_ctrl = value & 1u;
        return true;
    case k_cache_control:
        m_cache_control = value;
        return true;
    case k_sio2_intr:
        // SIO2 interrupt status bits 0/1 are write-one-to-clear.
        // Do not clear the independent IOP INTC I_STAT bit here:
        // INTRMAN acknowledges that interrupt separately.
        m_sio2_intr &= ~(value & 0x3u);
        return true;
    case k_sio2_ctrl:
        m_sio2_ctrl = value;
        if ((value & 0x0cu) == 0x0cu)
        {
            m_sio2_index = 0u;
            m_sio2_pending_tx.clear();
            m_sio2_pad0_cross_bytes_fifo_discarded +=
                static_cast<uint32_t>(std::count(
                    m_sio2_rx_cross_tags.begin(),
                    m_sio2_rx_cross_tags.end(), uint8_t{1u}));
            m_sio2_rx.clear();
            m_sio2_rx_cross_tags.clear();
            m_sio2_transfer_requested = false;
        }
        if ((value & 1u) != 0u)
        {
            ++m_sio2_ctrl_start_count;
            m_sio2_transfer_requested = true;
            m_sio2_last_start_guest_pc = m_dma_cpu != nullptr
                ? m_dma_cpu->state().m_pc : 0u;
            m_sio2_last_start_guest_ra = m_dma_cpu != nullptr
                ? m_dma_cpu->state().m_gpr[31u] : 0u;
            // Capture packet order *at* the guest's CTRL.start. Final
            // packet counts alone cannot show whether an incomplete
            // PIO packet was transmitted after its first START.
            m_sio2_packets_at_last_start = m_sio2_packet_count;
            m_sio2_tx_bytes_at_last_start = m_sio2_pending_tx.size();
            m_sio2_rx_bytes_at_last_start = m_sio2_rx.size();
            (void)run_sio2_transfer();
        }
        return true;
    default:
        return false;
    }
}

void c_hw::ee_write_mscom(uint32_t value)
{
    m_sif_mscom = value;
}

void c_hw::ee_set_msflag(uint32_t bits)
{
    m_sif_msflag |= bits;
}

void c_hw::ee_clear_smflag(uint32_t bits)
{
    m_sif_smflag &= ~bits;
}

uint32_t c_hw::sif_smcom() const
{
    uint32_t value = m_sif_smcom;
    if (m_sif_mmio_user != nullptr)
    {
        (void)m_sif_mmio_read(
            m_sif_mmio_user, k_sif_smcom, 4u, &value
        );
    }
    return value;
}

uint32_t c_hw::sif_smflag() const
{
    uint32_t value = m_sif_smflag;
    if (m_sif_mmio_user != nullptr)
    {
        (void)m_sif_mmio_read(
            m_sif_mmio_user, k_sif_smflag, 4u, &value
        );
    }
    return value;
}

void c_hw::assert_dma_irq(uint32_t& control, bool sif_channel)
{
    const bool force = (control & 0x00008000u) != 0u;
    const bool enabled = (control & 0x00800000u) != 0u;
    const bool channel_pending =
        (((control >> 8u) & control) & 0x007f0000u) != 0u;
    if (force || (enabled && channel_pending))
    {
        control |= 0x80000000u;
    }
    else
    {
        control &= ~0x80000000u;
    }
    if (sif_channel || (control & 0x80000000u) != 0u)
    {
        (void)raise_interrupt(k_dma_irq);
    }
}

uint32_t c_hw::update_dma_icr(uint32_t previous, uint32_t value)
{
    // Low 24 bits are writable control; pending bits 24..30 are
    // hardware flags acknowledged by write-one-to-clear. Bit 31
    // reflects the combined force/master/channel interrupt state.
    uint32_t result =
        (previous & 0x7f000000u) | (value & 0x00ffffffu);
    result &= ~(value & 0x7f000000u);
    assert_dma_irq(result, false);
    return result;
}

bool c_hw::complete_dma_channel(uint32_t channel)
{
    if (channel > 12u)
    {
        return false;
    }
    const bool secondary = channel >= 7u;
    const uint32_t index = secondary ? channel - 7u : channel;
    if (channel == 9u || channel == 10u)
    {
        // CHCR.STR clears only at a real completion supplied by the
        // DMA engine. Do not move bytes or synthesize a transfer here.
        m_sif_dma[channel - 9u][2u] &= ~0x01000000u;
    }
    uint32_t& control = secondary ? m_dma_icr2 : m_dma_icr;
    const bool sif_channel = secondary && (index == 2u || index == 3u);
    const uint32_t enable = uint32_t{1} << (16u + index);
    // PCSX2 models SIF0/SIF1 end/tag interrupts independently of
    // the per-channel mask; they do not fabricate any DMA completion.
    if ((control & enable) != 0u || sif_channel)
    {
        control |= uint32_t{1} << (24u + index);
        assert_dma_irq(control, sif_channel);
    }
    return true;
}

bool c_hw::raise_interrupt(uint32_t line)
{
    if (line >= 32u)
    {
        return false;
    }
    m_i_stat |= uint32_t{1} << line;
    return true;
}

bool c_hw::supports_interrupt_line(uint32_t line)
{
    return line < 39u || (line >= 40u && line < 46u);
}

bool c_hw::set_interrupt_mask(uint32_t line, bool enabled)
{
    if (!supports_interrupt_line(line))
    {
        return false;
    }
    if (line < 32u)
    {
        const uint32_t bit = uint32_t{1} << line;
        if (enabled)
        {
            m_i_mask |= bit;
        }
        else
        {
            m_i_mask &= ~bit;
        }
        return true;
    }

    // Primary channels 0..6 are INTRMAN lines 32..38. Secondary
    // channels 7..12 are lines 40..45; line 39 is not a channel.
    // Match ps2-web-recomp/SDBZ hw::set_interrupt_mask().
    const bool secondary = line >= 40u;
    uint32_t& control = secondary ? m_dma_icr2 : m_dma_icr;
    const uint32_t channel = secondary ? line - 40u : line - 32u;
    const uint32_t bit = uint32_t{1} << (16u + channel);
    if (enabled)
    {
        // DMA IRQs arrive through the ordinary IOP IRQ3 aggregator.
        // No new request or interrupt is generated by this enable.
        m_i_mask |= uint32_t{1} << k_dma_irq;
        m_dma_icr |= 0x00800000u;
        control |= bit;
    }
    else
    {
        // Disabling a channel does not acknowledge pending DMA flags,
        // clear the shared IRQ3 mask or stop a running DMA transfer.
        control &= ~bit;
    }
    assert_dma_irq(control, false);
    return true;
}

int32_t c_hw::resolve_pending_interrupt(uint32_t pending) const
{
    if ((pending & (1u << k_dma_irq)) != 0u)
    {
        const uint32_t primary =
            (m_dma_icr >> 24u) & (m_dma_icr >> 16u) & 0x7fu;
        const uint32_t secondary =
            (m_dma_icr2 >> 24u) & (m_dma_icr2 >> 16u) & 0x3fu;
        for (uint32_t index = 0u; index < 7u; ++index)
        {
            if ((primary & (1u << index)) != 0u)
            {
                return static_cast<int32_t>(32u + index);
            }
        }
        for (uint32_t index = 0u; index < 6u; ++index)
        {
            if ((secondary & (1u << index)) != 0u)
            {
                return static_cast<int32_t>(40u + index);
            }
        }
    }
    const uint32_t ordinary = pending & ~(1u << k_dma_irq);
    if (ordinary == 0u)
    {
        return -1;
    }
    for (uint32_t line = 0u; line < 32u; ++line)
    {
        if ((ordinary & (1u << line)) != 0u)
        {
            return static_cast<int32_t>(line);
        }
    }
    return -1;
}

bool c_hw::acknowledge_interrupt(uint32_t line)
{
    if (line < 32u)
    {
        m_i_stat &= ~(1u << line);
        return true;
    }
    if (!supports_interrupt_line(line))
    {
        return false;
    }
    const bool secondary = line >= 40u;
    uint32_t& control = secondary ? m_dma_icr2 : m_dma_icr;
    const uint32_t index = secondary ? line - 40u : line - 32u;
    control &= ~(1u << (24u + index));
    if (((m_dma_icr >> 24u) & 0x7fu) == 0u &&
        ((m_dma_icr2 >> 24u) & 0x3fu) == 0u)
    {
        m_i_stat &= ~(1u << k_dma_irq);
    }
    return true;
}

uint32_t c_hw::cache_control() const
{
    return m_cache_control;
}

uint64_t c_hw::idle_event_cycles_remaining() const
{
    constexpr uint64_t none = UINT64_MAX;
    if (pending_interrupts() != 0u || m_cdvd_ncmd_pending)
    {
        return 0u;
    }
    // AutoDMA may refill at a sample batch boundary. Do not attempt
    // to look past its externally initiated transfer state.
    if (m_spu2_auto[0u].m_active || m_spu2_auto[1u].m_active)
    {
        return 0u;
    }
    uint64_t next = none;
    // AutoDMA was rejected above. Only the ordinary two countdowns
    // can contribute here; no input-refill query or generic DMA scan.
    const uint32_t dma0 = m_spu2_dma_remaining[0u];
    const uint32_t dma1 = m_spu2_dma_remaining[1u];
    if (dma0 != 0u)
    {
        next = dma0;
    }
    if (dma1 != 0u)
    {
        next = std::min(next, static_cast<uint64_t>(dma1));
    }
    // Even with IRQs disabled the mixer can write guest sound RAM.
    // Stop at the next sample, not only at a predicted voice IRQ.
    if (m_spu2.clock_active())
    {
        next = std::min(next, static_cast<uint64_t>(
            c_spu2::k_cycles_per_sample - m_spu2.sample_phase()));
    }
    // Mode zero has a unit divisor and cannot compare or raise an IRQ.
    // Mode/count writes reset the prescaler remainder.
    if (m_timer5_mode == 0u)
    {
        return std::min(next, (uint64_t{1u} << 32u) - m_timer5_count);
    }
    // Mirror the modeled Timer5 compare condition. A counter already
    // past target must reach rollover before a later compare is possible.
    // Stop at rollover as well; partitioning across it is not assumed
    // equivalent to one large advance in the current device model.
    constexpr uint32_t shifts[4] = {0u, 3u, 4u, 8u};
    const uint32_t shift = shifts[(m_timer5_mode >> 13u) & 3u];
    uint64_t ticks = (uint64_t{1u} << 32u) - m_timer5_count;
    if ((m_timer5_mode & 0x08u) != 0u &&
        m_timer5_target != 0u && m_timer5_count < m_timer5_target)
    {
        ticks = m_timer5_target - m_timer5_count;
    }
    next = std::min(next, (ticks << shift) - m_timer5_remainder);
    return next;
}

void c_hw::advance_cycles(uint32_t cycles)
{
    if (cycles == 0u)
    {
        return;
    }
    if (m_spu2_auto[0u].m_active || m_spu2_auto[1u].m_active)
    {
        advance_spu2_devices(cycles);
    }
    else
    {
        if ((m_spu2_dma_remaining[0u] | m_spu2_dma_remaining[1u]) != 0u)
        {
            advance_spu2_dma(cycles);
        }
        if (m_spu2.advance_cycles(cycles) != 0u)
        {
            (void)raise_interrupt(c_spu2::k_irq_line);
        }
    }
    // The unconfigured unit-rate counter only wraps modulo 2^32.
    // No prescaler remainder or compare state can survive a mode-zero write.
    if (m_timer5_mode == 0u)
    {
        m_timer5_count += cycles;
        return;
    }
    // All Timer 5 divisors are powers of two. Keep the exact quotient
    // and fractional remainder without a variable 64-bit division.
    constexpr uint32_t shifts[4] = {0u, 3u, 4u, 8u};
    const uint32_t shift = shifts[(m_timer5_mode >> 13u) & 3u];
    const uint64_t accumulated =
        static_cast<uint64_t>(m_timer5_remainder) + cycles;
    const uint64_t elapsed = accumulated >> shift;
    m_timer5_remainder = static_cast<uint32_t>(
        accumulated & ((uint64_t{1u} << shift) - 1u));
    if (elapsed == 0u)
    {
        return;
    }

    const uint64_t count = static_cast<uint64_t>(m_timer5_count) + elapsed;
    if ((m_timer5_mode & 0x08u) != 0u &&
        m_timer5_target != 0u &&
        m_timer5_count < m_timer5_target &&
        count >= m_timer5_target)
    {
        const uint64_t over = count - m_timer5_target;
        m_timer5_count = static_cast<uint32_t>(
            over % m_timer5_target
        );
        m_timer5_mode |= 0x0800u;
        if ((m_timer5_mode & 0x10u) != 0u && m_timer5_irq_armed)
        {
            (void)raise_interrupt(k_timer5_irq);
            if ((m_timer5_mode & 0x40u) == 0u)
            {
                m_timer5_irq_armed = false;
            }
        }
    }
    else
    {
        // Unconfigured/free-running timer, or a counter past its
        // target: normal 32-bit rollover without an invented IRQ.
        m_timer5_count = static_cast<uint32_t>(count);
    }
}

bool c_hw::cdvd_ncmd_pending() const
{
    return m_cdvd_ncmd_pending;
}

uint8_t c_hw::cdvd_ncmd_command() const
{
    return m_cdvd_ncmd_command;
}

uint32_t c_hw::cdvd_ncmd_parameter_count() const
{
    return m_cdvd_ncmd_param_count;
}

bool c_hw::cdvd_ncmd_parameter(uint32_t index, uint8_t* value) const
{
    if (value == nullptr || index >= m_cdvd_ncmd_param_count)
    {
        return false;
    }
    *value = m_cdvd_ncmd_params[index];
    return true;
}

bool c_hw::cdrom_dma_register(uint32_t index, uint32_t* value) const
{
    if (value == nullptr || index >= 3u)
    {
        return false;
    }
    *value = m_cdrom_dma[index];
    return true;
}

size_t c_hw::sio2_pending_tx_bytes() const
{
    return m_sio2_pending_tx.size();
}

bool c_hw::sio2_tx_byte(size_t index, uint8_t* value) const
{
    if (value == nullptr || index >= m_sio2_pending_tx.size())
    {
        return false;
    }
    *value = m_sio2_pending_tx[index];
    return true;
}

bool c_hw::sio2_transfer_requested() const
{
    return m_sio2_transfer_requested;
}

uint32_t c_hw::sio2_ctrl_starts() const
{
    return m_sio2_ctrl_start_count;
}

uint32_t c_hw::sio2_packets() const
{
    return m_sio2_packet_count;
}

uint32_t c_hw::sio2_completed_transfers() const
{
    return m_sio2_transfer_count;
}

c_hw::s_sio2_diagnostics c_hw::sio2_diagnostics() const
{
    s_sio2_diagnostics result{};
    result.m_ctrl = m_sio2_ctrl;
    result.m_stat = m_sio2_stat;
    result.m_intr = m_sio2_intr;
    result.m_packet_index = m_sio2_index;
    result.m_last_start_guest_pc = m_sio2_last_start_guest_pc;
    result.m_last_start_guest_ra = m_sio2_last_start_guest_ra;
    result.m_packets_at_last_start = m_sio2_packets_at_last_start;
    result.m_tx_bytes_at_last_start = m_sio2_tx_bytes_at_last_start;
    result.m_rx_bytes_at_last_start = m_sio2_rx_bytes_at_last_start;
    result.m_first_packet_count = m_sio2_packet_count < 4u
        ? m_sio2_packet_count : 4u;
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        result.m_first_packets[index] = m_sio2_first_packets[index];
        result.m_send3[index] = m_sio2_send3[index];
    }
    for (uint32_t index = 0u; index < 3u; ++index)
    {
        result.m_dma11[index] = m_sio2_dma[0u][index];
        result.m_dma12[index] = m_sio2_dma[1u][index];
    }
    result.m_tx_bytes = m_sio2_pending_tx.size();
    result.m_rx_bytes = m_sio2_rx.size();
    result.m_empty_rx_reads = m_sio2_empty_rx_read_count;
    result.m_pad0_polls = m_sio2_pad0_poll_count;
    result.m_pad0_cross_pressed_polls =
        m_sio2_pad0_cross_pressed_polls;
    result.m_pad0_cross_first_poll =
        m_sio2_pad0_cross_first_poll;
    result.m_pad0_cross_poll_count =
        m_sio2_pad0_cross_poll_count;
    result.m_pad0_cross_bytes_pio_read =
        m_sio2_pad0_cross_bytes_pio_read;
    result.m_pad0_cross_bytes_dma_written =
        m_sio2_pad0_cross_bytes_dma_written;
    result.m_pad0_cross_bytes_fifo_discarded =
        m_sio2_pad0_cross_bytes_fifo_discarded;
    result.m_last_rejection = m_sio2_last_rejection;
    result.m_rejected_starts = m_sio2_rejected_start_count;
    return result;
}

const char* c_hw::sio2_rejection_name() const
{
    switch (m_sio2_last_rejection)
    {
    case e_sio2_rejection::none: return "none";
    case e_sio2_rejection::cpu_unavailable: return "cpu_unavailable";
    case e_sio2_rejection::dma11_geometry: return "dma11_geometry";
    case e_sio2_rejection::dma11_source: return "dma11_source";
    case e_sio2_rejection::dma12_geometry: return "dma12_geometry";
    case e_sio2_rejection::dma12_destination: return "dma12_destination";
    case e_sio2_rejection::packet_incomplete: return "packet_incomplete";
    case e_sio2_rejection::no_processed_packet: return "no_processed_packet";
    case e_sio2_rejection::dma12_packet_count: return "dma12_packet_count";
    case e_sio2_rejection::dma12_reply_underflow: return "dma12_reply_underflow";
    case e_sio2_rejection::dma12_block_too_small: return "dma12_block_too_small";
    case e_sio2_rejection::dma12_write: return "dma12_write";
    default: return "unknown";
    }
}

bool c_hw::complete_cdvd_read(uint8_t interrupt_reason)
{
    if (!m_cdvd_ncmd_pending || m_cdvd_ncmd_command != 0x06u ||
        (m_cdrom_dma[2u] & 0x01000000u) == 0u)
    {
        return false;
    }
    m_cdrom_dma[2u] &= ~0x01000000u;
    m_cdvd_ncmd_pending = false;
    m_cdvd_ncmd_param_count = 0u;
    m_cdvd_i_stat |= interrupt_reason;
    (void)complete_dma_channel(3u);
    (void)raise_interrupt(2u);
    return true;
}

const std::string& c_hw::debug_console() const
{
    return m_debug_console;
}

bool c_hw::debug_console_truncated() const
{
    return m_debug_console_truncated;
}
} // namespace iop
