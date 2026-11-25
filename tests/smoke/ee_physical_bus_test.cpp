#include "ee_memory.h"
#include "ee_runtime.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace
{
constexpr uint32_t k_intc_mask = 0x1000f010u;
constexpr uint32_t k_gs_csr = 0x12001000u;
constexpr uint32_t k_vif_fifo = 0x10004000u;

struct s_mock_io
{
    uint32_t m_reads = 0;
    uint32_t m_writes = 0;
    uint32_t m_last_address = 0;
    uint32_t m_last_width = 0;
    ee::s_ee_u128 m_last_value{};
    uint32_t m_previous_address = 0;
    uint32_t m_previous_width = 0;
    ee::s_ee_u128 m_previous_attempt{};
    ee::s_ee_u128 m_last_attempt{};
};

bool is_known_register(uint32_t address)
{
    return address == k_intc_mask || address == k_gs_csr || address == k_vif_fifo;
}

bool read_mmio(void* user, uint32_t address, uint32_t width, ee::s_ee_u128* value)
{
    s_mock_io* io = static_cast<s_mock_io*>(user);
    ++io->m_reads;
    io->m_previous_address = io->m_last_address;
    io->m_previous_width = io->m_last_width;
    io->m_last_address = address;
    io->m_last_width = width;
    if (!is_known_register(address))
    {
        return false;
    }

    value->m_lo = 0x8877665544332211ull;
    value->m_hi = 0xfedcba9876543210ull;
    return true;
}

bool write_mmio(void* user, uint32_t address, uint32_t width,
                const ee::s_ee_u128* value)
{
    s_mock_io* io = static_cast<s_mock_io*>(user);
    ++io->m_writes;
    io->m_previous_address = io->m_last_address;
    io->m_previous_width = io->m_last_width;
    io->m_previous_attempt = io->m_last_attempt;
    io->m_last_attempt = *value;
    io->m_last_address = address;
    io->m_last_width = width;
    if (!is_known_register(address))
    {
        return false;
    }

    io->m_last_value = *value;
    return true;
}

int fail(const char* text)
{
    std::fprintf(stderr, "EE direct physical/MMIO bus: %s\n", text);
    return 1;
}
} // namespace

int main()
{
    ee::ee_memory memory(4096);
    ee::s_ee_runtime runtime{};
    runtime.m_user = &memory;
    ee::s_ee_vu0_state vu0{};
    runtime.m_vu0_state = &vu0;

    s_mock_io io{};
    runtime.m_mmio_user = &io;
    runtime.m_mmio_read = read_mmio;
    runtime.m_mmio_write = write_mmio;

    memory.write32(0x20u, 0x12345678u);
    if (memory.read32(0x20000020u) != 0 ||
        memory.read32(0x30000020u) != 0 ||
        ee::ee_mem_read32(&runtime, 0x9000f010u) != 0 ||
        io.m_reads != 0)
    {
        return fail("direct aliases must remain opt-in");
    }

    memory.set_kseg_ram_aliases(true);
    if (memory.read32(0x80000020u) != 0x12345678u ||
        memory.read32(0xa0000020u) != 0x12345678u ||
        memory.read32(0x20000020u) != 0x12345678u ||
        memory.read32(0x30000020u) != 0x12345678u)
    {
        return fail("KSEG and 0x2/0x3 physical RAM aliases");
    }

    ee::ee_mem_write32(&runtime, 0x30000024u, 0xdeadbeefu);
    if (memory.read32(0x24u) != 0xdeadbeefu ||
        memory.contains(0x20001000u, 4) ||
        memory.contains(0x32000000u, 4))
    {
        return fail("direct alias write or out-of-range RAM validation");
    }


    // All widths take the direct physical RAM path without invoking the
    // installed device callbacks, including unaligned host-safe transfers.
    // Exercise both enabled aliases and ordinary physical addresses.
    const uint32_t fast_reads = io.m_reads;
    const uint32_t fast_writes = io.m_writes;
    const ee::s_ee_u128 fast_quad{0x8123456789abcdefull, 0xfedcba9876543210ull};
    for (uint32_t alias : {0u, 0x20000000u, 0x30000000u, 0x80000000u, 0xa0000000u})
    {
        const uint32_t base = alias + 0x101u;
        ee::ee_mem_write8(&runtime, base, 0xabu);
        ee::ee_mem_write16(&runtime, base + 1u, 0x89efu);
        ee::ee_mem_write32(&runtime, base + 3u, 0xfedcba98u);
        ee::ee_mem_write64(&runtime, base + 7u, fast_quad.m_lo);
        ee::ee_mem_write128(&runtime, base + 15u, &fast_quad);
        ee::s_ee_u128 loaded{};
        ee::ee_mem_read128(&runtime, base + 15u, &loaded);
        if (ee::ee_mem_read8(&runtime, base) != 0xabu ||
            ee::ee_mem_read16(&runtime, base + 1u) != 0x89efu ||
            ee::ee_mem_read32(&runtime, base + 3u) != 0xfedcba98u ||
            ee::ee_mem_read64(&runtime, base + 7u) != fast_quad.m_lo ||
            loaded.m_lo != fast_quad.m_lo || loaded.m_hi != fast_quad.m_hi ||
            memory.read64(0x110u) != fast_quad.m_lo ||
            memory.read64(0x118u) != fast_quad.m_hi)
        {
            return fail("direct RAM fast path width, alias or byte order");
        }
    }

    // Paired instruction transfers may fuse RAM but not the device bus.
    for (uint32_t alias : {0u, 0x20000000u, 0x30000000u, 0x80000000u, 0xa0000000u})
    {
        ee::s_ee_u128 loaded{};
        ee::ee_mem_write128_pair(&runtime, alias + 0x180u, fast_quad.m_lo, fast_quad.m_hi);
        ee::ee_mem_read128_pair(&runtime, alias + 0x180u, &loaded);
        if (loaded.m_lo != fast_quad.m_lo || loaded.m_hi != fast_quad.m_hi ||
            memory.read64(0x180u) != fast_quad.m_lo ||
            memory.read64(0x188u) != fast_quad.m_hi)
        {
            return fail("paired RAM transfer alias or byte order");
        }
    }

    // The lowering's speculative store must not modify traced or non-RAM
    // memory before its per-lane diagnostic fallback.
    if (ee::ee_mem_try_write128_ram(&runtime, 0x800001a0u, fast_quad.m_lo, fast_quad.m_hi) != 1u ||
        memory.read64(0x1a0u) != fast_quad.m_lo ||
        memory.read64(0x1a8u) != fast_quad.m_hi)
    {
        return fail("untraced RAM-only store did not fuse");
    }
    runtime.m_guest_store_trace_enabled = true;
    if (ee::ee_mem_try_write128_ram(&runtime, 0x1a0u, 0u, 0u) != 0u ||
        memory.read64(0x1a0u) != fast_quad.m_lo ||
        memory.read64(0x1a8u) != fast_quad.m_hi)
    {
        return fail("traced speculative store mutated RAM");
    }
    runtime.m_guest_store_trace_enabled = false;
    if (ee::ee_mem_try_write128_ram(&runtime, k_vif_fifo, 0u, 0u) != 0u ||
        ee::ee_mem_try_write128_ram(&runtime, 0x70000010u, 0u, 0u) != 0u ||
        ee::ee_mem_try_write128_ram(&runtime, 0x11000000u, 0u, 0u) != 0u ||
        ee::ee_mem_try_write128_ram(&runtime, 0xff8u, 0u, 0u) != 0u ||
        io.m_writes != fast_writes)
    {
        return fail("speculative store touched non-eligible address");
    }

    // A partial allocation must retain the successful first scalar lane.
    // An aligned quadword can be partially backed by a non-16-byte allocation.
    ee::ee_memory partial_memory(0x108u);
    ee::s_ee_runtime partial_runtime{};
    partial_runtime.m_user = &partial_memory;
    partial_memory.set_kseg_ram_aliases(true);
    ee::ee_mem_write128_pair(&partial_runtime, 0x80000100u, fast_quad.m_lo, fast_quad.m_hi);
    ee::s_ee_u128 partial{};
    ee::ee_mem_read128_pair(&partial_runtime, 0x80000100u, &partial);
    if (partial.m_lo != fast_quad.m_lo || partial.m_hi != 0u ||
        partial_memory.read64(0x100u) != fast_quad.m_lo)
    {
        return fail("paired transfer lost first lane at RAM allocation boundary");
    }

    // Unaligned calls are not fused and the second address wraps as i32.
    ee::ee_mem_write128_pair(&runtime, 0xfffffff8u, fast_quad.m_lo, fast_quad.m_hi);
    ee::ee_mem_read128_pair(&runtime, 0xfffffff8u, &partial);
    if (partial.m_lo != 0u || partial.m_hi != fast_quad.m_hi ||
        memory.read64(0u) != fast_quad.m_hi)
    {
        return fail("paired transfer changed scalar address wraparound");
    }
    ee::ee_mem_read128_pair(nullptr, 0x100u, &partial);
    if (partial.m_lo != 0u || partial.m_hi != 0u)
    {
        return fail("paired transfer with absent runtime");
    }

    // Scratchpad is a separate mapping and must retain checked fallback.
    ee::ee_mem_write128(&runtime, 0x70000010u, &fast_quad);
    ee::s_ee_u128 scratch{};
    ee::ee_mem_read128(&runtime, 0x70000010u, &scratch);
    if (scratch.m_lo != fast_quad.m_lo || scratch.m_hi != fast_quad.m_hi ||
        memory.read64(0x10u) != 0u)
    {
        return fail("scratchpad fast-path isolation");
    }
    ee::ee_mem_write128_pair(&runtime, 0x70000030u, fast_quad.m_lo, fast_quad.m_hi);
    ee::ee_mem_read128_pair(&runtime, 0x70000030u, &scratch);
    if (scratch.m_lo != fast_quad.m_lo || scratch.m_hi != fast_quad.m_hi ||
        memory.read64(0x30u) != 0u)
    {
        return fail("paired scratchpad transfer isolation");
    }
    // Verify exact end-of-allocation access and reject partial spans.
    ee::ee_mem_write128(&runtime, 0x80000ff0u, &fast_quad);
    ee::ee_mem_write128(&runtime, 0x80000ff8u, &fast_quad);
    ee::ee_mem_write64(&runtime, 0x80000ffcu, 0u);
    ee::s_ee_u128 tail{};
    ee::ee_mem_read128(&runtime, 0x80000ff0u, &tail);
    if (tail.m_lo != fast_quad.m_lo || tail.m_hi != fast_quad.m_hi ||
        ee::ee_mem_read64(&runtime, 0x80000ffcu) != 0u)
    {
        return fail("direct RAM tail bounds");
    }
    ee::ee_mem_read128(&runtime, 0x80000ff8u, &tail);
    if (tail.m_lo != 0u || tail.m_hi != 0u ||
        ee::ee_mem_read32(nullptr, 0x101u) != 0u ||
        io.m_reads != fast_reads || io.m_writes != fast_writes)
    {
        return fail("partial RAM reads or spurious device callbacks");
    }

    // Direct-mapped KSEG device address translates to the same physical
    // register as a normal bus access. Width/physical address are preserved.
    if (ee::ee_mem_read8(&runtime, 0x9000f010u) != 0x11u ||
        io.m_last_address != k_intc_mask || io.m_last_width != 1 ||
        ee::ee_mem_read16(&runtime, 0xb000f010u) != 0x2211u ||
        io.m_last_address != k_intc_mask || io.m_last_width != 2 ||
        ee::ee_mem_read32(&runtime, k_intc_mask) != 0x44332211u ||
        io.m_last_width != 4 ||
        ee::ee_mem_read64(&runtime, 0xb000f010u) != 0x8877665544332211ull ||
        io.m_last_width != 8)
    {
        return fail("MMIO scalar width/alias/physical-address dispatch");
    }

    const uint32_t reads_before_128 = io.m_reads;
    ee::s_ee_u128 result{};
    ee::ee_mem_read128(&runtime, k_vif_fifo, &result);
    if (io.m_reads != reads_before_128 + 1 ||
        io.m_last_address != k_vif_fifo || io.m_last_width != 16 ||
        result.m_lo != 0x8877665544332211ull ||
        result.m_hi != 0xfedcba9876543210ull)
    {
        return fail("128-bit FIFO read split or value corrupted");
    }

    ee::ee_mem_write8(&runtime, k_intc_mask, 0xabu);
    if (io.m_last_address != k_intc_mask ||
        io.m_last_width != 1 || io.m_last_value.m_lo != 0xabu)
    {
        return fail("8-bit register write");
    }
    ee::ee_mem_write16(&runtime, k_intc_mask, 0x1234u);
    if (io.m_last_width != 2 || io.m_last_value.m_lo != 0x1234u)
    {
        return fail("16-bit register write");
    }
    ee::ee_mem_write32(&runtime, 0xb000f010u, 0xdeadbeefu);
    if (io.m_last_width != 4 || io.m_last_address != k_intc_mask ||
        io.m_last_value.m_lo != 0xdeadbeefu)
    {
        return fail("32-bit KSEG MMIO write");
    }
    ee::ee_mem_write64(&runtime, k_gs_csr, 0x0123456789abcdefull);
    if (io.m_last_width != 8 || io.m_last_address != k_gs_csr ||
        io.m_last_value.m_lo != 0x0123456789abcdefull)
    {
        return fail("64-bit GS register write");
    }

    const ee::s_ee_u128 quad{0x0123456789abcdefull, 0xfedcba9876543210ull};
    const uint32_t writes_before_128 = io.m_writes;
    ee::ee_mem_write128(&runtime, k_vif_fifo, &quad);
    if (io.m_writes != writes_before_128 + 1 ||
        io.m_last_address != k_vif_fifo || io.m_last_width != 16 ||
        io.m_last_value.m_lo != quad.m_lo ||
        io.m_last_value.m_hi != quad.m_hi)
    {
        return fail("128-bit FIFO write split or value corrupted");
    }

    // The paired helpers MUST NOT use the 16-byte FIFO transaction above.
    // The mock recognizes the first address but rejects the second one.
    const uint32_t reads_before_pair = io.m_reads;
    ee::ee_mem_read128_pair(&runtime, 0xb0004000u, &result);
    if (io.m_reads != reads_before_pair + 2u ||
        io.m_previous_address != k_vif_fifo || io.m_previous_width != 8u ||
        io.m_last_address != k_vif_fifo + 8u || io.m_last_width != 8u ||
        result.m_lo != 0x8877665544332211ull || result.m_hi != 0u)
    {
        return fail("paired MMIO read width, order or rejected lane");
    }

    const uint32_t writes_before_pair = io.m_writes;
    ee::ee_mem_write128_pair(&runtime, 0xb0004000u, quad.m_lo, quad.m_hi);
    if (io.m_writes != writes_before_pair + 2u ||
        io.m_previous_address != k_vif_fifo || io.m_previous_width != 8u ||
        io.m_last_address != k_vif_fifo + 8u || io.m_last_width != 8u ||
        io.m_previous_attempt.m_lo != quad.m_lo || io.m_previous_attempt.m_hi != 0u ||
        io.m_last_attempt.m_lo != quad.m_hi || io.m_last_attempt.m_hi != 0u)
    {
        return fail("paired MMIO write width, order or lane data");
    }

    // No MMIO callbacks must be generated for the existing VU0 apertures.
    const uint32_t writes_before_vu = io.m_writes;
    ee::ee_mem_write32(&runtime, 0xb1000000u, 0xaabbccddu);
    ee::ee_mem_write32(&runtime, 0x11004020u, 0x11223344u);
    if (io.m_writes != writes_before_vu ||
        ee::ee_mem_read32(&runtime, 0x11000000u) != 0xaabbccddu ||
        ee::ee_mem_read32(&runtime, 0xb1004020u) != 0x11223344u)
    {
        return fail("VU0 data/micro physical aliases or MMIO priority");
    }

    const uint32_t reads_before_vu_pair = io.m_reads;
    ee::ee_mem_write128_pair(&runtime, 0xb1000040u, quad.m_lo, quad.m_hi);
    ee::ee_mem_read128_pair(&runtime, 0x11000040u, &result);
    if (result.m_lo != quad.m_lo || result.m_hi != quad.m_hi ||
        io.m_reads != reads_before_vu_pair || io.m_writes != writes_before_vu ||
        memory.read64(0x40u) != 0u)
    {
        return fail("paired VU0 aperture isolation");
    }

    // A transfer starting in VU0 cannot spill into an MMIO callback.
    const uint32_t reads_before_cross = io.m_reads;
    const uint32_t writes_before_cross = io.m_writes;
    ee::ee_mem_write32(&runtime, 0x11000ffeu, 0xffffffffu);
    ee::ee_mem_read64(&runtime, 0x11004ffcu);
    ee::ee_mem_read128(&runtime, 0x12fffffcu, &result);
    if (io.m_reads != reads_before_cross ||
        io.m_writes != writes_before_cross ||
        ee::ee_mem_read32(&runtime, 0x11000000u) != 0xaabbccddu)
    {
        return fail("cross-aperture read/write dispatched or mutated VU0");
    }

    // A named but unimplemented hardware register must not behave like RAM.
    result.m_lo = ~uint64_t{0};
    result.m_hi = ~uint64_t{0};
    ee::ee_mem_read128(&runtime, 0x1000f100u, &result);
    if (result.m_lo != 0 || result.m_hi != 0)
    {
        return fail("unimplemented 128-bit MMIO read did not fail closed");
    }
    const uint32_t writes_before_unknown = io.m_writes;
    ee::ee_mem_write32(&runtime, 0x1000f100u, 0xffffffffu);
    if (io.m_writes != writes_before_unknown + 1 ||
        memory.read32(0x20u) != 0x12345678u)
    {
        return fail("unimplemented device wrote main memory");
    }

    // Physical aliases can be disabled again; physical registers still work.
    memory.set_kseg_ram_aliases(false);
    const uint32_t reads_before_disabled = io.m_reads;
    if (ee::ee_mem_read32(&runtime, 0x9000f010u) != 0 ||
        ee::ee_mem_read32(&runtime, 0xb000f010u) != 0 ||
        io.m_reads != reads_before_disabled ||
        memory.read32(0x20000020u) != 0 ||
        ee::ee_mem_read32(&runtime, k_intc_mask) != 0x44332211u)
    {
        return fail("disabled aliases still resolve or physical MMIO stopped");
    }

    runtime.m_mmio_read = nullptr;
    runtime.m_mmio_write = nullptr;
    ee::ee_mem_write32(&runtime, k_intc_mask, 0xaaaaaaaau);
    if (ee::ee_mem_read32(&runtime, k_intc_mask) != 0 ||
        ee::ee_mem_read64(&runtime, 0x1fc00000u) != 0)
    {
        return fail("absent MMIO callbacks returned invented hardware state");
    }

    std::puts("EE direct aliases / side-effectful MMIO callback widths: PASS");
    return 0;
}
