#include "ee_host_audio.h"

#include "ee_pcm_queue.h"
#include "iop_hw.h"
#include "raudio.h"

#include <atomic>
#include <cstdio>
#include <cstring>

extern "C" unsigned int raudio_device_sample_rate(void);
extern "C" unsigned int raudio_device_channels(void);

namespace ee
{
struct c_ee_host_audio::s_impl
{
    static std::atomic<s_impl*> m_active;
    c_ee_pcm_queue<> m_queue;
    iop::c_hw* m_hardware = nullptr;
    AudioStream m_stream{};
    bool m_ready = false;
    bool m_paused = false;
    uint32_t m_device_rate = 0u;
    uint32_t m_device_channels = 0u;
    uint32_t m_paused_depth = 0u;

    static void sink(void* user, const int16_t* pcm, uint32_t frames)
    {
        static_cast<s_impl*>(user)->m_queue.push(pcm, frames);
    }

    static void callback(void* data, unsigned int frames)
    {
        auto* samples = static_cast<int16_t*>(data);
        s_impl* active = m_active.load(std::memory_order_acquire);
        if (active != nullptr)
        {
            active->m_queue.pop(samples, frames);
        }
        else
        {
            std::memset(samples, 0, sizeof(int16_t) * 2u * frames);
        }
    }
};

std::atomic<c_ee_host_audio::s_impl*> c_ee_host_audio::s_impl::m_active{nullptr};

c_ee_host_audio::c_ee_host_audio() = default;

c_ee_host_audio::~c_ee_host_audio()
{
    shutdown();
}

void c_ee_host_audio::start(iop::c_hw* hardware)
{
    if (m_impl != nullptr)
    {
        return;
    }
    if (hardware == nullptr)
    {
        m_status = "no_iop";
        return;
    }
    m_impl = std::make_unique<s_impl>();
    s_impl* expected = nullptr;
    if (!s_impl::m_active.compare_exchange_strong(expected, m_impl.get(),
            std::memory_order_acq_rel))
    {
        m_status = "already_active";
        return;
    }

    InitAudioDevice();
    if (!IsAudioDeviceReady())
    {
        s_impl::m_active.store(nullptr, std::memory_order_release);
        m_status = "device_failed";
        return;
    }
    m_impl->m_stream = LoadAudioStream(48000u, 16u, 2u);
    if (!IsAudioStreamReady(m_impl->m_stream))
    {
        CloseAudioDevice();
        s_impl::m_active.store(nullptr, std::memory_order_release);
        m_status = "stream_failed";
        return;
    }

    m_impl->m_device_rate = raudio_device_sample_rate();
    m_impl->m_device_channels = raudio_device_channels();
    m_impl->m_hardware = hardware;
    SetAudioStreamCallback(m_impl->m_stream, &s_impl::callback);
    PlayAudioStream(m_impl->m_stream);
    hardware->bind_spu2_pcm_sink(m_impl.get(), &s_impl::sink);
    m_impl->m_ready = true;
    m_status = "ready";
}

void c_ee_host_audio::pause()
{
    if (m_impl != nullptr && m_impl->m_ready && !m_impl->m_paused)
    {
        m_impl->m_hardware->bind_spu2_pcm_sink(nullptr, nullptr);
        // The vendored stream control synchronizes with the mixing thread.
        // No queue storage is touched until any in-flight callback finishes.
        PauseAudioStream(m_impl->m_stream);
        m_impl->m_paused = true;
        m_impl->m_paused_depth = m_impl->m_queue.stats().depth;
        m_impl->m_queue.discard_pending();
    }
}

void c_ee_host_audio::resume()
{
    if (m_impl != nullptr && m_impl->m_ready && m_impl->m_paused)
    {
        m_impl->m_hardware->bind_spu2_pcm_sink(m_impl.get(), &s_impl::sink);
        ResumeAudioStream(m_impl->m_stream);
        m_impl->m_paused = false;
    }
}

void c_ee_host_audio::shutdown()
{
    if (m_impl != nullptr && m_impl->m_ready)
    {
        pause();
        UnloadAudioStream(m_impl->m_stream);
        CloseAudioDevice();
        s_impl::m_active.store(nullptr, std::memory_order_release);
        m_impl->m_ready = false;
    }
}

void c_ee_host_audio::report() const
{
    // Caller has shut down the device, so all queue counters are quiescent.
    const s_pcm_queue_stats stats = m_impl != nullptr
        ? m_impl->m_queue.stats() : s_pcm_queue_stats{};
    std::printf(
        "IOP host audio: status=%s produced=%llu queued=%llu consumed=%llu "
        "dropped=%llu underrun=%llu discarded=%llu callbacks=%llu depth=%u "
        "max_depth=%u shutdown_depth=%u stream_rate=48000 stream_channels=2 "
        "stream_bits=16 device_rate=%u device_channels=%u\n",
        m_status,
        static_cast<unsigned long long>(stats.produced),
        static_cast<unsigned long long>(stats.queued),
        static_cast<unsigned long long>(stats.consumed),
        static_cast<unsigned long long>(stats.dropped),
        static_cast<unsigned long long>(stats.underrun),
        static_cast<unsigned long long>(stats.discarded),
        static_cast<unsigned long long>(stats.callbacks),
        stats.depth, stats.max_depth,
        m_impl != nullptr ? m_impl->m_paused_depth : 0u,
        m_impl != nullptr ? m_impl->m_device_rate : 0u,
        m_impl != nullptr ? m_impl->m_device_channels : 0u);
}
}
