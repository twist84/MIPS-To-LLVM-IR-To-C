#pragma once

#include "ee_dispatcher.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ee
{
// Exact-PC cache. The owning module remains loaded until clear().
// Collisions can only cause another lookup; never use an aliased PC.
class c_native_target_cache
{
public:
    explicit c_native_target_cache(bool enabled)
        : m_slots(enabled ? k_slot_count : 0u)
    {
    }

    template<typename t_resolve>
    t_ee_translated_entry resolve(uint32_t pc, const t_resolve& lookup)
    {
        if (m_slots.empty() || pc == 0u)
        {
            return nullptr;
        }
        auto& slot = m_slots[(pc >> 2u) & (k_slot_count - 1u)];
        // Zero is reserved for empty slots. Only nonzero PCs with a
        // callable entry are installed, so an exact match is sufficient.
        if (slot.m_pc == pc)
        {
            ++m_hits;
            return slot.m_entry;
        }
        ++m_misses;
        const auto entry = lookup(pc);
        if (entry != nullptr)
        {
            slot = {pc, entry};
        }
        return entry;
    }

    void clear()
    {
        std::fill(m_slots.begin(), m_slots.end(), s_slot{});
    }

    uint64_t hits() const { return m_hits; }
    uint64_t misses() const { return m_misses; }

private:
    static constexpr size_t k_slot_count = 65536u;
    struct s_slot
    {
        uint32_t m_pc = 0u;
        t_ee_translated_entry m_entry = nullptr;
    };
    std::vector<s_slot> m_slots;
    uint64_t m_hits = 0u;
    uint64_t m_misses = 0u;
};

// Host-only proof cache. Source ELF bytes and an already loaded first
// owner are immutable for the lifetime of a module. Never cache a null
// target (a new overlay may resolve it); clear before any owner unload.
// Exact PC, target and source bound must all match, including negatives.
struct s_native_alu_identity
{
    uint32_t m_owner = 0u;
    bool m_certified = false;
};

class c_native_alu_identity_cache
{
public:
    explicit c_native_alu_identity_cache(bool enabled)
        : m_slots(enabled ? k_slot_count : 0u)
    {
    }

    template<typename t_verify>
    s_native_alu_identity resolve(uint32_t pc, t_ee_translated_entry entry,
        uint32_t slots, const t_verify& verify)
    {
        if (m_slots.empty() || pc == 0u || entry == nullptr || slots == 0u)
        {
            return {};
        }
        auto& slot = m_slots[(pc >> 2u) & (k_slot_count - 1u)];
        if (slot.m_pc == pc && slot.m_entry == entry && slot.m_slots == slots)
        {
            ++m_hits;
            return slot.m_identity;
        }
        ++m_misses;
        const s_native_alu_identity identity = verify(pc, entry, slots);
        // Never treat a failed owner lookup as a certified entry.
        slot = {pc, entry, slots,
            {identity.m_owner, identity.m_owner != 0u && identity.m_certified}};
        return slot.m_identity;
    }

    void clear()
    {
        std::fill(m_slots.begin(), m_slots.end(), s_slot{});
    }

    uint64_t hits() const { return m_hits; }
    uint64_t misses() const { return m_misses; }

private:
    static constexpr size_t k_slot_count = 65536u;
    struct s_slot
    {
        uint32_t m_pc = 0u;
        t_ee_translated_entry m_entry = nullptr;
        uint32_t m_slots = 0u;
        s_native_alu_identity m_identity{};
    };
    std::vector<s_slot> m_slots;
    uint64_t m_hits = 0u;
    uint64_t m_misses = 0u;
};

} // namespace ee
