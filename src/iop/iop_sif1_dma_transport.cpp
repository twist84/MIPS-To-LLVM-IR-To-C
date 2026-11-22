#include "iop_sif1_dma_transport.h"

#include <vector>

namespace iop
{
c_sif1_dma_transport::c_sif1_dma_transport(
    ee::ee_memory& ee_ram, ee::c_ee_dmac& ee_dmac,
    c_cpu& iop_cpu, c_hw& iop_hw
)
    : m_ee_ram(ee_ram), m_ee_dmac(ee_dmac),
      m_iop_cpu(iop_cpu), m_iop_hw(iop_hw)
{
    reset();
    m_ee_dmac.bind_sif1_start(this, &start_callback);
    m_iop_hw.bind_sif1_start(this, &start_callback);
}

c_sif1_dma_transport::~c_sif1_dma_transport()
{
    m_ee_dmac.bind_sif1_start(nullptr, nullptr);
    m_iop_hw.bind_sif1_start(nullptr, nullptr);
}

void c_sif1_dma_transport::reset()
{
    m_fifo.fill(0u);
    m_head = 0u;
    m_count = 0u;
    m_iop_destination = 0u;
    m_iop_words_remaining = 0u;
    m_iop_final_tag = false;
    m_in_pump = false;
    m_faulted = false;
    m_queued_bytes = 0u;
    m_delivered_bytes = 0u;
    m_next_dma_id = 1u;
}

void c_sif1_dma_transport::start_callback(void* user)
{
    if (user != nullptr)
    {
        static_cast<c_sif1_dma_transport*>(user)->pump();
    }
}

bool c_sif1_dma_transport::read_ee_register(
    uint32_t offset, uint32_t* result)
{
    ee::s_ee_u128 word{};
    const bool valid = result != nullptr &&
        ee::c_ee_dmac::mmio_read(
            &m_ee_dmac, ee::c_ee_dmac::k_sif1_base + offset,
            4u, &word);
    if (valid)
    {
        *result = static_cast<uint32_t>(word.m_lo);
    }
    return valid;
}

bool c_sif1_dma_transport::read_iop_register(
    uint32_t offset, uint32_t* result)
{
    return result != nullptr &&
        m_iop_hw.read(c_hw::k_sif1_dma_base + offset, 4u, result);
}

uint32_t c_sif1_dma_transport::fifo_peek(uint32_t offset) const
{
    return m_fifo[(m_head + offset) % k_fifo_words];
}

void c_sif1_dma_transport::fifo_write(uint32_t word)
{
    m_fifo[(m_head + m_count) % k_fifo_words] = word;
    ++m_count;
}

uint32_t c_sif1_dma_transport::fifo_read()
{
    const uint32_t word = m_fifo[m_head];
    m_head = (m_head + 1u) % k_fifo_words;
    --m_count;
    return word;
}

bool c_sif1_dma_transport::move_one_ee_quadword()
{
    uint32_t chcr = 0u;
    uint32_t madr = 0u;
    uint32_t qwc = 0u;
    if (!read_ee_register(0u, &chcr) ||
        !read_ee_register(0x10u, &madr) ||
        !read_ee_register(0x20u, &qwc))
    {
        m_faulted = true;
        return false;
    }

    if ((chcr & ee::c_ee_dmac::k_chcr_str) == 0u ||
        m_count > k_fifo_words - 4u)
    {
        return false;
    }

    // EE source tags are distinct from the 16-byte IOP destination
    // tags carried in the FIFO. Advance one source tag at a time,
    // without generating a completion on intermediate segments.
    const uint32_t mode = (chcr >> 2u) & 3u;
    if (mode == 1u && qwc == 0u)
    {
        if (!m_ee_dmac.prepare_sif1_chain_tag())
        {
            m_faulted = true;
            return false;
        }
        return true;
    }

    if ((chcr & 1u) == 0u || (mode != 0u && mode != 1u) ||
        qwc == 0u || (madr & 15u) != 0u ||
        madr >= 0x02000000u ||
        static_cast<uint64_t>(madr) + uint64_t(qwc) * 16u >
            0x02000000ull ||
        !m_ee_ram.contains(madr, static_cast<size_t>(qwc) * 16u))
    {
        m_faulted = true;
        return false;
    }

    // Every source qword is backed by guest EE memory. The RAM
    // snapshot is taken when the hardware pump reads it.
    ee::s_ee_u128 quad{};
    m_ee_ram.read128(madr, &quad);
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        const uint64_t half = index < 2u ? quad.m_lo : quad.m_hi;
        fifo_write(static_cast<uint32_t>(
            half >> ((index & 1u) * 32u)));
    }
    m_queued_bytes += 16u;
    if (!m_ee_dmac.commit_sif1_qwords(1u))
    {
        m_faulted = true;
        return false;
    }
    return true;
}

bool c_sif1_dma_transport::consume_one_iop_word_or_tag()
{
    uint32_t chcr = 0u;
    if (!read_iop_register(8u, &chcr))
    {
        m_faulted = true;
        return false;
    }
    if ((chcr & 0x01000000u) == 0u)
    {
        return false;
    }

    if (m_iop_words_remaining == 0u && m_iop_final_tag)
    {
        m_iop_final_tag = false;
        if (!m_iop_hw.complete_dma_channel(10u))
        {
            m_faulted = true;
        }
        return !m_faulted;
    }

    if (m_iop_words_remaining == 0u)
    {
        if (m_count < 4u)
        {
            return false;
        }

        // The first two words of the IOP destination-chain header
        // contain MADR[23:0] and a word-aligned byte count. The upper
        // two words are EE tag data, not payload (PCSX2 Sif1.cpp).
        const uint32_t tag = fifo_peek(0u);
        const uint32_t count = fifo_peek(1u) & 0xffffcu;
        const uint32_t destination = tag & 0x00ffffffu;

        if (count == 0u || (destination & 3u) != 0u ||
            destination >= c_cpu::k_ram_size ||
            static_cast<uint64_t>(destination) + count >
                c_cpu::k_ram_size)
        {
            m_faulted = true;
            return false;
        }

        for (uint32_t index = 0u; index < 4u; ++index)
        {
            (void)fifo_read();
        }
        m_iop_destination = destination;
        m_iop_words_remaining = count / 4u;
        // IOP destination-chain tags encode ERT in bit 30 and IRQ
        // (INT_O) in bit 31. Both terminate the current DMA10 transfer:
        // the hardware receiver reaches completion after the tagged
        // payload. PCSX2's SIFIOPReadTag checks IRQ || (ID & 4).
        m_iop_final_tag = (tag & 0xc0000000u) != 0u;
        return true;
    }

    if (m_count == 0u)
    {
        return false;
    }

    if (!m_iop_cpu.write32(m_iop_destination, fifo_peek(0u)))
    {
        m_faulted = true;
        return false;
    }
    (void)fifo_read();
    m_iop_destination += 4u;
    --m_iop_words_remaining;
    m_delivered_bytes += 4u;
    if (!m_iop_hw.write(c_hw::k_sif1_dma_base, 4u,
                        m_iop_destination))
    {
        m_faulted = true;
        return false;
    }
    return true;
}

void c_sif1_dma_transport::pump()
{
    if (m_in_pump || m_faulted)
    {
        return;
    }
    m_in_pump = true;

    // Each iteration either moves real guest bytes, retires a tag or
    // completes a genuine DMA. A bounded loop handles 64 KiB chains
    // without an event-spin if one endpoint is not yet available.
    constexpr uint32_t k_max_steps = 1u << 18u;
    for (uint32_t index = 0u; index < k_max_steps; ++index)
    {
        bool progressed = false;
        if (consume_one_iop_word_or_tag())
        {
            progressed = true;
        }
        if (!m_faulted && move_one_ee_quadword())
        {
            progressed = true;
        }
        if (!progressed || m_faulted)
        {
            break;
        }
    }
    m_in_pump = false;
}


bool c_sif1_dma_transport::set_dma(
    uint32_t list, uint32_t count, uint32_t* dma_id)
{
    // sceSifSetDma descriptors are four words:
    // EE source, IOP destination, byte count, attributes.
    //
    // The EE kernel normally marshals these into an EE DMA6 source
    // chain containing IOP DMA10 destination tags and padded payloads.
    // Our bounded HLE boundary does the same *marshalling*, but the
    // actual bytes still traverse DMA6, the FIFO and DMA10: no IOP RAM
    // copy, HLE SIFCMD dispatch or synthetic channel completion.
    //
    // Host-owned staging is borrowed from EE RAM only while a fully
    // armed synchronous transfer runs. Every original byte is restored
    // before returning to guest code, including on a failed transfer.
    constexpr uint32_t k_stage = 0x01c00000u;
    constexpr uint32_t k_stage_capacity = 0x00010000u;
    uint32_t ee_chcr = 0u;
    uint32_t iop_chcr = 0u;
    const uint32_t list_address = m_ee_ram.translate_direct_address(list);
    if (dma_id == nullptr || m_next_dma_id == 0u ||
        count == 0u || count > 64u ||
        // A descriptor list is word-aligned; DMA payloads remain quadword-aligned.
        (list & 3u) != 0u ||
        list_address >= 0x02000000u ||
        !m_ee_ram.contains(list_address, static_cast<size_t>(count) * 16u) ||
        !m_ee_ram.contains(k_stage, k_stage_capacity) ||
        m_faulted || m_count != 0u ||
        !read_ee_register(0u, &ee_chcr) ||
        (ee_chcr & ee::c_ee_dmac::k_chcr_str) != 0u ||
        !read_iop_register(8u, &iop_chcr) ||
        (iop_chcr & 0x01000000u) == 0u)
    {
        return false;
    }

    std::vector<uint8_t> packet(16u, 0u);
    uint64_t expected_payload = 0u;
    for (uint32_t index = 0u; index < count; ++index)
    {
        const uint32_t descriptor = list_address + index * 16u;
        const uint32_t source = m_ee_ram.read32(descriptor);
        const uint32_t destination = m_ee_ram.read32(descriptor + 4u);
        const uint32_t size = m_ee_ram.read32(descriptor + 8u) & 0x7fffffffu;
        const uint32_t attributes = m_ee_ram.read32(descriptor + 12u);
        const uint32_t source_address =
            m_ee_ram.translate_direct_address(source);
        const uint32_t destination_region = destination & 0xe0000000u;
        const uint32_t destination_address = destination & 0x1fffffffu;
        const uint64_t padded =
            (static_cast<uint64_t>(size) + 15u) & ~uint64_t(15u);
        // SIFCMD's final command packet uses
        // SIF_DMA_ERT | SIF_DMA_INT_O (0x40 | 0x04). Accept the
        // observed terminal command form and plain DMA descriptors;
        // other attribute combinations still fail closed until their
        // precise kernel tag behavior is implemented.
        const bool supported_attributes =
            attributes == 0u ||
            (attributes == 0x44u && index + 1u == count);
        if (!supported_attributes || size == 0u || size > 65536u ||
            (source & 15u) != 0u ||
            (destination & 3u) != 0u ||
            (destination_region != 0u &&
             destination_region != 0x80000000u &&
             destination_region != 0xa0000000u) ||
            source_address >= 0x02000000u ||
            !m_ee_ram.contains(source_address, size) ||
            destination_address == 0u ||
            destination_address >= c_cpu::k_ram_size ||
            static_cast<uint64_t>(destination_address) + padded >
                c_cpu::k_ram_size ||
            packet.size() + 16u + padded > k_stage_capacity)
        {
            return false;
        }

        const size_t tag_offset = packet.size();
        packet.resize(tag_offset + 16u + static_cast<size_t>(padded), 0u);
        // IOP DMA10 tag: bit 30 = ERT, bit 31 = INT_O.
        // The bounded syscall transfer ends at its final descriptor.
        // SIF_DMA_INT_O requests a tag IRQ on the terminal SIFCMD
        // packet; it is distinct from ending the destination chain.
        const uint32_t destination_tag =
            destination_address |
            (index + 1u == count ? 0x40000000u : 0u) |
            ((attributes & 0x04u) != 0u ? 0x80000000u : 0u);
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            packet[tag_offset + lane] =
                static_cast<uint8_t>(destination_tag >> (lane * 8u));
            packet[tag_offset + 4u + lane] =
                static_cast<uint8_t>(padded >> (lane * 8u));
        }
        for (uint32_t offset = 0u; offset < size; ++offset)
        {
            packet[tag_offset + 16u + offset] =
                m_ee_ram.read8(source_address + offset);
        }
        expected_payload += padded;
    }

    // One genuine DMA6 END source tag carries all IOP DMA10
    // destination tags. Packets are word- and quadword-aligned,
    // with zeros only in the transfer padding.
    const uint32_t qwc =
        static_cast<uint32_t>((packet.size() - 16u) / 16u);
    const uint32_t source_tag = 0x70000000u | qwc;
    for (uint32_t lane = 0u; lane < 4u; ++lane)
    {
        packet[lane] = static_cast<uint8_t>(source_tag >> (lane * 8u));
    }

    // Snapshot the entire source packet and the borrowed backing RAM
    // before writing a single staging byte. Guest sources, including
    // those that alias the borrowed area, were read above.
    std::vector<uint8_t> original(packet.size());
    for (size_t index = 0u; index < packet.size(); ++index)
    {
        original[index] =
            m_ee_ram.read8(k_stage + static_cast<uint32_t>(index));
        m_ee_ram.write8(
            k_stage + static_cast<uint32_t>(index), packet[index]);
    }

    const uint64_t before_queued = m_queued_bytes;
    const uint64_t before_delivered = m_delivered_bytes;
    const ee::s_ee_u128 zero{0u, 0u};
    const ee::s_ee_u128 address{k_stage, 0u};
    const ee::s_ee_u128 start{0x105u, 0u};
    const bool started =
        ee::c_ee_dmac::mmio_write(
            &m_ee_dmac, ee::c_ee_dmac::k_sif1_base + 0x20u,
            4u, &zero) &&
        ee::c_ee_dmac::mmio_write(
            &m_ee_dmac, ee::c_ee_dmac::k_sif1_base + 0x30u,
            4u, &address) &&
        ee::c_ee_dmac::mmio_write(
            &m_ee_dmac, ee::c_ee_dmac::k_sif1_base,
            4u, &start);

    // The registered DMA6 callback pumps the real FIFO synchronously
    // when the IOP DMA10 receiver is already armed. If the transfer
    // cannot finish, report failure rather than pretending DMA finished.
    uint32_t final_ee_chcr = 0u;
    uint32_t final_iop_chcr = 0u;
    const bool completed =
        started && !m_faulted &&
        read_ee_register(0u, &final_ee_chcr) &&
        read_iop_register(8u, &final_iop_chcr) &&
        (final_ee_chcr & ee::c_ee_dmac::k_chcr_str) == 0u &&
        (final_iop_chcr & 0x01000000u) == 0u &&
        m_count == 0u &&
        m_queued_bytes == before_queued + packet.size() - 16u &&
        m_delivered_bytes == before_delivered + expected_payload &&
        (m_ee_dmac.status() & (1u << 6u)) != 0u;

    for (size_t index = 0u; index < original.size(); ++index)
    {
        m_ee_ram.write8(
            k_stage + static_cast<uint32_t>(index), original[index]);
    }
    if (!completed)
    {
        return false;
    }

    *dma_id = m_next_dma_id++;
    return true;
}

bool c_sif1_dma_transport::dma_stat(
    uint32_t dma_id, int32_t* result) const
{
    if (result == nullptr || dma_id == 0u ||
        dma_id >= m_next_dma_id)
    {
        return false;
    }
    // This bounded hardware-backed syscall adapter only returns an
    // ID after a real DMA6 + DMA10 completion.
    *result = -1;
    return true;
}

uint64_t c_sif1_dma_transport::queued_bytes() const
{
    return m_queued_bytes;
}

uint64_t c_sif1_dma_transport::delivered_bytes() const
{
    return m_delivered_bytes;
}

uint32_t c_sif1_dma_transport::fifo_words() const
{
    return m_count;
}

bool c_sif1_dma_transport::faulted() const
{
    return m_faulted;
}
} // namespace iop
