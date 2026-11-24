#include "ee_dmac.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
struct s_sink
{
    uint32_t m_calls = 0u;
    uint32_t m_channel = 0xffffffffu;
    bool m_accept = true;
    std::vector<uint8_t> m_packet;
};

bool accept(void* user, uint32_t channel, const uint8_t* data, uint32_t qwc)
{
    s_sink& sink = *static_cast<s_sink*>(user);
    ++sink.m_calls;
    sink.m_channel = channel;
    sink.m_packet.assign(data, data + static_cast<size_t>(qwc) * 16u);
    return sink.m_accept;
}

bool check(bool ok, const char* message)
{
    if (!ok)
    {
        std::fprintf(stderr, "EE VIF1 source chain: %s\n", message);
    }
    return ok;
}

bool write_channel(ee::c_ee_dmac& dmac, uint32_t offset, uint32_t value)
{
    const ee::s_ee_u128 quad{value, 0u};
    return ee::c_ee_dmac::mmio_write(
        &dmac, ee::c_ee_dmac::k_vif1_base + offset,
        4u, &quad);
}

void write_tag(ee::ee_memory& memory, uint32_t address,
               uint32_t qwc, uint32_t id, uint32_t irq,
               uint32_t external, uint64_t upper)
{
    // ADDR lives in bits 32..63 of the first 64-bit DMA tag word;
    // TTE sends the second word as VIF payload independently.
    const uint64_t low = static_cast<uint64_t>(qwc) |
        (static_cast<uint64_t>(id & 7u) << 28u) |
        (static_cast<uint64_t>(irq & 1u) << 31u) |
        (static_cast<uint64_t>(external & ~15u) << 32u);
    const ee::s_ee_u128 quad{low, upper};
    memory.write128(address, &quad);
}

bool begin(ee::c_ee_dmac& dmac, uint32_t address,
           uint32_t chcr = 0x1c5u)
{
    return write_channel(dmac, 0x30u, address) &&
        write_channel(dmac, 0x20u, 0u) &&
        write_channel(dmac, 0x00u, chcr);
}
} // namespace

int main()
{
    using reject = ee::c_ee_dmac::e_vif1_chain_reject;
    ee::ee_memory memory(4096u);
    ee::c_ee_dmac dmac{};
    dmac.bind_memory(&memory);

    // The real SDBZ 0x1C5 start is CHCR.MOD=chain, TTE=1,
    // TIE=1, DIR=to-VIF1, STR=1. With no VIF consumer we must
    // leave all guest registers, RAM and D_STAT untouched.
    write_tag(memory, 0x100u, 1u, 7u, 0u, 0u,
              0x1122334400000000ull);
    const ee::s_ee_u128 placeholder{0x8877665544332211ull,
                                   0x00ffeeddccbbaa99ull};
    memory.write128(0x110u, &placeholder);
    if (!check(begin(dmac, 0x100u), "start refused") ||
        !check(dmac.vif1_chain_last_reject() ==
               reject::missing_memory_or_sink, "missing sink not diagnosed") ||
        !check(dmac.vif1_channel_state().m_chcr == 0x1c5u,
               "missing sink retired STR") ||
        !check(dmac.vif1_channel_state().m_tadr == 0x100u,
               "missing sink advanced TADR") ||
        !check(dmac.status() == 0u, "missing sink published IRQ"))
    {
        return 1;
    }

    s_sink sink{};
    dmac.bind_device_transfer(&sink, &accept);
    dmac.reset();
    // The first CNT has one inline payload, followed by an END with
    // another inline payload. Both original tag upper halves must
    // appear as independent 16-byte VIF words with zero low halves.
    write_tag(memory, 0x100u, 1u, 1u, 0u, 0u,
              0x1122334400000000ull);
    const ee::s_ee_u128 first_data{0x1011121314151617ull,
                                  0x18191a1b1c1d1e1full};
    memory.write128(0x110u, &first_data);
    write_tag(memory, 0x120u, 1u, 7u, 0u, 0u,
              0x5566778800000000ull);
    const ee::s_ee_u128 last_data{0x2021222324252627ull,
                                 0x28292a2b2c2d2e2full};
    memory.write128(0x130u, &last_data);
    if (!check(begin(dmac, 0x100u), "CNT/END start refused") ||
        !check(sink.m_calls == 1u && sink.m_channel == 1u &&
               sink.m_packet.size() == 64u, "source not committed once") ||
        !check(dmac.vif1_channel_state().m_tadr == 0x140u &&
               dmac.vif1_channel_state().m_madr == 0x140u &&
               dmac.vif1_channel_state().m_qwc == 0u &&
               (dmac.vif1_channel_state().m_chcr & 0x100u) == 0u &&
               dmac.status() == 2u, "VIF1 chain did not retire after sink") ||
        !check(sink.m_packet[0] == 0u &&
               sink.m_packet[8] == 0u &&
               sink.m_packet[12] == 0x44u &&
               sink.m_packet[16] == 0x17u &&
               sink.m_packet[31] == 0x18u &&
               sink.m_packet[32] == 0u &&
               sink.m_packet[44] == 0x88u &&
               sink.m_packet[48] == 0x27u &&
               sink.m_packet[63] == 0x28u,
               "TTE/payload byte ordering mismatch"))
    {
        return 1;
    }

    // Regression from the real 2026-11-19 16:14 USA SDBZ trace.
    // First tag was 0x0077144020000003: NEXT, QWC=3, ADDR=0x00771440.
    // Its TTE half was 0x5000000313000000 (FLUSH + DIRECT(3)).
    // Do NOT decode ADDR from that second word: it yields 0x13000000.
    // Reproduce the same relative 0x40 jump in a small EE RAM fixture.
    dmac.reset();
    sink = {};
    write_tag(memory, 0x100u, 3u, 2u, 0u, 0x140u,
              0x5000000313000000ull);
    const ee::s_ee_u128 direct_tag{0x1000000000008002ull, 0x000000000000000eull};
    memory.write128(0x110u, &direct_tag);
    memory.write128(0x120u, &first_data);
    memory.write128(0x130u, &last_data);
    write_tag(memory, 0x140u, 0u, 7u, 0u, 0u, 0u);
    if (!check(begin(dmac, 0x100u), "SDBZ NEXT/TTE start refused") ||
        !check(sink.m_calls == 1u && sink.m_channel == 1u &&
               sink.m_packet.size() == 80u,
               "SDBZ NEXT/TTE did not deliver complete chain once") ||
        !check(sink.m_packet[8u] == 0x00u &&
               sink.m_packet[11u] == 0x13u &&
               sink.m_packet[12u] == 0x03u &&
               sink.m_packet[15u] == 0x50u &&
               sink.m_packet[16u] == 0x02u &&
               sink.m_packet[24u] == 0x0eu,
               "SDBZ NEXT/TTE VIF/GIF word separation changed") ||
        !check(dmac.vif1_chain_last_reject() == reject::none &&
               dmac.vif1_channel_state().m_tadr == 0x150u &&
               dmac.vif1_channel_state().m_madr == 0x150u &&
               (dmac.vif1_channel_state().m_chcr & 0x100u) == 0u &&
               dmac.status() == 2u,
               "SDBZ NEXT/TTE final DMAC registers incorrect"))
    {
        return 1;
    }

    // NEXT must honor ADDR even when it skips over inline continuation.
    // The bytes after the first tag are not a new DMA tag.
    dmac.reset();
    sink = {};
    write_tag(memory, 0x500u, 1u, 2u, 0u, 0x600u,
              0x1122334400000000ull);
    memory.write128(0x510u, &first_data);
    write_tag(memory, 0x600u, 1u, 7u, 0u, 0u,
              0x5566778800000000ull);
    memory.write128(0x610u, &last_data);
    if (!check(begin(dmac, 0x500u), "nonsequential NEXT start refused") ||
        !check(sink.m_calls == 1u && sink.m_packet.size() == 64u &&
               sink.m_packet[16u] == 0x17u &&
               sink.m_packet[48u] == 0x27u &&
               dmac.vif1_channel_state().m_tadr == 0x620u &&
               dmac.status() == 2u,
               "NEXT did not follow low-word ADDR"))
    {
        return 1;
    }

    // A rejected NEXT destination must preserve channel state, STR and
    // D_STAT instead of committing the valid first tag.
    dmac.reset();
    sink = {};
    write_tag(memory, 0x500u, 1u, 2u, 0u, 0x1300u,
              0x5000000313000000ull);
    if (!check(begin(dmac, 0x500u), "invalid NEXT start refused") ||
        !check(dmac.vif1_chain_last_reject() == reject::invalid_tag_address &&
               sink.m_calls == 0u &&
               dmac.vif1_channel_state().m_chcr == 0x1c5u &&
               dmac.vif1_channel_state().m_tadr == 0x500u &&
               dmac.vif1_channel_state().m_madr == 0u &&
               dmac.status() == 0u,
               "invalid NEXT incorrectly retired DMA"))
    {
        return 1;
    }

    // A rejected sink must see all bytes, but none of the channel
    // state or IRQ may be committed by the DMAC.
    dmac.reset();
    sink = {};
    sink.m_accept = false;
    if (!check(begin(dmac, 0x100u), "refusing-sink start refused") ||
        !check(sink.m_calls == 1u &&
               dmac.vif1_chain_last_reject() == reject::rejected_by_sink &&
               dmac.vif1_channel_state().m_chcr == 0x1c5u &&
               dmac.vif1_channel_state().m_tadr == 0x100u &&
               dmac.vif1_channel_state().m_madr == 0u &&
               dmac.status() == 0u,
               "rejected sink partially committed DMA"))
    {
        return 1;
    }

    // The first tag can terminate due to TIE and its IRQ bit even if
    // its ID is REF rather than END. Only that external payload
    // must be delivered, never a following malformed DMA tag.
    dmac.reset();
    sink = {};
    write_tag(memory, 0x200u, 1u, 3u, 1u, 0x400u,
              0x1122334400000000ull);
    const ee::s_ee_u128 referenced{0xaabbccddeeff0011ull,
                                   0x0123456789abcdefull};
    memory.write128(0x400u, &referenced);
    if (!check(begin(dmac, 0x200u), "REF+TIE start refused") ||
        !check(sink.m_calls == 1u && sink.m_packet.size() == 32u &&
               sink.m_packet[16] == 0x11u,
               "REF+TIE did not stop at first IRQ-tag") ||
        !check(dmac.vif1_channel_state().m_tadr == 0x210u &&
               dmac.vif1_channel_state().m_madr == 0x410u &&
               dmac.status() == 2u,
               "REF/TIE final channel state mismatch"))
    {
        return 1;
    }

    // REFS requires STADR stall scheduling, not a guessed completion.
    dmac.reset();
    sink = {};
    write_tag(memory, 0x300u, 1u, 4u, 0u, 0x400u, 0u);
    if (!check(begin(dmac, 0x300u), "REFS start refused") ||
        !check(dmac.vif1_chain_last_reject() == reject::unsupported_tag &&
               sink.m_calls == 0u &&
               dmac.vif1_channel_state().m_tadr == 0x300u &&
               dmac.status() == 0u,
               "REFS not fail-closed"))
    {
        return 1;
    }

    // Invalid external payload address must not transmit the first
    // accepted-looking TTE tag or claim a channel-1 completion.
    dmac.reset();
    sink = {};
    write_tag(memory, 0x300u, 1u, 0u, 0u, 0x2000u, 0u);
    if (!check(begin(dmac, 0x300u), "bad-span start refused") ||
        !check(dmac.vif1_chain_last_reject() == reject::invalid_payload_span &&
               sink.m_calls == 0u &&
               dmac.vif1_channel_state().m_tadr == 0x300u &&
               dmac.status() == 0u,
               "invalid payload partially committed DMA"))
    {
        return 1;
    }

    // Two individually legal REF payloads exceed the old 16-bit
    // aggregate cap. Keep the exact TTE byte order and retire once.
    ee::ee_memory large_memory(2u * 1024u * 1024u);
    ee::c_ee_dmac large_dmac{};
    large_dmac.bind_memory(&large_memory);
    large_dmac.bind_device_transfer(&sink, &accept);
    sink = {};
    write_tag(large_memory, 0x100u, 0x8000u, 3u, 0u, 0x1000u,
              0x1122334400000000ull);
    write_tag(large_memory, 0x110u, 0x8000u, 0u, 0u, 0x81000u,
              0x5566778800000000ull);
    large_memory.write128(0x1000u, &first_data);
    large_memory.write128(0x100ff0u, &last_data);
    if (!check(begin(large_dmac, 0x100u), "large chain start refused") ||
        !check(sink.m_calls == 1u && sink.m_packet.size() == 0x10002u * 16u &&
               sink.m_packet[12u] == 0x44u &&
               sink.m_packet[16u] == 0x17u &&
               sink.m_packet[0x8001u * 16u + 12u] == 0x88u &&
               sink.m_packet.back() == 0x28u &&
               large_dmac.vif1_chain_last_reject() == reject::none &&
               large_dmac.vif1_channel_state().m_tadr == 0x120u &&
               large_dmac.vif1_channel_state().m_madr == 0x101000u &&
               (large_dmac.vif1_channel_state().m_chcr & 0x100u) == 0u &&
               large_dmac.status() == 2u, "large chain did not commit atomically"))
    {
        return 1;
    }

    large_dmac.reset();
    sink = {};
    // Repeated NEXT with nonzero payload must be rejected before the
    // host accumulates a large cyclic transaction.
    write_tag(large_memory, 0x200u, 1u, 2u, 0u, 0x200u, 0u);
    if (!check(begin(large_dmac, 0x200u), "cyclic chain start refused") ||
        !check(large_dmac.vif1_chain_last_reject() == reject::cyclic_chain &&
               sink.m_calls == 0u && large_dmac.status() == 0u &&
               large_dmac.vif1_channel_state().m_tadr == 0x200u &&
               (large_dmac.vif1_channel_state().m_chcr & 0x100u) != 0u,
               "cycle did not fail closed"))
    {
        return 1;
    }

    std::puts("EE VIF1 source chain TTE/TIE/rollback: PASS");
    return 0;
}
