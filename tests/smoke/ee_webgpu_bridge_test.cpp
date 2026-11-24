#include "ee_webgpu_bridge.h"
#include "ps2_webgpu/gs/gpu/gpu.hpp"
#include "ps2_webgpu/gs/gpu/native_viewport.hpp"
#include "ps2_webgpu/gs/gpu/native_deinterlace.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
int s_draws = 0;
int s_images = 0;
int s_upload_bytes = 0;
int s_local_copies = 0;
int s_clut_loads = 0;
int s_texflushes = 0;
int s_presents = 0;
int s_resizes = 0;
uint32_t s_surface_width = 0u;
uint32_t s_surface_height = 0u;
ps2::gs::State s_last_state{};
ps2::gs::Vertex s_last_vertex{};
ps2::gs::gpu::DisplaySource s_last_source{};

void fail(const char* message)
{
    std::fprintf(stderr, "native WebGPU event adapter: %s\n", message);
}
} // namespace

namespace ps2::gs
{
uint32_t g_setup_generation = 0u;
}

namespace ps2::gs::gpu
{
bool init(Mode mode)
{
    return mode == Mode::primary;
}

void draw(const State& state, const Vertex* vertices, int count)
{
    if (vertices != nullptr && count > 0)
    {
        ++s_draws;
        s_last_state = state;
        s_last_vertex = vertices[0];
    }
}

void image_begin(const State&)
{
    ++s_images;
}

void image_data(const uint8_t*, size_t bytes)
{
    s_upload_bytes += static_cast<int>(bytes);
}

void local_copy(const State&)
{
    ++s_local_copies;
}

void load_clut(uint64_t, uint64_t)
{
    ++s_clut_loads;
}

bool download_checked(uint8_t*, uint32_t, uint32_t)
{
    return false;
}

bool read_clut_checked(uint32_t*, size_t)
{
    return false;
}

void texflush()
{
    ++s_texflushes;
}

void resize_surface(uint32_t width, uint32_t height)
{
    ++s_resizes;
    s_surface_width = width;
    s_surface_height = height;
}

bool present(const DisplaySource& src)
{
    ++s_presents;
    s_last_source = src;
    return true;
}

bool display(
    const DisplaySource& src, std::vector<uint32_t>& rgba,
    uint32_t& width, uint32_t& height)
{
    s_last_source = src;
    width = src.width;
    height = src.height;
    rgba.assign(static_cast<size_t>(width) * height, 0xff336699u);
    return true;
}
}

int main()
{
    using ps2::gs::gpu::window_viewport;
    using ps2::gs::gpu::parse_display_aspect;
    const char* aspect = "4:3";
    for (const char* value : {"16:9", "native", "1:1", "4:3", "invalid", "", "16:9\n"})
    {
        const char* actual = parse_display_aspect(value, std::strlen(value), aspect);
        const bool valid = std::strcmp(value, "16:9") == 0 || std::strcmp(value, "native") == 0 ||
            std::strcmp(value, "1:1") == 0 || std::strcmp(value, "4:3") == 0;
        if (std::strcmp(actual, valid ? value : aspect) != 0)
        {
            fail("live aspect selection did not preserve the last valid value");
            return 1;
        }
        aspect = actual;
    }
    const auto title = window_viewport(906, 540, 512, 448);
    const auto movie = window_viewport(906, 540, 640, 384);
    const auto narrow = window_viewport(600, 900, 512, 448);
    const auto native = window_viewport(906, 540, 512, 448, "native");
    const auto wide = window_viewport(1280, 720, 512, 448, "16:9");
    const auto square = window_viewport(906, 540, 512, 448, "1:1");
    const auto tiny = window_viewport(1, 1, 0, 0);
    if (title.x != 93 || title.y != 0 || title.width != 720 || title.height != 540 ||
        movie.x != title.x || movie.width != title.width || movie.height != title.height ||
        narrow.x != 0 || narrow.y != 225 || narrow.width != 600 || narrow.height != 450 ||
        native.width != 617 || native.height != 540 ||
        wide.width != 1280 || wide.height != 720 ||
        square.width != 540 || square.height != 540 ||
        tiny.x != 0 || tiny.y != 0 || tiny.width != 1 || tiny.height != 1)
    {
        fail("host display aspect or viewport fitting is incorrect");
        return 1;
    }
    using ps2::gs::gpu::e_deinterlace_mode;
    using ps2::gs::gpu::e_field_method;
    using ps2::gs::gpu::parse_deinterlace_mode;
    using ps2::gs::gpu::select_field_presentation;
    for (uint32_t index = 2u; index <= 9u; ++index)
    {
        const char value[] = {static_cast<char>('0' + index), '\0'};
        const auto mode = parse_deinterlace_mode(value);
        const auto field = select_field_presentation(mode, true, true, 5u, 4u, true);
        const auto next = select_field_presentation(mode, true, true, 6u, 5u, true);
        const auto gap = select_field_presentation(mode, true, true, 7u, 5u, true);
        const auto progressive = select_field_presentation(mode, false, false, 5u, 4u, true);
        if (static_cast<uint32_t>(mode) != index ||
            static_cast<uint32_t>(field.m_method) != 1u + (index - 2u) / 2u ||
            field.m_parity != (index & 1u) || next.m_parity == field.m_parity ||
            !field.m_history || gap.m_history || gap.m_method != e_field_method::bob ||
            progressive.m_method != e_field_method::off)
        {
            fail("deinterlace mode, field parity or history selection is incorrect");
            return 1;
        }
    }
    // Off must remain off even with valid history, interlaced half frames,
    // skipped fields or the legacy force-blend override.
    for (uint64_t field = 1u; field <= 4u; ++field)
    {
        const auto disabled = select_field_presentation(e_deinterlace_mode::off,
            true, true, field, field - 1u, true, true, false);
        if (disabled.m_method != e_field_method::off)
        {
            fail("off mode unexpectedly selected a field filter");
            return 1;
        }
    }
    const auto automatic = select_field_presentation(e_deinterlace_mode::automatic, true, true, 2u, 1u, true);
    const auto off = select_field_presentation(e_deinterlace_mode::off, true, true, 2u, 1u, true);
    if (automatic.m_method != e_field_method::blend || off.m_method != e_field_method::off ||
        parse_deinterlace_mode("invalid") != e_deinterlace_mode::automatic)
    {
        fail("automatic/off deinterlace mode selection is incorrect");
        return 1;
    }
    ee::c_ee_webgpu_bridge bridge{};
    if (!bridge.init() || !bridge.active())
    {
        fail("cannot initialize native-only mode");
        return 1;
    }
    ee::s_ee_gif_event events[5]{};
    events[0].m_type = ee::e_ee_gif_event_type::tag;
    events[1].m_type = ee::e_ee_gif_event_type::primitive;
    events[1].m_word0 = 0u; // point primitive
    events[2].m_type = ee::e_ee_gif_event_type::reglist;
    events[2].m_register = 0x01u; // RGBAQ, 1.0 Q
    events[2].m_word0 = 0x3f80000044332211ull;
    events[3].m_type = ee::e_ee_gif_event_type::reglist;
    events[3].m_register = 0x4cu; // FRAME_1
    events[3].m_word0 = (uint64_t{1} << 16u);
    events[4].m_type = ee::e_ee_gif_event_type::reglist;
    events[4].m_register = 0x05u; // XYZ2 32-bit Z
    events[4].m_word0 = 0x0000001100300020ull;
    uint8_t bytes[16]{};
    bridge.consume(events, 5u, bytes, 1u);
    if (s_draws != 1 || bridge.rendered_primitives() != 1u ||
        s_last_vertex.x != 0x20u || s_last_vertex.y != 0x30u ||
        s_last_vertex.z != 0x11u ||
        s_last_vertex.r != 0x11u || s_last_vertex.g != 0x22u ||
        s_last_vertex.b != 0x33u || s_last_vertex.a != 0x44u)
    {
        fail("point vertex or RGBAQ state was not forwarded");
        return 1;
    }
    bridge.consume(nullptr, 1u, bytes, 1u);
    if (s_draws != 1)
    {
        fail("invalid mirror input was not discarded");
        return 1;
    }

    ee::s_ee_gif_event transfers[5]{};
    for (auto& e : transfers)
    {
        e.m_type = ee::e_ee_gif_event_type::reglist;
    }
    transfers[0].m_register = 0x50u;
    transfers[0].m_word0 = 1ull << 48u;
    transfers[1].m_register = 0x52u;
    transfers[1].m_word0 = 4u | (uint64_t{2} << 32u);
    transfers[2].m_register = 0x53u;
    transfers[2].m_word0 = 0u; // host-to-local
    transfers[3].m_type = ee::e_ee_gif_event_type::image;
    transfers[3].m_byte_offset = 0u;
    transfers[3].m_qwc = 1u;
    transfers[4].m_register = 0x3fu; // TEXFLUSH
    bridge.consume(transfers, 5u, bytes, 1u);
    if (s_images != 1 || s_upload_bytes != 16 || s_texflushes != 1)
    {
        fail("native upload/texflush path was not forwarded");
        return 1;
    }
    ee::s_ee_gif_event copy{};
    copy.m_type = ee::e_ee_gif_event_type::reglist;
    copy.m_register = 0x53u;
    copy.m_word0 = 2u;
    ee::c_ee_webgpu_bridge::committed_gif_events(
        &bridge, &copy, 1u, bytes, 1u);
    if (s_local_copies != 1)
    {
        fail("native local VRAM copy path was not forwarded");
        return 1;
    }

    // Conditional loads share CBP0/CBP1 across TEX0 and TEX2.
    const auto cld = [&](uint32_t reg, uint32_t mode, uint32_t cbp)
    {
        ee::s_ee_gif_event event{};
        event.m_type = ee::e_ee_gif_event_type::reglist;
        event.m_register = reg;
        event.m_word0 = (uint64_t{mode} << 61u) |
            (uint64_t{cbp} << 37u) | (uint64_t{0x13u} << 20u);
        bridge.consume(&event, 1u, bytes, 1u);
    };
    cld(6u, 0u, 1u);
    cld(6u, 2u, 1u);
    cld(7u, 4u, 1u);
    cld(7u, 4u, 2u);
    cld(0x16u, 3u, 2u);
    cld(0x17u, 5u, 2u);
    cld(0x17u, 5u, 3u);
    cld(6u, 6u, 4u);
    cld(6u, 7u, 4u);
    cld(6u, 1u, 4u);
    if (s_clut_loads != 5)
    {
        fail("CLD reload and shared CBP controls are incorrect");
        return 1;
    }

    ee::s_ee_gs_display_registers display{};
    display.m_pmode = 1u;
    display.m_dispfb[0] = 1ull << 9u;
    display.m_display[0] = (63ull << 32u) | (31ull << 44u);
    std::vector<uint32_t> output;
    uint32_t width = 0u, height = 0u;
    if (!bridge.readback(display, 2u, output, width, height) ||
        width != 64u || height != 32u || output.size() != 2048u ||
        s_last_source.fbw != 1u ||
        s_last_source.width != width ||
        s_last_source.height != height ||
        output.front() != 0xff336699u)
    {
        fail("native compute-display source was not selected");
        return 1;
    }
    if (!bridge.present(display, 2u) || s_presents != 1 ||
        s_last_source.fbw != 1u ||
        s_last_source.width != 64u || s_last_source.height != 32u)
    {
        fail("valid GS scanout display was not presented");
        return 1;
    }
    display.m_smode2 = 3u;
    if (!bridge.present(display, 2u, 42u) || !s_last_source.interlaced ||
        !s_last_source.halved || s_last_source.guest_field != 42u)
    {
        fail("guest field and interlace state were not forwarded");
        return 1;
    }
    display.m_dispfb[0] |= 0x13ull << 15u;
    if (bridge.readback(display, 2u, output, width, height))
    {
        fail("unsupported indexed display source was accepted");
        return 1;
    }
    if (bridge.present(display, 2u) || s_presents != 2)
    {
        fail("unsupported indexed display source was presented");
        return 1;
    }

    if (!bridge.resize_surface(640u, 480u) ||
        !bridge.resize_surface(640u, 480u) ||
        !bridge.resize_surface(800u, 600u) ||
        s_resizes != 2 || s_surface_width != 800u || s_surface_height != 600u)
    {
        fail("surface resize fence or repeated-size suppression");
        return 1;
    }

    std::puts("EE native WebGPU event adapter: PASS");
    return 0;
}
