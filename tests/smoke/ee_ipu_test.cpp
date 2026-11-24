// IPU device through the real EE bus, DMAC channels 3/4, INTC and RAM:
// FDEC/VDEC results, BCLR + input FIFO, and CSC from toIPU to fromIPU DMA.

#include "ee_hw_bus.h"
#include "ee_memory.h"
#include "ee_exceptions.h"

#include <cstdint>
#include <cstdio>

namespace
{
int g_pass = 0;
int g_fail = 0;

void check(bool ok, const char* name)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    (ok ? g_pass : g_fail)++;
}

constexpr uint32_t k_ipu_cmd = 0x10002000u;
constexpr uint32_t k_ipu_ctrl = 0x10002010u;
constexpr uint32_t k_in_fifo = 0x10007010u;
constexpr uint32_t k_from_ipu = 0x1000b000u;
constexpr uint32_t k_to_ipu = 0x1000b400u;
constexpr uint32_t k_chcr_start = 0x100u;
} // namespace

int main()
{
    ee::s_ee_cpu_state cpu{};
    ee::ee_memory ram(64u * 1024u);
    ee::c_ee_hw_bus hardware;
    hardware.bind_cpu(&cpu);
    hardware.bind_memory(&ram);

    ee::s_ee_runtime runtime{};
    runtime.m_user = &ram;
    runtime.m_mmio_user = &hardware;
    runtime.m_mmio_read = &ee::c_ee_hw_bus::mmio_read;
    runtime.m_mmio_write = &ee::c_ee_hw_bus::mmio_write;

    // FDEC returns the next 32 bitstream bits, MSB first, pulled from a
    // normal-mode toIPU DMA. Completion raises INTC cause 8 (IPU).
    const uint8_t stream[16] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};
    for (uint32_t i = 0; i < 16u; ++i)
    {
        ram.write8(0x1000u + i, stream[i]);
    }
    ee::ee_mem_write32(&runtime, k_to_ipu + 0x10u, 0x1000u);  // MADR
    ee::ee_mem_write32(&runtime, k_to_ipu + 0x20u, 1u);       // QWC
    ee::ee_mem_write32(&runtime, k_to_ipu, k_chcr_start | 1u);
    ee::ee_mem_write32(&runtime, k_ipu_cmd, 4u << 28u);       // FDEC, skip 0
    check(ee::ee_mem_read32(&runtime, k_ipu_cmd) == 0x12345678u,
          "FDEC returns the first 32 stream bits from toIPU DMA");
    check((ee::ee_mem_read32(&runtime, k_ipu_ctrl) & 0x80000000u) == 0u,
          "IPU_CTRL is not busy after FDEC");
    check((hardware.intc().status() & (1u << 8u)) != 0u,
          "a completed command raises INTC cause 8");
    check((hardware.dmac().status() & (1u << 4u)) != 0u,
          "toIPU (channel 4) completion is reported in D_STAT");

    // BCLR then the input FIFO: '1' is the 1-bit macroblock address
    // increment code, decoded by VDEC table 0 as {length 1, increment 1}.
    ee::ee_mem_write32(&runtime, k_ipu_cmd, 0u << 28u);       // BCLR
    ee::s_ee_u128 fifo{};
    fifo.m_lo = 0x80u;  // first byte 0b1000_0000
    ee::ee_mem_write128(&runtime, k_in_fifo, &fifo);
    ee::ee_mem_write32(&runtime, k_ipu_cmd, 3u << 28u);       // VDEC, MBAI
    check(ee::ee_mem_read32(&runtime, k_ipu_cmd) == 0x00010001u,
          "VDEC decodes a macroblock address increment from the FIFO");

    // Retail SDBZ feeds SETIQ through the input FIFO with 64-bit SD pairs
    // at 0x10007010/0x10007018 before issuing the command.
    ee::ee_mem_write32(&runtime, k_ipu_cmd, 0u << 28u);       // BCLR
    for (uint32_t q = 0; q < 5u; ++q)
    {
        const uint64_t lo = 0x0807060504030201ull + q * 0x1010101010101010ull;
        ee::ee_mem_write64(&runtime, k_in_fifo, lo);
        ee::ee_mem_write64(&runtime, k_in_fifo + 8u, lo + 0x0808080808080808ull);
    }
    ee::ee_mem_write32(&runtime, k_ipu_cmd, 5u << 28u);       // SETIQ intra
    check((ee::ee_mem_read32(&runtime, k_ipu_ctrl) & 0x80000000u) == 0u,
          "SETIQ completes from 64-bit FIFO writes");
    ee::ee_mem_write32(&runtime, k_ipu_cmd, 4u << 28u);       // FDEC
    check(ee::ee_mem_read32(&runtime, k_ipu_cmd) == 0x41424344u,
          "FDEC reads the bytes after the 64-byte matrix");

    // CSC: one RAW8 macroblock (Y=235, Cb=Cr=128) from toIPU becomes 256
    // opaque white RGBA32 pixels on fromIPU.
    for (uint32_t i = 0; i < 384u; ++i)
    {
        ram.write8(0x2000u + i, i < 256u ? 235u : 128u);
    }
    ee::ee_mem_write32(&runtime, k_ipu_cmd, 0u << 28u);       // BCLR
    ee::ee_mem_write32(&runtime, k_from_ipu + 0x10u, 0x8000u);
    ee::ee_mem_write32(&runtime, k_from_ipu + 0x20u, 64u);
    ee::ee_mem_write32(&runtime, k_from_ipu, k_chcr_start);
    ee::ee_mem_write32(&runtime, k_to_ipu + 0x10u, 0x2000u);
    ee::ee_mem_write32(&runtime, k_to_ipu + 0x20u, 24u);
    ee::ee_mem_write32(&runtime, k_to_ipu, k_chcr_start | 1u);
    ee::ee_mem_write32(&runtime, k_ipu_cmd, (7u << 28u) | 1u); // CSC, RGB32, 1 MB
    bool white = true;
    for (uint32_t i = 0; i < 256u; ++i)
    {
        white = white && ram.read32(0x8000u + 4u * i) == 0x80ffffffu;
    }
    check(white, "CSC converts a RAW8 macroblock to RGBA32 via fromIPU DMA");
    check((ee::ee_mem_read32(&runtime, k_from_ipu) & k_chcr_start) == 0u &&
              ee::ee_mem_read32(&runtime, k_from_ipu + 0x20u) == 0u &&
              (hardware.dmac().status() & (1u << 3u)) != 0u,
          "fromIPU (channel 3) finishes with QWC 0 and D_STAT bit 3");
    check(hardware.ipu().output_qwords() == 64u && hardware.ipu().stalls() == 0u,
          "IPU pushed exactly 64 output qwords without stalling");

    std::printf("ee_ipu_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
