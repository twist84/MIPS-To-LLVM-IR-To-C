#include "ee_gif.h"
#include "ee_hw_bus.h"
#include "ee_memory.h"
#include "ee_exceptions.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
constexpr uint32_t k_gif = ee::c_ee_dmac::k_gif_base;
constexpr uint32_t k_d_stat = ee::c_ee_dmac::k_stat_address;
constexpr uint32_t k_str = ee::c_ee_dmac::k_chcr_str;
constexpr uint32_t k_ip3 = 1u << (ee::CAUSE_IP_SHIFT + ee::INT_DMAC);
constexpr uint32_t k_gif_mask = 1u << (16u + 2u);
constexpr uint32_t k_gif_status = 1u << 2u;
using t_type = ee::e_ee_gif_event_type;

struct s_capture
{
    bool m_accept = true;
    uint32_t m_calls = 0;
    std::vector<ee::s_ee_gif_event> m_events;
    std::vector<uint8_t> m_image_data;
};

bool on_events(
    void* user, const ee::s_ee_gif_event* events, size_t count,
    const uint8_t* data, uint32_t qwc)
{
    s_capture* capture = static_cast<s_capture*>(user);
    if (capture == nullptr || !capture->m_accept)
    {
        return false;
    }

    ++capture->m_calls;
    for (size_t i = 0; i < count; ++i)
    {
        if (events[i].m_byte_offset >= qwc * 16u)
        {
            return false;
        }
        if (events[i].m_type == t_type::image)
        {
            const size_t first = events[i].m_byte_offset;
            const size_t size = static_cast<size_t>(events[i].m_qwc) * 16u;
            if (first + size > static_cast<size_t>(qwc) * 16u)
            {
                return false;
            }
            capture->m_image_data.insert(
                capture->m_image_data.end(),
                data + first, data + first + size
            );
        }
        capture->m_events.push_back(events[i]);
    }
    return true;
}

int fail(const char* reason)
{
    std::fprintf(stderr, "EE GIF PATH3: %s\n", reason);
    return 1;
}

uint64_t make_tag(
    uint32_t nloop, uint32_t flg, uint32_t nreg,
    bool eop = true, bool pre = false, uint32_t prim = 0)
{
    return (nloop & 0x7fffu) |
        (static_cast<uint64_t>(eop ? 1u : 0u) << 15u) |
        (static_cast<uint64_t>(pre ? 1u : 0u) << 46u) |
        (static_cast<uint64_t>(prim & 0x7ffu) << 47u) |
        (static_cast<uint64_t>(flg & 3u) << 58u) |
        (static_cast<uint64_t>(nreg & 15u) << 60u);
}

void write_tag(
    ee::s_ee_runtime* runtime, uint32_t address,
    uint64_t tag, uint64_t regs)
{
    const ee::s_ee_u128 quad{tag, regs};
    ee::ee_mem_write128(runtime, address, &quad);
}

void write_quad(
    ee::s_ee_runtime* runtime, uint32_t address,
    uint64_t low, uint64_t high)
{
    const ee::s_ee_u128 quad{low, high};
    ee::ee_mem_write128(runtime, address, &quad);
}

void program_gif(
    ee::s_ee_runtime* runtime, uint32_t address, uint32_t qwc)
{
    ee::ee_mem_write32(runtime, k_gif + 0x10u, address);
    ee::ee_mem_write32(runtime, k_gif + 0x20u, qwc);
    ee::ee_mem_write32(runtime, k_gif, k_str);
}

void reset_capture(s_capture& capture)
{
    capture.m_accept = true;
    capture.m_calls = 0u;
    capture.m_events.clear();
    capture.m_image_data.clear();
}

bool is_event(
    const ee::s_ee_gif_event& event, t_type type, uint32_t reg)
{
    return event.m_type == type && event.m_register == reg;
}

} // namespace

int main()
{
    ee::ee_memory ram(0x4000);
    ee::s_ee_cpu_state cpu{};
    ee::c_ee_hw_bus hardware;
    ee::c_ee_gif_path3 parser;
    s_capture capture{};

    hardware.bind_cpu(&cpu);
    hardware.bind_memory(&ram);
    parser.bind_event_sink(&capture, on_events);
    hardware.bind_device_transfer(&parser, ee::c_ee_gif_path3::dma_transfer);

    ee::s_ee_runtime runtime{};
    runtime.m_user = &ram;
    runtime.m_mmio_user = &hardware;
    runtime.m_mmio_read = &ee::c_ee_hw_bus::mmio_read;
    runtime.m_mmio_write = &ee::c_ee_hw_bus::mmio_write;

    cpu.m_cop0[ee::COP0_Status] =
        ee::STATUS_IE | ee::STATUS_EIE | k_ip3;

    // PACKED: two loops of [A+D, UV], PRE=1 synthesizes a PRIM
    // register write. Split the packet across two actual DMAC starts.
    write_tag(&runtime, 0x100u, make_tag(2u, 0u, 2u, true, true, 0x25u),
              0x5eull);
    write_quad(&runtime, 0x110u, 0x1111u, 0x2222u);
    write_quad(&runtime, 0x120u, 0x3333u, 0x4444u);
    write_quad(&runtime, 0x130u, 0x5555u, 0x6666u);
    write_quad(&runtime, 0x140u, 0x7777u, 0x8888u);
    ee::ee_mem_write32(&runtime, k_d_stat, k_gif_mask);
    program_gif(&runtime, 0x100u, 2u);

    if (!parser.state().m_active ||
        parser.state().m_nloop != 2u ||
        parser.state().m_reg != 1u ||
        capture.m_calls != 1u ||
        capture.m_events.size() != 3u ||
        !is_event(capture.m_events[0], t_type::tag, 0u) ||
        !is_event(capture.m_events[1], t_type::primitive, 0u) ||
        capture.m_events[1].m_word0 != 0x25u ||
        !is_event(capture.m_events[2], t_type::packed, 0xeu) ||
        capture.m_events[2].m_word0 != 0x1111u ||
        capture.m_events[2].m_word1 != 0x2222u ||
        ee::ee_mem_read32(&runtime, k_gif + 0x10u) != 0x120u ||
        (cpu.m_cop0[ee::COP0_Cause] & k_ip3) == 0u)
    {
        return fail("first PACKED DMA chunk and PRE/PRIM event");
    }

    ee::ee_mem_write32(&runtime, k_d_stat, k_gif_status);
    program_gif(&runtime, 0x120u, 3u);
    if (parser.state().m_active ||
        parser.state().m_loop != 2u ||
        !parser.state().m_eop ||
        capture.m_calls != 2u ||
        capture.m_events.size() != 6u ||
        !is_event(capture.m_events[3], t_type::packed, 5u) ||
        !is_event(capture.m_events[4], t_type::packed, 0xeu) ||
        !is_event(capture.m_events[5], t_type::packed, 5u) ||
        capture.m_events[5].m_word0 != 0x7777u ||
        ee::ee_mem_read32(&runtime, k_gif + 0x20u) != 0u)
    {
        return fail("PACKED continuation across independent DMA starts");
    }

    // Counters are per parser and count only GS-accepted chunks, not
    // the current GIFtag state or hypothetical host DMA starts.
    if (parser.counters().m_accepted_batches != 2u ||
        parser.counters().m_rejected_batches != 0u ||
        parser.counters().m_input_qwc != 5u ||
        parser.counters().m_tags != 1u ||
        parser.counters().m_primitive_events != 1u ||
        parser.counters().m_packed_events != 4u)
    {
        return fail("PATH3 accepted parser accounting");
    }

    // REGLIST has two 64-bit entries per QWC. With NREG=3 the final
    // high half is padding, and the next quad must be treated as GIFtag.
    hardware.reset();
    parser.reset();
    reset_capture(capture);
    write_tag(&runtime, 0x200u, make_tag(1u, 1u, 3u), 0x321ull);
    write_quad(&runtime, 0x210u, 0xa1u, 0xb2u);
    write_quad(&runtime, 0x220u, 0xc3u, 0xdeadbeefu);
    write_tag(&runtime, 0x230u, make_tag(0u, 0u, 1u), 0u);
    program_gif(&runtime, 0x200u, 2u);
    if (capture.m_events.size() != 3u ||
        !is_event(capture.m_events[1], t_type::reglist, 1u) ||
        !is_event(capture.m_events[2], t_type::reglist, 2u) ||
        parser.state().m_reg != 2u || !parser.state().m_active)
    {
        return fail("reglist first two halfwords");
    }
    program_gif(&runtime, 0x220u, 2u);
    if (capture.m_events.size() != 5u ||
        !is_event(capture.m_events[3], t_type::reglist, 3u) ||
        capture.m_events[3].m_word0 != 0xc3u ||
        capture.m_events[3].m_byte_offset != 0u ||
        capture.m_events[4].m_type != t_type::tag ||
        parser.state().m_active ||
        parser.state().m_nloop != 0u ||
        parser.counters().m_tags != 2u ||
        parser.counters().m_reglist_events != 3u ||
        parser.counters().m_packed_events != 0u)
    {
        return fail("odd REGLIST padding or zero-loop next tag");
    }

    // NREG=0 means SIXTEEN packed registers, not zero.
    hardware.reset();
    parser.reset();
    reset_capture(capture);
    write_tag(&runtime, 0x300u, make_tag(1u, 0u, 0u), 0xfedcba9876543210ull);
    for (uint32_t i = 0u; i < 16u; ++i)
    {
        write_quad(&runtime, 0x310u + i * 16u, i, 0x100u + i);
    }
    program_gif(&runtime, 0x300u, 17u);
    if (capture.m_events.size() != 17u ||
        parser.state().m_active ||
        parser.state().m_nreg != 16u)
    {
        return fail("NREG=0 sixteen-register decoding");
    }
    for (uint32_t i = 0u; i < 16u; ++i)
    {
        if (!is_event(capture.m_events[i + 1u], t_type::packed, i) ||
            capture.m_events[i + 1u].m_word0 != i)
        {
            return fail("REGS nibble ordering for sixteen packed entries");
        }
    }

    // IMAGE and IMAGE2 consume NLOOP qwords, irrespective of NREG.
    // Image data is forwarded using input offsets rather than copied
    // or interpreted as GS framebuffer pixels.
    hardware.reset();
    parser.reset();
    reset_capture(capture);
    write_tag(&runtime, 0x600u, make_tag(3u, 2u, 0u), 0u);
    write_quad(&runtime, 0x610u, 0x0706050403020100ull,
               0x0f0e0d0c0b0a0908ull);
    write_quad(&runtime, 0x620u, 0x1716151413121110ull,
               0x1f1e1d1c1b1a1918ull);
    write_quad(&runtime, 0x630u, 0x2726252423222120ull,
               0x2f2e2d2c2b2a2928ull);
    write_tag(&runtime, 0x640u, make_tag(1u, 3u, 1u), 0u);
    write_quad(&runtime, 0x650u, 0x3736353433323130ull,
               0x3f3e3d3c3b3a3938ull);
    program_gif(&runtime, 0x600u, 3u);
    if (!parser.state().m_active ||
        parser.state().m_loop != 2u ||
        capture.m_events.size() != 2u ||
        capture.m_events[1].m_type != t_type::image ||
        capture.m_events[1].m_qwc != 2u ||
        capture.m_events[1].m_byte_offset != 16u ||
        capture.m_image_data.size() != 32u)
    {
        return fail("IMAGE payload grouping before split");
    }
    program_gif(&runtime, 0x630u, 3u);
    if (parser.state().m_active ||
        parser.state().m_flg != 3u ||
        capture.m_events.size() != 5u ||
        capture.m_events[2].m_type != t_type::image ||
        capture.m_events[2].m_qwc != 1u ||
        capture.m_events[3].m_type != t_type::tag ||
        capture.m_events[4].m_type != t_type::image ||
        capture.m_image_data.size() != 64u)
    {
        return fail("IMAGE continuation and IMAGE2 tag interpretation");
    }
    for (uint32_t i = 0; i < 64u; ++i)
    {
        if (capture.m_image_data[i] != static_cast<uint8_t>(i))
        {
            return fail("IMAGE payload byte ordering");
        }
    }
    if (parser.counters().m_image_events != 3u ||
        parser.counters().m_image_qwc != 4u ||
        parser.counters().m_image_nonzero_bytes != 63u ||
        parser.counters().m_image_payload_fnv1a64 !=
            0x8368214f77995ee5ull)
    {
        return fail("accepted IMAGE payload nonzero count and FNV digest");
    }

    // Rejected GIF IMAGE data must not count as accepted upload content.
    hardware.reset();
    parser.reset();
    reset_capture(capture);
    write_tag(&runtime, 0x700u, make_tag(1u, 2u, 0u), 0u);
    write_quad(&runtime, 0x710u, 0u, 0u);
    capture.m_accept = false;
    program_gif(&runtime, 0x700u, 2u);
    if (parser.counters().m_rejected_batches != 1u ||
        parser.counters().m_image_events != 0u ||
        parser.counters().m_image_nonzero_bytes != 0u ||
        parser.counters().m_image_payload_fnv1a64 != 0u)
    {
        return fail("rejected GIF IMAGE counted as committed content");
    }
    capture.m_accept = true;
    ee::ee_mem_write32(&runtime, k_gif, 0u);
    program_gif(&runtime, 0x700u, 2u);
    if (parser.counters().m_accepted_batches != 1u ||
        parser.counters().m_image_events != 1u ||
        parser.counters().m_image_qwc != 1u ||
        parser.counters().m_image_nonzero_bytes != 0u ||
        parser.counters().m_image_payload_fnv1a64 !=
            0x88201fb960ff6465ull)
    {
        return fail("accepted all-zero IMAGE content provenance");
    }

    // Structural provenance and exact sink payload survive an opt-out
    // of the diagnostic byte scan. No mid-stream partial digest.
    hardware.reset();
    parser.reset();
    reset_capture(capture);
    if (!parser.set_image_payload_digest(false))
    {
        return fail("cannot disable IMAGE byte digest before first batch");
    }
    program_gif(&runtime, 0x600u, 3u);
    program_gif(&runtime, 0x630u, 3u);
    if (parser.state().m_active ||
        parser.counters().m_image_payload_digest_enabled ||
        parser.counters().m_image_events != 3u ||
        parser.counters().m_image_qwc != 4u ||
        parser.counters().m_image_nonzero_bytes != 0u ||
        parser.counters().m_image_payload_fnv1a64 != 0u ||
        capture.m_image_data.size() != 64u ||
        parser.set_image_payload_digest(true))
    {
        return fail("disabled IMAGE digest changed parser or structural provenance");
    }
    for (uint32_t i = 0u; i < 64u; ++i)
    {
        if (capture.m_image_data[i] != static_cast<uint8_t>(i))
        {
            return fail("disabled digest changed IMAGE payload bytes");
        }
    }
    parser.reset();
    if (parser.counters().m_image_payload_digest_enabled ||
        !parser.set_image_payload_digest(true))
    {
        return fail("IMAGE digest configuration reset semantics");
    }

    // A rejected batch cannot advance the parser or the DMA channel.
    // Retrying the same stream after STR is cleared must parse identically.
    hardware.reset();
    parser.reset();
    reset_capture(capture);
    capture.m_accept = false;
    write_tag(&runtime, 0x800u, make_tag(1u, 0u, 1u), 0xeull);
    write_quad(&runtime, 0x810u, 0xaa55u, 0xbb66u);
    program_gif(&runtime, 0x800u, 2u);
    if (parser.state().m_active ||
        parser.state().m_nloop != 0u ||
        !capture.m_events.empty() ||
        ee::ee_mem_read32(&runtime, k_gif + 0x10u) != 0x800u ||
        ee::ee_mem_read32(&runtime, k_gif + 0x20u) != 2u ||
        (ee::ee_mem_read32(&runtime, k_gif) & k_str) == 0u ||
        hardware.dmac().status() != 0u)
    {
        return fail("failed event batch advanced parser or DMAC");
    }
    if (parser.counters().m_accepted_batches != 0u ||
        parser.counters().m_rejected_batches != 1u ||
        parser.counters().m_input_qwc != 0u ||
        parser.counters().m_tags != 0u ||
        parser.counters().m_packed_events != 0u ||
        parser.counters().m_primitive_events != 0u)
    {
        return fail("rejected PATH3 packet counted as committed");
    }
    capture.m_accept = true;
    ee::ee_mem_write32(&runtime, k_gif, 0u);
    program_gif(&runtime, 0x800u, 2u);
    if (parser.state().m_active ||
        capture.m_events.size() != 2u ||
        !is_event(capture.m_events[1], t_type::packed, 0xeu) ||
        capture.m_events[1].m_word0 != 0xaa55u ||
        ee::ee_mem_read32(&runtime, k_gif + 0x10u) != 0x820u ||
        (hardware.dmac().status() & k_gif_status) == 0u)
    {
        return fail("retry after sink refusal failed");
    }

    if (parser.counters().m_accepted_batches != 1u ||
        parser.counters().m_rejected_batches != 1u ||
        parser.counters().m_tags != 1u ||
        parser.counters().m_packed_events != 1u ||
        parser.counters().m_input_qwc != 2u)
    {
        return fail("PATH3 retry committed counters more than once");
    }

    // Explicit sink opt-in and channel identity: a GIF PATH3 parser
    // must not claim to understand VIF0 or VIF1 command streams.
    parser.reset();
    parser.bind_event_sink(nullptr, nullptr);
    if (parser.consume(nullptr, 1u) ||
        parser.consume(nullptr, 0x10000u) ||
        ee::c_ee_gif_path3::dma_transfer(&parser, 1u, nullptr, 1u) ||
        !parser.consume(nullptr, 0u) ||
        parser.state().m_active)
    {
        return fail("invalid, unbound or non-GIF input was accepted");
    }
    hardware.reset();
    program_gif(&runtime, 0x800u, 2u);
    if (hardware.dmac().status() != 0u ||
        ee::ee_mem_read32(&runtime, k_gif + 0x20u) != 2u ||
        (ee::ee_mem_read32(&runtime, k_gif) & k_str) == 0u)
    {
        return fail("unbound GIF parser falsely completed DMA");
    }

    std::puts("EE GIF PATH3 streaming PACKED/REGLIST/IMAGE parser: PASS");
    return 0;
}
