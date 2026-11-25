#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ee
{
struct s_pcm_queue_stats
{
    uint64_t produced;
    uint64_t queued;
    uint64_t consumed;
    uint64_t dropped;
    uint64_t underrun;
    uint64_t discarded;
    uint64_t callbacks;
    uint32_t depth;
    uint32_t max_depth;
};

// Single producer (guest thread), single consumer (audio callback).
// Publish samples before advancing the write index; publish the read index
// only after copying samples out. The producer never waits for the consumer.
template<uint32_t k_capacity = 4096u>
class c_ee_pcm_queue
{
    static_assert(k_capacity > 0u && (k_capacity & (k_capacity - 1u)) == 0u);
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "Host audio requires lock-free 64-bit atomics");

public:
    uint32_t push(const int16_t* samples, uint32_t frames)
    {
        const uint64_t write = m_write.load(std::memory_order_relaxed);
        const uint64_t read = m_read.load(std::memory_order_acquire);
        const uint32_t count = std::min(frames,
            k_capacity - static_cast<uint32_t>(write - read));
        const uint32_t slot = static_cast<uint32_t>(write) & (k_capacity - 1u);
        const uint32_t first = std::min(count, k_capacity - slot);
        if (first != 0u)
        {
            std::memcpy(m_samples + 2u * slot, samples,
                sizeof(int16_t) * 2u * first);
        }
        if (count != first)
        {
            std::memcpy(m_samples, samples + 2u * first,
                sizeof(int16_t) * 2u * (count - first));
        }
        m_write.store(write + count, std::memory_order_release);
        m_produced += frames;
        m_dropped += frames - count;
        m_max_depth = std::max(m_max_depth,
            static_cast<uint32_t>(write + count - read));
        return count;
    }

    uint32_t pop(int16_t* samples, uint32_t frames)
    {
        const uint64_t read = m_read.load(std::memory_order_relaxed);
        const uint64_t write = m_write.load(std::memory_order_acquire);
        const uint32_t count = static_cast<uint32_t>(
            std::min<uint64_t>(frames, write - read));
        const uint32_t slot = static_cast<uint32_t>(read) & (k_capacity - 1u);
        const uint32_t first = std::min(count, k_capacity - slot);
        if (first != 0u)
        {
            std::memcpy(samples, m_samples + 2u * slot,
                sizeof(int16_t) * 2u * first);
        }
        if (count != first)
        {
            std::memcpy(samples + 2u * first, m_samples,
                sizeof(int16_t) * 2u * (count - first));
        }
        if (frames != count)
        {
            std::memset(samples + 2u * count, 0,
                sizeof(int16_t) * 2u * (frames - count));
        }
        m_read.store(read + count, std::memory_order_release);
        m_consumed += count;
        m_underrun += frames - count;
        ++m_callbacks;
        return count;
    }

    // Control thread only, with producer and consumer both quiescent.
    void discard_pending()
    {
        const uint64_t write = m_write.load(std::memory_order_relaxed);
        const uint64_t read = m_read.load(std::memory_order_relaxed);
        m_discarded += write - read;
        m_read.store(write, std::memory_order_relaxed);
    }

    // Only inspect after the audio callback has stopped or while paused.
    s_pcm_queue_stats stats() const
    {
        return {m_produced, m_produced - m_dropped, m_consumed,
            m_dropped, m_underrun, m_discarded, m_callbacks,
            static_cast<uint32_t>(m_write.load(std::memory_order_relaxed) -
                m_read.load(std::memory_order_relaxed)), m_max_depth};
    }

private:
    int16_t m_samples[k_capacity * 2u]{};
    alignas(64) std::atomic<uint64_t> m_write{0u};
    uint64_t m_produced = 0u;
    uint64_t m_dropped = 0u;
    uint32_t m_max_depth = 0u;
    alignas(64) std::atomic<uint64_t> m_read{0u};
    uint64_t m_consumed = 0u;
    uint64_t m_underrun = 0u;
    uint64_t m_callbacks = 0u;
    uint64_t m_discarded = 0u;
};
}
