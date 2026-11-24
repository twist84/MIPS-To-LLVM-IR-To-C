#include "ee_webgpu_bridge.h"
#include "ee_runtime_profile.h"
#include "ee_gs_presentation_probe.h"

#include "ps2_webgpu/gs/gpu/gpu.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

namespace ee
{
namespace
{
uint32_t lane32(uint64_t lo, uint64_t hi, uint32_t lane)
{
    const uint64_t word = lane < 2u ? lo : hi;
    return static_cast<uint32_t>(word >> ((lane & 1u) * 32u));
}

float float_bits(uint32_t bits)
{
    float value = 0.0f;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
bool make_display_source(
    const s_ee_gs_display_registers& registers, uint32_t crt_mode,
    ps2::gs::gpu::DisplaySource& src)
{
    if ((registers.m_pmode & 3u) == 0u ||
        (crt_mode != 2u && crt_mode != 3u))
    {
        return false;
    }
    const uint32_t circuit = (registers.m_pmode & 2u) != 0u ? 1u : 0u;
    const uint64_t fb = registers.m_dispfb[circuit];
    const uint64_t disp = registers.m_display[circuit];
    const uint32_t base = (static_cast<uint32_t>(fb) & 0x1ffu) * 32u;
    const uint32_t fbw = static_cast<uint32_t>((fb >> 9u) & 0x3fu);
    const uint32_t psm = static_cast<uint32_t>((fb >> 15u) & 0x1fu);
    const uint32_t dbx = static_cast<uint32_t>((fb >> 32u) & 0x7ffu);
    const uint32_t dby = static_cast<uint32_t>((fb >> 43u) & 0x7ffu);
    if (psm != 0u && psm != 1u && psm != 2u && psm != 0x0au)
    {
        return false;
    }

    const uint32_t magh = static_cast<uint32_t>((disp >> 23u) & 0xfu) + 1u;
    const uint32_t magv = static_cast<uint32_t>((disp >> 27u) & 3u) + 1u;
    const uint32_t dw = static_cast<uint32_t>((disp >> 32u) & 0xfffu) + 1u;
    const uint32_t raw_dh =
        static_cast<uint32_t>((disp >> 44u) & 0x7ffu) + 1u;
    const uint32_t dy = static_cast<uint32_t>((disp >> 12u) & 0x7ffu);
    const bool pal = crt_mode == 3u;
    const uint32_t scale = (registers.m_smode2 & 1u) != 0u ? 2u : 1u;
    const uint32_t lines = (pal ? 288u : 240u) * scale;
    const uint32_t top = (pal ? 36u : 25u) * scale;
    const uint32_t span = (pal ? 256u : 224u) * scale;
    const uint32_t below = dy > top ? std::min(dy - top, span) : 0u;
    const uint32_t dh = std::min(raw_dh,
        std::min(lines, below != 0u ? span - below : lines));
    const uint32_t w = dw / magh;
    const uint32_t h = dh / magv;
    if (w == 0u || h == 0u || w > 2048u || h > 2048u ||
        dbx + w > 2048u || dby + h > 2048u)
    {
        return false;
    }

    src = ps2::gs::gpu::DisplaySource{
        base, fbw, psm, dbx, dby, w, h,
        (registers.m_smode2 & 3u) == 3u,
        (registers.m_smode2 & 1u) != 0u
    };
    return true;
}
} // namespace

bool c_ee_webgpu_bridge::init()
{
    if (m_ready)
    {
        return true;
    }

    // Native WebGPU compute mode, no window, SDL, JavaScript, or browser.
    m_ready = ps2::gs::gpu::init(ps2::gs::gpu::Mode::primary);
    if (m_ready)
    {
        m_state = {};
        m_state.q = 1.0f;
        m_clut_cbp0 = m_clut_cbp1 = 0xffffffffu;
        ++ps2::gs::g_setup_generation;
    }
    return m_ready;
}

bool c_ee_webgpu_bridge::active() const
{
    return m_ready;
}

uint64_t c_ee_webgpu_bridge::rendered_primitives() const
{
    return m_published_draw_count.load(std::memory_order_acquire);
}

bool c_ee_webgpu_bridge::enable_async()
{
    if (!m_ready || m_worker != nullptr)
    {
        return false;
    }
    try
    {
        m_worker = std::make_unique<c_ee_gs_worker>(this,
            [](void* user, const s_ee_gif_event* events, size_t count,
                const uint8_t* data, uint32_t qwc)
            {
                static_cast<c_ee_webgpu_bridge*>(user)->consume_now(
                    events, count, data, qwc);
            });
    }
    catch (...)
    {
        return false;
    }
    return true;
}

bool c_ee_webgpu_bridge::worker_healthy() const
{
    return !m_host_failed && (m_worker == nullptr || m_worker->healthy());
}

bool c_ee_webgpu_bridge::flush_committed()
{
    // The EE is the sole producer. No new work can be submitted between
    // this fence and the caller's readback/present/resize operation.
    const bool drained = m_worker == nullptr || m_worker->drain();
    return drained && !m_host_failed;
}

bool c_ee_webgpu_bridge::resize_surface(uint32_t width, uint32_t height)
{
    if (m_surface_width == width && m_surface_height == height)
    {
        return worker_healthy();
    }
    if (!flush_committed())
    {
        return false;
    }
    ps2::gs::gpu::resize_surface(width, height);
    m_surface_width = width;
    m_surface_height = height;
    return true;
}

void c_ee_webgpu_bridge::write_worker_statistics(FILE* output) const
{
    if (output != nullptr && m_worker != nullptr)
    {
        const auto stats = m_worker->statistics();
        std::fprintf(output,
            "GS async worker: submitted=%llu completed=%llu oversized=%llu "
            "execution_ns=%llu backpressure_ns=%llu backpressure_waits=%llu "
            "peak_bytes=%llu peak_jobs=%llu event_bytes=%llu image_bytes=%llu healthy=%u\n",
            static_cast<unsigned long long>(stats.m_submitted),
            static_cast<unsigned long long>(stats.m_completed),
            static_cast<unsigned long long>(m_oversized_batches),
            static_cast<unsigned long long>(stats.m_execution_ns),
            static_cast<unsigned long long>(stats.m_backpressure_ns),
            static_cast<unsigned long long>(stats.m_backpressure_waits),
            static_cast<unsigned long long>(stats.m_peak_bytes),
            static_cast<unsigned long long>(stats.m_peak_jobs),
            static_cast<unsigned long long>(stats.m_event_bytes),
            static_cast<unsigned long long>(stats.m_image_bytes),
            worker_healthy() ? 1u : 0u);
    }
}

void c_ee_webgpu_bridge::kick(bool draw)
{
    auto& state = m_state;
    const uint32_t primitive = static_cast<uint32_t>(state.prim & 7u);
    const int needed =
        primitive == 0u || primitive == 7u ? 1 :
        primitive == 1u || primitive == 2u || primitive == 6u ? 2 : 3;
    if (state.count < needed)
    {
        return;
    }

    if (draw && primitive != 7u)
    {
        ps2::gs::gpu::draw(state, state.queue, needed);
        ++m_draw_count;
    }

    switch (primitive)
    {
    case 2u: // line strip
        state.queue[0] = state.queue[1];
        state.count = 1;
        break;
    case 4u: // triangle strip
        state.queue[0] = state.queue[1];
        state.queue[1] = state.queue[2];
        state.count = 2;
        break;
    case 5u: // triangle fan
        state.queue[1] = state.queue[2];
        state.count = 2;
        break;
    default:
        state.count = 0;
        break;
    }
}

void c_ee_webgpu_bridge::vertex(
    uint32_t x, uint32_t y, uint32_t z, uint8_t f, bool draw)
{
    auto& state = m_state;
    if (state.count >= 3)
    {
        state.count = 2;
    }
    auto& v = state.queue[state.count++];
    v.x = x;
    v.y = y;
    v.z = z;
    v.f = f;
    v.r = static_cast<uint8_t>(state.rgbaq);
    v.g = static_cast<uint8_t>(state.rgbaq >> 8u);
    v.b = static_cast<uint8_t>(state.rgbaq >> 16u);
    v.a = static_cast<uint8_t>(state.rgbaq >> 24u);
    v.q = float_bits(static_cast<uint32_t>(state.rgbaq >> 32u));
    v.s = float_bits(static_cast<uint32_t>(state.st));
    v.t = float_bits(static_cast<uint32_t>(state.st >> 32u));
    v.u = static_cast<uint32_t>(state.uv) & 0x3fffu;
    v.v = static_cast<uint32_t>(state.uv >> 16u) & 0x3fffu;
    kick(draw);
}

void c_ee_webgpu_bridge::refresh_clut(uint64_t tex0)
{
    const uint32_t cld = static_cast<uint32_t>((tex0 >> 61u) & 7u);
    const uint32_t cbp = static_cast<uint32_t>((tex0 >> 37u) & 0x3fffu);
    if (cld == 0u || cld >= 6u ||
        (cld == 4u && m_clut_cbp0 == cbp) ||
        (cld == 5u && m_clut_cbp1 == cbp))
    {
        return;
    }
    ps2::gs::gpu::load_clut(tex0, m_state.texclut);
    if (cld == 2u || cld == 4u)
    {
        m_clut_cbp0 = cbp;
    }
    else if (cld == 3u || cld == 5u)
    {
        m_clut_cbp1 = cbp;
    }
}

void c_ee_webgpu_bridge::register_write(uint32_t reg, uint64_t value)
{
    auto& s = m_state;
    // This mirrors the original reference GS register dispatch.
    // Only vertex/color/texture-coordinate writes preserve setup cache.
    switch (reg)
    {
    case 0x01u:
    case 0x02u:
    case 0x03u:
    case 0x04u:
    case 0x05u:
    case 0x0au:
    case 0x0cu:
    case 0x0du:
        break;
    default:
        ++ps2::gs::g_setup_generation;
        break;
    }

    switch (reg)
    {
    case 0x00u: s.prim = value; s.count = 0; break;
    case 0x01u: s.rgbaq = value; break;
    case 0x02u: s.st = value; break;
    case 0x03u: s.uv = value; break;
    case 0x04u:
    case 0x0cu:
        vertex(static_cast<uint32_t>(value) & 0xffffu,
               static_cast<uint32_t>(value >> 16u) & 0xffffu,
               static_cast<uint32_t>(value >> 32u) & 0xffffffu,
               static_cast<uint8_t>(value >> 56u), reg == 0x04u);
        break;
    case 0x05u:
    case 0x0du:
        vertex(static_cast<uint32_t>(value) & 0xffffu,
               static_cast<uint32_t>(value >> 16u) & 0xffffu,
               static_cast<uint32_t>(value >> 32u),
               static_cast<uint8_t>(s.fog >> 56u), reg == 0x05u);
        break;
    case 0x06u: case 0x07u:
        s.ctx[reg - 0x06u].tex0 = value;
        refresh_clut(value);
        break;
    case 0x08u: case 0x09u:
        s.ctx[reg - 0x08u].clamp = value;
        break;
    case 0x0au: s.fog = value; break;
    case 0x14u: case 0x15u:
        s.ctx[reg - 0x14u].tex1 = value;
        break;
    case 0x16u: case 0x17u:
    {
        auto& c = s.ctx[reg - 0x16u];
        constexpr uint64_t k_mask = 0xffffffe003f00000ull;
        c.tex0 = (c.tex0 & ~k_mask) | (value & k_mask);
        refresh_clut(c.tex0);
        break;
    }
    case 0x18u: case 0x19u:
        s.ctx[reg - 0x18u].xyoffset = value;
        break;
    case 0x1au: s.prmodecont = value; break;
    case 0x1bu: s.prmode = value; break;
    case 0x1cu: s.texclut = value; break;
    case 0x22u: s.scanmsk = value; break;
    case 0x34u: case 0x35u:
        s.ctx[reg - 0x34u].miptbp1 = value;
        break;
    case 0x36u: case 0x37u:
        s.ctx[reg - 0x36u].miptbp2 = value;
        break;
    case 0x3bu: s.texa = value; break;
    case 0x3du: s.fogcol = value; break;
    case 0x3fu: ps2::gs::gpu::texflush(); break;
    case 0x40u: case 0x41u:
        s.ctx[reg - 0x40u].scissor = value;
        break;
    case 0x42u: case 0x43u:
        s.ctx[reg - 0x42u].alpha = value;
        break;
    case 0x44u: s.dimx = value; break;
    case 0x45u: s.dthe = value; break;
    case 0x46u: s.colclamp = value; break;
    case 0x47u: case 0x48u:
        s.ctx[reg - 0x47u].test = value;
        break;
    case 0x49u: s.pabe = value; break;
    case 0x4au: case 0x4bu:
        s.ctx[reg - 0x4au].fba = value;
        break;
    case 0x4cu: case 0x4du:
        s.ctx[reg - 0x4cu].frame = value;
        break;
    case 0x4eu: case 0x4fu:
        s.ctx[reg - 0x4eu].zbuf = value;
        break;
    case 0x50u: s.bitbltbuf = value; break;
    case 0x51u: s.trxpos = value; break;
    case 0x52u: s.trxreg = value; break;
    case 0x53u:
        s.trxdir = value;
        if ((value & 3u) == 0u)
        {
            ps2::gs::gpu::image_begin(s);
        }
        else if ((value & 3u) == 2u)
        {
            ps2::gs::gpu::local_copy(s);
        }
        break;
    case 0x54u:
    {
        uint8_t bytes[8]{};
        for (uint32_t i = 0; i < 8u; ++i)
        {
            bytes[i] = static_cast<uint8_t>(value >> (i * 8u));
        }
        ps2::gs::gpu::image_data(bytes, 8u);
        break;
    }
    default:
        break;
    }
}

void c_ee_webgpu_bridge::packed_write(
    uint32_t reg, uint64_t low, uint64_t high)
{
    const uint32_t a = lane32(low, high, 0u);
    const uint32_t b = lane32(low, high, 1u);
    const uint32_t c = lane32(low, high, 2u);
    const uint32_t d = lane32(low, high, 3u);
    switch (reg)
    {
    case 0x00u:
        register_write(0x00u, low & 0x7ffu);
        break;
    case 0x01u:
    {
        uint32_t qbits = 0u;
        std::memcpy(&qbits, &m_state.q, sizeof(qbits));
        register_write(0x01u,
            (a & 0xffu) | (static_cast<uint64_t>(b & 0xffu) << 8u) |
            (static_cast<uint64_t>(c & 0xffu) << 16u) |
            (static_cast<uint64_t>(d & 0xffu) << 24u) |
            (static_cast<uint64_t>(qbits) << 32u));
        break;
    }
    case 0x02u:
        register_write(0x02u, low);
        m_state.q = float_bits(c);
        break;
    case 0x03u:
        register_write(0x03u,
            (a & 0x3fffu) | (static_cast<uint64_t>(b & 0x3fffu) << 16u));
        break;
    case 0x04u: case 0x0cu:
        register_write(
            (reg == 0x0cu || ((high >> 47u) & 1u)) ? 0x0cu : 0x04u,
            (a & 0xffffu) | (static_cast<uint64_t>(b & 0xffffu) << 16u) |
            (static_cast<uint64_t>((c >> 4u) & 0xffffffu) << 32u) |
            (static_cast<uint64_t>((d >> 4u) & 0xffu) << 56u));
        break;
    case 0x05u: case 0x0du:
        register_write(
            (reg == 0x0du || ((high >> 47u) & 1u)) ? 0x0du : 0x05u,
            (a & 0xffffu) | (static_cast<uint64_t>(b & 0xffffu) << 16u) |
            (static_cast<uint64_t>(c) << 32u));
        break;
    case 0x0au:
        register_write(0x0au, static_cast<uint64_t>((d >> 4u) & 0xffu) << 56u);
        break;
    case 0x0eu:
        register_write(static_cast<uint32_t>(high & 0xffu), low);
        break;
    case 0x0fu:
        break;
    default:
        register_write(reg, low);
        break;
    }
}

void c_ee_webgpu_bridge::consume(
    const s_ee_gif_event* events, size_t count,
    const uint8_t* data, uint32_t qwc)
{
    if (m_worker == nullptr)
    {
        consume_now(events, count, data, qwc);
        return;
    }
    if (!worker_healthy() || count == 0u)
    {
        return;
    }

    using result = c_ee_gs_worker::e_submit;
    const auto submitted = m_worker->submit(events, count, data, qwc);
    if (submitted == result::accepted)
    {
        return;
    }
    if (submitted == result::too_large && flush_committed())
    {
        // The borrowed source remains valid for this callback. Never
        // exceed queue storage just to preserve asynchronous execution.
        ++m_oversized_batches;
        try
        {
            consume_now(events, count, data, qwc);
            return;
        }
        catch (...)
        {
            // Observer failure is a host runtime failure, not a new
            // guest DMA outcome. Report it at the next boundary.
        }
    }
    m_host_failed = true;
}

void c_ee_webgpu_bridge::consume_now(
    const s_ee_gif_event* events, size_t count,
    const uint8_t* data, uint32_t qwc)
{
    ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::webgpu_bridge, count);
    if (!m_ready || (events == nullptr && count != 0u) ||
        (data == nullptr && qwc != 0u))
    {
        return;
    }

    const size_t size = static_cast<size_t>(qwc) * 16u;
    for (size_t i = 0u; i < count; ++i)
    {
        const auto& e = events[i];
        switch (e.m_type)
        {
        case e_ee_gif_event_type::tag:
            m_state.q = 1.0f;
            break;
        case e_ee_gif_event_type::primitive:
            register_write(0x00u, e.m_word0);
            break;
        case e_ee_gif_event_type::packed:
            packed_write(e.m_register, e.m_word0, e.m_word1);
            break;
        case e_ee_gif_event_type::reglist:
            register_write(e.m_register, e.m_word0);
            break;
        case e_ee_gif_event_type::image:
            if (e.m_qwc != 0u && e.m_byte_offset <= size &&
                static_cast<size_t>(e.m_qwc) <=
                    (size - e.m_byte_offset) / 16u)
            {
                ps2::gs::gpu::image_data(
                    data + e.m_byte_offset, static_cast<size_t>(e.m_qwc) * 16u);
            }
            break;
        default:
            break;
        }
    }
    m_published_draw_count.store(m_draw_count, std::memory_order_release);
}

void c_ee_webgpu_bridge::committed_gif_events(
    void* user, const s_ee_gif_event* events, size_t count,
    const uint8_t* data, uint32_t qwc)
{
    if (user != nullptr)
    {
        static_cast<c_ee_webgpu_bridge*>(user)->consume(
            events, count, data, qwc);
    }
}

bool c_ee_webgpu_bridge::read_vram(void* user, uint8_t* memory, size_t bytes)
{
    auto* bridge = static_cast<c_ee_webgpu_bridge*>(user);
    return bridge != nullptr && bridge->active() &&
        bytes == ps2::gs::kMemorySize && bridge->flush_committed() &&
        ps2::gs::gpu::download_checked(memory);
}

bool c_ee_webgpu_bridge::read_clut(void* user, uint32_t* entries, size_t count)
{
    auto* bridge = static_cast<c_ee_webgpu_bridge*>(user);
    return bridge != nullptr && bridge->active() &&
        bridge->flush_committed() &&
        ps2::gs::gpu::read_clut_checked(entries, count);
}

bool c_ee_webgpu_bridge::supports_display(
    const s_ee_gs_display_registers& registers, uint32_t crt_mode) const
{
    ps2::gs::gpu::DisplaySource src{};
    return m_ready && make_display_source(registers, crt_mode, src);
}

bool c_ee_webgpu_bridge::present(
    const s_ee_gs_display_registers& registers, uint32_t crt_mode,
    uint64_t guest_field)
{
    ps2::gs::gpu::DisplaySource src{};
    if (!m_ready || !make_display_source(registers, crt_mode, src))
    {
        return false;
    }
    src.guest_field = guest_field;
    return flush_committed() && ps2::gs::gpu::present(src);
}

bool c_ee_webgpu_bridge::readback(
    const s_ee_gs_display_registers& registers, uint32_t crt_mode,
    std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height)
{
    ps2::gs::gpu::DisplaySource src{};
    return m_ready && make_display_source(registers, crt_mode, src)
        && flush_committed() && ps2::gs::gpu::display(src, pixels, width, height);
}

bool c_ee_webgpu_bridge::readback_frame(
    uint64_t frame, const s_ee_gs_display_registers& registers,
    uint32_t crt_mode, std::vector<uint32_t>& pixels,
    uint32_t& width, uint32_t& height)
{
    ps2::gs::gpu::DisplaySource src{};
    const s_ee_gs_frame_target target = decode_gs_frame_target(frame);
    if (!m_ready || !target.m_supported ||
        !make_display_source(registers, crt_mode, src))
    {
        return false;
    }
    src.fbp = target.m_base;
    src.fbw = target.m_fbw;
    src.psm = target.m_psm;
    return flush_committed() && ps2::gs::gpu::display(src, pixels, width, height);
}
} // namespace ee
