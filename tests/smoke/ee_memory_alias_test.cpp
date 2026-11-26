#include "ee_memory.h"
#include "ee_runtime.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>

int main()
{
    ee::ee_memory memory(4096);
    memory.write32(0x20u, 0x12345678u);
    if (memory.contains(0x80000020u, 4u) ||
        memory.read32(0xa0000020u) != 0u)
    {
        std::fprintf(stderr, "KSEG RAM aliases must be opt-in\n");
        return 1;
    }

    memory.set_kseg_ram_aliases(true);
    if (!memory.contains(0x80000020u, 4u) ||
        !memory.contains(0xa0000020u, 4u) ||
        memory.read32(0x80000020u) != 0x12345678u ||
        memory.read32(0xa0000020u) != 0x12345678u)
    {
        std::fprintf(stderr, "KSEG RAM mirrors do not map to EE physical RAM\n");
        return 1;
    }

    memory.write64(0xa0000030u, 0x1122334455667788ull);
    if (memory.read64(0x80000030u) != 0x1122334455667788ull ||
        memory.read64(0x30u) != 0x1122334455667788ull)
    {
        std::fprintf(stderr, "KSEG writes do not alias normal EE RAM\n");
        return 1;
    }

    ee::s_ee_u128 quad{};
    quad.m_lo = 0xabcdef0123456789ull;
    quad.m_hi = 0xfedcba9876543210ull;
    memory.write128(0x80000040u, &quad);
    ee::s_ee_u128 result{};
    memory.read128(0xa0000040u, &result);
    if (result.m_lo != quad.m_lo || result.m_hi != quad.m_hi)
    {
        std::fprintf(stderr, "KSEG 128-bit memory aliases failed\n");
        return 1;
    }

    if (memory.contains(0x80000ffcu, 8u) ||
        memory.contains(0x80100000u, 4u) ||
        memory.contains(0xb0000000u, 4u) ||
        memory.contains(0xffffffffu, 4u))
    {
        std::fprintf(stderr, "invalid KSEG RAM address accepted\n");
        return 1;
    }

    memory.set_kseg_ram_aliases(false);
    if (memory.contains(0x80000020u, 4u))
    {
        std::fprintf(stderr, "KSEG aliases cannot be disabled\n");
        return 1;
    }
    // Exercise the exported scalar ABI on both sides of the RAM fast
    // path, including address translation and exact bus callback widths.
    struct s_bus_probe
    {
        uint32_t m_reads = 0u;
        uint32_t m_writes = 0u;
        uint32_t m_address = 0u;
        uint32_t m_width = 0u;
        uint64_t m_value = 0u;
        bool m_handled = true;
    } bus;
    ee::s_ee_runtime scalar_runtime{};
    scalar_runtime.m_user = &memory;
    scalar_runtime.m_mmio_user = &bus;
    scalar_runtime.m_mmio_read = [](void* user, uint32_t address,
        uint32_t width, ee::s_ee_u128* value)
    {
        auto& probe = *static_cast<s_bus_probe*>(user);
        ++probe.m_reads;
        probe.m_address = address;
        probe.m_width = width;
        value->m_lo = 0xfedcba9876543210ull;
        return probe.m_handled;
    };
    scalar_runtime.m_mmio_write = [](void* user, uint32_t address,
        uint32_t width, const ee::s_ee_u128* value)
    {
        auto& probe = *static_cast<s_bus_probe*>(user);
        ++probe.m_writes;
        probe.m_address = address;
        probe.m_width = width;
        probe.m_value = value->m_lo;
        return probe.m_handled;
    };
    memory.set_kseg_ram_aliases(true);
    for (uint32_t base : {0u, 0x20000000u, 0x30000000u,
                          0x80000000u, 0xa0000000u, 0x70000000u})
    {
        ee::ee_mem_write8(&scalar_runtime, base + 0x100u, 0x12u);
        ee::ee_mem_write16(&scalar_runtime, base + 0x102u, 0x3456u);
        ee::ee_mem_write32(&scalar_runtime, base + 0x104u, 0x789abcdeu);
        ee::ee_mem_write64(&scalar_runtime, base + 0x108u, 0x1122334455667788ull);
        if (ee::ee_mem_read8(&scalar_runtime, base + 0x100u) != 0x12u ||
            ee::ee_mem_read16(&scalar_runtime, base + 0x102u) != 0x3456u ||
            ee::ee_mem_read32(&scalar_runtime, base + 0x104u) != 0x789abcdeu ||
            ee::ee_mem_read64(&scalar_runtime, base + 0x108u) != 0x1122334455667788ull)
        {
            std::fprintf(stderr, "scalar ABI RAM/alias/scratchpad mismatch\n");
            return 1;
        }
    }
    memory.write32(4092u, 0xabcdef12u);
    ee::ee_mem_write64(&scalar_runtime, 0xa0000ffcu, ~uint64_t{0});
    if (memory.read32(4092u) != 0xabcdef12u ||
        ee::ee_mem_read64(&scalar_runtime, 0xa0000ffcu) != 0u ||
        ee::ee_mem_read32(nullptr, 0u) != 0u ||
        bus.m_reads != 0u || bus.m_writes != 0u)
    {
        std::fprintf(stderr, "scalar ABI bounds or unintended bus access\n");
        return 1;
    }

    constexpr uint32_t mmio = 0xb000e000u;
    if (ee::ee_mem_read8(&scalar_runtime, mmio) != 0x10u ||
        bus.m_width != 1u ||
        ee::ee_mem_read16(&scalar_runtime, mmio) != 0x3210u ||
        bus.m_width != 2u ||
        ee::ee_mem_read32(&scalar_runtime, mmio) != 0x76543210u ||
        bus.m_width != 4u ||
        ee::ee_mem_read64(&scalar_runtime, mmio) != 0xfedcba9876543210ull ||
        bus.m_width != 8u || bus.m_reads != 4u ||
        bus.m_address != 0x1000e000u)
    {
        std::fprintf(stderr, "scalar ABI bus read width/translation mismatch\n");
        return 1;
    }
    ee::ee_mem_write8(&scalar_runtime, mmio, 0x12u);
    if (bus.m_width != 1u || bus.m_value != 0x12u) { return 1; }
    ee::ee_mem_write16(&scalar_runtime, mmio, 0x3456u);
    if (bus.m_width != 2u || bus.m_value != 0x3456u) { return 1; }
    ee::ee_mem_write32(&scalar_runtime, mmio, 0x789abcdeu);
    if (bus.m_width != 4u || bus.m_value != 0x789abcdeu) { return 1; }
    ee::ee_mem_write64(&scalar_runtime, mmio, 0x1122334455667788ull);
    if (bus.m_width != 8u || bus.m_value != 0x1122334455667788ull ||
        bus.m_address != 0x1000e000u || bus.m_writes != 4u)
    {
        std::fprintf(stderr, "scalar ABI bus write width/translation mismatch\n");
        return 1;
    }
    bus.m_handled = false;
    if (ee::ee_mem_read32(&scalar_runtime, mmio) != 0u ||
        bus.m_reads != 5u)
    {
        std::fprintf(stderr, "unhandled MMIO read must remain zero\n");
        return 1;
    }

    // Reaching the initial console-print limit must not stop the recent
    // host-side guest STORE ring. Its entries are pre-store SSA operands.
    ee::s_ee_guest_store_trace_entry recent[3]{};
    ee::s_ee_runtime runtime{};
    runtime.m_guest_store_trace_enabled = true;
    runtime.m_guest_store_trace_word = 0x80u;
    runtime.m_guest_store_trace_remaining = 0u;
    runtime.m_guest_store_trace_recent = recent;
    runtime.m_guest_store_trace_capacity = 3u;
    for (uint32_t index = 0u; index < 5u; ++index)
    {
        ee::ee_trace_guest_store(
            &runtime, 0x1000u + index * 4u, 0xac88000cu,
            0x80u, 0x80u, 4u, index + 1u, 0u,
            0x80u, 0x2000u, 0x3000u, 0x4000u
        );
    }
    ee::ee_trace_guest_store(
        &runtime, 0x2000u, 0xac88000cu,
        0x100u, 0x100u, 4u, 99u, 0u,
        0x100u, 0x2000u, 0x3000u, 0x4000u
    );
    if (runtime.m_guest_store_trace_count != 3u ||
        runtime.m_guest_store_trace_next != 2u ||
        runtime.m_guest_store_trace_remaining != 0u ||
        recent[2].m_pc != 0x1008u ||
        recent[0].m_pc != 0x100cu ||
        recent[1].m_pc != 0x1010u ||
        recent[1].m_source_value != 5u ||
        recent[1].m_ra != 0x4000u)
    {
        std::fprintf(stderr, "guest STORE recent ring did not wrap correctly\n");
        return 1;
    }
    std::printf("EE KSEG0/KSEG1 RAM alias and guest-store ring regression: PASS\n");
    return 0;
}
