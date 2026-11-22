#include "iop_sif1_bridge.h"

#include <cstdint>
#include <cstdio>

namespace
{
int fail(const char* description)
{
    std::fprintf(stderr, "EE-to-IOP SIF1: %s\n", description);
    return 1;
}

void descriptor(
    ee::ee_memory& memory, uint32_t at,
    uint32_t source, uint32_t destination,
    uint32_t size, uint32_t attributes = 0u
)
{
    memory.write32(at, source);
    memory.write32(at + 4u, destination);
    memory.write32(at + 8u, size);
    memory.write32(at + 12u, attributes);
}
} // namespace

int main()
{
    ee::ee_memory ee_ram(32u * 1024u * 1024u);
    ee::s_ee_cpu_state ee_cpu{};
    ee::c_ee_dmac dmac;
    dmac.bind_cpu(&ee_cpu);
    dmac.bind_memory(&ee_ram);
    dmac.reset();

    iop::c_cpu iop_cpu;
    iop::c_sif1_bridge bridge(ee_ram, iop_cpu, dmac);
    constexpr uint32_t k_list = 0x11004u;
    constexpr uint32_t k_source0 = 0x12000u;
    constexpr uint32_t k_source1 = 0x13000u;
    constexpr uint32_t k_destination0 = 0x14004u;
    constexpr uint32_t k_destination1 = 0x15000u;

    for (uint32_t i = 0u; i < 8u; ++i)
    {
        ee_ram.write32(k_source0 + i * 4u, 0xabc00000u + i);
    }
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        ee_ram.write32(k_source1 + i * 4u, 0xdef00000u + i);
    }
    descriptor(
        ee_ram, k_list, 0x80000000u | k_source0,
        k_destination0, 0x80000000u | 20u
    );
    descriptor(
        ee_ram, k_list + 16u, k_source1,
        0xa0000000u | k_destination1, 16u
    );

    uint32_t id = 0u;
    int32_t stat = 0;
    uint32_t word = 0u;
    if ((dmac.status() & (1u << 6u)) != 0u ||
        !bridge.set_dma(k_list, 2u, &id) ||
        id != 1u || !bridge.dma_stat(id, &stat) || stat != -1 ||
        (dmac.status() & (1u << 6u)) == 0u)
    {
        return fail("validated transfers did not complete");
    }
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        if (!iop_cpu.read32(k_destination0 + i * 4u, &word) ||
            word != 0xabc00000u + i)
        {
            return fail("SIF transfers must preserve rounded quadwords");
        }
    }
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        if (!iop_cpu.read32(k_destination1 + i * 4u, &word) ||
            word != 0xdef00000u + i)
        {
            return fail("second SIF transfer payload incorrect");
        }
    }
    if (bridge.dma_stat(0u, &stat) ||
        bridge.dma_stat(2u, &stat) ||
        bridge.dma_stat(id, nullptr))
    {
        return fail("invented DMA completion for unknown IDs");
    }

    dmac.acknowledge(6u);
    iop_cpu.write32(k_destination0, 0x11223344u);
    descriptor(
        ee_ram, k_list + 16u, k_source1,
        k_destination1, 16u, 0x100u
    );
    if (bridge.set_dma(k_list, 2u, &id) ||
        !iop_cpu.read32(k_destination0, &word) ||
        word != 0x11223344u ||
        (dmac.status() & (1u << 6u)) != 0u)
    {
        return fail("invalid second descriptor partially committed");
    }
    descriptor(
        ee_ram, k_list + 16u, k_source1,
        k_destination1, 16u
    );

    if (bridge.set_dma(k_list, 0u, &id) ||
        bridge.set_dma(k_list + 1u, 1u, &id) ||
        bridge.set_dma(0xffffff00u, 2u, &id))
    {
        return fail("invalid descriptor list was accepted");
    }
    descriptor(ee_ram, k_list, k_source0, k_destination0 + 1u, 16u);
    if (bridge.set_dma(k_list, 1u, &id))
    {
        return fail("non-word-aligned IOP destination was accepted");
    }
    descriptor(ee_ram, k_list, k_source0, 0u, 16u);
    if (bridge.set_dma(k_list, 1u, &id))
    {
        return fail("IOP reboot destination incorrectly treated as RAM");
    }
    descriptor(ee_ram, k_list, k_source0, 0xdeadbef0u, 16u);
    if (bridge.set_dma(k_list, 1u, &id))
    {
        return fail("RPC mailbox incorrectly treated as ordinary RAM");
    }

    ee_ram.write32(ee::ee_memory::k_scratchpad_base, 0x55667788u);
    descriptor(
        ee_ram, k_list, ee::ee_memory::k_scratchpad_base,
        k_destination0, 4u
    );
    if (!bridge.set_dma(k_list, 1u, &id) || id != 2u ||
        !iop_cpu.read32(k_destination0, &word) ||
        word != 0x55667788u)
    {
        return fail("EE scratchpad-to-IOP DMA failed");
    }
    std::printf("EE-to-IOP SIF1: PASS\n");
    return 0;
}
