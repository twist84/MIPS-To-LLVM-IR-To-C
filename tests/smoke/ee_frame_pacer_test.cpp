#include "ee_frame_pacer.h"

#include <cstdio>

#define CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            std::fprintf(stderr, "Frame pacer check failed at line %d\n", __LINE__); \
            return 1; \
        } \
    } while (false)

int main()
{
    using t_clock = ee::c_ee_frame_pacer::t_clock;
    for (bool enabled : {false, true})
    {
        ee::c_ee_frame_pacer pacer(enabled);
        pacer.wait_until(t_clock::now() - std::chrono::milliseconds(1));
        CHECK(pacer.calls() == 0u);
        const auto deadline = t_clock::now() + std::chrono::milliseconds(20);
        pacer.wait_until(deadline);
        CHECK(t_clock::now() >= deadline);
        // A preempted caller may enter after the deadline. No timing
        // upper bound is imposed on a shared CI worker.
        if (pacer.calls() != 0u)
        {
            CHECK(pacer.calls() == 1u);
            CHECK(pacer.high_res_calls() + pacer.fallback_calls() >= 1u);
            CHECK(pacer.requested_ns() > 0u);
            CHECK(pacer.waited_ns() >= pacer.requested_ns());
            CHECK(pacer.max_overshoot_ns() == pacer.overshoot_ns());
        }
        if (!enabled)
        {
            CHECK(pacer.high_res_calls() == 0u);
        }
    }
    return 0;
}
