#include "ee_gif.h"
#include "ee_gs.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{

bool check(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "M2 GIF diagnostic regression: %s\n", message);
    }
    return condition;
}

void qword(std::vector<uint8_t>& bytes, uint64_t low, uint64_t high)
{
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        bytes.push_back(static_cast<uint8_t>(low >> (8u * i)));
    }
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        bytes.push_back(static_cast<uint8_t>(high >> (8u * i)));
    }
}

void tag(std::vector<uint8_t>& bytes, uint32_t nloop,
         uint32_t flg, uint32_t nreg, uint64_t regs,
         bool pre = false, uint32_t prim = 0u)
{
    const uint64_t word =
        static_cast<uint64_t>(nloop & 0x7fffu) |
        (1ull << 15u) |
        (static_cast<uint64_t>(pre) << 46u) |
        (static_cast<uint64_t>(prim & 0x7ffu) << 47u) |
        (static_cast<uint64_t>(flg & 3u) << 58u) |
        (static_cast<uint64_t>(nreg & 15u) << 60u);
    qword(bytes, word, regs);
}

void ad(std::vector<uint8_t>& bytes, uint32_t register_address,
        uint64_t value)
{
    qword(bytes, value, register_address);
}

bool feed(ee::c_ee_gif_path3& gif, const std::vector<uint8_t>& bytes)
{
    return !bytes.empty() && bytes.size() % 16u == 0u &&
        gif.consume(bytes.data(), static_cast<uint32_t>(bytes.size() / 16u));
}

} // namespace

int main()
{
    ee::c_ee_gif_path3 gif{};
    ee::c_ee_gs_state gs{};
    gif.bind_event_sink(&gs, &ee::c_ee_gs_state::gif_events);

    // Two separate PRIM writes deliberately reset the *current primitive*
    // vertex count. They must not erase cumulative accepted vertex writes.
    std::vector<uint8_t> packet;
    tag(packet, 1u, 0u, 2u, 0x55u, true, 2u);
    qword(packet, 0x0001000200030004ull, 0u); // PACKED XYZ2 (kick)
    qword(packet, 0x0005000600070008ull, 0u); // PACKED XYZ2 (kick)
    if (!check(feed(gif, packet), "first PACKED PRIM/XYZ batch rejected"))
    {
        return 1;
    }

    packet.clear();
    tag(packet, 1u, 0u, 1u, 0xdu, true, 3u);
    qword(packet, 0x0001000200030004ull, 0u); // PACKED XYZ3 (ADC)
    if (!check(feed(gif, packet), "second PACKED PRIM/XYZ batch rejected"))
    {
        return 1;
    }

    const auto& first = gs.state();
    if (!check(first.m_tag_count == 2u &&
               first.m_tag_format_counts[0] == 2u &&
               first.m_committed_gif_batches == 2u &&
               first.m_vertex_count == 1u &&
               first.m_total_prim_writes == 2u &&
               first.m_total_vertex_writes == 3u &&
               first.m_total_vertex_kicks == 2u &&
               first.m_total_adc_vertices == 1u &&
               first.m_prim_type_counts[2] == 1u &&
               first.m_prim_type_counts[3] == 1u &&
               first.m_total_packed_events == 3u &&
               first.m_packed_register_counts[5] == 2u &&
               first.m_packed_register_counts[13] == 1u,
               "current primitive count incorrectly conflated with cumulative kicks"))
    {
        return 1;
    }

    // REGLIST two 64-bit registers share one 128-bit payload.
    packet.clear();
    tag(packet, 2u, 1u, 1u, 0x1u);
    qword(packet, 0x1234u, 0x5678u);
    if (!check(feed(gif, packet) &&
               gs.state().m_total_reglist_events == 2u &&
               gs.state().m_tag_format_counts[1] == 1u,
               "REGLIST events not counted"))
    {
        return 1;
    }

    // Configure a real GS host-to-local transfer using A+D GS registers.
    // This is the same supported register setup path as ee_gs_state_test.
    packet.clear();
    tag(packet, 4u, 0u, 1u, 0xeu);
    ad(packet, 0x50u, uint64_t{1} << 48u); // BITBLTBUF DBW=1, PSMCT32
    ad(packet, 0x51u, 0u); // TRXPOS
    ad(packet, 0x52u, uint64_t{1} | (uint64_t{1} << 32u)); // TRXREG 1x1
    ad(packet, 0x53u, 0u); // TRXDIR host-to-local
    if (!check(feed(gif, packet), "host-to-local setup rejected"))
    {
        return 1;
    }
    packet.clear();
    tag(packet, 1u, 2u, 1u, 0u);
    qword(packet, 0x0011223344556677ull, 0x8899aabbccddeeffull);
    if (!check(feed(gif, packet), "first IMAGE upload rejected"))
    {
        return 1;
    }
    if (!check(gs.state().m_image_qwc == 1u &&
               gs.state().m_total_image_qwc == 1u &&
               gs.state().m_tag_format_counts[2] == 1u &&
               gs.state().m_total_image_events == 1u,
               "IMAGE counter semantics incorrect"))
    {
        return 1;
    }

    // A new TRXDIR resets the current transfer but not total IMAGE traffic.
    packet.clear();
    tag(packet, 1u, 0u, 1u, 0xeu);
    ad(packet, 0x53u, 0u);
    if (!check(feed(gif, packet), "second TRXDIR rejected"))
    {
        return 1;
    }
    packet.clear();
    tag(packet, 1u, 3u, 1u, 0u);
    qword(packet, 0x0102030405060708ull, 0x1112131415161718ull);
    if (!check(feed(gif, packet) &&
               gs.state().m_image_qwc == 1u &&
               gs.state().m_total_image_qwc == 2u &&
               gs.state().m_tag_format_counts[3] == 1u &&
               gs.state().m_total_image_events == 2u &&
               gs.state().m_total_ad_events == 5u &&
               gs.state().m_ad_register_counts[0x53] == 2u,
               "second IMAGE2 transfer lost cumulative counters"))
    {
        return 1;
    }

    // A+D-only sprite registers in the actual SDBZ boot complete a
    // two-kick primitive; capture the first and most recent *committed*
    // rectangle's raw attributes and frame target without parsing
    // speculative GS event batches.
    const uint64_t rgbaq = 0x3f80000011223344ull;
    const uint64_t frame = 0x0000000000020090ull;
    const uint64_t xyz0 = (uint64_t{0x11} << 32u) |
        (uint64_t{0x0200} << 16u) | 0x0100u;
    const uint64_t xyz1 = (uint64_t{0x22} << 32u) |
        (uint64_t{0x0300} << 16u) | 0x0180u;
    packet.clear();
    tag(packet, 4u, 0u, 1u, 0xeu);
    ad(packet, 0x01u, rgbaq);
    ad(packet, 0x1au, 1u); // PRMODECONT selects primitive flags
    ad(packet, 0x18u, 0x0010002000300040ull); // XYOFFSET_1
    ad(packet, 0x4cu, frame); // FRAME_1
    if (!check(feed(gif, packet), "sprite frame setup rejected"))
    {
        return 1;
    }
    packet.clear();
    tag(packet, 2u, 0u, 1u, 0xeu, true, 6u);
    ad(packet, 0x05u, xyz0);
    ad(packet, 0x05u, xyz1);
    if (!check(feed(gif, packet), "A+D sprite pair rejected"))
    {
        return 1;
    }
    const auto& sample = gs.state().m_first_sprite_samples[0];
    if (!check(gs.state().m_total_sprite_pair_kicks == 1u &&
               gs.state().m_total_sprite_textured_kicks == 0u &&
               gs.state().m_total_sprite_nonblack_rgb_kicks == 1u &&
               sample.m_index == 1u &&
               sample.m_prim == 6u &&
               sample.m_effective_prim == 6u &&
               sample.m_context == 0u &&
               sample.m_rgbaq == rgbaq &&
               sample.m_xyz0 == xyz0 &&
               sample.m_xyz1 == xyz1 &&
               sample.m_frame == frame &&
               sample.m_xyoffset == 0x0010002000300040ull &&
               gs.state().m_recent_sprite_samples[0].m_index == 1u,
               "committed sprite provenance lost RGB, geometry or frame"))
    {
        return 1;
    }

    const auto before = gs.state();
    // Rejection is transactional even if the batch contains a complete
    // sprite pair before the unsupported FINISH. Nothing may leak.
    packet.clear();
    tag(packet, 3u, 0u, 1u, 0xeu, true, 6u);
    ad(packet, 0x05u, xyz0);
    ad(packet, 0x05u, xyz1);
    ad(packet, 0x61u, 1u); // Unsupported FINISH, fail closed
    if (!check(!feed(gif, packet) &&
               gs.state().m_committed_gif_batches == before.m_committed_gif_batches &&
               gs.state().m_tag_count == before.m_tag_count &&
               gs.state().m_total_prim_writes == before.m_total_prim_writes &&
               gs.state().m_total_vertex_writes == before.m_total_vertex_writes &&
               gs.state().m_total_ad_events == before.m_total_ad_events &&
               gs.state().m_tag_format_counts == before.m_tag_format_counts &&
               gs.state().m_ad_register_counts == before.m_ad_register_counts &&
               gs.state().m_total_sprite_pair_kicks == before.m_total_sprite_pair_kicks &&
               gs.state().m_total_sprite_textured_kicks == before.m_total_sprite_textured_kicks &&
               gs.state().m_total_sprite_nonblack_rgb_kicks == before.m_total_sprite_nonblack_rgb_kicks &&
               gs.state().m_first_sprite_samples[0].m_index ==
                   before.m_first_sprite_samples[0].m_index &&
               gs.state().m_recent_sprite_samples[0].m_index ==
                   before.m_recent_sprite_samples[0].m_index,
               "rejected GIF batch modified cumulative stats"))
    {
        return 1;
    }

    gif.reset();
    gs.reset();
    if (!check(gs.state().m_committed_gif_batches == 0u &&
               gs.state().m_tag_count == 0u &&
               gs.state().m_total_image_qwc == 0u &&
               gs.state().m_total_vertex_writes == 0u &&
               gs.state().m_total_sprite_pair_kicks == 0u &&
               gs.state().m_first_sprite_samples[0].m_index == 0u,
               "GS reset retained stale diagnostic state"))
    {
        return 1;
    }

    std::puts("M2 GIF/GS committed cumulative diagnostics: PASS");
    return 0;
}
