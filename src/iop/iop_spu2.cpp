#include "iop_spu2.h"

#include <algorithm>

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
        m_profile_sink.report("spu2_pcm_sink");
        m_profile_silent_rings.report("spu2_silent_ring_revolutions");
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

int16_t clamp_sample(int32_t value)
{
    return static_cast<int16_t>(std::clamp(value, -32768, 32767));
}

int32_t master_gain(uint16_t value)
{
    // Same fixed master gain rule as the SDBZ reference. Master sweep
    // mode still uses its reference full-scale fallback.
    return (value & 0x8000u) != 0u ? 0x7fff : signed_halfword(value * 2u);
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
        if (core.m_key_mask == 0u)
        {
            continue;
        }
        const auto voice_mask = [&core](uint32_t offset)
        {
            return static_cast<uint32_t>(core.m_registers[offset / 2u]) |
                (static_cast<uint32_t>(core.m_registers[offset / 2u + 1u] & 0xffu) << 16u);
        };
        const uint32_t dry_mask[2] = {voice_mask(0x188u), voice_mask(0x190u)};
        const uint32_t wet_mask[2] = {voice_mask(0x18cu), voice_mask(0x194u)};
        for (uint32_t index = 0u; index < 24u; ++index)
        {
            const uint32_t bit = 1u << index;
            if ((core.m_key_mask & bit) == 0u)
            {
                continue;
            }
            s_voice& voice = core.m_voices[index];
            if (voice.m_pending)
            {
                start_voice(core, index);
            }
            const uint16_t* regs = &core.m_registers[index * 8u];
            const uint32_t pitch = std::min<uint32_t>(regs[2], 0x3fffu);
            // Envelope release may stop on any sample, so retire the
            // cursor and ADSR in reference order, not an aggregate pitch
            // advance followed by a delayed envelope update.
            for (uint32_t sample = 0u; sample < samples; ++sample)
            {
                const uint32_t index_in_block = voice.m_position >> 12u;
                const uint32_t fraction = voice.m_position & 0xfffu;
                const int32_t first = voice.m_samples[0][index_in_block];
                const int32_t second = index_in_block == 27u ?
                    voice.m_samples[1][0] : voice.m_samples[0][index_in_block + 1u];
                const int32_t interpolated = floor_shift(
                    first * static_cast<int32_t>(0x1000u - fraction) +
                    second * static_cast<int32_t>(fraction), 12u);
                voice.m_position += pitch;
                if (voice.m_position >= block_length)
                {
                    voice.m_position -= block_length;
                    const uint8_t flags = voice.m_flags[0];
                    voice.m_block[0] = voice.m_block[1];
                    voice.m_flags[0] = voice.m_flags[1];
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
                    break;
                }
                const int32_t enveloped = floor_shift(
                    interpolated * voice.m_envelope.m_level, 15u);
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
                sum[core][sample][channel] += floor_shift(
                    static_cast<int32_t>(state.m_input[slot][channel]) *
                    signed_halfword(state.m_master[6u + channel]), 15u);
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
        const bool silent = (m_cores[0].m_key_mask | m_cores[1].m_key_mask |
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
            m_mixed_frames += frames;
            m_mixer_ring_batches += frames / k_input_batch;
            samples -= frames;
            continue;
        }
        const uint32_t count = static_cast<uint32_t>(
            std::min<uint64_t>(samples, k_input_batch - offset));
        for (auto& core : m_mixer_rings)
        {
            for (auto& ring : core)
            {
                std::fill_n(ring + offset, count, int16_t{0});
            }
        }
        if (silent && m_pcm_sink == nullptr)
        {
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
                        const int32_t dry = gate(0x040u, in) +
                            gate(0x400u, clamp_sample(sum[core][sample][channel])) +
                            gate(0x004u, ext);
                        const int32_t effect = gate(0x010u, in) +
                            gate(0x100u, clamp_sample(wet[core][sample][channel])) +
                            gate(0x001u, ext);
                        effect_nonzero |= effect != 0;
                        // Reverb return is still silent. The routed effect
                        // input is ready for the work-area/filter stage.
                        if (core == 1u && (dry < -32768 || dry > 32767))
                        {
                            ++m_clipped_samples;
                        }
                        const int16_t output = clamp_sample(scale_mix(clamp_sample(dry),
                            master_gain(m_cores[core].m_master[channel])));
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
    uint32_t earliest = 0u;
    for (const s_core& core : m_cores)
    {
        if (core.m_key_mask == 0u)
        {
            continue;
        }
        for (uint32_t index = 0u; index < 24u; ++index)
        {
            if ((core.m_key_mask & (1u << index)) == 0u)
            {
                continue;
            }
            const s_voice& voice = core.m_voices[index];
            uint32_t samples = 0u;
            if (voice.m_pending)
            {
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
            if (samples != 0u)
            {
                const uint32_t cycles = samples * k_cycles_per_sample - sample_phase();
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
                    sample_phase();
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
            k_cycles_per_sample - sample_phase();
        if (earliest == 0u || cycles < earliest)
        {
            earliest = cycles;
        }
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
        else if (field >= 0x14u)
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
