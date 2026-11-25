#pragma once

#include "ee_memory.h"

#include <cstdint>
#include <cstdio>

namespace ee
{
// Original USA SLUS_214.42:
// 0x00109b98 JALs the pad-read helper at 0x001884d0, passing sp+0x40.
// At 0x00109ba0 the original caller examines that buffer and later
// combines bytes +2/+3 into a raw active-low button word.
// This observer samples that guest RAM only at the authentic return
// continuation. It cannot establish which original branch was taken.
class c_ee_sdbz_pad_return_probe
{
public:
    static constexpr uint32_t k_return_pc = 0x00109ba0u;

    struct s_sample
    {
        bool m_valid = false;
        uint64_t m_dispatch = 0u;
        uint32_t m_buffer_address = 0u;
        uint32_t m_return_value = 0u;
        uint8_t m_bytes[4]{};
        bool m_analog_valid = false;
        uint8_t m_analog[4]{};
        uint16_t m_raw_buttons = 0u;
        uint16_t m_pressed_mask = 0u;
    };

    bool observe(uint32_t pc, uint64_t dispatch,
                 uint64_t stack_pointer, uint64_t return_value,
                 ee_memory* memory)
    {
        if (pc != k_return_pc)
        {
            return false;
        }

        ++m_return_entries;
        if (memory == nullptr)
        {
            return false;
        }

        // The original guest stack can be cached or uncached aliased.
        // Never read MMIO: sample only a four-byte backed RAM range.
        const uint32_t stack_address =
            static_cast<uint32_t>(stack_pointer) & 0x1fffffffu;
        if (stack_address > 0xffffffffu - 0x43u)
        {
            return false;
        }
        const uint32_t address = stack_address + 0x40u;
        if (!memory->contains(address, 4u))
        {
            return false;
        }

        s_sample sample{};
        sample.m_valid = true;
        sample.m_dispatch = dispatch;
        sample.m_buffer_address = address;
        sample.m_return_value = static_cast<uint32_t>(return_value);
        for (uint32_t index = 0u; index < 4u; ++index)
        {
            sample.m_bytes[index] = memory->read8(address + index);
        }
        sample.m_raw_buttons = static_cast<uint16_t>(
            (static_cast<uint32_t>(sample.m_bytes[2u]) << 8u) |
            sample.m_bytes[3u]);
        sample.m_pressed_mask =
            static_cast<uint16_t>(~sample.m_raw_buttons);

        ++m_ram_samples;
        // The caller checks a nonzero helper return and byte +0 == 0
        // before interpreting subsequent reply bytes. An all-zero,
        // rejected or otherwise unverified reply is not a button press.
        if (sample.m_return_value >= 4u && sample.m_bytes[0u] == 0u)
        {
            ++m_caller_candidate_samples;
            // Original 0x00109c50..0x00109cf0 consumes reply bytes
            // +6,+7,+4,+5 after subtracting 0x80 and applying ±36.
            if (sample.m_return_value >= 8u &&
                (sample.m_bytes[1] & 0xf0u) == 0x70u &&
                address <= 0xfffffff8u && memory->contains(address, 8u))
            {
                sample.m_analog_valid = true;
                bool outside = false;
                for (uint32_t index = 0u; index < 4u; ++index)
                {
                    sample.m_analog[index] = memory->read8(address + 4u + index);
                    const int axis = static_cast<int>(sample.m_analog[index]) - 128;
                    outside = outside || axis < -36 || axis > 36;
                }
                ++m_analog_samples;
                if (outside)
                {
                    ++m_analog_outside_deadzone;
                    if (!m_first_analog_outside.m_valid)
                    {
                        m_first_analog_outside = sample;
                    }
                }
            }
            // SIO2 sends Cross in bit 6 of the second button byte.
            // PADMAN preserves byte order. SLUS_214.42 shifts the
            // first byte left eight, so Cross is 0x0040 in its word.
            if ((sample.m_pressed_mask & 0x0040u) != 0u)
            {
                ++m_cross_bit_low_candidates;
                if (!m_first_cross.m_valid)
                {
                    m_first_cross = sample;
                }
            }
        }
        if (!m_first.m_valid)
        {
            m_first = sample;
        }
        m_last = sample;
        return true;
    }

    const s_sample& last_sample() const
    {
        return m_last;
    }

    void write_json(FILE* output) const
    {
        if (output == nullptr)
        {
            return;
        }
        std::fprintf(output,
            "{\"schema\":\"ee-sdbz-pad-return-observation-v2\","
            "\"site_pc\":%u,\"return_entries\":%llu,"
            "\"ram_samples\":%llu,\"caller_candidate_samples\":%llu,"
            "\"cross_bit_low_candidates\":%llu,"
            "\"analog_samples\":%llu,\"analog_outside_deadzone\":%llu,"
            "\"first\":",
            k_return_pc,
            static_cast<unsigned long long>(m_return_entries),
            static_cast<unsigned long long>(m_ram_samples),
            static_cast<unsigned long long>(m_caller_candidate_samples),
            static_cast<unsigned long long>(m_cross_bit_low_candidates),
            static_cast<unsigned long long>(m_analog_samples),
            static_cast<unsigned long long>(m_analog_outside_deadzone));
        write_sample(output, m_first);
        std::fputs(",\"first_cross_candidate\":", output);
        write_sample(output, m_first_cross);
        std::fputs(",\"last\":", output);
        write_sample(output, m_last);
        std::fputs(",\"first_analog_outside_deadzone\":", output);
        write_sample(output, m_first_analog_outside);
        std::fputs(",\"limitations\":["
            "\"return_site_only_not_full_guest_instruction_trace\","
            "\"candidate_mask_does_not_prove_input_branch\","
            "\"cannot_attribute_input_without_matched_neutral_replay\"]}",
            output);
    }

private:
    static void write_sample(FILE* output, const s_sample& sample)
    {
        if (!sample.m_valid)
        {
            std::fputs("null", output);
            return;
        }
        std::fprintf(output,
            "{\"dispatch\":%llu,\"buffer_address\":%u,"
            "\"return_value\":%u,\"bytes\":[%u,%u,%u,%u],"
            "\"raw_buttons\":%u,\"pressed_mask\":%u,"
            "\"analog_valid\":%s,\"analog_bytes\":[%u,%u,%u,%u]}",
            static_cast<unsigned long long>(sample.m_dispatch),
            sample.m_buffer_address, sample.m_return_value,
            sample.m_bytes[0u], sample.m_bytes[1u],
            sample.m_bytes[2u], sample.m_bytes[3u],
            sample.m_raw_buttons, sample.m_pressed_mask,
            sample.m_analog_valid ? "true" : "false",
            sample.m_analog[0], sample.m_analog[1],
            sample.m_analog[2], sample.m_analog[3]);
    }

    uint64_t m_return_entries = 0u;
    uint64_t m_ram_samples = 0u;
    uint64_t m_caller_candidate_samples = 0u;
    uint64_t m_cross_bit_low_candidates = 0u;
    uint64_t m_analog_samples = 0u;
    uint64_t m_analog_outside_deadzone = 0u;
    s_sample m_first_analog_outside{};
    s_sample m_first{};
    s_sample m_first_cross{};
    s_sample m_last{};
};

} // namespace ee
