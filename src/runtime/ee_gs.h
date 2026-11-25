#pragma once

#include "ee_gif.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ee
{
// GS register-state and swizzled PSMCT, PSMZ and indexed PSMT transfers.
// It does not rasterize primitives, present frames or implement GS IRQs.
// Based on ps2-web-recomp/runtime/src/gs/gs.cpp::write/write_packed.
// Bounded read-only provenance for committed sprite pairs. Raw GS values
// preserve all context/alpha/test/offset fields for offline interpretation.
struct s_ee_gs_sprite_sample
{
    uint64_t m_index = 0u;
    uint64_t m_prim = 0u;
    uint64_t m_effective_prim = 0u;
    uint64_t m_rgbaq = 0u;
    uint64_t m_xyz0 = 0u;
    uint64_t m_xyz1 = 0u;
    uint64_t m_xyoffset = 0u;
    uint64_t m_scissor = 0u;
    uint64_t m_frame = 0u;
    uint64_t m_zbuf = 0u;
    uint64_t m_test = 0u;
    uint32_t m_context = 0u;
};

struct s_ee_gs_state
{
    // In GPU-only mode palette values and physical VRAM are fetched on
    // demand; the remaining fields describe committed guest control state.
    bool m_gpu_vram_authoritative = false;
    std::array<uint64_t, 3> m_gpu_upload_layout{};
    std::array<uint64_t, 128> m_registers{};
    std::array<bool, 128> m_written{};
    uint64_t m_siglblid = 0u;
    bool m_signal_pending = false;
    uint32_t m_q_bits = 0x3f800000u; // GIFtag resets the STQ Q to 1.0
    uint64_t m_tag_count = 0;
    uint64_t m_vertex_count = 0; // diagnostic only; no rasterization
    uint32_t m_image_qwc = 0; // current host-to-local transfer, reset by TRXDIR=0

    // Cumulative *committed* GIF PATH3 traffic. These host diagnostics
    // do not draw, advance DMA, schedule GS fields, or alter guest registers.
    // Unlike m_vertex_count and m_image_qwc, these never reset on a new
    // PRIM or TRXDIR; reset() is the only clearing operation.
    uint64_t m_committed_gif_batches = 0u;
    uint64_t m_total_prim_writes = 0u;
    uint64_t m_total_vertex_writes = 0u;
    uint64_t m_total_vertex_kicks = 0u; // XYZF2/XYZ2, excluding ADC vertices
    uint64_t m_total_adc_vertices = 0u; // XYZF3/XYZ3
    uint64_t m_total_packed_events = 0u;
    uint64_t m_total_reglist_events = 0u;
    uint64_t m_total_ad_events = 0u;
    uint64_t m_total_image_events = 0u;
    uint64_t m_total_image_qwc = 0u;
    std::array<uint64_t, 4> m_tag_format_counts{}; // PACKED, REGLIST, IMAGE, IMAGE2
    std::array<uint64_t, 8> m_prim_type_counts{};
    std::array<uint64_t, 16> m_packed_register_counts{};
    std::array<uint64_t, 128> m_ad_register_counts{};

    static constexpr size_t k_sprite_sample_capacity = 8u;
    uint64_t m_total_sprite_pair_kicks = 0u; // committed primitive-6 XYZ2 pairs
    uint64_t m_total_sprite_textured_kicks = 0u; // effective PRIM.TME=1
    uint64_t m_total_sprite_nonblack_rgb_kicks = 0u; // raw RGBAQ RGB != 0
    uint64_t m_pending_sprite_xyz0 = 0u; // first vertex of next sprite pair
    std::array<s_ee_gs_sprite_sample, k_sprite_sample_capacity>
        m_first_sprite_samples{};
    std::array<s_ee_gs_sprite_sample, k_sprite_sample_capacity>
        m_recent_sprite_samples{};

    std::vector<uint8_t> m_image_data; // bounded diagnostic transfer capture
    bool m_transfer_started = false;
    uint32_t m_transfer_pixels_written = 0; // across IMAGE events
    uint8_t m_transfer_pending_size = 0; // PSMCT24 incomplete pixel
    std::array<uint8_t, 3> m_transfer_pending{};
    uint64_t m_hwreg_write_count = 0; // GS 0x54, 8-byte upload writes
    uint64_t m_hwreg_bytes = 0; // diagnostics, excludes IMAGE payload
    uint64_t m_local_copy_count = 0; // executed TRXDIR=2 operations
    uint64_t m_local_copy_pixels = 0; // copied pixels, including overlaps

    // Local-to-host transfer is read on demand by GIF/VIF1 reverse DMA.
    // Byte offset permits 24-bit partial pixels across 16-byte chunks.
    bool m_download_active = false;
    uint64_t m_download_bytes_read = 0u;
    uint64_t m_download_qwc = 0u;

    // Snapshot of the GS internal CLUT, not a view of current VRAM.
    // TEX0/TEX2 CLD controls when entries are reloaded from GS memory.
    std::array<uint32_t, 512> m_clut{};
    std::array<bool, 512> m_clut_valid{};
    uint32_t m_clut_cbp0 = 0xffffffffu;
    uint32_t m_clut_cbp1 = 0xffffffffu;
    uint64_t m_clut_load_count = 0;
};

// Transactional consumer for c_ee_gif_path3::t_event_callback.
// Unimplemented stateful GS operations and unknown registers are rejected.
// A second SIGNAL with CSR.SIGNAL already set requires resumable GIF
// stalling; until that is implemented, reject it transactionally.
class c_ee_gs_state
{
public:
    using t_committed_event_callback = void (*)(
        void* user, const s_ee_gif_event* events, size_t count,
        const uint8_t* data, uint32_t qwc
    );

    // Optional GPU mirror: invoked only AFTER transactional GS acceptance.
    // The callback is observer-only and cannot complete or reject guest DMA.
    void bind_committed_event_observer(
        void* user, t_committed_event_callback callback
    );

    // Optional GPU source for guest local-to-host transfers only.
    // CPU VRAM remains the transactional validation/reference mirror.
    // The callback must fill exactly one complete 4 MiB physical image
    // and report failure without publishing partial data to the guest.
    using t_vram_readback = bool (*)(void* user, uint8_t* memory, size_t bytes);
    void bind_vram_readback(void* user, t_vram_readback callback);
    using t_clut_readback = bool (*)(void* user, uint32_t* entries, size_t count);
    // Only a fresh GS may select this backend. No mid-run fallback to a
    // stale CPU mirror is permitted.
    bool enable_gpu_authority(void* user, t_vram_readback vram_callback,
                              t_clut_readback clut_callback);

    static constexpr size_t k_max_image_bytes = 4u * 1024u * 1024u;
    static constexpr size_t k_vram_bytes = 4u * 1024u * 1024u;
    // Keep per-batch staged writes bounded for malformed transfer sizes.
    static constexpr size_t k_max_local_copy_pixels = k_vram_bytes;

    // GS VRAM is separate from EE RAM. The vector reference remains
    // valid, but its backing storage may change after consume().
    // GPU authority: an empty vector indicates a failed snapshot read.
    const std::vector<uint8_t>& vram() const;
    bool read_vram8(uint32_t offset, uint8_t& value) const;
    // Returns false for out-of-range raw VRAM accesses.
    bool read_vram16(uint32_t offset, uint16_t& value) const;
    bool read_vram32(uint32_t offset, uint32_t& value) const;

    // Read-only swizzled GS display color access. RGB bytes are stored as
    // 0xAABBGGRR, with alpha forced opaque. No drawing or scanout occurs.
    bool read_color_pixel(uint32_t psm, uint32_t base, uint32_t fbw,
                          uint32_t x, uint32_t y, uint32_t& rgba) const;

    // Produce 16*qwc bytes from TRXDIR=1 into caller-owned memory.
    // Source pixels are swizzled as on GS, serialized little-endian
    // (4-bit low nibble first) and final qword padding is zero.
    // The download cursor commits only on a valid, complete read.
    bool read_image(uint8_t* data, uint32_t qwc);

    // Bind as c_ee_dmac::t_device_download_callback (channel 1/VIF1).
    static bool dma_download(
        void* user, uint32_t channel, uint8_t* data, uint32_t qwc
    );

    // Return raw 32-bit or 16-bit CPSM palette words without RGBA
    // expansion, alpha conversion, filtering, or texture rasterization.
    // Only entries loaded by a supported TEX0/TEX2 CLD are readable.
    bool read_clut_entry(uint32_t slot, uint32_t& value) const;
    bool read_indexed_clut_value(
        uint32_t context, uint32_t x, uint32_t y, uint32_t& value
    ) const;

    // Privileged GS registers share the same state as committed GIF A+D.
    using t_signal_irq = void (*)(void* user);
    void bind_signal_irq(void* user, t_signal_irq callback);
    uint64_t siglblid() const { return m_state.m_siglblid; }
    void set_siglblid(uint64_t value) { m_state.m_siglblid = value; }
    uint64_t signal_csr() const { return m_state.m_signal_pending ? 1u : 0u; }
    void acknowledge_signal() { m_state.m_signal_pending = false; }
    void reset_signal_state()
    {
        m_state.m_siglblid = 0u;
        m_state.m_signal_pending = false;
    }

    void reset();
    const s_ee_gs_state& state() const;
    bool was_written(uint32_t reg) const;
    uint64_t register_value(uint32_t reg) const;

    // Accepts or rejects one entire GIF event batch. The source buffer
    // is borrowed for the duration of this call only. On failure, neither
    // the GS state, the preceding GIF parser state, nor the DMA registers
    // are committed. No GPU commands are issued.
    bool consume(
        const s_ee_gif_event* events, size_t count,
        const uint8_t* data, uint32_t qwc
    );

    static bool gif_events(
        void* user, const s_ee_gif_event* events, size_t count,
        const uint8_t* data, uint32_t qwc
    );

private:
    struct s_vram_write
    {
        // 4 MiB VRAM requires only 22 address bits. Encode the byte
        // count (0 = nibble, 1..4 = bytes) and high-nibble bit alongside
        // the offset to keep the transaction journal at eight bytes.
        uint32_t m_address_and_format;
        uint32_t m_value;

        s_vram_write(uint32_t offset, uint32_t value, uint8_t bytes, uint8_t shift)
            : m_address_and_format(offset |
                (static_cast<uint32_t>(bytes) << 22u) |
                (static_cast<uint32_t>(shift >> 2u) << 25u)),
              m_value(value)
        {
        }

        uint32_t offset() const { return m_address_and_format & 0x3fffffu; }
        uint32_t bytes() const { return (m_address_and_format >> 22u) & 7u; }
        uint32_t shift() const { return ((m_address_and_format >> 25u) & 1u) * 4u; }
    };

    static_assert(sizeof(s_vram_write) == 8u, "compact VRAM journal entry");

    // GS PSMCT32/24: 64x32 pages, 8 KiB per page, 256-byte blocks.
    // Addresses wrap at 4 MiB, matching ps2-web-recomp GS memory.
    static uint32_t psmct32_address(
        uint32_t base, uint32_t width, uint32_t x, uint32_t y
    );

    // PSMCT16 and PSMCT16S use 64x64 pages with different 16x8
    // block permutations and a shared 16-bit column permutation.
    static uint32_t psmct16_address(
        uint32_t base, uint32_t width, uint32_t x, uint32_t y,
        uint32_t psm
    );

    // Depth PSMZ32/24 share 64x32 GS pages, but use k_blockz32.
    static uint32_t psmz32_address(
        uint32_t base, uint32_t width, uint32_t x, uint32_t y
    );

    // Depth PSMZ16/16S use separate 64x64 block permutations.
    static uint32_t psmz16_address(
        uint32_t base, uint32_t width, uint32_t x, uint32_t y,
        uint32_t psm
    );

    // PSMT8: 128x64 pages, 16x16 blocks and byte-lane swizzling.
    // DBW is still measured in units of 64 pixels.
    static uint32_t psmt8_address(
        uint32_t base, uint32_t width, uint32_t x, uint32_t y
    );

    // PSMT4: 128x128 pages, 32x16 blocks and nibble-lane swizzling.
    // The returned index is in nibbles, wrapped over 4 MiB GS VRAM.
    static uint32_t psmt4_nibble_address(
        uint32_t base, uint32_t width, uint32_t x, uint32_t y
    );

    // Pixel formats share address lookup, but differ in byte/nibble
    // widths. These helpers also preserve neighboring VRAM lanes.
    static bool supported_psm(uint32_t psm);
    static uint32_t read_pixel(
        const std::vector<uint8_t>& vram,
        uint32_t psm, uint32_t base, uint32_t width,
        uint32_t x, uint32_t y
    );
    static s_vram_write pixel_write(
        uint32_t psm, uint32_t base, uint32_t width,
        uint32_t x, uint32_t y, uint32_t value,
        uint8_t size
    );
    static void apply_vram_write(
        std::vector<uint8_t>& vram, const s_vram_write& write
    );

    // TRXDIR=2 snapshots the source side before any destination write.
    // The supplied VRAM view already includes earlier staged writes
    // within this GIF event batch, not later ones.
    static bool stage_local_copy(
        s_ee_gs_state& state, const std::vector<uint8_t>& source_vram,
        std::vector<s_vram_write>& writes, size_t* gpu_pixels = nullptr
    );

    // Both GIF IMAGE/IMAGE2 and A+D HWREG writes use the same
    // host-to-local pixel decoder and pending 24-bit carry.
    // Only completed pixels are staged; final padding is ignored.
    static bool stage_host_upload(
        s_ee_gs_state& state, std::vector<s_vram_write>& writes,
        const uint8_t* data, size_t byte_count, size_t* gpu_pixels = nullptr
    );

    // Shared transaction-local VRAM view for CLUT and local-copy reads.
    const std::vector<uint8_t>& staged_vram_view(
        const std::vector<s_vram_write>& writes,
        std::vector<uint8_t>& shadow, size_t& applied
    ) const;

    // Load a CLUT snapshot on TEX0/TEX2 writes, before final commit.
    bool load_clut(
        s_ee_gs_state& state, uint64_t tex0,
        const std::vector<s_vram_write>& writes,
        std::vector<uint8_t>& shadow, size_t& applied
    ) const;

    static bool write_register(
        s_ee_gs_state& state, uint32_t reg, uint64_t value
    );
    static bool write_packed(
        s_ee_gs_state& state, uint32_t reg, uint64_t low, uint64_t high
    );

    void* m_signal_user = nullptr;
    t_signal_irq m_signal_irq = nullptr;
    void* m_committed_user = nullptr;
    t_committed_event_callback m_committed_callback = nullptr;

    s_ee_gs_state m_state{};
    std::vector<uint8_t> m_vram = std::vector<uint8_t>(k_vram_bytes, 0u);

    // An accepted GIF batch may have changed GPU VRAM (including draws).
    // Cache a readback only until the next accepted nonempty batch.
    void* m_readback_user = nullptr;
    t_vram_readback m_readback_callback = nullptr;
    mutable std::vector<uint8_t> m_gpu_readback;
    mutable bool m_gpu_readback_valid = false;
    t_clut_readback m_clut_readback_callback = nullptr;
    mutable std::array<uint32_t, 512> m_gpu_clut{};
    mutable bool m_gpu_clut_valid = false;
    bool ensure_gpu_vram() const;
    bool ensure_gpu_clut() const;

    // Host-only scratch. Neither buffer is committed guest state.
    std::vector<s_vram_write> m_write_scratch;
    std::vector<uint8_t> m_shadow_scratch;
};
} // namespace ee
