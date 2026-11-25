#pragma once
#include "ee_state.h"
#include "ee_tlb.h"
#include <cstdint>
#include <cstddef>

namespace ee {

// Bounded EE RAM and 16 KiB scratchpad backing the guest memory helper ABI.
// This remains a prototype: TLB, MMIO, and address exceptions are not modelled.
class ee_memory
{
public:
    static constexpr uint32_t k_scratchpad_base = 0x70000000u;
    static constexpr size_t k_scratchpad_size = 16u * 1024u;

    explicit ee_memory(size_t size);
    ~ee_memory();

    uint8_t  read8 (uint32_t addr);
    uint16_t read16(uint32_t addr);
    uint32_t read32(uint32_t addr);
    uint64_t read64(uint32_t addr);
    void     read128(uint32_t addr, s_ee_u128* out);

    void     write8 (uint32_t addr, uint8_t  v);
    void     write16(uint32_t addr, uint16_t v);
    void     write32(uint32_t addr, uint32_t v);
    void     write64(uint32_t addr, uint64_t v);
    void     write128(uint32_t addr, const s_ee_u128* v);

    bool contains(uint32_t addr, size_t len) const;

    // Borrow a contiguous, bounds-checked backing span for a synchronous
    // host DMA copy. No MMIO, TLB lookup or guest side effect is implied.
    // The pointer is valid only while this memory object remains alive.
    const uint8_t* read_span(uint32_t addr, size_t len) const
    {
        return map_address(addr, len);
    }

    // Opt-in direct-mapped EE aliases. The original KSEG option now also
    // covers the EE's 0x2/0x3 uncached/accelerated aliases, matching
    // ps2-web-recomp's physical() mapping; the actual RAM buffer bounds
    // are still checked. This does not implement the TLB.
    void set_kseg_ram_aliases(bool enabled);

    // Persistent across EE thread switches; guest TLBWI/TLBR/TLBP state.
    c_ee_tlb& tlb();
    const c_ee_tlb& tlb() const;

    // Returns the physical bus address for known, enabled direct aliases,
    // even when the target is a device rather than RAM. Leaves all other
    // guest addresses unchanged. Used by the guest MMIO/VU dispatcher.
    uint32_t translate_direct_address(uint32_t addr) const;

    // The guest bus dispatcher has already translated the address and
    // excluded hardware apertures. Check the actual allocation once.
    // This is intentionally not a guest-address or scratchpad mapper.
    uint8_t* physical_ram_span(uint32_t physical, size_t len)
    {
        const size_t offset = static_cast<size_t>(physical);
        return m_data != nullptr && offset <= m_size && len <= m_size - offset
            ? m_data + offset : nullptr;
    }

    // Direct-RAM fast path (Phase 7 optimisation). Marks a guest address
    // range as safe for direct host pointer access, bypassing bounds checks.
    // Accesses outside this range still go through the checked helpers.
    void set_ram_range(uint32_t base, uint32_t size);

    // Unchecked direct access; only valid for addresses within the RAM range
    // set via set_ram_range. Caller must ensure the address is in range.
    uint32_t direct_read32(uint32_t addr);
    void     direct_write32(uint32_t addr, uint32_t v);
    uint64_t direct_read64(uint32_t addr);
    void     direct_write64(uint32_t addr, uint64_t v);

private:
    uint8_t* m_data = nullptr;
    size_t m_size = 0;
    alignas(16) uint8_t m_scratchpad[k_scratchpad_size]{};
    uint32_t m_ram_base = 0;
    uint32_t m_ram_size = 0;
    bool m_ram_configured = false;
    bool m_kseg_ram_aliases = false;
    c_ee_tlb m_tlb{};

    bool in_ram_range(uint32_t addr, size_t len) const;
    uint32_t normalize_ram_address(uint32_t addr) const;
    const uint8_t* map_address(uint32_t addr, size_t len) const;
    uint8_t* map_address(uint32_t addr, size_t len);
};

} // namespace ee
