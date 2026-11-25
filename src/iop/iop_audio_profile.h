#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>

namespace iop
{
// Thread-confined counters. Read host callback counters only after shutdown.
struct s_audio_profile_counter
{
    uint64_t m_calls = 0u;
    uint64_t m_ns = 0u;
    uint64_t m_max_ns = 0u;

    void report(const char* stage) const
    {
        std::printf("Audio profile: stage=%s calls=%llu ns=%llu max_ns=%llu\n",
            stage, static_cast<unsigned long long>(m_calls),
            static_cast<unsigned long long>(m_ns),
            static_cast<unsigned long long>(m_max_ns));
    }
};

class c_audio_profile_scope
{
public:
    c_audio_profile_scope(bool enabled, s_audio_profile_counter& counter)
        : m_counter(enabled ? &counter : nullptr)
    {
        if (m_counter != nullptr)
        {
            m_start = t_clock::now();
        }
    }

    ~c_audio_profile_scope()
    {
        if (m_counter != nullptr)
        {
            const uint64_t ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    t_clock::now() - m_start).count());
            ++m_counter->m_calls;
            m_counter->m_ns += ns;
            if (ns > m_counter->m_max_ns)
            {
                m_counter->m_max_ns = ns;
            }
        }
    }

    c_audio_profile_scope(const c_audio_profile_scope&) = delete;
    c_audio_profile_scope& operator=(const c_audio_profile_scope&) = delete;

private:
    using t_clock = std::chrono::steady_clock;
    s_audio_profile_counter* m_counter;
    t_clock::time_point m_start{};
};
}
