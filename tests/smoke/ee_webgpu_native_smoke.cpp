// Native GPU execution test; no browser, SDL, display server, or game ELF.
// Uses the original wgpu-native GS compute transfer/display passes.
#include "ps2_webgpu/gs/gpu/gpu.hpp"
#include "ee_gs.h"
#include "ee_webgpu_bridge.h"

#include <algorithm>
#include <array>

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <vector>

namespace
{
bool write_register(ee::c_ee_gs_state& gs, uint32_t reg, uint64_t value)
{
    ee::s_ee_gif_event event{};
    event.m_type = ee::e_ee_gif_event_type::packed;
    event.m_register = 0xeu;
    event.m_word0 = value;
    event.m_word1 = reg;
    const std::array<uint8_t, 16> bytes{};
    return gs.consume(&event, 1u, bytes.data(), 1u);
}

bool write_image(ee::c_ee_gs_state& gs, const uint8_t* bytes, uint32_t qwc)
{
    ee::s_ee_gif_event event{};
    event.m_type = ee::e_ee_gif_event_type::image;
    event.m_qwc = qwc;
    return gs.consume(&event, 1u, bytes, qwc);
}

bool compare_transfer_formats(ee::c_ee_webgpu_bridge& bridge)
{
    // A fresh zero-filled GPU VRAM/CLUT and CPU GS receive the exact
    // same accepted GIF events. Compare raw physical bytes, including
    // untouched lanes, rather than the rendered image or a checksum.
    using namespace ps2::gs;
    std::vector<uint32_t> zeros(kMemorySize / 4u, 0u);
    gpu::write_state(zeros, 1u, std::vector<uint32_t>(512u, 0u));
    ee::c_ee_gs_state gs{};
    gs.bind_committed_event_observer(&bridge,
        &ee::c_ee_webgpu_bridge::committed_gif_events);
    std::vector<uint8_t> actual(kMemorySize, 0u);
    struct s_format
    {
        uint32_t m_psm;
        uint32_t m_bits;
    };
    constexpr s_format formats[] =
    {
        {0x00u, 32u}, {0x01u, 24u}, {0x02u, 16u}, {0x0au, 16u},
        {0x13u, 8u}, {0x14u, 4u}, {0x1bu, 8u}, {0x24u, 4u},
        {0x2cu, 4u}, {0x30u, 32u}, {0x31u, 24u},
        {0x32u, 16u}, {0x3au, 16u}
    };
    for (const auto& format : formats)
    {
        // Reuse a nonzero base to exercise preservation of previously
        // written lanes, cross GS pages, and wrap the 4 MiB address.
        constexpr uint32_t width = 137u;
        constexpr uint32_t height = 3u;
        constexpr uint32_t base = 0x3fffu;
        constexpr uint32_t bw = 4u;
        constexpr uint32_t x = 5u;
        constexpr uint32_t y = 31u;
        const uint32_t qwc = (width * height * format.m_bits + 127u) / 128u;
        std::vector<uint8_t> payload(qwc * 16u);
        uint32_t seed = 0x12345678u ^ format.m_psm;
        for (auto& byte : payload)
        {
            seed = seed * 1664525u + 1013904223u;
            byte = static_cast<uint8_t>(seed >> 24u);
        }
        if (!write_register(gs, 0x50u, (uint64_t{base} << 32u) |
                (uint64_t{bw} << 48u) | (uint64_t{format.m_psm} << 56u)) ||
            !write_register(gs, 0x51u, (uint64_t{x} << 32u) | (uint64_t{y} << 48u)) ||
            !write_register(gs, 0x52u, width | (uint64_t{height} << 32u)) ||
            !write_register(gs, 0x53u, 0u) ||
            !write_image(gs, payload.data(), 1u) ||
            !write_image(gs, payload.data() + 16u, qwc - 1u) ||
            !gpu::download_checked(actual.data()))
        {
            std::fprintf(stderr, "GS transfer/readback failed PSM=%02x\n", format.m_psm);
            return false;
        }
        const auto& expected = gs.vram();
        const auto mismatch = std::mismatch(expected.begin(), expected.end(), actual.begin());
        if (mismatch.first != expected.end())
        {
            std::fprintf(stderr, "GS CPU/GPU transfer mismatch PSM=%02x offset=%zx CPU=%02x GPU=%02x\n",
                format.m_psm, static_cast<size_t>(mismatch.first - expected.begin()),
                static_cast<unsigned>(*mismatch.first), static_cast<unsigned>(*mismatch.second));
            return false;
        }
        std::printf("GS CPU/GPU transfer PSM=%02x: PASS\n", format.m_psm);
    }
    // Invalid requests must not touch the destination.
    const auto original = actual;
    return !gpu::download_checked(actual.data(), 1u, 4u) &&
           !gpu::download_checked(actual.data(), kMemorySize, 4u) &&
           !gpu::download_checked(nullptr) && actual == original;
}
}

int main()
{
    using ps2::gs::gpu::DisplaySource;
    ee::c_ee_webgpu_bridge bridge{};
    if (!bridge.init())
    {
        // No WebGPU adapter available on this runner.
        // CTest may report this as skipped; strict CI can require one.
        std::fprintf(stderr, "native WebGPU initialization failed\n");
        return std::getenv("EE_WEBGPU_REQUIRE_ADAPTER") ? 1 : 77;
    }

    ps2::gs::State state{};
    state.bitbltbuf = uint64_t{1} << 48u; // DBP=0, DBW=1, PSMCT32
    state.trxpos = 0u;
    state.trxreg = 1u | (uint64_t{1} << 32u); // one pixel
    state.trxdir = 0u;
    const uint8_t pixel[16] = {
        0x33u, 0x66u, 0x99u, 0xffu, 0u, 0u, 0u, 0u,
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
    };
    ps2::gs::gpu::image_begin(state);
    ps2::gs::gpu::image_data(pixel, sizeof(pixel));
    ps2::gs::gpu::flush();

    const DisplaySource source{
        0u, 1u, ps2::gs::PSMCT32, 0u, 0u, 1u, 1u, false
    };
    std::vector<uint32_t> rgba;
    uint32_t width = 0u, height = 0u;
    if (!ps2::gs::gpu::display(source, rgba, width, height) ||
        width != 1u || height != 1u || rgba.size() != 1u ||
        (rgba[0] & 0x00ffffffu) == 0u)
    {
        std::fprintf(stderr,
            "native GS WebGPU upload/display failed: %ux%u pixels=%zu"
            " first=%08x\n",
            width, height, rgba.size(), rgba.empty() ? 0u : rgba.front());
        return 1;
    }

    // Exercise the actual raster compute pipeline, not merely the
    // host-to-local upload/GS display compute pipelines. The 1x1
    // point primitive is written at (2,2), away from the texture upload.
    state.prim = 0u; // point
    state.prmodecont = 1u;
    state.ctx[0].frame = uint64_t{1} << 16u; // FBW=1, PSMCT32
    state.ctx[0].scissor =
        (uint64_t{63} << 16u) | (uint64_t{63} << 48u);
    ps2::gs::Vertex vertex{};
    vertex.x = 32u;
    vertex.y = 32u;
    vertex.z = 0u;
    vertex.r = 0x11u;
    vertex.g = 0x55u;
    vertex.b = 0xaau;
    vertex.a = 0x80u;
    vertex.q = 1.0f;
    ps2::gs::gpu::draw(state, &vertex, 1);
    ps2::gs::gpu::flush();

    const DisplaySource raster_source{
        0u, 1u, ps2::gs::PSMCT32, 0u, 0u, 4u, 4u, false
    };
    rgba.clear();
    width = height = 0u;
    if (!ps2::gs::gpu::display(
            raster_source, rgba, width, height) ||
        width != 4u || height != 4u || rgba.size() != 16u ||
        (rgba[2u + 2u * 4u] & 0x00ffffffu) == 0u)
    {
        std::fprintf(stderr,
            "native GS WebGPU point raster failed: %ux%u pixels=%zu "
            "at(2,2)=%08x\n",
            width, height, rgba.size(),
            rgba.size() >= 11u ? rgba[10u] : 0u);
        return 1;
    }

    // The CPU reference never rasterized the point above. Guest reverse
    // DMA must nevertheless observe its actual GPU color, with zero
    // padding, rather than returning CPU mirror zeros.
    {
        ee::c_ee_gs_state gs{};
        gs.bind_committed_event_observer(&bridge,
            &ee::c_ee_webgpu_bridge::committed_gif_events);
        gs.bind_vram_readback(&bridge, &ee::c_ee_webgpu_bridge::read_vram);
        std::array<uint8_t, 16> downloaded{};
        if (!write_register(gs, 0x50u, uint64_t{1} << 16u) ||
            !write_register(gs, 0x51u, 2u | (uint64_t{2} << 16u)) ||
            !write_register(gs, 0x52u, 1u | (uint64_t{1} << 32u)) ||
            !write_register(gs, 0x53u, 1u) ||
            !gs.read_image(downloaded.data(), 1u) ||
            downloaded[0] != 0x11u || downloaded[1] != 0x55u ||
            downloaded[2] != 0xaau ||
            !std::all_of(downloaded.begin() + 4, downloaded.end(),
                [](uint8_t value) { return value == 0u; }))
        {
            std::fprintf(stderr, "GPU raster -> guest reverse DMA failed\n");
            return 1;
        }
        std::puts("GPU raster -> guest reverse DMA: PASS");
    }

    if (!compare_transfer_formats(bridge))
    {
        return 1;
    }

    std::printf("native GS WebGPU compute: PASS "
                "(upload=%08x point=%08x)\n", rgba[0], rgba[10u]);
    return 0;
}
