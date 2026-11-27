#include "../../src/tools/ee_iop_service_attribution.h"

#include <cstdio>

int main()
{
    ee::c_iop_service_attribution profile;
    profile.checkpoint(false);
    profile.checkpoint(true);
    if (profile.checkpoints() != 2u || profile.no_debt() != 1u)
    {
        return 1;
    }

    uint64_t sampled = 0u;
    for (uint32_t group = 0u; group < 8u; ++group)
    {
        uint32_t group_samples = 0u;
        for (uint32_t index = 0u; index < profile.k_sample_group; ++index)
        {
            const bool sample = profile.begin_batch();
            group_samples += sample ? 1u : 0u;
            const uint64_t executed = group % 2u == 0u ? 0u : 7u;
            profile.finish_batch(16u, executed, false, true, sample,
                                 10u, 20u, 30u);
        }
        if (group_samples != 1u)
        {
            return 2;
        }
        sampled += group_samples;
    }
    const auto& idle = profile.bucket(ee::e_iop_service_kind::no_guest);
    const auto& guest = profile.bucket(ee::e_iop_service_kind::guest);
    if (profile.batches() != 2048u || sampled != 8u ||
        idle.m_batches != 1024u || idle.m_guest_instructions != 0u ||
        guest.m_batches != 1024u || guest.m_guest_instructions != 7168u ||
        idle.m_requested_cycles != 16384u || guest.m_requested_cycles != 16384u ||
        idle.m_samples != 4u || guest.m_samples != 4u ||
        idle.m_rpc_calls != 1024u || guest.m_rpc_samples != 4u ||
        guest.m_sampled_kernel_ns != 80u ||
        guest.m_sampled_planner_ns != 40u || guest.m_sampled_rpc_ns != 120u)
    {
        return 3;
    }

    const bool sample = profile.begin_batch();
    profile.finish_batch(32u, 0u, true, false, sample, 1u, 2u, 3u);
    const auto& quiet = profile.bucket(ee::e_iop_service_kind::quiet);
    if (quiet.m_batches != 1u || quiet.m_requested_cycles != 32u ||
        quiet.m_rpc_calls != 0u || quiet.m_rpc_samples != 0u ||
        quiet.m_sampled_rpc_ns != 0u || profile.batches() != 2049u)
    {
        return 4;
    }
    std::puts("IOP service attribution: PASS");
    return 0;
}
