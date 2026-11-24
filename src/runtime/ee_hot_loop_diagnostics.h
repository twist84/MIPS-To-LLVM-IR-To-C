#pragma once

// Host-only bounded M2.0 diagnostics: one observation per native function
// dispatch, not an EE instruction, guest cycle, IRQ or scheduled event.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <array>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ee
{

// Chronological bounded history with constant-time append and no allocation.
template <typename t_sample, size_t k_capacity>
class c_bounded_diagnostic_history
{
public:
    static_assert(k_capacity > 0u, "history capacity must be positive");

    void push_back(const t_sample& sample)
    {
        m_samples[(m_first + m_size) % k_capacity] = sample;
        if (m_size == k_capacity)
        {
            m_first = (m_first + 1u) % k_capacity;
        }
        else
        {
            ++m_size;
        }
    }

    size_t size() const { return m_size; }
    bool empty() const { return m_size == 0u; }

    const t_sample& operator[](size_t index) const
    {
        return m_samples[(m_first + index) % k_capacity];
    }

private:
    std::array<t_sample, k_capacity> m_samples{};
    size_t m_first = 0u;
    size_t m_size = 0u;
};

struct s_ee_hot_loop_state
{
    uint32_t m_thread_id = 0u;
    uint32_t m_cop0_status = 0u;
    uint32_t m_cop0_cause = 0u;
    uint32_t m_intc_status = 0u;
    uint32_t m_intc_mask = 0u;
    uint32_t m_dmac_status = 0u;
    uint32_t m_dmac_pending = 0u;
    uint32_t m_gif_chcr = 0u;
    uint32_t m_gif_madr = 0u;
    uint32_t m_gif_qwc = 0u;
    uint32_t m_gif_tadr = 0u;
    uint64_t m_gif_write_sequence = 0u;
    uint32_t m_gif_reject = 0u;
    uint64_t m_gs_fields = 0u;
    uint64_t m_gs_csr = 0u;
    uint64_t m_iop_rpc_sequence = 0u;

    bool operator==(const s_ee_hot_loop_state& other) const
    {
        return m_thread_id == other.m_thread_id &&
            m_cop0_status == other.m_cop0_status &&
            m_cop0_cause == other.m_cop0_cause &&
            m_intc_status == other.m_intc_status &&
            m_intc_mask == other.m_intc_mask &&
            m_dmac_status == other.m_dmac_status &&
            m_dmac_pending == other.m_dmac_pending &&
            m_gif_chcr == other.m_gif_chcr &&
            m_gif_madr == other.m_gif_madr &&
            m_gif_qwc == other.m_gif_qwc &&
            m_gif_tadr == other.m_gif_tadr &&
            m_gif_write_sequence == other.m_gif_write_sequence &&
            m_gif_reject == other.m_gif_reject &&
            m_gs_fields == other.m_gs_fields &&
            m_gs_csr == other.m_gs_csr &&
            m_iop_rpc_sequence == other.m_iop_rpc_sequence;
    }

    bool operator!=(const s_ee_hot_loop_state& other) const
    {
        return !(*this == other);
    }
};

class c_ee_dispatch_hotspots
{
public:
    static constexpr size_t k_pc_capacity = 4096u;
    static constexpr size_t k_edge_capacity = 8192u;
    static constexpr size_t k_change_capacity = 32u;
    static constexpr size_t k_top_count = 16u;

    explicit c_ee_dispatch_hotspots(
        size_t pc_capacity = k_pc_capacity,
        size_t edge_capacity = k_edge_capacity)
        : m_pc_capacity(pc_capacity),
          m_edge_capacity(edge_capacity)
    {
        m_pcs.reserve(pc_capacity);
        m_edges.reserve(edge_capacity);
    }

    // The counter caches retain stable pointers into the owning maps.
    // This host diagnostic object is never copied or moved.
    c_ee_dispatch_hotspots(const c_ee_dispatch_hotspots&) = delete;
    c_ee_dispatch_hotspots& operator=(const c_ee_dispatch_hotspots&) = delete;

    void record(uint64_t dispatch, uint32_t from_pc, uint32_t to_pc,
                const s_ee_hot_loop_state& state)
    {
        ++m_observations;
        const size_t pc_slot = (from_pc >> 2u) & (k_counter_cache_size - 1u);
        const size_t edge_slot = ((from_pc >> 2u) ^
            ((to_pc >> 2u) * 0x9e3779b9u)) & (k_counter_cache_size - 1u);
        count_cached(m_pcs, m_pc_cache[pc_slot], from_pc,
            m_pc_capacity, m_untracked_pc_samples);
        count_cached(m_edges, m_edge_cache[edge_slot],
            std::make_pair(from_pc, to_pc), m_edge_capacity,
            m_untracked_edge_samples);
        if (!m_has_state)
        {
            m_has_state = true;
            m_last_state = state;
        }
        else if (state != m_last_state)
        {
            m_changes.push_back({dispatch, from_pc, m_last_state, state});
            ++m_total_state_changes;
            m_last_state = state;
        }
    }

    uint64_t observations() const
    {
        return m_observations;
    }

    uint64_t untracked_pc_samples() const
    {
        return m_untracked_pc_samples;
    }

    uint64_t untracked_edge_samples() const
    {
        return m_untracked_edge_samples;
    }

    // Produces one object, no trailing comma/newline; stable sorting.
    // If a capacity fills, previously recorded keys remain exact; new
    // unmatched keys count as untracked, never as fabricated zero visits.
    void write_json(FILE* output) const
    {
        std::fprintf(output,
            "{\n    \"schema\": \"ee-m2-dispatch-hotspots-v1\","
            "\n    \"observation_unit\": \"native_dispatch_boundary\","
            "\n    \"observations\": %llu,"
            "\n    \"pc_capacity\": %zu, \"edge_capacity\": %zu,"
            "\n    \"tracked_pcs\": %zu, \"tracked_edges\": %zu,"
            "\n    \"untracked_pc_samples\": %llu,"
            "\n    \"untracked_edge_samples\": %llu,"
            "\n    \"total_state_changes\": %llu",
            static_cast<unsigned long long>(m_observations),
            m_pc_capacity, m_edge_capacity, m_pcs.size(), m_edges.size(),
            static_cast<unsigned long long>(m_untracked_pc_samples),
            static_cast<unsigned long long>(m_untracked_edge_samples),
            static_cast<unsigned long long>(m_total_state_changes));

        const auto pcs = top_counts(m_pcs);
        std::fputs(",\n    \"hot_pcs\": [", output);
        for (size_t i = 0u; i < pcs.size(); ++i)
        {
            std::fprintf(output,
                "%s{\"pc\": %u, \"visits\": %llu}",
                i ? ", " : "", pcs[i].first,
                static_cast<unsigned long long>(pcs[i].second));
        }
        std::fputs("],\n    \"hot_transitions\": [", output);
        const auto edges = top_counts(m_edges);
        for (size_t i = 0u; i < edges.size(); ++i)
        {
            std::fprintf(output,
                "%s{\"from_pc\": %u, \"to_pc\": %u, \"visits\": %llu}",
                i ? ", " : "", edges[i].first.first,
                edges[i].first.second,
                static_cast<unsigned long long>(edges[i].second));
        }
        std::fputs("],\n    \"latest_state\": ", output);
        if (m_has_state)
        {
            write_state(output, m_last_state);
        }
        else
        {
            std::fputs("null", output);
        }
        std::fputs(",\n    \"state_changes\": [", output);
        for (size_t i = 0u; i < m_changes.size(); ++i)
        {
            const s_change& change = m_changes[i];
            std::fprintf(output,
                "%s\n      {\"dispatch\": %llu, \"from_pc\": %u,"
                " \"before\": ",
                i ? "," : "",
                static_cast<unsigned long long>(change.m_dispatch),
                change.m_pc);
            write_state(output, change.m_before);
            std::fputs(", \"after\": ", output);
            write_state(output, change.m_after);
            std::fputc('}', output);
        }
        std::fputs(m_changes.empty() ? "]\n  }" : "\n    ]\n  }", output);
    }

private:
    struct s_change
    {
        uint64_t m_dispatch;
        uint32_t m_pc;
        s_ee_hot_loop_state m_before;
        s_ee_hot_loop_state m_after;
    };

    struct s_edge_hash
    {
        size_t operator()(const std::pair<uint32_t, uint32_t>& key) const
        {
            // Mix both aligned guest PCs before bucket selection.
            uint64_t value = (static_cast<uint64_t>(key.first) << 32u) | key.second;
            value ^= value >> 33u;
            value *= 0xff51afd7ed558ccdull;
            value ^= value >> 33u;
            return static_cast<size_t>(value);
        }
    };

    static constexpr size_t k_counter_cache_size = 1024u;

    template <typename t_key>
    struct s_counter_cache_entry
    {
        t_key m_key{};
        uint64_t* m_counter = nullptr;
        bool m_valid = false;
    };

    template <typename t_key, typename t_hash>
    static void count_cached(std::unordered_map<t_key, uint64_t, t_hash>& counts,
                             s_counter_cache_entry<t_key>& slot,
                             const t_key& key, size_t capacity,
                             uint64_t& untracked)
    {
        if (!slot.m_valid || slot.m_key != key)
        {
            auto found = counts.find(key);
            if (found == counts.end() && counts.size() < capacity)
            {
                found = counts.emplace(key, 0u).first;
            }
            // unordered_map references survive rehash. We never erase.
            // A null counter is cached only after the map is full;
            // capacity never changes, so the rejection remains valid.
            slot.m_key = key;
            slot.m_counter = found != counts.end() ? &found->second : nullptr;
            slot.m_valid = true;
        }
        if (slot.m_counter != nullptr)
        {
            ++*slot.m_counter;
        }
        else
        {
            ++untracked;
        }
    }

    template <typename t_key, typename t_hash>
    static std::vector<std::pair<t_key, uint64_t>> top_counts(
        const std::unordered_map<t_key, uint64_t, t_hash>& counts)
    {
        std::vector<std::pair<t_key, uint64_t>> sorted(
            counts.begin(), counts.end());
        std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b)
            {
                if (a.second != b.second)
                {
                    return a.second > b.second;
                }
                return a.first < b.first;
            });
        if (sorted.size() > k_top_count)
        {
            sorted.resize(k_top_count);
        }
        return sorted;
    }

    static void write_state(FILE* output, const s_ee_hot_loop_state& state)
    {
        std::fprintf(output,
            "{\"thread_id\": %u, \"cop0_status\": %u,"
            " \"cop0_cause\": %u, \"intc_status\": %u,"
            " \"intc_mask\": %u, \"dmac_status\": %u,"
            " \"dmac_pending\": %u, \"gif_chcr\": %u,"
            " \"gif_madr\": %u, \"gif_qwc\": %u, \"gif_tadr\": %u,"
            " \"gif_write_sequence\": %llu, \"gif_reject\": %u,"
            " \"gs_fields\": %llu, \"gs_csr\": %llu,"
            " \"iop_rpc_sequence\": %llu}",
            state.m_thread_id, state.m_cop0_status, state.m_cop0_cause,
            state.m_intc_status, state.m_intc_mask,
            state.m_dmac_status, state.m_dmac_pending,
            state.m_gif_chcr, state.m_gif_madr,
            state.m_gif_qwc, state.m_gif_tadr,
            static_cast<unsigned long long>(state.m_gif_write_sequence),
            state.m_gif_reject,
            static_cast<unsigned long long>(state.m_gs_fields),
            static_cast<unsigned long long>(state.m_gs_csr),
            static_cast<unsigned long long>(state.m_iop_rpc_sequence));
    }

    size_t m_pc_capacity = 0u;
    size_t m_edge_capacity = 0u;
    uint64_t m_observations = 0u;
    uint64_t m_untracked_pc_samples = 0u;
    uint64_t m_untracked_edge_samples = 0u;
    uint64_t m_total_state_changes = 0u;
    bool m_has_state = false;
    s_ee_hot_loop_state m_last_state{};
    std::unordered_map<uint32_t, uint64_t> m_pcs;
    std::unordered_map<std::pair<uint32_t, uint32_t>, uint64_t, s_edge_hash> m_edges;
    std::array<s_counter_cache_entry<uint32_t>, k_counter_cache_size> m_pc_cache{};
    std::array<s_counter_cache_entry<std::pair<uint32_t, uint32_t>>,
        k_counter_cache_size> m_edge_cache{};
    c_bounded_diagnostic_history<s_change, k_change_capacity> m_changes;
};

} // namespace ee
