#include "ee_strict_reset_vif.h"
#include "ee_strict_firmware_pilot_test.h"
#include "ee_ram_interpreter.h"
#include "ee_memory.h"
#include "ee_address_route.h"
#include "ee_memory_fault.h"
#include "ee_memory_bus_plan.h"
#include "ee_memory_fault_commit.h"
#include "ee_exceptions.h"
#include "ee_runtime.h"
#include "ee_strict_native_word.h"
#include "ee_strict_reset.h"
#include <vector>

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


bool test_strict_ram_execution()
{
    using result = ee::e_ee_strict_ram_step_result;
    constexpr uint32_t pc = 0x00400000u;
    constexpr uint32_t data = pc + 0x1000u;
    constexpr uint32_t dispatch = static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
    constexpr uint32_t exception = static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_EXCEPTION);
    constexpr uint32_t lo_code = (1u << 6u) | 0x12u;
    constexpr uint32_t lo_data = (2u << 6u) | 0x16u;
    constexpr uint64_t original = 0x8877665580ff8180ull;
    constexpr uint64_t marker = 0x1122334455667788ull;
    ee::ee_memory memory(0x3000u);
    memory.set_kseg_ram_aliases(true);
    if (!memory.tlb().write(0u, 0u, pc | 0xabu, lo_code, lo_data))
    {
        return false;
    }
    ee::s_ee_cpu_state cpu{};
    bool actual_delay_slot = true;
    const ee::s_ee_strict_ram_evidence evidence{true, true};
    const auto reset = [&](uint32_t primary, uint32_t address, uint32_t rt = 2u)
    {
        cpu = {};
        cpu.m_pc = pc;
        cpu.m_exit_reason = dispatch;
        cpu.m_cop0[ee::COP0_Status] = 0x10u;
        cpu.m_cop0[ee::COP0_EntryHi] = 0xabu;
        cpu.m_gpr[4].m_lo = address + 4u;
        cpu.m_gpr[2] = {marker, marker};
        actual_delay_slot = true;
        memory.write32(0x1000u, (primary << 26u) | (4u << 21u) |
            (rt << 16u) | 0xfffcu);
        memory.write64(0x2000u, original);
    };
    constexpr uint32_t loads[] = {0x20u, 0x24u, 0x21u, 0x25u, 0x23u, 0x27u, 0x37u};
    constexpr uint64_t expected[] =
    {
        0xffffffffffffff80ull, 0x80ull, 0xffffffffffff8180ull,
        0x8180ull, 0xffffffff80ff8180ull, 0x80ff8180ull, original
    };
    for (uint32_t i = 0u; i < 7u; ++i)
    {
        reset(loads[i], data);
        if (ee::ee_execute_strict_ram_step(cpu, memory, actual_delay_slot, evidence) !=
                result::COMMITTED ||
            cpu.m_pc != pc + 4u || cpu.m_exit_reason != dispatch ||
            cpu.m_gpr[2].m_lo != expected[i] || cpu.m_gpr[2].m_hi != marker ||
            memory.read64(0x2000u) != original || !actual_delay_slot)
        {
            return false;
        }
    }
    constexpr uint32_t stores[] = {0x28u, 0x29u, 0x2bu, 0x3fu};
    constexpr uint64_t stored[] =
    {
        0x8877665580ff8188ull, 0x8877665580ff7788ull,
        0x8877665555667788ull, marker
    };
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        reset(stores[i], data);
        if (ee::ee_execute_strict_ram_step(cpu, memory, actual_delay_slot, evidence) !=
                result::COMMITTED || cpu.m_pc != pc + 4u ||
            memory.read64(0x2000u) != stored[i] ||
            cpu.m_gpr[2].m_lo != marker || cpu.m_gpr[2].m_hi != marker)
        {
            return false;
        }
    }
    reset(0x23u, data + 1u);
    if (ee::ee_execute_strict_ram_step(cpu, memory, actual_delay_slot, evidence) !=
            result::FAULT_COMMITTED ||
        cpu.m_pc != ee::VEC_COMMON_BEV0 || cpu.m_exit_reason != exception ||
        cpu.m_cop0[ee::COP0_EPC] != pc || cpu.m_cop0[ee::COP0_BadVAddr] != data + 1u ||
        cpu.m_cop0[ee::COP0_Cause] != (ee::EX_AdEL << 2u) ||
        cpu.m_gpr[2].m_lo != marker || cpu.m_gpr[2].m_hi != marker ||
        memory.read64(0x2000u) != original || actual_delay_slot)
    {
        return false;
    }
    if (!memory.tlb().write(0u, 0u, pc | 0xabu, lo_code, lo_data & ~4u))
    {
        return false;
    }
    reset(0x2bu, data);
    if (ee::ee_execute_strict_ram_step(cpu, memory, actual_delay_slot, evidence) !=
            result::FAULT_COMMITTED ||
        cpu.m_cop0[ee::COP0_Cause] != (ee::EX_Mod << 2u) ||
        cpu.m_pc != ee::VEC_COMMON_BEV0 || memory.read64(0x2000u) != original)
    {
        return false;
    }
    // A load to zero still performs translation and can fault.
    reset(0x23u, 0x00600000u, 0u);
    if (ee::ee_execute_strict_ram_step(cpu, memory, actual_delay_slot, evidence) !=
            result::FAULT_COMMITTED || cpu.m_pc != ee::VEC_TLB_REFILL_BEV0 ||
        cpu.m_cop0[ee::COP0_BadVAddr] != 0x00600000u ||
        cpu.m_gpr[0].m_lo != 0u || memory.read64(0x2000u) != original)
    {
        return false;
    }
    reset(0x23u, data);
    cpu.m_cop0[ee::COP0_EntryHi] = 0xacu;
    if (ee::ee_execute_strict_ram_step(cpu, memory, actual_delay_slot, evidence) !=
            result::FAULT_COMMITTED || cpu.m_pc != ee::VEC_TLB_REFILL_BEV0 ||
        cpu.m_cop0[ee::COP0_BadVAddr] != pc || cpu.m_gpr[2].m_lo != marker)
    {
        return false;
    }
    reset(0x23u, data);
    if (ee::ee_execute_strict_ram_step(cpu, memory, actual_delay_slot, {}) !=
            result::UNSAFE_BOUNDARY ||
        cpu.m_pc != pc || cpu.m_gpr[2].m_lo != marker || !actual_delay_slot)
    {
        return false;
    }
    // Refuse cached data, missing RAM backing, and HLE-only aliases.
    constexpr uint32_t rejected_pfn[] = {(2u << 6u) | 0x1eu, (3u << 6u) | 0x16u,
                                        (0x20000u << 6u) | 0x16u};
    for (const uint32_t lo : rejected_pfn)
    {
        if (!memory.tlb().write(0u, 0u, pc | 0xabu, lo_code, lo))
        {
            return false;
        }
        reset(0x23u, data);
        cpu.m_cop0[ee::COP0_Config] = 1u << 16u;
        if (ee::ee_execute_strict_ram_step(cpu, memory, actual_delay_slot, evidence) !=
                result::UNMODELED_ACCESS ||
            cpu.m_pc != pc || cpu.m_exit_reason != dispatch ||
            cpu.m_gpr[2].m_lo != marker || memory.read64(0x2000u) != original ||
            !actual_delay_slot)
        {
            return false;
        }
    }
    reset(0x03u, data);
    return ee::ee_execute_strict_ram_step(cpu, memory, actual_delay_slot, evidence) ==
               result::UNSUPPORTED_INSTRUCTION &&
           cpu.m_pc == pc && cpu.m_gpr[2].m_lo == marker &&
           memory.read64(0x2000u) == original && actual_delay_slot;
}

bool test_strict_ram_wide_execution()
{
    using result = ee::e_ee_strict_ram_step_result;
    constexpr uint32_t pc = 0x00400000u;
    constexpr uint32_t data = pc + 0x1000u;
    constexpr uint32_t dispatch = static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
    constexpr uint32_t code_lo = (1u << 6u) | 0x12u;
    constexpr uint32_t data_lo = (2u << 6u) | 0x16u;
    constexpr ee::s_ee_u128 original{0x8877665580ff8180ull, 0xffeeddccbbaa9988ull};
    constexpr ee::s_ee_u128 marker{0x1122334455667788ull, 0x123456789abcdef0ull};
    ee::ee_memory memory(0x3000u);
    if (!memory.tlb().write(0u, 0u, pc | 0xabu, code_lo, data_lo))
    {
        return false;
    }
    ee::s_ee_cpu_state cpu{};
    bool actual_delay_slot = true;
    const ee::s_ee_strict_ram_evidence evidence{true, true};
    const auto reset = [&](uint32_t primary, uint32_t address, uint32_t rt = 2u)
    {
        cpu = {};
        cpu.m_pc = pc;
        cpu.m_exit_reason = dispatch;
        cpu.m_cop0[ee::COP0_Status] = 0x10u;
        cpu.m_cop0[ee::COP0_EntryHi] = 0xabu;
        cpu.m_gpr[4].m_lo = address;
        cpu.m_gpr[2] = marker;
        actual_delay_slot = true;
        memory.write32(0x1000u, (primary << 26u) | (4u << 21u) | (rt << 16u));
        memory.write128(0x2000u, &original);
    };
    const auto same_memory = [&](const ee::s_ee_u128& expected)
    {
        ee::s_ee_u128 actual{};
        memory.read128(0x2000u, &actual);
        return actual.m_lo == expected.m_lo && actual.m_hi == expected.m_hi;
    };
    for (uint32_t offset = 0u; offset < 16u; ++offset)
    {
        reset(0x1eu, data + offset);
        if (ee::ee_execute_strict_ram_step(cpu, memory, actual_delay_slot, evidence) !=
                result::COMMITTED || cpu.m_pc != pc + 4u ||
            cpu.m_gpr[2].m_lo != original.m_lo || cpu.m_gpr[2].m_hi != original.m_hi ||
            !same_memory(original))
        {
            return false;
        }
        reset(0x1fu, data + offset);
        if (ee::ee_execute_strict_ram_step(cpu, memory, actual_delay_slot, evidence) !=
                result::COMMITTED || cpu.m_pc != pc + 4u || !same_memory(marker) ||
            cpu.m_gpr[2].m_lo != marker.m_lo || cpu.m_gpr[2].m_hi != marker.m_hi)
        {
            return false;
        }
    }

    // Independent expected words, using the pinned PCSX2 left/right
    // shift-and-mask equations rather than the strict lane planner.
    constexpr uint32_t load_op[] = {0x22u, 0x26u, 0x1au, 0x1bu};
    constexpr uint32_t store_op[] = {0x2au, 0x2eu, 0x2cu, 0x2du};
    for (uint32_t family = 0u; family < 4u; ++family)
    {
        const uint32_t width = family < 2u ? 4u : 8u;
        const bool left = (family & 1u) == 0u;
        const uint64_t full = width == 4u ? 0xffffffffull : ~uint64_t{0};
        for (uint32_t offset = 0u; offset < width; ++offset)
        {
            const uint32_t shift = (left ? width - 1u - offset : offset) * 8u;
            const uint64_t load_mask = left ? (full << shift) & full : full >> shift;
            const uint64_t raw = original.m_lo & full;
            const uint64_t shifted = left ? raw << shift : raw >> shift;
            uint64_t expected = (marker.m_lo & ~load_mask) | (shifted & load_mask);
            if (width == 4u && (left || offset == 0u))
            {
                expected &= full;
                if ((expected & 0x80000000ull) != 0u)
                {
                    expected |= ~full;
                }
            }
            reset(load_op[family], data + offset);
            if (ee::ee_execute_strict_ram_step(cpu, memory, actual_delay_slot, evidence) !=
                    result::COMMITTED || cpu.m_pc != pc + 4u ||
                cpu.m_gpr[2].m_lo != expected || cpu.m_gpr[2].m_hi != marker.m_hi ||
                !same_memory(original))
            {
                return false;
            }
            const uint64_t store_mask = left ? full >> shift : (full << shift) & full;
            const uint64_t source = marker.m_lo & full;
            const uint64_t store_value = left ? source >> shift : source << shift;
            const ee::s_ee_u128 merged
            {
                (original.m_lo & ~store_mask) | (store_value & store_mask), original.m_hi
            };
            reset(store_op[family], data + offset);
            if (ee::ee_execute_strict_ram_step(cpu, memory, actual_delay_slot, evidence) !=
                    result::COMMITTED || cpu.m_pc != pc + 4u || !same_memory(merged) ||
                cpu.m_gpr[2].m_lo != marker.m_lo || cpu.m_gpr[2].m_hi != marker.m_hi)
            {
                return false;
            }
        }
    }
    reset(0x1eu, data + 15u, 0u);
    cpu.m_gpr[0] = marker; // Host corruption must not leak into architectural zero.
    if (ee::ee_execute_strict_ram_step(cpu, memory, actual_delay_slot, evidence) !=
            result::COMMITTED || cpu.m_gpr[0].m_lo != marker.m_lo ||
        cpu.m_gpr[0].m_hi != marker.m_hi || !same_memory(original))
    {
        return false;
    }
    reset(0x1fu, data + 15u, 0u);
    cpu.m_gpr[0] = marker;
    if (ee::ee_execute_strict_ram_step(cpu, memory, actual_delay_slot, evidence) !=
            result::COMMITTED || !same_memory({0u, 0u}))
    {
        return false;
    }
    if (!memory.tlb().write(0u, 0u, pc | 0xabu, code_lo, data_lo & ~4u))
    {
        return false;
    }
    for (const uint32_t op : {0x1fu, 0x2au, 0x2eu, 0x2cu, 0x2du})
    {
        reset(op, data + 7u);
        if (ee::ee_execute_strict_ram_step(cpu, memory, actual_delay_slot, evidence) !=
                result::FAULT_COMMITTED || cpu.m_pc != ee::VEC_COMMON_BEV0 ||
            cpu.m_cop0[ee::COP0_Cause] != (ee::EX_Mod << 2u) ||
            cpu.m_cop0[ee::COP0_BadVAddr] != (op == 0x1fu ? data : data + 7u) ||
            !same_memory(original) || cpu.m_gpr[2].m_lo != marker.m_lo ||
            cpu.m_gpr[2].m_hi != marker.m_hi)
        {
            return false;
        }
    }
    reset(0x26u, 0x00600003u, 0u);
    if (ee::ee_execute_strict_ram_step(cpu, memory, actual_delay_slot, evidence) !=
            result::FAULT_COMMITTED || cpu.m_pc != ee::VEC_TLB_REFILL_BEV0 ||
        cpu.m_cop0[ee::COP0_BadVAddr] != 0x00600003u || !same_memory(original))
    {
        return false;
    }
    // A backing truncated inside a 16-byte transfer must not commit either half.
    ee::ee_memory short_memory(0x2008u);
    if (!short_memory.tlb().write(0u, 0u, pc | 0xabu, code_lo, data_lo))
    {
        return false;
    }
    reset(0x1fu, data + 3u);
    short_memory.write32(0x1000u, (0x1fu << 26u) | (4u << 21u) | (2u << 16u));
    short_memory.write64(0x2000u, original.m_lo);
    return ee::ee_execute_strict_ram_step(cpu, short_memory, actual_delay_slot, evidence) ==
               result::UNMODELED_ACCESS &&
           cpu.m_pc == pc && cpu.m_exit_reason == dispatch &&
           short_memory.read64(0x2000u) == original.m_lo &&
           cpu.m_gpr[2].m_lo == marker.m_lo && cpu.m_gpr[2].m_hi == marker.m_hi &&
           actual_delay_slot;
}

bool test_strict_ram_handler_execution(bool rom_backed)
{
    using result = ee::e_ee_strict_ram_step_result;
    constexpr uint32_t pc = 0x00400000u;
    constexpr uint32_t data = 0x00601000u;
    constexpr uint32_t dispatch = static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
    constexpr uint32_t exception = static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_EXCEPTION);
    constexpr uint32_t code_lo = (1u << 6u) | 0x12u;
    constexpr uint32_t data_lo = (2u << 6u) | 0x16u;
    constexpr uint64_t marker = 0x1234567887654321ull;
    constexpr uint32_t original = 0x80ff8180u;
    const ee::s_ee_strict_ram_evidence plain{true, true};
    std::vector<uint8_t> rom(rom_backed ? ee::s_ee_strict_rom0_view::k_size : 0u);
    const ee::s_ee_strict_ram_evidence handler{true, true, true, {rom.data(), rom.size()}};
    const uint32_t user_status = 0x10u | (rom_backed ? ee::STATUS_BEV : 0u);
    const auto mfc = [](uint32_t rt, uint32_t rd)
    {
        return 0x40000000u | (rt << 16u) | (rd << 11u);
    };
    const auto mtc = [](uint32_t rt, uint32_t rd)
    {
        return 0x40800000u | (rt << 16u) | (rd << 11u);
    };
    for (uint32_t scenario = 0u; scenario < 3u; ++scenario)
    {
        ee::ee_memory memory(0x3000u);
        if (!memory.tlb().write(0u, 0u, pc | 0xabu, code_lo, code_lo))
        {
            return false;
        }
        if (scenario != 0u &&
            !memory.tlb().write(1u, 0u, (data & ~0x1fffu) | 0xabu,
                0x12u, scenario == 1u ? data_lo & ~2u : data_lo & ~4u))
        {
            return false;
        }
        const bool write = scenario == 2u;
        const uint32_t vector = rom_backed
            ? (scenario == 0u ? ee::VEC_TLB_REFILL_BEV1 : ee::VEC_COMMON_BEV1)
            : (scenario == 0u ? ee::VEC_TLB_REFILL_BEV0 : ee::VEC_COMMON_BEV0);
        const uint32_t vector_physical = vector & 0x1fffffffu;
        const auto put = [&](uint32_t address, uint32_t word)
        {
            if (rom_backed)
            {
                const uint32_t offset = address - ee::s_ee_strict_rom0_view::k_base;
                for (uint32_t i = 0u; i < 4u; ++i)
                {
                    rom[offset + i] = static_cast<uint8_t>(word >> (i * 8u));
                }
            }
            else
            {
                memory.write32(address, word);
            }
        };
        memory.write32(0x1000u, ((write ? 0x2bu : 0x23u) << 26u) |
            (4u << 21u) | (2u << 16u));
        memory.write32(0x2000u, original);
        const uint32_t instructions[] =
        {
            mfc(26u, ee::COP0_BadVAddr),
            mfc(27u, ee::COP0_EntryHi),
            0x24080001u, // addiu t0,zero,1
            mtc(8u, ee::COP0_Index),
            0x34090012u, // ori t1,zero,0x12
            mtc(9u, ee::COP0_EntryLo0),
            0x340a0000u | data_lo, // ori t2,zero, repaired EntryLo1
            mtc(10u, ee::COP0_EntryLo1),
            mtc(0u, ee::COP0_PageMask),
            0x42000002u, // TLBWI
            0x42000018u  // ERET
        };
        for (uint32_t i = 0u; i < sizeof(instructions) / sizeof(instructions[0]); ++i)
        {
            put(vector_physical + i * 4u, instructions[i]);
        }
        ee::s_ee_cpu_state cpu{};
        cpu.m_pc = pc;
        cpu.m_exit_reason = dispatch;
        cpu.m_cop0[ee::COP0_Status] = user_status;
        cpu.m_cop0[ee::COP0_Config] = 2u; // uncached KSEG0
        cpu.m_cop0[ee::COP0_EntryHi] = 0xabu;
        cpu.m_gpr[4].m_lo = data;
        cpu.m_gpr[2] = {marker, marker};
        bool actual_delay_slot = true;
        if (ee::ee_execute_strict_ram_step(cpu, memory, actual_delay_slot, plain) !=
                result::FAULT_COMMITTED ||
            cpu.m_pc != vector || cpu.m_exit_reason != exception ||
            cpu.m_cop0[ee::COP0_EPC] != pc ||
            cpu.m_cop0[ee::COP0_Cause] != ((write ? ee::EX_Mod : ee::EX_TLBL) << 2u) ||
            cpu.m_cop0[ee::COP0_Status] != (user_status | ee::STATUS_EXL) ||
            cpu.m_gpr[2].m_lo != marker || memory.read32(0x2000u) != original ||
            actual_delay_slot)
        {
            return false;
        }
        if (ee::ee_execute_strict_ram_step(cpu, memory, actual_delay_slot, plain) !=
                result::UNSAFE_BOUNDARY ||
            cpu.m_pc != vector || cpu.m_exit_reason != exception)
        {
            return false;
        }
        // A rejected first instruction must preserve the pending exception.
        put(vector_physical, 0x42000006u); // unsupported TLBWR
        if (ee::ee_execute_strict_ram_step(cpu, memory, actual_delay_slot, handler) !=
                result::UNSUPPORTED_INSTRUCTION ||
            cpu.m_pc != vector || cpu.m_exit_reason != exception ||
            cpu.m_cop0[ee::COP0_EPC] != pc)
        {
            return false;
        }
        put(vector_physical, instructions[0]);
        for (uint32_t i = 0u; i < sizeof(instructions) / sizeof(instructions[0]); ++i)
        {
            if (cpu.m_pc != vector + i * 4u ||
                ee::ee_execute_strict_ram_step(cpu, memory, actual_delay_slot, handler) !=
                    result::COMMITTED || cpu.m_exit_reason != dispatch)
            {
                return false;
            }
        }
        if (cpu.m_pc != pc || cpu.m_cop0[ee::COP0_Status] != user_status ||
            cpu.m_gpr[26].m_lo != data ||
            cpu.m_gpr[27].m_lo != ((data & ~0x1fffu) | 0xabu) ||
            memory.tlb().lookup(data, 0xabu, true).m_status != ee::e_ee_tlb_lookup_status::TRANSLATED ||
            memory.read32(0x2000u) != original ||
            ee::ee_execute_strict_ram_step(cpu, memory, actual_delay_slot, plain) !=
                result::COMMITTED ||
            cpu.m_pc != pc + 4u || cpu.m_gpr[2].m_hi != marker ||
            cpu.m_gpr[2].m_lo != (write ? marker : 0xffffffff80ff8180ull) ||
            memory.read32(0x2000u) != (write ? static_cast<uint32_t>(marker) : original))
        {
            return false;
        }
        // User-mode ERET is not silently executed by the strict pilot.
        memory.write32(0x1004u, 0x42000018u);
        if (ee::ee_execute_strict_ram_step(cpu, memory, actual_delay_slot, handler) !=
                result::UNSUPPORTED_INSTRUCTION ||
            cpu.m_pc != pc + 4u || cpu.m_cop0[ee::COP0_Status] != user_status)
        {
            return false;
        }
    }
    return true;
}

bool test_strict_rom0_native_fetch()
{
    using result = ee::e_ee_strict_ram_step_result;
    std::vector<uint8_t> rom(ee::s_ee_strict_rom0_view::k_size, 0u);
    const ee::s_ee_strict_rom0_view view{rom.data(), rom.size()};
    ee::ee_memory memory(0x3000u);
    ee::s_ee_cpu_state cpu{};
    cpu.m_exit_reason = static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
    cpu.m_pc = 0xbfc00000u;
    cpu.m_gpr[4].m_lo = 0xa0002000u;
    rom[2] = 0x82u;
    rom[3] = 0x8cu; // lw r2,0(r4)
    bool delay = true;
    const ee::s_ee_strict_ram_evidence evidence{true, true, false, view};
    ee::s_ee_strict_native_word_context context{&memory, &delay, evidence};
    uint8_t* bytes = nullptr;
    uint32_t lane = 0u;
    if (ee::ee_strict_native_prepare_transfer(&cpu, &context, cpu.m_pc,
            0x8c820000u, &bytes, &lane) != static_cast<uint32_t>(result::COMMITTED) ||
        bytes != memory.physical_ram_span(0x2000u, 4u))
    {
        return false;
    }
    cpu.m_gpr[4].m_lo = 0xbfc00000u;
    rom[3] = 0xacu; // sw r2,0(r4), ROM data access must refuse
    if (ee::ee_strict_native_prepare_transfer(&cpu, &context, cpu.m_pc,
            0xac820000u, &bytes, &lane) != static_cast<uint32_t>(result::UNMODELED_ACCESS) ||
        bytes != nullptr || cpu.m_pc != 0xbfc00000u ||
        ee::ee_execute_strict_ram_step(cpu, memory, delay, evidence) != result::UNMODELED_ACCESS ||
        ee::ee_read_instruction_word(rom.data()) != 0xac820000u)
    {
        return false;
    }
    const auto last = ee::ee_plan_instruction_fetch(memory.tlb(), 0xbffffffcu, 0u, 0u, 0u, 0u);
    const auto user = ee::ee_plan_instruction_fetch(memory.tlb(), 0xbfc00000u, 0x10u, 0u, 0u, 0u);
    const auto cached = ee::ee_plan_instruction_fetch(memory.tlb(), 0x9fc00000u, 0u, 3u, 0u, 0u);
    const ee::s_ee_strict_rom0_view short_view{rom.data(), rom.size() - 1u};
    return view.fetch_span(last) == rom.data() + rom.size() - 4u &&
           short_view.fetch_span(last) == nullptr &&
           view.fetch_span(user) == nullptr && view.fetch_span(cached) == nullptr;
}

bool test_strict_reset_prefix()
{
    using result = ee::e_ee_reset_prefix_result;
    std::vector<uint8_t> rom(ee::s_ee_strict_rom0_view::k_size, 0u);
    const ee::s_ee_strict_rom0_view view{rom.data(), rom.size()};
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    constexpr uint32_t reset_status = ee::STATUS_ERL | ee::STATUS_BEV;
    if (cpu.m_cop0[ee::COP0_Status] != reset_status ||
        cpu.m_cop0[ee::COP0_Random] != 47u ||
        cpu.m_cop0[ee::COP0_Wired] != 0u ||
        ee::ee_execute_strict_reset_prefix(cpu, {}).m_result != result::FETCH_UNAVAILABLE ||
        ee::ee_execute_strict_reset_prefix(cpu, view).m_result != result::UNSUPPORTED_OPCODE ||
        cpu.m_pc != 0xbfc00000u || cpu.m_cop0[ee::COP0_Status] != reset_status)
    {
        return false;
    }
    rom[1] = 0x60u;
    rom[2] = 0x80u;
    rom[3] = 0x40u;
    cpu.m_gpr[0] = {0xdeadbeefull, 0x12345678ull};
    cpu.m_gpr[3] = {0x76543210ull, 0x11223344ull};
    auto bad = cpu;
    bad.m_cop0[ee::COP0_Status] = 0u;
    if (ee::ee_execute_strict_reset_prefix(bad, view).m_result != result::UNSAFE_BOUNDARY ||
        bad.m_pc != 0xbfc00000u || bad.m_cop0[ee::COP0_Status] != 0u)
    {
        return false;
    }
    const auto step = ee::ee_execute_strict_reset_prefix(cpu, view);
    return step.m_result == result::COMMITTED && step.m_opcode == 0x40806000u &&
           cpu.m_pc == 0xbfc00004u && cpu.m_cop0[ee::COP0_Status] == 0u &&
           cpu.m_cop0[ee::COP0_Random] == 47u &&
           cpu.m_gpr[0].m_lo == 0xdeadbeefull &&
           cpu.m_gpr[3].m_lo == 0x76543210ull && cpu.m_gpr[3].m_hi == 0x11223344ull &&
           ee::ee_execute_strict_reset_prefix(cpu, view).m_result == result::UNSAFE_BOUNDARY &&
           cpu.m_pc == 0xbfc00004u;
}

bool test_strict_reset_register_execution()
{
    using result = ee::e_ee_reset_prefix_result;
    std::vector<uint8_t> rom(ee::s_ee_strict_rom0_view::k_size, 0u);
    const ee::s_ee_strict_rom0_view view{rom.data(), rom.size()};
    const auto put = [&](uint32_t offset, uint32_t word)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            rom[offset + i] = static_cast<uint8_t>(word >> (8u * i));
        }
    };
    const uint32_t words[] =
    {
        0x40806000u, 0u, 0u, 0x40087800u, 0u, 0x3108ff00u,
        0x34092e00u, 0x11090006u, 0u, 0x3c18a000u, 0x37181000u,
        0x3c0fbfc0u, 0x10000004u, 0x25ef0133u, 0x3c18a001u,
        0x3c0fbfc0u, 0x25ef012cu
    };
    for (uint32_t i = 0u; i < sizeof(words) / sizeof(words[0]); ++i)
    {
        put(i * 4u, words[i]);
    }
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    if (ee::ee_execute_strict_reset_prefix(cpu, view).m_result != result::COMMITTED)
    {
        return false;
    }
    uint32_t retired = 1u;
    while (retired < 12u)
    {
        const auto step = ee::ee_execute_strict_rom_register_step(cpu, view, 12u - retired);
        if (step.m_result != result::COMMITTED || step.m_retired == 0u ||
            step.m_retired > 12u - retired)
        {
            return false;
        }
        retired += step.m_retired;
    }
    if (cpu.m_pc != 0xbfc00044u || cpu.m_gpr[8].m_lo != 0x2e00u ||
        cpu.m_gpr[24].m_lo != 0xffffffffa0010000ull ||
        cpu.m_gpr[15].m_lo != 0xffffffffbfc0012cull)
    {
        return false;
    }
    // Branch operands sampled before the slot, even if it overwrites
    // an operand. Upper GPR half remains unchanged.
    cpu.m_pc = 0xbfc00100u;
    cpu.m_gpr[2] = {1u, 0x1234u};
    cpu.m_gpr[3].m_lo = 1u;
    put(0x100u, 0x10430003u); // beq r2,r3,+3
    put(0x104u, 0x24420001u); // addiu r2,r2,1
    if (ee::ee_execute_strict_rom_register_step(cpu, view, 1u).m_result != result::BUDGET_EXHAUSTED ||
        cpu.m_pc != 0xbfc00100u || cpu.m_gpr[2].m_lo != 1u)
    {
        return false;
    }
    // Check both decode refusal and a recognized SD with unavailable
    // backing in the delay slot. Neither may commit the branch.
    for (const uint32_t word : {0x00000001u, 0xfc820000u})
    {
        put(0x104u, word);
        const auto refused = ee::ee_execute_strict_rom_register_step(cpu, view, 2u);
        const auto expected = word == 0xfc820000u ? result::DATA_UNAVAILABLE :
            result::UNSUPPORTED_OPCODE;
        if (refused.m_result != expected || refused.m_retired != 0u ||
            refused.m_detail_pc != 0xbfc00104u ||
            cpu.m_pc != 0xbfc00100u || cpu.m_gpr[2].m_lo != 1u ||
            cpu.m_gpr[2].m_hi != 0x1234u || cpu.m_gpr[3].m_lo != 1u)
        {
            return false;
        }
    }
    put(0x104u, 0x24420001u);
    if (ee::ee_execute_strict_rom_register_step(cpu, view, 2u).m_retired != 2u ||
        cpu.m_pc != 0xbfc00110u || cpu.m_gpr[2].m_lo != 2u || cpu.m_gpr[2].m_hi != 0x1234u)
    {
        return false;
    }
    cpu.m_pc = 0xbfc00100u;
    if (ee::ee_execute_strict_rom_register_step(cpu, view, 2u).m_retired != 2u ||
        cpu.m_pc != 0xbfc00108u || cpu.m_gpr[2].m_lo != 3u)
    {
        return false;
    }
    cpu.m_pc = 0xbfc00100u;
    cpu.m_cop0[ee::COP0_Status] = ee::STATUS_ERL;
    return ee::ee_execute_strict_rom_register_step(cpu, view, 2u).m_result == result::UNSAFE_BOUNDARY &&
        cpu.m_pc == 0xbfc00100u && cpu.m_gpr[2].m_lo == 3u;
}

bool test_strict_reset_rom_data()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xbfc00000u;
    std::vector<uint8_t> rom(ee::s_ee_strict_rom0_view::k_size, 0u);
    const ee::s_ee_strict_rom0_view view{rom.data(), rom.size()};
    const auto put = [&](uint32_t offset, uint32_t word)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            rom[offset + i] = static_cast<uint8_t>(word >> (i * 8u));
        }
    };
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_cop0[ee::COP0_Status] = 0u;
    cpu.m_gpr[4].m_lo = 0xffffffffbfc01001ull;
    cpu.m_gpr[2] = {0x11223344u, 0x55667788u};
    rom[0x1000u] = 0xe5u;
    put(0u, 0x9082ffffu); // lbu r2,-1(r4)
    if (ee::ee_execute_strict_rom_register_step(cpu, view, 1u).m_retired != 1u ||
        cpu.m_pc != pc + 4u || cpu.m_gpr[2].m_lo != 0xe5u ||
        cpu.m_gpr[2].m_hi != 0x55667788u)
    {
        return false;
    }
    // ADDU wraps a low 32-bit sum, sign-extends and preserves upper half.
    cpu.m_gpr[3].m_lo = 0x123456787fffffffull;
    cpu.m_gpr[5].m_lo = 0xabcdef0000000001ull;
    put(4u, 0x00651021u); // addu r2,r3,r5
    if (ee::ee_execute_strict_rom_register_step(cpu, view, 1u).m_retired != 1u ||
        cpu.m_gpr[2].m_lo != 0xffffffff80000000ull || cpu.m_gpr[2].m_hi != 0x55667788u)
    {
        return false;
    }
    put(8u, 0x9080ffffu); // lbu zero,-1(r4)
    cpu.m_gpr[0] = {0x9999u, 0x7777u};
    if (ee::ee_execute_strict_rom_register_step(cpu, view, 1u).m_retired != 1u ||
        cpu.m_gpr[0].m_lo != 0x9999u || cpu.m_gpr[0].m_hi != 0x7777u)
    {
        return false;
    }
    for (const uint32_t address : {0x9fc01001u, 0xa0001001u, 0xbfc00000u, 0xc0000001u, 0x00401001u})
    {
        cpu.m_pc = pc + 8u;
        cpu.m_gpr[4].m_lo = address;
        const auto step = ee::ee_execute_strict_rom_register_step(cpu, view, 1u);
        if (step.m_result != result::DATA_UNAVAILABLE || step.m_detail_pc != pc + 8u ||
            step.m_data_address != address - 1u || step.m_retired != 0u ||
            cpu.m_pc != pc + 8u || cpu.m_gpr[0].m_lo != 0x9999u)
        {
            return false;
        }
    }
    cpu.m_pc = pc + 12u;
    cpu.m_gpr[4].m_lo = 0xffffffffbfffffffull;
    rom.back() = 0xffu;
    put(12u, 0x90820000u);
    if (ee::ee_execute_strict_rom_register_step(cpu, view, 1u).m_retired != 1u ||
        cpu.m_gpr[2].m_lo != 0xffu)
    {
        return false;
    }
    // A failed delay-slot read refuses the entire non-linking branch.
    put(16u, 0x10000003u);
    put(20u, 0x90820000u);
    cpu.m_gpr[4].m_lo = 0xa0001000u;
    const auto rejected = ee::ee_execute_strict_rom_register_step(cpu, view, 2u);
    if (rejected.m_result != result::DATA_UNAVAILABLE || rejected.m_detail_pc != pc + 20u ||
        rejected.m_opcode != 0x90820000u || rejected.m_data_address != 0xa0001000u ||
        cpu.m_pc != pc + 16u || cpu.m_gpr[2].m_lo != 0xffu)
    {
        return false;
    }
    cpu.m_gpr[4].m_lo = 0xbfc01000u;
    return ee::ee_execute_strict_rom_register_step(cpu, view, 2u).m_retired == 2u &&
           cpu.m_pc == pc + 32u && cpu.m_gpr[2].m_lo == 0xe5u &&
           cpu.m_gpr[2].m_hi == 0x55667788u;
}

bool test_strict_reset_rom_word_and_logic()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xbfc00000u;
    std::vector<uint8_t> rom(ee::s_ee_strict_rom0_view::k_size, 0u);
    const ee::s_ee_strict_rom0_view view{rom.data(), rom.size()};
    const auto put = [&](uint32_t offset, uint32_t word)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            rom[offset + i] = static_cast<uint8_t>(word >> (8u * i));
        }
    };
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_cop0[ee::COP0_Status] = 0u;
    cpu.m_gpr[4].m_lo = 0xffffffffbfc01004ull;
    cpu.m_gpr[2] = {0u, 0x123456789abcdef0ull};
    put(0x1000u, 0x80ff8180u);
    put(0u, 0x8c82fffcu); // lw r2,-4(r4)
    if (ee::ee_execute_strict_rom_register_step(cpu, view, 1u).m_retired != 1u ||
        cpu.m_gpr[2].m_lo != 0xffffffff80ff8180ull ||
        cpu.m_gpr[2].m_hi != 0x123456789abcdef0ull)
    {
        return false;
    }
    put(4u, 0x8c80fffcu); // lw zero,-4(r4): must check alignment/backing
    cpu.m_gpr[0] = {0xdeadbeefu, 0xcafebabeu};
    for (const uint32_t address : {0xbfc01005u, 0xbfc01006u, 0xbfc01007u,
                                  0x9fc01004u, 0xa0001004u, 0xc0000004u})
    {
        cpu.m_gpr[4].m_lo = address;
        const auto step = ee::ee_execute_strict_rom_register_step(cpu, view, 1u);
        if (step.m_result != result::DATA_UNAVAILABLE ||
            step.m_detail_pc != pc + 4u || step.m_data_address != address - 4u ||
            step.m_retired != 0u || cpu.m_pc != pc + 4u ||
            cpu.m_gpr[0].m_lo != 0xdeadbeefu || cpu.m_gpr[0].m_hi != 0xcafebabeu)
        {
            return false;
        }
    }
    cpu.m_gpr[4].m_lo = 0xbfc01004u;
    if (ee::ee_execute_strict_rom_register_step(cpu, view, 1u).m_retired != 1u ||
        cpu.m_gpr[0].m_lo != 0xdeadbeefu)
    {
        return false;
    }
    // AND/NOR use all 64 low bits and preserve the upper half.
    cpu.m_gpr[3].m_lo = 0xfedcba9876543210ull;
    cpu.m_gpr[5].m_lo = 0x0f0f0f0f0f0f0f0full;
    put(8u, 0x00651024u); // and r2,r3,r5
    put(12u, 0x00651027u); // nor r2,r3,r5
    if (ee::ee_execute_strict_rom_register_step(cpu, view, 1u).m_retired != 1u ||
        cpu.m_gpr[2].m_lo != (0xfedcba9876543210ull & 0x0f0f0f0f0f0f0f0full) ||
        ee::ee_execute_strict_rom_register_step(cpu, view, 1u).m_retired != 1u ||
        cpu.m_gpr[2].m_lo != ~(0xfedcba9876543210ull | 0x0f0f0f0f0f0f0f0full) ||
        cpu.m_gpr[2].m_hi != 0x123456789abcdef0ull)
    {
        return false;
    }
    // Last aligned ROM word succeeds; misaligned end refuses.
    put(0x3ffffcu, 0x7f010203u);
    put(16u, 0x8c820000u);
    cpu.m_gpr[4].m_lo = 0xbffffffcu;
    if (ee::ee_execute_strict_rom_register_step(cpu, view, 1u).m_retired != 1u ||
        cpu.m_gpr[2].m_lo != 0x7f010203u)
    {
        return false;
    }
    // Reserved shamt is not accepted as an ordinary logical opcode.
    put(20u, 0x00651064u);
    if (ee::ee_execute_strict_rom_register_step(cpu, view, 1u).m_result != result::UNSUPPORTED_OPCODE ||
        cpu.m_pc != pc + 20u || cpu.m_gpr[2].m_lo != 0x7f010203u)
    {
        return false;
    }
    // The directory scan's first mismatch/advance sequence, using
    // independent synthetic file sizes including unaligned lengths.
    constexpr uint32_t scan[] =
    {
        0x8d0a000cu, 0u, 0x254a000fu, 0x340b000fu, 0x01605827u,
        0x014b5024u, 0x012a4821u, 0x25080010u, 0x25ceffffu,
        0x15c0ffe7u, 0u
    };
    for (uint32_t i = 0u; i < sizeof(scan) / sizeof(scan[0]); ++i)
    {
        put(0x90u + i * 4u, scan[i]);
    }
    for (const uint32_t size : {0u, 1u, 15u, 16u, 17u, 0x12345u})
    {
        cpu.m_pc = pc + 0x90u;
        cpu.m_gpr[8].m_lo = 0xffffffffbfc01000ull;
        cpu.m_gpr[9].m_lo = 0xffffffffbfc00000ull;
        cpu.m_gpr[14].m_lo = 32u;
        put(0x100cu, size);
        uint32_t retired = 0u;
        while (retired < 11u)
        {
            const auto step = ee::ee_execute_strict_rom_register_step(cpu, view, 11u - retired);
            if (step.m_result != result::COMMITTED || step.m_retired == 0u)
            {
                return false;
            }
            retired += step.m_retired;
        }
        const uint32_t rounded = ((size + 15u) / 16u) * 16u;
        if (retired != 11u || cpu.m_pc != pc + 0x54u ||
            cpu.m_gpr[8].m_lo != 0xffffffffbfc01010ull ||
            cpu.m_gpr[9].m_lo != 0xffffffffbfc00000ull + rounded ||
            cpu.m_gpr[10].m_lo != rounded || cpu.m_gpr[14].m_lo != 31u)
        {
            return false;
        }
    }
    return true;
}

bool test_strict_reset_range_checks_and_byte_store()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xbfc00000u;
    std::vector<uint8_t> rom(ee::s_ee_strict_rom0_view::k_size, 0u);
    std::vector<uint8_t> ram(0x2000u, 0xa5u);
    const ee::s_ee_strict_rom0_view view{rom.data(), rom.size()};
    const ee::s_ee_reset_ram_view backing{ram.data(), ram.size()};
    const auto put = [&](uint32_t offset, uint32_t word)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            rom[offset + i] = static_cast<uint8_t>(word >> (8u * i));
        }
    };
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_cop0[ee::COP0_Status] = 0u;
    put(0u, 0x0065102bu); // sltu r2,r3,r5
    struct s_compare
    {
        uint64_t m_left;
        uint64_t m_right;
        uint64_t m_expected;
    };
    constexpr s_compare comparisons[] =
    {
        {0u, 0u, 0u}, {0u, 1u, 1u},
        {0x100000000ull, 1u, 0u},
        {1u, 0x100000000ull, 1u},
        {0xffffffffffffffffull, 1u, 0u},
        {0x7fffffffffffffffull, 0x8000000000000000ull, 1u},
        {0xffffffffbfc01090ull, 0xffffffffc0000000ull, 1u}
    };
    for (const auto& item : comparisons)
    {
        cpu.m_pc = pc;
        cpu.m_gpr[2] = {0x9999u, 0x1234u};
        cpu.m_gpr[3].m_lo = item.m_left;
        cpu.m_gpr[5].m_lo = item.m_right;
        if (ee::ee_execute_strict_rom_register_step(cpu, view, 1u).m_retired != 1u ||
            cpu.m_gpr[2].m_lo != item.m_expected || cpu.m_gpr[2].m_hi != 0x1234u)
        {
            return false;
        }
    }

    // Real LBU -> SB sequence with a negative store displacement.
    put(4u, 0x90820000u);
    put(8u, 0xa0c2ffffu);
    rom[0x1000u] = 0xe7u;
    cpu.m_pc = pc + 4u;
    cpu.m_gpr[4].m_lo = 0xffffffffbfc01000ull;
    cpu.m_gpr[6].m_lo = 0xffffffffa0001001ull;
    if (ee::ee_execute_strict_rom_register_step(cpu, view, 1u, backing).m_retired != 1u ||
        cpu.m_gpr[2].m_lo != 0xe7u ||
        ee::ee_execute_strict_rom_register_step(cpu, view, 1u).m_result != result::DATA_UNAVAILABLE ||
        cpu.m_pc != pc + 8u || ram[0x1000u] != 0xa5u ||
        ee::ee_execute_strict_rom_register_step(cpu, view, 1u, backing).m_retired != 1u ||
        ram[0x1000u] != 0xe7u || ram[0xfffu] != 0xa5u || ram[0x1001u] != 0xa5u)
    {
        return false;
    }
    // Never write ROM, MMIO, cached/accelerated aliases, unmapped
    // virtual addresses, or bytes outside actual backing.
    put(12u, 0xa0c20000u);
    for (const uint32_t address : {0xbfc01000u, 0xb0000000u, 0x80001000u,
                                  0xa0002000u, 0xa2000000u, 0x00401000u})
    {
        cpu.m_pc = pc + 12u;
        cpu.m_gpr[6].m_lo = address;
        const auto step = ee::ee_execute_strict_rom_register_step(cpu, view, 1u, backing);
        if (step.m_result != result::DATA_UNAVAILABLE || step.m_retired != 0u ||
            step.m_data_address != address || cpu.m_pc != pc + 12u ||
            rom[0x1000u] != 0xe7u || ram[0x1000u] != 0xe7u)
        {
            return false;
        }
    }
    // SB zero ignores the stored contents of the architectural zero
    // register. Execute it in a delay slot at the last backed byte.
    put(16u, 0x10000003u);
    put(20u, 0xa0c00000u);
    cpu.m_pc = pc + 16u;
    cpu.m_gpr[0] = {0xdeadbeefu, 0x1234u};
    cpu.m_gpr[6].m_lo = 0xa0002000u;
    const auto rejected = ee::ee_execute_strict_rom_register_step(cpu, view, 2u, backing);
    if (rejected.m_result != result::DATA_UNAVAILABLE ||
        rejected.m_detail_pc != pc + 20u || cpu.m_pc != pc + 16u ||
        ram.back() != 0xa5u)
    {
        return false;
    }
    cpu.m_gpr[6].m_lo = 0xa0001fffu;
    if (ee::ee_execute_strict_rom_register_step(cpu, view, 1u, backing).m_result != result::BUDGET_EXHAUSTED ||
        ram.back() != 0xa5u || cpu.m_pc != pc + 16u)
    {
        return false;
    }
    return ee::ee_execute_strict_rom_register_step(cpu, view, 2u, backing).m_retired == 2u &&
           cpu.m_pc == pc + 32u && ram.back() == 0u &&
           ram[ram.size() - 2u] == 0xa5u && cpu.m_gpr[0].m_lo == 0xdeadbeefu;
}

bool test_strict_reset_jr_and_ram_fetch()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xbfc00000u;
    constexpr uint32_t target = 0xa0001000u;
    std::vector<uint8_t> rom(ee::s_ee_strict_rom0_view::k_size, 0u);
    std::vector<uint8_t> ram(0x1004u, 0xa5u);
    const ee::s_ee_strict_rom0_view view{rom.data(), rom.size()};
    const ee::s_ee_reset_ram_view backing{ram.data(), ram.size()};
    const auto put = [&](uint32_t offset, uint32_t word)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            rom[offset + i] = static_cast<uint8_t>(word >> (8u * i));
        }
    };
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_cop0[ee::COP0_Status] = 0u;
    // Guest stores generate ADDIU r3,zero,7 in live RAM. A differing
    // ROM word at the same offset must not become the fetched code.
    put(0u, 0xa0820000u); // sb r2,0(r4)
    put(0x1000u, 0x24030009u);
    cpu.m_gpr[4].m_lo = target;
    constexpr uint32_t instruction = 0x24030007u;
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        cpu.m_pc = pc;
        cpu.m_gpr[2].m_lo = instruction >> (i * 8u);
        cpu.m_gpr[4].m_lo = target + i;
        if (ee::ee_execute_strict_rom_register_step(cpu, view, 1u, backing).m_retired != 1u)
        {
            return false;
        }
    }

    constexpr uint32_t branch_pc = pc + 4u;
    put(4u, 0x03000008u); // jr t8
    put(8u, 0x27180004u); // addiu t8,t8,4
    cpu.m_pc = branch_pc;
    cpu.m_gpr[24] = {0x12345678a0001000ull, 0x5678u};
    const auto step = [&](uint32_t budget)
    {
        return ee::ee_execute_strict_rom_register_step(cpu, view, budget, backing);
    };
    if (step(1u).m_result != result::BUDGET_EXHAUSTED ||
        cpu.m_pc != branch_pc || cpu.m_gpr[24].m_lo != 0x12345678a0001000ull)
    {
        return false;
    }
    put(8u, 0x03000008u); // unsupported branch in slot
    const auto refused = step(2u);
    if (refused.m_result != result::UNSUPPORTED_OPCODE || refused.m_retired != 0u ||
        refused.m_detail_pc != branch_pc + 4u || cpu.m_pc != branch_pc ||
        cpu.m_gpr[24].m_lo != 0x12345678a0001000ull)
    {
        return false;
    }
    put(8u, 0xa0800000u); // sb zero,0(r4), outside backing
    cpu.m_gpr[4].m_lo = target + 4u;
    const auto data_refused = step(2u);
    if (data_refused.m_result != result::DATA_UNAVAILABLE ||
        data_refused.m_detail_pc != branch_pc + 4u ||
        cpu.m_pc != branch_pc || ram.back() != 0x24u)
    {
        return false;
    }
    put(8u, 0x27180004u);
    if (step(2u).m_retired != 2u || cpu.m_pc != target ||
        cpu.m_gpr[24].m_lo != 0xffffffffa0001004ull || cpu.m_gpr[24].m_hi != 0x5678u ||
        ee::ee_execute_strict_rom_register_step(cpu, view, 1u).m_result != result::FETCH_UNAVAILABLE ||
        cpu.m_pc != target ||
        step(1u).m_retired != 1u || cpu.m_gpr[3].m_lo != 7u || cpu.m_pc != target + 4u ||
        step(1u).m_result != result::FETCH_UNAVAILABLE)
    {
        return false;
    }

    // JR's reserved fields must not be accepted or execute its slot.
    for (const uint32_t invalid : {0x03010008u, 0x03000808u, 0x03000048u})
    {
        put(4u, invalid);
        cpu.m_pc = branch_pc;
        if (step(2u).m_result != result::UNSUPPORTED_OPCODE || cpu.m_pc != branch_pc ||
            cpu.m_gpr[24].m_lo != 0xffffffffa0001004ull)
        {
            return false;
        }
    }
    put(4u, 0x03000008u);
    put(8u, 0u);
    for (const uint32_t unavailable : {target + 1u, target + 4u, 0x80001000u,
                                      0x00401000u, 0xa2000000u, 0xb0000000u})
    {
        cpu.m_pc = branch_pc;
        cpu.m_gpr[24].m_lo = unavailable;
        // No speculative target fetch, including misaligned targets.
        if (step(2u).m_retired != 2u || cpu.m_pc != unavailable)
        {
            return false;
        }
        const auto fetch = step(1u);
        if (fetch.m_result != result::FETCH_UNAVAILABLE ||
            fetch.m_detail_pc != unavailable || fetch.m_retired != 0u ||
            cpu.m_pc != unavailable)
        {
            return false;
        }
    }
    put(4u, 0x00000008u); // jr zero
    cpu.m_pc = branch_pc;
    cpu.m_gpr[0] = {target, 0x1234u};
    if (step(2u).m_retired != 2u || cpu.m_pc != 0u ||
        cpu.m_gpr[0].m_lo != target || step(1u).m_result != result::FETCH_UNAVAILABLE)
    {
        return false;
    }

    // A JR at the very last backed RAM word cannot fetch its slot.
    cpu.m_pc = target;
    ram[0x1000u] = 8u;
    ram[0x1001u] = 0u;
    ram[0x1002u] = 0u;
    ram[0x1003u] = 3u;
    const auto slot = step(2u);
    return slot.m_result == result::FETCH_UNAVAILABLE && slot.m_retired == 0u &&
        slot.m_detail_pc == target + 4u && cpu.m_pc == target &&
        ram[0xfffu] == 0xa5u;
}

bool test_strict_reset_status()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xa0000000u;
    uint8_t bytes[16u]{};
    const ee::s_ee_reset_ram_view ram{bytes, sizeof(bytes)};
    const auto put = [&](uint32_t offset, uint32_t word)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            bytes[offset + i] = static_cast<uint8_t>(word >> (8u * i));
        }
    };
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_cop0[ee::COP0_Status] = 0u;
    cpu.m_pc = pc;
    cpu.m_gpr[8] = {0x1234567870000000ull, 0xabcdefu};
    put(0u, 0x40886000u); // mtc0 t0,Status
    put(4u, 0x10000001u); // beq zero,zero,+1
    put(8u, 0xa0800000u); // sb zero,0(r4)
    cpu.m_gpr[4].m_lo = pc + 15u;
    bytes[15] = 0xa5u;
    const auto step = [&](uint32_t budget)
    {
        return ee::ee_execute_strict_rom_register_step(cpu, {}, budget, ram);
    };
    if (step(1u).m_retired != 1u || cpu.m_pc != pc + 4u ||
        cpu.m_cop0[ee::COP0_Status] != 0x70000000u ||
        cpu.m_gpr[8].m_lo != 0x1234567870000000ull || cpu.m_gpr[8].m_hi != 0xabcdefu ||
        step(2u).m_retired != 2u || cpu.m_pc != pc + 12u ||
        bytes[15] != 0u)
    {
        return false;
    }
    for (const uint32_t invalid : {1u, ee::STATUS_ERL, ee::STATUS_EXL,
                                   ee::STATUS_BEV, 0x10u, 0x80000000u, 0x70010000u})
    {
        cpu.m_pc = pc;
        cpu.m_gpr[8].m_lo = invalid;
        const auto rejected = step(1u);
        if (rejected.m_result != result::UNSUPPORTED_OPCODE ||
            rejected.m_retired != 0u || cpu.m_pc != pc ||
            cpu.m_cop0[ee::COP0_Status] != 0x70000000u)
        {
            return false;
        }
    }
    for (const uint32_t invalid : {0x40886001u, 0x40886800u})
    {
        put(0u, invalid);
        cpu.m_gpr[8].m_lo = 0u;
        if (step(1u).m_result != result::UNSUPPORTED_OPCODE ||
            cpu.m_pc != pc || cpu.m_cop0[ee::COP0_Status] != 0x70000000u)
        {
            return false;
        }
    }
    put(0u, 0x40806000u); // mtc0 zero,Status
    cpu.m_gpr[0].m_lo = 0xffffffffffffffffull;
    if (step(1u).m_retired != 1u || cpu.m_cop0[ee::COP0_Status] != 0u ||
        cpu.m_gpr[0].m_lo != 0xffffffffffffffffull)
    {
        return false;
    }
    // A disallowed Status value in a delay slot must not change
    // either the Status register or the restart PC.
    put(4u, 0x10000001u);
    put(8u, 0x40886000u);
    cpu.m_gpr[8].m_lo = 1u;
    const auto rejected = step(2u);
    if (rejected.m_result != result::UNSUPPORTED_OPCODE ||
        rejected.m_detail_pc != pc + 8u || cpu.m_pc != pc + 4u ||
        cpu.m_cop0[ee::COP0_Status] != 0u)
    {
        return false;
    }
    cpu.m_gpr[8].m_lo = 0x70000000u;
    if (step(2u).m_retired != 2u || cpu.m_pc != pc + 12u ||
        cpu.m_cop0[ee::COP0_Status] != 0x70000000u)
    {
        return false;
    }
    cpu.m_cop0[ee::COP0_Status] = 0x70000001u;
    return step(1u).m_result == result::UNSAFE_BOUNDARY &&
        cpu.m_pc == pc + 12u;
}

bool test_strict_reset_direct_jumps()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xa0000000u;
    uint8_t bytes[32u]{};
    const ee::s_ee_reset_ram_view ram{bytes, sizeof(bytes)};
    const auto put = [&](uint32_t offset, uint32_t word)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            bytes[offset + i] = static_cast<uint8_t>(word >> (8u * i));
        }
    };
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_pc = pc;
    cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
    cpu.m_gpr[31] = {0xfedcba9876543210ull, 0x1234u};
    constexpr uint64_t old_link = 0xfedcba9876543210ull;
    constexpr uint64_t link = 0x00000000a0000008ull;
    put(0u, 0x0c000004u); // jal a0000010
    put(4u, 0x03e01066u); // XOR r2,ra,zero with reserved shamt=1
    const auto step = [&](uint32_t budget)
    {
        return ee::ee_execute_strict_rom_register_step(cpu, {}, budget, ram);
    };
    if (step(1u).m_result != result::BUDGET_EXHAUSTED ||
        cpu.m_pc != pc || cpu.m_gpr[31].m_lo != old_link)
    {
        return false;
    }
    const auto bad_slot = step(2u);
    if (bad_slot.m_result != result::UNSUPPORTED_OPCODE || bad_slot.m_retired != 0u ||
        bad_slot.m_detail_pc != pc + 4u || cpu.m_pc != pc ||
        cpu.m_gpr[31].m_lo != old_link || cpu.m_gpr[31].m_hi != 0x1234u)
    {
        return false;
    }
    // The canonical XOR must see the new JAL link, unlike a refused
    // reserved encoding. Restore the fixture before the store cases.
    put(4u, 0x03e01026u);
    if (step(2u).m_retired != 2u || cpu.m_pc != pc + 16u ||
        cpu.m_gpr[2].m_lo != link || cpu.m_gpr[31].m_lo != link ||
        cpu.m_gpr[31].m_hi != 0x1234u)
    {
        return false;
    }
    cpu.m_pc = pc;
    cpu.m_gpr[31].m_lo = old_link;
    put(4u, 0xa09f0000u); // sb ra,0(r4)
    cpu.m_gpr[4].m_lo = pc + sizeof(bytes);
    const auto bad_store = step(2u);
    if (bad_store.m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc ||
        cpu.m_gpr[31].m_lo != old_link)
    {
        return false;
    }
    cpu.m_gpr[4].m_lo = pc + 31u;
    if (step(2u).m_retired != 2u || cpu.m_pc != pc + 16u ||
        cpu.m_gpr[31].m_lo != link || cpu.m_gpr[31].m_hi != 0x1234u ||
        bytes[31] != 8u)
    {
        return false;
    }
    // The callee returns through the link, with an actual delay slot.
    put(16u, 0x03e00008u);
    put(20u, 0x24020001u);
    if (step(2u).m_retired != 2u || cpu.m_pc != pc + 8u ||
        cpu.m_gpr[2].m_lo != 1u || cpu.m_gpr[31].m_lo != link)
    {
        return false;
    }
    // A successful delay slot may overwrite the link.
    cpu.m_pc = pc;
    put(4u, 0x241f0007u);
    if (step(2u).m_retired != 2u || cpu.m_pc != pc + 16u ||
        cpu.m_gpr[31].m_lo != 7u || cpu.m_gpr[31].m_hi != 0x1234u)
    {
        return false;
    }
    // J must not write a link; neither direct jump fetches its target
    // until the next architectural step.
    cpu.m_pc = pc;
    put(0u, 0x08000008u); // j a0000020 (unbacked)
    put(4u, 0u);
    if (step(2u).m_retired != 2u || cpu.m_pc != pc + 32u ||
        cpu.m_gpr[31].m_lo != 7u || step(1u).m_result != result::FETCH_UNAVAILABLE)
    {
        return false;
    }
    cpu.m_pc = pc;
    put(0u, 0x0c000008u);
    if (step(2u).m_retired != 2u || cpu.m_pc != pc + 32u ||
        cpu.m_gpr[31].m_lo != link || step(1u).m_result != result::FETCH_UNAVAILABLE)
    {
        return false;
    }
    // Missing slot at end of backing must leave the link unchanged.
    cpu.m_pc = pc + 28u;
    put(28u, 0x0c000004u);
    const auto missing = step(2u);
    return missing.m_result == result::FETCH_UNAVAILABLE && missing.m_retired == 0u &&
        missing.m_detail_pc == pc + 32u && cpu.m_pc == pc + 28u &&
        cpu.m_gpr[31].m_lo == link && cpu.m_gpr[31].m_hi == 0x1234u;
}


bool test_strict_reset_jalr()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xa0000000u;
    uint8_t bytes[32u]{};
    const ee::s_ee_reset_ram_view ram{bytes, sizeof(bytes)};
    const auto put = [&](uint32_t offset, uint32_t word)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            bytes[offset + i] = static_cast<uint8_t>(word >> (8u * i));
        }
    };
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_pc = pc;
    cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
    cpu.m_gpr[25].m_lo = pc + 16u;
    cpu.m_gpr[7] = {0xfedcba9876543210ull, 0x1234u};
    put(0u, 0x03203809u); // JALR a3,t9
    put(4u, 0x00e01066u); // XOR v0,a3,zero with reserved shamt=1
    const auto step = [&](uint32_t budget)
    {
        return ee::ee_execute_strict_rom_register_step(cpu, {}, budget, ram);
    };
    if (step(1u).m_result != result::BUDGET_EXHAUSTED ||
        step(2u).m_result != result::UNSUPPORTED_OPCODE || cpu.m_pc != pc ||
        cpu.m_gpr[7].m_lo != 0xfedcba9876543210ull || cpu.m_gpr[7].m_hi != 0x1234u)
    {
        return false;
    }
    for (const uint32_t word : {0x00e01025u, 0x00e01026u}) // OR and XOR v0,a3,zero
    {
        cpu.m_pc = pc;
        cpu.m_gpr[7].m_lo = 0xfedcba9876543210ull;
        put(4u, word);
        if (step(2u).m_retired != 2u || cpu.m_pc != pc + 16u ||
            cpu.m_gpr[7].m_lo != pc + 8u || cpu.m_gpr[7].m_hi != 0x1234u ||
            cpu.m_gpr[2].m_lo != pc + 8u)
        {
            return false;
        }
    }
    cpu.m_pc = pc;
    put(0u, 0x03200009u); // JALR zero,t9
    put(4u, 0u);
    if (step(2u).m_retired != 2u || cpu.m_pc != pc + 16u || cpu.m_gpr[0].m_lo != 0u)
    {
        return false;
    }
    for (uint32_t invalid : {0x0320c809u, 0x0321f809u, 0x0320f849u})
    {
        cpu.m_pc = pc;
        put(0u, invalid);
        if (step(2u).m_result != result::UNSUPPORTED_OPCODE || cpu.m_pc != pc ||
            cpu.m_gpr[25].m_lo != pc + 16u)
        {
            return false;
        }
    }
    return true;
}



bool test_strict_reset_instruction_cache()
{
    using cache = ee::c_ee_reset_instruction_cache;
    constexpr uint32_t pc = 0x82000u;
    uint8_t bytes[0x4080u]{};
    bytes[0] = 0x11u;
    bytes[64] = 0x22u;
    cache lines;
    {
        cache::c_transaction refused(&lines);
        if (refused.fetch(pc, 0u, bytes, sizeof(bytes)) == nullptr ||
            refused.fetch(pc + 64u, 64u, bytes, sizeof(bytes)) == nullptr)
        {
            return false;
        }
        // Host refusal discards both staged fills.
    }
    if (lines.resident_count() != 0u)
    {
        return false;
    }
    {
        cache::c_transaction accepted(&lines);
        if (*accepted.fetch(pc, 0u, bytes, sizeof(bytes)) != 0x11u ||
            *accepted.fetch(pc + 64u, 64u, bytes, sizeof(bytes)) != 0x22u)
        {
            return false;
        }
        accepted.commit();
    }
    bytes[0] = 0x33u; // uncached backing write must not change I-cache hit
    {
        cache::c_transaction next(&lines);
        const auto* hit = next.fetch(pc, 0u, bytes, sizeof(bytes));
        if (hit == nullptr || *hit != 0x11u ||
            next.fetch(pc + 0x2000u, 0x2000u, bytes, sizeof(bytes)) == nullptr)
        {
            return false;
        }
        next.commit();
    }
    {
        cache::c_transaction conflict(&lines);
        if (conflict.fetch(pc + 0x4000u, 0x4000u, bytes, sizeof(bytes)) != nullptr ||
            conflict.fetch(pc + 64u, 0u, bytes, sizeof(bytes)) != nullptr ||
            conflict.fetch(pc + 0x4040u, 0x4040u, bytes, 0x4060u) != nullptr)
        {
            return false;
        }
    }
    return lines.resident_count() == 3u;
}

bool test_strict_reset_cached_kernel_fetch()
{
    using result = ee::e_ee_reset_prefix_result;
    using cache = ee::c_ee_reset_instruction_cache;
    uint8_t bytes[0x200u]{};
    constexpr uint32_t pc = 0x80000180u;
    bytes[0x180u] = 7u; // ORI v0,zero,7
    bytes[0x182u] = 2u;
    bytes[0x183u] = 0x34u;
    cache lines;
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_pc = pc;
    cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
    cpu.m_cop0[ee::COP0_Config] = 0x20443u;
    {
        const ee::s_ee_reset_ram_view isolated{bytes, sizeof(bytes)};
        if (ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, isolated).m_result !=
            result::FETCH_UNAVAILABLE)
        {
            return false;
        }
    }
    {
        cache::c_transaction transaction(&lines);
        const ee::s_ee_reset_ram_view ram{bytes, sizeof(bytes), true, &transaction};
        if (ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram).m_result != result::COMMITTED ||
            cpu.m_gpr[2].m_lo != 7u || lines.resident_count() != 0u)
        {
            return false;
        }
        transaction.commit();
    }
    bytes[0x180u] = 9u;
    cpu.m_pc = pc;
    cache::c_transaction transaction(&lines);
    const ee::s_ee_reset_ram_view ram{bytes, sizeof(bytes), true, &transaction};
    if (ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram).m_result != result::COMMITTED ||
        cpu.m_gpr[2].m_lo != 7u)
    {
        return false;
    }
    transaction.commit();
    return lines.resident_count() == 1u;
}

bool test_strict_reset_mapped_fetch()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0x00082000u;
    uint8_t bytes[8192u]{};
    bytes[0] = 0x07u; // ORI v0,zero,7
    bytes[2] = 0x02u;
    bytes[3] = 0x34u;
    const ee::s_ee_reset_ram_view mapped{bytes, sizeof(bytes), true};
    const ee::s_ee_reset_ram_view isolated{bytes, sizeof(bytes)};
    for (uint32_t cache : {2u, 3u, 7u})
    {
        for (uint32_t ice : {0u, 1u << 17u})
        {
            ee::c_ee_tlb tlb;
            const uint32_t flags = (cache << 3u) | 3u;
            // Clean, valid, global TLB page, nonidentity RAM mapping.
            if (!tlb.write(0u, 0u, pc, flags, 0x40u | flags))
            {
                return false;
            }
            auto cpu = ee::ee_make_strict_reset_prefix_state();
            cpu.m_pc = pc;
            cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
            cpu.m_cop0[ee::COP0_Config] = 0x440u | ice;
            const auto fetch = ee::ee_plan_instruction_fetch(tlb, pc,
                cpu.m_cop0[ee::COP0_Status], cpu.m_cop0[ee::COP0_Config], 0u, 0u);
            if (isolated.fetch_span(fetch) != nullptr)
            {
                return false;
            }
            const bool allowed = cache == 2u || (cache == 3u && ice == 0u);
            const auto step = ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, mapped,
                nullptr, &tlb);
            if (allowed)
            {
                if (step.m_result != result::COMMITTED || cpu.m_pc != pc + 4u ||
                    cpu.m_gpr[2].m_lo != 7u)
                {
                    return false;
                }
            }
            else if (step.m_result != result::FETCH_UNAVAILABLE ||
                cpu.m_pc != pc || cpu.m_gpr[2].m_lo != 0u)
            {
                return false;
            }
        }
    }
    // No translation, invalid page and out-of-backing mappings must
    // not read the virtual address or fall back to a ROM image.
    ee::c_ee_tlb tlb;
    for (uint32_t variant = 0u; variant < 3u; ++variant)
    {
        if (variant != 0u &&
            !tlb.write(0u, 0u, pc, variant == 1u ? 0x11u : 0x93u, 0x13u))
        {
            return false;
        }
        const auto fetch = ee::ee_plan_instruction_fetch(tlb, pc, 0x70000000u, 0x440u, 0u, 0u);
        if (mapped.fetch_span(fetch) != nullptr)
        {
            return false;
        }
    }
    return true;
}

bool test_strict_reset_scalar_completion()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xa0000000u;
    uint8_t bytes[8u]{};
    const ee::s_ee_reset_ram_view ram{bytes, sizeof(bytes)};
    struct s_case
    {
        uint32_t m_word;
        uint64_t m_rs;
        uint64_t m_rt;
        uint64_t m_expected;
        uint32_t m_reserved;
    };
    constexpr uint32_t rr = (4u << 21u) | (5u << 16u) | (2u << 11u);
    constexpr uint32_t ir = (5u << 16u) | (2u << 11u);
    const s_case cases[] = {
        {rr | 0x26u, 0xfedcba9876543210ull, 0x0123456789abcdefull, ~0ull, 0x40u},
        {rr | 0x2au, 0x8000000000000000ull, 1u, 1u, 0x40u},
        {rr | 0x2au, 1u, 0x8000000000000000ull, 0u, 0x40u},
        {rr | 0x2du, ~0ull, 1u, 0u, 0x40u},
        {rr | 0x2fu, 0u, 1u, ~0ull, 0x40u},
        {rr | 4u, 63u, 1u, 0xffffffff80000000ull, 0x40u},
        {rr | 6u, 32u, 0x1234567880000000ull, 0xffffffff80000000ull, 0x40u},
        {rr | 7u, 33u, 0x80000000u, 0xffffffffc0000000ull, 0x40u},
        {rr | 0x14u, 127u, 1u, 0x8000000000000000ull, 0x40u},
        {rr | 0x16u, 64u, 0x8000000000000000ull, 0x8000000000000000ull, 0x40u},
        {rr | 0x17u, 63u, 0x8000000000000000ull, ~0ull, 0x40u},
        {ir | (31u << 6u) | 0x38u, 0u, 1u, 0x80000000u, 1u << 21u},
        {ir | (31u << 6u) | 0x3au, 0u, 0x8000000000000000ull, 0x100000000ull, 1u << 21u},
        {ir | 0x3bu, 0u, 0x8000000000000000ull, 0x8000000000000000ull, 1u << 21u},
        {ir | (31u << 6u) | 0x3cu, 0u, 1u, 0x8000000000000000ull, 1u << 21u},
        {ir | 0x3eu, 0u, 0x8000000000000000ull, 0x80000000u, 1u << 21u},
        {ir | (31u << 6u) | 0x3fu, 0u, 0x8000000000000000ull, ~0ull, 1u << 21u}
    };
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
    const auto put = [&](uint32_t offset, uint32_t word)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            bytes[offset + i] = static_cast<uint8_t>(word >> (8u * i));
        }
    };
    const auto step = [&](uint32_t word)
    {
        cpu.m_pc = pc;
        put(0u, word);
        return ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram);
    };
    for (const auto& item : cases)
    {
        cpu.m_gpr[4].m_lo = item.m_rs;
        cpu.m_gpr[5].m_lo = item.m_rt;
        cpu.m_gpr[2] = {0x55u, 0x1234u};
        cpu.m_gpr[0] = {0x66u, 0x77u};
        if (step(item.m_word).m_retired != 1u ||
            cpu.m_gpr[2].m_lo != item.m_expected || cpu.m_gpr[2].m_hi != 0x1234u ||
            step(item.m_word & ~0xf800u).m_retired != 1u ||
            cpu.m_gpr[0].m_lo != 0x66u || cpu.m_gpr[0].m_hi != 0x77u ||
            step(item.m_word | item.m_reserved).m_result != result::UNSUPPORTED_OPCODE ||
            cpu.m_pc != pc || cpu.m_gpr[2].m_lo != item.m_expected)
        {
            return false;
        }
        // Execute in a branch slot, with no change on insufficient budget.
        cpu.m_gpr[2].m_lo = 0x55u;
        put(0u, 0x10000001u);
        put(4u, item.m_word);
        if (ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram).m_result != result::BUDGET_EXHAUSTED ||
            cpu.m_pc != pc || cpu.m_gpr[2].m_lo != 0x55u ||
            ee::ee_execute_strict_rom_register_step(cpu, {}, 2u, ram).m_retired != 2u ||
            cpu.m_pc != pc + 8u || cpu.m_gpr[2].m_lo != item.m_expected ||
            cpu.m_gpr[2].m_hi != 0x1234u)
        {
            return false;
        }
    }
    cpu.m_gpr[4] = {0u, 0x1234u};
    return step(0x6484ffffu).m_retired == 1u && cpu.m_gpr[4].m_lo == ~0ull &&
        step(0x64840001u).m_retired == 1u && cpu.m_gpr[4].m_lo == 0u &&
        cpu.m_gpr[4].m_hi == 0x1234u &&
        step(0x6480ffffu).m_retired == 1u && cpu.m_gpr[0].m_lo == 0x66u;
}

bool test_strict_reset_exception_context()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xa0000000u;
    uint8_t bytes[64u]{};
    const ee::s_ee_reset_ram_view ram{bytes, sizeof(bytes)};
    const auto put = [&](uint32_t word)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            bytes[i] = static_cast<uint8_t>(word >> (8u * i));
        }
    };
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
    cpu.m_hi = 0x0123456789abcdefull;
    cpu.m_lo = 0xfedcba9876543210ull;
    cpu.m_hi1 = 0x8000000000000000ull;
    cpu.m_lo1 = 0x7fffffffffffffffull;
    cpu.m_sa = 11u << 3u;
    cpu.m_gpr[27].m_hi = 0x12345678u;
    cpu.m_gpr[26].m_lo = pc + 32u;
    const auto step = [&](uint32_t opcode)
    {
        cpu.m_pc = pc;
        put(opcode);
        return ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram);
    };
    const uint32_t words[] = {0x0000d810u, 0x0000d812u, 0x7000d810u,
        0x7000d812u, 0x0000d828u};
    const uint64_t expected[] = {cpu.m_hi, cpu.m_lo, cpu.m_hi1, cpu.m_lo1, 11u};
    for (uint32_t i = 0u; i < 5u; ++i)
    {
        if (step(words[i]).m_retired != 1u || cpu.m_pc != pc + 4u ||
            cpu.m_gpr[27].m_lo != expected[i] || cpu.m_gpr[27].m_hi != 0x12345678u)
        {
            return false;
        }
    }
    cpu.m_gpr[0] = {0x55u, 0x66u};
    for (const uint32_t opcode : words)
    {
        if (step(opcode & ~0xf800u).m_retired != 1u ||
            cpu.m_gpr[0].m_lo != 0x55u || cpu.m_gpr[0].m_hi != 0x66u ||
            step(opcode | (1u << 21u)).m_result != result::UNSUPPORTED_OPCODE ||
            cpu.m_pc != pc)
        {
            return false;
        }
    }
    cpu.m_gpr[27].m_lo = cpu.m_lo;
    if (step(0xff5b0000u).m_retired != 1u) // SD k1,0(k0)
    {
        return false;
    }
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        if (bytes[32u + i] != static_cast<uint8_t>(cpu.m_lo >> (8u * i)))
        {
            return false;
        }
    }
    cpu.m_gpr[27].m_lo = 0u;
    if (step(0xdf5b0000u).m_retired != 1u || cpu.m_gpr[27].m_lo != cpu.m_lo ||
        cpu.m_gpr[27].m_hi != 0x12345678u ||
        step(0xff5b0001u).m_result != result::DATA_UNAVAILABLE ||
        step(0xdf5b0001u).m_result != result::DATA_UNAVAILABLE ||
        cpu.m_gpr[27].m_lo != cpu.m_lo || cpu.m_pc != pc)
    {
        return false;
    }
    if (step(0xff400008u).m_retired != 1u || step(0xdf400000u).m_retired != 1u ||
        cpu.m_gpr[0].m_lo != 0x55u || cpu.m_gpr[0].m_hi != 0x66u)
    {
        return false;
    }
    for (uint32_t i = 40u; i < 48u; ++i)
    {
        if (bytes[i] != 0u)
        {
            return false;
        }
    }
    cpu.m_gpr[26].m_lo = 0x80000020u; // never bypass cached data
    return step(0xff5b0000u).m_result == result::DATA_UNAVAILABLE &&
        step(0xdf5b0000u).m_result == result::DATA_UNAVAILABLE && cpu.m_pc == pc;
}

bool test_strict_reset_ram_scalar()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xa0000000u;
    uint8_t bytes[40u]{};
    for (auto& byte : bytes)
    {
        byte = 0xa5u;
    }
    const ee::s_ee_reset_ram_view ram{bytes, sizeof(bytes)};
    const auto put = [&](uint32_t offset, uint32_t word)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            bytes[offset + i] = static_cast<uint8_t>(word >> (8u * i));
        }
    };
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_pc = pc;
    cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
    cpu.m_gpr[4].m_lo = 0xffffffffa0000024ull;
    cpu.m_gpr[2] = {0x1122334480abcdefull, 0x4567u};
    cpu.m_gpr[3] = {0x7654u, 0x89abu};
    put(0u, 0xac82fffcu); // sw r2,-4(r4)
    put(4u, 0x8c83fffcu); // lw r3,-4(r4)
    put(8u, 0x9085fffdu); // lbu r5,-3(r4)
    const auto step = [&](uint32_t budget)
    {
        return ee::ee_execute_strict_rom_register_step(cpu, {}, budget, ram);
    };
    if (step(1u).m_retired != 1u || cpu.m_pc != pc + 4u ||
        bytes[32] != 0xefu || bytes[33] != 0xcdu ||
        bytes[34] != 0xabu || bytes[35] != 0x80u ||
        bytes[31] != 0xa5u || bytes[36] != 0xa5u ||
        step(1u).m_retired != 1u || cpu.m_gpr[3].m_lo != 0xffffffff80abcdefull ||
        cpu.m_gpr[3].m_hi != 0x89abu ||
        step(1u).m_retired != 1u || cpu.m_gpr[5].m_lo != 0xcdu ||
        cpu.m_gpr[2].m_lo != 0x1122334480abcdefull || cpu.m_gpr[2].m_hi != 0x4567u)
    {
        return false;
    }
    put(12u, 0xac800000u); // sw zero,0(r4)
    put(16u, 0x8c800000u); // lw zero,0(r4)
    cpu.m_gpr[0] = {0xfeedfaceu, 0x9876u};
    if (step(1u).m_retired != 1u || step(1u).m_retired != 1u ||
        ee::ee_read_instruction_word(bytes + 36u) != 0u ||
        cpu.m_gpr[0].m_lo != 0xfeedfaceu || cpu.m_gpr[0].m_hi != 0x9876u)
    {
        return false;
    }
    // Reject all unsupported data paths even when rt is zero, without
    // changing any byte or architectural state.
    for (const uint32_t address : {pc + 33u, pc + 40u, 0xa2000000u,
                                   0x80000020u, 0x00400020u, 0xb0000000u})
    {
        cpu.m_gpr[4].m_lo = address;
        for (const uint32_t offset : {12u, 16u})
        {
            cpu.m_pc = pc + offset;
            const auto refused = step(1u);
            if (refused.m_result != result::DATA_UNAVAILABLE || refused.m_retired != 0u ||
                refused.m_data_address != address || cpu.m_pc != pc + offset ||
                ee::ee_read_instruction_word(bytes + 32u) != 0x80abcdefu ||
                ee::ee_read_instruction_word(bytes + 36u) != 0u)
            {
                return false;
            }
        }
    }
    // The full word must fit in backing, not just its first byte.
    cpu.m_pc = pc + 12u;
    cpu.m_gpr[4].m_lo = pc + 36u;
    const ee::s_ee_reset_ram_view short_ram{bytes, 39u};
    if (ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, short_ram).m_result != result::DATA_UNAVAILABLE ||
        cpu.m_pc != pc + 12u)
    {
        return false;
    }
    // A JAL delay-slot SW sees the link; refusal rolls it back.
    put(20u, 0x0c000007u); // jal a000001c
    put(24u, 0xac9f0000u); // sw ra,0(r4)
    cpu.m_pc = pc + 20u;
    cpu.m_gpr[31] = {0x12345678u, 0xabcdefu};
    cpu.m_gpr[4].m_lo = pc + 34u;
    const auto rejected = step(2u);
    if (rejected.m_result != result::DATA_UNAVAILABLE ||
        rejected.m_detail_pc != pc + 24u || cpu.m_pc != pc + 20u ||
        cpu.m_gpr[31].m_lo != 0x12345678u ||
        ee::ee_read_instruction_word(bytes + 32u) != 0x80abcdefu)
    {
        return false;
    }
    cpu.m_gpr[4].m_lo = pc + 36u;
    return step(2u).m_retired == 2u && cpu.m_pc == pc + 28u &&
        cpu.m_gpr[31].m_lo == 0x00000000a000001cull &&
        cpu.m_gpr[31].m_hi == 0xabcdefu &&
        ee::ee_read_instruction_word(bytes + 36u) == 0xa000001cu &&
        ee::ee_read_instruction_word(bytes + 32u) == 0x80abcdefu;
}

bool test_strict_reset_sio_tx()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xa0000000u;
    uint8_t code[24u]{};
    uint8_t output[4u]{0xa5u, 0xa5u, 0xa5u, 0xa5u};
    const ee::s_ee_reset_ram_view ram{code, sizeof(code)};
    ee::s_ee_reset_sio_tx sio{output + 1u, 2u, 0u};
    const auto put = [&](uint32_t offset, uint32_t word)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            code[offset + i] = static_cast<uint8_t>(word >> (8u * i));
        }
    };
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_pc = pc;
    cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
    cpu.m_gpr[4].m_lo = 0xffffffffb000f180ull;
    cpu.m_gpr[2].m_lo = 0x123456785bu;
    put(0u, 0xa0820000u); // sb r2,0(r4)
    const auto step = [&](uint32_t budget)
    {
        return ee::ee_execute_strict_rom_register_step(cpu, {}, budget, ram, &sio);
    };
    if (ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram).m_result != result::DATA_UNAVAILABLE ||
        cpu.m_pc != pc || sio.m_count != 0u || step(1u).m_retired != 1u ||
        cpu.m_pc != pc + 4u || sio.m_count != 1u || output[1] != 0x5bu)
    {
        return false;
    }
    for (const uint32_t address : {0xb000f181u, 0xb000f184u, 0x9000f180u, 0x1000f180u})
    {
        cpu.m_pc = pc;
        cpu.m_gpr[4].m_lo = address;
        const auto rejected = step(1u);
        if (rejected.m_result != result::DATA_UNAVAILABLE || rejected.m_retired != 0u ||
            rejected.m_data_address != address || cpu.m_pc != pc ||
            sio.m_count != 1u || output[2] != 0xa5u)
        {
            return false;
        }
    }
    cpu.m_gpr[4].m_lo = 0xb000f180u;
    for (const uint32_t opcode : {0xac820000u, 0x90820000u, 0x8c820000u})
    {
        put(0u, opcode);
        if (step(1u).m_result != result::DATA_UNAVAILABLE ||
            sio.m_count != 1u || cpu.m_pc != pc)
        {
            return false;
        }
    }
    // OR uses both low 64-bit operands, retains the upper half,
    // and reads architectural zero even with poisoned storage.
    cpu.m_gpr[0] = {~uint64_t{0}, 0x42u};
    cpu.m_gpr[3] = {0x8000000000000041ull, 0x99u};
    cpu.m_gpr[2] = {0x1000000000000002ull, 0x1234u};
    put(0u, 0x00621025u); // or r2,r3,r2
    put(4u, 0x00401825u); // or r3,r2,zero
    if (step(1u).m_retired != 1u || cpu.m_gpr[2].m_lo != 0x9000000000000043ull ||
        cpu.m_gpr[2].m_hi != 0x1234u || step(1u).m_retired != 1u ||
        cpu.m_gpr[3].m_lo != 0x9000000000000043ull || cpu.m_gpr[3].m_hi != 0x99u)
    {
        return false;
    }
    put(8u, 0x0c000004u); // jal a0000010
    put(12u, 0xa0820000u); // sb r2,0(r4) in slot
    cpu.m_gpr[31].m_lo = 0x1234u;
    if (step(1u).m_result != result::BUDGET_EXHAUSTED ||
        sio.m_count != 1u || cpu.m_pc != pc + 8u ||
        step(2u).m_retired != 2u || sio.m_count != 2u ||
        cpu.m_pc != pc + 16u || output[2] != 0x43u)
    {
        return false;
    }
    cpu.m_pc = pc + 8u;
    cpu.m_gpr[31].m_lo = 0x4321u;
    const auto full = step(2u);
    return full.m_result == result::DATA_UNAVAILABLE && full.m_retired == 0u &&
        full.m_detail_pc == pc + 12u && cpu.m_pc == pc + 8u &&
        cpu.m_gpr[31].m_lo == 0x4321u && sio.m_count == 2u &&
        output[0] == 0xa5u && output[1] == 0x5bu &&
        output[2] == 0x43u && output[3] == 0xa5u;
}

bool test_strict_reset_config()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xa0000000u;
    uint8_t code[32u]{};
    const ee::s_ee_reset_ram_view ram{code, sizeof(code)};
    const auto put = [&](uint32_t offset, uint32_t word)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            code[offset + i] = static_cast<uint8_t>(word >> (8u * i));
        }
    };
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_pc = pc;
    cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
    cpu.m_gpr[2] = {0x1234567800073003ull, 0x9876u};
    cpu.m_gpr[3].m_hi = 0x1234u;
    put(0u, 0x40038000u); // mfc0 v1,Config
    put(4u, 0x40828000u); // mtc0 v0,Config
    put(8u, 0x0000040fu); // sync.p
    put(12u, 0x40038000u);
    const auto step = [&](uint32_t budget)
    {
        return ee::ee_execute_strict_rom_register_step(cpu, {}, budget, ram);
    };
    if (step(1u).m_retired != 1u || cpu.m_gpr[3].m_lo != 0x440u ||
        cpu.m_gpr[3].m_hi != 0x1234u || step(1u).m_retired != 1u ||
        cpu.m_cop0[ee::COP0_Config] != 0x73443u ||
        cpu.m_gpr[2].m_lo != 0x1234567800073003ull ||
        cpu.m_gpr[2].m_hi != 0x9876u || step(1u).m_retired != 1u ||
        step(1u).m_retired != 1u || cpu.m_gpr[3].m_lo != 0x73443u ||
        cpu.m_gpr[3].m_hi != 0x1234u || cpu.m_pc != pc + 16u)
    {
        return false;
    }
    for (const uint32_t invalid : {1u, 4u, 5u, 6u, 0x80000000u, 0x8u, 0x80000u})
    {
        cpu.m_pc = pc + 4u;
        cpu.m_gpr[2].m_lo = invalid;
        const auto refused = step(1u);
        if (refused.m_result != result::UNSUPPORTED_OPCODE || refused.m_retired != 0u ||
            cpu.m_pc != pc + 4u || cpu.m_cop0[ee::COP0_Config] != 0x73443u)
        {
            return false;
        }
    }
    cpu.m_gpr[2].m_lo = 0x70000fc0u;
    if (step(1u).m_retired != 1u || cpu.m_cop0[ee::COP0_Config] != 0x440u)
    {
        return false;
    }
    cpu.m_pc = pc + 4u;
    cpu.m_gpr[0] = {0xffffffffffffffffull, 0x1234u};
    put(4u, 0x40808000u); // mtc0 zero,Config
    if (step(1u).m_retired != 1u || cpu.m_cop0[ee::COP0_Config] != 0x440u)
    {
        return false;
    }
    put(8u, 0x40008000u); // mfc0 zero,Config
    if (step(1u).m_retired != 1u || cpu.m_gpr[0].m_lo != 0xffffffffffffffffull ||
        cpu.m_gpr[0].m_hi != 0x1234u)
    {
        return false;
    }
    put(12u, 0x40828001u); // reserved low bit
    if (step(1u).m_result != result::UNSUPPORTED_OPCODE || cpu.m_pc != pc + 12u)
    {
        return false;
    }
    put(12u, 0x0000004fu); // unsupported SYNC type
    if (step(1u).m_result != result::UNSUPPORTED_OPCODE || cpu.m_pc != pc + 12u)
    {
        return false;
    }
    put(12u, 0x0000000fu); // sync.l
    if (step(1u).m_retired != 1u)
    {
        return false;
    }
    // Enabling cache controls must not enable a cached access path.
    cpu.m_cop0[ee::COP0_Config] = 0x73443u;
    cpu.m_gpr[4].m_lo = 0x8000001cu;
    put(16u, 0x8c820000u);
    const auto refused = step(1u);
    return refused.m_result == result::DATA_UNAVAILABLE &&
        cpu.m_pc == pc + 16u && cpu.m_cop0[ee::COP0_Config] == 0x73443u;
}

bool test_strict_reset_word_shifts()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xa0000000u;
    uint8_t bytes[8u]{};
    const ee::s_ee_reset_ram_view ram{bytes, sizeof(bytes)};
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
    const auto put = [&](uint32_t word)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            bytes[i] = static_cast<uint8_t>(word >> (8u * i));
        }
    };
    struct s_case
    {
        uint32_t m_function;
        uint32_t m_shift;
        uint64_t m_input;
        uint64_t m_expected;
    };
    constexpr s_case cases[] =
    {
        {2u, 16u, 0xffffffffa0019160ull, 0xa001u},
        {2u, 0u, 0x1234567880000001ull, 0xffffffff80000001ull},
        {2u, 31u, 0x1234567880000001ull, 1u},
        {0u, 31u, 0x1234567800000001ull, 0xffffffff80000000ull},
        {0u, 1u, 0x1234567880000001ull, 2u},
        {3u, 31u, 0x1234567880000000ull, 0xffffffffffffffffull},
        {3u, 16u, 0x1234567880000000ull, 0xffffffffffff8000ull},
        {3u, 16u, 0xffffffff7fff0000ull, 0x7fffu},
        {3u, 0u, 0x1234567880000001ull, 0xffffffff80000001ull}
    };
    for (const auto& item : cases)
    {
        cpu.m_pc = pc;
        cpu.m_gpr[1] = {item.m_input, 0xabcdefu};
        // In-place shift: the original low word must be read first.
        put((1u << 16u) | (1u << 11u) | (item.m_shift << 6u) | item.m_function);
        const auto step = ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram);
        if (step.m_result != result::COMMITTED || step.m_retired != 1u ||
            cpu.m_pc != pc + 4u || cpu.m_gpr[1].m_lo != item.m_expected ||
            cpu.m_gpr[1].m_hi != 0xabcdefu)
        {
            return false;
        }
    }
    cpu.m_gpr[0] = {0xffffffffffffffffull, 0x1234u};
    cpu.m_pc = pc;
    put(0x00000842u); // srl r1,zero,1
    if (ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram).m_retired != 1u ||
        cpu.m_gpr[1].m_lo != 0u)
    {
        return false;
    }
    cpu.m_pc = pc;
    cpu.m_gpr[1].m_lo = 0xffffffff80000000ull;
    put(0x00010043u); // sra zero,r1,1
    if (ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram).m_retired != 1u ||
        cpu.m_gpr[0].m_lo != 0xffffffffffffffffull || cpu.m_gpr[0].m_hi != 0x1234u)
    {
        return false;
    }
    for (const uint32_t function : {0u, 2u, 3u})
    {
        cpu.m_pc = pc;
        put((1u << 21u) | (1u << 16u) | (1u << 11u) | function);
        const auto refused = ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram);
        if (refused.m_result != result::UNSUPPORTED_OPCODE || refused.m_retired != 0u ||
            cpu.m_pc != pc || cpu.m_gpr[1].m_lo != 0xffffffff80000000ull)
        {
            return false;
        }
    }
    return true;
}

bool test_strict_reset_wired()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xa0000000u;
    uint8_t bytes[16u]{};
    const ee::s_ee_reset_ram_view ram{bytes, sizeof(bytes)};
    const auto put = [&](uint32_t offset, uint32_t word)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            bytes[offset + i] = static_cast<uint8_t>(word >> (8u * i));
        }
    };
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
    put(0u, 0x40843000u); // mtc0 a0,Wired
    put(4u, 0x40023000u); // mfc0 v0,Wired
    put(8u, 0x40030800u); // mfc0 v1,Random (unsupported)
    put(12u, 0x42000006u); // tlbwr (unsupported)
    const auto step = [&](uint32_t budget)
    {
        return ee::ee_execute_strict_rom_register_step(cpu, {}, budget, ram);
    };
    for (uint32_t wired = 0u; wired < ee::c_ee_tlb::k_entry_count; ++wired)
    {
        cpu.m_pc = pc;
        cpu.m_cop0[ee::COP0_Random] = 12u;
        cpu.m_gpr[4] = {0x1234567800000000ull | wired, 0x5678u};
        cpu.m_gpr[2].m_hi = 0xabcdefu;
        if (step(1u).m_retired != 1u || cpu.m_cop0[ee::COP0_Wired] != wired ||
            cpu.m_cop0[ee::COP0_Random] != 47u || step(1u).m_retired != 1u ||
            cpu.m_gpr[2].m_lo != wired || cpu.m_gpr[2].m_hi != 0xabcdefu ||
            cpu.m_gpr[4].m_lo != (0x1234567800000000ull | wired) ||
            cpu.m_gpr[4].m_hi != 0x5678u)
        {
            return false;
        }
    }
    for (const uint32_t invalid : {48u, 63u, 0xffffffffu})
    {
        cpu.m_pc = pc;
        cpu.m_gpr[4].m_lo = invalid;
        cpu.m_cop0[ee::COP0_Random] = 17u;
        const auto refused = step(1u);
        if (refused.m_result != result::UNSUPPORTED_OPCODE || refused.m_retired != 0u ||
            cpu.m_pc != pc || cpu.m_cop0[ee::COP0_Wired] != 47u ||
            cpu.m_cop0[ee::COP0_Random] != 17u)
        {
            return false;
        }
    }
    cpu.m_gpr[4].m_lo = 0u;
    put(0u, 0x40843001u); // reserved encoding
    if (step(1u).m_result != result::UNSUPPORTED_OPCODE || cpu.m_pc != pc ||
        cpu.m_cop0[ee::COP0_Wired] != 47u || cpu.m_cop0[ee::COP0_Random] != 17u)
    {
        return false;
    }
    cpu.m_gpr[0] = {0xffffffffffffffffull, 0x9876u};
    put(0u, 0x40803000u); // mtc0 zero,Wired
    if (step(1u).m_retired != 1u || cpu.m_cop0[ee::COP0_Wired] != 0u ||
        cpu.m_cop0[ee::COP0_Random] != 47u)
    {
        return false;
    }
    put(4u, 0x40003000u); // mfc0 zero,Wired
    if (step(1u).m_retired != 1u || cpu.m_gpr[0].m_lo != 0xffffffffffffffffull ||
        cpu.m_gpr[0].m_hi != 0x9876u)
    {
        return false;
    }
    if (step(1u).m_result != result::UNSUPPORTED_OPCODE || cpu.m_pc != pc + 8u)
    {
        return false;
    }
    cpu.m_pc = pc + 12u;
    return step(1u).m_result == result::UNSUPPORTED_OPCODE &&
        cpu.m_pc == pc + 12u && cpu.m_cop0[ee::COP0_Random] == 47u;
}

bool test_strict_reset_indexed_tlb()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xa0000000u;
    uint8_t code[64u]{};
    const ee::s_ee_reset_ram_view ram{code, sizeof(code)};
    ee::c_ee_tlb tlb{};
    const auto put = [&](uint32_t offset, uint32_t word)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            code[offset + i] = static_cast<uint8_t>(word >> (8u * i));
        }
    };
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_pc = pc;
    cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
    cpu.m_gpr[4].m_lo = 0u;
    cpu.m_gpr[5].m_lo = 0u;
    cpu.m_gpr[6].m_lo = 0x70000000u;
    cpu.m_gpr[7].m_lo = 0xffffffff80000007ull;
    cpu.m_gpr[8].m_lo = 7u;
    constexpr uint32_t words[] = {
        0x40840000u, 0x40852800u, 0x40865000u, 0x40871000u, 0x40881800u,
        0x0000040fu, 0x42000002u, 0x0000040fu, 0x42000001u, 0x40021000u
    };
    for (uint32_t i = 0u; i < sizeof(words) / sizeof(words[0]); ++i)
    {
        put(i * 4u, words[i]);
    }
    const auto step = [&](uint32_t budget)
    {
        return ee::ee_execute_strict_rom_register_step(cpu, {}, budget, ram, nullptr, &tlb);
    };
    if (ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram).m_result != result::UNSUPPORTED_OPCODE ||
        cpu.m_pc != pc)
    {
        return false;
    }
    for (uint32_t i = 0u; i < 6u; ++i)
    {
        if (step(1u).m_retired != 1u)
        {
            return false;
        }
    }
    ee::s_ee_tlb_entry entry{};
    if (tlb.read(0u, &entry) || step(0u).m_result != result::BUDGET_EXHAUSTED ||
        tlb.read(0u, &entry) || step(1u).m_retired != 1u || !tlb.read(0u, &entry) ||
        entry.m_page_mask != 0u || entry.m_entry_hi != 0x70000000u ||
        entry.m_entry_lo0 != 0x80000007u || entry.m_entry_lo1 != 7u)
    {
        return false;
    }
    const auto scratch = tlb.lookup(0x70001234u, 0u, true);
    if (scratch.m_status != ee::e_ee_tlb_lookup_status::SCRATCHPAD_MAPPED ||
        scratch.m_scratchpad_offset != 0x1234u || step(1u).m_retired != 1u)
    {
        return false;
    }
    cpu.m_cop0[ee::COP0_EntryLo0] = 0u;
    cpu.m_cop0[ee::COP0_EntryLo1] = 0u;
    cpu.m_cop0[ee::COP0_PageMask] = 0x6000u;
    cpu.m_cop0[ee::COP0_EntryHi] = 0u;
    cpu.m_gpr[2].m_hi = 0xabcdefu;
    if (step(1u).m_retired != 1u || cpu.m_cop0[ee::COP0_EntryHi] != 0x70000000u ||
        cpu.m_cop0[ee::COP0_EntryLo0] != 0x80000007u ||
        cpu.m_cop0[ee::COP0_EntryLo1] != 7u || cpu.m_cop0[ee::COP0_PageMask] != 0u ||
        step(1u).m_retired != 1u || cpu.m_gpr[2].m_lo != 0xffffffff80000007ull ||
        cpu.m_gpr[2].m_hi != 0xabcdefu)
    {
        return false;
    }
    // Invalid mask/index refuse without changing the table or PC.
    cpu.m_pc = pc + 24u;
    cpu.m_cop0[ee::COP0_PageMask] = 0x2000u;
    cpu.m_cop0[ee::COP0_EntryLo0] = 0u;
    if (step(1u).m_result != result::UNSUPPORTED_OPCODE || cpu.m_pc != pc + 24u ||
        !tlb.read(0u, &entry) || entry.m_entry_lo0 != 0x80000007u)
    {
        return false;
    }
    cpu.m_cop0[ee::COP0_PageMask] = 0u;
    cpu.m_cop0[ee::COP0_Index] = 48u;
    if (step(1u).m_result != result::UNSUPPORTED_OPCODE || cpu.m_pc != pc + 24u)
    {
        return false;
    }
    cpu.m_pc = pc + 32u;
    cpu.m_cop0[ee::COP0_Index] = 1u;
    if (step(1u).m_result != result::UNSUPPORTED_OPCODE || cpu.m_pc != pc + 32u ||
        cpu.m_cop0[ee::COP0_EntryLo0] != 0u)
    {
        return false;
    }
    put(40u, 0x42000006u); // TLBWR needs real Random evolution
    cpu.m_pc = pc + 40u;
    if (step(1u).m_result != result::UNSUPPORTED_OPCODE || cpu.m_pc != pc + 40u)
    {
        return false;
    }
    // A programmed scratchpad entry does not authorize a backing
    // access in this reset-only executor.
    put(44u, 0x8cc20000u); // lw v0,0(a2)
    cpu.m_pc = pc + 44u;
    return step(1u).m_result == result::DATA_UNAVAILABLE &&
        cpu.m_pc == pc + 44u && tlb.read(0u, &entry) &&
        entry.m_entry_lo0 == 0x80000007u;
}

bool test_strict_reset_sltiu()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xa0000000u;
    uint8_t code[8u]{};
    const ee::s_ee_reset_ram_view ram{code, sizeof(code)};
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
    const auto put = [&](uint32_t offset, uint32_t word)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            code[offset + i] = static_cast<uint8_t>(word >> (8u * i));
        }
    };
    struct s_case
    {
        uint64_t m_source;
        uint32_t m_immediate;
        uint64_t m_expected;
    };
    constexpr s_case cases[] =
    {
        {0u, 1u, 1u},
        {1u, 1u, 0u},
        {0x100000000ull, 1u, 0u},
        {0x8000000000000000ull, 0x7fffu, 0u},
        {0u, 0xffffu, 1u},
        {0xfffffffffffffffeull, 0xffffu, 1u},
        {0xffffffffffffffffull, 0xffffu, 0u},
        {0x0000000000008000ull, 0x8000u, 1u},
        {0xffffffffffff8000ull, 0x8000u, 0u},
        {0u, 0u, 0u}
    };
    for (const auto& item : cases)
    {
        cpu.m_pc = pc;
        cpu.m_gpr[1] = {item.m_source, 0xabcdefu};
        put(0u, 0x2c210000u | item.m_immediate);
        const auto step = ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram);
        if (step.m_result != result::COMMITTED || step.m_retired != 1u ||
            cpu.m_pc != pc + 4u || cpu.m_gpr[1].m_lo != item.m_expected ||
            cpu.m_gpr[1].m_hi != 0xabcdefu)
        {
            return false;
        }
    }
    cpu.m_gpr[0] = {0xffffffffffffffffull, 0x1234u};
    cpu.m_pc = pc;
    put(0u, 0x2c010001u); // sltiu at,zero,1
    if (ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram).m_retired != 1u ||
        cpu.m_gpr[1].m_lo != 1u)
    {
        return false;
    }
    cpu.m_pc = pc;
    put(0u, 0x2c200002u); // sltiu zero,at,2
    if (ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram).m_retired != 1u ||
        cpu.m_gpr[0].m_lo != 0xffffffffffffffffull || cpu.m_gpr[0].m_hi != 0x1234u)
    {
        return false;
    }
    // Test the instruction in a branch delay slot with budget refusal.
    cpu.m_pc = pc;
    cpu.m_gpr[1].m_lo = 0u;
    put(0u, 0x10000001u);
    put(4u, 0x2c210001u);
    if (ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram).m_result != result::BUDGET_EXHAUSTED ||
        cpu.m_pc != pc || cpu.m_gpr[1].m_lo != 0u)
    {
        return false;
    }
    return ee::ee_execute_strict_rom_register_step(cpu, {}, 2u, ram).m_retired == 2u &&
        cpu.m_pc == pc + 8u && cpu.m_gpr[1].m_lo == 1u &&
        cpu.m_gpr[1].m_hi == 0xabcdefu;
}

bool test_strict_reset_slti()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xa0000000u;
    uint8_t code[8u]{};
    const ee::s_ee_reset_ram_view ram{code, sizeof(code)};
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
    const auto put = [&](uint32_t offset, uint32_t word)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            code[offset + i] = static_cast<uint8_t>(word >> (8u * i));
        }
    };
    struct s_case
    {
        uint64_t m_source;
        uint32_t m_immediate;
        uint64_t m_expected;
    };
    constexpr s_case cases[] =
    {
        {0u, 1u, 1u},
        {1u, 1u, 0u},
        {0x100000000ull, 1u, 0u},
        {0x8000000000000000ull, 0x7fffu, 1u},
        {0x7fffffffffffffffull, 0x7fffu, 0u},
        {0u, 0xffffu, 0u},
        {0xfffffffffffffffeull, 0xffffu, 1u},
        {0xffffffffffffffffull, 0xffffu, 0u},
        {0x0000000000008000ull, 0x8000u, 0u},
        {0xffffffffffff7fffull, 0x8000u, 1u},
        {0xffffffffffff8000ull, 0x8000u, 0u},
        {0xffffffff00000000ull, 0u, 1u},
        {0x00000000ffffffffull, 0u, 0u},
        {0u, 0u, 0u}
    };
    for (const auto& item : cases)
    {
        cpu.m_pc = pc;
        cpu.m_gpr[1] = {item.m_source, 0xabcdefu};
        put(0u, 0x28210000u | item.m_immediate);
        const auto step = ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram);
        if (step.m_result != result::COMMITTED || step.m_retired != 1u ||
            cpu.m_pc != pc + 4u || cpu.m_gpr[1].m_lo != item.m_expected ||
            cpu.m_gpr[1].m_hi != 0xabcdefu)
        {
            return false;
        }
    }
    cpu.m_gpr[0] = {0xffffffffffffffffull, 0x1234u};
    cpu.m_pc = pc;
    put(0u, 0x28010001u); // slti at,zero,1
    if (ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram).m_retired != 1u ||
        cpu.m_gpr[1].m_lo != 1u)
    {
        return false;
    }
    cpu.m_pc = pc;
    put(0u, 0x28200002u); // slti zero,at,2
    if (ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram).m_retired != 1u ||
        cpu.m_gpr[0].m_lo != 0xffffffffffffffffull || cpu.m_gpr[0].m_hi != 0x1234u)
    {
        return false;
    }
    // Test the instruction in a branch delay slot with budget refusal.
    cpu.m_pc = pc;
    cpu.m_gpr[1].m_lo = 0u;
    put(0u, 0x10000001u);
    put(4u, 0x28210001u);
    if (ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram).m_result != result::BUDGET_EXHAUSTED ||
        cpu.m_pc != pc || cpu.m_gpr[1].m_lo != 0u)
    {
        return false;
    }
    return ee::ee_execute_strict_rom_register_step(cpu, {}, 2u, ram).m_retired == 2u &&
        cpu.m_pc == pc + 8u && cpu.m_gpr[1].m_lo == 1u &&
        cpu.m_gpr[1].m_hi == 0xabcdefu;
}

bool test_strict_reset_cached_slti()
{
    using result = ee::e_ee_reset_prefix_result;
    using cache = ee::c_ee_reset_instruction_cache;
    constexpr uint32_t pc = 0x0008203cu;
    uint8_t bytes[128u]{};
    const auto put = [&](uint32_t offset, uint32_t word)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            bytes[offset + i] = static_cast<uint8_t>(word >> (8u * i));
        }
    };
    put(60u, 0x10000001u); // BEQ zero,zero,+1
    put(64u, 0x2888ffffu); // SLTI t0,a0,-1, next I-cache line
    ee::c_ee_tlb tlb;
    if (!tlb.write(0u, 0u, pc & ~0xfffu, 0x1bu, 0x5bu))
    {
        return false;
    }
    cache lines;
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_pc = pc;
    cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
    cpu.m_cop0[ee::COP0_Config] = 0x20440u;
    cpu.m_gpr[4].m_lo = 0xfffffffffffffffeull;
    cpu.m_gpr[8] = {0u, 0xabcdefu};
    {
        cache::c_transaction refused(&lines);
        const ee::s_ee_reset_ram_view ram{bytes, sizeof(bytes), true, &refused};
        const auto step = ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram,
            nullptr, &tlb);
        if (step.m_result != result::BUDGET_EXHAUSTED ||
            cpu.m_pc != pc || cpu.m_gpr[8].m_lo != 0u)
        {
            return false;
        }
    }
    if (lines.resident_count() != 0u)
    {
        return false;
    }
    {
        cache::c_transaction accepted(&lines);
        const ee::s_ee_reset_ram_view ram{bytes, sizeof(bytes), true, &accepted};
        const auto step = ee::ee_execute_strict_rom_register_step(cpu, {}, 2u, ram,
            nullptr, &tlb);
        if (step.m_result != result::COMMITTED || step.m_retired != 2u ||
            cpu.m_pc != pc + 8u || cpu.m_gpr[8].m_lo != 1u ||
            cpu.m_gpr[8].m_hi != 0xabcdefu || lines.resident_count() != 0u)
        {
            return false;
        }
        accepted.commit();
    }
    return lines.resident_count() == 2u;
}

bool test_strict_reset_vif_devices()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xa0000000u;
    uint8_t code[8u]{};
    const ee::s_ee_reset_ram_view ram{code, sizeof(code)};
    ee::c_ee_reset_vif_devices devices;
    const auto vif = devices.transport();
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
    cpu.m_pc = pc;
    cpu.m_gpr[2].m_lo = 1u;
    cpu.m_gpr[4].m_lo = 0xffffffffb0003810ull;
    const auto put = [&](uint32_t offset, uint32_t word)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            code[offset + i] = static_cast<uint8_t>(word >> (8u * i));
        }
    };
    const auto step = [&](uint32_t budget)
    {
        return ee::ee_execute_strict_rom_register_step(cpu, {}, budget, ram,
            nullptr, nullptr, &vif);
    };
    // Seed real VIF command state and ROW; FBRST clears ERR but
    // preserves ROW independently for each device.
    for (uint32_t channel = 0u; channel < 2u; ++channel)
    {
        if (!devices.device(channel).write_register(0x20u, 0x1234u) ||
            !devices.device(channel).write_register(0x100u, 0x5678u))
        {
            return false;
        }
    }
    put(0u, 0xac820000u);
    if (ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram).m_result != result::DATA_UNAVAILABLE ||
        cpu.m_pc != pc || devices.reset_count(0u) != 0u ||
        devices.device(0u).read_register(0x20u) != 0x1234u ||
        step(1u).m_retired != 1u || devices.reset_count(0u) != 1u ||
        devices.device(0u).read_register(0x20u) != 0u ||
        devices.device(0u).read_register(0x100u) != 0x5678u ||
        devices.device(1u).read_register(0x20u) != 0x1234u)
    {
        return false;
    }
    for (const uint32_t address : {0xb0003814u, 0xb0003820u, 0x90003810u, 0xb0003811u})
    {
        cpu.m_pc = pc;
        cpu.m_gpr[4].m_lo = address;
        if (step(1u).m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc ||
            devices.reset_count(0u) != 1u || devices.reset_count(1u) != 0u)
        {
            return false;
        }
    }
    cpu.m_gpr[4].m_lo = 0xb0003c10u;
    cpu.m_gpr[2].m_lo = 2u;
    if (step(1u).m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc)
    {
        return false;
    }
    cpu.m_gpr[2].m_lo = 1u;
    put(0u, 0xa0820000u); // wrong width
    if (step(1u).m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc)
    {
        return false;
    }
    put(0u, 0x8c820000u); // no read stub
    if (step(1u).m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc)
    {
        return false;
    }
    put(0u, 0x10000001u);
    put(4u, 0xac820000u);
    if (step(1u).m_result != result::BUDGET_EXHAUSTED ||
        devices.reset_count(1u) != 0u || cpu.m_pc != pc)
    {
        return false;
    }
    return step(2u).m_retired == 2u && cpu.m_pc == pc + 8u &&
        devices.reset_count(0u) == 1u && devices.reset_count(1u) == 1u &&
        devices.device(1u).read_register(0x20u) == 0u &&
        devices.device(1u).read_register(0x100u) == 0x5678u;
}

bool test_strict_reset_quadword_fifo()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xa0000000u;
    uint8_t code[80u]{};
    const ee::s_ee_reset_ram_view ram{code, sizeof(code)};
    ee::c_ee_reset_vif_devices devices;
    const auto vif = devices.transport();
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
    const auto put = [&](uint32_t offset, uint32_t word)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            code[offset + i] = static_cast<uint8_t>(word >> (i * 8u));
        }
    };
    const auto step = [&](uint32_t budget)
    {
        return ee::ee_execute_strict_rom_register_step(cpu, {}, budget, ram,
            nullptr, nullptr, &vif);
    };
    // Four real commands, two STCYCL separated by NOPs.
    put(32u, 0x01000101u);
    put(36u, 0u);
    put(40u, 0x01000202u);
    put(44u, 0u);
    put(0u, 0x78880000u); // lq t0,0(a0)
    put(4u, 0x7ca80000u); // sq t0,0(a1)
    cpu.m_pc = pc;
    cpu.m_gpr[4].m_lo = pc + 47u; // align down
    cpu.m_gpr[8] = {~uint64_t{0}, ~uint64_t{0}};
    if (step(1u).m_retired != 1u || cpu.m_gpr[8].m_lo != 0x01000101u ||
        cpu.m_gpr[8].m_hi != 0x01000202u)
    {
        return false;
    }
    cpu.m_gpr[5].m_lo = pc + 63u;
    if (step(1u).m_retired != 1u)
    {
        return false;
    }
    for (uint32_t i = 0u; i < 16u; ++i)
    {
        if (code[48u + i] != code[32u + i])
        {
            return false;
        }
    }
    for (uint32_t channel = 0u; channel < 2u; ++channel)
    {
        cpu.m_pc = pc + 4u;
        cpu.m_gpr[5].m_lo = channel == 0u ? 0xb000400fu : 0xb000500fu;
        if (step(1u).m_retired != 1u || devices.fifo_count(channel) != 1u ||
            devices.device(channel).read_register(0x40u) != 0x0202u ||
            devices.last_fifo(channel).m_lo != 0x01000101u ||
            devices.last_fifo(channel).m_hi != 0x01000202u)
        {
            return false;
        }
    }
    cpu.m_pc = pc + 4u;
    cpu.m_gpr[5].m_lo = 0xb0004000u;
    if (ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram).m_result != result::DATA_UNAVAILABLE ||
        cpu.m_pc != pc + 4u || devices.fifo_count(0u) != 1u)
    {
        return false;
    }
    cpu.m_gpr[5].m_lo = 0x90004000u;
    if (step(1u).m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc + 4u)
    {
        return false;
    }
    cpu.m_gpr[5].m_lo = 0xb0004010u;
    if (step(1u).m_result != result::DATA_UNAVAILABLE || devices.fifo_count(0u) != 1u)
    {
        return false;
    }
    // A rejected parser command is not an accepted FIFO transfer.
    cpu.m_gpr[5].m_lo = 0xb0004000u;
    cpu.m_gpr[8] = {0x0f000000u, 0u};
    if (step(1u).m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc + 4u ||
        devices.fifo_count(0u) != 1u || devices.device(0u).read_register(0x40u) != 0x0202u)
    {
        return false;
    }
    cpu.m_pc = pc;
    cpu.m_gpr[4].m_lo = pc + 80u;
    if (step(1u).m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc ||
        cpu.m_gpr[8].m_lo != 0x0f000000u)
    {
        return false;
    }
    cpu.m_gpr[0] = {~uint64_t{0}, ~uint64_t{0}};
    cpu.m_gpr[4].m_lo = pc + 32u;
    put(0u, 0x78800000u); // lq zero,0(a0)
    if (step(1u).m_retired != 1u || cpu.m_gpr[0].m_lo != ~uint64_t{0} ||
        cpu.m_gpr[0].m_hi != ~uint64_t{0})
    {
        return false;
    }
    put(4u, 0x7ca00000u); // sq zero,0(a1)
    cpu.m_gpr[5].m_lo = pc + 48u;
    if (step(1u).m_retired != 1u)
    {
        return false;
    }
    for (uint32_t i = 48u; i < 64u; ++i)
    {
        if (code[i] != 0u)
        {
            return false;
        }
    }
    return true;
}

bool test_strict_reset_intc_mask()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xa0000000u;
    uint8_t code[16u]{};
    const ee::s_ee_reset_ram_view ram{code, sizeof(code)};
    ee::c_ee_reset_vif_devices devices;
    const auto mmio = devices.transport();
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
    cpu.m_gpr[4].m_lo = 0xb000f010u;
    cpu.m_gpr[2] = {2u, 0x1234u};
    cpu.m_gpr[1] = {0xdeadbeefu, 0x5678u};
    const auto put = [&](uint32_t offset, uint32_t word)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            code[offset + i] = static_cast<uint8_t>(word >> (8u * i));
        }
    };
    const auto step = [&](uint32_t budget)
    {
        return ee::ee_execute_strict_rom_register_step(cpu, {}, budget, ram,
            nullptr, nullptr, &mmio);
    };
    put(0u, 0x8c810000u); // lw at,0(a0)
    put(4u, 0xac820000u); // sw v0,0(a0)
    cpu.m_pc = pc;
    if (ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram).m_result != result::DATA_UNAVAILABLE ||
        cpu.m_pc != pc || cpu.m_gpr[1].m_lo != 0xdeadbeefu ||
        step(1u).m_retired != 1u || cpu.m_gpr[1].m_lo != 0u ||
        cpu.m_gpr[1].m_hi != 0x5678u || devices.intc_read_count() != 1u ||
        step(1u).m_retired != 1u || devices.intc_mask() != 2u ||
        devices.first_intc_write() != 2u || devices.first_intc_mask() != 2u)
    {
        return false;
    }
    cpu.m_pc = pc;
    if (step(1u).m_retired != 1u || cpu.m_gpr[1].m_lo != 2u ||
        step(1u).m_retired != 1u || devices.intc_mask() != 0u ||
        devices.intc_write_count() != 2u)
    {
        return false;
    }
    for (const uint32_t address : {0xb000f000u, 0xb000f014u, 0x9000f010u, 0xb000f011u})
    {
        cpu.m_pc = pc;
        cpu.m_gpr[4].m_lo = address;
        if (step(1u).m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc ||
            devices.intc_read_count() != 2u)
        {
            return false;
        }
        cpu.m_pc = pc + 4u;
        if (step(1u).m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc + 4u ||
            devices.intc_write_count() != 2u || devices.intc_mask() != 0u)
        {
            return false;
        }
    }
    cpu.m_gpr[4].m_lo = 0xb000f010u;
    cpu.m_pc = pc;
    put(0u, 0x90810000u); // lbu: incorrect width
    if (step(1u).m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc)
    {
        return false;
    }
    put(0u, 0xa0820000u); // sb: incorrect width
    if (step(1u).m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc)
    {
        return false;
    }
    cpu.m_gpr[0] = {~uint64_t{0}, ~uint64_t{0}};
    put(0u, 0x8c800000u);
    if (step(1u).m_retired != 1u || devices.intc_read_count() != 3u ||
        cpu.m_gpr[0].m_lo != ~uint64_t{0} || cpu.m_gpr[0].m_hi != ~uint64_t{0})
    {
        return false;
    }
    cpu.m_pc = pc;
    put(0u, 0x10000001u);
    if (step(1u).m_result != result::BUDGET_EXHAUSTED ||
        devices.intc_write_count() != 2u || cpu.m_pc != pc)
    {
        return false;
    }
    return step(2u).m_retired == 2u && cpu.m_pc == pc + 8u &&
        devices.intc_write_count() == 3u && devices.intc_mask() == 2u &&
        devices.intc_status() == 0u && devices.first_intc_read() == 0u;
}

bool test_strict_reset_timer3()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xa0000000u;
    uint8_t code[32u]{};
    const ee::s_ee_reset_ram_view ram{code, sizeof(code)};
    ee::c_ee_reset_vif_devices devices;
    const auto mmio = devices.transport();
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
    const auto put = [&](uint32_t offset, uint32_t word)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            code[offset + i] = static_cast<uint8_t>(word >> (i * 8u));
        }
    };
    const auto step = [&](uint32_t budget)
    {
        return ee::ee_execute_strict_rom_register_step(cpu, {}, budget, ram,
            nullptr, nullptr, &mmio);
    };
    put(0u, 0xac820000u);
    constexpr uint32_t addresses[] = {0xb0001810u, 0xb0001800u, 0xb0001820u, 0xb0001810u};
    constexpr uint32_t values[] = {0xc00u, 0u, 0xffffu, 0x83u};
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        cpu.m_pc = pc;
        cpu.m_gpr[4].m_lo = addresses[i];
        cpu.m_gpr[2] = {values[i], 0x1234u};
        if (step(1u).m_retired != 1u || devices.timer_write_count() != i + 1u)
        {
            return false;
        }
        const auto actual = devices.timer_write(i);
        if (actual.m_address != addresses[i] - 0xa0000000u ||
            actual.m_value != values[i] || actual.m_mode != (i == 3u ? 0x83u : 0u) ||
            actual.m_count != 0u)
        {
            return false;
        }
    }
    cpu.m_pc = pc;
    put(0u, 0x8c820000u); // no clock, no timer read
    if (step(1u).m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc)
    {
        return false;
    }
    put(0u, 0xac820000u);
    for (const uint32_t address : {0xb0001811u, 0xb0001830u, 0xb0001010u, 0x90001810u})
    {
        cpu.m_gpr[4].m_lo = address;
        if (step(1u).m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc ||
            devices.timer_write_count() != 4u)
        {
            return false;
        }
    }
    // SH is a two-byte RAM store, not a device word or aligned-down SQ.
    put(0u, 0xa4820000u);
    cpu.m_gpr[4].m_lo = pc + 18u;
    cpu.m_gpr[2] = {0x123456789abcdef0ull, 0x5678u};
    code[17] = code[18] = code[19] = code[20] = 0xa5u;
    if (step(1u).m_retired != 1u || code[17] != 0xa5u || code[18] != 0xf0u ||
        code[19] != 0xdeu || code[20] != 0xa5u)
    {
        return false;
    }
    cpu.m_pc = pc;
    cpu.m_gpr[4].m_lo = pc + 19u;
    if (step(1u).m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc ||
        code[19] != 0xdeu || code[20] != 0xa5u)
    {
        return false;
    }
    cpu.m_gpr[4].m_lo = 0xb0001810u;
    return step(1u).m_result == result::DATA_UNAVAILABLE &&
        cpu.m_pc == pc && devices.timer_write_count() == 4u && devices.intc_status() == 0u;
}

bool test_strict_reset_dmac_sif()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xa0000000u;
    uint8_t code[8u]{};
    const ee::s_ee_reset_ram_view ram{code, sizeof(code)};
    ee::c_ee_reset_vif_devices devices;
    const auto mmio = devices.transport();
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
    const auto put = [&](uint32_t opcode)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            code[i] = static_cast<uint8_t>(opcode >> (i * 8u));
        }
    };
    const auto step = [&]()
    {
        cpu.m_pc = pc;
        return ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram,
            nullptr, nullptr, &mmio);
    };
    cpu.m_gpr[4].m_lo = 0xb000e000u;
    cpu.m_gpr[2] = {0xdeadbeefu, 0x1234u};
    put(0x8c820000u);
    if (step().m_retired != 1u || cpu.m_gpr[2].m_lo != 0u ||
        cpu.m_gpr[2].m_hi != 0x1234u || devices.dmac_read_count() != 1u)
    {
        return false;
    }
    put(0xac820000u);
    cpu.m_gpr[2].m_lo = 1u;
    if (step().m_retired != 1u || devices.dmac_control() != 1u ||
        devices.dmac_write_count() != 1u || devices.first_dmac_write() != 1u)
    {
        return false;
    }
    cpu.m_gpr[2].m_lo = 5u;
    if (step().m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc ||
        devices.dmac_control() != 1u || devices.dmac_write_count() != 1u)
    {
        return false;
    }
    put(0x8c820000u);
    if (step().m_retired != 1u || cpu.m_gpr[2].m_lo != 1u)
    {
        return false;
    }
    put(0xac820000u);
    cpu.m_gpr[2].m_lo = 0u;
    if (step().m_retired != 1u || devices.dmac_control() != 0u)
    {
        return false;
    }
    cpu.m_gpr[4].m_lo = 0xb000f220u;
    cpu.m_gpr[2].m_lo = 0x10000u;
    if (step().m_retired != 1u || devices.sif_msflag() != 0x10000u ||
        devices.first_sif_write() != 0x10000u || devices.sif_write_count() != 1u)
    {
        return false;
    }
    cpu.m_gpr[2].m_lo = 0x20000u;
    if (step().m_retired != 1u || devices.sif_msflag() != 0x30000u)
    {
        return false;
    }
    put(0x8c820000u);
    if (step().m_retired != 1u || cpu.m_gpr[2].m_lo != 0x30000u)
    {
        return false;
    }
    for (const uint32_t address : {0xb000e010u, 0xb000c000u, 0xb000f230u, 0x9000e000u, 0xb000e001u})
    {
        cpu.m_gpr[4].m_lo = address;
        put(0x8c820000u);
        if (step().m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc)
        {
            return false;
        }
        put(0xac820000u);
        if (step().m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc)
        {
            return false;
        }
    }
    cpu.m_gpr[4].m_lo = 0xb000e000u;
    put(0xa0820000u);
    if (step().m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc)
    {
        return false;
    }
    return devices.dmac_control() == 0u && devices.dmac_write_count() == 2u &&
        devices.sif_msflag() == 0x30000u && devices.sif_write_count() == 2u &&
        devices.intc_status() == 0u;
}

bool test_strict_reset_gs_csr()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xa0000000u;
    uint8_t code[8u]{};
    const ee::s_ee_reset_ram_view ram{code, sizeof(code)};
    ee::c_ee_reset_vif_devices devices;
    const auto mmio = devices.transport();
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
    const auto put = [&](uint32_t opcode)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            code[i] = static_cast<uint8_t>(opcode >> (i * 8u));
        }
    };
    const auto step = [&]()
    {
        cpu.m_pc = pc;
        return ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram,
            nullptr, nullptr, &mmio);
    };
    put(0xac820000u);
    cpu.m_gpr[4].m_lo = 0xb2001004u;
    cpu.m_gpr[2].m_lo = 0u;
    if (step().m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc ||
        devices.gs_upper_count() != 0u)
    {
        return false;
    }
    cpu.m_gpr[4].m_lo = 0xb2001000u;
    for (const uint32_t value : {0u, 0x100u, 0x201u, 0x400u})
    {
        cpu.m_gpr[2].m_lo = value;
        if (step().m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc ||
            devices.gs_reset_count() != 0u)
        {
            return false;
        }
    }
    cpu.m_gpr[2] = {0x200u, 0x1234u};
    if (step().m_retired != 1u || devices.gs_reset_count() != 1u ||
        devices.gs_siglblid() != 0u || devices.intc_status() != 0u)
    {
        return false;
    }
    // Another reset is outside the cold initialization subset.
    if (step().m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc)
    {
        return false;
    }
    cpu.m_gpr[4].m_lo = 0xb2001004u;
    if (step().m_result != result::DATA_UNAVAILABLE || devices.gs_upper_count() != 0u)
    {
        return false;
    }
    cpu.m_gpr[2].m_lo = 0u;
    if (step().m_retired != 1u || devices.gs_upper_count() != 1u)
    {
        return false;
    }
    for (const uint32_t address : {0xb2001001u, 0xb2001010u, 0x92001000u})
    {
        cpu.m_gpr[4].m_lo = address;
        cpu.m_gpr[2].m_lo = 0x200u;
        if (step().m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc)
        {
            return false;
        }
    }
    cpu.m_gpr[4].m_lo = 0xb2001000u;
    put(0x8c820000u); // no CSR status/timing read
    return step().m_result == result::DATA_UNAVAILABLE &&
        cpu.m_pc == pc && devices.gs_reset_count() == 1u &&
        devices.gs_upper_count() == 1u && devices.intc_status() == 0u;
}

bool test_strict_reset_rom_bounds()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xa0000000u;
    uint8_t code[32u]{};
    const ee::s_ee_reset_ram_view ram{code, sizeof(code)};
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
    const auto put = [&](uint32_t offset, uint32_t opcode)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            code[offset + i] = static_cast<uint8_t>(opcode >> (i * 8u));
        }
    };
    const auto step = [&](uint32_t budget)
    {
        return ee::ee_execute_strict_rom_register_step(cpu, {}, budget, ram);
    };
    struct s_arithmetic_case
    {
        uint32_t m_opcode;
        uint64_t m_left;
        uint64_t m_right;
        uint64_t m_expected;
    };
    constexpr s_arithmetic_case arithmetic[] =
    {
        {0x3882ffffu, 0x123456789abcdef0ull, 0u, 0x123456789abc210full},
        {0x38828000u, 0xffffffffffffffffull, 0u, 0xffffffffffff7fffull},
        {0x00851023u, 0x1234567800000000ull, 1u, 0xffffffffffffffffull},
        {0x00851023u, 0x80000000u, 1u, 0x7fffffffu},
        {0x00851023u, 0x7fffffffu, 0xffffffffu, 0xffffffff80000000ull},
        {0x00851023u, 0x100000000ull, 0x200000000ull, 0u}
    };
    for (const auto& item : arithmetic)
    {
        cpu.m_pc = pc;
        cpu.m_gpr[4].m_lo = item.m_left;
        cpu.m_gpr[5].m_lo = item.m_right;
        cpu.m_gpr[2] = {0xdeadbeefu, 0x1234u};
        put(0u, item.m_opcode);
        const auto action = step(1u);
        if (action.m_result != result::COMMITTED || action.m_retired != 1u ||
            cpu.m_pc != pc + 4u || cpu.m_gpr[2].m_lo != item.m_expected ||
            cpu.m_gpr[2].m_hi != 0x1234u)
        {
            return false;
        }
    }
    cpu.m_pc = pc;
    put(0u, 0x00851063u); // SUBU with reserved shamt
    if (step(1u).m_result != result::UNSUPPORTED_OPCODE || cpu.m_pc != pc)
    {
        return false;
    }
    cpu.m_gpr[0] = {~uint64_t{0}, ~uint64_t{0}};
    put(0u, 0x38028000u); // XORI v0,zero,8000
    if (step(1u).m_retired != 1u || cpu.m_gpr[2].m_lo != 0x8000u)
    {
        return false;
    }
    cpu.m_pc = pc;
    put(0u, 0x00850023u); // SUBU zero,a0,a1
    if (step(1u).m_retired != 1u || cpu.m_gpr[0].m_lo != ~uint64_t{0} ||
        cpu.m_gpr[0].m_hi != ~uint64_t{0})
    {
        return false;
    }
    // The actual seven-word ROM bounds helper. The unsigned
    // comparison before subtraction must reject out-of-range offsets.
    constexpr uint32_t words[] =
    {
        0x00c4082bu, 0x38210001u, 0x00c41023u, 0x0045102bu,
        0x38420001u, 0x03e00008u, 0x00221024u
    };
    for (uint32_t i = 0u; i < sizeof(words) / sizeof(words[0]); ++i)
    {
        put(i * 4u, words[i]);
    }
    struct s_bounds_case
    {
        uint32_t m_offset;
        uint32_t m_size;
        uint64_t m_expected;
    };
    constexpr s_bounds_case bounds[] =
    {
        {0x1000u, 0x20u, 1u},
        {0x3fffffu, 1u, 1u},
        {0x3fffffu, 2u, 0u},
        {0x400000u, 0u, 1u},
        {0x400000u, 1u, 0u},
        {0x400001u, 0u, 0u}
    };
    for (const auto& item : bounds)
    {
        cpu.m_pc = pc;
        cpu.m_gpr[4].m_lo = item.m_offset;
        cpu.m_gpr[5].m_lo = item.m_size;
        cpu.m_gpr[6].m_lo = 0x400000u;
        cpu.m_gpr[31].m_lo = pc + 28u;
        cpu.m_gpr[2].m_hi = 0x5678u;
        for (uint32_t i = 0u; i < 5u; ++i)
        {
            if (step(1u).m_retired != 1u)
            {
                return false;
            }
        }
        const uint64_t before = cpu.m_gpr[2].m_lo;
        if (step(1u).m_result != result::BUDGET_EXHAUSTED ||
            cpu.m_pc != pc + 20u || cpu.m_gpr[2].m_lo != before ||
            step(2u).m_retired != 2u || cpu.m_pc != pc + 28u ||
            cpu.m_gpr[2].m_lo != item.m_expected || cpu.m_gpr[2].m_hi != 0x5678u)
        {
            return false;
        }
    }
    return true;
}

bool test_strict_reset_partial_word()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xa0000000u;
    uint8_t code[64u]{};
    const ee::s_ee_reset_ram_view ram{code, sizeof(code)};
    std::vector<uint8_t> image(ee::s_ee_strict_rom0_view::k_size, 0u);
    const ee::s_ee_strict_rom0_view rom{image.data(), image.size()};
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
    const auto put = [&](uint32_t offset, uint32_t opcode)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            code[offset + i] = static_cast<uint8_t>(opcode >> (i * 8u));
        }
    };
    const auto step = [&](uint32_t budget)
    {
        return ee::ee_execute_strict_rom_register_step(cpu, rom, budget, ram);
    };
    constexpr uint64_t left[] = {0x0000000011667788ull, 0x0000000022117788ull,
        0x0000000033221188ull, 0xffffffff84332211ull};
    constexpr uint64_t right[] = {0xffffffff84332211ull, 0x1122334455843322ull,
        0x1122334455668433ull, 0x1122334455667784ull};
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        code[32u + i] = image[32u + i] = i == 3u ? 0x84u : static_cast<uint8_t>((i + 1u) * 0x11u);
    }
    for (const uint32_t base : {pc, 0xbfc00000u})
    {
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            cpu.m_gpr[4].m_lo = base + 32u + lane;
            cpu.m_pc = pc;
            cpu.m_gpr[2] = {0x1122334455667788ull, 0xabcdefu};
            put(0u, 0x88820000u);
            if (step(1u).m_retired != 1u || cpu.m_gpr[2].m_lo != left[lane] ||
                cpu.m_gpr[2].m_hi != 0xabcdefu)
            {
                return false;
            }
            cpu.m_pc = pc;
            cpu.m_gpr[2].m_lo = 0x1122334455667788ull;
            put(0u, 0x98820000u);
            if (step(1u).m_retired != 1u || cpu.m_gpr[2].m_lo != right[lane] ||
                cpu.m_gpr[2].m_hi != 0xabcdefu)
            {
                return false;
            }
        }
    }
    // Independently check each selected store lane and untouched bytes.
    for (const bool left_store : {false, true})
    {
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            for (uint32_t i = 32u; i < 36u; ++i)
            {
                code[i] = 0xa5u;
            }
            cpu.m_pc = pc;
            cpu.m_gpr[4].m_lo = pc + 32u + lane;
            cpu.m_gpr[2].m_lo = 0x44332211u;
            put(0u, left_store ? 0xa8820000u : 0xb8820000u);
            if (step(1u).m_retired != 1u)
            {
                return false;
            }
            for (uint32_t i = 0u; i < 4u; ++i)
            {
                const bool enabled = left_store ? i <= lane : i >= lane;
                const uint32_t register_lane = left_store ? 3u - lane + i : i - lane;
                const uint8_t expected = enabled ?
                    static_cast<uint8_t>(0x11u * (register_lane + 1u)) : 0xa5u;
                if (code[32u + i] != expected)
                {
                    return false;
                }
            }
        }
    }
    // Actual cross-word pair; preserve the upper 64 bits.
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        code[32u + i] = static_cast<uint8_t>(0x80u + i);
    }
    cpu.m_pc = pc;
    cpu.m_gpr[4].m_lo = pc + 33u;
    put(0u, 0x88820003u);
    put(4u, 0x98820000u);
    if (step(1u).m_retired != 1u || step(1u).m_retired != 1u ||
        cpu.m_gpr[2].m_lo != 0xffffffff84838281ull || cpu.m_gpr[2].m_hi != 0xabcdefu)
    {
        return false;
    }
    // Missing backing, cached alias, device and ROM stores refuse.
    for (const uint32_t address : {pc + 64u, 0x80000020u, 0xb000f010u})
    {
        cpu.m_pc = pc;
        cpu.m_gpr[4].m_lo = address;
        const uint64_t old = cpu.m_gpr[2].m_lo;
        if (step(1u).m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc ||
            cpu.m_gpr[2].m_lo != old)
        {
            return false;
        }
    }
    cpu.m_pc = pc;
    cpu.m_gpr[4].m_lo = 0xbfc00020u;
    put(0u, 0xa8820000u);
    if (step(1u).m_result != result::DATA_UNAVAILABLE || cpu.m_pc != pc)
    {
        return false;
    }
    // A delay slot with insufficient budget must not merge any lanes.
    cpu.m_gpr[4].m_lo = 0xbfc00023u;
    put(0u, 0x10000001u);
    put(4u, 0x88820000u);
    if (step(1u).m_result != result::BUDGET_EXHAUSTED || cpu.m_pc != pc ||
        cpu.m_gpr[2].m_lo != 0xffffffff84838281ull)
    {
        return false;
    }
    return step(2u).m_retired == 2u && cpu.m_pc == pc + 8u &&
        cpu.m_gpr[2].m_lo == 0xffffffff84332211ull;
}

bool test_strict_reset_cold_cache()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xa0000000u;
    uint8_t code[16u]{};
    const ee::s_ee_reset_ram_view ram{code, sizeof(code)};
    ee::c_ee_reset_cold_cache cache;
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
    const auto put = [&](uint32_t offset, uint32_t opcode)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            code[offset + i] = static_cast<uint8_t>(opcode >> (i * 8u));
        }
    };
    const auto step = [&](uint32_t budget)
    {
        return ee::ee_execute_strict_rom_register_step(cpu, {}, budget, ram,
            nullptr, nullptr, nullptr, &cache);
    };
    cpu.m_pc = pc;
    put(0u, 0xbd140000u);
    cpu.m_gpr[8] = {0u, 0x1234u};
    if (ee::ee_execute_strict_rom_register_step(cpu, {}, 1u, ram).m_result != result::UNSUPPORTED_OPCODE ||
        cpu.m_pc != pc || cache.data_count() != 0u)
    {
        return false;
    }
    for (uint32_t index = 0u; index < 128u; ++index)
    {
        for (uint32_t way = 0u; way < 2u; ++way)
        {
            cpu.m_pc = pc;
            cpu.m_gpr[8].m_lo = index * 64u;
            put(0u, 0xbd070000u | way);
            if (step(1u).m_retired != 1u || cache.instruction_count() != index * 2u + way + 1u)
            {
                return false;
            }
            if (index < 64u)
            {
                cpu.m_pc = pc;
                put(0u, 0xbd140000u | way);
                if (step(1u).m_retired != 1u || cache.data_count() != index * 2u + way + 1u)
                {
                    return false;
                }
                cpu.m_pc = pc;
                if (step(1u).m_retired != 1u || cache.data_count() != index * 2u + way + 1u)
                {
                    return false;
                }
            }
        }
    }
    cpu.m_pc = pc;
    cpu.m_gpr[8].m_lo = 0x1000u;
    put(0u, 0xbd140000u);
    if (step(1u).m_result != result::UNSUPPORTED_OPCODE || cpu.m_pc != pc)
    {
        return false;
    }
    cpu.m_gpr[8].m_lo = 0u;
    put(0u, 0xbd0e0000u); // IFL: cannot fill the cold cache
    if (step(1u).m_result != result::UNSUPPORTED_OPCODE || cpu.m_pc != pc)
    {
        return false;
    }
    put(0u, 0xbd070002u);
    if (step(1u).m_result != result::UNSUPPORTED_OPCODE || cpu.m_pc != pc)
    {
        return false;
    }
    ee::c_ee_reset_cold_cache slot_cache;
    put(0u, 0x10000001u);
    put(4u, 0xbd140001u);
    const auto slot_step = [&](uint32_t budget)
    {
        return ee::ee_execute_strict_rom_register_step(cpu, {}, budget, ram,
            nullptr, nullptr, nullptr, &slot_cache);
    };
    if (slot_step(1u).m_result != result::BUDGET_EXHAUSTED ||
        slot_cache.data_count() != 0u || cpu.m_pc != pc)
    {
        return false;
    }
    return slot_step(2u).m_retired == 2u && cpu.m_pc == pc + 8u &&
        slot_cache.data_count() == 1u && cache.data_count() == 128u &&
        cache.instruction_count() == 256u && cpu.m_gpr[8].m_hi == 0x1234u;
}

bool test_strict_reset_interrupt_boundary()
{
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t pc = 0xa0000000u;
    uint8_t code[32u]{};
    const ee::s_ee_reset_ram_view ram{code, sizeof(code)};
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    cpu.m_cop0[ee::COP0_Status] = 0x70000000u;
    cpu.m_cop0[ee::COP0_Cause] = 0x400u;
    cpu.m_gpr[2] = {0x30c01u, 0x1234u};
    cpu.m_gpr[3] = {~uint64_t{0}, 0x5678u};
    cpu.m_pc = pc;
    const auto put = [&](uint32_t offset, uint32_t opcode)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            code[offset + i] = static_cast<uint8_t>(opcode >> (i * 8u));
        }
    };
    const auto step = [&](uint32_t budget)
    {
        return ee::ee_execute_strict_rom_register_step(cpu, {}, budget, ram);
    };
    put(0u, 0x40036000u);
    put(4u, 0x00621025u);
    put(8u, 0x0000000fu);
    put(12u, 0x40826000u);
    for (uint32_t i = 0u; i < 3u; ++i)
    {
        if (step(1u).m_result != result::COMMITTED)
        {
            return false;
        }
    }
    const auto refused = step(1u);
    if (refused.m_result != result::INTERRUPT_SCHEDULER_REQUIRED ||
        refused.m_retired != 0u || refused.m_detail_pc != pc + 12u ||
        cpu.m_pc != pc + 12u || cpu.m_cop0[ee::COP0_Status] != 0x70000000u ||
        cpu.m_cop0[ee::COP0_Cause] != 0x400u ||
        cpu.m_gpr[2].m_lo != 0x70030c01u || cpu.m_gpr[2].m_hi != 0x1234u ||
        cpu.m_gpr[3].m_lo != 0x70000000u || cpu.m_gpr[3].m_hi != 0x5678u)
    {
        return false;
    }
    // A reserved MFC0 encoding is not silently accepted.
    cpu.m_pc = pc;
    put(0u, 0x40036001u);
    if (step(1u).m_result != result::UNSUPPORTED_OPCODE || cpu.m_pc != pc)
    {
        return false;
    }
    // Architectural zero suppresses the register write.
    put(0u, 0x40006000u);
    cpu.m_gpr[0] = {~uint64_t{0}, ~uint64_t{0}};
    if (step(1u).m_retired != 1u || cpu.m_gpr[0].m_lo != ~uint64_t{0} ||
        cpu.m_gpr[0].m_hi != ~uint64_t{0})
    {
        return false;
    }
    // Refuse an enabling write in a JAL delay slot without leaking
    // either the link update or a partial status transition.
    cpu.m_pc = pc;
    cpu.m_gpr[31] = {0x12345678u, 0xabcd};
    put(0u, 0x0c000004u);
    put(4u, 0x40826000u);
    if (step(1u).m_result != result::BUDGET_EXHAUSTED || cpu.m_pc != pc)
    {
        return false;
    }
    const auto slot = step(2u);
    if (slot.m_result != result::INTERRUPT_SCHEDULER_REQUIRED ||
        slot.m_detail_pc != pc + 4u || slot.m_retired != 0u || cpu.m_pc != pc ||
        cpu.m_gpr[31].m_lo != 0x12345678u || cpu.m_gpr[31].m_hi != 0xabcdu ||
        cpu.m_cop0[ee::COP0_Status] != 0x70000000u ||
        cpu.m_cop0[ee::COP0_Cause] != 0x400u)
    {
        return false;
    }
    // Existing interrupt-disabled Status writes remain supported.
    put(0u, 0x40826000u);
    cpu.m_gpr[2].m_lo = 0x60000000u;
    return step(1u).m_retired == 1u && cpu.m_pc == pc + 4u &&
        cpu.m_cop0[ee::COP0_Status] == 0x60000000u;
}

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
    if (!check(test_strict_firmware_exception_registers(), "scheduler-owned exception COP0 reads") ||
        !check(test_strict_firmware_syscall(), "scheduler-owned architectural SYSCALL") ||
        !check(test_strict_firmware_jalr(), "scheduler-owned JALR") ||
        !check(test_strict_firmware_gs_reset(), "scheduler-owned cold GS reset") ||
        !check(test_strict_firmware_hblank_timer(), "scheduler-owned HBlank reference timer") ||
        !check(test_strict_firmware_serial(), "scheduler-owned serial byte transport") ||
        !check(test_strict_firmware_split_slots(), "scheduler-owned resumable branch slots") ||
        !check(test_strict_firmware_iop_clock(), "scheduler-owned synchronized EE/IOP clock") ||
        !check(test_strict_firmware_scanout(), "scheduler-owned GS scanout and VBlank edges") ||
        !check(test_strict_firmware_sif_transport(), "scheduler-owned bidirectional SIF transport") ||
        !check(test_strict_firmware_sif_vif(), "scheduler-owned SIF mailbox and VIF DMA") ||
        !check(test_strict_firmware_spr_dma(), "scheduler-owned synchronous SPR DMA") ||
        !check(test_strict_firmware_cop0_clock(), "scheduler-owned COP0 Count/Compare deadlines") ||
        !check(test_strict_firmware_interrupts(), "scheduler-owned firmware interrupt entry and ERET") ||
        !check(test_strict_firmware_pilot(), "scheduler-owned strict firmware pilot"))
    {
        return 1;
    }

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
    if (!check(test_strict_ram_execution(), "isolated strict RAM execution regression"))
    {
        return 1;
    }
    if (!check(test_strict_ram_wide_execution(), "isolated strict RAM wide execution regression"))
    {
        return 1;
    }
    if (!check((test_strict_ram_handler_execution(false) && test_strict_ram_handler_execution(true)), "strict guest TLB handler and ERET regression"))
    {
        return 1;
    }
    if (!check(test_strict_rom0_native_fetch(), "strict ROM0 native fetch regression"))
    {
        return 1;
    }
    if (!check(test_strict_reset_prefix(), "strict reset prefix regression"))
    {
        return 1;
    }
    if (!check(test_strict_reset_register_execution(), "strict reset processor selection regression"))
    {
        return 1;
    }
    if (!check(test_strict_reset_rom_data(), "strict reset ROM data and ADDU regression"))
    {
        return 1;
    }
    if (!check(test_strict_reset_rom_word_and_logic(), "strict reset ROM word/directory scan regression"))
    {
        return 1;
    }
    if (!check(test_strict_reset_interrupt_boundary(), "strict reset Status read and interrupt scheduler boundary"))
    {
        return 1;
    }
    if (!check(test_strict_reset_cold_cache(), "strict reset cold cache index/way regression"))
    {
        return 1;
    }
    if (!check(test_strict_reset_partial_word(), "strict reset partial word ROM and RAM regression"))
    {
        return 1;
    }
    if (!check(test_strict_reset_rom_bounds(), "strict reset XORI SUBU and ROM bounds regression"))
    {
        return 1;
    }
    if (!check(test_strict_reset_gs_csr(), "strict reset cold GS CSR regression"))
    {
        return 1;
    }
    if (!check(test_strict_reset_dmac_sif(), "strict reset DMAE and SIF MSFLAG regression"))
    {
        return 1;
    }
    if (!check(test_strict_reset_timer3(), "strict reset Timer3 programming and SH regression"))
    {
        return 1;
    }
    if (!check(test_strict_reset_intc_mask(), "strict reset INTC mask toggle regression"))
    {
        return 1;
    }
    if (!check(test_strict_reset_quadword_fifo(), "strict reset quadword RAM and VIF FIFO regression"))
    {
        return 1;
    }
    if (!check(test_strict_reset_vif_devices(), "strict reset real VIF FBRST regression"))
    {
        return 1;
    }
    if (!check(test_strict_reset_slti(), "strict reset SLTI regression") ||
        !check(test_strict_reset_cached_slti(), "strict reset cached SLTI transaction") ||
        !check(test_strict_reset_sltiu(), "strict reset SLTIU regression"))
    {
        return 1;
    }
    if (!check(test_strict_reset_indexed_tlb(), "strict reset persistent indexed TLB regression"))
    {
        return 1;
    }
    if (!check(test_strict_reset_wired(), "strict reset Wired and Random boundary regression"))
    {
        return 1;
    }
    if (!check(test_strict_reset_word_shifts(), "strict reset immediate word shifts regression"))
    {
        return 1;
    }
    if (!check(test_strict_reset_config(), "strict reset Config and serialization regression"))
    {
        return 1;
    }
    if (!check(test_strict_reset_sio_tx(), "strict reset SIO TX and OR regression"))
    {
        return 1;
    }
    if (!check(test_strict_reset_scalar_completion(), "strict reset remaining nontrapping scalar operations") ||
        !check(test_strict_reset_exception_context(), "strict reset exception context transfers") ||
        !check(test_strict_reset_ram_scalar(), "strict reset RAM scalar transfer regression"))
    {
        return 1;
    }
    if (!check(test_strict_reset_cached_kernel_fetch(), "strict reset cached KSEG0 fetch") ||
        !check(test_strict_reset_instruction_cache(), "strict reset transactional I-cache fills") ||
        !check(test_strict_reset_mapped_fetch(), "strict reset mapped cache-bypass fetch") ||
        !check(test_strict_reset_jalr(), "strict reset JALR link and rollback") ||
        !check(test_strict_reset_direct_jumps(), "strict reset J/JAL link and slot regression"))
    {
        return 1;
    }
    if (!check(test_strict_reset_status(), "strict reset restricted COP0 Status regression"))
    {
        return 1;
    }
    if (!check(test_strict_reset_jr_and_ram_fetch(), "strict reset JR and live RAM fetch regression"))
    {
        return 1;
    }
    if (!check(test_strict_reset_range_checks_and_byte_store(), "strict reset SLTU and RAM byte store regression"))
    {
        return 1;
    }
    std::puts("ee_ram_tlb_test: PASS");
    return 0;
}
