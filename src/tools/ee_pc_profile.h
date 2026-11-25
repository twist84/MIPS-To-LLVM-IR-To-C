#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ee
{
struct s_pc_profile
{
    uint64_t m_calls = 0u;
    uint64_t m_dispatches = 0u;
    uint64_t m_instructions = 0u;
    uint64_t m_idle_instructions = 0u;
    uint64_t m_wall_ns = 0u;
};

// Exact, uncapped per-PC accounting. Cache stable references to the
// owning map's counters; rehash does not invalidate element references.
// Collisions (including unaligned PCs) only force another map lookup.
template <std::size_t k_slots = 16384u>
class c_ee_pc_profile
{
public:
    static_assert(k_slots != 0u && (k_slots & (k_slots - 1u)) == 0u,
        "profile cache size must be a nonzero power of two");

    explicit c_ee_pc_profile(bool enabled)
        : m_slots(enabled ? k_slots : 0u)
    {
    }

    c_ee_pc_profile(const c_ee_pc_profile&) = delete;
    c_ee_pc_profile& operator=(const c_ee_pc_profile&) = delete;

    void record(uint32_t pc, uint32_t dispatches, uint64_t instructions,
                uint64_t idle_instructions, uint64_t wall_ns)
    {
        if (!m_slots.empty())
        {
            const uint32_t word = pc >> 2u;
            auto& slot = m_slots[(word ^ (word >> 12u)) & (k_slots - 1u)];
            if (slot.m_entry == nullptr || slot.m_pc != pc)
            {
                slot = {pc, &m_entries[pc]};
            }
            auto& entry = *slot.m_entry;
            ++entry.m_calls;
            entry.m_dispatches += dispatches;
            entry.m_instructions += instructions;
            entry.m_idle_instructions += idle_instructions;
            entry.m_wall_ns += wall_ns;
        }
    }

    const std::unordered_map<uint32_t, s_pc_profile>& entries() const
    {
        return m_entries;
    }

private:
    struct s_slot
    {
        uint32_t m_pc = 0u;
        s_pc_profile* m_entry = nullptr;
    };

    // No erasure or replacement of map entries is exposed.
    std::unordered_map<uint32_t, s_pc_profile> m_entries;
    std::vector<s_slot> m_slots;
};
} // namespace ee
