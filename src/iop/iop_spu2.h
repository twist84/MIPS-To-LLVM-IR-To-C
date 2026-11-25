#pragma once

#include "iop_audio_profile.h"

#include <cstdint>
#include <vector>

namespace iop
{
// Guest-visible 16-bit SPU2 register bank from ps2-web-recomp/SDBZ.
// Register/transfer state, guest-cycle sample clock and ADPCM voice cursors.
// Stereo PCM mixer and guest-visible mixer rings; reverb remains unimplemented.
class c_spu2
{
public:
    static constexpr uint32_t k_base = 0x1f900000u;
    static constexpr uint32_t k_size = 0x800u;
    static constexpr uint32_t k_irq_line = 9u;
    static constexpr uint32_t k_ram_words = 1u << 20u;
    // Nominal PS2 IOP clock 36.864 MHz / 48 kHz (reference SPU2).
    static constexpr uint32_t k_cycles_per_sample = 768u;

    c_spu2();
    void reset();
    void enable_audio_profile(bool enabled) { m_audio_profile_enabled = enabled; }
    void report_audio_profile() const;

    // Start on the first valid-width SPU2 access, like the reference.
    // Count elapsed guest cycles, not generated audio frames or host time.
    // Returns newly latched playback IRQ core bits (zero if none).
    uint32_t advance_cycles(uint32_t cycles);
    // Earliest possible voice/input fetch or mixer-ring IRQ edge.
    // Zero means no pending playback IRQ opportunity.
    uint32_t voice_irq_cycles_remaining() const;
    uint64_t playback_irq_events() const { return m_playback_irq_events; }
    // Decode one PS2 16-byte ADPCM block (8 little-endian halfwords).
    // History is shared between consecutive blocks of a voice.
    static void decode_adpcm_block(const uint16_t* words, int16_t* samples,
                                   int32_t& history1, int32_t& history2);
    uint64_t decoded_blocks() const { return m_decoded_blocks; }
    uint64_t voice_starts() const { return m_voice_starts; }
    uint64_t voice_end_events() const { return m_voice_end_events; }
    uint32_t active_voice_mask(uint32_t core) const
    {
        return core < 2u ? m_cores[core].m_key_mask : 0u;
    }
    bool clock_active() const { return m_clock_active; }
    uint64_t clock_cycles() const { return m_clock_cycles; }
    uint64_t elapsed_samples() const { return m_clock_cycles / k_cycles_per_sample; }
    uint32_t sample_phase() const
    {
        return static_cast<uint32_t>(m_clock_cycles % k_cycles_per_sample);
    }

    // Physical IOP bus addresses. 16- and 32-bit transfers only.
    bool read(uint32_t address, uint32_t width, uint32_t* value);
    bool write(uint32_t address, uint32_t width, uint32_t value);
    bool irq_pending() const;
    // One bit for each newly latched SPU2 core interrupt.
    uint32_t irq_latches() const;
    // SPU2 uses shared 2 MiB sound RAM for both core DMA channels.
    // An ordinary DMA copies real little-endian halfwords starting at
    // the core's guest-programmed TSA, wrapping in the sound RAM.
    // AutoDMA uses separate planar input rings, not TSA.
    bool auto_dma_enabled(uint32_t core) const;
    static constexpr uint32_t k_input_frames = 512u;
    static constexpr uint32_t k_input_chunk_bytes = 1024u;
    static constexpr uint32_t k_input_batch = 32u;
    static constexpr uint32_t k_sample_rate = 48000u;
    // The sink is an optional nonblocking host consumer. It must copy
    // the interleaved samples before returning. No guest state depends
    // on its presence or on a host audio device.
    using t_pcm_sink = void (*)(void* user, const int16_t* pcm, uint32_t frames);
    void bind_pcm_sink(void* user, t_pcm_sink sink)
    {
        m_pcm_user = user;
        m_pcm_sink = sink;
        m_pcm_pending = 0u;
    }
    uint64_t mixed_frames() const { return m_mixed_frames; }
    uint64_t nonzero_frames() const { return m_nonzero_frames; }
    uint32_t pcm_peak() const { return m_pcm_peak; }
    uint64_t clipped_samples() const { return m_clipped_samples; }
    uint64_t effect_input_frames(uint32_t core) const
    {
        return core < 2u ? m_effect_input_frames[core] : 0u;
    }
    // Effect addresses are logical halfword addresses; map to shared
    // physical sound RAM only after wrapping within the work area.
    uint32_t effect_start(uint32_t core) const;
    uint32_t effect_end(uint32_t core) const;
    bool effect_enabled(uint32_t core) const;
    uint64_t effect_enable_writes(uint32_t core) const;
    bool effect_address(uint32_t core, uint64_t step, int64_t offset,
        uint32_t* address) const;
    // Effect taps are real shared RAM accesses. Reads remain available
    // while writeback and effect IRQ comparisons are disabled.
    // Call only for an actual filter transaction at its guest sample tick.
    bool read_effect_sample(uint32_t core, uint64_t step, int64_t offset,
        int16_t* sample);
    bool write_effect_sample(uint32_t core, uint64_t step, int64_t offset,
        int32_t sample);
    uint64_t effect_irq_events() const { return m_effect_irq_events; }
    uint64_t mixer_ring_batches() const { return m_mixer_ring_batches; }
    uint64_t mixer_irq_events() const { return m_mixer_irq_events; }
    uint32_t input_frames(uint32_t core) const;
    uint32_t input_refill_cycles(uint32_t core) const;
    bool push_input(uint32_t core, const uint8_t* data, uint32_t bytes);
    uint64_t input_frames_written() const { return m_input_frames_written; }
    uint64_t input_frames_consumed() const { return m_input_frames_consumed; }
    bool copy_dma(uint32_t core, std::vector<uint8_t>& data, bool to_spu);
    bool ram_word(uint32_t address, uint16_t* value) const;

private:
    enum class e_voice_phase : uint8_t
    {
        off,
        attack,
        decay,
        sustain,
        release
    };

    struct s_envelope
    {
        uint32_t m_counter = 0u;
        int32_t m_level = 0;
        void step(uint32_t rate, bool decrease, bool exponential);
    };

    struct s_voice
    {
        uint32_t m_block[2]{};
        uint8_t m_flags[2]{};
        uint32_t m_next = 0u;
        uint32_t m_loop = 0u;
        uint32_t m_position = 0u; // 12 fractional bits within current block
        int32_t m_history1 = 0;
        int32_t m_history2 = 0;
        int16_t m_samples[2][28]{};
        bool m_loop_written = false;
        bool m_pending = false;
        bool m_started = false;
        e_voice_phase m_phase = e_voice_phase::off;
        s_envelope m_envelope{};
        s_envelope m_volume[2]{};
    };

    struct s_core
    {
        // 0x000..0x3FF for each core, including voice parameter latches.
        uint16_t m_registers[0x200]{};
        // Reference master-volume window at 0x760 and 0x788.
        uint16_t m_master[8]{};
        // IIR, COMB1..4, WALL, APF1/2, input L/R coefficients.
        uint16_t m_effect_coefficients[10]{};
        uint64_t m_effect_enable_writes = 0u;
        uint32_t m_irqa = 0x800u;
        uint32_t m_tsa = 0u;
        uint32_t m_endx = 0u;
        uint32_t m_key_mask = 0u;
        s_voice m_voices[24]{};
        uint32_t m_input_read = 0u;
        uint32_t m_input_count = 0u;
        int16_t m_input[k_input_frames][2]{};
        uint32_t m_statx_settling = 0u;
        uint16_t m_attr = 0u;
        bool m_irq = false;
    };

    static uint32_t voice_address(const s_core& core, uint32_t voice, uint32_t field);
    void decode_voice_block(s_voice& voice, uint32_t slot);
    void check_voice_irq(uint32_t address);
    void check_input_irq(uint32_t address, uint32_t words);
    void check_effect_irq(uint32_t address);
    void advance_input(uint32_t samples, int32_t (&sum)[2][k_input_batch][2]);
    void mix_frames(uint64_t samples);
    void write_mixer_rings();
    void count_mixer_irq(uint32_t address, uint32_t words);
    void emit_pcm(int16_t left, int16_t right);
    void start_voice(s_core& core, uint32_t index);
    static void update_envelope(s_voice& voice, uint16_t adsr1, uint16_t adsr2);
    void advance_voices(uint32_t samples, int32_t (&dry)[2][k_input_batch][2],
        int32_t (&wet)[2][k_input_batch][2]);
    static uint32_t set_hi(uint32_t address, uint32_t value);
    static uint32_t set_lo(uint32_t address, uint32_t value);

    bool read16(uint32_t offset, uint16_t* value);
    bool write16(uint32_t offset, uint16_t value);
    bool read_core(uint32_t index, uint32_t offset, uint16_t* value);
    bool write_core(uint32_t index, uint32_t offset, uint16_t value);

    // Host diagnostics accumulate across guest resets; no guest-visible state.
    bool m_audio_profile_enabled = false;
    s_audio_profile_counter m_profile_mix{};
    s_audio_profile_counter m_profile_voices{};
    s_audio_profile_counter m_profile_adpcm{};
    s_audio_profile_counter m_profile_input_mix{};
    s_audio_profile_counter m_profile_input_dma{};
    s_audio_profile_counter m_profile_rings{};
    s_audio_profile_counter m_profile_sink{};
    s_audio_profile_counter m_profile_silent_rings{};

    bool m_clock_active = false;
    uint64_t m_clock_cycles = 0u;
    uint64_t m_decoded_blocks = 0u;
    uint64_t m_voice_starts = 0u;
    uint64_t m_voice_end_events = 0u;
    uint64_t m_playback_irq_events = 0u;
    uint64_t m_input_frames_written = 0u;
    uint64_t m_input_frames_consumed = 0u;
    uint64_t m_mixed_frames = 0u;
    uint64_t m_nonzero_frames = 0u;
    uint32_t m_pcm_peak = 0u;
    uint64_t m_clipped_samples = 0u;
    uint64_t m_effect_input_frames[2]{};
    uint64_t m_mixer_ring_batches = 0u;
    uint64_t m_mixer_irq_events = 0u;
    uint64_t m_effect_irq_events = 0u;
    // Per-core input L/R, voice 1/3, dry L/R and wet L/R.
    int16_t m_mixer_rings[2][8][k_input_batch]{};
    void* m_pcm_user = nullptr;
    t_pcm_sink m_pcm_sink = nullptr;
    int16_t m_pcm_batch[k_input_batch * 2u]{};
    uint32_t m_pcm_pending = 0u;
    s_core m_cores[2]{};
    // 2 MiB of SPU2 sound RAM, allocated on the heap (not the IOP stack).
    std::vector<uint16_t> m_ram;
    uint16_t m_spdif_out = 0u;
    uint16_t m_spdif_mode = 0u;
    uint16_t m_spdif_media = 0u;
};
} // namespace iop
