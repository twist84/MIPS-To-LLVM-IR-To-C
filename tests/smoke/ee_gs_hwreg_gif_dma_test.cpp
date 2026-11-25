#include "ee_gs.h"
#include "ee_gif.h"
#include "ee_hw_bus.h"
#include "ee_memory.h"
#include "ee_exceptions.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace
{
constexpr uint32_t k_gif = ee::c_ee_dmac::k_gif_base;
constexpr uint32_t k_str = ee::c_ee_dmac::k_chcr_str;

int fail(const char* reason)
{
    std::fprintf(stderr, "GS HWREG PATH3 DMA: %s\n", reason);
    return 1;
}

uint64_t bitblt(uint32_t bp, uint32_t bw, uint32_t psm)
{
    return (static_cast<uint64_t>(bp) << 32u) |
        (static_cast<uint64_t>(bw) << 48u) |
        (static_cast<uint64_t>(psm) << 56u);
}

uint64_t trxreg(uint32_t width, uint32_t height)
{
    return width | (static_cast<uint64_t>(height) << 32u);
}

uint64_t tag_value(uint32_t nloop, uint32_t flg)
{
    return static_cast<uint64_t>(nloop & 0x7fffu) |
        (1ull << 15u) |
        (static_cast<uint64_t>(flg & 3u) << 58u) |
        (1ull << 60u);
}

uint64_t word64(const uint8_t* bytes)
{
    uint64_t value = 0u;
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        value |= static_cast<uint64_t>(bytes[i]) << (8u * i);
    }
    return value;
}

void quad(ee::s_ee_runtime* runtime,
          uint32_t address, uint64_t low, uint64_t high)
{
    const ee::s_ee_u128 value{low, high};
    ee::ee_mem_write128(runtime, address, &value);
}

void tag(ee::s_ee_runtime* runtime, uint32_t address,
         uint32_t nloop, uint32_t flg)
{
    quad(runtime, address, tag_value(nloop, flg),
         flg == 0u ? 0xeu : 0u);
}

void ad(ee::s_ee_runtime* runtime, uint32_t address,
        uint32_t reg, uint64_t value)
{
    quad(runtime, address, value, reg);
}

void dma(ee::s_ee_runtime* runtime,
         uint32_t address, uint32_t qwc)
{
    ee::ee_mem_write32(runtime, k_gif + 0x10u, address);
    ee::ee_mem_write32(runtime, k_gif + 0x20u, qwc);
    ee::ee_mem_write32(runtime, k_gif, k_str);
}

uint32_t mmio(ee::s_ee_runtime* runtime, uint32_t offset)
{
    return ee::ee_mem_read32(runtime, k_gif + offset);
}

bool read32(const ee::c_ee_gs_state& gs,
            uint32_t at, uint32_t expected)
{
    uint32_t got = 0u;
    return gs.read_vram32(at, got) && got == expected;
}
} // namespace

int main()
{
    ee::ee_memory memory(0x4000u);
    ee::s_ee_cpu_state cpu{};
    ee::c_ee_hw_bus hardware;
    ee::c_ee_gif_path3 gif;
    ee::c_ee_gs_state gs;
    hardware.bind_cpu(&cpu);
    hardware.bind_memory(&memory);
    hardware.bind_device_transfer(
        &gif, &ee::c_ee_gif_path3::dma_transfer
    );
    gif.bind_event_sink(&gs, &ee::c_ee_gs_state::gif_events);

    ee::s_ee_runtime runtime{};
    runtime.m_user = &memory;
    runtime.m_mmio_user = &hardware;
    runtime.m_mmio_read = &ee::c_ee_hw_bus::mmio_read;
    runtime.m_mmio_write = &ee::c_ee_hw_bus::mmio_write;

    constexpr uint32_t bp = 0x140u;
    constexpr uint32_t base = bp * 256u;
    constexpr uint32_t offsets[6] =
        {0u, 4u, 16u, 20u, 32u, 36u};
    std::array<uint8_t, 32> source{};
    source.fill(0xa5u);
    for (uint32_t i = 0u; i < 18u; ++i)
    {
        source[i] = static_cast<uint8_t>(0x10u + i);
    }

    // Five A+D registers in one GIF PACKED packet. The final HWREG
    // holds the first eight bytes of a six-pixel PSMCT24 upload.
    tag(&runtime, 0x100u, 5u, 0u);
    ad(&runtime, 0x110u, 0x50u, bitblt(bp, 1u, 1u));
    ad(&runtime, 0x120u, 0x51u, 0u);
    ad(&runtime, 0x130u, 0x52u, trxreg(6u, 1u));
    ad(&runtime, 0x140u, 0x53u, 0u);
    ad(&runtime, 0x150u, 0x54u, word64(source.data()));
    dma(&runtime, 0x100u, 6u);
    if (mmio(&runtime, 0x10u) != 0x160u ||
        mmio(&runtime, 0x20u) != 0u ||
        (mmio(&runtime, 0u) & k_str) != 0u ||
        (hardware.dmac().status() & (1u << 2u)) == 0u ||
        gif.state().m_active ||
        gs.state().m_hwreg_write_count != 1u ||
        gs.state().m_transfer_pixels_written != 2u ||
        gs.state().m_transfer_pending_size != 2u ||
        gs.state().m_image_qwc != 0u ||
        !read32(gs, base, 0x00121110u) ||
        !read32(gs, base + 4u, 0x00151413u))
    {
        return fail("A+D HWREG initial upload through GIF and DMAC");
    }

    // Another independent GIF DMA carries the next eight bytes,
    // retaining 24-bit pixel state and leaving one pending byte.
    tag(&runtime, 0x200u, 1u, 0u);
    ad(&runtime, 0x210u, 0x54u, word64(source.data() + 8u));
    dma(&runtime, 0x200u, 2u);
    if (gs.state().m_hwreg_write_count != 2u ||
        gs.state().m_transfer_pixels_written != 5u ||
        gs.state().m_transfer_pending_size != 1u ||
        !read32(gs, base + 16u, 0x00181716u) ||
        !read32(gs, base + 20u, 0x001b1a19u) ||
        !read32(gs, base + 32u, 0x001e1d1cu))
    {
        return fail("HWREG 8-byte input split across DMA transfers");
    }

    // Continue with an IMAGE tag; only two bytes of its full qword
    // complete the 6th pixel, and the remaining 14 are padding.
    tag(&runtime, 0x300u, 1u, 2u);
    std::array<uint8_t, 16> last{};
    last.fill(0xf9u);
    last[0] = source[16u];
    last[1] = source[17u];
    quad(&runtime, 0x310u,
         word64(last.data()), word64(last.data() + 8u));
    dma(&runtime, 0x300u, 2u);
    if (gs.state().m_transfer_pixels_written != 6u ||
        gs.state().m_transfer_pending_size != 0u ||
        gs.state().m_hwreg_write_count != 2u ||
        gs.state().m_image_qwc != 1u ||
        gs.state().m_image_data.size() != 16u ||
        !read32(gs, base + 36u, 0x0021201fu) ||
        gif.state().m_active)
    {
        return fail("HWREG-to-IMAGE 24-bit carry across PATH3 DMA");
    }
    for (uint32_t i = 0u; i < 6u; ++i)
    {
        const uint32_t expected =
            static_cast<uint32_t>(source[i * 3u]) |
            (static_cast<uint32_t>(source[i * 3u + 1u]) << 8u) |
            (static_cast<uint32_t>(source[i * 3u + 2u]) << 16u);
        if (!read32(gs, base + offsets[i], expected))
        {
            return fail("HWREG/IMAGE pixel order or swizzle mismatch");
        }
    }

    // Restart transfer through PACKED A+D; a later unsupported
    // FINISH in the same packet must roll back both 8-byte HWREG
    // pixels and the entire GIF/DMA transaction.
    tag(&runtime, 0x400u, 2u, 0u);
    ad(&runtime, 0x410u, 0x52u, trxreg(2u, 1u));
    ad(&runtime, 0x420u, 0x53u, 0u);
    dma(&runtime, 0x400u, 3u);
    if (gs.state().m_transfer_pixels_written != 0u ||
        !gs.state().m_transfer_started)
    {
        return fail("unable to restart PSMCT24 upload through GIF");
    }
    const uint64_t previous_tags = gs.state().m_tag_count;
    const uint64_t previous_hwreg = gs.state().m_hwreg_write_count;
    const uint32_t before0 = 0x00121110u;
    const uint32_t before1 = 0x00151413u;
    tag(&runtime, 0x500u, 2u, 0u);
    ad(&runtime, 0x510u, 0x54u, 0x0807060504030201ull);
    ad(&runtime, 0x520u, 0x61u, 0x1234ull);
    dma(&runtime, 0x500u, 3u);
    if (mmio(&runtime, 0x10u) != 0x500u ||
        mmio(&runtime, 0x20u) != 3u ||
        (mmio(&runtime, 0u) & k_str) == 0u ||
        gs.state().m_tag_count != previous_tags ||
        gs.state().m_hwreg_write_count != previous_hwreg ||
        gs.state().m_transfer_pixels_written != 0u ||
        gs.state().m_transfer_pending_size != 0u ||
        !read32(gs, base, before0) ||
        !read32(gs, base + 4u, before1) ||
        gif.state().m_active)
    {
        return fail("rejected FINISH leaked HWREG VRAM or parser state");
    }

    // Fix only the trailing offending A+D register and retry from
    // unchanged DMA address. Two 24-bit pixels complete the 2x1
    // rectangle; the final two HWREG bytes are ignored as padding.
    ad(&runtime, 0x520u, 0x4cu, 0xabcdefull);
    ee::ee_mem_write32(&runtime, k_gif, 0u);
    dma(&runtime, 0x500u, 3u);
    if (mmio(&runtime, 0x10u) != 0x530u ||
        (mmio(&runtime, 0u) & k_str) != 0u ||
        gs.state().m_hwreg_write_count != previous_hwreg + 1u ||
        gs.state().m_transfer_pixels_written != 2u ||
        gs.state().m_transfer_pending_size != 0u ||
        !read32(gs, base, 0x00030201u) ||
        !read32(gs, base + 4u, 0x00060504u) ||
        gs.register_value(0x4cu) != 0xabcdefull ||
        gif.state().m_active)
    {
        return fail("HWREG transaction retry through GIF DMAC");
    }

    std::puts("GS HWREG PATH3/DMAC carry, pad, rollback and retry: PASS");
    return 0;
}
