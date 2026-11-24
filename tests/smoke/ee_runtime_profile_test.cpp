#include "ee_runtime_profile.h"

#include <cstdio>
#include <string>

int main()
{
    using ee::e_runtime_profile_id;
    ee::runtime_profile_begin(false);
    {
        ee::c_runtime_profile_scope scope(e_runtime_profile_id::ee_native_boundary);
    }
    if (ee::g_runtime_profile.m_counters[0].m_calls != 0u)
    {
        return 1;
    }

    ee::runtime_profile_begin(true);
    uint32_t steps = 0u;
    {
        ee::c_runtime_profile_scope root(e_runtime_profile_id::ee_native_boundary);
        {
            ee::c_runtime_profile_scope child(e_runtime_profile_id::vu1_run, 0u, &steps);
            {
                ee::c_runtime_profile_scope kick(e_runtime_profile_id::vu1_xgkick);
            }
            steps = 37u;
            child.stop();
            child.stop();
        }
    }
    const auto& state = ee::g_runtime_profile;
    const auto& root = state.m_counters[static_cast<std::size_t>(e_runtime_profile_id::ee_native_boundary)];
    const auto& child = state.m_counters[static_cast<std::size_t>(e_runtime_profile_id::vu1_run)];
    const auto& kick = state.m_counters[static_cast<std::size_t>(e_runtime_profile_id::vu1_xgkick)];
    if (root.m_calls != 1u || child.m_calls != 1u || kick.m_calls != 1u ||
        root.m_inclusive_ns != root.m_exclusive_ns + child.m_inclusive_ns ||
        child.m_inclusive_ns != child.m_exclusive_ns + kick.m_inclusive_ns ||
        state.m_covered_ns != root.m_exclusive_ns + child.m_exclusive_ns + kick.m_exclusive_ns ||
        child.m_work != 37u || state.m_active != nullptr)
    {
        return 2;
    }
    // Exercise same-ID nesting: exclusive time must still add up exactly.
    {
        ee::c_runtime_profile_scope first(e_runtime_profile_id::gif_consume, 3u);
        ee::c_runtime_profile_scope second(e_runtime_profile_id::gif_consume, 5u);
    }
    const auto& gif = state.m_counters[static_cast<std::size_t>(e_runtime_profile_id::gif_consume)];
    if (gif.m_calls != 2u || gif.m_work != 8u ||
        state.m_covered_ns != root.m_inclusive_ns + gif.m_exclusive_ns)
    {
        return 3;
    }
    ee::runtime_profile_begin(false);
    if (ee::g_runtime_profile.m_covered_ns != 0u || ee::g_runtime_profile.m_enabled)
    {
        return 4;
    }
    return 0;
}
