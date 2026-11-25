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
        // Fieldwise XOR reduction permits grouped comparisons on the
        // common unchanged-state path without inspecting struct padding.
        const uint32_t changed32 =
            (m_thread_id ^ other.m_thread_id) |
            (m_cop0_status ^ other.m_cop0_status) |
            (m_cop0_cause ^ other.m_cop0_cause) |
            (m_intc_status ^ other.m_intc_status) |
            (m_intc_mask ^ other.m_intc_mask) |
            (m_dmac_status ^ other.m_dmac_status) |
            (m_dmac_pending ^ other.m_dmac_pending) |
            (m_gif_chcr ^ other.m_gif_chcr) |
            (m_gif_madr ^ other.m_gif_madr) |
            (m_gif_qwc ^ other.m_gif_qwc) |
            (m_gif_tadr ^ other.m_gif_tadr) |
            (m_gif_reject ^ other.m_gif_reject);
        const uint64_t changed64 =
            (m_gif_write_sequence ^ other.m_gif_write_sequence) |
            (m_gs_fields ^ other.m_gs_fields) |
            (m_gs_csr ^ other.m_gs_csr) |
            (m_iop_rpc_sequence ^ other.m_iop_rpc_sequence);
        return (static_cast<uint64_t>(changed32) | changed64) == 0u;
    }

    bool operator!=(const s_ee_hot_loop_state& other) const
    {
        return !(*this == other);
    }
};

class c_ee_dispatch_hotspots
{
public:
    // Retail P40 exhausted the previous 4096/8192 limits during
    // startup, losing approximately half of later dispatch observations.
    // Keep the maps bounded but allow a larger exact working set. The
    // independent hot counter caches remain unchanged.
    static constexpr size_t k_pc_capacity = 32768u;
    static constexpr size_t k_edge_capacity = 65536u;
    static constexpr size_t k_change_capacity = 32u;
    static constexpr size_t k_top_count = 16u;

    explicit c_ee_dispatch_hotspots(
        size_t pc_capacity = k_pc_capacity,
        size_t edge_capacity = k_edge_capacity)
        : m_pc_capacity(pc_capacity),
          m_edge_capacity(edge_capacity),
          m_pc_cache(k_pc_cache_size),
          m_edge_cache(k_edge_cache_size)
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
        // Keep the full PC/edge keys for equality, including zero and
        // unaligned addresses. Fold high address bits into the cache index.
        const uint32_t from_word = from_pc >> 2u;
        const uint32_t to_word = to_pc >> 2u;
        const size_t pc_slot = (from_word ^ (from_word >> 12u)) &
            (k_pc_cache_size - 1u);
        const size_t edge_slot = (from_word ^ (from_word >> 13u) ^
            ((to_word ^ (to_word >> 13u)) * 0x9e3779b9u)) &
            (k_edge_cache_size - 1u);
        m_pc_cache_misses += count_cached(m_pcs, m_pc_cache[pc_slot], from_pc,
            m_pc_capacity, m_untracked_pc_samples) ? 1u : 0u;
        m_edge_cache_misses += count_cached(m_edges, m_edge_cache[edge_slot],
            edge_key(from_pc, to_pc), m_edge_capacity,
            m_untracked_edge_samples) ? 1u : 0u;
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
            "\n    \"total_state_changes\": %llu,"
            "\n    \"pc_cache_slots\": %zu, \"edge_cache_slots\": %zu,"
            "\n    \"pc_cache_misses\": %llu, \"edge_cache_misses\": %llu",
            static_cast<unsigned long long>(m_observations),
            m_pc_capacity, m_edge_capacity, m_pcs.size(), m_edges.size(),
            static_cast<unsigned long long>(m_untracked_pc_samples),
            static_cast<unsigned long long>(m_untracked_edge_samples),
            static_cast<unsigned long long>(m_total_state_changes),
            m_pc_cache.size(), m_edge_cache.size(),
            static_cast<unsigned long long>(m_pc_cache_misses),
            static_cast<unsigned long long>(m_edge_cache_misses));

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
                i ? ", " : "", static_cast<uint32_t>(edges[i].first >> 32u),
                static_cast<uint32_t>(edges[i].first),
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

    // Unsigned packed order is identical to lexicographic (from, to)
    // order, including zero, unaligned and full-width guest addresses.
    // A cache hit now compares one scalar key instead of two PC fields.
    static constexpr uint64_t edge_key(uint32_t from_pc, uint32_t to_pc)
    {
        return (static_cast<uint64_t>(from_pc) << 32u) | to_pc;
    }

    struct s_edge_hash
    {
        size_t operator()(uint64_t value) const
        {
            // Mix both guest PCs before bucket selection.
            value ^= value >> 33u;
            value *= 0xff51afd7ed558ccdull;
            value ^= value >> 33u;
            return static_cast<size_t>(value);
        }
    };

    // Independent of tracking capacity: negative lookups are cacheable
    // after the bounded map fills. Heap storage avoids runner stack growth.
    static constexpr size_t k_pc_cache_size = 8192u;
    static constexpr size_t k_edge_cache_size = 16384u;

    template <typename t_key>
    struct s_counter_cache_entry
    {
        t_key m_key{};
        uint64_t* m_counter = nullptr;
    };

    template <typename t_key, typename t_hash>
    static bool count_cached(std::unordered_map<t_key, uint64_t, t_hash>& counts,
                             s_counter_cache_entry<t_key>& slot,
                             const t_key& key, size_t capacity,
                             uint64_t& untracked)
    {
        const bool miss = slot.m_counter == nullptr || slot.m_key != key;
        if (miss)
        {
            auto found = counts.find(key);
            if (found == counts.end() && counts.size() < capacity)
            {
                found = counts.emplace(key, 0u).first;
            }
            // Map references survive rehash; we never erase. Rejected
            // keys point to the owning object's untracked counter once
            // the fixed capacity fills. The object cannot move or copy,
            // so both kinds of counter pointers remain valid. Null now
            // denotes only a cold slot, including for the zero key.
            slot.m_key = key;
            slot.m_counter = found != counts.end() ? &found->second : &untracked;
        }
        ++*slot.m_counter;
        return miss;
    }

    template <typename t_key, typename t_hash>
    static std::vector<std::pair<t_key, uint64_t>> top_counts(
        const std::unordered_map<t_key, uint64_t, t_hash>& counts)
    {
        std::vector<std::pair<t_key, uint64_t>> sorted(
            counts.begin(), counts.end());
        // Only the first k_top_count entries are serialized. A partial
        // sort retains the exact count/key ordering without sorting the
        // thousands of entries that will be discarded.
        const size_t selected = std::min(sorted.size(), k_top_count);
        std::partial_sort(sorted.begin(), sorted.begin() + selected, sorted.end(),
            [](const auto& a, const auto& b)
            {
                if (a.second != b.second)
                {
                    return a.second > b.second;
                }
                return a.first < b.first;
            });
        sorted.resize(selected);
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
    std::unordered_map<uint64_t, uint64_t, s_edge_hash> m_edges;
    std::vector<s_counter_cache_entry<uint32_t>> m_pc_cache;
    std::vector<s_counter_cache_entry<uint64_t>> m_edge_cache;
    uint64_t m_pc_cache_misses = 0u;
    uint64_t m_edge_cache_misses = 0u;
    c_bounded_diagnostic_history<s_change, k_change_capacity> m_changes;
};

} // namespace ee
