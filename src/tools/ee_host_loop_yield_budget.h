#pragma once

#include <cstdint>

namespace ee
{
// Limits consecutive native watchdog safepoints while the guest has not
// produced any independently observed GS/GIF or IOP RPC progress.
//
// The translated loop has already committed its architectural state before
// each HOST_YIELD. This bookkeeping neither advances guest time nor makes an
// interrupt, DMA completion, RPC response, or framebuffer update happen.
class c_host_loop_yield_budget
{
public:
    constexpr bool try_resume(
        uint32_t limit, uint64_t gs_fields,
        uint64_t iop_rpc_sequence, uint64_t gif_writes
    )
    {
        if (m_seen && (
            gs_fields != m_last_gs_fields ||
            iop_rpc_sequence != m_last_iop_rpc_sequence ||
            gif_writes != m_last_gif_writes))
        {
            m_without_progress = 0u;
            ++m_progress_resets;
        }

        m_seen = true;
        m_last_gs_fields = gs_fields;
        m_last_iop_rpc_sequence = iop_rpc_sequence;
        m_last_gif_writes = gif_writes;

        const bool allowed = m_without_progress < limit;
        if (allowed)
        {
            ++m_without_progress;
            ++m_total_resumes;
        }
        return allowed;
    }

    constexpr uint32_t without_progress() const
    {
        return m_without_progress;
    }

    constexpr uint64_t total_resumes() const
    {
        return m_total_resumes;
    }

    constexpr uint64_t progress_resets() const
    {
        return m_progress_resets;
    }

private:
    bool m_seen = false;
    uint32_t m_without_progress = 0u;
    uint64_t m_total_resumes = 0u;
    uint64_t m_progress_resets = 0u;
    uint64_t m_last_gs_fields = 0u;
    uint64_t m_last_iop_rpc_sequence = 0u;
    uint64_t m_last_gif_writes = 0u;
};
} // namespace ee
