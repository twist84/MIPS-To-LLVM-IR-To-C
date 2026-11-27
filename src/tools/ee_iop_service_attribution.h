#pragma once

#include <cstdint>

namespace ee
{
    enum class e_iop_service_kind : uint32_t
    {
        no_guest,
        guest,
        quiet,
        count
    };

    struct s_iop_service_bucket
    {
        uint64_t m_batches = 0u;
        uint64_t m_requested_cycles = 0u;
        uint64_t m_guest_instructions = 0u;
        uint64_t m_samples = 0u;
        uint64_t m_sampled_planner_ns = 0u;
        uint64_t m_sampled_kernel_ns = 0u;
        uint64_t m_rpc_calls = 0u;
        uint64_t m_rpc_samples = 0u;
        uint64_t m_sampled_rpc_ns = 0u;
    };

    // Exact work counts with one timing sample per complete group of
    // 256 physical service batches. Vary the offset within each group
    // to avoid locking to one fixed scheduler phase. Diagnostic only.
    class c_iop_service_attribution
    {
    public:
        static constexpr uint32_t k_sample_group = 256u;

        void checkpoint(bool due)
        {
            ++m_checkpoints;
            if (!due)
            {
                ++m_no_debt;
            }
        }

        bool begin_batch()
        {
            const uint32_t offset = static_cast<uint32_t>(
                m_batches % k_sample_group);
            if (offset == 0u)
            {
                // Defined unsigned wrap; high bits select the offset.
                m_rng = m_rng * 1664525u + 1013904223u;
                m_sample_offset = m_rng >> 24u;
            }
            ++m_batches;
            return offset == m_sample_offset;
        }

        void finish_batch(uint64_t cycles, uint64_t executed, bool quiet,
                          bool rpc, bool sampled, uint64_t planner_ns,
                          uint64_t kernel_ns, uint64_t rpc_ns)
        {
            const auto kind = quiet ? e_iop_service_kind::quiet :
                (executed != 0u ? e_iop_service_kind::guest :
                    e_iop_service_kind::no_guest);
            auto& bucket = m_buckets[static_cast<uint32_t>(kind)];
            ++bucket.m_batches;
            bucket.m_requested_cycles += cycles;
            bucket.m_guest_instructions += executed;
            if (rpc)
            {
                ++bucket.m_rpc_calls;
            }
            if (sampled)
            {
                ++bucket.m_samples;
                bucket.m_sampled_planner_ns += planner_ns;
                bucket.m_sampled_kernel_ns += kernel_ns;
                if (rpc)
                {
                    ++bucket.m_rpc_samples;
                    bucket.m_sampled_rpc_ns += rpc_ns;
                }
            }
        }

        uint64_t checkpoints() const { return m_checkpoints; }
        uint64_t no_debt() const { return m_no_debt; }
        uint64_t batches() const { return m_batches; }

        const s_iop_service_bucket& bucket(e_iop_service_kind kind) const
        {
            return m_buckets[static_cast<uint32_t>(kind)];
        }

    private:
        uint64_t m_checkpoints = 0u;
        uint64_t m_no_debt = 0u;
        uint64_t m_batches = 0u;
        uint32_t m_rng = 0x9e3779b9u;
        uint32_t m_sample_offset = 0u;
        s_iop_service_bucket m_buckets[
            static_cast<uint32_t>(e_iop_service_kind::count)]{};
    };
}
