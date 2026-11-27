#include "ee_ram_interpreter.h"
#include "ee_memory.h"
#include "ee_address_route.h"
#include "ee_memory_fault.h"
#include "ee_memory_bus_plan.h"
#include "ee_memory_fault_commit.h"
#include "ee_exceptions.h"
#include "ee_runtime.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace
{
constexpr uint32_t k_entry = 0x800750c8u;
constexpr uint32_t k_return = 0xfffffff0u;

constexpr bool check(bool passed, const char* message)
{
    if !consteval
    {
        if (!passed)
        {
            std::fprintf(stderr, "ee_ram_tlb_test: %s\n", message);
        }
    }
    return passed;
}

using status = ee::e_ee_tlb_lookup_status;

consteval bool test_tlb_page_mask(uint32_t mask)
{
    ee::c_ee_tlb tlb{};
    constexpr uint32_t virt = 0x40000000u;
    constexpr uint32_t even = 0x10000000u;
    constexpr uint32_t odd = 0x20000000u;
    const uint32_t size = ((mask | 0x1fffu) + 1u) / 2u;
    const uint32_t lo0 = ((even >> 12u) << 6u) | (3u << 3u) | 6u;
    const uint32_t lo1 = ((odd >> 12u) << 6u) | (2u << 3u) | 2u;
    if (!check(tlb.write(47u, mask, virt | 0xabu, lo0, lo1),
               "supported page mask at last TLB index"))
    {
        return false;
    }
    for (const uint32_t offset : {0u, size - 1u})
    {
        const auto a = tlb.lookup(virt + offset, 0xabu, true);
        const auto b = tlb.lookup(virt + size + offset, 0xabu, false);
        if (!check(a.m_status == status::TRANSLATED &&
                   a.m_physical_address == even + offset &&
                   a.m_page_size == size && a.m_entry_index == 47u &&
                   !a.m_odd_page && a.m_cache_mode == 3u &&
                   b.m_status == status::TRANSLATED &&
                   b.m_physical_address == odd + offset &&
                   b.m_page_size == size && b.m_odd_page && b.m_cache_mode == 2u,
                   "even/odd PFN, boundary and cache-mode classification"))
        {
            return false;
        }
    }
    if (!check(tlb.lookup(virt + size, 0xabu, true).m_status ==
                   status::PAGE_NOT_DIRTY &&
               tlb.lookup(virt, 0xacu, false).m_status == status::NO_MATCH &&
               tlb.lookup(virt + 2u * size, 0xabu, false).m_status == status::NO_MATCH,
               "modified, ASID and pair boundary are distinct"))
    {
        return false;
    }
    if (!check(tlb.write(47u, mask, virt | 0xabu, lo0 | 1u, lo1) &&
               tlb.lookup(virt, 0xacu, false).m_status == status::NO_MATCH &&
               tlb.write(47u, mask, virt | 0xabu, lo0 | 1u, lo1 | 1u) &&
               tlb.lookup(virt, 0xacu, false).m_status == status::TRANSLATED,
               "both G bits required for global ASID bypass"))
    {
        return false;
    }
    return true;
}

consteval bool test_tlb_attributes_and_overlap()
{
    ee::c_ee_tlb tlb{};
    constexpr uint32_t virt = 0x00400000u;
    constexpr uint32_t lo = (0x100u << 6u) | (7u << 3u) | 6u;
    if (!check(tlb.write(0u, 0u, virt, lo & ~2u, lo) &&
               tlb.lookup(virt, 0u, true).m_status == status::PAGE_INVALID &&
               tlb.lookup(virt + 0x1000u, 0u, true).m_cache_mode == 7u &&
               tlb.lookup(virt + 0x1000u, 0u, true).m_status == status::TRANSLATED,
               "invalid even page does not invalidate odd page"))
    {
        return false;
    }
    if (!check(tlb.write(0u, 0u, virt, lo & ~0x38u, lo) &&
               tlb.lookup(virt, 0u, false).m_status == status::RESERVED_CACHE_MODE &&
               tlb.write(0u, 0u, virt, lo | 0x80000000u, lo) &&
               tlb.lookup(virt + 0x1000u, 0u, false).m_status ==
                   status::SCRATCHPAD_MAPPED &&
               tlb.write(0u, 0u, virt, lo, lo | 0x80000000u) &&
               tlb.lookup(virt + 0x1000u, 0u, false).m_status ==
                   status::RESERVED_ENTRY_BITS,
               "reserved cache/entry bits and scratchpad cannot yield RAM mappings"))
    {
        return false;
    }
    if (!check(tlb.write(0u, 0u, virt, lo, lo) &&
               tlb.write(1u, 0u, virt, lo, lo) &&
               tlb.lookup(virt, 0u, false).m_status == status::MULTIPLE_MATCHES &&
               tlb.lookup(virt, 0u, false).m_physical_address == 0u,
               "multiple matches do not pick an arbitrary PFN"))
    {
        return false;
    }
    return true;
}

consteval bool test_address_routes()
{
    using route = ee::e_ee_address_route;
    constexpr uint32_t starts[] = {0u, 0x20000000u, 0x40000000u,
        0x60000000u, 0x80000000u, 0xa0000000u, 0xc0000000u, 0xe0000000u};
    constexpr route expected[4][8] =
    {
        {route::TLB_MAPPED, route::TLB_MAPPED, route::TLB_MAPPED,
         route::TLB_MAPPED, route::DIRECT_PHYSICAL, route::DIRECT_PHYSICAL,
         route::TLB_MAPPED, route::TLB_MAPPED},
        {route::TLB_MAPPED, route::TLB_MAPPED, route::TLB_MAPPED,
         route::TLB_MAPPED, route::PRIVILEGE_ERROR, route::PRIVILEGE_ERROR,
         route::TLB_MAPPED, route::PRIVILEGE_ERROR},
        {route::TLB_MAPPED, route::TLB_MAPPED, route::TLB_MAPPED,
         route::TLB_MAPPED, route::PRIVILEGE_ERROR, route::PRIVILEGE_ERROR,
         route::PRIVILEGE_ERROR, route::PRIVILEGE_ERROR},
        {route::RESERVED_MODE, route::RESERVED_MODE, route::RESERVED_MODE,
         route::RESERVED_MODE, route::RESERVED_MODE, route::RESERVED_MODE,
         route::RESERVED_MODE, route::RESERVED_MODE}
    };
    for (uint32_t mode = 0u; mode < 4u; ++mode)
    {
        for (uint32_t levels = 0u; levels < 4u; ++levels)
        {
            const uint32_t state = (mode << 3u) | (levels << 1u);
            for (uint32_t segment = 0u; segment < 8u; ++segment)
            {
                for (uint32_t offset : {0u, 0x1fffffffu})
                {
                    const uint32_t address = starts[segment] + offset;
                    const auto actual = ee::ee_classify_address(address, state, 3u);
                    const bool direct_low = (levels & 2u) != 0u && segment < 4u;
                    const route want = direct_low ? route::DIRECT_PHYSICAL :
                        expected[levels != 0u ? 0u : mode][segment];
                    if (!check(actual.m_route == want, "address privilege/segment route") ||
                        (want == route::DIRECT_PHYSICAL &&
                         !check(actual.m_physical_address == (direct_low ? address : offset) &&
                                actual.m_cache_mode == (!direct_low && segment == 4u ? 3u : 2u),
                                "direct mapping and cache attribute")))
                    {
                        return false;
                    }
                }
            }
        }
    }
    for (uint32_t cache = 0u; cache < 8u; ++cache)
    {
        const auto k0 = ee::ee_classify_address(0x9fc00000u, 0u, cache);
        const auto k1 = ee::ee_classify_address(0xbfc00000u, 0u, cache);
        const bool supported = cache == 0u || cache == 2u || cache == 3u || cache == 7u;
        if (!check(k0.m_route == (supported ? route::DIRECT_PHYSICAL :
                                   route::RESERVED_CACHE_MODE) &&
                   (!supported || (k0.m_physical_address == 0x1fc00000u &&
                                   k0.m_cache_mode == cache)) &&
                   k1.m_route == route::DIRECT_PHYSICAL &&
                   k1.m_physical_address == 0x1fc00000u && k1.m_cache_mode == 2u,
                   "KSEG0 Config.K0 versus fixed uncached KSEG1"))
        {
            return false;
        }
    }
    if (!check(ee::ee_classify_address(0x20000000u, 0u, 3u).m_route ==
                   route::TLB_MAPPED &&
               ee::ee_classify_address(0x70000000u, 0u, 3u).m_route ==
                   route::TLB_MAPPED,
               "HLE aliases and scratchpad are not unconditional architectural mappings"))
    {
        return false;
    }
    return true;
}

consteval bool test_access_and_fault_plans()
{
    using access = ee::e_ee_access_status;
    ee::c_ee_tlb tlb{};
    constexpr uint32_t virt = 0x34566000u;
    constexpr uint32_t physical_base = 0x12345000u;
    constexpr uint32_t asid = 0x57u;
    constexpr uint32_t lo = ((physical_base >> 12u) << 6u) | (3u << 3u);
    if (!check(ee::ee_resolve_access(tlb, 0xbfc00000u, 2u << 3u,
                                   3u, asid, false).m_status == access::ADDRESS_ERROR &&
               ee::ee_resolve_access(tlb, virt, 0u, 3u, asid, false).m_status ==
                   access::TLB_REFILL &&
               tlb.write(0u, 0u, virt | asid, lo, lo | 2u) &&
               ee::ee_resolve_access(tlb, virt, 0u, 3u, asid, false).m_status ==
                   access::TLB_INVALID &&
               ee::ee_resolve_access(tlb, virt + 0x1000u, 0u, 3u, asid, true).m_status ==
                   access::TLB_MODIFIED,
               "combined routing preserves privilege, refill, invalid and modified"))
    {
        return false;
    }
    const auto translated = ee::ee_resolve_access(
        tlb, virt + 0x1123u, 0u, 3u, asid, false);
    const auto erl_direct = ee::ee_resolve_access(tlb, virt, ee::STATUS_ERL,
                                                   3u, asid, false);
    if (!check(translated.m_status == access::PHYSICAL &&
               translated.m_physical_address == physical_base + 0x123u &&
               translated.m_cache_mode == 3u &&
               erl_direct.m_status == access::PHYSICAL &&
               erl_direct.m_physical_address == virt &&
               erl_direct.m_cache_mode == 2u,
               "physical bus address is separate from memory backing"))
    {
        return false;
    }
    if (!check(tlb.write(1u, 0u, virt | asid, lo | 6u, lo | 6u) &&
               ee::ee_resolve_access(tlb, virt, 0u, 3u, asid, false).m_status ==
                   access::UNMODELED,
               "duplicate mapping cannot become a fabricated TLB fault"))
    {
        return false;
    }
    constexpr uint32_t bad = 0x92345678u;
    for (uint32_t flags : {0u, ee::STATUS_EXL, ee::STATUS_BEV,
                          ee::STATUS_EXL | ee::STATUS_BEV})
    {
        for (bool write : {false, true})
        {
            const uint32_t common = (flags & ee::STATUS_BEV) != 0u
                ? ee::VEC_COMMON_BEV1 : ee::VEC_COMMON_BEV0;
            const uint32_t refill = (flags & ee::STATUS_EXL) != 0u ? common :
                ((flags & ee::STATUS_BEV) != 0u ? ee::VEC_TLB_REFILL_BEV1 :
                                                 ee::VEC_TLB_REFILL_BEV0);
            for (access kind : {access::TLB_REFILL, access::TLB_INVALID,
                                access::TLB_MODIFIED, access::ADDRESS_ERROR})
            {
                const auto plan = ee::ee_plan_memory_fault(
                    kind, write, bad, flags, 0xabcdef01u, 0x76543f57u);
                if (kind == access::TLB_MODIFIED && !write)
                {
                    if (!check(!plan.m_valid, "modified fault requires a store"))
                    {
                        return false;
                    }
                    continue;
                }
                const bool tlb_fault = kind != access::ADDRESS_ERROR;
                const uint32_t expected_code = kind == access::ADDRESS_ERROR ?
                    (write ? ee::EX_AdES : ee::EX_AdEL) :
                    (kind == access::TLB_MODIFIED ? ee::EX_Mod :
                        (write ? ee::EX_TLBS : ee::EX_TLBL));
                if (!check(plan.m_valid && plan.m_code == expected_code &&
                           plan.m_bad_vaddr == bad &&
                           plan.m_vector == (kind == access::TLB_REFILL ? refill : common) &&
                           plan.m_update_tlb_registers == tlb_fault &&
                           (!tlb_fault || (plan.m_entry_hi == 0x92344057u &&
                                           plan.m_context == 0xabc91a20u)),
                           "fault vector and COP0 register update plan"))
                {
                    return false;
                }
            }
        }
    }
    if (!check(!ee::ee_plan_memory_fault(access::UNMODELED, false, bad, 0u, 0u, 0u).m_valid &&
               !ee::ee_plan_memory_fault(access::PHYSICAL, false, bad, 0u, 0u, 0u).m_valid,
               "nonfault and unmodeled outcomes cannot enter exception handler"))
    {
        return false;
    }
    return true;
}

consteval bool test_scratchpad_mapping()
{
    ee::c_ee_tlb tlb{};
    constexpr uint32_t virt = 0x70000000u;
    constexpr uint32_t asid = 7u;
    constexpr uint32_t lo0 = 0x80004006u; // S, ignored PFN/C, V/D
    constexpr uint32_t lo1 = 0x0000803eu; // ignored PFN/C, V/D
    if (!check(tlb.write(0u, 0u, virt | asid, lo0, lo1),
               "valid scratchpad entry"))
    {
        return false;
    }
    for (uint32_t offset : {0u, 0xfffu, 0x1000u, 0x1fffu,
                            0x2000u, 0x3000u, 0x3fffu})
    {
        const auto hit = tlb.lookup(virt + offset, asid, true);
        const auto resolved = ee::ee_resolve_access(tlb, virt + offset,
                                                     0u, 3u, asid, true);
        if (!check(hit.m_status == status::SCRATCHPAD_MAPPED &&
                   hit.m_scratchpad_offset == offset && hit.m_page_size == 0x4000u &&
                   hit.m_cache_mode == 2u &&
                   resolved.m_status == ee::e_ee_access_status::SCRATCHPAD &&
                   resolved.m_scratchpad_offset == offset &&
                   resolved.m_physical_address == 0u,
                   "full scratchpad region uses a distinct non-bus target"))
        {
            return false;
        }
    }
    if (!check(tlb.lookup(virt + 0x4000u, asid, false).m_status == status::NO_MATCH &&
               tlb.lookup(virt + 0x2000u, asid + 1u, false).m_status == status::NO_MATCH &&
               tlb.write(0u, 0u, virt | asid, lo0 & ~4u, lo1 & ~4u) &&
               tlb.lookup(virt + 0x3000u, asid, true).m_status == status::PAGE_NOT_DIRTY &&
               tlb.write(0u, 0u, virt | asid, lo0 & ~2u, lo1 & ~2u) &&
               tlb.lookup(virt + 0x3000u, asid, false).m_status == status::PAGE_INVALID &&
               tlb.write(0u, 0u, virt | asid, lo0, lo1 & ~4u) &&
               tlb.lookup(virt + 0x2000u, asid, false).m_status ==
                   status::SCRATCHPAD_UNMODELED &&
               tlb.write(0u, 0u, virt | asid | 0x2000u, lo0, lo1) &&
               tlb.lookup(virt + 0x2000u, asid, false).m_status ==
                   status::SCRATCHPAD_UNMODELED,
               "scratchpad ASID, boundary, permissions and malformed entry"))
    {
        return false;
    }
    if (!check(tlb.write(0u, 0u, virt | asid, lo0, lo1) &&
               tlb.write(1u, 0u, (virt + 0x2000u) | asid, 0x1eu, 0x1eu) &&
               tlb.lookup(virt + 0x2000u, asid, false).m_status == status::MULTIPLE_MATCHES &&
               !ee::ee_plan_memory_fault(ee::e_ee_access_status::SCRATCHPAD, false,
                                         virt, 0u, 0u, asid).m_valid,
               "overlapping RAM mapping ambiguous; scratchpad is not a fault"))
    {
        return false;
    }
    return true;
}

constexpr bool test_instruction_data_access()
{
    using kind = ee::e_ee_data_access_kind;
    using result = ee::e_ee_access_status;
    ee::c_ee_tlb tlb{};
    constexpr uint32_t virt = 0x00400000u;
    constexpr uint32_t asid = 0xabu;
    constexpr uint32_t context = 0xab800000u;
    constexpr uint32_t lo0 = (0x100u << 6u) | 0x1eu;
    constexpr uint32_t lo1 = (0x200u << 6u) | 0x1au;
    if (!tlb.write(0u, 0u, virt | asid, lo0, lo1))
    {
        return false;
    }
    constexpr kind kinds[] =
    {
        kind::BYTE, kind::HALFWORD, kind::WORD, kind::DOUBLEWORD,
        kind::GPR_QUADWORD
    };
    for (uint32_t i = 0u; i < 5u; ++i)
    {
        const uint32_t width = 1u << i;
        for (uint32_t offset = 0u; offset < 16u; ++offset)
        {
            for (uint32_t store = 0u; store < 2u; ++store)
            {
                const uint32_t address = virt + 0xff0u + offset;
                const auto plan = ee::ee_plan_data_access(tlb, kinds[i],
                    store != 0u, address, 0u, 3u, context, asid);
                const uint32_t adjusted = i == 4u ? address & ~15u : address;
                if (plan.m_effective_address != address ||
                    plan.m_virtual_address != adjusted || plan.m_width != width)
                {
                    return false;
                }
                if (i != 4u && (offset & (width - 1u)) != 0u)
                {
                    if (plan.m_access.m_status != result::ADDRESS_ERROR ||
                        !plan.m_fault.m_valid || plan.m_fault.m_update_tlb_registers ||
                        plan.m_fault.m_bad_vaddr != address ||
                        plan.m_fault.m_code != (store != 0u ? ee::EX_AdES : ee::EX_AdEL))
                    {
                        return false;
                    }
                }
                else if (plan.m_access.m_status != result::PHYSICAL ||
                         plan.m_access.m_physical_address != 0x100000u + adjusted - virt ||
                         plan.m_access.m_cache_mode != 3u || plan.m_fault.m_valid)
                {
                    return false;
                }
            }
        }
    }
    const auto misaligned_miss = ee::ee_plan_data_access(tlb, kind::WORD,
        false, 0x00500001u, 0u, 3u, context, asid);
    const auto quad_miss = ee::ee_plan_data_access(tlb, kind::GPR_QUADWORD,
        false, 0x0050000fu, 0u, 3u, context, asid);
    const auto modified = ee::ee_plan_data_access(tlb, kind::GPR_QUADWORD,
        true, virt + 0x100fu, 0u, 3u, context, asid);
    const auto high = ee::ee_plan_data_access(tlb, kind::GPR_QUADWORD,
        false, 0xffffffffu, 0u, 3u, context, asid);
    const auto invalid = ee::ee_plan_data_access(tlb, static_cast<kind>(255u),
        false, virt, 0u, 3u, context, asid);
    return misaligned_miss.m_access.m_status == result::ADDRESS_ERROR &&
           misaligned_miss.m_fault.m_code == ee::EX_AdEL &&
           quad_miss.m_access.m_status == result::TLB_REFILL &&
           quad_miss.m_fault.m_bad_vaddr == 0x00500000u &&
           quad_miss.m_fault.m_update_tlb_registers &&
           modified.m_access.m_status == result::TLB_MODIFIED &&
           modified.m_fault.m_code == ee::EX_Mod &&
           modified.m_fault.m_bad_vaddr == virt + 0x1000u &&
           high.m_virtual_address == 0xfffffff0u &&
           high.m_access.m_status == result::TLB_REFILL &&
           invalid.m_width == 0u && invalid.m_access.m_status == result::UNMODELED &&
           !invalid.m_fault.m_valid;
}

static_assert(test_instruction_data_access(), "instruction data access alignment regression");

constexpr bool test_partial_access_plans()
{
    using kind = ee::e_ee_partial_access_kind;
    using result = ee::e_ee_access_status;
    ee::c_ee_tlb tlb{};
    constexpr uint32_t virt = 0x00400000u;
    constexpr uint32_t asid = 0xabu;
    constexpr uint32_t lo0 = (0x100u << 6u) | 0x1eu;
    constexpr uint32_t lo1 = (0x200u << 6u) | 0x1au;
    if (!tlb.write(0u, 0u, virt | asid, lo0, lo1))
    {
        return false;
    }
    constexpr kind kinds[] =
    {
        kind::WORD_LEFT, kind::WORD_RIGHT,
        kind::DOUBLEWORD_LEFT, kind::DOUBLEWORD_RIGHT
    };
    constexpr uint32_t masks[4][8] =
    {
        {1u, 3u, 7u, 15u},
        {15u, 14u, 12u, 8u},
        {1u, 3u, 7u, 15u, 31u, 63u, 127u, 255u},
        {255u, 254u, 252u, 248u, 240u, 224u, 192u, 128u}
    };
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        const uint32_t width = i < 2u ? 4u : 8u;
        for (uint32_t offset = 0u; offset < width; ++offset)
        {
            for (uint32_t store = 0u; store < 2u; ++store)
            {
                const uint32_t address = virt + 0x1000u - width + offset;
                const auto plan = ee::ee_plan_partial_access(tlb, kinds[i],
                    store != 0u, address, 0u, 3u, 0u, asid);
                const bool left = (i & 1u) == 0u;
                if (plan.m_width != width ||
                    plan.m_effective_address != address ||
                    plan.m_aligned_virtual_address != virt + 0x1000u - width ||
                    plan.m_access.m_status != result::PHYSICAL ||
                    plan.m_access.m_physical_address != 0x100000u + address - virt ||
                    plan.m_aligned_physical_address != 0x101000u - width ||
                    plan.m_memory_first_lane != (left ? 0u : offset) ||
                    plan.m_register_first_lane != (left ? width - offset - 1u : 0u) ||
                    plan.m_lane_count != (left ? offset + 1u : width - offset) ||
                    plan.m_memory_byte_mask != masks[i][offset] ||
                    plan.m_sign_extend_word_load !=
                        (store == 0u && width == 4u && (left || offset == 0u)) ||
                    plan.m_fault.m_valid)
                {
                    return false;
                }
            }
        }
    }
    const auto clean_read = ee::ee_plan_partial_access(tlb, kind::WORD_RIGHT,
        false, virt + 0x1003u, 0u, 3u, 0u, asid);
    const auto clean_write = ee::ee_plan_partial_access(tlb, kind::WORD_RIGHT,
        true, virt + 0x1003u, 0u, 3u, 0u, asid);
    const auto miss = ee::ee_plan_partial_access(tlb, kind::DOUBLEWORD_LEFT,
        true, 0x00500005u, 0u, 3u, 0u, asid);
    const auto privilege = ee::ee_plan_partial_access(tlb, kind::WORD_LEFT,
        false, 0x80000001u, 0x10u, 3u, 0u, asid);
    const auto invalid = ee::ee_plan_partial_access(tlb, static_cast<kind>(255u),
        false, virt, 0u, 3u, 0u, asid);
    if (!tlb.write(1u, 0u, 0x00600000u | asid, 0x80000006u, 6u))
    {
        return false;
    }
    const auto scratch = ee::ee_plan_partial_access(tlb, kind::DOUBLEWORD_RIGHT,
        true, 0x00603fffu, 0u, 3u, 0u, asid);
    return clean_read.m_access.m_status == result::PHYSICAL &&
           clean_write.m_access.m_status == result::TLB_MODIFIED &&
           clean_write.m_fault.m_code == ee::EX_Mod &&
           clean_write.m_fault.m_bad_vaddr == virt + 0x1003u &&
           miss.m_access.m_status == result::TLB_REFILL &&
           miss.m_fault.m_code == ee::EX_TLBS &&
           miss.m_fault.m_bad_vaddr == 0x00500005u &&
           privilege.m_access.m_status == result::ADDRESS_ERROR &&
           privilege.m_fault.m_bad_vaddr == 0x80000001u &&
           invalid.m_width == 0u && !invalid.m_fault.m_valid &&
           scratch.m_access.m_status == result::SCRATCHPAD &&
           scratch.m_access.m_scratchpad_offset == 0x3fffu &&
           scratch.m_aligned_scratchpad_offset == 0x3ff8u &&
           scratch.m_memory_byte_mask == 128u && !scratch.m_fault.m_valid;
}

static_assert(test_partial_access_plans(), "left/right partial access regression");

constexpr bool test_instruction_fetch_plans()
{
    using route = ee::e_ee_fetch_route;
    using result = ee::e_ee_access_status;
    constexpr uint32_t ice = 1u << 17u;
    constexpr uint32_t dce = 1u << 16u;
    constexpr uint32_t virt = 0x00400000u;
    constexpr uint32_t asid = 0xabu;
    constexpr uint32_t context = 0xab800000u;
    ee::c_ee_tlb tlb{};
    // Clean cached even page, invalid odd page.
    if (!tlb.write(0u, 0u, virt | asid, (0x100u << 6u) | 0x1au,
                   (0x200u << 6u) | 0x18u) ||
        !tlb.write(1u, 0u, 0x70000000u | asid, 0x8000001au, 0x1au))
    {
        return false;
    }
    for (uint32_t mode = 0u; mode < 8u; ++mode)
    {
        for (uint32_t enables = 0u; enables < 4u; ++enables)
        {
            const uint32_t config = mode | (enables << 16u);
            const auto kseg0 = ee::ee_plan_instruction_fetch(tlb, 0x80000ffcu,
                0u, config, context, asid);
            const auto kseg1 = ee::ee_plan_instruction_fetch(tlb, 0xa0000ffcu,
                0u, config, context, asid);
            const bool valid = mode == 0u || mode == 2u || mode == 3u || mode == 7u;
            const bool cached = (config & ice) != 0u && (mode == 0u || mode == 3u);
            if (kseg0.m_route != (!valid ? route::UNMODELED :
                    cached ? route::CACHED_PHYSICAL : route::CACHE_BYPASS_PHYSICAL) ||
                kseg0.m_fault.m_valid ||
                (valid && (kseg0.m_access.m_physical_address != 0xffcu ||
                           kseg0.m_access.m_cache_mode != mode)) ||
                kseg1.m_route != route::CACHE_BYPASS_PHYSICAL ||
                kseg1.m_access.m_physical_address != 0xffcu ||
                kseg1.m_access.m_cache_mode != 2u || kseg1.m_fault.m_valid)
            {
                return false;
            }
        }
    }
    const auto clean = ee::ee_plan_instruction_fetch(tlb, virt + 0xffcu,
        0x10u, ice, context, asid);
    const auto disabled = ee::ee_plan_instruction_fetch(tlb, virt,
        0x10u, dce, context, asid);
    if (clean.m_route != route::CACHED_PHYSICAL ||
        clean.m_access.m_physical_address != 0x100ffcu ||
        clean.m_access.m_cache_mode != 3u || clean.m_fault.m_valid ||
        disabled.m_route != route::CACHE_BYPASS_PHYSICAL ||
        disabled.m_access.m_cache_mode != 3u)
    {
        return false;
    }
    for (uint32_t flags = 0u; flags < 4u; ++flags)
    {
        const uint32_t status = ((flags & 1u) ? ee::STATUS_EXL : 0u) |
                                ((flags & 2u) ? ee::STATUS_BEV : 0u);
        const uint32_t common = (flags & 2u) ? ee::VEC_COMMON_BEV1 : ee::VEC_COMMON_BEV0;
        const uint32_t refill = (flags & 1u) ? common :
            ((flags & 2u) ? ee::VEC_TLB_REFILL_BEV1 : ee::VEC_TLB_REFILL_BEV0);
        const auto invalid = ee::ee_plan_instruction_fetch(tlb, virt + 0x1000u,
            status, ice, context, asid);
        const auto miss = ee::ee_plan_instruction_fetch(tlb, virt,
            status, ice, context, asid + 1u);
        if (invalid.m_route != route::FAULT ||
            invalid.m_access.m_status != result::TLB_INVALID ||
            invalid.m_fault.m_code != ee::EX_TLBL ||
            invalid.m_fault.m_vector != common ||
            !invalid.m_fault.m_update_tlb_registers ||
            invalid.m_fault.m_bad_vaddr != virt + 0x1000u ||
            miss.m_route != route::FAULT ||
            miss.m_access.m_status != result::TLB_REFILL ||
            miss.m_fault.m_code != ee::EX_TLBL ||
            miss.m_fault.m_vector != refill ||
            !miss.m_fault.m_update_tlb_registers ||
            miss.m_fault.m_bad_vaddr != virt ||
            miss.m_fault.m_entry_hi != (virt | (asid + 1u)))
        {
            return false;
        }
        for (uint32_t offset = 1u; offset < 4u; ++offset)
        {
            const uint32_t pc = 0x00500000u + offset;
            const auto misaligned = ee::ee_plan_instruction_fetch(tlb, pc,
                status, ice, context, asid);
            if (misaligned.m_pc != pc || misaligned.m_route != route::FAULT ||
                misaligned.m_fault.m_code != ee::EX_AdEL ||
                misaligned.m_fault.m_bad_vaddr != pc ||
                misaligned.m_fault.m_vector != common ||
                misaligned.m_fault.m_update_tlb_registers)
            {
                return false;
            }
        }
    }
    const auto scratch = ee::ee_plan_instruction_fetch(tlb, 0x70003ffcu,
        0x10u, ice, context, asid);
    const auto alias_miss = ee::ee_plan_instruction_fetch(tlb, 0x70004000u,
        0u, ice, context, asid);
    const auto privilege = ee::ee_plan_instruction_fetch(tlb, 0x80000000u,
        0x10u, ice, context, asid);
    const auto erl = ee::ee_plan_instruction_fetch(tlb, virt,
        ee::STATUS_ERL, ice, context, asid);
    const auto reserved = ee::ee_plan_instruction_fetch(tlb, virt,
        0x18u, ice, context, asid);
    return scratch.m_route == route::SCRATCHPAD_UNVALIDATED &&
           scratch.m_access.m_status == result::SCRATCHPAD &&
           scratch.m_access.m_scratchpad_offset == 0x3ffcu && !scratch.m_fault.m_valid &&
           alias_miss.m_route == route::FAULT &&
           alias_miss.m_access.m_status == result::TLB_REFILL &&
           privilege.m_route == route::FAULT && privilege.m_fault.m_code == ee::EX_AdEL &&
           erl.m_route == route::CACHE_BYPASS_PHYSICAL &&
           erl.m_access.m_physical_address == virt &&
           reserved.m_route == route::UNMODELED && !reserved.m_fault.m_valid;
}

static_assert(test_instruction_fetch_plans(), "instruction fetch planning regression");

constexpr bool test_data_bus_spans()
{
    using region = ee::e_ee_bus_region;
    using policy = ee::e_ee_data_cache_policy;
    using result = ee::e_ee_access_status;
    constexpr uint32_t ram = 0x02000000u;
    constexpr uint32_t dce = 1u << 16u;
    constexpr uint32_t ice = 1u << 17u;
    constexpr policy disabled[] =
    {
        policy::CACHE_DISABLED, policy::UNMODELED, policy::UNCACHED,
        policy::CACHE_DISABLED, policy::UNMODELED, policy::UNMODELED,
        policy::UNMODELED, policy::UNCACHED_ACCELERATED
    };
    constexpr policy enabled[] =
    {
        policy::CACHED_NO_WRITEBACK_ALLOCATE, policy::UNMODELED, policy::UNCACHED,
        policy::CACHED_WRITEBACK_ALLOCATE, policy::UNMODELED, policy::UNMODELED,
        policy::UNMODELED, policy::UNCACHED_ACCELERATED
    };
    for (uint32_t mode = 0u; mode < 8u; ++mode)
    {
        const ee::s_ee_access_decision access{result::PHYSICAL, 0x1000u, mode};
        if (ee::ee_classify_data_cache(access, 0u) != disabled[mode] ||
            ee::ee_classify_data_cache(access, ice) != disabled[mode] ||
            ee::ee_classify_data_cache(access, dce) != enabled[mode] ||
            ee::ee_classify_data_cache(access, dce | ice) != enabled[mode])
        {
            return false;
        }
    }
    const auto span = [](uint32_t address, uint32_t width,
                         uint64_t size = 0x02000000u)
    {
        return ee::ee_classify_physical_span(address, width, size);
    };
    if (span(0u, 16u) != region::RAM ||
        span(ram - 16u, 16u) != region::RAM ||
        span(ram - 1u, 2u) != region::INVALID_SPAN ||
        span(0u, 1u, 0u) != region::RAM_BACKING_UNAVAILABLE ||
        span(0x1000u, 4u, 0x1003u) != region::RAM_BACKING_UNAVAILABLE ||
        span(ram, 4u, uint64_t{1} << 32u) != region::UNMODELED ||
        span(0x20001000u, 4u) != region::UNMODELED ||
        span(0x80001000u, 4u) != region::UNMODELED ||
        span(0x70000000u, 4u) != region::UNMODELED ||
        span(0x10000000u, 4u) != region::DEVICE ||
        span(0x0fffffffu, 2u) != region::INVALID_SPAN ||
        span(0x11000000u, 16u) != region::VU0_CODE ||
        span(0x11000fffu, 2u) != region::INVALID_SPAN ||
        span(0x10ffffffu, 2u) != region::INVALID_SPAN ||
        span(0x11004000u, 16u) != region::VU0_DATA ||
        span(0x11003fffu, 2u) != region::INVALID_SPAN ||
        span(0x11005000u, 4u) != region::DEVICE ||
        span(0x12ffffffu, 2u) != region::INVALID_SPAN ||
        span(0x1fbfffffu, 2u) != region::INVALID_SPAN ||
        span(0x1fc00000u, 4u) != region::ROM0 ||
        span(0x1fffffffu, 2u) != region::INVALID_SPAN ||
        span(0xffffffffu, 1u) != region::UNMODELED ||
        span(0xffffffffu, 2u) != region::INVALID_SPAN ||
        span(0u, 0u) != region::INVALID_SPAN ||
        span(0u, 17u) != region::INVALID_SPAN)
    {
        return false;
    }
    const ee::s_ee_access_decision scratch{result::SCRATCHPAD, 0u, 2u, 0x3fffu};
    const auto last = ee::ee_plan_data_bus_span(scratch, 1u, dce, ram);
    const auto crossed = ee::ee_plan_data_bus_span(scratch, 2u, dce, ram);
    const auto fault = ee::ee_plan_data_bus_span({result::TLB_REFILL}, 4u, dce, ram);
    ee::c_ee_tlb tlb{};
    const auto left = ee::ee_plan_partial_access(tlb,
        ee::e_ee_partial_access_kind::WORD_LEFT, true,
        0xa0001002u, 0u, 3u, 0u, 0u);
    const auto right = ee::ee_plan_partial_access(tlb,
        ee::e_ee_partial_access_kind::WORD_RIGHT, false,
        0xa0001002u, 0u, 3u, 0u, 0u);
    const auto left_span = ee::ee_plan_partial_bus_span(left, dce, ram);
    const auto right_span = ee::ee_plan_partial_bus_span(right, dce, ram);
    return last.m_region == region::SCRATCHPAD &&
           last.m_address == 0x3fffu && last.m_cache_policy == policy::SCRATCHPAD &&
           crossed.m_region == region::INVALID_SPAN &&
           fault.m_region == region::UNMODELED && fault.m_cache_policy == policy::UNMODELED &&
           left_span.m_region == region::RAM && left_span.m_address == 0x1000u &&
           left_span.m_width == 3u && left_span.m_cache_policy == policy::UNCACHED &&
           right_span.m_region == region::RAM && right_span.m_address == 0x1002u &&
           right_span.m_width == 2u;
}

static_assert(test_data_bus_spans(), "strict physical span and cache policy regression");

constexpr bool test_memory_fault_commit()
{
    using kind = ee::e_ee_memory_request_kind;
    using result = ee::e_ee_memory_fault_commit_result;
    constexpr uint32_t pc = 0x00100000u;
    constexpr uint32_t virt = 0x00400000u;
    constexpr uint32_t context = 0xab800000u;
    constexpr uint32_t asid = 0xabu;
    constexpr uint32_t pending = 0x40028400u;
    constexpr uint32_t dispatch = static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
    constexpr uint32_t exception = static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_EXCEPTION);
    ee::c_ee_tlb tlb{};
    if (!tlb.write(0u, 0u, virt | asid, 0x1au, 0x18u))
    {
        return false;
    }
    for (uint32_t flags = 0u; flags < 16u; ++flags)
    {
        const bool nested = (flags & 1u) != 0u;
        const bool bev = (flags & 2u) != 0u;
        const bool delay = (flags & 4u) != 0u;
        const bool saved_bd = (flags & 8u) != 0u;
        for (uint32_t scenario = 0u; scenario < 5u; ++scenario)
        {
            ee::s_ee_cpu_state cpu{};
            cpu.m_pc = pc;
            cpu.m_exit_reason = dispatch;
            cpu.m_gpr[3] = {0x1122334455667788ull, 0x99aabbccddeeff00ull};
            cpu.m_cop0[ee::COP0_Status] = (nested ? ee::STATUS_EXL : 0u) |
                (bev ? ee::STATUS_BEV : 0u);
            const uint32_t old_status = cpu.m_cop0[ee::COP0_Status];
            cpu.m_cop0[ee::COP0_Cause] = pending | (saved_bd ? ee::CAUSE_BD : 0u);
            cpu.m_cop0[ee::COP0_EPC] = 0x12340000u;
            cpu.m_cop0[ee::COP0_ErrorEPC] = 0x87654320u;
            cpu.m_cop0[ee::COP0_Context] = context | 0x120u;
            cpu.m_cop0[ee::COP0_EntryHi] = 0x00200000u | asid;
            cpu.m_cop0[ee::COP0_EntryLo0] = 0x123u;
            cpu.m_cop0[ee::COP0_Random] = 31u;
            ee::s_ee_memory_fault_request request{};
            request.m_kind = scenario == 4u ? kind::PARTIAL : kind::DATA;
            request.m_data_kind = ee::e_ee_data_access_kind::WORD;
            request.m_partial_kind = ee::e_ee_partial_access_kind::WORD_RIGHT;
            request.m_faulting_pc = pc + (delay ? 4u : 0u);
            request.m_branch_pc = delay ? pc : 0u;
            request.m_in_delay_slot = delay;
            request.m_architectural_state_restored = true;
            request.m_write_access = scenario >= 3u;
            constexpr uint32_t addresses[] =
            {
                0x00500000u, virt + 0x1000u, 0x00500001u,
                virt + 0x1000u, virt + 3u
            };
            constexpr uint32_t codes[] =
            {
                ee::EX_TLBL, ee::EX_TLBL, ee::EX_AdEL, ee::EX_TLBS, ee::EX_Mod
            };
            request.m_address = addresses[scenario];
            bool actual_delay_slot = !delay;
            const uint32_t expected_vector = scenario == 0u && !nested
                ? (bev ? ee::VEC_TLB_REFILL_BEV1 : ee::VEC_TLB_REFILL_BEV0)
                : (bev ? ee::VEC_COMMON_BEV1 : ee::VEC_COMMON_BEV0);
            const uint32_t expected_cause = pending | (codes[scenario] << 2u) |
                ((nested ? saved_bd : delay) ? ee::CAUSE_BD : 0u);
            if (ee::ee_commit_memory_fault_at_boundary(cpu, actual_delay_slot, tlb, request) !=
                    result::COMMITTED ||
                cpu.m_pc != expected_vector || cpu.m_exit_reason != exception ||
                cpu.m_cop0[ee::COP0_Status] != (old_status | ee::STATUS_EXL) ||
                cpu.m_cop0[ee::COP0_Cause] != expected_cause ||
                cpu.m_cop0[ee::COP0_EPC] != (nested ? 0x12340000u : pc) ||
                cpu.m_cop0[ee::COP0_ErrorEPC] != 0x87654320u ||
                cpu.m_cop0[ee::COP0_BadVAddr] != addresses[scenario] ||
                cpu.m_cop0[ee::COP0_Context] != (scenario == 2u ? context | 0x120u :
                    context | ((addresses[scenario] >> 9u) & 0x007ffff0u)) ||
                cpu.m_cop0[ee::COP0_EntryHi] != (scenario == 2u ? 0x00200000u | asid :
                    (addresses[scenario] & 0xffffe000u) | asid) ||
                cpu.m_cop0[ee::COP0_EntryLo0] != 0x123u ||
                cpu.m_cop0[ee::COP0_Random] != 31u ||
                cpu.m_pad != request.m_faulting_pc || actual_delay_slot != delay ||
                cpu.m_gpr[3].m_lo != 0x1122334455667788ull ||
                cpu.m_gpr[3].m_hi != 0x99aabbccddeeff00ull ||
                ee::ee_commit_memory_fault_at_boundary(cpu, actual_delay_slot, tlb, request) !=
                    result::UNSAFE_BOUNDARY)
            {
                return false;
            }
        }
    }
    return true;
}

constexpr bool test_memory_fault_commit_guards()
{
    using kind = ee::e_ee_memory_request_kind;
    using result = ee::e_ee_memory_fault_commit_result;
    constexpr uint32_t dispatch = static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
    ee::c_ee_tlb tlb{};
    ee::s_ee_cpu_state cpu{};
    cpu.m_pc = 0x00400001u;
    cpu.m_exit_reason = dispatch;
    cpu.m_cop0[ee::COP0_BadVAddr] = 0x76543210u;
    cpu.m_cop0[ee::COP0_Cause] = 0x80008400u;
    bool actual_delay_slot = true;
    ee::s_ee_memory_fault_request request{};
    request.m_kind = kind::FETCH;
    request.m_address = cpu.m_pc;
    request.m_faulting_pc = cpu.m_pc;
    if (ee::ee_commit_memory_fault_at_boundary(cpu, actual_delay_slot, tlb, request) !=
            result::UNSAFE_BOUNDARY)
    {
        return false;
    }
    request.m_architectural_state_restored = true;
    request.m_branch_pc = 4u;
    if (ee::ee_commit_memory_fault_at_boundary(cpu, actual_delay_slot, tlb, request) !=
            result::UNSAFE_BOUNDARY)
    {
        return false;
    }
    request.m_branch_pc = 0u;
    request.m_write_access = true;
    if (ee::ee_commit_memory_fault_at_boundary(cpu, actual_delay_slot, tlb, request) !=
            result::INVALID_REQUEST)
    {
        return false;
    }
    request.m_write_access = false;
    cpu.m_cop0[ee::COP0_Status] = ee::STATUS_ERL;
    if (ee::ee_commit_memory_fault_at_boundary(cpu, actual_delay_slot, tlb, request) !=
            result::UNMODELED_LEVEL)
    {
        return false;
    }
    cpu.m_cop0[ee::COP0_Status] = 0u;
    if (cpu.m_pc != 0x00400001u || cpu.m_exit_reason != dispatch ||
        cpu.m_cop0[ee::COP0_BadVAddr] != 0x76543210u ||
        cpu.m_cop0[ee::COP0_Cause] != 0x80008400u || !actual_delay_slot)
    {
        return false;
    }
    if (ee::ee_commit_memory_fault_at_boundary(cpu, actual_delay_slot, tlb, request) !=
            result::COMMITTED ||
        cpu.m_cop0[ee::COP0_EPC] != 0x00400001u ||
        cpu.m_cop0[ee::COP0_BadVAddr] != 0x00400001u ||
        cpu.m_pc != ee::VEC_COMMON_BEV0 || actual_delay_slot)
    {
        return false;
    }
    // Recompute live translation: a mapping installed before entry
    // makes a previously faulting aligned fetch succeed.
    cpu = {};
    cpu.m_pc = 0x00400000u;
    cpu.m_exit_reason = dispatch;
    request.m_address = cpu.m_pc;
    request.m_faulting_pc = cpu.m_pc;
    if (!tlb.write(0u, 0u, cpu.m_pc, 0x1eu, 0x1eu))
    {
        return false;
    }
    return ee::ee_commit_memory_fault_at_boundary(cpu, actual_delay_slot, tlb, request) ==
               result::NO_MODELED_FAULT &&
           cpu.m_pc == 0x00400000u && cpu.m_exit_reason == dispatch &&
           cpu.m_cop0[ee::COP0_Status] == 0u && !actual_delay_slot;
}

static_assert(test_memory_fault_commit(), "precise Level 1 memory fault commitment");
static_assert(test_memory_fault_commit_guards(), "memory fault commitment guards");

// Separate page sizes keep each constant-evaluation budget bounded.
static_assert(test_tlb_page_mask(0u), "4 KiB TLB page regression");
static_assert(test_tlb_page_mask(0x6000u), "16 KiB TLB page regression");
static_assert(test_tlb_page_mask(0x1e000u), "64 KiB TLB page regression");
static_assert(test_tlb_page_mask(0x7e000u), "256 KiB TLB page regression");
static_assert(test_tlb_page_mask(0x1fe000u), "1 MiB TLB page regression");
static_assert(test_tlb_page_mask(0x7fe000u), "4 MiB TLB page regression");
static_assert(test_tlb_page_mask(0x1ffe000u), "16 MiB TLB page regression");
static_assert(test_tlb_attributes_and_overlap(), "TLB attribute regression");
static_assert(test_address_routes(), "EE address route regression");
static_assert(test_access_and_fault_plans(), "EE memory fault plan regression");
static_assert(test_scratchpad_mapping(), "EE scratchpad mapping regression");


void store(ee::ee_memory& memory, uint32_t offset, uint32_t instruction)
{
    memory.write32(k_entry + offset, instruction);
}

uint32_t mtc0(uint32_t rt, uint32_t rd)
{
    return 0x40800000u | (rt << 16u) | (rd << 11u);
}

uint32_t mfc0(uint32_t rt, uint32_t rd)
{
    return 0x40000000u | (rt << 16u) | (rd << 11u);
}

void write_set_entry(ee::ee_memory& memory)
{
    // Actual SDBZ 0x800750c8 _SetTLBEntry guest RAM handler.
    store(memory, 0x00u, 0x2c820030u); // sltiu v0,a0,48
    store(memory, 0x04u, 0x1040000bu); // beq v0,zero,invalid
    store(memory, 0x08u, 0x2402ffffu); // delay: v0=-1
    store(memory, 0x0cu, 0x40840000u); // mtc0 a0,Index
    store(memory, 0x10u, 0x40852800u); // mtc0 a1,PageMask
    store(memory, 0x14u, 0x40865000u); // mtc0 a2,EntryHi
    store(memory, 0x18u, 0x40871000u); // mtc0 a3,EntryLo0
    store(memory, 0x1cu, 0x40881800u); // mtc0 t0,EntryLo1
    store(memory, 0x20u, 0x0000040fu); // sync.p
    store(memory, 0x24u, 0x42000002u); // tlbwi
    store(memory, 0x28u, 0x0000040fu); // sync.p
    store(memory, 0x2cu, 0x03e00008u); // jr ra
    store(memory, 0x30u, 0x0080102du); // move v0,a0 (delay)
    store(memory, 0x34u, 0x03e00008u); // invalid: jr ra
    store(memory, 0x38u, 0x00000000u); // delay: nop
}

ee::s_ee_ram_execution execute(ee::ee_memory& memory,
                              ee::s_ee_cpu_state& cpu)
{
    return ee::ee_execute_ram_callback(
        &cpu, &memory, k_entry, k_return, 128u);
}
}

int main()
{
    ee::ee_memory memory(8u * 1024u * 1024u);
    memory.set_kseg_ram_aliases(true);
    write_set_entry(memory);

    ee::s_ee_cpu_state cpu{};
    cpu.m_gpr[31].m_lo = k_return;
    cpu.m_gpr[4].m_lo = 7u;
    cpu.m_gpr[5].m_lo = 0u;
    cpu.m_gpr[6].m_lo = 0x00400001u;
    cpu.m_gpr[7].m_lo = (0x100u << 6u) | 6u;
    cpu.m_gpr[8].m_lo = (0x101u << 6u) | 2u;
    const ee::s_ee_ram_execution written = execute(memory, cpu);
    ee::s_ee_tlb_entry entry{};
    if (!check(written.m_result == ee::e_ee_ram_result::RETURNED &&
               written.m_tlb_writes == 1u &&
               written.m_instructions == 13u &&
               cpu.m_gpr[2].m_lo == 7u &&
               cpu.m_pc == k_return,
               "real SDBZ handler must commit exactly one TLBWI and return index") ||
        !check(memory.tlb().read(7u, &entry) &&
               entry.m_page_mask == 0u &&
               entry.m_entry_hi == 0x00400001u &&
               entry.m_entry_lo0 == ((0x100u << 6u) | 6u) &&
               entry.m_entry_lo1 == ((0x101u << 6u) | 2u),
               "TLBWI must commit all guest COP0 entry values"))
    {
        return 1;
    }

    uint32_t physical = 0u;
    if (!check(memory.tlb().translate(0x00400124u, 1u, true, &physical) &&
               physical == 0x00100124u,
               "valid dirty even page must translate using PFN") ||
        !check(memory.tlb().translate(0x00401124u, 1u, false, &physical) &&
               physical == 0x00101124u,
               "valid odd page must translate independently") ||
        !check(!memory.tlb().translate(0x00401124u, 1u, true, &physical),
               "clean odd page must not be writable") ||
        !check(!memory.tlb().translate(0x00400124u, 2u, false, &physical),
               "ASID mismatch must not translate") ||
        !check(!memory.tlb().translate(0x00500124u, 1u, false, &physical),
               "unmapped VPN2 must not translate"))
    {
        return 1;
    }

    cpu = {};
    cpu.m_gpr[31].m_lo = k_return;
    cpu.m_gpr[4].m_lo = 48u;
    const ee::s_ee_ram_execution invalid_index = execute(memory, cpu);
    if (!check(invalid_index.m_result == ee::e_ee_ram_result::RETURNED &&
               invalid_index.m_tlb_writes == 0u &&
               invalid_index.m_instructions == 5u &&
               cpu.m_gpr[2].m_lo == 0xffffffffffffffffull,
               "out-of-range handler index must return -1 without TLBWI"))
    {
        return 1;
    }

    cpu = {};
    cpu.m_gpr[31].m_lo = k_return;
    cpu.m_gpr[4].m_lo = 8u;
    cpu.m_gpr[5].m_lo = 0x1234u;
    const ee::s_ee_ram_execution invalid_mask = execute(memory, cpu);
    if (!check(invalid_mask.m_result ==
                   ee::e_ee_ram_result::INVALID_TLB_STATE &&
               invalid_mask.m_pc == k_entry + 0x24u &&
               invalid_mask.m_opcode == 0x42000002u &&
               invalid_mask.m_tlb_writes == 0u &&
               !memory.tlb().read(8u, &entry),
               "unsupported PageMask must fail without writing entry"))
    {
        return 1;
    }

    // TLBR must recover the committed register values from the shared
    // entry table, independent of stale CPU scratch registers.
    store(memory, 0u, mtc0(4u, ee::COP0_Index));
    store(memory, 4u, 0x42000001u); // TLBR
    store(memory, 8u, mfc0(2u, ee::COP0_EntryLo0));
    store(memory, 12u, 0x03e00008u); // jr ra
    store(memory, 16u, 0u);
    cpu = {};
    cpu.m_gpr[31].m_lo = k_return;
    cpu.m_gpr[4].m_lo = 7u;
    const ee::s_ee_ram_execution read = execute(memory, cpu);
    if (!check(read.m_result == ee::e_ee_ram_result::RETURNED &&
               read.m_tlb_writes == 0u &&
               cpu.m_cop0[ee::COP0_PageMask] == 0u &&
               cpu.m_cop0[ee::COP0_EntryHi] == 0x00400001u &&
               cpu.m_cop0[ee::COP0_EntryLo1] == ((0x101u << 6u) | 2u) &&
               cpu.m_gpr[2].m_lo == ((0x100u << 6u) | 6u),
               "TLBR/MFC0 must expose committed entry fields"))
    {
        return 1;
    }

    // TLBP: exact ASID match succeeds, other ASID probes set Index.P.
    store(memory, 0u, mtc0(4u, ee::COP0_EntryHi));
    store(memory, 4u, 0x42000008u); // TLBP
    store(memory, 8u, mfc0(2u, ee::COP0_Index));
    cpu = {};
    cpu.m_gpr[31].m_lo = k_return;
    cpu.m_gpr[4].m_lo = 0x00400001u;
    if (!check(execute(memory, cpu).m_result ==
                   ee::e_ee_ram_result::RETURNED &&
               cpu.m_gpr[2].m_lo == 7u,
               "TLBP must locate matching VPN2 and ASID"))
    {
        return 1;
    }
    cpu = {};
    cpu.m_gpr[31].m_lo = k_return;
    cpu.m_gpr[4].m_lo = 0x00400002u;
    if (!check(execute(memory, cpu).m_result ==
                   ee::e_ee_ram_result::RETURNED &&
               cpu.m_gpr[2].m_lo == 0xffffffff80000000ull,
               "TLBP miss must set Index.P"))
    {
        return 1;
    }

    // No false TLBWR/unknown COP0 success in the bounded callback.
    store(memory, 0u, 0x42000006u);
    cpu = {};
    if (!check(execute(memory, cpu).m_result ==
                   ee::e_ee_ram_result::UNSUPPORTED,
               "unmodeled Random/Wired TLBWR must fail closed"))
    {
        return 1;
    }

    if (!check(test_instruction_data_access(), "instruction data access alignment regression"))
    {
        return 1;
    }
    if (!check(test_partial_access_plans(), "left/right partial access regression"))
    {
        return 1;
    }
    if (!check(test_instruction_fetch_plans(), "instruction fetch planning regression"))
    {
        return 1;
    }
    if (!check(test_data_bus_spans(), "strict physical span and cache policy regression"))
    {
        return 1;
    }
    if (!check(test_memory_fault_commit() && test_memory_fault_commit_guards(),
               "precise Level 1 memory fault commitment"))
    {
        return 1;
    }
    std::puts("ee_ram_tlb_test: PASS");
    return 0;
}
