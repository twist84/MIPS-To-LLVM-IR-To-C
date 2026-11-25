#include "ee_ram_interpreter.h"
#include "ee_memory.h"
#include "ee_address_route.h"
#include "ee_memory_fault.h"
#include "ee_exceptions.h"
#include "ee_runtime.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace
{
constexpr uint32_t k_entry = 0x800750c8u;
constexpr uint32_t k_return = 0xfffffff0u;

bool check(bool passed, const char* message)
{
    if (!passed)
    {
        std::fprintf(stderr, "ee_ram_tlb_test: %s\n", message);
    }
    return passed;
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

    // Detailed lookup is independent of the legacy direct-address path.
    // Exercise every supported page size and both ends of both pages.
    using status = ee::e_ee_tlb_lookup_status;
    constexpr uint32_t masks[] = {0u, 0x6000u, 0x1e000u, 0x7e000u,
                                  0x1fe000u, 0x7fe000u, 0x1ffe000u};
    for (const uint32_t mask : masks)
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
            return 1;
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
                return 1;
            }
        }
        if (!check(tlb.lookup(virt + size, 0xabu, true).m_status ==
                       status::PAGE_NOT_DIRTY &&
                   tlb.lookup(virt, 0xacu, false).m_status == status::NO_MATCH &&
                   tlb.lookup(virt + 2u * size, 0xabu, false).m_status == status::NO_MATCH,
                   "modified, ASID and pair boundary are distinct"))
        {
            return 1;
        }
        if (!check(tlb.write(47u, mask, virt | 0xabu, lo0 | 1u, lo1) &&
                   tlb.lookup(virt, 0xacu, false).m_status == status::NO_MATCH &&
                   tlb.write(47u, mask, virt | 0xabu, lo0 | 1u, lo1 | 1u) &&
                   tlb.lookup(virt, 0xacu, false).m_status == status::TRANSLATED,
                   "both G bits required for global ASID bypass"))
        {
            return 1;
        }
    }

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
            return 1;
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
            return 1;
        }
        if (!check(tlb.write(0u, 0u, virt, lo, lo) &&
                   tlb.write(1u, 0u, virt, lo, lo) &&
                   tlb.lookup(virt, 0u, false).m_status == status::MULTIPLE_MATCHES &&
                   tlb.lookup(virt, 0u, false).m_physical_address == 0u,
                   "multiple matches do not pick an arbitrary PFN"))
        {
            return 1;
        }
    }

    // Every segment boundary, in every operating mode and exception level.
    // Expected routes follow Figure 5-1, independently of the TLB contents.
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
                            return 1;
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
                return 1;
            }
        }
        if (!check(ee::ee_classify_address(0x20000000u, 0u, 3u).m_route ==
                       route::TLB_MAPPED &&
                   ee::ee_classify_address(0x70000000u, 0u, 3u).m_route ==
                       route::TLB_MAPPED,
                   "HLE aliases and scratchpad are not unconditional architectural mappings"))
        {
            return 1;
        }
    }

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
            return 1;
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
            return 1;
        }
        if (!check(tlb.write(1u, 0u, virt | asid, lo | 6u, lo | 6u) &&
                   ee::ee_resolve_access(tlb, virt, 0u, 3u, asid, false).m_status ==
                       access::UNMODELED,
                   "duplicate mapping cannot become a fabricated TLB fault"))
        {
            return 1;
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
                            return 1;
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
                        return 1;
                    }
                }
            }
        }
        if (!check(!ee::ee_plan_memory_fault(access::UNMODELED, false, bad, 0u, 0u, 0u).m_valid &&
                   !ee::ee_plan_memory_fault(access::PHYSICAL, false, bad, 0u, 0u, 0u).m_valid,
                   "nonfault and unmodeled outcomes cannot enter exception handler"))
        {
            return 1;
        }
    }

    // Scratchpad mapping has special 16 KiB matching and permissions.
    {
        ee::c_ee_tlb tlb{};
        constexpr uint32_t virt = 0x70000000u;
        constexpr uint32_t asid = 7u;
        constexpr uint32_t lo0 = 0x80004006u; // S, ignored PFN/C, V/D
        constexpr uint32_t lo1 = 0x0000803eu; // ignored PFN/C, V/D
        if (!check(tlb.write(0u, 0u, virt | asid, lo0, lo1),
                   "valid scratchpad entry"))
        {
            return 1;
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
                return 1;
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
            return 1;
        }
        if (!check(tlb.write(0u, 0u, virt | asid, lo0, lo1) &&
                   tlb.write(1u, 0u, (virt + 0x2000u) | asid, 0x1eu, 0x1eu) &&
                   tlb.lookup(virt + 0x2000u, asid, false).m_status == status::MULTIPLE_MATCHES &&
                   !ee::ee_plan_memory_fault(ee::e_ee_access_status::SCRATCHPAD, false,
                                             virt, 0u, 0u, asid).m_valid,
                   "overlapping RAM mapping ambiguous; scratchpad is not a fault"))
        {
            return 1;
        }
    }

    std::puts("ee_ram_tlb_test: PASS");
    return 0;
}
