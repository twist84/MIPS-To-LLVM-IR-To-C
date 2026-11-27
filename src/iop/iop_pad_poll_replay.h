#pragma once

#include "iop_hw.h"

#include <algorithm>
#include <cstdio>
#include <istream>
#include <sstream>
#include <string>
#include <vector>

namespace iop
{
// Host-only fixture stream. The sequence is global across IOP resets;
// each sample also retains the guest's reset-local poll number and shape.
class c_pad_poll_replay
{
public:
    static constexpr size_t k_max_samples = 1000000u;

    void record(FILE* output)
    {
        m_record = output;
    }

    bool load(std::istream& stream)
    {
        std::vector<c_hw::s_pad0_poll> samples;
        std::string line;
        bool complete = false;
        while (std::getline(stream, line))
        {
            const std::string prefix = "PAD0 input: ";
            const std::string footer = "PAD0 input summary: record ";
            if (line.compare(0u, prefix.size(), prefix) == 0)
            {
                uint64_t values[10]{};
                std::istringstream input(line.substr(prefix.size()));
                for (auto& value : values)
                {
                    if (!(input >> value))
                    {
                        return false;
                    }
                }
                std::string extra;
                if ((input >> extra) || complete ||
                    samples.size() >= k_max_samples ||
                    values[0u] != samples.size() + 1u ||
                    values[1u] == 0u || values[1u] > UINT32_MAX ||
                    values[2u] > 0x1ffu ||
                    values[3u] == 0u || values[3u] > 511u ||
                    values[4u] == 0u || values[4u] > 511u ||
                    values[5u] > UINT16_MAX)
                {
                    return false;
                }
                c_hw::s_pad0_poll sample{};
                sample.m_poll = static_cast<uint32_t>(values[1u]);
                sample.m_mode = static_cast<uint32_t>(values[2u]);
                sample.m_input_size = static_cast<uint32_t>(values[3u]);
                sample.m_output_size = static_cast<uint32_t>(values[4u]);
                sample.m_buttons = static_cast<uint16_t>(values[5u]);
                for (uint32_t index = 0u; index < 4u; ++index)
                {
                    if (values[6u + index] > UINT8_MAX)
                    {
                        return false;
                    }
                    sample.m_axes[index] = static_cast<uint8_t>(values[6u + index]);
                }
                samples.push_back(sample);
            }
            else if (line.compare(0u, footer.size(), footer) == 0)
            {
                uint64_t count = 0u;
                std::string extra;
                std::istringstream input(line.substr(footer.size()));
                if (complete || !(input >> count) || (input >> extra) ||
                    count != samples.size())
                {
                    return false;
                }
                complete = true;
            }
        }
        if (stream.bad() || !complete || samples.empty() ||
            m_record != nullptr || m_consumed != 0u || m_replay)
        {
            return false;
        }
        m_samples.swap(samples);
        m_replay = true;
        return true;
    }

    bool enabled() const { return m_record != nullptr || m_replay; }
    bool failed() const { return m_failed; }
    bool finished() const
    {
        return !m_failed && (!m_replay || m_consumed == m_samples.size());
    }
    size_t consumed() const { return m_consumed; }
    size_t total() const { return m_samples.size(); }

    void summary(FILE* output) const
    {
        if (m_record != nullptr && !m_failed)
        {
            std::fprintf(output, "PAD0 input summary: record %zu\n", m_consumed);
        }
        else if (m_replay)
        {
            std::fprintf(output,
                "PAD0 input summary: replay consumed=%zu total=%zu valid=%u\n",
                m_consumed, m_samples.size(), finished() ? 1u : 0u);
        }
    }

    static bool poll(void* user, c_hw::s_pad0_poll& sample)
    {
        auto& self = *static_cast<c_pad_poll_replay*>(user);
        if (self.m_failed)
        {
            return false;
        }
        if (self.m_replay)
        {
            if (self.m_consumed >= self.m_samples.size())
            {
                self.m_failed = true;
                return false;
            }
            const auto& expected = self.m_samples[self.m_consumed];
            if (sample.m_poll != expected.m_poll || sample.m_mode != expected.m_mode ||
                sample.m_input_size != expected.m_input_size ||
                sample.m_output_size != expected.m_output_size)
            {
                self.m_failed = true;
                return false;
            }
            sample.m_buttons = expected.m_buttons;
            std::copy_n(expected.m_axes, 4u, sample.m_axes);
        }
        else if (self.m_record != nullptr)
        {
            if (self.m_consumed >= k_max_samples ||
                std::fprintf(self.m_record, "PAD0 input: %zu %u %u %u %u %u %u %u %u %u\n",
                    self.m_consumed + 1u, sample.m_poll, sample.m_mode,
                    sample.m_input_size, sample.m_output_size,
                    static_cast<unsigned>(sample.m_buttons),
                    static_cast<unsigned>(sample.m_axes[0u]),
                    static_cast<unsigned>(sample.m_axes[1u]),
                    static_cast<unsigned>(sample.m_axes[2u]),
                    static_cast<unsigned>(sample.m_axes[3u])) < 0)
            {
                self.m_failed = true;
                return false;
            }
        }
        else
        {
            return false;
        }
        ++self.m_consumed;
        return true;
    }

private:
    FILE* m_record = nullptr;
    std::vector<c_hw::s_pad0_poll> m_samples;
    size_t m_consumed = 0u;
    bool m_replay = false;
    bool m_failed = false;
};
} // namespace iop
