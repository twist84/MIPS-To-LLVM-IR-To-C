#pragma once

#include "ee_address_route.h"
#include "ee_tlb.h"
#include "ee_exceptions.h"

namespace ee
{
// Pure architectural access decision: no memory access, register mutation,
// or exception dispatch. The caller must handle alignment first, then
// commit the selected fault at the correct guest instruction boundary.
enum class e_ee_access_status : uint8_t
{
    PHYSICAL,
    SCRATCHPAD,
    ADDRESS_ERROR,
    TLB_REFILL,
    TLB_INVALID,
    TLB_MODIFIED,
    UNMODELED
};

struct s_ee_access_decision
{
    e_ee_access_status m_status = e_ee_access_status::UNMODELED;
    uint32_t m_physical_address = 0u;
    uint32_t m_cache_mode = 0u;
    uint32_t m_scratchpad_offset = 0u;
};

inline s_ee_access_decision ee_resolve_access(const c_ee_tlb& tlb,
    uint32_t address, uint32_t status, uint32_t config, uint32_t entry_hi,
    bool write_access)
{
    using route = e_ee_address_route;
    using result = e_ee_access_status;
    using lookup = e_ee_tlb_lookup_status;
    const auto segment = ee_classify_address(address, status, config);
    switch (segment.m_route)
    {
    case route::DIRECT_PHYSICAL:
        return {result::PHYSICAL, segment.m_physical_address, segment.m_cache_mode};
    case route::PRIVILEGE_ERROR:
        return {result::ADDRESS_ERROR};
    case route::TLB_MAPPED:
        break;
    default:
        return {};
    }
    const auto mapping = tlb.lookup(address, entry_hi & 0xffu, write_access);
    switch (mapping.m_status)
    {
    case lookup::TRANSLATED:
        return {result::PHYSICAL, mapping.m_physical_address, mapping.m_cache_mode};
    case lookup::SCRATCHPAD_MAPPED:
        return {result::SCRATCHPAD, 0u, 2u, mapping.m_scratchpad_offset};
    case lookup::NO_MATCH:
        return {result::TLB_REFILL};
    case lookup::PAGE_INVALID:
        return {result::TLB_INVALID};
    case lookup::PAGE_NOT_DIRTY:
        return {result::TLB_MODIFIED};
    default:
        return {};
    }
}

struct s_ee_memory_fault
{
    bool m_valid = false;
    bool m_update_tlb_registers = false;
    uint32_t m_code = 0u;
    uint32_t m_vector = 0u;
    uint32_t m_bad_vaddr = 0u;
    uint32_t m_context = 0u;
    uint32_t m_entry_hi = 0u;
};

// Manual sections 4.2.6-4.2.9. Refill and invalid share Cause.ExcCode,
// but only an outer refill selects V_TLB_REFILL. Context.PTEBase and
// the current ASID survive the fault. Do not fabricate Random/EntryLo.
inline constexpr s_ee_memory_fault ee_plan_memory_fault(
    e_ee_access_status access, bool write_access, uint32_t address,
    uint32_t status, uint32_t context, uint32_t entry_hi)
{
    using e = e_ee_access_status;
    s_ee_memory_fault fault{};
    if (access == e::ADDRESS_ERROR)
    {
        fault.m_code = write_access ? EX_AdES : EX_AdEL;
    }
    else if (access == e::TLB_REFILL || access == e::TLB_INVALID ||
             access == e::TLB_MODIFIED)
    {
        if (access == e::TLB_MODIFIED && !write_access)
        {
            return {};
        }
        fault.m_code = access == e::TLB_MODIFIED ? EX_Mod :
            (write_access ? EX_TLBS : EX_TLBL);
        fault.m_update_tlb_registers = true;
        fault.m_context = (context & 0xff800000u) | ((address >> 9u) & 0x007ffff0u);
        fault.m_entry_hi = (address & 0xffffe000u) | (entry_hi & 0xffu);
    }
    else
    {
        return {};
    }
    fault.m_valid = true;
    fault.m_bad_vaddr = address;
    const bool bev = (status & STATUS_BEV) != 0u;
    const bool refill = access == e::TLB_REFILL && (status & STATUS_EXL) == 0u;
    fault.m_vector = refill
        ? (bev ? VEC_TLB_REFILL_BEV1 : VEC_TLB_REFILL_BEV0)
        : (bev ? VEC_COMMON_BEV1 : VEC_COMMON_BEV0);
    return fault;
}

} // namespace ee
