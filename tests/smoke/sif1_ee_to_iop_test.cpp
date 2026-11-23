#include "iop_sif1_dma_transport.h"
#include "iop_kernel.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
constexpr uint32_t k_source = 0x00100000u;
constexpr uint32_t k_destination = 0x00018000u;

int fail(const char* message, uint32_t size)
{
    std::fprintf(stderr, "SIF1 EE->IOP: %s (size=%u)\n", message, size);
    return 1;
}

void put32(std::vector<uint8_t>& rom, uint32_t at, uint32_t word)
{
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        rom[at + index] = static_cast<uint8_t>(word >> (index * 8u));
    }
}

bool write_ee(ee::c_ee_dmac& dmac, uint32_t offset, uint32_t value)
{
    const ee::s_ee_u128 word{value, 0u};
    return ee::c_ee_dmac::mmio_write(
        &dmac, ee::c_ee_dmac::k_sif1_base + offset, 4u, &word);
}

bool read_ee(ee::c_ee_dmac& dmac, uint32_t offset, uint32_t* value)
{
    ee::s_ee_u128 word{};
    const bool result = value != nullptr &&
        ee::c_ee_dmac::mmio_read(
            &dmac, ee::c_ee_dmac::k_sif1_base + offset, 4u, &word);
    if (result)
    {
        *value = static_cast<uint32_t>(word.m_lo);
    }
    return result;
}

uint8_t pattern(uint32_t index, uint32_t size)
{
    return static_cast<uint8_t>((index * 29u + size * 3u + 0x5bu) & 0xffu);
}

int run_case(uint32_t size, bool sender_first)
{
    ee::ee_memory ee_ram(32u * 1024u * 1024u);
    ee::s_ee_cpu_state ee_cpu{};
    ee::c_ee_dmac dmac{};
    dmac.bind_memory(&ee_ram);
    dmac.bind_cpu(&ee_cpu);
    dmac.reset();

    iop::c_kernel kernel{};
    std::vector<uint8_t> rom(iop::c_cpu::k_rom0_size, 0u);
    const uint32_t instructions[] =
    {
        0x3c08bf80u, // lui t0,0xbf80
        0x35081538u, // ori t0,t0,0x1538 (DMA10 CHCR)
        0x3c090100u, // lui t1,0x0100
        0x35290700u, // ori t1,t1,0x0700
        0xad090000u, // sw t1,0(t0), arm genuine IOP receiver
        0x0000000du  // break
    };
    for (uint32_t index = 0u;
         index < sizeof(instructions) / sizeof(instructions[0]); ++index)
    {
        put32(rom, index * 4u, instructions[index]);
    }
    if (!kernel.start_rom0_boot(rom))
    {
        return fail("could not start receiver ROM0 fixture", size);
    }

    iop::c_sif1_dma_transport transport(
        ee_ram, dmac, kernel.cpu(), kernel.hardware());

    // Enable channel-10 DMA status and IRQ3 delivery without generating
    // a fake completion. EE D_STAT masks are write-one-to-toggle.
    const ee::s_ee_u128 mask{0x00400000u, 0u};
    if (!ee::c_ee_dmac::mmio_write(
            &dmac, ee::c_ee_dmac::k_stat_address, 4u, &mask) ||
        !kernel.hardware().write(iop::c_hw::k_dma_icr2,
                                 4u, 0x00880000u) ||
        !kernel.hardware().write(
            iop::c_hw::k_i_mask, 4u, 1u << iop::c_hw::k_dma_irq))
    {
        return fail("could not configure DMA interrupt masks", size);
    }
    if (dmac.pending() != 0u ||
        kernel.hardware().pending_interrupts() != 0u)
    {
        return fail("enabling IRQ masks created an interrupt", size);
    }

    // IOP DMA10 tag bits: END=31, IRQ=30.
    ee_ram.write32(k_source, 0xc0000000u | k_destination);
    ee_ram.write32(k_source + 4u, size);
    ee_ram.write32(k_source + 8u, 0u);
    ee_ram.write32(k_source + 12u, 0u);

    for (uint32_t index = 0u; index < size; ++index)
    {
        ee_ram.write8(k_source + 16u + index, pattern(index, size));
    }

    for (uint32_t address = k_destination - 16u;
         address < k_destination + size + 16u; address += 4u)
    {
        if (!kernel.cpu().write32(address, 0xcdcdcdcdu))
        {
            return fail("could not initialize destination guards", size);
        }
    }

    const auto send = [&]()
    {
        return write_ee(dmac, 0x10u, k_source) &&
               write_ee(dmac, 0x20u, (size + 16u) / 16u) &&
               write_ee(dmac, 0u, 0x101u);
    };
    const auto receive = [&]()
    {
        const iop::s_kernel_run_result run = kernel.run(32u, 1u);
        return run.m_stop == iop::e_kernel_stop::breakpoint &&
               run.m_executed == 5u &&
               run.m_guest_pc == 0xbfc00014u;
    };

    if (sender_first)
    {
        if (!send() ||
            kernel.hardware().pending_interrupts() != 0u)
        {
            return fail("sender-first issued a false IOP completion", size);
        }
        uint32_t chcr = 0u;
        if (size > 496u && (!read_ee(dmac, 0u, &chcr) ||
                            (chcr & ee::c_ee_dmac::k_chcr_str) == 0u))
        {
            return fail("EE prematurely completed while FIFO full", size);
        }
        if (!receive())
        {
            return fail("IOP did not execute its receiver MMIO write", size);
        }
    }
    else
    {
        if (!receive() ||
            kernel.hardware().pending_interrupts() != 0u ||
            !send())
        {
            return fail("receiver-first startup or EE DMA start", size);
        }
    }

    uint32_t ee_chcr = 0u;
    uint32_t ee_qwc = 0u;
    uint32_t iop_chcr = 0u;
    uint32_t iop_madr = 0u;
    uint32_t iop_icr2 = 0u;
    uint32_t iop_stat = 0u;
    if (transport.faulted() || transport.fifo_words() != 0u ||
        transport.delivered_bytes() != size ||
        transport.queued_bytes() != size + 16u ||
        !read_ee(dmac, 0u, &ee_chcr) ||
        !read_ee(dmac, 0x20u, &ee_qwc) ||
        (ee_chcr & ee::c_ee_dmac::k_chcr_str) != 0u ||
        ee_qwc != 0u ||
        (dmac.status() & (1u << 6u)) == 0u ||
        (dmac.pending() & (1u << 6u)) == 0u ||
        !kernel.hardware().read(
            iop::c_hw::k_sif1_dma_base + 8u, 4u, &iop_chcr) ||
        (iop_chcr & 0x01000000u) != 0u ||
        !kernel.hardware().read(
            iop::c_hw::k_sif1_dma_base, 4u, &iop_madr) ||
        iop_madr != k_destination + size ||
        !kernel.hardware().read(
            iop::c_hw::k_dma_icr2, 4u, &iop_icr2) ||
        (iop_icr2 & (1u << 27u)) == 0u ||
        !kernel.hardware().read(
            iop::c_hw::k_i_stat, 4u, &iop_stat) ||
        (iop_stat & (1u << iop::c_hw::k_dma_irq)) == 0u ||
        kernel.hardware().pending_interrupts() == 0u)
    {
        return fail("transfer bytes, STR or channel IRQ status mismatch", size);
    }

    uint64_t source_sum = 0u;
    uint64_t dest_sum = 0u;
    for (uint32_t address = k_destination - 16u;
         address < k_destination + size + 16u; address += 4u)
    {
        uint32_t word = 0u;
        if (!kernel.cpu().read32(address, &word))
        {
            return fail("IOP RAM read failed after DMA", size);
        }
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            const uint32_t index = address + lane - k_destination;
            const uint8_t actual = static_cast<uint8_t>(
                word >> (lane * 8u));
            if (address < k_destination ||
                address >= k_destination + size)
            {
                if (actual != 0xcdu)
                {
                    return fail("destination guard overwritten", size);
                }
            }
            else
            {
                const uint8_t expected = pattern(index, size);
                if (actual != expected ||
                    ee_ram.read8(k_source + 16u + index) != expected)
                {
                    return fail("source/destination byte mismatch", size);
                }
                source_sum += expected;
                dest_sum += actual;
            }
        }
    }

    std::printf(
        "{\"direction\":\"SIF1\",\"ee_channel\":6,"
        "\"iop_channel\":10,\"source\":%u,\"destination\":%u,"
        "\"requested_bytes\":%u,\"transferred_bytes\":%llu,"
        "\"source_checksum\":%llu,\"destination_checksum\":%llu,"
        "\"ee_dstat\":%u,\"iop_dicr2\":%u,\"iop_istat\":%u,"
        "\"sender_first\":%s}\n",
        k_source, k_destination, size,
        static_cast<unsigned long long>(transport.delivered_bytes()),
        static_cast<unsigned long long>(source_sum),
        static_cast<unsigned long long>(dest_sum),
        dmac.status(), iop_icr2, iop_stat,
        sender_first ? "true" : "false");
    return 0;
}

int run_syscall_case(uint32_t size, uint32_t count,
                     uint32_t final_attributes)
{
    constexpr uint32_t k_descriptor = 0x00110004u;
    constexpr uint32_t k_data = 0x00120000u;
    constexpr uint32_t k_borrowed_stage = 0x01c00000u;
    ee::ee_memory memory(32u * 1024u * 1024u);
    ee::s_ee_cpu_state ee_cpu{};
    ee::c_ee_dmac dmac{};
    dmac.bind_memory(&memory);
    dmac.bind_cpu(&ee_cpu);
    dmac.reset();

    iop::c_kernel kernel{};
    std::vector<uint8_t> rom(iop::c_cpu::k_rom0_size, 0u);
    constexpr uint32_t receiver_program[] =
    {
        0x3c08bf80u, // lui t0,0xbf80
        0x35081538u, // ori t0,t0,0x1538 (DMA10 CHCR)
        0x3c090100u, // lui t1,0x0100
        0x35290700u, // ori t1,t1,0x0700
        0xad090000u, // sw t1,0(t0), genuine DMA10 start
        0x0000000du  // break
    };
    for (uint32_t index = 0u; index < 6u; ++index)
    {
        put32(rom, index * 4u, receiver_program[index]);
    }
    if (!kernel.start_rom0_boot(rom))
    {
        return fail("syscall ROM0 fixture failed to reset", size);
    }
    iop::c_sif1_dma_transport transport(
        memory, dmac, kernel.cpu(), kernel.hardware());

    const uint32_t padded = (size + 15u) & ~15u;
    for (uint32_t index = 0u; index < 256u; ++index)
    {
        memory.write8(k_borrowed_stage + index,
                      static_cast<uint8_t>(index ^ 0x5au));
    }
    for (uint32_t n = 0u; n < count; ++n)
    {
        const uint32_t src = k_data + n * 0x1000u;
        const uint32_t dst = k_destination + 4u + n * 0x1000u;
        memory.write32(k_descriptor + n * 16u, src);
        memory.write32(k_descriptor + n * 16u + 4u, dst);
        memory.write32(k_descriptor + n * 16u + 8u, 0x80000000u | size);
        memory.write32(k_descriptor + n * 16u + 12u,
                       n + 1u == count ? final_attributes : 0u);
        for (uint32_t index = 0u; index < padded + 16u; ++index)
        {
            memory.write8(
                src + index,
                index < size ? pattern(index + n, size) : 0xa5u);
        }
        for (uint32_t offset = 0u; offset < padded + 16u;
             offset += 4u)
        {
            if (!kernel.cpu().write32(
                    dst + offset, 0xcdcdcdcdu))
            {
                return fail("could not arm destination guards", size);
            }
        }
    }

    uint32_t id = 0xdeadbeefu;
    if (transport.set_dma(k_descriptor, count, &id) ||
        id != 0xdeadbeefu || dmac.status() != 0u)
    {
        return fail("unarmed DMA10 accepted a guest descriptor", size);
    }

    const iop::s_kernel_run_result run = kernel.run(32u, 1u);
    if (run.m_stop != iop::e_kernel_stop::breakpoint ||
        run.m_executed != 5u ||
        run.m_guest_pc != 0xbfc00014u)
    {
        return fail("IOP receiver MMIO setup not executed", size);
    }

    memory.write32(k_descriptor + 12u, 1u);
    if (transport.set_dma(k_descriptor, count, &id) ||
        id != 0xdeadbeefu || dmac.status() != 0u)
    {
        return fail("invalid attributes committed partial DMA", size);
    }
    memory.write32(k_descriptor + 12u,
                   count == 1u ? final_attributes : 0u);

    // A terminal SIFCMD marker is supported only on the final packet.
    // Rejecting an earlier marker must not submit or partially deliver DMA.
    if (count > 1u)
    {
        memory.write32(k_descriptor + 12u, 0x44u);
        if (transport.set_dma(k_descriptor, count, &id) ||
            id != 0xdeadbeefu || dmac.status() != 0u)
        {
            return fail("nonterminal SIFCMD marker accepted", size);
        }
        memory.write32(k_descriptor + 12u, 0u);
    }

    if (transport.set_dma(k_descriptor + 1u, count, &id) ||
        id != 0xdeadbeefu || dmac.status() != 0u)
    {
        return fail("non-word-aligned descriptor list accepted", size);
    }
    memory.write32(k_descriptor + 4u, k_destination + 5u);
    if (transport.set_dma(k_descriptor, count, &id) ||
        id != 0xdeadbeefu || dmac.status() != 0u)
    {
        return fail("non-word-aligned IOP destination accepted", size);
    }
    memory.write32(k_descriptor + 4u, k_destination + 4u);
    if (!transport.set_dma(k_descriptor, count, &id) || id != 1u)
    {
        return fail("real syscall DMA6->DMA10 failed", size);
    }
    int32_t dma_status = 0;
    if (!transport.dma_stat(id, &dma_status) ||
        dma_status != -1 ||
        transport.dma_stat(0u, &dma_status) ||
        transport.dma_stat(id + 1u, &dma_status) ||
        transport.queued_bytes() !=
            static_cast<uint64_t>(count) * (16u + padded) ||
        transport.delivered_bytes() !=
            static_cast<uint64_t>(count) * padded ||
        transport.fifo_words() != 0u ||
        transport.faulted() ||
        (dmac.status() & (1u << 6u)) == 0u)
    {
        return fail("DMA stat, completion or byte counters mismatch", size);
    }

    uint32_t ee_chcr = 0u;
    uint32_t iop_chcr = 0u;
    if (!read_ee(dmac, 0u, &ee_chcr) ||
        (ee_chcr & ee::c_ee_dmac::k_chcr_str) != 0u ||
        !kernel.hardware().read(
            iop::c_hw::k_sif1_dma_base + 8u, 4u, &iop_chcr) ||
        (iop_chcr & 0x01000000u) != 0u)
    {
        return fail("guest DMA channel STR did not complete", size);
    }
    for (uint32_t index = 0u; index < 256u; ++index)
    {
        if (memory.read8(k_borrowed_stage + index) !=
            static_cast<uint8_t>(index ^ 0x5au))
        {
            return fail("host staging altered guest EE RAM", size);
        }
    }
    for (uint32_t n = 0u; n < count; ++n)
    {
        const uint32_t src = k_data + n * 0x1000u;
        const uint32_t dst = k_destination + 4u + n * 0x1000u;
        for (uint32_t index = 0u; index < padded + 16u; ++index)
        {
            uint32_t read_word = 0u;
            if (!kernel.cpu().read32(dst + (index & ~3u),
                                     &read_word))
            {
                return fail("IOP payload read fault", size);
            }
            const uint8_t actual = static_cast<uint8_t>(
                read_word >> ((index & 3u) * 8u));
            const uint8_t expected = index < size
                ? pattern(index + n, size)
                : index < padded ? 0xa5u : 0xcdu;
            const uint8_t expected_source = index < size
                ? pattern(index + n, size) : 0xa5u;
            const uint8_t observed_source = memory.read8(src + index);
            if (actual != expected || observed_source != expected_source)
            {
                std::fprintf(stderr,
                    "SIF1 guest DMA mismatch: descriptor=%u offset=%u "
                    "dst=0x%08x expected=0x%02x actual=0x%02x "
                    "src=0x%08x src_expected=0x%02x src_actual=0x%02x\n",
                    n, index, dst + index, expected, actual, src + index,
                    expected_source, observed_source);
                return fail("guest payload, padding or guard mismatch", size);
            }
        }
    }
    std::printf(
        "SifSetDma guest descriptors: PASS size=%u count=%u attr=%08x "
        "real_DMA6_DSTAT=%08x queued=%llu delivered=%llu\n",
        size, count, final_attributes, dmac.status(),
        static_cast<unsigned long long>(transport.queued_bytes()),
        static_cast<unsigned long long>(transport.delivered_bytes()));
    return 0;
}

} // namespace

int main()
{
    const uint32_t sizes[] = {16u, 32u, 128u, 256u, 4096u};
    for (uint32_t size : sizes)
    {
        if (run_case(size, size == 16u || size == 4096u) != 0)
        {
            return 1;
        }
    }
    if (run_syscall_case(20u, 1u, 0u) != 0 ||
        run_syscall_case(20u, 1u, 0x44u) != 0 ||
        run_syscall_case(56u, 2u, 0x44u) != 0)
    {
        return 1;
    }
    std::printf("SIF1 EE-to-IOP hardware FIFO: PASS\n");
    return 0;
}
