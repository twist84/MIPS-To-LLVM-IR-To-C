#pragma once

#include <cstdint>

namespace ee
{
// The EE's 48-entry software-visible TLB register array. The HLE runtime
// retains ps2-web-recomp's direct-memory policy; this table does not silently
// enable unverified address translations. The explicit translate() operation
// is available to callers that can report TLB faults correctly.
struct s_ee_tlb_entry
{
    uint32_t m_page_mask = 0u;
    uint32_t m_entry_hi = 0u;
    uint32_t m_entry_lo0 = 0u;
    uint32_t m_entry_lo1 = 0u;
    bool m_written = false;
};

// Detailed opt-in lookup. These are model outcomes, not delivered guest
// exceptions. Multiple matches and unsupported attributes deliberately
// fail closed rather than choose an arbitrary physical mapping.
enum class e_ee_tlb_lookup_status : uint8_t
{
    TRANSLATED,
    NO_MATCH,
    PAGE_INVALID,
    PAGE_NOT_DIRTY,
    SCRATCHPAD_MAPPED,
    SCRATCHPAD_UNMODELED,
    RESERVED_ENTRY_BITS,
    RESERVED_CACHE_MODE,
    MULTIPLE_MATCHES
};

struct s_ee_tlb_lookup
{
    e_ee_tlb_lookup_status m_status = e_ee_tlb_lookup_status::NO_MATCH;
    uint32_t m_physical_address = 0u;
    uint32_t m_entry_index = 0xffffffffu;
    uint32_t m_page_size = 0u;
    uint32_t m_cache_mode = 0u;
    bool m_odd_page = false;
    uint32_t m_scratchpad_offset = 0u;
};

class c_ee_tlb
{
public:
    static constexpr uint32_t k_entry_count = 48u;

    static constexpr bool supported_page_mask(uint32_t mask)
    {
        switch (mask)
        {
        case 0x00000000u: // 4 KiB pages
        case 0x00006000u: // 16 KiB
        case 0x0001e000u: // 64 KiB
        case 0x0007e000u: // 256 KiB
        case 0x001fe000u: // 1 MiB
        case 0x007fe000u: // 4 MiB
        case 0x01ffe000u: // 16 MiB
            return true;
        default:
            return false;
        }
    }

    constexpr bool write(uint32_t index, uint32_t page_mask, uint32_t entry_hi,
               uint32_t entry_lo0, uint32_t entry_lo1)
    {
        if (index >= k_entry_count || !supported_page_mask(page_mask))
        {
            return false;
        }
        s_ee_tlb_entry& entry = m_entries[index];
        entry.m_page_mask = page_mask;
        entry.m_entry_hi = entry_hi;
        entry.m_entry_lo0 = entry_lo0;
        entry.m_entry_lo1 = entry_lo1;
        entry.m_written = true;
        return true;
    }

    constexpr bool read(uint32_t index, s_ee_tlb_entry* output) const
    {
        if (output == nullptr || index >= k_entry_count ||
            !m_entries[index].m_written)
        {
            return false;
        }
        *output = m_entries[index];
        return true;
    }

    constexpr bool probe(uint32_t entry_hi, uint32_t* index) const
    {
        if (index == nullptr)
        {
            return false;
        }
        for (uint32_t i = 0u; i < k_entry_count; ++i)
        {
            const s_ee_tlb_entry& entry = m_entries[i];
            if (!entry.m_written)
            {
                continue;
            }
            const uint32_t comparison_mask = entry.m_page_mask | 0x1fffu;
            const bool global =
                (entry.m_entry_lo0 & entry.m_entry_lo1 & 1u) != 0u;
            if (((entry_hi ^ entry.m_entry_hi) &
                 ~comparison_mask) == 0u &&
                (global || (entry_hi & 0xffu) ==
                           (entry.m_entry_hi & 0xffu)))
            {
                *index = i;
                return true;
            }
        }
        return false;
    }

    // Architectural classification for a caller that can deliver precise
    // faults. Existing HLE direct aliases and legacy translate() are not
    // changed; cache modes are reported, not emulated.
    constexpr s_ee_tlb_lookup lookup(uint32_t virtual_address, uint32_t asid,
                           bool write_access) const
    {
        s_ee_tlb_lookup result{};
        for (uint32_t i = 0u; i < k_entry_count; ++i)
        {
            const s_ee_tlb_entry& entry = m_entries[i];
            if (!entry.m_written)
            {
                continue;
            }
            const bool scratchpad = (entry.m_entry_lo0 & 0x80000000u) != 0u;
            // A scratchpad entry covers a single aligned 16 KiB region,
            // despite a zero MASK field (manual section 5.2.5).
            const uint32_t mask = scratchpad ? 0x3fffu : (entry.m_page_mask | 0x1fffu);
            const bool global = (entry.m_entry_lo0 & entry.m_entry_lo1 & 1u) != 0u;
            if (((virtual_address ^ entry.m_entry_hi) & ~mask) != 0u ||
                (!global && (asid & 0xffu) != (entry.m_entry_hi & 0xffu)))
            {
                continue;
            }
            if (result.m_entry_index != 0xffffffffu)
            {
                result = {};
                result.m_status = e_ee_tlb_lookup_status::MULTIPLE_MATCHES;
                return result;
            }

            result.m_entry_index = i;
            result.m_page_size = scratchpad ? 0x4000u : (mask + 1u) / 2u;
            result.m_odd_page = !scratchpad && (virtual_address & result.m_page_size) != 0u;
            const uint32_t lo = result.m_odd_page ?
                entry.m_entry_lo1 : entry.m_entry_lo0;
            result.m_cache_mode = (lo >> 3u) & 7u;

            // S belongs to EntryLo0, not to each independent PFN.
            // Scratchpad translation needs a separately audited mapping.
            if ((entry.m_entry_lo0 & 0x7c000000u) != 0u ||
                (entry.m_entry_lo1 & 0xfc000000u) != 0u)
            {
                result.m_status = e_ee_tlb_lookup_status::RESERVED_ENTRY_BITS;
            }
            else if (scratchpad && (entry.m_page_mask != 0u ||
                         (entry.m_entry_hi & 0x2000u) != 0u ||
                         ((entry.m_entry_lo0 ^ entry.m_entry_lo1) & 6u) != 0u))
            {
                result.m_status = e_ee_tlb_lookup_status::SCRATCHPAD_UNMODELED;
            }
            else if ((lo & 2u) == 0u)
            {
                result.m_status = e_ee_tlb_lookup_status::PAGE_INVALID;
            }
            else if (write_access && (lo & 4u) == 0u)
            {
                result.m_status = e_ee_tlb_lookup_status::PAGE_NOT_DIRTY;
            }
            else if (scratchpad)
            {
                result.m_cache_mode = 2u;
                result.m_scratchpad_offset = virtual_address & 0x3fffu;
                result.m_status = e_ee_tlb_lookup_status::SCRATCHPAD_MAPPED;
            }
            else if (result.m_cache_mode != 2u && result.m_cache_mode != 3u &&
                     result.m_cache_mode != 7u)
            {
                result.m_status = e_ee_tlb_lookup_status::RESERVED_CACHE_MODE;
            }
            else
            {
                const uint32_t frame = ((lo & 0x03ffffc0u) << 6u) &
                    ~(result.m_page_size - 1u);
                result.m_physical_address = frame |
                    (virtual_address & (result.m_page_size - 1u));
                result.m_status = e_ee_tlb_lookup_status::TRANSLATED;
            }
        }
        return result;
    }

    // Explicit translation of a matched user/mapped address. A matching
    // invalid/clean page is an access failure, never an identity fallback.
    // Do not use this API without also raising architectural TLB exceptions.
    constexpr bool translate(uint32_t virtual_address, uint32_t asid, bool write_access,
                   uint32_t* physical_address) const
    {
        if (physical_address == nullptr)
        {
            return false;
        }
        for (uint32_t i = 0u; i < k_entry_count; ++i)
        {
            const s_ee_tlb_entry& entry = m_entries[i];
            if (!entry.m_written)
            {
                continue;
            }
            const uint32_t comparison_mask = entry.m_page_mask | 0x1fffu;
            const bool global =
                (entry.m_entry_lo0 & entry.m_entry_lo1 & 1u) != 0u;
            if (((virtual_address ^ entry.m_entry_hi) &
                 ~comparison_mask) != 0u ||
                (!global && (asid & 0xffu) !=
                    (entry.m_entry_hi & 0xffu)))
            {
                continue;
            }
            const uint32_t page_size = (comparison_mask + 1u) / 2u;
            const bool odd = (virtual_address & page_size) != 0u;
            const uint32_t entry_lo = odd ?
                entry.m_entry_lo1 : entry.m_entry_lo0;
            // Scratchpad-TLB mapping requires separate scratchpad handling.
            if ((entry_lo & 0x80000000u) != 0u ||
                (entry_lo & 2u) == 0u ||
                (write_access && (entry_lo & 4u) == 0u))
            {
                return false;
            }
            const uint32_t frame =
                ((entry_lo & 0x03ffffc0u) << 6u) & ~(page_size - 1u);
            *physical_address = frame | (virtual_address & (page_size - 1u));
            return true;
        }
        return false;
    }

private:
    s_ee_tlb_entry m_entries[k_entry_count]{};
};
} // namespace ee
