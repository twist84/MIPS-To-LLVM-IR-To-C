#include "iop_hw.h"
#include "iop_kernel.h"
#include "iop_memory_card.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <cstdio>
#include <vector>

namespace
{
int fail(const char* message)
{
    std::fprintf(stderr, "IOP ROM0 hardware: %s\n", message);
    return 1;
}

void put32(std::vector<uint8_t>& data, uint32_t at, uint32_t value)
{
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        data[at + index] =
            static_cast<uint8_t>(value >> (8u * index));
    }
}

bool test_spu2_sample_clock()
{
    constexpr uint32_t spu = iop::c_spu2::k_base;
    constexpr uint32_t period = iop::c_spu2::k_cycles_per_sample;
    iop::c_hw hw{};
    uint32_t value = 0u;
    hw.advance_cycles(1000u);
    if (hw.spu2().clock_active() || hw.spu2().clock_cycles() != 0u ||
        hw.read(spu, 1u, &value) || hw.spu2().clock_active())
    {
        return false;
    }
    if (!hw.write(spu + 0x760u, 2u, 1u) || !hw.spu2().clock_active())
    {
        return false;
    }
    hw.advance_cycles(period - 1u);
    if (hw.spu2().elapsed_samples() != 0u ||
        hw.spu2().sample_phase() != period - 1u)
    {
        return false;
    }
    hw.advance_cycles(1u);
    hw.advance_cycles(3u * period + 17u);
    if (hw.spu2().elapsed_samples() != 4u ||
        hw.spu2().sample_phase() != 17u ||
        hw.spu2().clock_cycles() != 4u * period + 17u ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &value) || value != 0u)
    {
        return false;
    }
    // Large advance must retain the fractional phase without u32 overflow.
    hw.advance_cycles(0xffffffffu);
    const uint64_t total = uint64_t{4u * period + 17u} + 0xffffffffu;
    if (hw.spu2().elapsed_samples() != total / period ||
        hw.spu2().sample_phase() != total % period)
    {
        return false;
    }
    hw.reset();
    if (hw.spu2().clock_active() || hw.spu2().clock_cycles() != 0u)
    {
        return false;
    }

    // The attached CPU and kernel share the same hardware. A running
    // thread's retired instructions must advance it exactly once.
    iop::c_kernel kernel;
    constexpr uint32_t program = 0x00018000u;
    if (!kernel.attach_hle_hardware() ||
        !kernel.hardware().read(spu + 0x760u, 2u, &value) ||
        !kernel.cpu().write32(program, 0x1000ffffu) || // beq zero,zero,-1
        !kernel.cpu().write32(program + 4u, 0u) ||
        kernel.create_thread(program, 0u, 10u) != 1u ||
        !kernel.start_thread(1u))
    {
        return false;
    }
    const auto result = kernel.run(period + 5u, 128u);
    if (result.m_executed != period + 5u ||
        kernel.cycles() != period + 5u ||
        kernel.hardware().spu2().clock_cycles() != kernel.cycles() ||
        kernel.hardware().spu2().elapsed_samples() != 1u ||
        kernel.hardware().spu2().sample_phase() != 5u)
    {
        return false;
    }

    // Idle time is owned by the kernel rather than the R3000 CPU.
    iop::c_kernel idle;
    if (!idle.attach_hle_hardware() ||
        !idle.hardware().read(spu + 0x760u, 2u, &value))
    {
        return false;
    }
    (void)idle.run(period + 3u);
    return idle.cycles() == period + 3u &&
        idle.hardware().spu2().clock_cycles() == idle.cycles() &&
        idle.hardware().spu2().elapsed_samples() == 1u &&
        idle.hardware().spu2().sample_phase() == 3u &&
        idle.cpu().cycles() == 0u;
}

bool test_spu2_adpcm_voice_progression()
{
    // Low nibble first, signed nibble expansion, predictor history,
    // saturation and negative rounding. Independent known vectors.
    uint16_t words[8] = {0x0000u, 0x87f1u};
    int16_t decoded[28]{};
    int32_t history1 = 0;
    int32_t history2 = 0;
    iop::c_spu2::decode_adpcm_block(words, decoded, history1, history2);
    if (decoded[0] != 4096 || decoded[1] != -4096 ||
        decoded[2] != 28672 || decoded[3] != -32768 ||
        history1 != 0 || history2 != 0)
    {
        return false;
    }
    for (uint16_t& word : words)
    {
        word = 0u;
    }
    words[0] = 0x0010u;
    history1 = 1024;
    history2 = 0;
    iop::c_spu2::decode_adpcm_block(words, decoded, history1, history2);
    if (decoded[0] != 960 || decoded[1] != 900 || decoded[2] != 844)
    {
        return false;
    }
    words[0] = 0x000du;
    words[1] = 0x000fu;
    history1 = history2 = 0;
    iop::c_spu2::decode_adpcm_block(words, decoded, history1, history2);
    if (decoded[0] != -1 || decoded[1] != 0)
    {
        return false;
    }

    constexpr uint32_t base = iop::c_spu2::k_base;
    constexpr uint32_t period = iop::c_spu2::k_cycles_per_sample;
    iop::c_hw hw{};
    uint32_t value = 0u;
    const auto put_block = [&](uint32_t address, uint16_t header)
    {
        if (!hw.write(base + 0x1a8u, 2u, address >> 16u) ||
            !hw.write(base + 0x1aau, 2u, address & 0xffffu) ||
            !hw.write(base + 0x1acu, 2u, header))
        {
            return false;
        }
        for (uint32_t i = 0u; i < 7u; ++i)
        {
            if (!hw.write(base + 0x1acu, 2u, 0x1234u))
            {
                return false;
            }
        }
        return true;
    };
    const auto reg = [&](uint32_t offset, uint32_t expected)
    {
        return hw.read(base + offset, 2u, &value) && value == expected;
    };
    // Cross sound-RAM wrap with a two-block stream. First block marks
    // loop start, second block ends without repeat.
    if (!put_block(0xffff8u, 0x0400u) || !put_block(0u, 0x0100u) ||
        !hw.write(base + 0x1c0u, 2u, 15u) ||
        !hw.write(base + 0x1c2u, 2u, 0xfff8u) ||
        !hw.write(base + 4u, 2u, 0x1000u) ||
        !hw.write(base + 0x1a0u, 2u, 1u) ||
        !reg(0x1cau, 0xfff9u) || hw.spu2().decoded_blocks() != 0u)
    {
        return false;
    }
    hw.advance_cycles(period - 1u);
    if (hw.spu2().voice_starts() != 0u)
    {
        return false;
    }
    hw.advance_cycles(1u);
    if (hw.spu2().voice_starts() != 1u || hw.spu2().decoded_blocks() != 2u ||
        !reg(0x1c6u, 0xfff8u) || !reg(0x340u, 0u))
    {
        return false;
    }
    hw.advance_cycles(3u * period);
    if (!reg(0x1cau, 0xfffau) || !reg(0x1c8u, 15u))
    {
        return false;
    }
    hw.advance_cycles(24u * period);
    if (!reg(0x1c8u, 0u) || !reg(0x1cau, 1u) || !reg(0x340u, 0u))
    {
        return false;
    }
    hw.advance_cycles(28u * period);
    if (!reg(0x340u, 1u) || hw.spu2().active_voice_mask(0u) != 0u ||
        hw.spu2().voice_end_events() != 1u || !reg(0x1cau, 0xfff9u))
    {
        return false;
    }
    // Core 1, voice 23, looping stream at half pitch. ENDX is retired
    // only at consumption; the repeat bit keeps the voice running.
    constexpr uint32_t voice23 = 0x400u + 0x1c0u + 23u * 12u;
    if (!put_block(0x100u, 0x0700u) ||
        !hw.write(base + voice23 + 2u, 2u, 0x100u) ||
        !hw.write(base + 0x400u + 23u * 16u + 4u, 2u, 0x800u) ||
        !hw.write(base + 0x5a2u, 2u, 0x8000u | 0x80u) ||
        hw.spu2().active_voice_mask(1u) != (1u << 23u))
    {
        return false;
    }
    hw.advance_cycles(55u * period);
    if (!reg(0x742u, 0u) || !reg(voice23 + 10u, 0x107u))
    {
        return false;
    }
    hw.advance_cycles(period);
    if (!reg(0x742u, 0x80u) || !reg(voice23 + 10u, 0x101u) ||
        hw.spu2().active_voice_mask(1u) != (1u << 23u) ||
        !hw.write(base + 0x5a6u, 2u, 0x80u) ||
        hw.spu2().active_voice_mask(1u) != (1u << 23u))
    {
        return false;
    }
    hw.advance_cycles(2u * period);
    if (hw.spu2().active_voice_mask(1u) != 0u ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &value) || value != 0u)
    {
        return false;
    }
    return true;
}

bool test_spu2_envelopes()
{
    constexpr uint32_t base = iop::c_spu2::k_base;
    constexpr uint32_t period = iop::c_spu2::k_cycles_per_sample;
    iop::c_hw hw{};
    uint32_t value = 0u;
    const auto write = [&](uint32_t offset, uint32_t data)
    {
        return hw.write(base + offset, 2u, data);
    };
    const auto read = [&](uint32_t offset, uint32_t expected)
    {
        return hw.read(base + offset, 2u, &value) && value == expected;
    };
    // Zero pitch isolates envelope progression. Fast linear attack,
    // maximum sustain target, positive/negative fixed stereo volumes.
    if (!write(4u, 0u) || !write(6u, 0x000fu) ||
        !write(8u, 0u) || !write(0u, 0x1000u) ||
        !write(2u, 0x4000u) || !read(0xcu, 0x2000u) ||
        !read(0xeu, 0x8000u) || !write(0x1a0u, 1u) ||
        !read(0xau, 0u))
    {
        return false;
    }
    hw.advance_cycles(period - 1u);
    if (!read(0xau, 0u))
    {
        return false;
    }
    hw.advance_cycles(1u);
    if (!read(0xau, 14336u))
    {
        return false;
    }
    hw.advance_cycles(period);
    if (!read(0xau, 28672u))
    {
        return false;
    }
    hw.advance_cycles(period);
    if (!read(0xau, 32767u))
    {
        return false;
    }
    hw.advance_cycles(period); // exponential decay: -16384 at full scale
    if (!read(0xau, 16383u))
    {
        return false;
    }
    hw.advance_cycles(period); // sustain increases
    if (!read(0xau, 30719u) || !write(0xau, 32767u) ||
        !write(0x1a4u, 1u) || hw.spu2().active_voice_mask(0u) != 1u)
    {
        return false;
    }
    hw.advance_cycles(period);
    if (!read(0xau, 16383u) || hw.spu2().active_voice_mask(0u) != 1u)
    {
        return false;
    }
    hw.advance_cycles(period);
    if (!read(0xau, 0u) || hw.spu2().active_voice_mask(0u) != 0u ||
        !read(0x340u, 0u))
    {
        return false;
    }
    // Sweep timing and cancellation before the first sample.
    if (!write(0u, 0u) || !write(0u, 0x8000u) ||
        !write(0x1a0u, 1u) || !write(0x1a4u, 1u))
    {
        return false;
    }
    hw.advance_cycles(period);
    if (hw.spu2().voice_starts() != 1u || !read(0xcu, 0u) ||
        !write(0x1a0u, 1u))
    {
        return false;
    }
    hw.advance_cycles(period);
    if (!read(0xcu, 14336u))
    {
        return false;
    }
    hw.advance_cycles(2u * period);
    return read(0xcu, 32767u) && read(0xau, 32767u);
}

bool test_authentic_spu2_dma()
{
    iop::c_kernel kernel;
    if (!kernel.attach_hle_hardware())
    {
        return false;
    }
    iop::c_hw& hw = kernel.hardware();
    iop::c_cpu& cpu = kernel.cpu();
    constexpr uint32_t dma4 = iop::c_hw::k_spu2_dma4_base;
    constexpr uint32_t dma7 = iop::c_hw::k_spu2_dma7_base;
    constexpr uint32_t spu2 = iop::c_spu2::k_base;
    constexpr uint32_t source = 0x00032000u;
    uint32_t actual = 0u;
    uint16_t sample = 0u;
    uint8_t bytes[16]{};

    // Transfer four real IOP words to SPU2 RAM, crossing its 1M-word
    // wrap. DICR and IRQ3 remain untouched until the scheduled
    // completion, not merely because the guest wrote CHCR.STR.
    for (uint32_t i = 0u; i < 16u; ++i)
    {
        if (!cpu.write8(source + i, static_cast<uint8_t>(i + 1u)))
        {
            return false;
        }
    }
    if (!hw.set_interrupt_mask(36u, true) ||
        !hw.write(spu2 + 0x1a8u, 2u, 0x000fu) ||
        !hw.write(spu2 + 0x1aau, 2u, 0xfffeu) ||
        !hw.write(dma4, 4u, source) ||
        !hw.write(dma4 + 4u, 4u, 0x00010004u) ||
        !cpu.write32(0xbf8010c8u, 0x01000001u) ||
        !hw.spu2_ram_word(iop::c_spu2::k_ram_words - 2u, &sample) ||
        sample != 0x0201u ||
        !hw.spu2_ram_word(iop::c_spu2::k_ram_words - 1u, &sample) ||
        sample != 0x0403u ||
        !hw.spu2_ram_word(0u, &sample) ||
        sample != 0x0605u ||
        !hw.spu2_ram_word(5u, &sample) ||
        sample != 0x100fu ||
        !hw.read(spu2 + 0x1aau, 2u, &actual) || actual != 6u ||
        !hw.read(dma4 + 8u, 4u, &actual) ||
        (actual & iop::c_hw::k_spu2_dma_str_bit) == 0u ||
        !hw.read(iop::c_hw::k_dma_icr, 4u, &actual) ||
        (actual & 0x10000000u) != 0u ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &actual) ||
        actual != 0u || hw.pending_interrupts() != 0u)
    {
        return false;
    }
    // An IOP kernel with no runnable threads must still advance
    // the real DMA event through its bounded idle scheduler.
    if (!kernel.has_guest_work())
    {
        return false;
    }
    (void)kernel.run(64u);
    if (!hw.read(dma4 + 8u, 4u, &actual) ||
        (actual & iop::c_hw::k_spu2_dma_str_bit) == 0u ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &actual) ||
        actual != 0u)
    {
        return false;
    }
    (void)kernel.run(1u);
    if (kernel.has_guest_work() ||
        !hw.read(dma4 + 8u, 4u, &actual) ||
        (actual & iop::c_hw::k_spu2_dma_str_bit) != 0u ||
        !hw.read(iop::c_hw::k_dma_icr, 4u, &actual) ||
        (actual & 0x10000000u) == 0u ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &actual) ||
        (actual & (1u << iop::c_hw::k_dma_irq)) == 0u ||
        (hw.pending_interrupts() &
         (1u << iop::c_hw::k_dma_irq)) == 0u)
    {
        return false;
    }

    // Program the second core's real SPU2 RAM data port and transfer
    // four sound halfwords back to a different IOP RAM destination.
    // No fabricated sample output or unrelated DMA4 CHCR mutation.
    const uint32_t dest = 0x00033000u;
    if (!hw.write(iop::c_hw::k_i_stat, 4u,
                  ~(1u << iop::c_hw::k_dma_irq)) ||
        !hw.write(spu2 + 0x400u + 0x1a8u, 2u, 0u) ||
        !hw.write(spu2 + 0x400u + 0x1aau, 2u, 0x0500u))
    {
        return false;
    }
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        if (!hw.write(spu2 + 0x400u + 0x1acu, 2u, 0x1000u + i))
        {
            return false;
        }
    }
    if (!hw.write(spu2 + 0x400u + 0x1aau, 2u, 0x0500u) ||
        !hw.write(dma7, 4u, dest) ||
        !hw.write(dma7 + 4u, 4u, 0x00010002u) ||
        !cpu.write32(0xbf801508u, 0x01000000u) ||
        !cpu.dma_read(dest, bytes, 8u))
    {
        return false;
    }
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        if (bytes[i * 2u] != i || bytes[i * 2u + 1u] != 0x10u)
        {
            return false;
        }
    }
    if (!hw.read(dma7 + 8u, 4u, &actual) ||
        (actual & iop::c_hw::k_spu2_dma_str_bit) == 0u ||
        !hw.read(iop::c_hw::k_dma_icr2, 4u, &actual) ||
        (actual & 0x01000000u) != 0u)
    {
        return false;
    }
    hw.advance_cycles(63u);
    if (!hw.read(dma7 + 8u, 4u, &actual) ||
        (actual & iop::c_hw::k_spu2_dma_str_bit) == 0u)
    {
        return false;
    }
    hw.advance_cycles(1u);
    if (!hw.read(dma7 + 8u, 4u, &actual) ||
        (actual & iop::c_hw::k_spu2_dma_str_bit) != 0u ||
        !hw.read(iop::c_hw::k_dma_icr2, 4u, &actual) ||
        (actual & 0x01000000u) == 0u ||
        !hw.read(dma4 + 8u, 4u, &actual) ||
        (actual & iop::c_hw::k_spu2_dma_str_bit) != 0u)
    {
        return false;
    }

    // Cancel a real in-flight DMA request before its timer fires.
    // There must be no DMA completion even though the original
    // memory transfer already occurred at CHCR.STR launch.
    hw.reset();
    if (!kernel.attach_hle_hardware() ||
        !hw.write(spu2 + 0x1aau, 2u, 0x0500u) ||
        !hw.write(dma4, 4u, source) ||
        !hw.write(dma4 + 4u, 4u, 0x00010004u) ||
        !cpu.write32(0xbf8010c8u, 0x01000001u) ||
        !hw.write(dma4 + 8u, 4u, 0u))
    {
        return false;
    }
    hw.advance_cycles(1000u);
    if (!hw.read(iop::c_hw::k_dma_icr, 4u, &actual) ||
        actual != 0u ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &actual) ||
        actual != 0u)
    {
        return false;
    }

    // Small AutoDMA fits in the input ring immediately, and completes
    // after 32 cycles, without changing TSA.
    hw.reset();
    if (!kernel.attach_hle_hardware() ||
        !hw.write(spu2 + 0x1b0u, 2u, 1u) ||
        !hw.write(dma4, 4u, source) ||
        !hw.write(dma4 + 4u, 4u, 0x00010004u) ||
        !cpu.write32(0xbf8010c8u, 0x01000001u))
    {
        return false;
    }
    hw.advance_cycles(31u);
    if (!hw.read(dma4 + 8u, 4u, &actual) ||
        (actual & iop::c_hw::k_spu2_dma_str_bit) == 0u ||
        hw.spu2().input_frames(0u) != 4u ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &actual) || actual != 0u)
    {
        return false;
    }
    hw.advance_cycles(1u);
    if (!hw.read(dma4 + 8u, 4u, &actual) ||
        (actual & iop::c_hw::k_spu2_dma_str_bit) != 0u ||
        hw.spu2_auto_dma_completed() != 1u ||
        !hw.read(spu2 + 0x1aau, 2u, &actual) || actual != 0u)
    {
        return false;
    }

    // A transfer outside the physically backed 2 MiB IOP RAM
    // cannot complete or partially modify sound RAM.
    hw.reset();
    if (!kernel.attach_hle_hardware() ||
        !hw.write(dma4, 4u, iop::c_cpu::k_ram_size - 2u) ||
        !hw.write(dma4 + 4u, 4u, 0x00010004u) ||
        !cpu.write32(0xbf8010c8u, 0x01000001u) ||
        cpu.dma_read(iop::c_cpu::k_ram_size - 2u, bytes, 16u))
    {
        return false;
    }
    hw.advance_cycles(1000u);
    if (!hw.read(dma4 + 8u, 4u, &actual) ||
        (actual & iop::c_hw::k_spu2_dma_str_bit) == 0u ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &actual) ||
        actual != 0u ||
        !hw.read(iop::c_hw::k_dma_icr, 4u, &actual) ||
        actual != 0u ||
        !hw.spu2_ram_word(0u, &sample) ||
        sample != 0u)
    {
        return false;
    }

    hw.reset();
    if (!hw.read(dma4 + 8u, 4u, &actual) || actual != 0u ||
        !hw.read(dma7 + 8u, 4u, &actual) || actual != 0u ||
        !hw.read(iop::c_hw::k_dma_icr, 4u, &actual) ||
        actual != 0u ||
        !hw.read(iop::c_hw::k_dma_icr2, 4u, &actual) ||
        actual != 0u)
    {
        return false;
    }
    return true;
}

bool test_spu2_mixer_rings()
{
    constexpr uint32_t base = iop::c_spu2::k_base;
    constexpr uint32_t period = iop::c_spu2::k_cycles_per_sample;
    constexpr uint32_t irq = 1u << iop::c_spu2::k_irq_line;
    iop::c_kernel kernel;
    if (!kernel.attach_hle_hardware())
    {
        return false;
    }
    auto& hw = kernel.hardware();
    uint32_t value = 0u;
    uint16_t word = 0u;
    // Program a real voice-1 sample, outside reserved mixer RAM.
    // IRQA targets its enveloped (pre-volume) tap, not its ADPCM data.
    if (!hw.write(base + 0x1aau, 2u, 0x5000u) ||
        !hw.write(base + 0x1acu, 2u, 0u) ||
        !hw.write(base + 0x1acu, 2u, 0x4444u) ||
        !hw.write(base + 0x1ceu, 2u, 0x5000u) ||
        !hw.write(base + 0x10u, 2u, 0x3fffu) ||
        !hw.write(base + 0x16u, 2u, 0x000fu) ||
        !hw.write(base + 0x19eu, 2u, 0x420u) ||
        !hw.write(base + 0x19au, 2u, 0x40u) ||
        !hw.write(base + 0x1a0u, 2u, 2u))
    {
        return false;
    }
    (void)kernel.run(31u * period);
    if (hw.spu2().mixer_ring_batches() != 0u ||
        hw.spu2().mixer_irq_events() != 0u ||
        !hw.spu2_ram_word(0x420u, &word) || word != 0u ||
        hw.spu2().voice_irq_cycles_remaining() != period)
    {
        return false;
    }
    (void)kernel.run(period);
    if (hw.spu2().mixer_ring_batches() != 1u ||
        hw.spu2().mixer_irq_events() != 1u ||
        !hw.spu2_ram_word(0x420u, &word) || word != 7168u ||
        !hw.spu2_ram_word(0x1020u, &word) || word != 7167u ||
        !hw.spu2_ram_word(0x820u, &word) || word != 7166u ||
        !hw.spu2_ram_word(0x1420u, &word) || word != 7167u ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &value) || (value & irq) == 0u)
    {
        return false;
    }

    // No active voice is necessary for a mixer-ring write/IRQ.
    // Sound RAM advances even in a headless idle kernel.
    kernel.reset();
    if (!kernel.attach_hle_hardware() ||
        !hw.write(base + 0x1aau, 2u, 0x400u) ||
        !hw.write(base + 0x1acu, 2u, 0x1234u) ||
        !hw.write(base + 0x59eu, 2u, 0x400u) ||
        !hw.write(base + 0x59au, 2u, 0x40u) ||
        hw.spu2().voice_irq_cycles_remaining() != 512u * period ||
        !kernel.has_guest_work())
    {
        return false;
    }
    (void)kernel.run(512u * period - 1u);
    if (!hw.spu2_ram_word(0x400u, &word) || word != 0x1234u ||
        hw.spu2().mixer_irq_events() != 0u)
    {
        return false;
    }
    (void)kernel.run(1u);
    if (!hw.spu2_ram_word(0x400u, &word) || word != 0u ||
        hw.spu2().mixer_ring_batches() != 16u ||
        hw.spu2().mixer_irq_events() != 1u ||
        hw.spu2().irq_latches() != 2u ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &value) || (value & irq) == 0u)
    {
        return false;
    }
    return true;
}

bool test_spu2_voice_routing()
{
    constexpr uint32_t base = iop::c_spu2::k_base;
    constexpr uint32_t period = iop::c_spu2::k_cycles_per_sample;
    iop::c_spu2 spu;
    uint32_t mask = 0u;
    uint16_t word = 0u;
    for (uint32_t core = 0u; core < 2u; ++core)
    {
        const uint32_t registers = base + core * 0x400u;
        for (uint32_t offset = 0x188u; offset <= 0x194u; offset += 4u)
        {
            if (!spu.read(registers + offset, 4u, &mask) || mask != 0xffffffu)
            {
                return false;
            }
        }
        const uint32_t voice = core == 0u ? 1u : 23u;
        const uint32_t bit = 1u << voice;
        const uint32_t parameters = registers + voice * 16u;
        const uint32_t address = registers + 0x1c0u + voice * 12u;
        // Core 0: voice 1 dry L / wet R. Core 1: voice 23 dry R / wet L.
        if (!spu.write(registers + 0x188u, 4u, core == 0u ? bit : 0u) ||
            !spu.write(registers + 0x18cu, 4u, core == 1u ? bit : 0u) ||
            !spu.write(registers + 0x190u, 4u, core == 1u ? bit : 0u) ||
            !spu.write(registers + 0x194u, 4u, core == 0u ? bit : 0u) ||
            !spu.write(registers + 0x1aau, 2u, 0x5000u) ||
            !spu.write(registers + 0x1acu, 2u, 0u) ||
            !spu.write(registers + 0x1acu, 2u, 0x4444u) ||
            !spu.write(address + 2u, 2u, 0x5000u) ||
            !spu.write(parameters, 2u, 0x3fffu) ||
            !spu.write(parameters + 2u, 2u, 0x3fffu) ||
            !spu.write(parameters + 6u, 2u, 0x000fu) ||
            !spu.write(registers + 0x1a0u, 4u, bit))
        {
            return false;
        }
    }
    (void)spu.advance_cycles(32u * period);
    const uint32_t addresses[] =
    {
        0x1020u, 0x1220u, 0x1420u, 0x1620u,
        0x1820u, 0x1a20u, 0x1c20u, 0x1e20u
    };
    const uint16_t expected[] =
    {
        7167u, 0u, 0u, 7167u, 0u, 7167u, 7167u, 0u
    };
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        if (!spu.ram_word(addresses[i], &word) || word != expected[i])
        {
            return false;
        }
    }
    return spu.active_voice_mask(0u) == 2u &&
        spu.active_voice_mask(1u) == (1u << 23u);
}

bool test_spu2_effect_registers()
{
    constexpr uint32_t base = iop::c_spu2::k_base;
    iop::c_spu2 spu;
    uint32_t value = 0u;
    uint32_t address = 0u;
    if (spu.effect_start(0u) != 0xefff8u ||
        spu.effect_end(0u) != 0xeffffu ||
        spu.effect_start(1u) != 0xffff8u ||
        spu.effect_end(1u) != 0xfffffu ||
        spu.effect_enabled(0u) || spu.effect_enabled(1u))
    {
        return false;
    }
    if (!spu.effect_address(0u, 0u, -1, &address) || address != 0xeffffu ||
        !spu.effect_address(0u, 8u, 1, &address) || address != 0xefff9u ||
        spu.effect_address(2u, 0u, 0, &address) ||
        spu.effect_address(0u, 0u, 0, nullptr))
    {
        return false;
    }

    // Effect addresses are high-halfword first; EEA low is read-only.
    if (!spu.write(base + 0x2e0u, 4u, 0xfff80010u) ||
        !spu.write(base + 0x33cu, 4u, 0x12340010u) ||
        !spu.read(base + 0x33cu, 4u, &value) || value != 0xffff0010u ||
        spu.effect_start(0u) != 0x10fff8u ||
        spu.effect_end(0u) != 0x10ffffu ||
        !spu.effect_address(0u, 0u, -1, &address) || address != 0x0ffffu ||
        !spu.effect_address(0u, 0xffffffffffffffffull, 2, &address) ||
        address != 0x0fff9u ||
        !spu.write(base + 0x33cu, 2u, 0x000fu) ||
        spu.effect_address(0u, 0u, 0, &address))
    {
        return false;
    }

    // Address parameters and signed coefficient bit patterns are
    // independent between cores and remain accessible while disabled.
    for (uint32_t core = 0u; core < 2u; ++core)
    {
        for (uint32_t reg = 0x2e4u; reg <= 0x338u; reg += 4u)
        {
            const uint32_t expected = 0x34560000u | (reg + core);
            if (!spu.write(base + core * 0x400u + reg, 4u, expected) ||
                !spu.read(base + core * 0x400u + reg, 4u, &value) ||
                value != expected)
            {
                return false;
            }
        }
        for (uint32_t reg = 0u; reg < 10u; ++reg)
        {
            const uint32_t expected = 0x8000u + reg + core * 16u;
            const uint32_t mmio = base + 0x774u + core * 0x28u + reg * 2u;
            if (!spu.write(mmio, 2u, expected) ||
                !spu.read(mmio, 2u, &value) || value != expected)
            {
                return false;
            }
        }
    }
    if (!spu.read(base + 0x774u, 2u, &value) || value != 0x8000u ||
        !spu.read(base + 0x2e4u, 4u, &value) || value != 0x345602e4u ||
        !spu.write(base + 0x59au, 2u, 0x80u) ||
        !spu.effect_enabled(1u) || spu.effect_enabled(0u) ||
        spu.effect_enable_writes(1u) != 1u ||
        !spu.write(base + 0x59au, 2u, 0u) || spu.effect_enabled(1u))
    {
        return false;
    }
    spu.reset();
    return spu.effect_enable_writes(1u) == 0u &&
        spu.effect_start(0u) == 0xefff8u &&
        spu.read(base + 0x774u, 2u, &value) && value == 0u;
}

bool test_spu2_mmix()
{
    constexpr uint32_t base = iop::c_spu2::k_base;
    constexpr uint32_t period = iop::c_spu2::k_cycles_per_sample;
    iop::c_spu2 spu;
    std::vector<int16_t> pcm;
    spu.bind_pcm_sink(&pcm, [](void* user, const int16_t* data, uint32_t frames)
    {
        auto& output = *static_cast<std::vector<int16_t>*>(user);
        output.insert(output.end(), data, data + frames * 2u);
    });
    uint32_t value = 0u;
    if (!spu.read(base + 0x198u, 2u, &value) || value != 0xff0u ||
        !spu.read(base + 0x598u, 2u, &value) || value != 0xffcu)
    {
        return false;
    }
    const uint8_t input[] = {0x00u, 0x40u, 0x00u, 0x40u};
    // Core 0 input dry L / wet R; core 1 external dry L.
    // Other gates are closed. Core 1 AVOL is a signed Q15 gain.
    if (!spu.write(base + 0x198u, 2u, 0x090u) ||
        !spu.write(base + 0x598u, 2u, 0x008u) ||
        !spu.write(base + 0x790u, 2u, 0x4000u) ||
        !spu.push_input(0u, input, sizeof(input)))
    {
        return false;
    }
    (void)spu.advance_cycles(32u * period);
    uint16_t word = 0u;
    if (pcm.size() != 64u || pcm[0] != 8190 || pcm[1] != 0 ||
        !spu.ram_word(0x820u, &word) || word != 16382u ||
        spu.effect_input_frames(0u) != 1u || spu.effect_input_frames(1u) != 0u)
    {
        return false;
    }

    // Route the core 0 output into core 1's effect input only.
    pcm.clear();
    if (!spu.write(base + 0x598u, 2u, 0x002u) ||
        !spu.push_input(0u, input, sizeof(input)))
    {
        return false;
    }
    (void)spu.advance_cycles(32u * period);
    if (pcm.size() != 64u || pcm[0] != 0 || pcm[1] != 0 ||
        spu.effect_input_frames(1u) != 1u)
    {
        return false;
    }

    // Core 1's own input bypasses AVOL. Closing the dry gate must
    // not stop AutoDMA consumption or the pre-effect signal.
    pcm.clear();
    if (!spu.write(base + 0x598u, 2u, 0x060u) ||
        !spu.push_input(1u, input, sizeof(input)))
    {
        return false;
    }
    (void)spu.advance_cycles(32u * period);
    if (pcm.size() != 64u || pcm[0] != 0 || pcm[1] != 16382 ||
        spu.effect_input_frames(1u) != 2u ||
        spu.input_frames_consumed() != 3u)
    {
        return false;
    }

    // Voice gates are independent of per-voice VMIX and tap writeback.
    pcm.clear();
    if (!spu.write(base + 0x598u, 2u, 0x200u) ||
        !spu.write(base + 0x5aau, 2u, 0x5000u) ||
        !spu.write(base + 0x5acu, 2u, 0u) ||
        !spu.write(base + 0x5acu, 2u, 0x4444u) ||
        !spu.write(base + 0x5c2u, 2u, 0x5000u) ||
        !spu.write(base + 0x400u, 2u, 0x3fffu) ||
        !spu.write(base + 0x406u, 2u, 0x000fu) ||
        !spu.write(base + 0x5a0u, 2u, 1u))
    {
        return false;
    }
    (void)spu.advance_cycles(32u * period);
    return pcm.size() == 64u && pcm[0] == 0 && pcm[1] == 0 &&
        spu.effect_input_frames(1u) == 34u &&
        spu.ram_word(0x1880u, &word) && word == 7167u &&
        spu.ram_word(0x1c80u, &word) && word == 7167u;
}

bool test_spu2_pcm_mixer()
{
    constexpr uint32_t base = iop::c_spu2::k_base;
    constexpr uint32_t period = iop::c_spu2::k_cycles_per_sample;
    iop::c_hw hw{};
    std::vector<int16_t> pcm;
    hw.bind_spu2_pcm_sink(&pcm, [](void* user, const int16_t* data, uint32_t frames)
    {
        auto& output = *static_cast<std::vector<int16_t>*>(user);
        output.insert(output.end(), data, data + frames * 2u);
    });
    // Constant positive ADPCM sample (nibble 4, shift 0), zero pitch,
    // fast attack, left-only fixed volume and ordinary master gain.
    if (!hw.write(base + 0x1aau, 2u, 0x5000u) ||
        !hw.write(base + 0x1acu, 2u, 0u) ||
        !hw.write(base + 0x1acu, 2u, 0x4444u) ||
        !hw.write(base + 0x1c2u, 2u, 0x5000u) ||
        !hw.write(base + 0u, 2u, 0x3fffu) ||
        !hw.write(base + 2u, 2u, 0u) ||
        !hw.write(base + 6u, 2u, 0x000fu) ||
        !hw.write(base + 0x1a0u, 2u, 1u))
    {
        return false;
    }
    hw.advance_cycles(31u * period);
    if (!pcm.empty() || hw.spu2().mixed_frames() != 31u)
    {
        return false;
    }
    hw.advance_cycles(period);
    if (pcm.size() != 64u || pcm[0] != 7164 || pcm[1] != 0 ||
        pcm[2] != 14332 || pcm[3] != 0 ||
        hw.spu2().nonzero_frames() != 32u ||
        hw.spu2().pcm_peak() == 0u)
    {
        return false;
    }
    // Host sink binding does not affect guest state. Reset clears the
    // mixer counters and pending host batch but retains the binding.
    hw.reset();
    pcm.clear();
    uint32_t ignored = 0u;
    if (!hw.read(base + 0x760u, 2u, &ignored))
    {
        return false;
    }
    hw.advance_cycles(32u * period);
    if (pcm.size() != 64u || hw.spu2().mixed_frames() != 32u ||
        hw.spu2().nonzero_frames() != 0u || hw.spu2().pcm_peak() != 0u)
    {
        return false;
    }
    for (const int16_t sample : pcm)
    {
        if (sample != 0)
        {
            return false;
        }
    }

    // Independent AutoDMA planar PCM through the same mixer.
    iop::c_kernel kernel;
    if (!kernel.attach_hle_hardware())
    {
        return false;
    }
    auto& input_hw = kernel.hardware();
    pcm.clear();
    input_hw.bind_spu2_pcm_sink(&pcm, [](void* user, const int16_t* data, uint32_t frames)
    {
        auto& output = *static_cast<std::vector<int16_t>*>(user);
        output.insert(output.end(), data, data + frames * 2u);
    });
    constexpr uint32_t source = 0x60030u;
    constexpr uint32_t dma = iop::c_hw::k_spu2_dma4_base;
    if (!kernel.cpu().write32(source, 0xc0004000u) ||
        !input_hw.write(base + 0x1b0u, 2u, 1u) ||
        !input_hw.write(dma, 4u, source) ||
        !input_hw.write(dma + 4u, 4u, 0x00010001u) ||
        !input_hw.write(dma + 8u, 4u, 0x01000201u))
    {
        return false;
    }
    (void)kernel.run(32u * period);
    return pcm.size() == 64u && pcm[0] == 16380 && pcm[1] == -16383 &&
        pcm[2] == 0 && pcm[3] == 0 &&
        input_hw.spu2().mixed_frames() == 32u &&
        input_hw.spu2().nonzero_frames() == 1u &&
        input_hw.spu2().input_frames_consumed() == 1u;
}

bool test_spu2_auto_dma()
{
    iop::c_kernel kernel;
    if (!kernel.attach_hle_hardware())
    {
        return false;
    }
    auto& hw = kernel.hardware();
    auto& cpu = kernel.cpu();
    constexpr uint32_t spu = iop::c_spu2::k_base;
    constexpr uint32_t dma = iop::c_hw::k_spu2_dma4_base;
    constexpr uint32_t source = 0x60030u;
    constexpr uint32_t period = iop::c_spu2::k_cycles_per_sample;
    uint32_t value = 0u;
    uint16_t word = 0u;
    for (uint32_t chunk = 0u; chunk < 3u; ++chunk)
    {
        for (uint32_t i = 0u; i < 256u; ++i)
        {
            const uint32_t address = source + chunk * 1024u + i * 2u;
            const uint16_t left = static_cast<uint16_t>(0x1000u + chunk * 0x100u + i);
            const uint16_t right = static_cast<uint16_t>(0x8000u + chunk * 0x100u + i);
            if (!cpu.write8(address, static_cast<uint8_t>(left)) ||
                !cpu.write8(address + 1u, static_cast<uint8_t>(left >> 8u)) ||
                !cpu.write8(address + 512u, static_cast<uint8_t>(right)) ||
                !cpu.write8(address + 513u, static_cast<uint8_t>(right >> 8u)))
            {
                return false;
            }
        }
    }
    if (!hw.write(spu + 0x1b0u, 2u, 1u) ||
        !hw.write(dma, 4u, source) ||
        !hw.write(dma + 4u, 4u, 0x00010300u) ||
        !hw.write(dma + 8u, 4u, 0x01000201u) ||
        hw.spu2().input_frames(0u) != 512u ||
        hw.spu2_auto_dma_bytes() != 2048u ||
        !hw.spu2_ram_word(0x2000u, &word) || word != 0x1000u ||
        !hw.spu2_ram_word(0x2200u, &word) || word != 0x8000u ||
        !hw.spu2_ram_word(0x2100u, &word) || word != 0x1100u ||
        hw.spu2_dma_cycles_remaining() != 256u * period)
    {
        return false;
    }
    // Later chunk reads live IOP RAM, not a snapshot at DMA start.
    if (!cpu.write8(source + 2048u, 0x55u))
    {
        return false;
    }
    (void)kernel.run(256u * period - 1u);
    if (hw.spu2_auto_dma_bytes() != 2048u ||
        hw.spu2().input_frames(0u) != 257u)
    {
        return false;
    }
    (void)kernel.run(1u);
    if (hw.spu2_auto_dma_bytes() != 3072u ||
        hw.spu2().input_frames(0u) != 512u ||
        !hw.spu2_ram_word(0x2000u, &word) || word != 0x1255u ||
        !hw.spu2_ram_word(0x2200u, &word) || word != 0x8200u ||
        hw.spu2_dma_cycles_remaining() != 32u)
    {
        return false;
    }
    (void)kernel.run(31u);
    if (!hw.read(dma + 8u, 4u, &value) ||
        (value & iop::c_hw::k_spu2_dma_str_bit) == 0u)
    {
        return false;
    }
    (void)kernel.run(1u);
    if (!hw.read(dma + 8u, 4u, &value) ||
        (value & iop::c_hw::k_spu2_dma_str_bit) != 0u ||
        hw.spu2_auto_dma_completed() != 1u ||
        hw.spu2().input_frames_consumed() != 256u ||
        !hw.read(spu + 0x1aau, 2u, &value) || value != 0u)
    {
        return false;
    }
    // Cancel a new transfer before refill and verify no DMA completion.
    if (!hw.write(dma + 8u, 4u, 0x01000201u) ||
        !hw.write(dma + 8u, 4u, 0u))
    {
        return false;
    }
    hw.advance_cycles(1024u * period);
    return hw.spu2_auto_dma_started() == 2u &&
        hw.spu2_auto_dma_completed() == 1u &&
        hw.spu2_auto_dma_bytes() == 3072u &&
        hw.spu2().input_frames(0u) == 0u;
}

bool test_spu2_playback_irq()
{
    iop::c_kernel kernel;
    if (!kernel.attach_hle_hardware())
    {
        return false;
    }
    auto& hw = kernel.hardware();
    constexpr uint32_t base = iop::c_spu2::k_base;
    constexpr uint32_t period = iop::c_spu2::k_cycles_per_sample;
    constexpr uint32_t irq = 1u << iop::c_spu2::k_irq_line;
    uint32_t value = 0u;
    // Default sound RAM is zero-filled (valid unfiltered ADPCM blocks).
    // First block ends at RAM wrap; lookahead starts at word zero.
    if (!hw.write(base + 0x19cu, 2u, 15u) ||
        !hw.write(base + 0x19eu, 2u, 0xffffu) ||
        !hw.write(base + 0x59cu, 2u, 0u) ||
        !hw.write(base + 0x59eu, 2u, 2u) ||
        !hw.write(base + 0x19au, 2u, 0x40u) ||
        !hw.write(base + 0x59au, 2u, 0x40u) ||
        !hw.write(base + 0x1c0u, 2u, 15u) ||
        !hw.write(base + 0x1c2u, 2u, 0xfff8u) ||
        !hw.write(base + 4u, 2u, 0x1000u) ||
        !hw.write(base + 0x1a0u, 2u, 1u) ||
        !kernel.has_guest_work() ||
        hw.spu2().voice_irq_cycles_remaining() != period)
    {
        return false;
    }
    (void)kernel.run(period - 1u);
    if (hw.spu2().irq_latches() != 0u ||
        hw.spu2().voice_irq_cycles_remaining() != 1u ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &value) || value != 0u)
    {
        return false;
    }
    (void)kernel.run(1u);
    if (hw.spu2().irq_latches() != 1u ||
        hw.spu2().playback_irq_events() != 1u ||
        hw.spu2().decoded_blocks() != 2u ||
        hw.spu2().voice_irq_cycles_remaining() != 27u * period ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &value) || (value & irq) == 0u ||
        !hw.write(iop::c_hw::k_i_stat, 4u, ~irq))
    {
        return false;
    }
    (void)kernel.run(27u * period - 1u);
    if (hw.spu2().irq_latches() != 1u ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &value) || (value & irq) != 0u)
    {
        return false;
    }
    (void)kernel.run(1u);
    // The core-one edge reasserts shared IRQ9 even though core zero
    // remained latched. No speculative read of lookahead raised it.
    if (hw.spu2().irq_latches() != 3u ||
        hw.spu2().playback_irq_events() != 2u ||
        hw.spu2().voice_irq_cycles_remaining() != 0u ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &value) || (value & irq) == 0u ||
        !hw.write(iop::c_hw::k_i_stat, 4u, ~irq) ||
        !hw.write(base + 0x1a0u, 2u, 1u))
    {
        return false;
    }
    (void)kernel.run(28u * period);
    if (hw.spu2().playback_irq_events() != 2u ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &value) || (value & irq) != 0u ||
        !hw.write(base + 0x59au, 2u, 0u) ||
        !hw.write(base + 0x59au, 2u, 0x40u) ||
        !hw.write(base + 0x1a0u, 2u, 1u))
    {
        return false;
    }
    (void)kernel.run(28u * period);
    return hw.spu2().playback_irq_events() == 3u &&
        hw.spu2().irq_latches() == 3u &&
        hw.read(iop::c_hw::k_i_stat, 4u, &value) && (value & irq) != 0u;
}

bool test_spu2_irq_edges_and_spdif()
{
    iop::c_kernel kernel;
    if (!kernel.attach_hle_hardware())
    {
        return false;
    }
    iop::c_hw& hw = kernel.hardware();
    iop::c_cpu& cpu = kernel.cpu();
    constexpr uint32_t spu = iop::c_spu2::k_base;
    constexpr uint32_t dma7 = iop::c_hw::k_spu2_dma7_base;
    constexpr uint32_t src = 0x00032000u;
    uint32_t value = 0u;
    // Core zero latches IRQ9, then I_STAT acknowledges the edge while
    // the SPU2 core's internal latch remains set.
    if (!hw.write(spu + 0x19au, 2u, 0x40u) ||
        !hw.write(spu + 0x19eu, 2u, 0x0500u) ||
        !hw.write(spu + 0x1aau, 2u, 0x0500u) ||
        !hw.write(spu + 0x1acu, 2u, 0x1234u) ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &value) ||
        (value & (1u << iop::c_spu2::k_irq_line)) == 0u ||
        !hw.write(iop::c_hw::k_i_stat, 4u,
                  ~(1u << iop::c_spu2::k_irq_line)))
    {
        return false;
    }
    // A NEW latch on core one, triggered by an actual DMA write,
    // must reassert IRQ9 even while core zero's latch stays set.
    if (!hw.write(spu + 0x59au, 2u, 0x40u) ||
        !hw.write(spu + 0x59eu, 2u, 0x0600u) ||
        !hw.write(spu + 0x5aau, 2u, 0x0600u) ||
        !cpu.write32(src, 0x44332211u) ||
        !hw.write(dma7, 4u, src) ||
        !hw.write(dma7 + 4u, 4u, 0x00010001u) ||
        !hw.write(dma7 + 8u, 4u, 0x01000001u) ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &value) ||
        (value & (1u << iop::c_spu2::k_irq_line)) == 0u ||
        !hw.read(spu + 0x7c2u, 2u, &value) ||
        (value & 0x0cu) != 0x0cu)
    {
        return false;
    }
    // Reference SPDIF_OUT accepts a 32-bit store; its 0x7c2
    // upper halfword is ignored, not treated as a partial-store fault.
    if (!hw.write(spu + 0x7c0u, 4u, 0xdead1234u) ||
        !hw.read(spu + 0x7c0u, 2u, &value) || value != 0x1234u ||
        !hw.read(spu + 0x7c2u, 2u, &value) ||
        (value & 0x0cu) != 0x0cu ||
        hw.write(spu + 0x7c4u, 4u, 0xbadbeefu))
    {
        return false;
    }
    return true;
}

// Native PADMAN needs the original guest SIO2 command/response transport.
// No IRQ or DMA completion may be generated by mere register setup.
bool test_guest_sio2_memory_card()
{
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto folder = std::filesystem::temp_directory_path() /
        ("ee-card-test-" + std::to_string(nonce));
    if (!std::filesystem::create_directory(folder))
    {
        return false;
    }
    struct s_remove_directory
    {
        std::filesystem::path m_directory;
        ~s_remove_directory()
        {
            std::error_code error;
            std::filesystem::remove_all(m_directory, error);
        }
    } cleanup{folder};
    const auto image = folder / "card.ps2";
    const std::vector<char> blank(8650752u, static_cast<char>(0xffu));
    std::ofstream output(image, std::ios::binary);
    output.write(blank.data(), blank.size());
    output.close();

    iop::c_memory_card card;
    if (!output || card.mount(folder / "missing.ps2") || !card.mount(image))
    {
        return false;
    }

    iop::c_hw hw;
    iop::c_cpu cpu;
    const auto transfer = [&](const std::vector<uint8_t>& request,
                              const std::vector<uint8_t>& expected)
    {
        if (!hw.write(iop::c_hw::k_sio2_ctrl, 4u, 0x0cu) ||
            !hw.write(iop::c_hw::k_sio2_send3_base, 4u,
                2u | (static_cast<uint32_t>(request.size()) << 8u) |
                (static_cast<uint32_t>(expected.size()) << 18u)))
        {
            return false;
        }
        for (const uint8_t byte : request)
        {
            if (!hw.write(iop::c_hw::k_sio2_tx_fifo, 1u, byte))
            {
                return false;
            }
        }
        if (!hw.write(iop::c_hw::k_sio2_ctrl, 4u, 1u))
        {
            return false;
        }
        for (const uint8_t byte : expected)
        {
            uint32_t value = 0u;
            if (!hw.read(iop::c_hw::k_sio2_rx_fifo, 1u, &value) || value != byte)
            {
                return false;
            }
        }
        return true;
    };

    hw.bind_dma_cpu(&cpu);
    const std::vector<uint8_t> ack{0xffu, 0xffu, 0x2bu, 0x55u};
    if (!transfer({0x81u, 0x11u}, {0xffu, 0xffu, 0xffu, 0xffu}))
    {
        return false;
    }
    hw.bind_memory_card(&card);
    if (hw.complete_memory_card_auth_reset(2u, 0u) ||
        !transfer({0x81u, 0xf3u, 0u, 0u, 0u},
                  {0xffu, 0xffu, 0u, 0x2bu, 0x55u}) ||
        hw.complete_memory_card_auth_reset(3u, 0u) ||
        hw.complete_memory_card_auth_reset(2u, 1u) ||
        !hw.complete_memory_card_auth_reset(2u, 0u) ||
        hw.complete_memory_card_auth_reset(2u, 0u) ||
        !transfer({0x81u, 0x26u},
                  {0xffu, 0xffu, 0x2bu, 0u, 2u, 16u, 0u,
                   0u, 0x40u, 0u, 0u, 0x52u, 0x55u}) ||
        !transfer({0x81u, 0x22u, 1u, 0u, 0u, 0u}, ack) ||
        !transfer({0x81u, 0x42u, 2u, 0x12u, 0x34u, 0u},
                  {0xffu, 0xffu, 0xffu, 0xffu}) ||
        !transfer({0x81u, 0x42u, 2u, 0x12u, 0x34u, 0x26u}, ack) ||
        !card.save())
    {
        return false;
    }

    // IOP reboot preserves the mounted card. Remount from disk as well to
    // verify both data persistence and the serial read checksum.
    hw.reset();
    hw.bind_dma_cpu(&cpu);
    if (!transfer({0x81u, 0x11u}, ack))
    {
        return false;
    }
    iop::c_memory_card persisted;
    if (!persisted.mount(image))
    {
        return false;
    }
    hw.bind_memory_card(&persisted);
    return transfer({0x81u, 0x23u, 1u, 0u, 0u, 0u}, ack) &&
        transfer({0x81u, 0x43u, 2u},
                 {0xffu, 0xffu, 0u, 0x2bu, 0x12u, 0x34u, 0x26u, 0x55u});
}

bool test_guest_sio2_controller()
{
    iop::c_kernel kernel;
    if (!kernel.attach_hle_hardware())
    {
        return false;
    }
    iop::c_hw& hw = kernel.hardware();
    iop::c_cpu& cpu = kernel.cpu();
    constexpr uint32_t send3 = iop::c_hw::k_sio2_send3_base;
    constexpr uint32_t tx = iop::c_hw::k_sio2_tx_fifo;
    constexpr uint32_t rx = iop::c_hw::k_sio2_rx_fifo;
    constexpr uint32_t ctrl = iop::c_hw::k_sio2_ctrl;
    constexpr uint32_t irq = uint32_t{1} << 17u;
    constexpr uint32_t source = 0x00032000u;
    constexpr uint32_t destination = 0x00033000u;
    constexpr uint32_t dma11 = iop::c_hw::k_sio2_dma_rx_base;
    constexpr uint32_t dma12 = iop::c_hw::k_sio2_dma_tx_base;
    uint32_t status = 0u;
    uint8_t result[8]{};
    const uint8_t poll[5] = {0x01u, 0x42u, 0u, 0u, 0u};
    const uint8_t neutral_reply[5] =
    {
        0xffu, 0x41u, 0x5au, 0xffu, 0xffu
    };
    // The original SIO2MAN interrupt handler at 0x00020180
    // accesses this device latch through its KSEG1 alias.
    constexpr uint32_t intr = iop::c_hw::k_sio2_intr;
    constexpr uint32_t guest_intr = 0xbf808280u;

    // Real PIO: a started and fully transmitted SEND3-described packet
    // can obtain the neutral virtual controller reply and IRQ17.
    if (!hw.set_interrupt_mask(17u, true) ||
        !hw.write(send3, 4u, (5u << 8u) | (5u << 18u)) ||
        !hw.write(tx, 1u, poll[0u]) ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &status) ||
        (status & irq) != 0u ||
        !hw.read(rx, 1u, &status) || status != 0xffu ||
        !cpu.read32(guest_intr, &status) || status != 0u)
    {
        return false;
    }
    // The actual SIO2MAN IRX reads the uncached byte RX FIFO at
    // 0xbf808264. An empty FIFO returns 0xff without creating a
    // controller response, interrupt or DMA completion.
    uint8_t guest_empty_rx = 0u;
    if (!cpu.read8(0xbf808264u, &guest_empty_rx) ||
        guest_empty_rx != 0xffu ||
        hw.sio2_diagnostics().m_empty_rx_reads != 2u ||
        hw.sio2_diagnostics().m_rx_bytes != 0u ||
        hw.sio2_packets() != 0u ||
        hw.sio2_completed_transfers() != 0u ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &status) ||
        (status & irq) != 0u)
    {
        return false;
    }

    // Requesting a transfer with an incomplete packet must not
    // manufacture a device response, DMA completion or interrupt.
    if (!hw.write(ctrl, 4u, 1u) ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &status) ||
        (status & irq) != 0u)
    {
        return false;
    }
    // An actual CTRL.start with a short guest PIO packet must refuse
    // completion without clearing any DMA/IRQ state. Attribute the
    // exact rejected preflight and preserve the guest packet bytes.
    const iop::c_hw::s_sio2_diagnostics first_rejection =
        hw.sio2_diagnostics();
    if (first_rejection.m_last_rejection !=
            iop::c_hw::e_sio2_rejection::packet_incomplete ||
        first_rejection.m_rejected_starts != 1u ||
        first_rejection.m_tx_bytes != 1u ||
        first_rejection.m_rx_bytes != 0u ||
        first_rejection.m_packet_index != 0u ||
        first_rejection.m_packets_at_last_start != 0u ||
        first_rejection.m_tx_bytes_at_last_start != 1u ||
        first_rejection.m_rx_bytes_at_last_start != 0u ||
        first_rejection.m_send3[0u] !=
            ((5u << 8u) | (5u << 18u)) ||
        !hw.sio2_transfer_requested() ||
        !hw.read(intr, 4u, &status) || status != 0u)
    {
        return false;
    }
    for (uint32_t index = 1u; index < 5u; ++index)
    {
        if (!hw.write(tx, 1u, poll[index]))
        {
            return false;
        }
    }
    if (!hw.read(iop::c_hw::k_i_stat, 4u, &status) ||
        (status & irq) != 0u ||
        !hw.write(ctrl, 4u, 1u) ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &status) ||
        (status & irq) == 0u)
    {
        return false;
    }
    const iop::c_hw::s_sio2_diagnostics first_packet =
        hw.sio2_diagnostics();
    if (hw.sio2_ctrl_starts() != 2u ||
        hw.sio2_packets() != 1u ||
        hw.sio2_completed_transfers() != 1u ||
        // The authentic SIO2 interrupt completion bit is latched
        // before the IOP IRQ17 handler reads and acknowledges it.
        !cpu.read32(guest_intr, &status) || status != 1u ||
        !cpu.write32(guest_intr, 2u) ||
        !hw.read(intr, 4u, &status) || status != 1u ||
        !cpu.write32(guest_intr, 1u) ||
        !cpu.read32(guest_intr, &status) || status != 0u ||
        // Device W1C does not acknowledge the separate IOP INTC bit.
        !hw.read(iop::c_hw::k_i_stat, 4u, &status) ||
        (status & irq) == 0u ||
        first_packet.m_first_packet_count != 1u ||
        first_packet.m_packets_at_last_start != 1u ||
        first_packet.m_tx_bytes_at_last_start != 0u ||
        first_packet.m_rx_bytes_at_last_start != 5u ||
        first_packet.m_first_packets[0u].m_port != 0u ||
        first_packet.m_first_packets[0u].m_input_size != 5u ||
        first_packet.m_first_packets[0u].m_output_size != 5u ||
        first_packet.m_first_packets[0u].m_first_tx_byte != 0x01u ||
        first_packet.m_first_packets[0u].m_command != 0x42u ||
        !first_packet.m_first_packets[0u].m_pad_present)
    {
        return false;
    }
    for (uint32_t index = 0u; index < 5u; ++index)
    {
        if (!hw.read(rx, 1u, &status) ||
            status != neutral_reply[index])
        {
            return false;
        }
    }
    if (!hw.read(rx, 1u, &status) || status != 0xffu ||
        !hw.write(iop::c_hw::k_i_stat, 4u, ~irq) ||
        hw.pending_interrupts() != 0u)
    {
        return false;
    }
    // As in ps2-web-recomp/SDBZ, an actual empty PIO CTRL.start
    // still completes through IRQ17. No new controller bytes or
    // DMA11/12 completion may be generated for this control event.
    if (!hw.write(ctrl, 4u, 1u) ||
        hw.sio2_diagnostics().m_last_rejection !=
            iop::c_hw::e_sio2_rejection::none ||
        hw.sio2_diagnostics().m_rejected_starts != 1u ||
        hw.sio2_diagnostics().m_packet_index != 1u ||
        hw.sio2_diagnostics().m_rx_bytes != 0u ||
        hw.sio2_transfer_requested() ||
        hw.sio2_completed_transfers() != 2u ||
        !cpu.read32(guest_intr, &status) || status != 1u ||
        !cpu.write32(guest_intr, status) ||
        !cpu.read32(guest_intr, &status) || status != 0u ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &status) ||
        (status & irq) == 0u ||
        !hw.write(iop::c_hw::k_i_stat, 4u, ~irq))
    {
        return false;
    }

    // Original DMA11 copies an actual controller command from IOP RAM;
    // DMA12 stores the received bytes in the guest's requested buffer.
    // Both requests are merely armed until SIO2 CTRL.start executes.
    if (!hw.write(ctrl, 4u, 0x0cu) ||
        !hw.write(send3, 4u, (8u << 8u) | (8u << 18u)) ||
        !cpu.dma_write(source, poll, 5u) ||
        !hw.write(dma11, 4u, source) ||
        !hw.write(dma11 + 4u, 4u, 0x00010002u) ||
        !hw.write(dma11 + 8u, 4u, 0x01000000u) ||
        !hw.write(dma12, 4u, destination) ||
        !hw.write(dma12 + 4u, 4u, 0x00010002u) ||
        !hw.write(dma12 + 8u, 4u, 0x01000000u) ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &status) ||
        (status & irq) != 0u ||
        !hw.read(dma11 + 8u, 4u, &status) ||
        (status & 0x01000000u) == 0u ||
        !hw.read(dma12 + 8u, 4u, &status) ||
        (status & 0x01000000u) == 0u)
    {
        return false;
    }
    if (!hw.write(ctrl, 4u, 1u) ||
        !cpu.dma_read(destination, result, 8u) ||
        result[0u] != 0xffu || result[1u] != 0x41u ||
        result[2u] != 0x5au || result[3u] != 0xffu ||
        result[4u] != 0xffu ||
        !hw.read(dma11 + 8u, 4u, &status) ||
        (status & 0x01000000u) != 0u ||
        !hw.read(dma12 + 8u, 4u, &status) ||
        (status & 0x01000000u) != 0u ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &status) ||
        (status & irq) == 0u)
    {
        return false;
    }

    const iop::c_hw::s_sio2_diagnostics second_packet =
        hw.sio2_diagnostics();
    if (hw.sio2_ctrl_starts() != 4u ||
        hw.sio2_packets() != 2u ||
        hw.sio2_completed_transfers() != 3u ||
        second_packet.m_last_rejection !=
            iop::c_hw::e_sio2_rejection::none ||
        second_packet.m_rejected_starts != 1u ||
        second_packet.m_first_packet_count != 2u ||
        second_packet.m_packets_at_last_start != 1u ||
        second_packet.m_tx_bytes_at_last_start != 0u ||
        second_packet.m_rx_bytes_at_last_start != 0u ||
        second_packet.m_first_packets[1u].m_port != 0u ||
        second_packet.m_first_packets[1u].m_input_size != 8u ||
        second_packet.m_first_packets[1u].m_output_size != 8u ||
        second_packet.m_first_packets[1u].m_command != 0x42u ||
        !second_packet.m_first_packets[1u].m_pad_present)
    {
        return false;
    }

    // An out-of-bounds DMA11 source cannot cause a false transfer or
    // clear STR. Real DMA reads must pass RAM preflight.
    if (!hw.write(ctrl, 4u, 0x0cu) ||
        !hw.write(send3, 4u, (8u << 8u) | (8u << 18u)) ||
        !hw.write(iop::c_hw::k_i_stat, 4u, ~irq) ||
        !hw.write(dma11, 4u, iop::c_cpu::k_ram_size - 4u) ||
        !hw.write(dma11 + 4u, 4u, 0x00010002u) ||
        !hw.write(dma11 + 8u, 4u, 0x01000000u) ||
        !hw.write(ctrl, 4u, 1u) ||
        !hw.read(dma11 + 8u, 4u, &status) ||
        (status & 0x01000000u) == 0u ||
        !hw.read(iop::c_hw::k_i_stat, 4u, &status) ||
        (status & irq) != 0u ||
        hw.sio2_ctrl_starts() != 5u ||
        hw.sio2_packets() != 2u ||
        hw.sio2_completed_transfers() != 3u ||
        hw.sio2_diagnostics().m_last_rejection !=
            iop::c_hw::e_sio2_rejection::dma11_source ||
        hw.sio2_diagnostics().m_rejected_starts != 2u ||
        hw.sio2_diagnostics().m_dma11[0u] !=
            iop::c_cpu::k_ram_size - 4u)
    {
        return false;
    }
    // Exact SDBZ retail sequence: SIO2MAN sends two original pad
    // 0x42 PIO polls (connected port 0, disconnected port 1),
    // then writes CTRL=0x3bd (FIFO reset + START simultaneously).
    // The reset discards both queued replies before the start handler
    // runs, but IRQ17 must still signal genuine CTRL completion.
    iop::c_kernel retail_kernel;
    if (!retail_kernel.attach_hle_hardware())
    {
        return false;
    }
    iop::c_hw& retail_hw = retail_kernel.hardware();
    if (!retail_hw.set_interrupt_mask(17u, true) ||
        !retail_hw.write(send3, 4u, (5u << 8u) | (5u << 18u)) ||
        !retail_hw.write(send3 + 4u, 4u,
                         (5u << 8u) | (5u << 18u) | 1u))
    {
        return false;
    }
    for (uint32_t packet = 0u; packet < 2u; ++packet)
    {
        for (const uint8_t byte : poll)
        {
            if (!retail_hw.write(tx, 1u, byte))
            {
                return false;
            }
        }
    }
    const iop::c_hw::s_sio2_diagnostics before_start =
        retail_hw.sio2_diagnostics();
    if (before_start.m_first_packet_count != 2u ||
        before_start.m_packet_index != 2u ||
        before_start.m_tx_bytes != 0u ||
        before_start.m_rx_bytes != 10u ||
        before_start.m_first_packets[0u].m_port != 0u ||
        !before_start.m_first_packets[0u].m_pad_present ||
        before_start.m_first_packets[1u].m_port != 1u ||
        before_start.m_first_packets[1u].m_pad_present ||
        before_start.m_first_packets[0u].m_command != 0x42u ||
        before_start.m_first_packets[1u].m_command != 0x42u ||
        !retail_hw.read(iop::c_hw::k_i_stat, 4u, &status) ||
        (status & irq) != 0u)
    {
        return false;
    }
    if (!retail_hw.write(ctrl, 4u, 0x03bdu))
    {
        return false;
    }
    const iop::c_hw::s_sio2_diagnostics after_start =
        retail_hw.sio2_diagnostics();
    if (retail_hw.sio2_ctrl_starts() != 1u ||
        retail_hw.sio2_packets() != 2u ||
        retail_hw.sio2_completed_transfers() != 1u ||
        retail_hw.sio2_transfer_requested() ||
        after_start.m_last_rejection !=
            iop::c_hw::e_sio2_rejection::none ||
        after_start.m_rejected_starts != 0u ||
        after_start.m_ctrl != 0x03bdu ||
        after_start.m_packet_index != 0u ||
        after_start.m_packets_at_last_start != 2u ||
        after_start.m_tx_bytes_at_last_start != 0u ||
        after_start.m_rx_bytes_at_last_start != 0u ||
        after_start.m_rx_bytes != 0u ||
        !retail_hw.read(rx, 1u, &status) || status != 0xffu ||
        !retail_kernel.cpu().read32(guest_intr, &status) ||
        status != 1u ||
        !retail_kernel.cpu().write32(guest_intr, status) ||
        !retail_kernel.cpu().read32(guest_intr, &status) ||
        status != 0u ||
        !retail_hw.read(iop::c_hw::k_i_stat, 4u, &status) ||
        (status & irq) == 0u ||
        !retail_hw.read(dma11 + 8u, 4u, &status) ||
        (status & 0x01000000u) != 0u ||
        !retail_hw.read(dma12 + 8u, 4u, &status) ||
        (status & 0x01000000u) != 0u)
    {
        return false;
    }

    // Reproduce original SIO2MAN's two read-modify-write CTRL calls:
    // first reset via CTRL | 0x0c, then START via CTRL | 1 AFTER
    // transmitting the two genuine 5-byte PADMAN poll packets.
    // The reference returns zero for CTRL reads, so START does not
    // repeat the FIFO reset and the original worker receives the
    // already-queued pad replies after IRQ17.
    iop::c_kernel guest_sequence_kernel;
    if (!guest_sequence_kernel.attach_hle_hardware())
    {
        return false;
    }
    iop::c_hw& guest_sequence = guest_sequence_kernel.hardware();
    guest_sequence.configure_sio2_pad0_cross_pulse(1u, 1u);
    uint32_t guest_ctrl = 0u;
    if (!guest_sequence.read(ctrl, 4u, &guest_ctrl) ||
        guest_ctrl != 0u ||
        !guest_sequence.write(ctrl, 4u, guest_ctrl | 0x0cu) ||
        !guest_sequence.read(ctrl, 4u, &guest_ctrl) ||
        guest_ctrl != 0u ||
        !guest_sequence.write(send3, 4u,
            (5u << 8u) | (5u << 18u)) ||
        !guest_sequence.write(send3 + 4u, 4u,
            (5u << 8u) | (5u << 18u) | 1u))
    {
        return false;
    }
    for (uint32_t port = 0u; port < 2u; ++port)
    {
        for (const uint8_t byte : poll)
        {
            if (!guest_sequence.write(tx, 1u, byte))
            {
                return false;
            }
        }
    }
    if (guest_sequence.sio2_diagnostics().m_rx_bytes != 10u ||
        guest_sequence.sio2_completed_transfers() != 0u ||
        !guest_sequence.read(ctrl, 4u, &guest_ctrl) ||
        guest_ctrl != 0u ||
        !guest_sequence.write(ctrl, 4u, guest_ctrl | 1u) ||
        guest_sequence.sio2_completed_transfers() != 1u ||
        guest_sequence.sio2_diagnostics().m_ctrl != 1u ||
        guest_sequence.sio2_diagnostics().m_rx_bytes != 10u)
    {
        return false;
    }
    for (uint32_t port = 0u; port < 2u; ++port)
    {
        for (uint32_t reply_index = 0u; reply_index < 5u;
             ++reply_index)
        {
            uint8_t guest_byte = 0u;
            const uint8_t expected =
                port == 0u ? (reply_index == 4u ?
                    0xbfu : neutral_reply[reply_index]) : 0xffu;
            if (!guest_sequence_kernel.cpu().read8(
                    0xbf808264u, &guest_byte) ||
                guest_byte != expected)
            {
                return false;
            }
        }
    }
    const iop::c_hw::s_sio2_diagnostics guest_sequence_result =
        guest_sequence.sio2_diagnostics();
    if (guest_sequence_result.m_pad0_polls != 1u ||
        guest_sequence_result.m_pad0_cross_pressed_polls != 1u ||
        guest_sequence_result.m_pad0_cross_bytes_pio_read != 1u ||
        guest_sequence_result.m_pad0_cross_bytes_dma_written != 0u ||
        guest_sequence_result.m_pad0_cross_bytes_fifo_discarded != 0u ||
        guest_sequence_result.m_rx_bytes != 0u ||
        guest_sequence_result.m_empty_rx_reads != 0u ||
        guest_sequence_result.m_rejected_starts != 0u ||
        guest_sequence.sio2_packets() != 2u)
    {
        return false;
    }

    // A deliberate host Cross press is delivered ONLY in genuine
    // connected port-0 SIO2 0x42 reply bytes, for a bounded number of
    // guest pad polls. No new packet/IRQ is created by configuration,
    // and the ordinary neutral state must resume automatically.
    iop::c_kernel input_probe_kernel;
    if (!input_probe_kernel.attach_hle_hardware())
    {
        return false;
    }
    iop::c_hw& input_probe = input_probe_kernel.hardware();
    input_probe.configure_sio2_pad0_cross_pulse(3u, 2u);
    if (input_probe.sio2_diagnostics().m_pad0_cross_first_poll != 3u ||
        input_probe.sio2_diagnostics().m_pad0_cross_poll_count != 2u ||
        input_probe.sio2_packets() != 0u ||
        input_probe.sio2_completed_transfers() != 0u)
    {
        return false;
    }
    for (uint32_t pad_poll = 1u; pad_poll <= 7u; ++pad_poll)
    {
        const bool live_pressed = pad_poll == 3u;
        input_probe.set_sio2_pad0_input(
            live_pressed ? 0x0820u : 0u, 128u, 128u, 128u, 128u);
        if (!input_probe.write(ctrl, 4u, 0x0cu) ||
            !input_probe.write(send3, 4u, (5u << 8u) | (5u << 18u)))
        {
            return false;
        }
        for (const uint8_t byte : poll)
        {
            if (!input_probe.write(tx, 1u, byte))
            {
                return false;
            }
        }
        for (uint32_t reply_index = 0u; reply_index < 5u;
             ++reply_index)
        {
            uint32_t expected = neutral_reply[reply_index];
            if (reply_index == 3u && live_pressed)
            {
                expected = 0xdfu; // D-pad right
            }
            if (reply_index == 4u && (pad_poll == 3u || pad_poll == 4u))
            {
                expected = live_pressed ? 0xb7u : 0xbfu; // Cross plus R1
            }
            if (!input_probe.read(rx, 1u, &status) ||
                status != expected)
            {
                return false;
            }
        }
        if (input_probe.pad_trace_window() != (pad_poll >= 2u && pad_poll <= 6u) ||
            input_probe.pad0_polls() != pad_poll ||
            input_probe.sio2_completed_transfers() != pad_poll - 1u ||
            !input_probe.write(ctrl, 4u, 0x03bdu) ||
            input_probe.sio2_completed_transfers() != pad_poll ||
            !input_probe.write(intr, 4u, 1u) ||
            !input_probe.write(iop::c_hw::k_i_stat, 4u, ~irq))
        {
            return false;
        }
    }
    const iop::c_hw::s_sio2_diagnostics input_result =
        input_probe.sio2_diagnostics();
    if (input_result.m_pad0_polls != 7u ||
        input_result.m_pad0_cross_pressed_polls != 2u ||
        input_result.m_pad0_cross_first_poll != 3u ||
        input_result.m_pad0_cross_poll_count != 2u ||
        input_result.m_pad0_cross_bytes_pio_read != 2u ||
        input_result.m_pad0_cross_bytes_dma_written != 0u ||
        input_result.m_pad0_cross_bytes_fifo_discarded != 0u ||
        input_probe.sio2_packets() != 7u ||
        input_probe.sio2_ctrl_starts() != 7u ||
        input_probe.sio2_completed_transfers() != 7u ||
        input_result.m_rx_bytes != 0u)
    {
        return false;
    }
    // A reply that is generated but never consumed must not count as
    // delivered. The guest can reset the FIFO via CTRL=0x0c; retain
    // host-only provenance for the discarded Cross button byte.
    input_probe.configure_sio2_pad0_cross_pulse(1u, 1u);
    if (!input_probe.write(ctrl, 4u, 0x0cu) ||
        !input_probe.write(send3, 4u, (5u << 8u) | (5u << 18u)))
    {
        return false;
    }
    for (const uint8_t byte : poll)
    {
        if (!input_probe.write(tx, 1u, byte))
        {
            return false;
        }
    }
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        if (!input_probe.read(rx, 1u, &status) ||
            status != neutral_reply[index])
        {
            return false;
        }
    }
    if (!input_probe.write(ctrl, 4u, 0x0cu))
    {
        return false;
    }
    const iop::c_hw::s_sio2_diagnostics discarded =
        input_probe.sio2_diagnostics();
    if (discarded.m_pad0_cross_pressed_polls != 1u ||
        discarded.m_pad0_cross_bytes_pio_read != 0u ||
        discarded.m_pad0_cross_bytes_dma_written != 0u ||
        discarded.m_pad0_cross_bytes_fifo_discarded != 1u ||
        discarded.m_rx_bytes != 0u ||
        input_probe.sio2_completed_transfers() != 7u)
    {
        return false;
    }

    input_probe_kernel.reset();
    if (input_probe_kernel.hardware().sio2_diagnostics()
            .m_pad0_cross_poll_count != 0u ||
        input_probe_kernel.hardware().sio2_diagnostics()
            .m_pad0_cross_bytes_fifo_discarded != 0u)
    {
        return false;
    }

    kernel.reset();
    return kernel.hardware().sio2_ctrl_starts() == 0u &&
        kernel.hardware().sio2_packets() == 0u &&
        kernel.hardware().read(intr, 4u, &status) &&
        status == 0u &&
        kernel.hardware().sio2_completed_transfers() == 0u &&
        !kernel.hardware().sio2_transfer_requested() &&
        kernel.hardware().sio2_pending_tx_bytes() == 0u &&
        kernel.hardware().pending_interrupts() == 0u;
}

} // namespace

int main()
{
    if (!test_guest_sio2_memory_card())
    {
        return fail("guest SIO2 memory card exchange or image persistence");
    }
    if (!test_guest_sio2_controller())
    {
        return fail("guest SIO2 PIO/DMA11/12 transport or IRQ17 fidelity");
    }
    if (!test_spu2_mixer_rings())
    {
        return fail("SPU2 mixer-ring taps, silent IRQ or ring wrap");
    }
    if (!test_spu2_voice_routing())
    {
        return fail("SPU2 independent dry/wet voice routing");
    }
    if (!test_spu2_effect_registers())
    {
        return fail("SPU2 effect registers and work-area address wrapping");
    }
    if (!test_spu2_mmix())
    {
        return fail("SPU2 MMIX source gates and core cascade");
    }
    if (!test_spu2_pcm_mixer())
    {
        return fail("SPU2 stereo PCM mixing, input gain or host sink");
    }
    if (!test_spu2_auto_dma())
    {
        return fail("SPU2 AutoDMA planar ring, refill, completion or cancellation");
    }
    if (!test_spu2_playback_irq())
    {
        return fail("SPU2 playback IRQ timing, cross-core latch or idle deadline");
    }
    if (!test_spu2_irq_edges_and_spdif())
    {
        return fail("per-core SPU2 IRQ9 and SPDIF word parity");
    }
    if (!test_spu2_envelopes())
    {
        return fail("SPU2 ADSR, fixed/swept volumes or release timing");
    }
    if (!test_spu2_adpcm_voice_progression())
    {
        return fail("SPU2 ADPCM decoding, pitch, loop or NAX progression");
    }
    if (!test_spu2_sample_clock())
    {
        return fail("SPU2 guest sample clock or IOP cycle ownership");
    }
    if (!test_authentic_spu2_dma())
    {
        return fail("authentic SPU2 DMA4/7 copy, timing or cancellation");
    }
    iop::c_hw hardware{};
    uint32_t actual = 0u;

    if (!hardware.read(iop::c_hw::k_i_ctrl, 4u, &actual) ||
        actual != 1u ||
        !hardware.read(iop::c_hw::k_i_ctrl, 4u, &actual) ||
        actual != 0u ||
        hardware.pending_interrupts() != 0u)
    {
        return fail("I_CTRL read-to-disable reset behavior");
    }
    if (!hardware.write(iop::c_hw::k_i_ctrl, 4u, 0xffffffffu) ||
        !hardware.raise_interrupt(3u) ||
        hardware.raise_interrupt(32u) ||
        !hardware.write(iop::c_hw::k_i_mask, 4u, 1u << 3u) ||
        hardware.pending_interrupts() != (1u << 3u) ||
        !hardware.read(iop::c_hw::k_i_stat, 4u, &actual) ||
        actual != (1u << 3u) ||
        !hardware.write(iop::c_hw::k_i_stat, 4u, 0xffffffffu) ||
        hardware.pending_interrupts() != (1u << 3u) ||
        !hardware.write(iop::c_hw::k_i_stat, 4u, ~(1u << 3u)) ||
        hardware.pending_interrupts() != 0u)
    {
        return fail("IRQ status/mask AND-acknowledge or pending semantics");
    }

    const uint32_t scratch = iop::c_hw::k_scratchpad_base;
    if (!hardware.write(scratch, 4u, 0x11223344u) ||
        !hardware.read(scratch + 1u, 1u, &actual) ||
        actual != 0x33u ||
        !hardware.write(scratch + 2u, 2u, 0xaabbu) ||
        !hardware.read(scratch, 4u, &actual) ||
        actual != 0xaabb3344u ||
        !hardware.write(scratch + 1020u, 4u, 0xcafebabeu) ||
        hardware.write(scratch + 1022u, 4u, 0u) ||
        hardware.read(scratch + 1022u, 4u, &actual) ||
        hardware.read(scratch + 1024u, 1u, &actual) ||
        hardware.read(0x1f801100u, 4u, &actual) ||
        hardware.write(0x1f8010fcu, 4u, 1u))
    {
        return fail("scratchpad size, subword access or unknown MMIO safety");
    }

    // The real LIBSD.IRX thread writes the SPU2 SSBUS window base
    // through KSEG1 (0xBF801404). These are bus configuration latches,
    // not audio writes, DMA completion or successful SPU2 startup.
    for (const auto& entry : std::array<std::array<uint32_t, 2u>, 4u>{{
        {{iop::c_hw::k_ssbus_spu2_addr, 0xbf900000u}},
        {{iop::c_hw::k_ssbus_spu2_addr1, 0xbf900800u}},
        {{iop::c_hw::k_ssbus_spu2_delay, 0x200b31e1u}},
        {{iop::c_hw::k_ssbus_spu2_delay1, 0x200b31e1u}}
    }})
    {
        if (!hardware.write(entry[0], 4u, entry[1]) ||
            !hardware.read(entry[0], 4u, &actual) ||
            actual != entry[1] ||
            hardware.write(entry[0], 2u, 0u) ||
            hardware.read(entry[0], 2u, &actual))
        {
            return fail("SPU2 SSBUS register latch or access width");
        }
    }
    if (hardware.write(0x1f801424u, 4u, 0u) ||
        hardware.read(0x1f801424u, 4u, &actual) ||
        hardware.pending_interrupts() != 0u)
    {
        return fail("unmapped SSBUS register or false SPU2 interrupt");
    }
    hardware.reset();
    for (uint32_t address : {
             iop::c_hw::k_ssbus_spu2_addr,
             iop::c_hw::k_ssbus_spu2_addr1,
             iop::c_hw::k_ssbus_spu2_delay,
             iop::c_hw::k_ssbus_spu2_delay1})
    {
        if (!hardware.read(address, 4u, &actual) || actual != 0u)
        {
            return fail("SPU2 SSBUS configuration survived reset");
        }
    }

    // INTRMAN's extended lines 32..38 and 40..45 map to the
    // primary/secondary DMAC mask bits; merely enabling them must
    // not raise IRQ3 or complete any transfer. Leave reserved line
    // 39 and out-of-range source IDs unsupported.
    if (!hardware.set_interrupt_mask(36u, true) ||
        !hardware.set_interrupt_mask(41u, true) ||
        !hardware.read(iop::c_hw::k_dma_icr, 4u, &actual) ||
        (actual & ((1u << 20u) | 0x00800000u)) !=
            ((1u << 20u) | 0x00800000u) ||
        !hardware.read(iop::c_hw::k_dma_icr2, 4u, &actual) ||
        (actual & (1u << 17u)) == 0u ||
        !hardware.read(iop::c_hw::k_i_mask, 4u, &actual) ||
        (actual & (1u << iop::c_hw::k_dma_irq)) == 0u ||
        !hardware.read(iop::c_hw::k_i_stat, 4u, &actual) ||
        actual != 0u || hardware.pending_interrupts() != 0u ||
        hardware.set_interrupt_mask(39u, true) ||
        hardware.set_interrupt_mask(46u, true) ||
        !hardware.set_interrupt_mask(36u, false) ||
        !hardware.set_interrupt_mask(41u, false) ||
        !hardware.read(iop::c_hw::k_dma_icr, 4u, &actual) ||
        (actual & (1u << 20u)) != 0u ||
        (actual & 0x00800000u) == 0u ||
        !hardware.read(iop::c_hw::k_dma_icr2, 4u, &actual) ||
        (actual & (1u << 17u)) != 0u ||
        hardware.pending_interrupts() != 0u)
    {
        return fail("extended INTRMAN DMA masks or no-fake-IRQ contract");
    }
    hardware.reset();
    if (!hardware.read(iop::c_hw::k_dma_icr, 4u, &actual) ||
        actual != 0u ||
        !hardware.read(iop::c_hw::k_dma_icr2, 4u, &actual) ||
        actual != 0u ||
        !hardware.read(iop::c_hw::k_i_mask, 4u, &actual) ||
        actual != 0u)
    {
        return fail("extended INTRMAN DMA enables survived reset");
    }

    // CPU aliases must reach the same physical SSBUS registers.
    iop::c_kernel audio_kernel;
    if (!audio_kernel.attach_hle_hardware() ||
        !audio_kernel.cpu().write32(0xbf801404u, 0xbf900000u) ||
        !audio_kernel.cpu().read32(0xbf801404u, &actual) ||
        actual != 0xbf900000u ||
        audio_kernel.hardware().pending_interrupts() != 0u)
    {
        return fail("LIBSD guest KSEG1 SPU2 bus write failed");
    }

    // The matched SDBZ LIBSD worker originally faulted on a real
    // R3000 SH at PC 0x0005C8E8, physical SPU2 master register 0x760.
    // Execute that same opcode through KSEG1, not a host-side bypass.
    if (!audio_kernel.cpu().write32(0x1000u, 0xa4450000u))
    {
        return fail("cannot stage LIBSD SPU2 store instruction");
    }
    iop::s_cpu_state& audio_cpu = audio_kernel.cpu().state();
    audio_cpu.m_pc = 0x1000u;
    audio_cpu.m_next_pc = 0x1004u;
    audio_cpu.m_gpr[2] = 0xbf900760u;
    audio_cpu.m_gpr[5] = 0x1234u;
    if (audio_kernel.cpu().step() != iop::e_step_result::completed ||
        !audio_kernel.hardware().read(
            iop::c_spu2::k_base + 0x760u, 2u, &actual) ||
        actual != 0x1234u ||
        audio_kernel.cpu().last_memory_fault().m_valid ||
        audio_kernel.hardware().pending_interrupts() != 0u)
    {
        return fail("guest LIBSD SH to SPU2 master volume failed");
    }

    // Retail SDBZ executes this exact SH v0, 0(a1) instruction at
    // 0x0005A824 after the RPC worker receives WaitSema(0) = -408.
    // PS2SDK LIBSD InitSpdif writes MODE=0x900, MEDIA=0x200 and
    // the final SPDIF control halfword 0x7CA=8. The primary
    // ps2-web-recomp/SDBZ model accepts 0x7CA without latching it.
    // These are configuration writes, not proof of SPU readiness.
    if (!audio_kernel.cpu().write32(0x0005a824u, 0xa4a20000u) ||
        !audio_kernel.hardware().write(
            iop::c_spu2::k_base + 0x7c0u, 2u, 0u) ||
        !audio_kernel.hardware().write(
            iop::c_spu2::k_base + 0x7c0u, 2u, 0x8000u) ||
        !audio_kernel.hardware().write(
            iop::c_spu2::k_base + 0x7c6u, 2u, 0x900u) ||
        !audio_kernel.hardware().write(
            iop::c_spu2::k_base + 0x7c8u, 2u, 0x200u))
    {
        return fail("cannot stage retail LIBSD SPDIF init");
    }
    audio_cpu.m_pc = 0x0005a824u;
    audio_cpu.m_next_pc = 0x0005a828u;
    audio_cpu.m_gpr[2] = 8u;
    audio_cpu.m_gpr[5] = 0xbf9007cau;
    if (audio_kernel.cpu().step() != iop::e_step_result::completed ||
        audio_kernel.cpu().last_memory_fault().m_valid ||
        !audio_kernel.hardware().read(
            iop::c_spu2::k_base + 0x7c0u, 2u, &actual) ||
        actual != 0x8000u ||
        !audio_kernel.hardware().read(
            iop::c_spu2::k_base + 0x7c6u, 2u, &actual) ||
        actual != 0x900u ||
        !audio_kernel.hardware().read(
            iop::c_spu2::k_base + 0x7c8u, 2u, &actual) ||
        actual != 0x200u ||
        !audio_kernel.hardware().read(
            iop::c_spu2::k_base + 0x7cau, 2u, &actual) ||
        actual != 0u ||
        audio_kernel.hardware().read(
            iop::c_spu2::k_base + 0x7c4u, 2u, &actual) ||
        audio_kernel.hardware().write(
            iop::c_spu2::k_base + 0x7ccu, 2u, 8u) ||
        audio_kernel.hardware().write(
            iop::c_spu2::k_base + 0x7cau, 1u, 8u) ||
        audio_kernel.hardware().pending_interrupts() != 0u)
    {
        return fail("guest LIBSD SPDIF init fabricated state or faulted");
    }

    // In SDBZ's 05:57:06 replay, LIBSD's original instruction
    // at 0x0005B450 is lw v0, 0(s3), s3=0xBF8010C8. The physical
    // address is DMA4.CHCR. Run that exact R3000 load via KSEG1,
    // after checking the matching primary/secondary DMA register
    // bank storage. These are requests, never synthetic transfers.
    constexpr uint32_t dma4 = iop::c_hw::k_spu2_dma4_base;
    constexpr uint32_t dma7 = iop::c_hw::k_spu2_dma7_base;
    for (uint32_t base : {dma4, dma7})
    {
        for (uint32_t index = 0u; index < 4u; ++index)
        {
            if (!hardware.read(base + index * 4u, 4u, &actual) ||
                actual != 0u)
            {
                return fail("SPU2 DMA4/7 registers not reset to zero");
            }
        }
    }
    if (!hardware.write(dma4, 4u, 0x00024000u) ||
        !hardware.write(dma4 + 4u, 4u, 0x00010020u) ||
        !hardware.write(dma4 + 8u, 4u, 0x01000201u) ||
        !hardware.write(dma4 + 12u, 4u, 0x0002a000u) ||
        !hardware.write(dma7, 4u, 0x00028000u) ||
        !hardware.write(dma7 + 4u, 4u, 0x00020010u) ||
        !hardware.write(dma7 + 8u, 4u, 0x01000200u) ||
        !hardware.write(dma7 + 12u, 4u, 0x0002c000u) ||
        !hardware.read(dma4 + 8u, 4u, &actual) ||
        actual != 0x01000201u ||
        !hardware.read(dma7 + 8u, 4u, &actual) ||
        actual != 0x01000200u ||
        !hardware.read(dma4 + 12u, 4u, &actual) ||
        actual != 0x0002a000u ||
        !hardware.read(dma7 + 12u, 4u, &actual) ||
        actual != 0x0002c000u ||
        !hardware.read(iop::c_hw::k_dma_icr, 4u, &actual) ||
        actual != 0u ||
        !hardware.read(iop::c_hw::k_dma_icr2, 4u, &actual) ||
        actual != 0u ||
        !hardware.read(iop::c_hw::k_i_stat, 4u, &actual) ||
        actual != 0u ||
        hardware.pending_interrupts() != 0u ||
        !hardware.write(dma4 + 4u, 2u, 0x00abu) ||
        !hardware.read(dma4 + 4u, 4u, &actual) ||
        actual != 0x000100abu ||
        !hardware.write(dma4 + 6u, 2u, 0x3344u) ||
        !hardware.read(dma4 + 4u, 4u, &actual) ||
        actual != 0x334400abu ||
        !hardware.read(dma4 + 6u, 2u, &actual) ||
        actual != 0x3344u ||
        !hardware.read(dma4 + 4u, 1u, &actual) ||
        actual != 0xabu ||
        !hardware.write(dma7 + 8u, 1u, 0x55u) ||
        !hardware.read(dma7 + 8u, 4u, &actual) ||
        actual != 0x01000255u ||
        !hardware.read(dma7 + 9u, 1u, &actual) ||
        actual != 0x02u ||
        !hardware.write(dma7 + 0xbu, 1u, 0x7au) ||
        !hardware.read(dma7 + 8u, 4u, &actual) ||
        actual != 0x7a000255u ||
        hardware.write(dma4 + 5u, 2u, 0u) ||
        hardware.read(dma4 + 7u, 2u, &actual) ||
        hardware.write(dma7 + 0x10u, 4u, 0u) ||
        hardware.read(dma7 + 0x10u, 4u, &actual))
    {
        return fail("SPU2 DMA register banks or no-fake-DMA invariant");
    }

    if (!audio_kernel.cpu().write32(0x0005b450u, 0x8e620000u) ||
        !audio_kernel.hardware().write(
            dma4 + 8u, 4u, 0x01000201u))
    {
        return fail("cannot stage original LIBSD DMA4 CHCR read");
    }
    audio_cpu.m_pc = 0x0005b450u;
    audio_cpu.m_next_pc = 0x0005b454u;
    audio_cpu.m_gpr[19] = 0xbf8010c8u;
    audio_cpu.m_gpr[2] = 0xffffffffu;
    if (audio_kernel.cpu().step() != iop::e_step_result::completed ||
        audio_cpu.m_gpr[2] != 0x01000201u ||
        audio_kernel.cpu().last_memory_fault().m_valid ||
        audio_kernel.hardware().pending_interrupts() != 0u)
    {
        return fail("retail LIBSD R3000 lw DMA4.CHCR faulted");
    }

    // Original R3000 SH v1, 0(a1) at 0x0005B5D4 in the
    // matched 06:03:34 replay: a1=0xBF8010C4 (DMA4.BCR).
    // The guest also addresses the upper BCR halfword at 0xC6.
    if (!audio_kernel.cpu().write32(0x0005b5d4u, 0xa4a30000u) ||
        !audio_kernel.hardware().write(
            dma4 + 4u, 4u, 0xaabbccddu))
    {
        return fail("cannot stage retail LIBSD DMA4 BCR store");
    }
    audio_cpu.m_pc = 0x0005b5d4u;
    audio_cpu.m_next_pc = 0x0005b5d8u;
    audio_cpu.m_gpr[5] = 0xbf8010c4u;
    audio_cpu.m_gpr[3] = 0x1234u;
    if (audio_kernel.cpu().step() != iop::e_step_result::completed ||
        !audio_kernel.cpu().read32(0xbf8010c4u, &actual) ||
        actual != 0xaabb1234u ||
        audio_kernel.cpu().last_memory_fault().m_valid)
    {
        return fail("retail LIBSD R3000 SH DMA4.BCR low half fault");
    }
    audio_cpu.m_pc = 0x0005b5d4u;
    audio_cpu.m_next_pc = 0x0005b5d8u;
    audio_cpu.m_gpr[5] = 0xbf8010c6u;
    audio_cpu.m_gpr[3] = 0x5678u;
    if (audio_kernel.cpu().step() != iop::e_step_result::completed ||
        !audio_kernel.cpu().read32(0xbf8010c4u, &actual) ||
        actual != 0x56781234u ||
        !audio_kernel.hardware().read(
            iop::c_hw::k_dma_icr, 4u, &actual) ||
        actual != 0u ||
        !audio_kernel.hardware().read(
            iop::c_hw::k_i_stat, 4u, &actual) ||
        actual != 0u ||
        audio_kernel.hardware().pending_interrupts() != 0u ||
        audio_kernel.cpu().last_memory_fault().m_valid)
    {
        return fail("retail LIBSD R3000 SH DMA4.BCR upper half fault");
    }

    hardware.reset();
    for (uint32_t base : {dma4, dma7})
    {
        for (uint32_t index = 0u; index < 4u; ++index)
        {
            if (!hardware.read(base + index * 4u, 4u, &actual) ||
                actual != 0u)
            {
                return fail("SPU2 DMA4/7 register bank survived reboot");
            }
        }
    }

    const uint32_t spu2 = iop::c_spu2::k_base;
    if (!hardware.read(spu2 + 0x760u, 2u, &actual) ||
        actual != 0x3fffu ||
        !hardware.read(spu2 + 0x788u, 2u, &actual) ||
        actual != 0x3fffu ||
        !hardware.read(spu2 + 0x768u, 2u, &actual) ||
        actual != 0x7fffu ||
        !hardware.write(spu2 + 0x760u, 4u, 0x55667788u) ||
        !hardware.read(spu2 + 0x760u, 4u, &actual) ||
        actual != 0x55667788u ||
        !hardware.read(spu2 + 0x788u, 2u, &actual) ||
        actual != 0x3fffu ||
        !hardware.write(spu2 + 0x7c6u, 2u, 0x4321u) ||
        !hardware.read(spu2 + 0x7c6u, 2u, &actual) ||
        actual != 0x4321u ||
        hardware.write(spu2 + 0x760u, 1u, 0x77u) ||
        hardware.read(spu2 + 0x760u, 1u, &actual) ||
        hardware.read(spu2 + 0x7c4u, 2u, &actual) ||
        hardware.write(spu2 + 0x800u, 2u, 0u) ||
        hardware.pending_interrupts() != 0u)
    {
        return fail("SPU2 master, core isolation, SPDIF or MMIO widths");
    }

    // Reference core STATX settles for sixteen polls following an
    // enabled ATTR rising edge. SPU RAM port writes advance TSA and
    // assert IRQ9 only on an actual guest-selected IRQA hit.
    if (!hardware.write(spu2 + 0x19au, 2u, 0x8040u))
    {
        return fail("cannot enable reference SPU2 core");
    }
    for (uint32_t index = 0u; index < 16u; ++index)
    {
        if (!hardware.read(spu2 + 0x344u, 2u, &actual) ||
            actual != 0u)
        {
            return fail("SPU2 STATX settled before reference delay");
        }
    }
    if (!hardware.read(spu2 + 0x344u, 2u, &actual) ||
        actual != 0x80u ||
        !hardware.write(spu2 + 0x1a8u, 2u, 0u) ||
        !hardware.write(spu2 + 0x1aau, 2u, 0x800u) ||
        !hardware.write(spu2 + 0x1acu, 2u, 0x55aau) ||
        !hardware.read(spu2 + 0x1aau, 2u, &actual) ||
        actual != 0x801u ||
        !hardware.read(spu2 + 0x7c2u, 2u, &actual) ||
        actual != 4u ||
        !hardware.read(iop::c_hw::k_i_stat, 4u, &actual) ||
        (actual & (1u << iop::c_spu2::k_irq_line)) == 0u ||
        !hardware.write(iop::c_hw::k_i_stat, 4u,
                        ~(1u << iop::c_spu2::k_irq_line)) ||
        !hardware.write(spu2 + 0x1acu, 2u, 0x55abu) ||
        !hardware.read(iop::c_hw::k_i_stat, 4u, &actual) ||
        (actual & (1u << iop::c_spu2::k_irq_line)) != 0u ||
        !hardware.write(spu2 + 0x19au, 2u, 0u) ||
        !hardware.read(spu2 + 0x7c2u, 2u, &actual) ||
        actual != 0u ||
        hardware.pending_interrupts() != 0u)
    {
        return fail("SPU2 STATX, TSA, explicit data-port IRQ or reset");
    }

    // Key-on before a sample boundary must not decode or raise an IRQ.
    if (!hardware.write(spu2 + 0x4u, 2u, 0x1010u) ||
        !hardware.read(spu2 + 0x4u, 2u, &actual) ||
        actual != 0x1010u ||
        !hardware.write(spu2 + 0x1a0u, 2u, 1u) ||
        !hardware.read(spu2 + 0x0au, 2u, &actual) || actual != 0u ||
        !hardware.read(spu2 + 0x1c8u, 2u, &actual) || actual != 0u ||
        !hardware.write(spu2 + 0x1a4u, 2u, 1u) ||
        !hardware.read(spu2 + 0x0au, 2u, &actual) ||
        actual != 0u)
    {
        return fail("SPU2 voice programming or dynamic read guard");
    }
    hardware.reset();
    if (!hardware.read(spu2 + 0x760u, 2u, &actual) ||
        actual != 0x3fffu ||
        !hardware.read(spu2 + 0x7c6u, 2u, &actual) ||
        actual != 0u ||
        !hardware.read(spu2 + 0x7c2u, 2u, &actual) ||
        actual != 0u ||
        hardware.pending_interrupts() != 0u)
    {
        return fail("SPU2 core/master/SPDIF state survived reset");
    }

    // Official OpenPS2BIOS stores zero to $at=0xBF8010B8 at
    // PC 0x0001CA00: primary DMA3/CD-ROM CHCR through KSEG1.
    // Register programming is not an implicit DMA completion.
    const uint32_t dma3 = iop::c_hw::k_cdrom_dma_base;
    if (!hardware.read(dma3 + 8u, 4u, &actual) || actual != 0u ||
        !hardware.write(dma3, 4u, 0x00001000u) ||
        !hardware.write(dma3 + 4u, 4u, 0x00000020u) ||
        !hardware.write(dma3 + 8u, 4u, 0u) ||
        !hardware.read(dma3 + 8u, 4u, &actual) || actual != 0u ||
        !hardware.write(dma3 + 8u, 4u, 0x41000200u) ||
        !hardware.read(dma3 + 8u, 4u, &actual) ||
        actual != 0x41000200u ||
        hardware.pending_interrupts() != 0u ||
        !hardware.write(dma3 + 8u, 4u, 0u) ||
        !hardware.read(dma3, 4u, &actual) || actual != 0x1000u ||
        !hardware.read(dma3 + 4u, 4u, &actual) || actual != 0x20u ||
        !hardware.read(dma3 + 8u, 4u, &actual) || actual != 0u ||
        hardware.read(dma3 + 12u, 4u, &actual) ||
        hardware.write(dma3 + 8u, 2u, 0u))
    {
        return fail("primary IOP DMA3 CHCR pending start/reset semantics");
    }
    hardware.reset();
    if (!hardware.read(dma3, 4u, &actual) || actual != 0u ||
        !hardware.read(dma3 + 4u, 4u, &actual) || actual != 0u ||
        !hardware.read(dma3 + 8u, 4u, &actual) || actual != 0u)
    {
        return fail("primary DMA3 registers survived reset");
    }

    // Readiness is a controller state, NOT proof of an inserted disc.
    // The idle no-disc state reports N_READY=0x40 and disc type=0.
    if (!hardware.read(iop::c_hw::k_cdvd_ncmd_status, 1u, &actual) ||
        actual != iop::c_hw::k_cdvd_ncmd_ready ||
        hardware.read(iop::c_hw::k_cdvd_ncmd_status, 2u, &actual) ||
        hardware.read(iop::c_hw::k_cdvd_ncmd_status, 4u, &actual) ||
        hardware.write(iop::c_hw::k_cdvd_ncmd_status, 2u, 0u))
    {
        return fail("CDVD N-command idle readiness or unsupported MMIO width");
    }
    // OpenPS2BIOS submits CDVD READ(0x06) for LBA 16, one sector.
    // The physical 0x1F402005 is status on read and FIFO data on write.
    const uint8_t ncmd_params[11] =
    {
        0x10u, 0u, 0u, 0u, 1u, 0u, 0u, 0u, 0x10u, 0x85u, 0u
    };
    for (uint32_t index = 0u; index < 11u; ++index)
    {
        if (!hardware.write(iop::c_hw::k_cdvd_ncmd_status, 1u,
                            ncmd_params[index]))
        {
            return fail("CDVD N-command FIFO rejected firmware parameter");
        }
    }
    uint8_t observed_param = 0u;
    if (hardware.cdvd_ncmd_parameter_count() != 11u ||
        !hardware.cdvd_ncmd_parameter(9u, &observed_param) ||
        observed_param != 0x85u ||
        hardware.cdvd_ncmd_parameter(11u, &observed_param) ||
        hardware.cdvd_ncmd_parameter(0u, nullptr) ||
        !hardware.write(iop::c_hw::k_cdvd_ncmd_command, 1u, 0x06u) ||
        !hardware.cdvd_ncmd_pending() ||
        !hardware.read(iop::c_hw::k_cdvd_ncmd_command, 1u, &actual) ||
        actual != 0x06u ||
        !hardware.read(iop::c_hw::k_cdvd_ncmd_status, 1u, &actual) ||
        actual != iop::c_hw::k_cdvd_ncmd_busy ||
        hardware.write(iop::c_hw::k_cdvd_ncmd_command, 1u, 0x06u) ||
        hardware.write(iop::c_hw::k_cdvd_ncmd_status, 1u, 0u) ||
        hardware.pending_interrupts() != 0u)
    {
        return fail("CDVD READ was completed, duplicated or interrupted");
    }
    hardware.reset();
    if (hardware.cdvd_ncmd_pending() ||
        hardware.cdvd_ncmd_parameter_count() != 0u ||
        !hardware.read(iop::c_hw::k_cdvd_ncmd_status, 1u, &actual) ||
        actual != iop::c_hw::k_cdvd_ncmd_ready)
    {
        return fail("CDVD N-command FIFO and pending state survived reset");
    }
    for (uint32_t index = 0u;
         index < iop::c_hw::k_cdvd_ncmd_param_capacity; ++index)
    {
        if (!hardware.write(iop::c_hw::k_cdvd_ncmd_status, 1u, index))
        {
            return fail("CDVD N-command FIFO rejected a bounded byte");
        }
    }
    if (hardware.write(iop::c_hw::k_cdvd_ncmd_status, 1u, 0xffu))
    {
        return fail("CDVD N-command FIFO overflow was silently accepted");
    }
    hardware.reset();

    // CDVD I_STAT (0x1F402008) is a distinct byte-wide W1C
    // interrupt status register; acknowledging zero pending reasons
    // must not synthesize an interrupt or complete a command.
    if (!hardware.read(iop::c_hw::k_cdvd_i_stat, 1u, &actual) ||
        actual != 0u ||
        !hardware.write(iop::c_hw::k_cdvd_i_stat, 1u, 0xffu) ||
        !hardware.read(iop::c_hw::k_cdvd_i_stat, 1u, &actual) ||
        actual != 0u ||
        hardware.pending_interrupts() != 0u ||
        hardware.read(iop::c_hw::k_cdvd_i_stat, 4u, &actual) ||
        hardware.write(iop::c_hw::k_cdvd_i_stat, 2u, 1u))
    {
        return fail("CDVD interrupt reason W1C or unsupported width");
    }

    // OpenPS2BIOS clears CDVD DEC_SET at 0xBF40203A. Preserve the
    // 8-bit control value without treating the store as an I/O command.
    if (!hardware.read(iop::c_hw::k_cdvd_dec_set, 1u, &actual) ||
        actual != 0u ||
        !hardware.write(iop::c_hw::k_cdvd_dec_set, 1u, 0x15au) ||
        !hardware.read(iop::c_hw::k_cdvd_dec_set, 1u, &actual) ||
        actual != 0x5au ||
        hardware.read(iop::c_hw::k_cdvd_dec_set, 2u, &actual) ||
        hardware.read(iop::c_hw::k_cdvd_dec_set, 4u, &actual) ||
        hardware.write(iop::c_hw::k_cdvd_dec_set, 2u, 0u) ||
        hardware.write(iop::c_hw::k_cdvd_dec_set + 1u, 1u, 0u) ||
        hardware.pending_interrupts() != 0u)
    {
        return fail("CDVD DEC_SET byte register storage/width semantics");
    }
    hardware.reset();
    if (!hardware.read(iop::c_hw::k_cdvd_dec_set, 1u, &actual) ||
        actual != 0u)
    {
        return fail("CDVD DEC_SET retained state after reset");
    }

    // The standalone ROM0 runner has no mounted disc. The CDVD
    // disk-type register must report no media, not an invented game disc.
    if (!hardware.read(iop::c_hw::k_cdvd_disc_type, 1u, &actual) ||
        actual != iop::c_hw::k_cdvd_no_disc ||
        hardware.read(iop::c_hw::k_cdvd_disc_type, 2u, &actual) ||
        hardware.read(iop::c_hw::k_cdvd_disc_type, 4u, &actual) ||
        hardware.write(iop::c_hw::k_cdvd_disc_type, 1u, 0x14u) ||
        hardware.read(iop::c_hw::k_cdvd_disc_type - 1u, 1u, &actual))
    {
        return fail("CDVD no-media type access or unsupported width safety");
    }

    // Firmware writes bytes to the emulator's IOP debug console.
    // Other widths, reads, and nearby unmapped addresses must still fault.
    const uint32_t console = iop::c_hw::k_debug_console;
    if (!hardware.write(console, 1u, 'H') ||
        !hardware.write(console, 1u, 'i') ||
        hardware.debug_console() != "Hi" ||
        hardware.debug_console_truncated() ||
        hardware.read(console, 1u, &actual) ||
        hardware.write(console, 2u, 0x1234u) ||
        hardware.write(console, 4u, 0x12345678u) ||
        hardware.write(console + 1u, 1u, 0u))
    {
        return fail("IOP diagnostic console register write/read/width behavior");
    }
    hardware.reset();
    if (!hardware.debug_console().empty() ||
        hardware.debug_console_truncated())
    {
        return fail("IOP diagnostic console reset should clear output");
    }
    for (size_t index = 0u;
         index <= iop::c_hw::k_debug_console_capacity; ++index)
    {
        if (!hardware.write(console, 1u, 'X'))
        {
            return fail("IOP diagnostic console failed bounded byte write");
        }
    }
    if (hardware.debug_console().size() !=
            iop::c_hw::k_debug_console_capacity ||
        !hardware.debug_console_truncated())
    {
        return fail("IOP diagnostic console exceeded bounded capacity");
    }
    hardware.reset();

    if (!hardware.write(iop::c_hw::k_cache_control, 4u, 0x804u) ||
        !hardware.read(iop::c_hw::k_cache_control, 4u, &actual) ||
        actual != 0x804u ||
        hardware.write(iop::c_hw::k_cache_control, 2u, 0xffffu) ||
        hardware.read(iop::c_hw::k_cache_control + 4u, 4u, &actual))
    {
        return fail("cache-control register access validation");
    }

    // Module startup resets/configures SIO2 before a pad transfer.
    // The primary reference returns zero on CTRL reads, not the last
    // written 0x3bc: original SIO2MAN uses this for both reset and
    // START read-modify-write. Preserve the write in diagnostics only.
    if (!hardware.write(iop::c_hw::k_sio2_ctrl, 4u, 0x3bcu) ||
        !hardware.read(iop::c_hw::k_sio2_ctrl, 4u, &actual) ||
        actual != 0u ||
        hardware.sio2_diagnostics().m_ctrl != 0x3bcu ||
        hardware.pending_interrupts() != 0u ||
        hardware.write(iop::c_hw::k_sio2_ctrl, 2u, 0u) ||
        !hardware.read(iop::c_hw::k_sio2_stat, 4u, &actual) ||
        actual != 0u)
    {
        return fail("SIO2 CTRL reference readback and write validation");
    }

    // SIO2 register family backed by guest-written state only.
    const uint32_t send3 = iop::c_hw::k_sio2_send3_base;
    const uint32_t port = iop::c_hw::k_sio2_port_base;
    uint8_t transmit = 0u;
    if (!hardware.write(send3, 4u, 0xaabbccddu) ||
        !hardware.read(send3, 4u, &actual) ||
        actual != 0xaabbccddu ||
        !hardware.write(send3 + 0x3cu, 4u, 0x12345678u) ||
        !hardware.read(send3 + 0x3cu, 4u, &actual) ||
        actual != 0x12345678u ||
        !hardware.write(port, 4u, 0x123u) ||
        !hardware.write(port + 0x1cu, 4u, 0x456u) ||
        !hardware.read(port + 0x1cu, 4u, &actual) ||
        actual != 0x456u ||
        hardware.read(port + 0x20u, 4u, &actual) ||
        hardware.write(send3, 2u, 1u) ||
        !hardware.write(iop::c_hw::k_sio2_tx_fifo, 1u, 0xabu) ||
        !hardware.write(iop::c_hw::k_sio2_tx_fifo, 4u, 0xcdu) ||
        !hardware.read(iop::c_hw::k_sio2_rx_fifo, 4u, &actual) ||
        actual != 0xffu ||
        !hardware.read(iop::c_hw::k_sio2_rx_fifo, 1u, &actual) ||
        actual != 0xffu ||
        hardware.sio2_pending_tx_bytes() != 2u ||
        !hardware.sio2_tx_byte(1u, &transmit) ||
        transmit != 0xcdu ||
        hardware.sio2_tx_byte(2u, &transmit) ||
        !hardware.write(iop::c_hw::k_sio2_ctrl, 4u, 1u) ||
        !hardware.sio2_transfer_requested() ||
        !hardware.read(iop::c_hw::k_sio2_stat, 4u, &actual) ||
        actual != 0u ||
        !hardware.read(iop::c_hw::k_sio2_port_status, 4u, &actual) ||
        actual != 0x0fu ||
        hardware.pending_interrupts() != 0u)
    {
        return fail("SIO2 configuration/FIFO invented device behavior");
    }
    if (!hardware.write(iop::c_hw::k_sio2_ctrl, 4u, 0x0cu) ||
        hardware.sio2_pending_tx_bytes() != 0u ||
        hardware.sio2_transfer_requested() ||
        !hardware.read(iop::c_hw::k_sio2_aux_status, 4u, &actual) ||
        actual != 0u)
    {
        return fail("SIO2 request reset did not clear queued FIFO");
    }
    hardware.reset();
    if (!hardware.read(send3, 4u, &actual) || actual != 0u ||
        !hardware.read(port, 4u, &actual) || actual != 0u ||
        hardware.sio2_pending_tx_bytes() != 0u ||
        hardware.sio2_transfer_requested())
    {
        return fail("SIO2 state leaked across IOP reset");
    }

    // The ROM0 entrypoint performs actual R3000 load/store operations
    // against scratchpad, cache control and IOP interrupt mask.
    // These are original synthetic instructions, not copied BIOS data.
    std::vector<uint8_t> rom(iop::c_cpu::k_rom0_size, 0u);
    const uint32_t instructions[] =
    {
        0x3c081f80u, // lui t0,0x1f80
        0x24091234u, // addiu t1,zero,0x1234
        0xad090000u, // sw t1,0(t0) (scratchpad)
        0x8d0a0000u, // lw t2,0(t0)
        0x3c0bfffeu, // lui t3,0xfffe
        0x356b0130u, // ori t3,t3,0x0130
        0x240c0804u, // addiu t4,zero,0x804
        0xad6c0000u, // sw t4,0(t3) (cache control)
        0x8d6d0000u, // lw t5,0(t3)
        0x3c0e1f80u, // lui t6,0x1f80
        0x35ce1074u, // ori t6,t6,0x1074
        0x240f0008u, // addiu t7,zero,8
        0xadcf0000u, // sw t7,0(t6) (I_MASK)
        0x8dd80000u, // lw t8,0(t6)
        0x0000000du  // break
    };
    for (uint32_t index = 0u; index < 15u; ++index)
    {
        put32(rom, index * 4u, instructions[index]);
    }

    iop::c_kernel kernel{};
    if (!kernel.start_rom0_boot(rom))
    {
        return fail("IOP ROM0 kernel startup rejected valid image");
    }

    const iop::s_kernel_run_result executed = kernel.run(32u, 4u);
    if (executed.m_stop != iop::e_kernel_stop::breakpoint ||
        executed.m_guest_pc != 0xbfc00038u ||
        executed.m_executed != 14u ||
        kernel.cpu().state().m_gpr[10u] != 0x1234u ||
        kernel.cpu().state().m_gpr[13u] != 0x804u ||
        kernel.cpu().state().m_gpr[24u] != 8u ||
        kernel.hardware().cache_control() != 0x804u ||
        !kernel.hardware().read(iop::c_hw::k_i_mask, 4u, &actual) ||
        actual != 8u ||
        !kernel.cpu().read32(0x1f800000u, &actual) ||
        actual != 0x1234u ||
        !kernel.cpu().read32(0xbf800000u, &actual) ||
        actual != 0x1234u)
    {
        return fail("guest instruction stream did not access mapped hardware");
    }

    // Execute original ROM0-style instructions against the KSEG1 DMA3
    // CHCR alias. This test never starts or completes a transfer.
    const uint32_t dma3_instructions[] =
    {
        0x3c01bf80u, // lui at,0xbf80
        0x342110b8u, // ori at,at,0x10b8
        0xac200000u, // sw zero,0(at), observed ROM0 opcode
        0x8c290000u, // lw t1,0(at)
        0x0000000du  // break
    };
    for (uint32_t index = 0u; index < 5u; ++index)
    {
        put32(rom, index * 4u, dma3_instructions[index]);
    }
    iop::c_kernel dma3_kernel{};
    if (!dma3_kernel.start_rom0_boot(rom))
    {
        return fail("DMA3 KSEG1 fixture failed to install guest ROM");
    }
    const iop::s_kernel_run_result dma3_result =
        dma3_kernel.run(16u, 4u);
    if (dma3_result.m_stop != iop::e_kernel_stop::breakpoint ||
        dma3_result.m_executed != 4u ||
        dma3_kernel.cpu().state().m_gpr[9u] != 0u ||
        dma3_kernel.cpu().last_memory_fault().m_valid ||
        dma3_kernel.hardware().pending_interrupts() != 0u)
    {
        return fail("guest DMA3 CHCR reset faulted or created phantom IRQ");
    }

    // Actual ROM0-style CDVD DMA3 CHCR start (0x41000200). This is
    // a *pending* request: STR remains set until a real transport
    // completes the work. Neither data nor an IRQ may be fabricated.
    const uint32_t dma3_start_instructions[] =
    {
        0x3c08bf80u, // lui t0,0xbf80
        0x350810b8u, // ori t0,t0,0x10b8
        0x3c094100u, // lui t1,0x4100
        0x35290200u, // ori t1,t1,0x0200
        0xad090000u, // sw t1,0(t0)
        0x8d0a0000u, // lw t2,0(t0)
        0xad000000u, // sw zero,0(t0), guest stop
        0x8d0b0000u, // lw t3,0(t0)
        0x0000000du  // break
    };
    for (uint32_t index = 0u; index < 9u; ++index)
    {
        put32(rom, index * 4u, dma3_start_instructions[index]);
    }
    iop::c_kernel dma3_start_kernel{};
    if (!dma3_start_kernel.start_rom0_boot(rom))
    {
        return fail("DMA3 start KSEG1 fixture failed to install guest ROM");
    }
    const iop::s_kernel_run_result dma3_start =
        dma3_start_kernel.run(16u, 4u);
    if (dma3_start.m_stop != iop::e_kernel_stop::breakpoint ||
        dma3_start.m_executed != 8u ||
        dma3_start_kernel.cpu().state().m_gpr[10u] != 0x41000200u ||
        dma3_start_kernel.cpu().state().m_gpr[11u] != 0u ||
        dma3_start_kernel.cpu().last_memory_fault().m_valid ||
        dma3_start_kernel.hardware().pending_interrupts() != 0u)
    {
        return fail("guest DMA3 CHCR start/stop fabricated DMA or IRQ");
    }

    // Actual guest SB from KSEG1 into the diagnostic console, without
    // allowing reads or changing memory state to fake hardware progress.
    const uint32_t debug_instructions[] =
    {
        0x3c08bf80u, // lui t0,0xbf80
        0x35082080u, // ori t0,t0,0x2080
        0x24090041u, // addiu t1,zero,'A'
        0xa1090000u, // sb t1,0(t0)
        0x0000000du  // break
    };
    for (uint32_t index = 0u; index < 5u; ++index)
    {
        put32(rom, index * 4u, debug_instructions[index]);
    }
    iop::c_kernel debug_kernel{};
    if (!debug_kernel.start_rom0_boot(rom))
    {
        return fail("IOP ROM0 console test failed to install ROM");
    }
    const iop::s_kernel_run_result logged = debug_kernel.run(16u, 4u);
    if (logged.m_stop != iop::e_kernel_stop::breakpoint ||
        logged.m_executed != 4u ||
        debug_kernel.hardware().debug_console() != "A")
    {
        return fail("Guest SB to IOP diagnostic console was not captured");
    }

    // Guest LBU from CDVD N-command readiness and disc-type via KSEG1.
    // The first LBU matches the real faulting ROM0 access at 0x0001CA14.
    const uint32_t cdvd_instructions[] =
    {
        0x3c08bf40u, // lui t0,0xbf40
        0x35082005u, // ori t0,t0,0x2005
        0x91090000u, // lbu t1,0(t0): ready=0x40
        0x3508200fu, // ori t0,t0,0x200f
        0x910a0000u, // lbu t2,0(t0): disc type=0x00
        0x0000000du  // break
    };
    for (uint32_t index = 0u; index < 6u; ++index)
    {
        put32(rom, index * 4u, cdvd_instructions[index]);
    }
    iop::c_kernel cdvd_kernel{};
    if (!cdvd_kernel.start_rom0_boot(rom))
    {
        return fail("IOP ROM0 CDVD test failed to install ROM");
    }
    const iop::s_kernel_run_result cdvd = cdvd_kernel.run(16u, 4u);
    if (cdvd.m_stop != iop::e_kernel_stop::breakpoint ||
        cdvd.m_executed != 5u ||
        cdvd_kernel.cpu().state().m_gpr[9u] !=
            iop::c_hw::k_cdvd_ncmd_ready ||
        cdvd_kernel.cpu().state().m_gpr[10u] !=
            iop::c_hw::k_cdvd_no_disc)
    {
        return fail("Guest LBU did not read CDVD no-media state");
    }

    // Real ROM0 writes SB to CDVD interrupt-status 0xBF402008.
    // Test that the guest acknowledges outstanding reasons without
    // adding an IRQ, claiming a completed command or inventing media.
    const uint32_t cdvd_ack_instructions[] =
    {
        0x3c08bf40u, // lui t0,0xbf40
        0x35082008u, // ori t0,t0,0x2008
        0x240900ffu, // addiu t1,zero,0xff
        0xa1090000u, // sb t1,0(t0)
        0x910a0000u, // lbu t2,0(t0)
        0x0000000du  // break
    };
    for (uint32_t index = 0u; index < 6u; ++index)
    {
        put32(rom, index * 4u, cdvd_ack_instructions[index]);
    }
    iop::c_kernel cdvd_ack_kernel{};
    if (!cdvd_ack_kernel.start_rom0_boot(rom))
    {
        return fail("IOP ROM0 CDVD ack fixture failed to install ROM");
    }
    const iop::s_kernel_run_result ack =
        cdvd_ack_kernel.run(16u, 4u);
    if (ack.m_stop != iop::e_kernel_stop::breakpoint ||
        ack.m_executed != 5u ||
        cdvd_ack_kernel.cpu().state().m_gpr[10u] != 0u ||
        cdvd_ack_kernel.cpu().last_memory_fault().m_valid ||
        cdvd_ack_kernel.hardware().pending_interrupts() != 0u)
    {
        return fail("guest CDVD interrupt-ack created phantom IRQ/fault");
    }

    // Execute genuine R3000 SB/LBU instructions against CDVD DEC_SET
    // through the KSEG1 MMIO mirror. This tests register programming
    // only, not decryption or successful disc I/O.
    const uint32_t cdvd_dec_set_instructions[] =
    {
        0x3c08bf40u, // lui t0,0xbf40
        0x3508203au, // ori t0,t0,0x203a
        0x2409005au, // addiu t1,zero,0x5a
        0xa1090000u, // sb t1,0(t0)
        0x910a0000u, // lbu t2,0(t0)
        0xa1000000u, // sb zero,0(t0) (ROM0 reset store)
        0x910b0000u, // lbu t3,0(t0)
        0x0000000du  // break
    };
    for (uint32_t index = 0u; index < 8u; ++index)
    {
        put32(rom, index * 4u, cdvd_dec_set_instructions[index]);
    }
    iop::c_kernel cdvd_dec_set_kernel{};
    if (!cdvd_dec_set_kernel.start_rom0_boot(rom))
    {
        return fail("IOP ROM0 CDVD DEC_SET fixture failed to install ROM");
    }
    const iop::s_kernel_run_result dec_set =
        cdvd_dec_set_kernel.run(16u, 4u);
    if (dec_set.m_stop != iop::e_kernel_stop::breakpoint ||
        dec_set.m_executed != 7u ||
        cdvd_dec_set_kernel.cpu().state().m_gpr[10u] != 0x5au ||
        cdvd_dec_set_kernel.cpu().state().m_gpr[11u] != 0u ||
        cdvd_dec_set_kernel.cpu().last_memory_fault().m_valid ||
        cdvd_dec_set_kernel.hardware().pending_interrupts() != 0u)
    {
        return fail("Guest CDVD DEC_SET access faulted or fabricated I/O");
    }

    // Real CDVD parameters use SB at the same address where LBU
    // reports readiness. The next SB to 0xBF402004 submits READ(0x06).
    const uint32_t ncmd_instructions[] =
    {
        0x3c08bf40u, // lui t0,0xbf40
        0x35082005u, // ori t0,t0,0x2005 (N_DATAIN/N_READY)
        0x24090010u, // addiu t1,zero,0x10 (LBA low byte)
        0xa1090000u, // sb t1,0(t0) (N_DATAIN)
        0x910a0000u, // lbu t2,0(t0): ready before command
        0x2508ffffu, // addiu t0,t0,-1 (N_CMD)
        0x24090006u, // addiu t1,zero,0x06 (READ)
        0xa1090000u, // sb t1,0(t0)
        0x910b0000u, // lbu t3,0(t0): command
        0x25080001u, // addiu t0,t0,1 (N_READY)
        0x910c0000u, // lbu t4,0(t0): still busy
        0x0000000du  // break
    };
    for (uint32_t index = 0u; index < 12u; ++index)
    {
        put32(rom, index * 4u, ncmd_instructions[index]);
    }
    iop::c_kernel ncmd_kernel{};
    if (!ncmd_kernel.start_rom0_boot(rom))
    {
        return fail("CDVD N-command KSEG1 fixture failed to install guest ROM");
    }
    const iop::s_kernel_run_result ncmd = ncmd_kernel.run(20u, 4u);
    if (ncmd.m_stop != iop::e_kernel_stop::breakpoint ||
        ncmd.m_executed != 11u ||
        ncmd_kernel.cpu().state().m_gpr[10u] !=
            iop::c_hw::k_cdvd_ncmd_ready ||
        ncmd_kernel.cpu().state().m_gpr[11u] != 0x06u ||
        ncmd_kernel.cpu().state().m_gpr[12u] !=
            iop::c_hw::k_cdvd_ncmd_busy ||
        !ncmd_kernel.hardware().cdvd_ncmd_pending() ||
        ncmd_kernel.hardware().cdvd_ncmd_parameter_count() != 1u ||
        ncmd_kernel.cpu().last_memory_fault().m_valid ||
        ncmd_kernel.hardware().pending_interrupts() != 0u)
    {
        return fail("guest CDVD N-command/parameter created phantom completion");
    }

    // HLE IRX guest MMIO uses the physical bus, independent of ROM0.
    iop::c_kernel hle_kernel{};
    const uint32_t program = 0x00018000u;
    const uint32_t code[] =
    {
        0x3c08bf80u, // lui t0,0xbf80
        0x35088268u, // ori t0,t0,0x8268 (SIO2 CTRL)
        0x24090034u, // addiu t1,zero,0x34
        0xad090000u, // sw t1,0(t0)
        0x8d0a0000u, // lw t2,0(t0)
        0x0000000du  // break
    };
    for (uint32_t index = 0u; index < 6u; ++index)
    {
        if (!hle_kernel.cpu().write32(program + index * 4u, code[index]))
        {
            return fail("could not prepare HLE guest SIO2 MMIO fixture");
        }
    }
    if (!hle_kernel.attach_hle_hardware() ||
        hle_kernel.create_thread(program, 0u, 10u) != 1u ||
        !hle_kernel.start_thread(1u))
    {
        return fail("could not schedule HLE guest SIO2 MMIO fixture");
    }
    const iop::s_kernel_run_result hle_run = hle_kernel.run(32u, 8u);
    // The authentic R3000 KSEG1 LW sees the reference-compatible
    // zero CTRL readback; the guest SW remains in host diagnostics.
    if (hle_run.m_stop != iop::e_kernel_stop::breakpoint ||
        hle_run.m_executed != 5u ||
        hle_kernel.cpu().state().m_gpr[10] != 0u ||
        hle_kernel.hardware().sio2_diagnostics().m_ctrl != 0x34u ||
        hle_kernel.cpu().last_memory_fault().m_valid ||
        hle_kernel.hardware().pending_interrupts() != 0u)
    {
        return fail("HLE R3000 MMIO access failed or invented IRQ");
    }
    hle_kernel.reset();
    if (hle_kernel.cpu().write32(iop::c_hw::k_sio2_ctrl, 0u) ||
        !hle_kernel.attach_hle_hardware() ||
        !hle_kernel.cpu().read32(iop::c_hw::k_sio2_ctrl, &actual) ||
        actual != 0u ||
        !hle_kernel.start_rom0_boot(rom) ||
        hle_kernel.attach_hle_hardware())
    {
        return fail("HLE reboot attachment or ROM0 isolation failed");
    }

    // The cache-control register is address-specific. The IOP bus
    // must not turn all of KSEG2/KSEG3 into a permissive RAM alias.
    if (kernel.cpu().write32(0xfffe0134u, 0u) ||
        kernel.cpu().read32(0xfffe0134u, &actual) ||
        kernel.cpu().read32(0x1f808000u, &actual))
    {
        return fail("unimplemented high control/MMIO address did not fault");
    }

    if (!kernel.hardware().raise_interrupt(3u) ||
        kernel.hardware().pending_interrupts() != 8u)
    {
        return fail("ROM I_MASK did not gate host-raised IRQ state");
    }
    kernel.reset();
    if (kernel.hardware().pending_interrupts() != 0u ||
        !kernel.hardware().read(iop::c_hw::k_i_ctrl, 4u, &actual) ||
        actual != 1u ||
        !kernel.hardware().read(iop::c_hw::k_i_mask, 4u, &actual) ||
        actual != 0u ||
        kernel.cpu().write32(0x1f800000u, 123u))
    {
        return fail("IOP reset retained stale MMIO or stale bus attachment");
    }

    std::printf("IOP ROM0 hardware: PASS\n");
    return 0;
}
