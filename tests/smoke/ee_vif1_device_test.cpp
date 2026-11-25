#include "ee_dmac.h"
#include "ee_gif.h"
#include "ee_gs.h"
#include "ee_graphics_batch.h"
#include "ee_hw_bus.h"
#include "ee_memory.h"
#include "ee_vif1.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
struct s_capture
{
    uint32_t m_calls = 0u;
    std::vector<ee::s_ee_gif_event> m_events;
};

bool capture_gif(void* user, const ee::s_ee_gif_event* events,
                 size_t count, const uint8_t*, uint32_t)
{
    s_capture& capture = *static_cast<s_capture*>(user);
    ++capture.m_calls;
    capture.m_events.insert(
        capture.m_events.end(), events, events + count);
    return true;
}

bool check(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "VIF1 native device: %s\n", message);
    }
    return condition;
}

std::vector<uint8_t> pack_words(const std::vector<uint32_t>& words)
{
    std::vector<uint8_t> bytes(words.size() * 4u);
    for (size_t index = 0u; index < words.size(); ++index)
    {
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            bytes[index * 4u + lane] =
                static_cast<uint8_t>(words[index] >> (lane * 8u));
        }
    }
    return bytes;
}

bool send(ee::c_ee_vif1& vif1, const std::vector<uint32_t>& words)
{
    if ((words.size() & 3u) != 0u)
    {
        return false;
    }
    const std::vector<uint8_t> bytes = pack_words(words);
    return vif1.consume(bytes.data(),
                        static_cast<uint32_t>(bytes.size() / 16u));
}

uint32_t lane32(const uint8_t* data, uint32_t address, uint32_t lane)
{
    uint32_t value = 0u;
    std::memcpy(&value, data + address * 16u + lane * 4u, 4u);
    return value;
}

bool write_dmac(ee::c_ee_dmac& dmac, uint32_t offset, uint32_t value)
{
    const ee::s_ee_u128 quad{value, 0u};
    return ee::c_ee_dmac::mmio_write(
        &dmac, ee::c_ee_dmac::k_vif1_base + offset, 4u, &quad);
}
} // namespace

int main()
{
    ee::c_ee_vif1 vif1{};
    ee::c_ee_gif_path3 path2{};
    s_capture capture{};
    path2.bind_event_sink(&capture, capture_gif);
    vif1.bind_path2(&path2);

    // End-to-end aggregate PATH2 transaction: two DIRECT commands,
    // each with a legal 0x8000-qword GIF packet. Empty GIFtags avoid
    // unrelated texture/VRAM side effects while exercising all limits.
    {
        ee::c_ee_vif1 large_vif{};
        ee::c_ee_gif_path3 large_path{};
        ee::c_ee_gs_state large_gs{};
        large_path.bind_event_sink(&large_gs,
            [](void* user, const ee::s_ee_gif_event* events, size_t count,
               const uint8_t* data, uint32_t qwc)
            {
                return static_cast<ee::c_ee_gs_state*>(user)->consume(events, count, data, qwc);
            });
        large_vif.bind_path2(&large_path);
        std::vector<uint32_t> words;
        for (uint32_t part = 0u; part < 2u; ++part)
        {
            words.push_back(0x50008000u);
            words.resize(words.size() + 0x8000u * 4u, 0u);
        }
        words.resize(words.size() + 2u, 0u);
        if (!check(send(large_vif, words), "large DIRECT batch refused") ||
            !check(large_vif.diagnostic().m_direct_qwc == 0x10000u &&
                   large_path.counters().m_input_qwc == 0x10000u &&
                   large_path.counters().m_accepted_batches == 1u &&
                   large_vif.diagnostic().m_pending_words == 0u,
                   "large DIRECT did not reach GS as one transaction"))
        {
            return 1;
        }
        // A non-null pointer must not be dereferenced for over-budget input.
        const uint8_t dummy = 0u;
        if (!check(!large_vif.consume(&dummy, ee::k_max_graphics_batch_qwc + 1u) &&
                   !large_path.consume(&dummy, ee::k_max_graphics_batch_qwc + 1u),
                   "oversized host batch accepted"))
        {
            return 1;
        }
    }

    // Basic command sequencing and V4-32 writes.
    if (!check(send(vif1, {0x01000101u, 0x6c010000u,
                          10u, 20u, 30u, 40u, 0u, 0u}),
               "STCYCL/UNPACK refused") ||
        !check(vif1.read_register(0x40u) == 0x101u,
               "STCYCL state") ||
        !check(lane32(vif1.vu1_data(), 0u, 0u) == 10u &&
               lane32(vif1.vu1_data(), 0u, 3u) == 40u,
               "V4-32 VU1 memory"))
    {
        return 1;
    }

    // Bulk V4-32 must be identical to the generic masked path with a
    // zero mask, including NUM=0, TOPS-relative addressing and wrap.
    {
        ee::c_ee_vif1 fast{}, reference{};
        const std::vector<uint32_t> setup{
            0x01000404u, 0x030003f0u, 0x02000000u, 0u};
        if (!check(send(fast, setup) && send(reference, setup),
                   "bulk UNPACK setup rejected"))
        {
            return 1;
        }
        std::vector<uint32_t> packet{0x6c00800eu};
        for (uint32_t i = 0u; i < 256u * 4u; ++i)
        {
            packet.push_back(0x89abcdefu ^ (i * 0x1020304u));
        }
        packet.resize(packet.size() + 3u, 0u);
        auto masked = packet;
        masked[0] |= 0x10000000u;
        const auto raw = pack_words(packet);
        std::vector<uint8_t> unaligned(raw.size() + 1u);
        std::memcpy(unaligned.data() + 1u, raw.data(), raw.size());
        if (!check(fast.consume(unaligned.data() + 1u,
                       static_cast<uint32_t>(raw.size() / 16u)) &&
                   send(reference, masked),
                   "bulk/reference V4-32 rejected") ||
            !check(std::memcmp(fast.vu1_data(), reference.vu1_data(),
                       ee::c_ee_vif1::k_vu1_memory_size) == 0 &&
                   lane32(fast.vu1_data(), 1022u, 0u) == packet[1] &&
                   lane32(fast.vu1_data(), 0u, 0u) == packet[9] &&
                   fast.diagnostic().m_unpack_commands == 1u,
                   "bulk UNPACK changed wrapped VU1 data"))
        {
            return 1;
        }
    }

    // V2-16 sign extension, vector lane replication and continuation.
    if (!check(send(vif1, {0x65020001u, 0x0003fffeu,
                          0x7fff8001u, 0u}),
               "V2-16 command refused") ||
        !check(lane32(vif1.vu1_data(), 1u, 0u) == 0xfffffffeu &&
               lane32(vif1.vu1_data(), 1u, 1u) == 3u &&
               lane32(vif1.vu1_data(), 1u, 2u) == 0xfffffffeu &&
               lane32(vif1.vu1_data(), 2u, 0u) == 0xffff8001u,
               "V2-16 sign extension and replication"))
    {
        return 1;
    }

    // STROW/STCOL, STMASK and masked UNPACK: data/row/col/skip.
    if (!check(send(vif1, {0x30000000u, 9u, 8u, 7u,
                          6u, 0x31000000u, 11u, 12u,
                          13u, 14u, 0x20000000u, 0x39u}),
               "STROW/STCOL/STMASK refused") ||
        !check(send(vif1, {0x7c010000u, 1u, 2u, 3u,
                          4u, 0u, 0u, 0u}),
               "masked V4-32 refused") ||
        !check(lane32(vif1.vu1_data(), 0u, 0u) == 9u &&
               lane32(vif1.vu1_data(), 0u, 1u) == 11u &&
               lane32(vif1.vu1_data(), 0u, 2u) == 30u &&
               lane32(vif1.vu1_data(), 0u, 3u) == 4u,
               "masked UNPACK row/col/skip semantics"))
    {
        return 1;
    }

    // MPG writes the real VU1 microprogram memory. A VU program with
    // an unsupported upper opcode must reject MSCAL transactionally.
    const uint32_t before = vif1.read_register(0x40u);
    if (!check(!send(vif1, {0x01000202u, 0x4a010002u,
                           0x11223344u, 0x00000030u,
                           0x14000002u, 0u, 0u, 0u}),
               "unsupported MSCAL microprogram succeeded") ||
        !check(vif1.diagnostic().m_reason ==
               ee::c_ee_vif1::e_reject::unsupported_vu_execution &&
               vif1.diagnostic().m_code == 0x14000002u &&
               vif1.diagnostic().m_rejected_batches == 1u &&
               vif1.diagnostic().m_first_rejection ==
                   ee::c_ee_vif1::e_reject::unsupported_vu_execution &&
               vif1.diagnostic().m_first_rejected_code == 0x14000002u &&
               vif1.diagnostic().m_committed_opcodes[0x14u] == 0u &&
               vif1.diagnostic().m_committed_opcodes[0x4au] == 0u &&
               vif1.diagnostic().m_vu_start_attempts == 1u &&
               vif1.diagnostic().m_vu_start_rejections == 1u &&
               vif1.diagnostic().m_last_rejected_vu_start_code == 0x14000002u,
               "MSCAL rejection provenance and no partial command commit") ||
        !check(vif1.read_register(0x40u) == before &&
               vif1.vu1_code()[16u] == 0u,
               "failed batch partly committed STCYCL/MPG") ||
        !check(send(vif1, {0x4a010002u, 0x11223344u,
                          0x55667788u, 0u}),
               "MPG upload refused") ||
        !check(vif1.vu1_code()[16u] == 0x44u &&
               vif1.vu1_code()[23u] == 0x55u &&
               vif1.diagnostic().m_committed_opcodes[0x4au] == 1u &&
               vif1.diagnostic().m_rejected_batches == 1u &&
               vif1.diagnostic().m_first_rejected_code == 0x14000002u,
               "MPG code bytes and persistent rejected-DMA provenance"))
    {
        return 1;
    }

    // Split DIRECT: command and first seven words in one DMA; the
    // eighth GIF word arrives in a subsequent DMA with trailing NOPs.
    vif1.reset();
    path2.reset();
    if (!check(send(vif1, {0x50000002u, 0x8001u, 0u, 0xeu,
                          0u, 0x06u, 0u, 0u}),
               "partial DIRECT refused") ||
        !check(vif1.diagnostic().m_pending_words == 8u &&
               capture.m_calls == 0u, "partial DIRECT committed GIF") ||
        !check(send(vif1, {0u, 0u, 0u, 0u}),
               "DIRECT continuation refused") ||
        !check(capture.m_calls == 1u &&
               capture.m_events.size() == 2u &&
               capture.m_events[0].m_type ==
                   ee::e_ee_gif_event_type::tag &&
               capture.m_events[1].m_type ==
                   ee::e_ee_gif_event_type::packed &&
               vif1.diagnostic().m_pending_words == 0u &&
               vif1.diagnostic().m_committed_opcodes[0x50u] == 1u,
               "DIRECT did not deliver real GIF PATH2"))
    {
        return 1;
    }

    // The split DIRECT fixture above checks command continuation. Also
    // compare a nonzero IMAGE payload byte-for-byte at the actual GIF sink.
    {
        ee::c_ee_vif1 image_vif{};
        ee::c_ee_gif_path3 image_path{};
        std::vector<uint8_t> received;
        image_path.bind_event_sink(&received,
            [](void* user, const ee::s_ee_gif_event*, size_t,
               const uint8_t* data, uint32_t qwc)
            {
                auto& bytes = *static_cast<std::vector<uint8_t>*>(user);
                bytes.assign(data, data + static_cast<size_t>(qwc) * 16u);
                return true;
            });
        image_vif.bind_path2(&image_path);
        // NLOOP=2, EOP, FLG=IMAGE; two arbitrary payload qwords.
        const std::vector<uint32_t> payload{
            0x00008002u, 0x08000000u, 0u, 0u,
            0x01234567u, 0x89abcdefu, 0x0badc0deu, 0x10203040u,
            0xfeedfaceu, 0xdeadbeefu, 0x11223344u, 0xaabbccddu};
        std::vector<uint32_t> packet{0x50000003u};
        packet.insert(packet.end(), payload.begin(), payload.end());
        packet.resize(packet.size() + 3u, 0u);
        if (!check(send(image_vif, packet) &&
                   received == pack_words(payload),
                   "bulk DIRECT changed GIF IMAGE bytes"))
        {
            return 1;
        }
    }

    // Scratch reuse must not retain rejected DIRECT data, including a
    // split command that was already partially committed before refusal.
    {
        ee::c_ee_vif1 reused{};
        ee::c_ee_gif_path3 path{};
        std::vector<uint8_t> received;
        bool accept = false;
        struct s_sink
        {
            bool* m_accept;
            std::vector<uint8_t>* m_received;
        } sink{&accept, &received};
        path.bind_event_sink(&sink,
            [](void* user, const ee::s_ee_gif_event*, size_t,
               const uint8_t* data, uint32_t qwc)
            {
                auto& sink = *static_cast<s_sink*>(user);
                if (!*sink.m_accept)
                {
                    return false;
                }
                sink.m_received->assign(data, data + static_cast<size_t>(qwc) * 16u);
                return true;
            });
        reused.bind_path2(&path);
        // DIRECT(2), split after seven payload words. The final word is
        // sent with three NOPs in a second qword-aligned packet.
        const std::vector<uint32_t> first{0x50000002u,
            0x00008001u, 0x08000000u, 0u, 0u,
            0x12345678u, 0x89abcdefu, 0x10203040u};
        const std::vector<uint32_t> last{0xfedcba98u, 0u, 0u, 0u};
        std::vector<uint32_t> payload(first.begin() + 1u, first.end());
        payload.push_back(last.front());
        if (!check(send(reused, first) && !send(reused, last) &&
                   received.empty() && path.counters().m_accepted_batches == 0u,
                   "rejected split DIRECT published scratch bytes"))
        {
            return 1;
        }
        accept = true;
        if (!check(send(reused, last) && received == pack_words(payload) &&
                   reused.diagnostic().m_pending_words == 0u &&
                   path.counters().m_accepted_batches == 1u,
                   "split DIRECT retry reused stale transaction bytes"))
        {
            return 1;
        }
        reused.reset();
        path.reset();
        received.clear();
        if (!check(send(reused, first) && send(reused, last) &&
                   received == pack_words(payload),
                   "reset left stale VIF transaction scratch"))
        {
            return 1;
        }
    }

    // Unknown commands and IRQ stalls fail closed without altering
    // the accepted VIF state or delivering a second PATH2 batch.
    const uint32_t calls_before = capture.m_calls;
    const uint32_t accepted_before = vif1.diagnostic().m_commands;
    if (!check(!send(vif1, {0x1e000000u, 0u, 0u, 0u}),
               "unknown opcode silently completed") ||
        !check(vif1.diagnostic().m_reason ==
               ee::c_ee_vif1::e_reject::unsupported_command &&
               vif1.diagnostic().m_code == 0x1e000000u,
               "unknown opcode diagnostic") ||
        !check(capture.m_calls == calls_before &&
               vif1.read_register(0x80u) == 0u,
               "rejected opcode partially committed") ||
        !check(!send(vif1, {0x80000000u, 0u, 0u, 0u}) &&
               vif1.diagnostic().m_reason ==
                   ee::c_ee_vif1::e_reject::irq_requires_resume,
               "VIF IRQ fabricated completion") ||
        !check(vif1.diagnostic().m_commands == accepted_before,
               "rejected command advanced state"))
    {
        return 1;
    }

    // Guest MMIO register state, 128-bit VIF FIFO, and FBRST reset.
    ee::c_ee_hw_bus hardware{};
    hardware.bind_vif1(&vif1);
    ee::s_ee_u128 quad{0x0000000001000101ull, 0u};
    const ee::s_ee_u128 fbrst{1u, 0u};
    if (!check(ee::c_ee_hw_bus::mmio_write(
                   &hardware, ee::c_ee_vif1::k_fifo_base, 16u, &quad),
               "VIF1 FIFO did not accept a quadword") ||
        !check(vif1.read_register(0x40u) == 0x101u,
               "VIF1 FIFO did not decode STCYCL") ||
        !check(ee::c_ee_hw_bus::mmio_write(
                   &hardware, ee::c_ee_vif1::k_register_base + 0x10u,
                   4u, &fbrst),
               "VIF1 FBRST MMIO rejected") ||
        !check(vif1.read_register(0x40u) == 0u,
               "VIF1 FBRST failed to reset command state"))
    {
        return 1;
    }

    // VU1 CPU apertures share physical code/data with VIF1 MPG/UNPACK.
    // The reference maps four pages each at 0x11008000/0x1100c000.
    ee::s_ee_runtime native_runtime{};
    native_runtime.m_mmio_user = &hardware;
    native_runtime.m_mmio_read = &ee::c_ee_hw_bus::mmio_read;
    native_runtime.m_mmio_write = &ee::c_ee_hw_bus::mmio_write;
    const ee::s_ee_u128 original_code{
        0x0102030405060708ull, 0x1112131415161718ull};
    ee::ee_mem_write128(&native_runtime,
                        ee::c_ee_vif1::k_vu1_code_base,
                        &original_code);
    ee::ee_mem_write32(&native_runtime,
                       ee::c_ee_vif1::k_vu1_data_base + 16u,
                       0x11223344u);
    ee::s_ee_u128 code_read{};
    ee::ee_mem_read128(&native_runtime,
                       ee::c_ee_vif1::k_vu1_code_base, &code_read);
    if (!check(code_read.m_lo == original_code.m_lo &&
               code_read.m_hi == original_code.m_hi &&
               ee::ee_mem_read32(
                   &native_runtime,
                   ee::c_ee_vif1::k_vu1_data_base + 16u) == 0x11223344u &&
               vif1.vu1_data()[16u] == 0x44u,
               "physical VU1 code/data apertures do not share residency") ||
        !check(ee::c_ee_hw_bus::mmio_write(
                   &hardware, ee::c_ee_vif1::k_register_base + 0x10u,
                   4u, &fbrst) &&
               vif1.vu1_code()[0u] == 0x08u &&
               vif1.vu1_data()[16u] == 0x44u,
               "FBRST erased resident VU1 memory"))
    {
        return 1;
    }

    // Real channel-1 source chain: TTE's high tag half carries
    // DIRECT(2), followed by the two original GIF quadwords.
    ee::ee_memory memory(0x2000u);
    ee::c_ee_dmac dmac{};
    dmac.bind_memory(&memory);
    dmac.bind_device_transfer(&vif1, &ee::c_ee_vif1::dma_transfer);
    const ee::s_ee_u128 tag{
        2u | (uint64_t{7u} << 28u),
        static_cast<uint64_t>(0x50000002u) << 32u
    };
    const ee::s_ee_u128 gif_tag{0x8001u, 0xeu};
    const ee::s_ee_u128 packed{0x06u, 0u};
    memory.write128(0x100u, &tag);
    memory.write128(0x110u, &gif_tag);
    memory.write128(0x120u, &packed);
    const uint32_t before_chain = capture.m_calls;
    if (!check(write_dmac(dmac, 0x30u, 0x100u) &&
               write_dmac(dmac, 0x20u, 0u) &&
               write_dmac(dmac, 0x00u, 0x1c5u),
               "VIF1 TTE chain start refused") ||
        !check(dmac.status() == 2u &&
               (dmac.vif1_channel_state().m_chcr & 0x100u) == 0u &&
               dmac.vif1_channel_state().m_tadr == 0x130u &&
               capture.m_calls == before_chain + 1u,
               "VIF1 TTE DMA did not consume and complete"))
    {
        return 1;
    }


    // M2.7: run original VU1 pairs from MPG storage, with E-bit
    // delay-slot termination and a genuine XGKICK GIF PATH1 stream.
    vif1.reset();
    ee::c_ee_gif_path3 path1{};
    path1.bind_event_sink(&capture, capture_gif);
    vif1.bind_path1(&path1);
    const uint32_t before_kick = capture.m_calls;
    const uint32_t nop_upper = 0x000002ffu;
    const uint32_t nop_lower = 0x8000033cu;
    // IADDIU VI1, VI0, +2; XGKICK VI1; final delay-slot pair.
    const uint32_t vi1_two = (0x08u << 25u) | (1u << 16u) | 2u;
    if (!check(send(vif1, {
                   0x4a030000u, vi1_two, nop_upper,
                   0x80000efcu, 0x400002ffu,
                   nop_lower, nop_upper, 0u}),
               "VU1 3-instruction MPG upload refused"))
    {
        return 1;
    }
    // GIFtag NREG=1 (NREG=0 would mean sixteen payload registers).
    const ee::s_ee_u128 kick_tag{0x1000000000008001ull, 0xeu};
    const ee::s_ee_u128 kick_body{0x06u, 0u};
    if (!check(ee::c_ee_vif1::mmio_write(
                   &vif1, ee::c_ee_vif1::k_vu1_data_base + 32u,
                   16u, &kick_tag) &&
               ee::c_ee_vif1::mmio_write(
                   &vif1, ee::c_ee_vif1::k_vu1_data_base + 48u,
                   16u, &kick_body),
               "VU1 data MMIO writes refused") ||
        !check(send(vif1, {0x14000000u, 0u, 0u, 0u}),
               "bounded MSCAL/XGKICK rejected") ||
        !check(capture.m_calls == before_kick + 1u &&
               capture.m_events.size() >= 2u &&
               capture.m_events[capture.m_events.size() - 2u].m_type ==
                   ee::e_ee_gif_event_type::tag &&
               capture.m_events.back().m_type ==
                   ee::e_ee_gif_event_type::packed &&
               vif1.diagnostic().m_committed_opcodes[0x14u] == 1u &&
               vif1.diagnostic().m_committed_opcodes[0x4au] == 1u &&
               vif1.diagnostic().m_vu_start_attempts == 1u &&
               vif1.diagnostic().m_vu_start_rejections == 0u,
               "real XGKICK and committed MSCAL did not reach GIF PATH1"))
    {
        return 1;
    }

    // Detached VU execution must not expose partial VIF state or GIF
    // output, even if the worker finishes before the controller polls.
    {
        using result = ee::c_ee_vif1::e_deferred_result;
        ee::c_ee_vif1 deferred{};
        ee::c_ee_gif_path3 deferred_path{};
        s_capture deferred_capture{};
        deferred_path.bind_event_sink(&deferred_capture, capture_gif);
        deferred.bind_path1(&deferred_path);
        const auto program = pack_words({
            0x4a030000u, vi1_two, nop_upper,
            0x80000efcu, 0x400002ffu, nop_lower, nop_upper, 0u,
            0x14000000u, 0x10000000u, 0x14000000u, 0u});
        if (!check(deferred.enable_deferred(), "enable deferred worker") ||
            !check(ee::c_ee_vif1::mmio_write(
                &deferred, ee::c_ee_vif1::k_vu1_data_base + 32u,
                16u, &kick_tag) &&
                ee::c_ee_vif1::mmio_write(
                &deferred, ee::c_ee_vif1::k_vu1_data_base + 48u,
                16u, &kick_body), "deferred GIF fixture setup") ||
            !check(deferred.begin_deferred(program.data(),
                static_cast<uint32_t>(program.size() / 16u)) == result::pending,
                "deferred transaction did not yield to controller") ||
            !check(deferred.deferred_busy() && deferred_capture.m_calls == 0u &&
                deferred.diagnostic().m_committed_opcodes[0x14u] == 0u &&
                deferred.vu1_code()[0u] == 0u &&
                !send(deferred, {0u, 0u, 0u, 0u}) &&
                !deferred.write_register(0x30u, 1u),
                "uncommitted VIF transaction became visible") ||
            !check(deferred.wait_deferred() == result::completed &&
                deferred_capture.m_calls == 1u &&
                deferred_capture.m_events.size() == 4u &&
                deferred.diagnostic().m_committed_opcodes[0x14u] == 2u &&
                deferred.diagnostic().m_committed_opcodes[0x10u] == 1u &&
                !deferred.deferred_busy(),
                "deferred MSCAL/FLUSHE ordering or publication"))
        {
            return 1;
        }

        const auto bad = pack_words({0x14000000u, 0x06000000u, 0u, 0u});
        if (!check(deferred.begin_deferred(bad.data(), 1u) == result::pending &&
                deferred.wait_deferred() == result::rejected &&
                deferred_capture.m_calls == 1u &&
                deferred.diagnostic().m_committed_opcodes[0x14u] == 2u,
                "late VIF rejection published worker output"))
        {
            return 1;
        }

        const auto start = pack_words({0x14000000u, 0u, 0u, 0u});
        if (!check(deferred.begin_deferred(start.data(), 1u) == result::pending &&
                deferred.write_register(0x10u, 1u) &&
                !deferred.deferred_busy() &&
                deferred.poll_deferred() == result::idle &&
                deferred_capture.m_calls == 1u &&
                deferred.vu1_data()[32u] == 1u &&
                deferred.diagnostic().m_committed_opcodes[0x14u] == 0u,
                "FBRST published or retained cancelled work"))
        {
            return 1;
        }

        // Reuse the staging slot after invalid input and cancellation.
        // It must not retain an old cursor, rejection, PATH1 or snapshot.
        const auto* const committed_data = deferred.vu1_data();
        const auto* const committed_code = deferred.vu1_code();
        if (!check(deferred.begin_deferred(nullptr, 1u) == result::rejected &&
                !deferred.deferred_busy() &&
                deferred.begin_deferred(start.data(), 1u) == result::pending &&
                deferred.wait_deferred() == result::completed &&
                deferred_capture.m_calls == 2u &&
                deferred.diagnostic().m_committed_opcodes[0x14u] == 1u &&
                deferred.vu1_data() == committed_data &&
                deferred.vu1_code() == committed_code,
                "reused transaction retained rejected or cancelled state"))
        {
            return 1;
        }
        deferred.reset();
        const auto nop = pack_words({0u, 0u, 0u, 0u});
        if (!check(deferred.begin_deferred(nop.data(), 1u) == result::completed &&
                !deferred.deferred_busy() &&
                deferred.diagnostic().m_commands == 4u &&
                deferred.diagnostic().m_committed_opcodes[0x14u] == 0u &&
                deferred.vu1_data()[32u] == 0u,
                "reused transaction retained pre-reset guest state"))
        {
            return 1;
        }
    }

    // The DMA controller commits its staged register state and raises
    // channel 1 only after the worker and VIF/GIF publication succeed.
    {
        using result = ee::c_ee_dmac::e_deferred_vif1;
        ee::c_ee_vif1 deferred{};
        ee::c_ee_gif_path3 deferred_path{};
        s_capture deferred_capture{};
        deferred_path.bind_event_sink(&deferred_capture, capture_gif);
        deferred.bind_path1(&deferred_path);
        ee::ee_memory dma_memory(0x2000u);
        ee::c_ee_hw_bus bus{};
        bus.bind_memory(&dma_memory);
        bus.bind_vif1(&deferred);
        auto& dma = bus.dmac();
        const ee::s_ee_u128 start_command{0x14000000u, 0u};
        const ee::s_ee_u128 bad_command{0x0600000014000000ull, 0u};
        const ee::s_ee_u128 end_tag{1u | (uint64_t{7u} << 28u), 0u};
        dma_memory.write128(0x100u, &start_command);
        dma_memory.write128(0x200u, &end_tag);
        dma_memory.write128(0x210u, &start_command);
        dma_memory.write128(0x300u, &end_tag);
        dma_memory.write128(0x310u, &bad_command);

        if (!check(deferred.enable_deferred() &&
            send(deferred, {0x4a030000u, vi1_two, nop_upper,
                0x80000efcu, 0x400002ffu, nop_lower, nop_upper, 0u}) &&
            ee::c_ee_vif1::mmio_write(&deferred,
                ee::c_ee_vif1::k_vu1_data_base + 32u, 16u, &kick_tag) &&
            ee::c_ee_vif1::mmio_write(&deferred,
                ee::c_ee_vif1::k_vu1_data_base + 48u, 16u, &kick_body),
            "deferred DMA fixture setup"))
        {
            return 1;
        }
        dma.bind_deferred_vif1(&deferred);
        if (!check(write_dmac(dma, 0x10u, 0x100u) &&
                write_dmac(dma, 0x20u, 1u) &&
                write_dmac(dma, 0x00u, 0x101u) &&
                dma.deferred_vif1_pending() &&
                dma.vif1_channel_state().m_chcr == 0x101u &&
                dma.vif1_channel_state().m_madr == 0x100u &&
                dma.vif1_channel_state().m_qwc == 1u &&
                dma.status() == 0u && deferred_capture.m_calls == 0u &&
                (deferred.read_register(0u) & 4u) != 0u,
                "normal DMA fabricated early completion") ||
            !check(dma.service_deferred_vif1(true) == result::completed &&
                !dma.deferred_vif1_pending() &&
                dma.vif1_channel_state().m_chcr == 1u &&
                dma.vif1_channel_state().m_madr == 0x110u &&
                dma.vif1_channel_state().m_qwc == 0u &&
                dma.status() == 2u && deferred_capture.m_calls == 1u &&
                (deferred.read_register(0u) & 4u) == 0u,
                "normal DMA did not retire on publication"))
        {
            return 1;
        }

        if (!check(dma.acknowledge(1u) &&
                write_dmac(dma, 0x30u, 0x200u) &&
                write_dmac(dma, 0x00u, 0x105u) &&
                dma.deferred_vif1_pending() && dma.status() == 0u &&
                dma.vif1_channel_state().m_tadr == 0x200u &&
                deferred_capture.m_calls == 1u,
                "chain DMA published early") ||
            !check(dma.service_deferred_vif1(true) == result::completed &&
                dma.vif1_channel_state().m_tadr == 0x220u &&
                (dma.vif1_channel_state().m_chcr & 0x100u) == 0u &&
                dma.status() == 2u && deferred_capture.m_calls == 2u,
                "chain DMA failed to commit post-transfer registers"))
        {
            return 1;
        }

        if (!check(dma.acknowledge(1u) &&
                write_dmac(dma, 0x30u, 0x300u) &&
                write_dmac(dma, 0x00u, 0x105u) &&
                dma.deferred_vif1_pending() &&
                dma.service_deferred_vif1(true) == result::rejected &&
                dma.vif1_chain_last_reject() ==
                    ee::c_ee_dmac::e_vif1_chain_reject::rejected_by_sink &&
                dma.vif1_channel_state().m_tadr == 0x300u &&
                (dma.vif1_channel_state().m_chcr & 0x100u) != 0u &&
                dma.status() == 0u && deferred_capture.m_calls == 2u,
                "rejected chain retired or leaked worker output"))
        {
            return 1;
        }
        if (!check(write_dmac(dma, 0x00u, 1u) &&
                write_dmac(dma, 0x10u, 0x100u) &&
                write_dmac(dma, 0x20u, 1u) &&
                write_dmac(dma, 0x00u, 0x101u) &&
                dma.deferred_vif1_pending() &&
                write_dmac(dma, 0x00u, 1u) &&
                !dma.deferred_vif1_pending() &&
                dma.service_deferred_vif1() == result::idle &&
                dma.status() == 0u && deferred_capture.m_calls == 2u,
                "STR clear fabricated completion"))
        {
            return 1;
        }
        if (!check(write_dmac(dma, 0x00u, 0x101u) &&
                dma.deferred_vif1_pending() &&
                ee::c_ee_hw_bus::mmio_write(&bus,
                    ee::c_ee_vif1::k_register_base + 0x10u, 4u, &fbrst) &&
                !dma.deferred_vif1_pending() &&
                !deferred.deferred_busy() &&
                dma.status() == 0u && deferred_capture.m_calls == 2u,
                "FBRST did not discard pending DMA"))
        {
            return 1;
        }
        bus.reset();
        if (!check(dma.status() == 0u && dma.vif1_channel_state().m_chcr == 0u &&
                deferred.vu1_data()[32u] == 0u,
                "full reset retained deferred state"))
        {
            return 1;
        }
    }

    // Deferred continuation must see the MPG and UNPACK writes that
    // follow the first VU job, not a pre-handoff snapshot of those writes.
    {
        using result = ee::c_ee_vif1::e_deferred_result;
        ee::c_ee_vif1 continued{};
        constexpr uint32_t end_nop = 0x400002ffu;
        const auto sequence = pack_words({
            0x4a020000u, nop_lower, end_nop, nop_lower, nop_upper,
            0x14000000u, 0x11000000u,
            0x4a020002u, nop_lower, end_nop, nop_lower, nop_upper,
            0x01000101u, 0x6c010004u, 0x11223344u, 2u, 3u, 4u,
            0x17000000u, 0x13000000u, 0x15000000u, 0u, 0u, 0u});
        if (!check(continued.enable_deferred() &&
                continued.begin_deferred(sequence.data(),
                    static_cast<uint32_t>(sequence.size() / 16u)) == result::pending &&
                lane32(continued.vu1_data(), 4u, 0u) == 0u,
                "deferred continuation published before completion") ||
            !check(continued.wait_deferred() == result::completed &&
                lane32(continued.vu1_data(), 4u, 0u) == 0x11223344u &&
                continued.diagnostic().m_committed_opcodes[0x14u] == 1u &&
                continued.diagnostic().m_committed_opcodes[0x17u] == 1u &&
                continued.diagnostic().m_committed_opcodes[0x15u] == 1u &&
                continued.diagnostic().m_committed_opcodes[0x4au] == 2u &&
                continued.diagnostic().m_committed_opcodes[0x11u] == 1u &&
                continued.diagnostic().m_committed_opcodes[0x13u] == 1u,
                "deferred MPG/UNPACK/MSCNT/MSCALF ordering"))
        {
            return 1;
        }
    }

    // Mixed PATH2 -> PATH1 -> PATH2 must reach the same GS sink in
    // command order. A late invalid GS write must roll the entire batch
    // back, including parser counters and VIF microprogram state.
    for (bool worker_mode : {false, true})
    {
        ee::c_ee_vif1 mixed{};
        if (worker_mode && !mixed.enable_deferred())
        {
            return 1;
        }
        ee::c_ee_gif_path3 first{}, second{};
        ee::c_ee_gs_state gs{};
        s_capture observer{};
        first.bind_event_sink(&gs, &ee::c_ee_gs_state::gif_events);
        second.bind_event_sink(&gs, &ee::c_ee_gs_state::gif_events);
        gs.bind_committed_event_observer(&observer,
            [](void* user, const ee::s_ee_gif_event* events, size_t count,
               const uint8_t* data, uint32_t qwc)
            {
                capture_gif(user, events, count, data, qwc);
            });
        mixed.bind_path1(&first);
        mixed.bind_path2(&second);
        const ee::s_ee_u128 body{2u, 0x00u};
        if (!check(send(mixed, {
                       0x4a030000u, vi1_two, nop_upper,
                       0x80000efcu, 0x400002ffu,
                       nop_lower, nop_upper, 0u}) &&
                   ee::c_ee_vif1::mmio_write(&mixed,
                       ee::c_ee_vif1::k_vu1_data_base + 32u, 16u, &kick_tag) &&
                   ee::c_ee_vif1::mmio_write(&mixed,
                       ee::c_ee_vif1::k_vu1_data_base + 48u, 16u, &body),
                   "mixed GIF fixture failed"))
        {
            return 1;
        }
        const auto batch = [](uint32_t last_register)
        {
            return std::vector<uint32_t>{
                0x50000002u, 0x8001u, 0x10000000u, 0xeu, 0u,
                1u, 0u, 0u, 0u,
                0x14000000u,
                0x50000002u, 0x8001u, 0x10000000u, 0xeu, 0u,
                3u, 0u, last_register, 0u, 0u};
        };
        if (!check(!send(mixed, batch(0x7fu)) &&
                   observer.m_calls == 0u && !gs.was_written(0u) &&
                   first.counters().m_input_qwc == 0u &&
                   second.counters().m_input_qwc == 0u &&
                   mixed.diagnostic().m_committed_opcodes[0x14u] == 0u,
                   "mixed GIF failure leaked staged writes"))
        {
            return 1;
        }
        if (!check(send(mixed, batch(0u)) &&
                   observer.m_calls == 1u && observer.m_events.size() == 6u &&
                   observer.m_events[1].m_word0 == 1u &&
                   observer.m_events[3].m_word0 == 2u &&
                   observer.m_events[5].m_word0 == 3u &&
                   observer.m_events[1].m_byte_offset == 16u &&
                   observer.m_events[3].m_byte_offset == 48u &&
                   observer.m_events[5].m_byte_offset == 80u &&
                   gs.register_value(0u) == 3u &&
                   first.counters().m_input_qwc == 2u &&
                   second.counters().m_input_qwc == 4u &&
                   mixed.diagnostic().m_committed_opcodes[0x14u] == 1u,
                   "mixed GIF command order or atomic completion failed"))
        {
            return 1;
        }
    }

    // Unknown upper operation must fail before emitting or retiring DMA.
    // An invalid MPG pair is visible in the actual VU code aperture.
    // One VU pair is two 32-bit words in the LOW 64-bit lane.
    const ee::s_ee_u128 unsupported_pair{
        static_cast<uint64_t>(nop_lower) |
            (static_cast<uint64_t>(0x01e00031u) << 32u), 0u};
    const uint32_t after_kick = capture.m_calls;
    if (!check(ee::c_ee_vif1::mmio_write(
                   &vif1, ee::c_ee_vif1::k_vu1_code_base,
                   16u, &unsupported_pair),
               "VU1 unsupported-code MMIO failed") ||
        !check(!send(vif1, {0x14000000u, 0u, 0u, 0u}) &&
               vif1.diagnostic().m_reason ==
                   ee::c_ee_vif1::e_reject::unsupported_vu_execution &&
               vif1.diagnostic().m_vu_pc == 0u &&
               vif1.diagnostic().m_vu_upper == 0x01e00031u &&
               capture.m_calls == after_kick &&
               vif1.diagnostic().m_committed_opcodes[0x14u] == 1u &&
               vif1.diagnostic().m_rejected_batches == 1u &&
               vif1.diagnostic().m_first_rejected_code == 0x14000000u &&
               vif1.diagnostic().m_vu_start_attempts == 2u &&
               vif1.diagnostic().m_vu_start_rejections == 1u &&
               vif1.diagnostic().m_last_rejected_vu_pc == 0u &&
               vif1.diagnostic().m_last_rejected_vu_upper == 0x01e00031u,
               "unsupported VU opcode fabricated DMA/GIF completion"))
    {
        return 1;
    }

    // An unsupported arithmetic opcode with mask=0 is still invalid:
    // a zero destination mask must not turn an unknown op into NOP.
    const ee::s_ee_u128 masked_bad_pair{
        static_cast<uint64_t>(nop_lower) |
            (static_cast<uint64_t>(0x00000030u) << 32u), 0u};
    if (!check(ee::c_ee_vif1::mmio_write(
                   &vif1, ee::c_ee_vif1::k_vu1_code_base,
                   16u, &masked_bad_pair),
               "masked unknown-code MMIO failed") ||
        !check(!send(vif1, {0x14000000u, 0u, 0u, 0u}) &&
               vif1.diagnostic().m_vu_upper == 0x00000030u &&
               capture.m_calls == after_kick &&
               vif1.diagnostic().m_rejected_batches == 2u &&
               vif1.diagnostic().m_vu_start_attempts == 3u &&
               vif1.diagnostic().m_vu_start_rejections == 2u &&
               vif1.diagnostic().m_committed_opcodes[0x14u] == 1u,
               "masked unknown VU opcode silently executed"))
    {
        return 1;
    }

    // The upper pair writes back after the lower pair; vector arithmetic
    // uses actual floating-point register bit patterns.
    ee::s_ee_vu1_state state{};
    state.m_initialized = true;
    state.m_vf[0][3] = 0x3f800000u;
    for (uint32_t lane = 0u; lane < 4u; ++lane)
    {
        state.m_vf[2][lane] = 0x3f800000u;
        state.m_vf[3][lane] = 0x40000000u;
    }
    std::array<uint8_t, ee::c_ee_vu1::k_memory_size> code{};
    std::array<uint8_t, ee::c_ee_vu1::k_memory_size> data{};
    const uint32_t upper_add =
        0x40000000u | (15u << 21u) | (3u << 16u) |
        (2u << 11u) | (1u << 6u) | 0x28u;
    std::memcpy(code.data(), &nop_lower, 4u);
    std::memcpy(code.data() + 4u, &upper_add, 4u);
    std::memcpy(code.data() + 8u, &nop_lower, 4u);
    std::memcpy(code.data() + 12u, &nop_upper, 4u);
    std::vector<uint8_t> no_gif;
    ee::s_ee_vu1_issue issue{};
    if (!check(ee::c_ee_vu1::run(
                   state, code, data, 0u, 0u, 0u, no_gif, &issue) &&
               issue.m_steps == 2u &&
               state.m_vf[1][0] == 0x40400000u &&
               state.m_vf[1][3] == 0x40400000u &&
               no_gif.empty(),
               "VU1 dual-word arithmetic or E-bit delay-slot failed"))
    {
        return 1;
    }


    // PS2 reference FMAC arithmetic: MULA establishes ACC from VF2*VF3;
    // MADD uses the same ACC, and both vector lanes commit after lower.
    ee::s_ee_vu1_state mac_state{};
    mac_state.m_initialized = true;
    mac_state.m_vf[0][3] = 0x3f800000u;
    for (uint32_t lane = 0u; lane < 4u; ++lane)
    {
        mac_state.m_vf[2][lane] = 0x3f800000u;
        mac_state.m_vf[3][lane] = 0x40000000u;
    }
    code.fill(0u);
    const uint32_t mula =
        (15u << 21u) | (3u << 16u) | (2u << 11u) | 0x2beu;
    const uint32_t madd =
        (15u << 21u) | (3u << 16u) | (2u << 11u) | (1u << 6u) | 0x29u;
    const uint32_t madd_e = madd | 0x40000000u;
    std::memcpy(code.data(), &nop_lower, 4u);
    std::memcpy(code.data() + 4u, &mula, 4u);
    std::memcpy(code.data() + 8u, &nop_lower, 4u);
    std::memcpy(code.data() + 12u, &madd_e, 4u);
    std::memcpy(code.data() + 16u, &nop_lower, 4u);
    std::memcpy(code.data() + 20u, &nop_upper, 4u);
    if (!check(ee::c_ee_vu1::run(
                   mac_state, code, data, 0u, 0u, 0u, no_gif, &issue) &&
               issue.m_steps == 3u &&
               mac_state.m_acc[0] == 0x40000000u &&
               mac_state.m_vf[1][0] == 0x40800000u &&
               mac_state.m_vf[1][3] == 0x40800000u &&
               mac_state.m_mac == 0u,
               "VU1 ACC/MADD PS2 reference arithmetic"))
    {
        return 1;
    }

    // FTOI4 converts exactly 1.0 to fixed 16 using the reference math;
    // conversion must not modify the previous MAC/status flags.
    const uint32_t ftoi4 =
        0x40000000u | (8u << 21u) | (1u << 16u) |
        (2u << 11u) | 0x17du;
    code.fill(0u);
    std::memcpy(code.data(), &nop_lower, 4u);
    std::memcpy(code.data() + 4u, &ftoi4, 4u);
    std::memcpy(code.data() + 8u, &nop_lower, 4u);
    std::memcpy(code.data() + 12u, &nop_upper, 4u);
    const uint32_t old_mac = mac_state.m_mac;
    const uint32_t old_status = mac_state.m_status;
    if (!check(ee::c_ee_vu1::run(
                   mac_state, code, data, 0u, 0u, 0u, no_gif, &issue) &&
               mac_state.m_vf[1][0] == 16u &&
               mac_state.m_mac == old_mac &&
               mac_state.m_status == old_status,
               "VU1 FTOI4 conversion/flags"))
    {
        return 1;
    }

    // CLIP flag results are delayed four issued microinstructions before
    // FCGET sees them. X=3 > W=2 and Y=-4 < -W each contribute a flag.
    ee::s_ee_vu1_state clip_state{};
    clip_state.m_initialized = true;
    clip_state.m_vf[0][3] = 0x3f800000u;
    clip_state.m_vf[2][0] = 0x40400000u;
    clip_state.m_vf[2][1] = 0xc0800000u;
    clip_state.m_vf[3][3] = 0x40000000u;
    code.fill(0u);
    const uint32_t clip_upper =
        (2u << 11u) | (3u << 16u) | 0x1ffu;
    const uint32_t fcget_early = (0x1cu << 25u) | (2u << 16u);
    const uint32_t fcget_late = (0x1cu << 25u) | (3u << 16u);
    const uint32_t e_nop = nop_upper | 0x40000000u;
    auto micro_pair = [&](uint32_t pair, uint32_t lo, uint32_t hi)
    {
        std::memcpy(code.data() + pair * 8u, &lo, 4u);
        std::memcpy(code.data() + pair * 8u + 4u, &hi, 4u);
    };
    micro_pair(0u, nop_lower, clip_upper);
    micro_pair(1u, fcget_early, nop_upper);
    micro_pair(2u, nop_lower, nop_upper);
    micro_pair(3u, nop_lower, nop_upper);
    micro_pair(4u, fcget_late, e_nop);
    micro_pair(5u, nop_lower, nop_upper);
    if (!check(ee::c_ee_vu1::run(
                   clip_state, code, data, 0u, 0u, 0u, no_gif, &issue) &&
               clip_state.m_clip == 9u &&
               clip_state.m_vi[2] == 0u &&
               clip_state.m_vi[3] == 9u &&
               issue.m_steps == 6u,
               "VU1 CLIP / FCGET four-cycle flag visibility"))
    {
        return 1;
    }


    // VIF1 TOP is the buffer actually supplied to MSCAL, while TOPS
    // toggles for the NEXT UNPACK buffer (ps2-web-recomp vif::run_vu).
    vif1.reset();
    if (!check(send(vif1, {
                   0x03000020u, 0x02000010u,
                   0x4a020000u, nop_lower, e_nop,
                   nop_lower, nop_upper, 0u}),
               "VIF TOP/TOPS setup and two-pair MPG failed") ||
        !check(send(vif1, {0x14000000u, 0u, 0u, 0u}),
               "VIF TOP/TOPS MSCAL refused") ||
        !check(vif1.read_register(0xe0u) == 0x20u &&
               vif1.read_register(0xc0u) == 0x30u &&
               (vif1.read_register(0x00u) & 0x80u) != 0u,
               "VIF TOP/TOPS double-buffer state incorrectly aliased"))
    {
        return 1;
    }

    // VIF reset preserves the whole resident VU1 context, including
    // the continuation PC. A full device reset must discard it.
    vif1.reset();
    if (!check(send(vif1, {0x4a040000u,
                   nop_lower, e_nop, nop_lower, nop_upper,
                   nop_lower, e_nop, nop_lower, nop_upper,
                   0u, 0u, 0u}),
               "VU1 resident continuation MPG failed") ||
        !check(send(vif1, {0x14000000u, 0u, 0u, 0u}),
               "VU1 resident continuation MSCAL failed") ||
        !check(vif1.write_register(0x10u, 1u) &&
               send(vif1, {0x17000000u, 0u, 0u, 0u}) &&
               vif1.diagnostic().m_committed_opcodes[0x17u] == 1u,
               "FBRST erased resident VU1 continuation state"))
    {
        return 1;
    }
    vif1.reset();
    if (!check(!send(vif1, {0x17000000u, 0u, 0u, 0u}) &&
               vif1.diagnostic().m_reason ==
                   ee::c_ee_vif1::e_reject::unsupported_vu_execution,
               "full reset retained stale VU1 continuation state"))
    {
        return 1;
    }

    // MSCALF uses the explicit start address, not the MSCNT resume PC.
    vif1.reset();
    ee::c_ee_gif_path3 path3{};
    path3.bind_event_sink(&capture, capture_gif);
    vif1.bind_path3(&path3);
    if (!check(send(vif1, {
                   0x03000020u, 0x02000010u,
                   0x4a020004u, nop_lower, e_nop,
                   nop_lower, nop_upper, 0u}),
               "MSCALF test MPG failed") ||
        !check(send(vif1, {0x15000004u, 0u, 0u, 0u}) &&
               vif1.diagnostic().m_committed_opcodes[0x15u] == 1u &&
               vif1.diagnostic().m_vu_start_attempts == 1u &&
               vif1.diagnostic().m_vu_start_rejections == 0u &&
               vif1.read_register(0xe0u) == 0x20u &&
               vif1.read_register(0xc0u) == 0x30u,
               "idle MSCALF failed to execute and advance TOPS"))
    {
        return 1;
    }
    const auto partial = pack_words({0x00008001u, 0x10000000u, 0xeu, 0u});
    if (!check(path3.consume(partial.data(), 1u) && path3.state().m_active,
               "PATH3 partial packet fixture failed") ||
        !check(!send(vif1, {0x15000004u, 0u, 0u, 0u}) &&
               vif1.diagnostic().m_reason == ee::c_ee_vif1::e_reject::mscalf_path3_busy &&
               vif1.diagnostic().m_committed_opcodes[0x15u] == 1u &&
               vif1.diagnostic().m_last_rejected_vu_steps == 0u &&
               vif1.read_register(0xc0u) == 0x30u,
               "MSCALF retired while PATH3 was incomplete"))
    {
        return 1;
    }
    const auto payload = pack_words({6u, 0u, 0u, 0u});
    if (!check(path3.consume(payload.data(), 1u) && !path3.state().m_active &&
               send(vif1, {0x15000004u, 0u, 0u, 0u}) &&
               vif1.diagnostic().m_committed_opcodes[0x15u] == 2u &&
               vif1.read_register(0xc0u) == 0x20u,
               "MSCALF did not execute after PATH3 completed"))
    {
        return 1;
    }
    vif1.reset();
    if (!check(!send(vif1, {0x4a010000u, nop_lower, 0x00000030u,
                            0x15000000u}) &&
               vif1.diagnostic().m_last_rejected_vu_steps == 1u &&
               vif1.diagnostic().m_last_rejected_staged_mpg == 1u &&
               vif1.diagnostic().m_mpg_uploads == 0u &&
               vif1.diagnostic().m_last_rejected_vu_upper == 0x00000030u,
               "MSCALF failed to preserve rejected microprogram evidence"))
    {
        return 1;
    }

    std::puts("EE native VIF1/MPG, VU1/XGKICK PATH1, PATH2 and TTE: PASS");
    return 0;
}
