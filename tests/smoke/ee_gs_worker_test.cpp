#include "ee_gs_worker.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>

namespace
{
struct s_capture
{
    std::vector<uint64_t> m_words;
    std::vector<uint8_t> m_images;
    bool m_valid = true;
    bool m_throw = false;
};

void consume(void* user, const ee::s_ee_gif_event* events, size_t count,
    const uint8_t* data, uint32_t qwc)
{
    auto& capture = *static_cast<s_capture*>(user);
    if (capture.m_throw)
    {
        throw std::runtime_error("host renderer failure");
    }
    for (size_t i = 0u; i < count; ++i)
    {
        const auto& e = events[i];
        if (e.m_type == ee::e_ee_gif_event_type::image)
        {
            if (e.m_byte_offset > static_cast<size_t>(qwc) * 16u ||
                e.m_qwc > (static_cast<size_t>(qwc) * 16u - e.m_byte_offset) / 16u)
            {
                capture.m_valid = false;
                return;
            }
            const auto* start = data + e.m_byte_offset;
            capture.m_images.insert(capture.m_images.end(), start,
                start + e.m_qwc * 16u);
        }
        else
        {
            capture.m_words.push_back(e.m_word0);
        }
    }
}

bool check(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "GS worker: %s\n", message);
    }
    return condition;
}
}

int main()
{
    using result = ee::c_ee_gs_worker::e_submit;
    s_capture capture{};
    constexpr size_t batch_bytes = 3u * sizeof(ee::s_ee_gif_event) + 32u;
    ee::c_ee_gs_worker worker(&capture, consume, batch_bytes, 1u);
    std::array<ee::s_ee_gif_event, 3> events{};
    events[0].m_type = ee::e_ee_gif_event_type::packed;
    events[1].m_type = events[2].m_type = ee::e_ee_gif_event_type::image;
    events[1].m_byte_offset = 32u;
    events[2].m_byte_offset = 0u;
    events[1].m_qwc = events[2].m_qwc = 1u;
    std::array<uint8_t, 64> bytes{};
    for (uint32_t index = 0u; index < 50u; ++index)
    {
        events[0].m_word0 = index;
        bytes.fill(0x55u);
        std::fill_n(bytes.begin(), 16u, static_cast<uint8_t>(index));
        std::fill_n(bytes.begin() + 32u, 16u, static_cast<uint8_t>(index + 50u));
        if (!check(worker.submit(events.data(), events.size(), bytes.data(), 4u) ==
                result::accepted, "owned batch submit"))
        {
            return 1;
        }
        // Reuse parser scratch immediately; the worker must not borrow it.
        events[0].m_word0 = 0xffffu;
        bytes.fill(0xffu);
    }
    if (!check(worker.drain() && capture.m_valid &&
            capture.m_words.size() == 50u && capture.m_images.size() == 1600u,
            "ordered worker drain"))
    {
        return 1;
    }
    for (uint32_t index = 0u; index < 50u; ++index)
    {
        if (!check(capture.m_words[index] == index, "register order"))
        {
            return 1;
        }
        for (uint32_t lane = 0u; lane < 16u; ++lane)
        {
            if (!check(capture.m_images[index * 32u + lane] == index + 50u &&
                    capture.m_images[index * 32u + 16u + lane] == index,
                    "compacted image provenance"))
            {
                return 1;
            }
        }
    }
    const auto stats = worker.statistics();
    if (!check(stats.m_submitted == 50u && stats.m_completed == 50u &&
            stats.m_peak_jobs == 1u && stats.m_peak_bytes == batch_bytes &&
            stats.m_image_bytes == 1600u, "bounded active storage"))
    {
        return 1;
    }
    events[1].m_byte_offset = 64u;
    if (!check(worker.submit(events.data(), events.size(), bytes.data(), 4u) ==
            result::invalid, "out-of-range image accepted"))
    {
        return 1;
    }
    events[1].m_byte_offset = 32u;
    events[2].m_qwc = 2u;
    if (!check(worker.submit(events.data(), events.size(), bytes.data(), 4u) ==
            result::too_large, "oversized batch was queued"))
    {
        return 1;
    }
    s_capture failed{};
    failed.m_throw = true;
    ee::c_ee_gs_worker failing(&failed, consume);
    if (!check(failing.submit(events.data(), 1u, nullptr, 0u) == result::accepted &&
            !failing.drain() && !failing.healthy() &&
            failing.submit(events.data(), 1u, nullptr, 0u) == result::failed,
            "renderer exception was not surfaced"))
    {
        return 1;
    }
    std::puts("GS ordered worker: PASS");
    return 0;
}
