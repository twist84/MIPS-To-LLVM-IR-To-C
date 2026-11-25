#include "iop_spu2.h"

#include <algorithm>
#include <cstdio>

namespace iop
{
c_spu2::c_spu2()
    : m_ram(k_ram_words, 0u)
{
    reset();
}

void c_spu2::report_audio_profile() const
{
    if (m_audio_profile_enabled)
    {
        m_profile_mix.report("spu2_mix_frames");
        m_profile_voices.report("spu2_advance_voices");
        m_profile_adpcm.report("spu2_decode_voice_block");
        m_profile_input_mix.report("spu2_advance_input");
        m_profile_input_dma.report("spu2_push_input");
        m_profile_rings.report("spu2_write_mixer_rings");
        m_profile_effect.report("spu2_effect");
        m_profile_sink.report("spu2_pcm_sink");
        m_profile_silent_rings.report("spu2_silent_ring_revolutions");
    }
}

void c_spu2::report_signal_path() const
{
    for (uint32_t core = 0u; core < 2u; ++core)
    {
        const s_core& state = m_cores[core];
        std::printf("IOP SPU2 signal: core=%u source_peak=%u envelope_peak=%u "
            "voice_volume_peak=%u input_raw_peak=%u input_gain_peak=%u "
            "external_peak=%u dry_peak=%u output_peak=%u attr14_frames=%llu\n",
            core, state.m_source_peak, state.m_envelope_peak, state.m_voice_volume_peak,
            state.m_input_raw_peak, state.m_input_gain_peak, state.m_external_peak,
            state.m_dry_peak, state.m_output_peak,
            static_cast<unsigned long long>(state.m_attr14_frames));
        std::printf("IOP SPU2 registers: core=%u attr=0x%04x mmix=0x%04x "
            "mvol=0x%04x/0x%04x mvolx=0x%04x/0x%04x "
            "evol=0x%04x/0x%04x avol=0x%04x/0x%04x bvol=0x%04x/0x%04x "
            "vmixl=0x%06x vmixr=0x%06x vmixel=0x%06x vmixer=0x%06x\n",
            core, static_cast<unsigned>(state.m_attr),
            static_cast<unsigned>(state.m_registers[0x198u / 2u]),
            static_cast<unsigned>(state.m_master[0]), static_cast<unsigned>(state.m_master[1]),
            static_cast<unsigned>(static_cast<uint16_t>(state.m_master_volume[0].m_level)),
            static_cast<unsigned>(static_cast<uint16_t>(state.m_master_volume[1].m_level)),
            static_cast<unsigned>(state.m_master[2]), static_cast<unsigned>(state.m_master[3]),
            static_cast<unsigned>(state.m_master[4]), static_cast<unsigned>(state.m_master[5]),
            static_cast<unsigned>(state.m_master[6]), static_cast<unsigned>(state.m_master[7]),
            static_cast<uint32_t>(state.m_registers[0x188u / 2u]) |
                (static_cast<uint32_t>(state.m_registers[0x18au / 2u] & 0xffu) << 16u),
            static_cast<uint32_t>(state.m_registers[0x190u / 2u]) |
                (static_cast<uint32_t>(state.m_registers[0x192u / 2u] & 0xffu) << 16u),
            static_cast<uint32_t>(state.m_registers[0x18cu / 2u]) |
                (static_cast<uint32_t>(state.m_registers[0x18eu / 2u] & 0xffu) << 16u),
            static_cast<uint32_t>(state.m_registers[0x194u / 2u]) |
                (static_cast<uint32_t>(state.m_registers[0x196u / 2u] & 0xffu) << 16u));
    }
}

void c_spu2::reset()
{
    m_clock_active = false;
    m_clock_cycles = 0u;
    m_decoded_blocks = 0u;
    m_voice_starts = 0u;
    m_voice_end_events = 0u;
    m_playback_irq_events = 0u;
    m_input_frames_written = 0u;
    m_input_frames_consumed = 0u;
    m_mixed_frames = 0u;
    m_nonzero_frames = 0u;
    m_pcm_peak = 0u;
    m_clipped_samples = 0u;
    m_effect_input_frames[0] = m_effect_input_frames[1] = 0u;
    m_mixer_ring_batches = 0u;
    m_mixer_irq_events = 0u;
    m_effect_irq_events = 0u;
    m_pcm_pending = 0u;
    for (s_core& core : m_cores)
    {
        core = s_core{};
        // Initial voice routing follows the SPU2 compatibility defaults:
        // all 24 voices routed to both dry and effect paths.
        for (uint32_t offset = 0x188u; offset <= 0x194u; offset += 4u)
        {
            core.m_registers[offset / 2u] = 0xffffu;
            core.m_registers[offset / 2u + 1u] = 0x00ffu;
        }
        core.m_master[0] = 0x3fffu;
        core.m_master[1] = 0x3fffu;
        core.m_master_volume[0].m_level = 0x7ffe;
        core.m_master_volume[1].m_level = 0x7ffe;
        core.m_master[4] = 0x7fffu;
        core.m_master[5] = 0x7fffu;
        core.m_master[6] = 0x7fffu;
        core.m_master[7] = 0x7fffu;
    }
    m_cores[0].m_registers[0x198u / 2u] = 0x0ff0u;
    m_cores[1].m_registers[0x198u / 2u] = 0x0ffcu;
    // Compatibility reset work areas (word addresses).
    for (uint32_t core = 0u; core < 2u; ++core)
    {
        const uint16_t page = static_cast<uint16_t>(0x000eu + core);
        m_cores[core].m_registers[0x2e0u / 2u] = page;
        m_cores[core].m_registers[0x2e2u / 2u] = 0xfff8u;
        m_cores[core].m_registers[0x33cu / 2u] = page;
        m_cores[core].m_registers[0x33eu / 2u] = 0xffffu;
    }
    std::fill(m_ram.begin(), m_ram.end(), 0u);
    m_spdif_out = 0u;
    m_spdif_mode = 0u;
    m_spdif_media = 0u;
}

namespace
{
// Explicit arithmetic shift also works on hosts with older signed-shift
// language rules. All decoder intermediates fit in int32_t.
int32_t floor_shift(int32_t value, uint32_t shift)
{
    return value >= 0 ? value >> shift : -1 - ((-1 - value) >> shift);
}

int32_t signed_halfword(uint32_t value)
{
    value &= 0xffffu;
    return static_cast<int32_t>(value) - (value >= 0x8000u ? 0x10000 : 0);
}

void record_peak(uint32_t& peak, int32_t sample)
{
    // Inputs are bounded mixer intermediates, never INT32_MIN.
    peak = std::max(peak, static_cast<uint32_t>(sample < 0 ? -sample : sample));
}

int16_t clamp_sample(int32_t value)
{
    return static_cast<int16_t>(std::clamp(value, -32768, 32767));
}

// Documented SPU half-band 39-tap FIR, represented by its symmetric
// nonzero even taps and center. The upsampler doubles the gain.
int16_t resample_effect(const int16_t (&history)[64], uint32_t position, bool up)
{
    constexpr int32_t side[10] = {-1, 2, -10, 35, -103, 266, -616, 1332, -2960, 10246};
    const uint32_t first = (position + 64u - 39u) & 63u;
    int64_t sum = static_cast<int64_t>(history[(first + 19u) & 63u]) *
        (up ? 32767 : 16384);
    for (uint32_t tap = 0u; tap < 10u; ++tap)
    {
        const int32_t pair = static_cast<int32_t>(history[(first + tap * 2u) & 63u]) +
            history[(first + 38u - tap * 2u) & 63u];
        sum += static_cast<int64_t>(pair) * side[tap] * (up ? 2 : 1);
    }
    const int32_t rounded = static_cast<int32_t>(sum >= 0 ? sum >> 15u :
        -1 - ((-1 - sum) >> 15u));
    return clamp_sample(rounded);
}

int32_t scale_mix(int32_t value, int32_t gain)
{
    const int64_t product = static_cast<int64_t>(value) * gain;
    return static_cast<int32_t>(product >= 0 ? product >> 15u :
        -1 - ((-1 - product) >> 15u));
}
}

uint32_t c_spu2::effect_start(uint32_t core) const
{
    uint32_t result = 0u;
    if (core < 2u)
    {
        const uint16_t* regs = m_cores[core].m_registers;
        result = (static_cast<uint32_t>(regs[0x2e0u / 2u] & 0x3fu) << 16u) |
            regs[0x2e2u / 2u];
    }
    return result;
}

uint32_t c_spu2::effect_end(uint32_t core) const
{
    return core < 2u ?
        (static_cast<uint32_t>(m_cores[core].m_registers[0x33cu / 2u] & 0x3fu) << 16u) |
        0xffffu : 0u;
}

bool c_spu2::effect_enabled(uint32_t core) const
{
    return core < 2u && (m_cores[core].m_attr & 0x80u) != 0u;
}

uint64_t c_spu2::effect_enable_writes(uint32_t core) const
{
    return core < 2u ? m_cores[core].m_effect_enable_writes : 0u;
}

bool c_spu2::effect_address(uint32_t core, uint64_t step, int64_t offset,
    uint32_t* address) const
{
    bool result = false;
    const uint32_t start = effect_start(core);
    const uint32_t end = effect_end(core);
    if (core < 2u && address != nullptr && start < end)
    {
        const uint32_t length = end - start + 1u;
        // Reduce both terms separately, so negative taps and very large
        // guest-clock positions cannot overflow signed arithmetic.
        int64_t relative = static_cast<int64_t>(step % length) +
            offset % static_cast<int64_t>(length);
        relative %= static_cast<int64_t>(length);
        if (relative < 0)
        {
            relative += length;
        }
        *address = (start + static_cast<uint32_t>(relative)) & (k_ram_words - 1u);
        result = true;
    }
    return result;
}

void c_spu2::check_effect_irq(uint32_t address)
{
    const uint32_t prior = irq_latches();
    check_input_irq(address, 1u);
    const uint32_t raised = irq_latches() & ~prior;
    m_effect_irq_events += (raised & 1u) + ((raised >> 1u) & 1u);
}

bool c_spu2::read_effect_sample(uint32_t core, uint64_t step, int64_t offset,
    int16_t* sample)
{
    uint32_t address = 0u;
    const bool result = sample != nullptr && effect_address(core, step, offset, &address);
    if (result)
    {
        *sample = static_cast<int16_t>(signed_halfword(m_ram[address]));
        if (effect_enabled(core))
        {
            check_effect_irq(address);
        }
    }
    return result;
}

bool c_spu2::write_effect_sample(uint32_t core, uint64_t step, int64_t offset,
    int32_t sample)
{
    uint32_t address = 0u;
    const bool result = effect_enabled(core) && effect_address(core, step, offset, &address);
    if (result)
    {
        m_ram[address] = static_cast<uint16_t>(clamp_sample(sample));
        check_effect_irq(address);
    }
    return result;
}

int64_t c_spu2::effect_tap_offset(uint32_t core, uint32_t reg) const
{
    const uint16_t* regs = m_cores[core].m_registers;
    const uint32_t bits = (static_cast<uint32_t>(regs[reg / 2u]) << 16u) |
        regs[reg / 2u + 1u];
    // Native SPU2 offsets are halfword units, not PS1 eight-byte units.
    return static_cast<int64_t>(bits) -
        ((bits & 0x80000000u) != 0u ? 0x100000000ll : 0ll);
}

bool c_spu2::calculate_effect_reflections(uint32_t core, uint32_t channel,
    uint64_t step, int16_t input, s_effect_reflections* result)
{
    bool valid = core < 2u && channel < 2u && result != nullptr &&
        effect_start(core) < effect_end(core);
    if (valid)
    {
        const uint16_t* coeff = m_cores[core].m_effect_coefficients;
        const int32_t excitation = scale_mix(input, signed_halfword(coeff[8u + channel]));
        const int32_t wall = signed_halfword(coeff[5u]);
        const int32_t alpha = signed_halfword(coeff[0u]);
        const uint32_t channel_offset = channel * 4u;
        // The DIFF source pair is crossed; destinations stay in channel order.
        const uint32_t source_regs[2] = {0x304u + channel_offset,
            0x328u - channel_offset};
        const uint32_t dest_regs[2] = {0x2ecu + channel_offset,
            0x30cu + channel_offset};
        s_effect_reflections next{};
        for (uint32_t path = 0u; path < 2u && valid; ++path)
        {
            int16_t delayed = 0;
            int16_t previous = 0;
            const int64_t dest = effect_tap_offset(core, dest_regs[path]);
            valid = read_effect_sample(core, step,
                effect_tap_offset(core, source_regs[path]), &delayed) &&
                read_effect_sample(core, step, dest - 1, &previous);
            if (valid)
            {
                const int32_t delta = excitation + scale_mix(delayed, wall) - previous;
                next.m_offset[path] = dest;
                next.m_sample[path] = previous + scale_mix(delta, alpha);
            }
        }
        if (valid)
        {
            *result = next;
        }
    }
    return valid;
}

bool c_spu2::commit_effect_reflections(uint32_t core, uint64_t step,
    const s_effect_reflections& reflections)
{
    bool valid = core < 2u && effect_start(core) < effect_end(core);
    if (valid && effect_enabled(core))
    {
        for (uint32_t path = 0u; path < 2u && valid; ++path)
        {
            valid = write_effect_sample(core, step, reflections.m_offset[path],
                reflections.m_sample[path]);
        }
    }
    return valid;
}

bool c_spu2::calculate_effect_comb(uint32_t core, uint32_t channel,
    uint64_t step, int32_t* result)
{
    bool valid = core < 2u && channel < 2u && result != nullptr &&
        effect_start(core) < effect_end(core);
    if (valid)
    {
        constexpr uint32_t left_regs[4] = {0x2f4u, 0x2fcu, 0x314u, 0x31cu};
        const uint16_t* coeff = m_cores[core].m_effect_coefficients;
        int32_t sum = 0;
        for (uint32_t tap = 0u; tap < 4u && valid; ++tap)
        {
            int16_t sample = 0;
            const int64_t offset = effect_tap_offset(core, left_regs[tap] + channel * 4u);
            // Even a zero coefficient does not suppress a RAM read or IRQ.
            valid = read_effect_sample(core, step, offset, &sample);
            if (valid)
            {
                sum += scale_mix(sample, signed_halfword(coeff[1u + tap]));
            }
        }
        if (valid)
        {
            // Four independently rounded Q15 products fit in int32_t.
            // Do not saturate before the following all-pass stages.
            *result = sum;
        }
    }
    return valid;
}

bool c_spu2::calculate_effect_allpass(uint32_t core, uint32_t channel,
    uint64_t step, int32_t input, s_effect_allpass* result)
{
    // Four signed Q15 products of signed halfwords cannot exceed this
    // range. Reject arbitrary wider inputs to keep intermediates bounded.
    bool valid = core < 2u && channel < 2u && result != nullptr &&
        input >= -131072 && input <= 131072 && effect_start(core) < effect_end(core);
    if (valid)
    {
        const uint16_t* coeff = m_cores[core].m_effect_coefficients;
        s_effect_allpass next{};
        int32_t signal = input;
        for (uint32_t stage = 0u; stage < 2u && valid; ++stage)
        {
            const int64_t destination = effect_tap_offset(core,
                0x32cu + stage * 8u + channel * 4u);
            const int64_t delay = effect_tap_offset(core, 0x2e4u + stage * 4u);
            int16_t history = 0;
            valid = read_effect_sample(core, step, destination - delay, &history);
            if (valid)
            {
                const int32_t gain = signed_halfword(coeff[6u + stage]);
                // Delay-line state and output are distinct: the stored
                // sample is clipped only when actually written to RAM.
                const int32_t state = signal - scale_mix(history, gain);
                next.m_offset[stage] = destination;
                next.m_sample[stage] = state;
                signal = history + scale_mix(state, gain);
            }
        }
        if (valid)
        {
            next.m_output = clamp_sample(signal);
            *result = next;
        }
    }
    return valid;
}

bool c_spu2::process_effect_channel(uint32_t core, uint32_t channel,
    uint64_t step, int16_t input, int16_t* output)
{
    s_effect_reflections reflections{};
    s_effect_allpass allpass{};
    int32_t comb = 0;
    bool valid = output != nullptr &&
        calculate_effect_reflections(core, channel, step, input, &reflections) &&
        calculate_effect_comb(core, channel, step, &comb) &&
        calculate_effect_allpass(core, channel, step, comb, &allpass);
    if (valid && effect_enabled(core))
    {
        valid = commit_effect_reflections(core, step, reflections);
        for (uint32_t stage = 0u; stage < 2u && valid; ++stage)
        {
            valid = write_effect_sample(core, step, allpass.m_offset[stage],
                allpass.m_sample[stage]);
        }
    }
    if (valid)
    {
        *output = allpass.m_output;
    }
    return valid;
}

bool c_spu2::read_disabled_effect(uint32_t core, uint32_t channel,
    uint64_t step, int16_t* output)
{
    // Called only for a valid work area with ATTR effect writes disabled.
    // No tap access can raise an effect IRQ in that state. Reflections
    // and the downsampled input only contribute to discarded writebacks.
    const uint16_t* coeff = m_cores[core].m_effect_coefficients;
    const int32_t gain2 = signed_halfword(coeff[7u]);
    bool valid = false;
    if (gain2 == 0)
    {
        // APF2 output is exactly its old delay tap, independent of
        // the comb accumulator, APF1, reflections and excitation.
        valid = read_effect_sample(core, step,
            effect_tap_offset(core, 0x334u + channel * 4u) -
            effect_tap_offset(core, 0x2e8u), output);
    }
    else if (coeff[6u] == 0u)
    {
        // APF1 output is its old delay tap. Preserve the same signed
        // Q15 rounding and final clamp in the second stage.
        int16_t first = 0;
        int16_t second = 0;
        valid = read_effect_sample(core, step,
            effect_tap_offset(core, 0x32cu + channel * 4u) -
            effect_tap_offset(core, 0x2e4u), &first) &&
            read_effect_sample(core, step,
                effect_tap_offset(core, 0x334u + channel * 4u) -
                effect_tap_offset(core, 0x2e8u), &second);
        if (valid)
        {
            const int32_t state = first - scale_mix(second, gain2);
            *output = clamp_sample(second + scale_mix(state, gain2));
        }
    }
    else
    {
        int32_t comb = 0;
        s_effect_allpass allpass{};
        valid = calculate_effect_comb(core, channel, step, &comb) &&
            calculate_effect_allpass(core, channel, step, comb, &allpass);
        if (valid)
        {
            *output = allpass.m_output;
        }
    }
    return valid;
}

void c_spu2::advance_effect(uint32_t core, const int32_t (&input)[2],
    int16_t (&output)[2])
{
    if (effect_start(core) < effect_end(core))
    {
        c_audio_profile_scope profile(m_audio_profile_enabled, m_profile_effect);
        s_core& state = m_cores[core];
        const uint32_t pos = state.m_effect_history_pos;
        const uint32_t channel = static_cast<uint32_t>(m_mixed_frames & 1u);
        const uint64_t bit = uint64_t{1} << pos;
        for (uint32_t c = 0u; c < 2u; ++c)
        {
            // Keep input history even when no reflection is written,
            // so enabling effects never loses earlier excitation.
            state.m_effect_down[c][pos] = clamp_sample(input[c]);
            state.m_effect_up[c][pos] = 0;
            state.m_effect_up_nonzero[c] &= ~bit;
        }
        int16_t filtered = 0;
        const bool fast = m_effect_fast_path && !effect_enabled(core);
        const bool valid = fast ?
            read_disabled_effect(core, channel, m_mixed_frames >> 1u, &filtered) :
            process_effect_channel(core, channel, m_mixed_frames >> 1u,
                resample_effect(state.m_effect_down[channel], pos, false), &filtered);
        if (valid)
        {
            ++state.m_effect_ticks;
            if (fast)
            {
                ++state.m_effect_fast_ticks;
            }
            state.m_effect_up[channel][pos] = filtered;
            if (filtered != 0)
            {
                state.m_effect_up_nonzero[channel] |= bit;
            }
        }
        state.m_effect_history_pos = (pos + 1u) & 63u;
        bool nonzero = false;
        for (uint32_t c = 0u; c < 2u; ++c)
        {
            // A zero occupancy mask proves the entire 64-word history
            // is zero, including all 39 FIR taps. Never infer this from
            // the current input, EVOL, or a previous zero output.
            if (m_effect_fast_path && state.m_effect_up_nonzero[c] == 0u)
            {
                output[c] = 0;
                ++state.m_effect_zero_fir_skips;
            }
            else
            {
                output[c] = resample_effect(state.m_effect_up[c], state.m_effect_history_pos, true);
            }
            nonzero |= output[c] != 0;
        }
        if (nonzero)
        {
            ++state.m_effect_return_frames;
        }
    }
}

uint32_t c_spu2::effect_irq_cycles_remaining() const
{
    uint64_t earliest = 0u;
    const uint32_t sample_remainder = sample_phase();
    const uint32_t next_sample = k_cycles_per_sample - sample_remainder;
    // Solve the circular tap address for each physical IRQA alias and
    // alternating channel. No RAM transaction or IRQ is generated here.
    for (uint32_t core = 0u; core < 2u; ++core)
    {
        if (!effect_enabled(core))
        {
            continue;
        }
        const uint32_t start = effect_start(core);
        const uint32_t end = effect_end(core);
        if (start >= end)
        {
            continue;
        }
        const uint32_t length = end - start + 1u;
        for (const s_core& candidate : m_cores)
        {
            if ((candidate.m_attr & 0x40u) == 0u || candidate.m_irq)
            {
                continue;
            }
            // Physical sound RAM is mirrored in the logical effect area.
            // Find the first alias in the work area before reading taps;
            // a target outside it cannot be touched by any channel.
            uint32_t first_alias = candidate.m_irqa;
            if (first_alias < start)
            {
                first_alias += ((start - first_alias + k_ram_words - 1u) /
                    k_ram_words) * k_ram_words;
            }
            if (first_alias > end)
            {
                continue;
            }
            for (uint32_t channel = 0u; channel < 2u; ++channel)
            {
                const uint32_t shift = channel * 4u;
                const int64_t same = effect_tap_offset(core, 0x2ecu + shift);
                const int64_t diff = effect_tap_offset(core, 0x30cu + shift);
                const int64_t apf1 = effect_tap_offset(core, 0x32cu + shift);
                const int64_t apf2 = effect_tap_offset(core, 0x334u + shift);
                const int64_t taps[] = {
                    effect_tap_offset(core, 0x304u + shift), same, same - 1,
                    effect_tap_offset(core, 0x328u - shift), diff, diff - 1,
                    effect_tap_offset(core, 0x2f4u + shift),
                    effect_tap_offset(core, 0x2fcu + shift),
                    effect_tap_offset(core, 0x314u + shift),
                    effect_tap_offset(core, 0x31cu + shift),
                    apf1, apf1 - effect_tap_offset(core, 0x2e4u),
                    apf2, apf2 - effect_tap_offset(core, 0x2e8u)};
                const uint64_t parity = (m_mixed_frames & 1u) ^ channel;
                const uint64_t step = (m_mixed_frames + parity) >> 1u;
                const int64_t step_in_ring = static_cast<int64_t>(step % length);
                for (uint32_t alias = first_alias; alias <= end; alias += k_ram_words)
                {
                    for (int64_t tap : taps)
                    {
                        int64_t distance = static_cast<int64_t>(alias - start) -
                            step_in_ring - tap % length;
                        distance %= length;
                        if (distance < 0)
                        {
                            distance += length;
                        }
                        const uint64_t cycles = (static_cast<uint64_t>(distance) * 2u +
                            parity + 1u) * k_cycles_per_sample - sample_remainder;
                        if (cycles == next_sample)
                        {
                            return next_sample;
                        }
                        if (earliest == 0u || cycles < earliest)
                        {
                            earliest = cycles;
                        }
                    }
                }
            }
        }
    }
    return static_cast<uint32_t>(std::min<uint64_t>(earliest, 0xffffffffu));
}

void c_spu2::s_envelope::step(uint32_t rate, bool decrease, bool exponential)
{
    // Reference SDBZ envelope clock. Counter is retained across phase and
    // register changes; all signed products fit within int32_t.
    const int32_t shift = static_cast<int32_t>(rate >> 2u);
    uint32_t period = 1u << std::max(0, shift - 11);
    if (exponential && !decrease && m_level > 0x6000)
    {
        period *= 4u;
    }
    if (++m_counter >= period)
    {
        m_counter = 0u;
        const int32_t magnitude = decrease ?
            -8 + static_cast<int32_t>(rate & 3u) :
            7 - static_cast<int32_t>(rate & 3u);
        int32_t delta = magnitude * (1 << std::max(0, 11 - shift));
        if (exponential && decrease)
        {
            delta = floor_shift(delta * m_level, 15u);
        }
        m_level = std::clamp(m_level + delta, 0, 0x7fff);
    }
}

int16_t c_spu2::gaussian_sample(const int16_t (&samples)[4], uint32_t fraction)
{
    // Hardware coefficient ROM measured/documented in PSX-SPX.
    static constexpr int16_t gauss[512] =
    {
        -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1,
        0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001,
        0x0001, 0x0001, 0x0001, 0x0002, 0x0002, 0x0002, 0x0003, 0x0003,
        0x0003, 0x0004, 0x0004, 0x0005, 0x0005, 0x0006, 0x0007, 0x0007,
        0x0008, 0x0009, 0x0009, 0x000a, 0x000b, 0x000c, 0x000d, 0x000e,
        0x000f, 0x0010, 0x0011, 0x0012, 0x0013, 0x0015, 0x0016, 0x0018,
        0x0019, 0x001b, 0x001c, 0x001e, 0x0020, 0x0021, 0x0023, 0x0025,
        0x0027, 0x0029, 0x002c, 0x002e, 0x0030, 0x0033, 0x0035, 0x0038,
        0x003a, 0x003d, 0x0040, 0x0043, 0x0046, 0x0049, 0x004d, 0x0050,
        0x0054, 0x0057, 0x005b, 0x005f, 0x0063, 0x0067, 0x006b, 0x006f,
        0x0074, 0x0078, 0x007d, 0x0082, 0x0087, 0x008c, 0x0091, 0x0096,
        0x009c, 0x00a1, 0x00a7, 0x00ad, 0x00b3, 0x00ba, 0x00c0, 0x00c7,
        0x00cd, 0x00d4, 0x00db, 0x00e3, 0x00ea, 0x00f2, 0x00fa, 0x0101,
        0x010a, 0x0112, 0x011b, 0x0123, 0x012c, 0x0135, 0x013f, 0x0148,
        0x0152, 0x015c, 0x0166, 0x0171, 0x017b, 0x0186, 0x0191, 0x019c,
        0x01a8, 0x01b4, 0x01c0, 0x01cc, 0x01d9, 0x01e5, 0x01f2, 0x0200,
        0x020d, 0x021b, 0x0229, 0x0237, 0x0246, 0x0255, 0x0264, 0x0273,
        0x0283, 0x0293, 0x02a3, 0x02b4, 0x02c4, 0x02d6, 0x02e7, 0x02f9,
        0x030b, 0x031d, 0x0330, 0x0343, 0x0356, 0x036a, 0x037e, 0x0392,
        0x03a7, 0x03bc, 0x03d1, 0x03e7, 0x03fc, 0x0413, 0x042a, 0x0441,
        0x0458, 0x0470, 0x0488, 0x04a0, 0x04b9, 0x04d2, 0x04ec, 0x0506,
        0x0520, 0x053b, 0x0556, 0x0572, 0x058e, 0x05aa, 0x05c7, 0x05e4,
        0x0601, 0x061f, 0x063e, 0x065c, 0x067c, 0x069b, 0x06bb, 0x06dc,
        0x06fd, 0x071e, 0x0740, 0x0762, 0x0784, 0x07a7, 0x07cb, 0x07ef,
        0x0813, 0x0838, 0x085d, 0x0883, 0x08a9, 0x08d0, 0x08f7, 0x091e,
        0x0946, 0x096f, 0x0998, 0x09c1, 0x09eb, 0x0a16, 0x0a40, 0x0a6c,
        0x0a98, 0x0ac4, 0x0af1, 0x0b1e, 0x0b4c, 0x0b7a, 0x0ba9, 0x0bd8,
        0x0c07, 0x0c38, 0x0c68, 0x0c99, 0x0ccb, 0x0cfd, 0x0d30, 0x0d63,
        0x0d97, 0x0dcb, 0x0e00, 0x0e35, 0x0e6b, 0x0ea1, 0x0ed7, 0x0f0f,
        0x0f46, 0x0f7f, 0x0fb7, 0x0ff1, 0x102a, 0x1065, 0x109f, 0x10db,
        0x1116, 0x1153, 0x118f, 0x11cd, 0x120b, 0x1249, 0x1288, 0x12c7,
        0x1307, 0x1347, 0x1388, 0x13c9, 0x140b, 0x144d, 0x1490, 0x14d4,
        0x1517, 0x155c, 0x15a0, 0x15e6, 0x162c, 0x1672, 0x16b9, 0x1700,
        0x1747, 0x1790, 0x17d8, 0x1821, 0x186b, 0x18b5, 0x1900, 0x194b,
        0x1996, 0x19e2, 0x1a2e, 0x1a7b, 0x1ac8, 0x1b16, 0x1b64, 0x1bb3,
        0x1c02, 0x1c51, 0x1ca1, 0x1cf1, 0x1d42, 0x1d93, 0x1de5, 0x1e37,
        0x1e89, 0x1edc, 0x1f2f, 0x1f82, 0x1fd6, 0x202a, 0x207f, 0x20d4,
        0x2129, 0x217f, 0x21d5, 0x222c, 0x2282, 0x22da, 0x2331, 0x2389,
        0x23e1, 0x2439, 0x2492, 0x24eb, 0x2545, 0x259e, 0x25f8, 0x2653,
        0x26ad, 0x2708, 0x2763, 0x27be, 0x281a, 0x2876, 0x28d2, 0x292e,
        0x298b, 0x29e7, 0x2a44, 0x2aa1, 0x2aff, 0x2b5c, 0x2bba, 0x2c18,
        0x2c76, 0x2cd4, 0x2d33, 0x2d91, 0x2df0, 0x2e4f, 0x2eae, 0x2f0d,
        0x2f6c, 0x2fcc, 0x302b, 0x308b, 0x30ea, 0x314a, 0x31aa, 0x3209,
        0x3269, 0x32c9, 0x3329, 0x3389, 0x33e9, 0x3449, 0x34a9, 0x3509,
        0x3569, 0x35c9, 0x3629, 0x3689, 0x36e8, 0x3748, 0x37a8, 0x3807,
        0x3867, 0x38c6, 0x3926, 0x3985, 0x39e4, 0x3a43, 0x3aa2, 0x3b00,
        0x3b5f, 0x3bbd, 0x3c1b, 0x3c79, 0x3cd7, 0x3d35, 0x3d92, 0x3def,
        0x3e4c, 0x3ea9, 0x3f05, 0x3f62, 0x3fbd, 0x4019, 0x4074, 0x40d0,
        0x412a, 0x4185, 0x41df, 0x4239, 0x4292, 0x42eb, 0x4344, 0x439c,
        0x43f4, 0x444c, 0x44a3, 0x44fa, 0x4550, 0x45a6, 0x45fc, 0x4651,
        0x46a6, 0x46fa, 0x474e, 0x47a1, 0x47f4, 0x4846, 0x4898, 0x48e9,
        0x493a, 0x498a, 0x49d9, 0x4a29, 0x4a77, 0x4ac5, 0x4b13, 0x4b5f,
        0x4bac, 0x4bf7, 0x4c42, 0x4c8d, 0x4cd7, 0x4d20, 0x4d68, 0x4db0,
        0x4df7, 0x4e3e, 0x4e84, 0x4ec9, 0x4f0e, 0x4f52, 0x4f95, 0x4fd7,
        0x5019, 0x505a, 0x509a, 0x50da, 0x5118, 0x5156, 0x5194, 0x51d0,
        0x520c, 0x5247, 0x5281, 0x52ba, 0x52f3, 0x532a, 0x5361, 0x5397,
        0x53cc, 0x5401, 0x5434, 0x5467, 0x5499, 0x54ca, 0x54fa, 0x5529,
        0x5558, 0x5585, 0x55b2, 0x55de, 0x5609, 0x5632, 0x565b, 0x5684,
        0x56ab, 0x56d1, 0x56f6, 0x571b, 0x573e, 0x5761, 0x5782, 0x57a3,
        0x57c3, 0x57e2, 0x57ff, 0x581c, 0x5838, 0x5853, 0x586d, 0x5886,
        0x589e, 0x58b5, 0x58cb, 0x58e0, 0x58f4, 0x5907, 0x5919, 0x592a,
        0x593a, 0x5949, 0x5958, 0x5965, 0x5971, 0x597c, 0x5986, 0x598f,
        0x5997, 0x599e, 0x59a4, 0x59a9, 0x59ad, 0x59b0, 0x59b2, 0x59b3,
    };
    const uint32_t phase = (fraction >> 4u) & 0xffu;
    const int32_t sum =
        floor_shift(gauss[255u - phase] * samples[0], 15u) +
        floor_shift(gauss[511u - phase] * samples[1], 15u) +
        floor_shift(gauss[256u + phase] * samples[2], 15u) +
        floor_shift(gauss[phase] * samples[3], 15u);
    return clamp_sample(sum);
}

uint32_t c_spu2::modulated_pitch(uint16_t pitch, int16_t previous)
{
    const int32_t value = scale_mix(static_cast<int32_t>(pitch), 32768 + previous);
    return static_cast<uint32_t>(std::clamp(value, 0, 0x3fff));
}

int16_t c_spu2::noise_sample(uint32_t core) const
{
    return core < 2u ? static_cast<int16_t>(signed_halfword(m_cores[core].m_noise)) : 0;
}

int16_t c_spu2::advance_noise(s_core& core)
{
    // Shared per-core generator, clocked even with no allocated voices.
    const uint32_t clock = (core.m_attr >> 8u) & 0x3fu;
    constexpr uint32_t fraction[4] = {0u, 84u, 140u, 180u};
    const uint32_t increment = fraction[clock & 3u];
    const uint32_t limit = (0x8000u >> (clock >> 2u)) << 16u;
    uint64_t next = static_cast<uint64_t>(core.m_noise_counter) + 0x10000u + increment;
    if ((next & 0xffffu) >= 210u)
    {
        next += 0x10000u - increment;
    }
    if (next >= limit)
    {
        next %= limit;
        const uint32_t bits = core.m_noise;
        const uint32_t feedback = 1u ^ (((bits >> 10u) ^ (bits >> 11u) ^
            (bits >> 12u) ^ (bits >> 15u)) & 1u);
        core.m_noise = static_cast<uint16_t>((bits << 1u) | feedback);
    }
    core.m_noise_counter = static_cast<uint32_t>(next);
    return static_cast<int16_t>(signed_halfword(core.m_noise));
}

void c_spu2::advance_master_volume(s_core& core)
{
    for (uint32_t channel = 0u; channel < 2u; ++channel)
    {
        const uint16_t reg = core.m_master[channel];
        if ((reg & 0x8000u) == 0u)
        {
            continue;
        }
        s_envelope& volume = core.m_master_volume[channel];
        const bool decrease = (reg & 0x2000u) != 0u;
        const bool exponential = (reg & 0x4000u) != 0u;
        const bool negative = (reg & 0x1000u) != 0u;
        const uint32_t rate = reg & 0x7fu;
        const int32_t shift = static_cast<int32_t>(rate >> 2u);
        uint32_t increment = 0x8000u >> std::max(0, shift - 11);
        int32_t delta = (decrease ? -8 + static_cast<int32_t>(rate & 3u) :
            7 - static_cast<int32_t>(rate & 3u)) * (1 << std::max(0, 11 - shift));
        if (exponential && !decrease && volume.m_level > 0x6000)
        {
            increment >>= 2u;
        }
        if (exponential && decrease)
        {
            delta = floor_shift(delta * volume.m_level, 15u);
        }
        else if (negative)
        {
            delta = -delta;
        }
        if (rate != 0x7fu)
        {
            increment = std::max(1u, increment);
        }
        volume.m_counter += increment;
        if (volume.m_counter >= 0x8000u)
        {
            volume.m_counter = 0u;
            const int32_t low = decrease && (!negative || exponential) ? 0 : -32768;
            const int32_t high = decrease && negative && !exponential ? 0 : 32767;
            volume.m_level = std::clamp(volume.m_level + delta, low, high);
        }
    }
}

void c_spu2::advance_silent_controls(uint64_t samples)
{
    for (uint64_t sample = 0u; sample < samples; ++sample)
    {
        for (s_core& core : m_cores)
        {
            (void)advance_noise(core);
            advance_master_volume(core);
        }
    }
}

void c_spu2::update_envelope(s_voice& voice, uint16_t adsr1, uint16_t adsr2)
{
    s_envelope& env = voice.m_envelope;
    switch (voice.m_phase)
    {
    case e_voice_phase::attack:
        env.step((adsr1 >> 8u) & 0x7fu, false, (adsr1 & 0x8000u) != 0u);
        if (env.m_level >= 0x7fff)
        {
            voice.m_phase = e_voice_phase::decay;
        }
        break;
    case e_voice_phase::decay:
        env.step(((adsr1 >> 4u) & 15u) << 2u, true, true);
        if (env.m_level <= static_cast<int32_t>(((adsr1 & 15u) + 1u) * 0x800u))
        {
            voice.m_phase = e_voice_phase::sustain;
        }
        break;
    case e_voice_phase::sustain:
        env.step((adsr2 >> 6u) & 0x7fu, (adsr2 & 0x4000u) != 0u,
                 (adsr2 & 0x8000u) != 0u);
        break;
    case e_voice_phase::release:
        env.step((adsr2 & 31u) << 2u, true, (adsr2 & 0x20u) != 0u);
        if (env.m_level <= 0)
        {
            voice.m_phase = e_voice_phase::off;
        }
        break;
    default:
        break;
    }
}

void c_spu2::decode_adpcm_block(const uint16_t* words, int16_t* samples,
                               int32_t& history1, int32_t& history2)
{
    // SDBZ reference predictor table, nibble order, rounding and saturation.
    static constexpr int32_t filters[5][2] =
    {
        {0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}
    };
    const uint32_t shift = words[0] & 15u;
    const uint32_t filter = std::min<uint32_t>((words[0] >> 4u) & 15u, 4u);
    for (uint32_t i = 0u; i < 28u; ++i)
    {
        const uint32_t nibble = (words[1u + i / 4u] >> ((i & 3u) * 4u)) & 15u;
        const int32_t signed_nibble = static_cast<int32_t>(nibble) -
            (nibble >= 8u ? 16 : 0);
        const int32_t prediction = floor_shift(
            history1 * filters[filter][0] + history2 * filters[filter][1] + 32, 6u);
        const int32_t sample = std::clamp(
            floor_shift(signed_nibble * 4096, shift) + prediction, -32768, 32767);
        history2 = history1;
        history1 = sample;
        samples[i] = static_cast<int16_t>(sample);
    }
}

uint32_t c_spu2::voice_address(const s_core& core, uint32_t voice, uint32_t field)
{
    const uint32_t reg = (0x1c0u + voice * 12u + field) / 2u;
    return ((core.m_registers[reg] & 15u) << 16u) | core.m_registers[reg + 1u];
}

void c_spu2::decode_voice_block(s_voice& voice, uint32_t slot)
{
    c_audio_profile_scope profile(m_audio_profile_enabled, m_profile_adpcm);
    const uint32_t address = voice.m_next & (k_ram_words - 1u);
    uint16_t words[8];
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        words[i] = m_ram[(address + i) & (k_ram_words - 1u)];
    }
    decode_adpcm_block(words, voice.m_samples[slot], voice.m_history1, voice.m_history2);
    ++m_decoded_blocks;
    voice.m_block[slot] = address;
    voice.m_flags[slot] = static_cast<uint8_t>(words[0] >> 8u);
    if ((voice.m_flags[slot] & 4u) != 0u && !voice.m_loop_written)
    {
        voice.m_loop = address;
    }
    voice.m_next = (voice.m_flags[slot] & 1u) != 0u ? voice.m_loop :
        (address + 8u) & (k_ram_words - 1u);
}

void c_spu2::check_voice_irq(uint32_t address)
{
    // The reference raises IRQ on the consumed block, not speculative
    // lookahead. Either core may observe the shared sound RAM access.
    for (s_core& candidate : m_cores)
    {
        const uint32_t distance = (candidate.m_irqa - address) &
            (k_ram_words - 1u);
        if ((candidate.m_attr & 0x40u) != 0u &&
            !candidate.m_irq && distance < 8u)
        {
            candidate.m_irq = true;
            ++m_playback_irq_events;
        }
    }
}

void c_spu2::start_voice(s_core& core, uint32_t index)
{
    s_voice& voice = core.m_voices[index];
    voice.m_pending = false;
    voice.m_started = true;
    voice.m_next = voice_address(core, index, 0u);
    voice.m_position = 0u;
    voice.m_history1 = 0;
    voice.m_history2 = 0;
    voice.m_previous_sample = 0;
    voice.m_phase = e_voice_phase::attack;
    voice.m_envelope = {};
    core.m_endx &= ~(1u << index);
    ++m_voice_starts;
    decode_voice_block(voice, 0u);
    decode_voice_block(voice, 1u);
    check_voice_irq(voice.m_block[0]);
}

void c_spu2::advance_voices(uint32_t samples, int32_t (&dry)[2][k_input_batch][2],
    int32_t (&wet)[2][k_input_batch][2])
{
    c_audio_profile_scope profile(m_audio_profile_enabled, m_profile_voices);
    constexpr uint32_t block_length = 28u << 12u;
    for (uint32_t core_index = 0u; core_index < 2u; ++core_index)
    {
        s_core& core = m_cores[core_index];
        int16_t noise[k_input_batch]{};
        for (uint32_t sample = 0u; sample < samples; ++sample)
        {
            noise[sample] = advance_noise(core);
        }
        if (core.m_key_mask == 0u)
        {
            continue;
        }
        int16_t previous[k_input_batch]{};
        const auto voice_mask = [&core](uint32_t offset)
        {
            return static_cast<uint32_t>(core.m_registers[offset / 2u]) |
                (static_cast<uint32_t>(core.m_registers[offset / 2u + 1u] & 0xffu) << 16u);
        };
        const uint32_t pitch_mask = voice_mask(0x180u) & 0xfffffeu;
        const uint32_t noise_mask = voice_mask(0x184u);
        const uint32_t dry_mask[2] = {voice_mask(0x188u), voice_mask(0x190u)};
        const uint32_t wet_mask[2] = {voice_mask(0x18cu), voice_mask(0x194u)};
        for (uint32_t index = 0u; index < 24u; ++index)
        {
            const uint32_t bit = 1u << index;
            if ((core.m_key_mask & bit) == 0u)
            {
                std::fill_n(previous, samples, int16_t{0});
                continue;
            }
            s_voice& voice = core.m_voices[index];
            if (voice.m_pending)
            {
                start_voice(core, index);
            }
            const uint16_t* regs = &core.m_registers[index * 8u];
            const uint32_t fixed_pitch = std::min<uint32_t>(regs[2], 0x3fffu);
            // Envelope release may stop on any sample, so retire the
            // cursor and ADSR in reference order, not an aggregate pitch
            // advance followed by a delayed envelope update.
            for (uint32_t sample = 0u; sample < samples; ++sample)
            {
                const uint32_t index_in_block = voice.m_position >> 12u;
                const uint32_t fraction = voice.m_position & 0xfffu;
                const auto decoded = [&](uint32_t index)
                {
                    return voice.m_samples[index / 28u][index % 28u];
                };
                const int16_t window[4] = {
                    index_in_block == 0u ? voice.m_previous_sample : decoded(index_in_block - 1u),
                    decoded(index_in_block), decoded(index_in_block + 1u),
                    decoded(index_in_block + 2u)};
                const int32_t interpolated = (noise_mask & bit) != 0u ? noise[sample] :
                    gaussian_sample(window, fraction);
                const uint32_t pitch = (pitch_mask & bit) != 0u ?
                    modulated_pitch(regs[2], previous[sample]) : fixed_pitch;
                voice.m_position += pitch;
                if (voice.m_position >= block_length)
                {
                    voice.m_position -= block_length;
                    const uint8_t flags = voice.m_flags[0];
                    voice.m_block[0] = voice.m_block[1];
                    voice.m_flags[0] = voice.m_flags[1];
                    voice.m_previous_sample = voice.m_samples[0][27u];
                    std::copy_n(voice.m_samples[1], 28u, voice.m_samples[0]);
                    if ((flags & 1u) != 0u)
                    {
                        core.m_endx |= bit;
                        ++m_voice_end_events;
                        if ((flags & 2u) == 0u)
                        {
                            voice.m_phase = e_voice_phase::off;
                            voice.m_envelope.m_level = 0;
                        }
                    }
                    // As in the reference, the next block becomes the
                    // visible cursor even at a non-repeating end.
                    check_voice_irq(voice.m_block[0]);
                    if (voice.m_phase != e_voice_phase::off)
                    {
                        decode_voice_block(voice, 1u);
                    }
                }
                update_envelope(voice, regs[3], regs[4]);
                for (uint32_t channel = 0u; channel < 2u; ++channel)
                {
                    const uint16_t volume = regs[channel];
                    if ((volume & 0x8000u) != 0u)
                    {
                        voice.m_volume[channel].step(volume & 0x7fu,
                            (volume & 0x2000u) != 0u, (volume & 0x4000u) != 0u);
                    }
                }
                if (voice.m_phase == e_voice_phase::off)
                {
                    core.m_key_mask &= ~bit;
                    std::fill_n(previous + sample, samples - sample, int16_t{0});
                    break;
                }
                const int32_t enveloped = floor_shift(
                    interpolated * voice.m_envelope.m_level, 15u);
                previous[sample] = clamp_sample(enveloped);
                record_peak(core.m_source_peak, interpolated);
                record_peak(core.m_envelope_peak, enveloped);
                if (index == 1u || index == 3u)
                {
                    const uint32_t ring = index == 1u ? 2u : 3u;
                    const uint32_t slot = static_cast<uint32_t>(
                        m_mixed_frames % k_input_batch) + sample;
                    m_mixer_rings[core_index][ring][slot] = clamp_sample(enveloped);
                }
                for (uint32_t channel = 0u; channel < 2u; ++channel)
                {
                    const int32_t gain = voice.m_volume[channel].m_level;
                    const int32_t contribution = floor_shift(enveloped * gain, 15u);
                    record_peak(core.m_voice_volume_peak, contribution);
                    if ((dry_mask[channel] & bit) != 0u)
                    {
                        dry[core_index][sample][channel] += contribution;
                    }
                    if ((wet_mask[channel] & bit) != 0u)
                    {
                        wet[core_index][sample][channel] += contribution;
                    }
                }
            }
        }
    }
}

uint32_t c_spu2::input_frames(uint32_t core) const
{
    return core < 2u ? m_cores[core].m_input_count : 0u;
}

uint32_t c_spu2::input_refill_cycles(uint32_t core) const
{
    if (core >= 2u || !m_clock_active)
    {
        return 0u;
    }
    // Reference feed_input refills after a 32-frame tick once an
    // entire 256-frame half is available. Initial fill is immediate.
    const uint32_t count = input_frames(core);
    const uint64_t now = elapsed_samples();
    const uint64_t ready = now + std::max<uint32_t>(
        1u, count > 256u ? count - 256u : 0u);
    const uint64_t tick = ((ready + k_input_batch - 1u) / k_input_batch) * k_input_batch;
    return static_cast<uint32_t>((tick - now) * k_cycles_per_sample - sample_phase());
}

bool c_spu2::push_input(uint32_t core, const uint8_t* data, uint32_t bytes)
{
    c_audio_profile_scope profile(m_audio_profile_enabled, m_profile_input_dma);
    if (core >= 2u || data == nullptr || bytes == 0u ||
        bytes > k_input_chunk_bytes || (bytes & 3u) != 0u ||
        input_frames(core) > k_input_frames - 256u)
    {
        return false;
    }
    s_core& state = m_cores[core];
    const uint32_t frames = bytes / 4u;
    const uint32_t base = 0x2000u + core * 0x400u;
    for (uint32_t i = 0u; i < frames; ++i)
    {
        const uint32_t slot = (state.m_input_read + state.m_input_count + i) %
            k_input_frames;
        for (uint32_t channel = 0u; channel < 2u; ++channel)
        {
            const uint32_t offset = channel * (bytes / 2u) + i * 2u;
            const uint16_t word = static_cast<uint16_t>(data[offset] |
                (static_cast<uint16_t>(data[offset + 1u]) << 8u));
            const int32_t signed_word = static_cast<int32_t>(word) -
                (word >= 0x8000u ? 0x10000 : 0);
            state.m_input[slot][channel] = static_cast<int16_t>(signed_word);
            m_ram[base + channel * k_input_frames + slot] = word;
        }
    }
    state.m_input_count += frames;
    m_input_frames_written += frames;
    return true;
}

void c_spu2::check_input_irq(uint32_t address, uint32_t words)
{
    for (s_core& candidate : m_cores)
    {
        const uint32_t distance = (candidate.m_irqa - address) & (k_ram_words - 1u);
        if ((candidate.m_attr & 0x40u) != 0u && !candidate.m_irq &&
            distance < words)
        {
            candidate.m_irq = true;
            ++m_playback_irq_events;
        }
    }
}

void c_spu2::advance_input(uint32_t samples, int32_t (&sum)[2][k_input_batch][2])
{
    c_audio_profile_scope profile(m_audio_profile_enabled, m_profile_input_mix);
    for (uint32_t core = 0u; core < 2u; ++core)
    {
        s_core& state = m_cores[core];
        const uint32_t count = std::min(samples, state.m_input_count);
        if (count == 0u)
        {
            continue;
        }
        // Reference checks the left input-ring address on consumption.
        const uint32_t base = 0x2000u + core * 0x400u;
        const uint32_t first = std::min(count, k_input_frames - state.m_input_read);
        check_input_irq(base + state.m_input_read, first);
        if (count > first)
        {
            check_input_irq(base, count - first);
        }
        for (uint32_t sample = 0u; sample < count; ++sample)
        {
            const uint32_t slot = (state.m_input_read + sample) % k_input_frames;
            for (uint32_t channel = 0u; channel < 2u; ++channel)
            {
                // Core 0 taps the raw input; core 1's input tap is
                // the post-master core 0 output, populated by the mixer.
                if (core == 0u)
                {
                    const uint32_t ring_slot = static_cast<uint32_t>(
                        m_mixed_frames % k_input_batch) + sample;
                    m_mixer_rings[0u][channel][ring_slot] = state.m_input[slot][channel];
                }
                // B-volume is the signed AutoDMA input gain.
                const int32_t raw = state.m_input[slot][channel];
                const int32_t gained = floor_shift(raw *
                    signed_halfword(state.m_master[6u + channel]), 15u);
                record_peak(state.m_input_raw_peak, raw);
                record_peak(state.m_input_gain_peak, gained);
                sum[core][sample][channel] += gained;
            }
        }
        state.m_input_read = (state.m_input_read + count) % k_input_frames;
        state.m_input_count -= count;
        m_input_frames_consumed += count;
    }
}

void c_spu2::emit_pcm(int16_t left, int16_t right)
{
    ++m_mixed_frames;
    if (left != 0 || right != 0)
    {
        ++m_nonzero_frames;
    }
    const int32_t l = left;
    const int32_t r = right;
    m_pcm_peak = std::max(m_pcm_peak, static_cast<uint32_t>(
        std::max(l < 0 ? -l : l, r < 0 ? -r : r)));
    if (m_pcm_sink != nullptr)
    {
        m_pcm_batch[m_pcm_pending * 2u] = left;
        m_pcm_batch[m_pcm_pending * 2u + 1u] = right;
        if (++m_pcm_pending == k_input_batch)
        {
            c_audio_profile_scope profile(m_audio_profile_enabled, m_profile_sink);
            m_pcm_sink(m_pcm_user, m_pcm_batch, k_input_batch);
            m_pcm_pending = 0u;
        }
    }
}

void c_spu2::count_mixer_irq(uint32_t address, uint32_t words)
{
    const uint32_t prior = irq_latches();
    check_input_irq(address, words);
    const uint32_t raised = irq_latches() & ~prior;
    m_mixer_irq_events += (raised & 1u) + ((raised >> 1u) & 1u);
}

void c_spu2::write_mixer_rings()
{
    c_audio_profile_scope profile(m_audio_profile_enabled, m_profile_rings);
    // Reference tick N writes 32 frames at (N*32) modulo 512. The
    // first tick writes words 32..63, not words 0..31.
    const uint32_t position = static_cast<uint32_t>(m_mixed_frames % k_input_frames);
    for (uint32_t core = 0u; core < 2u; ++core)
    {
        for (uint32_t ring = 0u; ring < 8u; ++ring)
        {
            const uint32_t address = (ring < 4u ? 0u : 0x1000u) +
                core * 0x800u + (ring & 3u) * k_input_frames + position;
            std::copy_n(m_mixer_rings[core][ring], k_input_batch,
                m_ram.begin() + address);
            count_mixer_irq(address, k_input_batch);
        }
    }
    ++m_mixer_ring_batches;
}

void c_spu2::mix_frames(uint64_t samples)
{
    c_audio_profile_scope profile(m_audio_profile_enabled, m_profile_mix);
    while (samples != 0u)
    {
        const uint32_t offset = static_cast<uint32_t>(m_mixed_frames % k_input_batch);
        const bool effects_valid = effect_start(0u) < effect_end(0u) ||
            effect_start(1u) < effect_end(1u);
        const bool effect_writes = (effect_enabled(0u) && effect_start(0u) < effect_end(0u)) ||
            (effect_enabled(1u) && effect_start(1u) < effect_end(1u));
        const bool silent = !effects_valid && (m_cores[0].m_key_mask | m_cores[1].m_key_mask |
            m_cores[0].m_input_count | m_cores[1].m_input_count) == 0u;
        if (silent && m_pcm_sink == nullptr && offset == 0u &&
            samples >= k_input_frames)
        {
            // A complete silent ring revolution writes every reserved
            // mixer word exactly once. There are no source reads or
            // external callbacks within this advance; IRQ latches may
            // rise only once per core. Preserve the exact batch phase.
            const uint64_t frames = samples / k_input_frames * k_input_frames;
            c_audio_profile_scope profile(m_audio_profile_enabled, m_profile_silent_rings);
            std::fill_n(m_ram.begin(), 0x2000u, 0u);
            count_mixer_irq(0u, 0x2000u);
            advance_silent_controls(frames);
            m_mixed_frames += frames;
            m_mixer_ring_batches += frames / k_input_batch;
            samples -= frames;
            continue;
        }
        // Active effect RAM writes must not run behind a batch of future
        // voice fetches or input reads that could alias the work area.
        const uint32_t count = static_cast<uint32_t>(
            std::min<uint64_t>(samples, effect_writes ? 1u : k_input_batch - offset));
        for (auto& core : m_mixer_rings)
        {
            for (auto& ring : core)
            {
                std::fill_n(ring + offset, count, int16_t{0});
            }
        }
        if (silent && m_pcm_sink == nullptr)
        {
            advance_silent_controls(count);
            m_mixed_frames += count;
        }
        else
        {
            int32_t sum[2][k_input_batch][2]{};
            int32_t wet[2][k_input_batch][2]{};
            advance_voices(count, sum, wet);
            // Dry and wet voice taps precede input gain and master scaling.
            // Wet taps are effect inputs, not the processed reverb return.
            for (uint32_t core = 0u; core < 2u; ++core)
            {
                for (uint32_t sample = 0u; sample < count; ++sample)
                {
                    for (uint32_t channel = 0u; channel < 2u; ++channel)
                    {
                        m_mixer_rings[core][4u + channel][offset + sample] =
                            clamp_sample(sum[core][sample][channel]);
                        m_mixer_rings[core][6u + channel][offset + sample] =
                            clamp_sample(wet[core][sample][channel]);
                    }
                }
            }
            int32_t input[2][k_input_batch][2]{};
            advance_input(count, input);
            const uint16_t mmix[2] = {
                static_cast<uint16_t>(m_cores[0].m_registers[0x198u / 2u] & 0x0ff0u),
                static_cast<uint16_t>(m_cores[1].m_registers[0x198u / 2u] & 0x0fffu)};
            for (uint32_t sample = 0u; sample < count; ++sample)
            {
                int16_t pcm[2]{};
                int32_t external[2]{};
                for (uint32_t core = 0u; core < 2u; ++core)
                {
                    bool effect_nonzero = false;
                    s_core& state = m_cores[core];
                    if ((state.m_attr & 0x4000u) != 0u)
                    {
                        ++state.m_attr14_frames;
                    }
                    int32_t dry_mix[2]{};
                    int32_t effect_mix[2]{};
                    int16_t effect_return[2]{};
                    for (uint32_t channel = 0u; channel < 2u; ++channel)
                    {
                        // MMIX pairs are right/left, whereas PCM is left/right.
                        const uint32_t shift = 1u - channel;
                        const auto gate = [&](uint32_t right_bit, int32_t value)
                        {
                            return (mmix[core] & (right_bit << shift)) != 0u ? value : 0;
                        };
                        const int32_t in = input[core][sample][channel];
                        const int32_t ext = external[channel];
                        record_peak(state.m_external_peak, ext);
                        dry_mix[channel] = gate(0x040u, in) +
                            gate(0x400u, clamp_sample(sum[core][sample][channel])) +
                            gate(0x004u, ext);
                        effect_mix[channel] = gate(0x010u, in) +
                            gate(0x100u, clamp_sample(wet[core][sample][channel])) +
                            gate(0x001u, ext);
                        effect_nonzero |= effect_mix[channel] != 0;
                    }
                    advance_effect(core, effect_mix, effect_return);
                    advance_master_volume(m_cores[core]);
                    for (uint32_t channel = 0u; channel < 2u; ++channel)
                    {
                        const int32_t dry = dry_mix[channel] + scale_mix(effect_return[channel],
                            signed_halfword(m_cores[core].m_master[2u + channel]));
                        if (core == 1u && (dry < -32768 || dry > 32767))
                        {
                            ++m_clipped_samples;
                        }
                        // PS2SDK sets ATTR bit 14 during initialization,
                        // while both current compatibility references leave
                        // its output gate inactive. Preserve the latch but
                        // do not silence playback until hardware semantics
                        // have been established.
                        record_peak(state.m_dry_peak, dry);
                        const int16_t output = clamp_sample(scale_mix(clamp_sample(dry),
                            state.m_master_volume[channel].m_level));
                        record_peak(state.m_output_peak, output);
                        if (core == 0u)
                        {
                            // Tap the core 0 output before core 1 AVOL.
                            m_mixer_rings[1u][channel][offset + sample] = output;
                            external[channel] = scale_mix(output,
                                signed_halfword(m_cores[1].m_master[4u + channel]));
                        }
                        else
                        {
                            pcm[channel] = output;
                        }
                    }
                    if (effect_nonzero)
                    {
                        ++m_effect_input_frames[core];
                    }
                }
                emit_pcm(pcm[0], pcm[1]);
            }
        }
        if (m_mixed_frames % k_input_batch == 0u)
        {
            write_mixer_rings();
        }
        samples -= count;
    }
}

uint32_t c_spu2::voice_irq_cycles_remaining() const
{
    if (!m_clock_active ||
        (((m_cores[0].m_attr & 0x40u) == 0u || m_cores[0].m_irq) &&
         ((m_cores[1].m_attr & 0x40u) == 0u || m_cores[1].m_irq)))
    {
        return 0u;
    }
    constexpr uint32_t block_length = 28u << 12u;
    const uint32_t sample_remainder = sample_phase();
    const uint32_t next_sample = k_cycles_per_sample - sample_remainder;
    uint32_t earliest = 0u;
    for (const s_core& core : m_cores)
    {
        if (core.m_key_mask == 0u)
        {
            continue;
        }
        const uint32_t pmon = static_cast<uint32_t>(core.m_registers[0x180u / 2u]) |
            (static_cast<uint32_t>(core.m_registers[0x182u / 2u]) << 16u);
        for (uint32_t index = 0u, active = core.m_key_mask & 0x00ffffffu;
            active != 0u; ++index, active >>= 1u)
        {
            if ((active & 1u) == 0u)
            {
                continue;
            }
            const s_voice& voice = core.m_voices[index];
            uint32_t samples = 0u;
            if (voice.m_pending || (index != 0u && (pmon & (1u << index)) != 0u))
            {
                // Modulation may alter the next sample's cursor step.
                samples = 1u;
            }
            else
            {
                const uint32_t pitch = std::min<uint32_t>(
                    core.m_registers[index * 8u + 2u], 0x3fffu);
                if (pitch != 0u)
                {
                    samples = (block_length - voice.m_position + pitch - 1u) / pitch;
                }
            }
            // Every candidate below (including effect taps and mixer
            // rings) is evaluated at a sample boundary. Nothing can
            // shorten a deadline already at the very next sample.
            if (samples == 1u)
            {
                return next_sample;
            }
            if (samples != 0u)
            {
                const uint32_t cycles = samples * k_cycles_per_sample - sample_remainder;
                if (earliest == 0u || cycles < earliest)
                {
                    earliest = cycles;
                }
            }
        }
    }
    // Buffered input may also raise IRQ after its DMA has completed.
    for (uint32_t core = 0u; core < 2u; ++core)
    {
        const s_core& state = m_cores[core];
        const uint32_t base = 0x2000u + core * 0x400u;
        for (const s_core& candidate : m_cores)
        {
            if ((candidate.m_attr & 0x40u) == 0u || candidate.m_irq ||
                candidate.m_irqa < base || candidate.m_irqa >= base + k_input_frames)
            {
                continue;
            }
            const uint32_t distance = (candidate.m_irqa - base +
                k_input_frames - state.m_input_read) % k_input_frames;
            if (distance < state.m_input_count)
            {
                const uint32_t cycles = (distance + 1u) * k_cycles_per_sample -
                    sample_remainder;
                if (earliest == 0u || cycles < earliest)
                {
                    earliest = cycles;
                }
            }
        }
    }
    // The reserved 0x0000..0x1fff sound-RAM mixer rings are updated
    // even when no voices are active. Each IRQA within that range is
    // visited once per 512 samples, at the matching 32-frame tick.
    const uint32_t phase = static_cast<uint32_t>(m_mixed_frames % k_input_frames);
    const uint32_t next_tick = (phase / k_input_batch + 1u) * k_input_batch;
    for (const s_core& candidate : m_cores)
    {
        if ((candidate.m_attr & 0x40u) == 0u || candidate.m_irq ||
            candidate.m_irqa >= 0x2000u)
        {
            continue;
        }
        const uint32_t target = (candidate.m_irqa % k_input_frames) /
            k_input_batch * k_input_batch;
        const uint32_t distance = (target + k_input_frames - next_tick) %
            k_input_frames;
        const uint32_t cycles = (next_tick - phase + distance) *
            k_cycles_per_sample - sample_remainder;
        if (earliest == 0u || cycles < earliest)
        {
            earliest = cycles;
        }
    }
    const uint32_t effect = effect_irq_cycles_remaining();
    if (effect != 0u && (earliest == 0u || effect < earliest))
    {
        earliest = effect;
    }
    // This is an opportunity, not a promised IRQ: address comparison
    // and envelope termination are evaluated only by real advancement.
    return earliest;
}

uint32_t c_spu2::advance_cycles(uint32_t cycles)
{
    uint32_t raised = 0u;
    if (m_clock_active)
    {
        const uint64_t before = m_clock_cycles;
        m_clock_cycles += cycles;
        const uint64_t samples = elapsed_samples() - before / k_cycles_per_sample;
        if (samples != 0u)
        {
            const uint32_t prior = irq_latches();
            mix_frames(samples);
            raised = irq_latches() & ~prior;
        }
    }
    return raised;
}

uint32_t c_spu2::set_hi(uint32_t address, uint32_t value)
{
    return ((value & 0x0fu) << 16u) | (address & 0xffffu);
}

uint32_t c_spu2::set_lo(uint32_t address, uint32_t value)
{
    return (address & 0xf0000u) | (value & 0xffffu);
}

bool c_spu2::irq_pending() const
{
    return irq_latches() != 0u;
}

uint32_t c_spu2::irq_latches() const
{
    return (m_cores[0].m_irq ? 1u : 0u) |
           (m_cores[1].m_irq ? 2u : 0u);
}

bool c_spu2::auto_dma_enabled(uint32_t core) const
{
    return core < 2u &&
        (m_cores[core].m_registers[0x1b0u / 2u] &
         (uint16_t{1} << core)) != 0u;
}

bool c_spu2::ram_word(uint32_t address, uint16_t* value) const
{
    if (value == nullptr || address >= k_ram_words)
    {
        return false;
    }
    *value = m_ram[address];
    return true;
}

bool c_spu2::copy_dma(
    uint32_t core, std::vector<uint8_t>& data, bool to_spu
)
{
    if (core >= 2u || data.empty() || (data.size() & 1u) != 0u ||
        data.size() > k_ram_words * 2u ||
        (to_spu && auto_dma_enabled(core)))
    {
        return false;
    }

    m_clock_active = true;
    const uint32_t halfwords = static_cast<uint32_t>(data.size() / 2u);
    s_core& selected = m_cores[core];
    const uint32_t start = selected.m_tsa & (k_ram_words - 1u);
    if (to_spu)
    {
        // The primary reference checks both cores' IRQA against
        // the sound-RAM range written by DMA, including wraparound.
        for (s_core& candidate : m_cores)
        {
            const uint32_t distance =
                (candidate.m_irqa - start) & (k_ram_words - 1u);
            if ((candidate.m_attr & 0x40u) != 0u &&
                !candidate.m_irq && distance < halfwords)
            {
                candidate.m_irq = true;
            }
        }
    }
    for (uint32_t i = 0u; i < halfwords; ++i)
    {
        const uint32_t address = (start + i) & (k_ram_words - 1u);
        if (to_spu)
        {
            m_ram[address] = static_cast<uint16_t>(
                static_cast<uint16_t>(data[2u * i]) |
                (static_cast<uint16_t>(data[2u * i + 1u]) << 8u));
        }
        else
        {
            const uint16_t sample = m_ram[address];
            data[2u * i] = static_cast<uint8_t>(sample);
            data[2u * i + 1u] = static_cast<uint8_t>(sample >> 8u);
        }
    }
    selected.m_tsa = (start + halfwords) & (k_ram_words - 1u);
    return true;
}

bool c_spu2::read(uint32_t address, uint32_t width, uint32_t* value)
{
    if (value == nullptr || (width != 2u && width != 4u) ||
        address < k_base || address - k_base >= k_size ||
        (address & (width - 1u)) != 0u ||
        width > k_size - (address - k_base))
    {
        return false;
    }

    m_clock_active = true;
    const uint32_t offset = address - k_base;
    uint16_t low = 0u;
    if (!read16(offset, &low))
    {
        return false;
    }

    uint16_t high = 0u;
    if (width == 4u && !read16(offset + 2u, &high))
    {
        return false;
    }
    *value = static_cast<uint32_t>(low) |
             (static_cast<uint32_t>(high) << 16u);
    return true;
}

bool c_spu2::write(uint32_t address, uint32_t width, uint32_t value)
{
    if ((width != 2u && width != 4u) ||
        address < k_base || address - k_base >= k_size ||
        (address & (width - 1u)) != 0u ||
        width > k_size - (address - k_base))
    {
        return false;
    }

    m_clock_active = true;
    const uint32_t offset = address - k_base;
    if (!write16(offset, static_cast<uint16_t>(value)))
    {
        return false;
    }
    if (width == 4u &&
        !write16(offset + 2u, static_cast<uint16_t>(value >> 16u)))
    {
        return false;
    }
    return true;
}

bool c_spu2::read_core(
    uint32_t index, uint32_t offset, uint16_t* value
)
{
    s_core& core = m_cores[index];
    if (offset < 0x180u)
    {
        const s_voice& voice = core.m_voices[offset >> 4u];
        const uint32_t field = offset & 0x0fu;
        if (field == 0x0au)
        {
            *value = static_cast<uint16_t>(voice.m_envelope.m_level);
            return true;
        }
        if (field == 0x0cu || field == 0x0eu)
        {
            *value = static_cast<uint16_t>(voice.m_volume[(field - 0x0cu) / 2u].m_level);
            return true;
        }
    }
    else if (offset >= 0x1c0u && offset < 0x2e0u)
    {
        const uint32_t voice = (offset - 0x1c0u) / 12u;
        const uint32_t field = (offset - 0x1c0u) % 12u;
        const s_voice& state = core.m_voices[voice];
        if (field == 4u || field == 6u)
        {
            *value = static_cast<uint16_t>(field == 4u ?
                state.m_loop >> 16u : state.m_loop);
            return true;
        }
        if (field == 8u || field == 10u)
        {
            const uint32_t nax = ((state.m_started && !state.m_pending ?
                state.m_block[0] + (state.m_position >> 14u) :
                voice_address(core, voice, 0u)) + 1u) & (k_ram_words - 1u);
            *value = static_cast<uint16_t>(field == 8u ? nax >> 16u : nax);
            return true;
        }
    }

    switch (offset)
    {
    case 0x19au:
        *value = core.m_attr;
        return true;
    case 0x19cu:
        *value = static_cast<uint16_t>(core.m_irqa >> 16u);
        return true;
    case 0x19eu:
        *value = static_cast<uint16_t>(core.m_irqa);
        return true;
    case 0x1a8u:
        *value = static_cast<uint16_t>(core.m_tsa >> 16u);
        return true;
    case 0x1aau:
        *value = static_cast<uint16_t>(core.m_tsa);
        return true;
    case 0x1b0u:
        *value = core.m_registers[offset / 2u];
        return true;
    case 0x340u:
        *value = static_cast<uint16_t>(core.m_endx);
        return true;
    case 0x342u:
        *value = static_cast<uint16_t>(core.m_endx >> 16u);
        return true;
    case 0x344u:
        // Reference SPU2 STATX waits 16 reads after core enable, then
        // exposes the ordinary powered-up status bit.
        if (core.m_statx_settling != 0u)
        {
            --core.m_statx_settling;
            *value = 0u;
        }
        else
        {
            *value = 0x80u;
        }
        return true;
    default:
        *value = core.m_registers[offset / 2u];
        return true;
    }
}

bool c_spu2::write_core(
    uint32_t index, uint32_t offset, uint16_t value
)
{
    s_core& core = m_cores[index];

    // EEA selects the upper address page only; its low half is fixed.
    if (offset == 0x33eu)
    {
        return true;
    }
    // Preserve register latches and update dynamic voice state.
    core.m_registers[offset / 2u] = value;
    if (offset < 0x180u)
    {
        s_voice& voice = core.m_voices[offset >> 4u];
        const uint32_t field = offset & 0x0fu;
        if ((field == 0u || field == 2u) && (value & 0x8000u) == 0u)
        {
            const uint32_t bits = (static_cast<uint32_t>(value) << 1u) & 0xffffu;
            voice.m_volume[field / 2u].m_level = static_cast<int32_t>(bits) -
                (bits >= 0x8000u ? 0x10000 : 0);
        }
        else if (field == 0x0au)
        {
            // Reference accepts a signed ENVX write. Keep the arithmetic
            // bounded until the next envelope step clamps it.
            voice.m_envelope.m_level = static_cast<int32_t>(value) -
                (value >= 0x8000u ? 0x10000 : 0);
        }
    }
    else if (offset >= 0x1c0u && offset < 0x2e0u)
    {
        const uint32_t index = (offset - 0x1c0u) / 12u;
        const uint32_t field = (offset - 0x1c0u) % 12u;
        s_voice& voice = core.m_voices[index];
        if (field == 4u || field == 6u)
        {
            voice.m_loop = field == 4u ? set_hi(voice.m_loop, value) :
                set_lo(voice.m_loop, value);
            voice.m_loop_written = true;
        }
        else if (field == 8u || field == 10u)
        {
            voice.m_next = field == 8u ? set_hi(voice.m_next, value) :
                set_lo(voice.m_next, value);
        }
    }

    switch (offset)
    {
    case 0x19au:
        if ((value & 0x8000u) != 0u &&
            (core.m_attr & 0x8000u) == 0u)
        {
            core.m_statx_settling = 16u;
        }
        if ((value & 0x80u) != 0u)
        {
            ++core.m_effect_enable_writes;
        }
        core.m_attr = value;
        if ((value & 0x40u) == 0u)
        {
            core.m_irq = false;
        }
        break;
    case 0x19cu:
        core.m_irqa = set_hi(core.m_irqa, value);
        break;
    case 0x19eu:
        core.m_irqa = set_lo(core.m_irqa, value);
        break;
    case 0x1a0u:
    case 0x1a2u:
    {
        const uint32_t bits =
            static_cast<uint32_t>(value) <<
            (offset == 0x1a2u ? 16u : 0u);
        core.m_key_mask |= bits & 0x00ffffffu;
        core.m_endx &= ~bits;
        for (uint32_t index = 0u; index < 24u; ++index)
        {
            if ((bits & (1u << index)) != 0u)
            {
                core.m_voices[index].m_pending = true;
                core.m_voices[index].m_loop_written = false;
            }
        }
        break;
    }
    case 0x1a4u:
    case 0x1a6u:
    {
        const uint32_t bits = static_cast<uint32_t>(value) <<
            (offset == 0x1a6u ? 16u : 0u);
        for (uint32_t index = 0u; index < 24u; ++index)
        {
            const uint32_t bit = 1u << index;
            if ((bits & bit) != 0u)
            {
                s_voice& voice = core.m_voices[index];
                if (voice.m_pending)
                {
                    voice.m_pending = false;
                    if (voice.m_phase == e_voice_phase::off)
                    {
                        core.m_key_mask &= ~bit;
                    }
                }
                else if (voice.m_phase != e_voice_phase::off)
                {
                    voice.m_phase = e_voice_phase::release;
                }
            }
        }
        break;
    }
    case 0x1a8u:
        core.m_tsa = set_hi(core.m_tsa, value);
        break;
    case 0x1aau:
        core.m_tsa = set_lo(core.m_tsa, value);
        break;
    case 0x1acu:
        // This is a real 16-bit sound-RAM data-port write. The SPU2
        // can assert IRQ9 only when its guest-programmed IRQA is hit;
        // it cannot manufacture DMA completion, key-on output, or
        // any periodically ticking mixer event.
        if ((core.m_attr & 0x40u) != 0u &&
            !core.m_irq && core.m_tsa == core.m_irqa)
        {
            core.m_irq = true;
        }
        m_ram[core.m_tsa & (k_ram_words - 1u)] = value;
        core.m_tsa = (core.m_tsa + 1u) & (k_ram_words - 1u);
        break;
    case 0x1b0u:
        if (value == 0u)
        {
            core.m_input_count = 0u;
            core.m_input_read = 0u;
        }
        break;
    case 0x340u:
        core.m_endx &= 0xffff0000u;
        break;
    case 0x342u:
        core.m_endx &= 0x0000ffffu;
        break;
    default:
        break;
    }
    return true;
}

bool c_spu2::read16(uint32_t offset, uint16_t* value)
{
    if (offset < 0x760u)
    {
        return read_core(
            (offset & 0x400u) != 0u ? 1u : 0u,
            offset & 0x3feu, value
        );
    }
    if (offset < 0x7b0u)
    {
        const uint32_t bank = (offset - 0x760u) / 0x28u;
        const uint32_t field = (offset - 0x760u) % 0x28u;
        *value = 0u;
        if (field < 0x10u)
        {
            *value = m_cores[bank].m_master[field / 2u];
        }
        else if (field < 0x14u)
        {
            *value = static_cast<uint16_t>(m_cores[bank].m_master_volume[(field - 0x10u) / 2u].m_level);
        }
        else
        {
            *value = m_cores[bank].m_effect_coefficients[(field - 0x14u) / 2u];
        }
        return true;
    }

    switch (offset)
    {
    case 0x7c0u:
        *value = m_spdif_out;
        return true;
    case 0x7c2u:
        *value = static_cast<uint16_t>(
            (m_cores[0].m_irq ? 4u : 0u) |
            (m_cores[1].m_irq ? 8u : 0u)
        );
        return true;
    case 0x7c6u:
        *value = m_spdif_mode;
        return true;
    case 0x7c8u:
        *value = m_spdif_media;
        return true;
    case 0x7cau:
        // PS2SDK LIBSD writes 8 to this SPDIF control halfword during
        // InitSpdif(). The primary ps2-web-recomp/SDBZ model leaves it
        // unlatched and reads back zero. This is not a ready/status bit.
        *value = 0u;
        return true;
    default:
        // Unknown global status is not automatically a zero/ready bit.
        return false;
    }
}

bool c_spu2::write16(uint32_t offset, uint16_t value)
{
    if (offset < 0x760u)
    {
        return write_core(
            (offset & 0x400u) != 0u ? 1u : 0u,
            offset & 0x3feu, value
        );
    }
    if (offset < 0x7b0u)
    {
        const uint32_t bank = (offset - 0x760u) / 0x28u;
        const uint32_t field = (offset - 0x760u) % 0x28u;
        if (field < 0x10u)
        {
            m_cores[bank].m_master[field / 2u] = value;
            if (field < 4u && (value & 0x8000u) == 0u)
            {
                m_cores[bank].m_master_volume[field / 2u].m_level = signed_halfword(value * 2u);
            }
        }
        else if (field >= 0x14u)
        {
            m_cores[bank].m_effect_coefficients[(field - 0x14u) / 2u] = value;
        }
        return true;
    }

    switch (offset)
    {
    case 0x7c0u:
        m_spdif_out = value;
        return true;
    case 0x7c2u:
        // Reference write_global ignores the IRQ status halfword.
        // In particular SW at 0x7c0 must not fault after writing OUT.
        return true;
    case 0x7c6u:
        m_spdif_mode = value;
        return true;
    case 0x7c8u:
        m_spdif_media = value;
        return true;
    case 0x7cau:
        // PS2SDK LIBSD InitSpdif writes 8 here. The reference
        // ps2-web-recomp/SDBZ write_global() deliberately ignores it.
        // Accept the actual control write without inventing a latch,
        // sound readiness, DMA completion, an interrupt, or an RPC reply.
        return true;
    default:
        return false;
    }
}
} // namespace iop
