#include "ee_pcm_queue.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

#define CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            std::fprintf(stderr, "PCM queue check failed at line %d\n", __LINE__); \
            return 1; \
        } \
    } while (false)

int main()
{
    ee::c_ee_pcm_queue<4u> queue;
    const int16_t first[] = {1, -1, 2, -2, 3, -3, 4, -4, 5, -5};
    int16_t out[12]{};
    CHECK(queue.pop(out, 2u) == 0u);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0);
    CHECK(queue.push(first, 5u) == 4u);
    CHECK(queue.push(first, 1u) == 0u);
    CHECK(queue.pop(out, 3u) == 3u);
    for (int i = 0; i < 6; ++i)
    {
        CHECK(out[i] == first[i]);
    }
    CHECK(queue.push(first, 3u) == 3u);
    CHECK(queue.pop(out, 6u) == 4u);
    CHECK(out[0] == 4 && out[1] == -4);
    for (int i = 0; i < 6; ++i)
    {
        CHECK(out[i + 2] == first[i]);
    }
    CHECK(out[8] == 0 && out[9] == 0 && out[10] == 0 && out[11] == 0);
    CHECK(queue.push(first, 2u) == 2u);
    queue.discard_pending();
    CHECK(queue.pop(out, 1u) == 0u);
    CHECK(queue.push(first, 1u) == 1u);
    CHECK(queue.pop(out, 1u) == 1u);
    CHECK(out[0] == 1 && out[1] == -1);
    const auto stats = queue.stats();
    CHECK(stats.produced == 12u && stats.queued == 10u);
    CHECK(stats.consumed == 8u && stats.dropped == 2u);
    CHECK(stats.discarded == 2u && stats.underrun == 5u);
    CHECK(stats.depth == 0u && stats.max_depth == 4u && stats.callbacks == 5u);

    ee::c_ee_pcm_queue<64u> concurrent;
    constexpr uint32_t k_frames = 100000u;
    std::atomic<bool> failed{false};
    std::thread producer([&]()
    {
        for (uint32_t i = 0u; i < k_frames; ++i)
        {
            const auto sample = static_cast<int16_t>(i % 30000u);
            const int16_t pair[] = {sample, static_cast<int16_t>(-sample)};
            while (concurrent.push(pair, 1u) == 0u)
            {
                std::this_thread::yield();
            }
        }
    });
    for (uint32_t i = 0u; i < k_frames;)
    {
        int16_t pair[2]{};
        if (concurrent.pop(pair, 1u) == 0u)
        {
            if (pair[0] != 0 || pair[1] != 0)
            {
                failed.store(true);
            }
            std::this_thread::yield();
            continue;
        }
        const auto sample = static_cast<int16_t>(i % 30000u);
        if (pair[0] != sample || pair[1] != -sample)
        {
            failed.store(true);
        }
        ++i;
    }
    producer.join();
    const auto concurrent_stats = concurrent.stats();
    CHECK(!failed.load());
    CHECK(concurrent_stats.queued == k_frames);
    CHECK(concurrent_stats.consumed == k_frames);
    CHECK(concurrent_stats.produced ==
        concurrent_stats.queued + concurrent_stats.dropped);
    CHECK(concurrent_stats.depth == 0u && concurrent_stats.max_depth <= 64u);
    return 0;
}
