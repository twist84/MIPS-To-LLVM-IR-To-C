#pragma once

#include <cstddef>
#include <cstdint>

namespace ee
{
// Minimal, transport-level GIF PATH3 event. The parser does not interpret
// GS register semantics or draw. For image events, m_byte_offset points into
// the original DMA input buffer, valid only during the event callback.
enum class e_ee_gif_event_type : uint8_t
{
    tag,
    primitive,
    packed,
    reglist,
    image
};

struct s_ee_gif_event
{
    e_ee_gif_event_type m_type = e_ee_gif_event_type::tag;
    uint32_t m_register = 0;
    uint32_t m_byte_offset = 0;
    uint32_t m_qwc = 0;
    uint64_t m_word0 = 0;
    uint64_t m_word1 = 0;
};

// Per-parser input provenance. Counters advance only after the GS sink
// accepts a full batch; rejected input never contributes committed events.
struct s_ee_gif_path_counters
{
    uint64_t m_accepted_batches = 0u;
    uint64_t m_rejected_batches = 0u;
    uint64_t m_input_qwc = 0u;
    uint64_t m_tags = 0u;
    uint64_t m_primitive_events = 0u;
    uint64_t m_packed_events = 0u;
    uint64_t m_reglist_events = 0u;
    uint64_t m_image_events = 0u;
    uint64_t m_image_qwc = 0u;
    // Read-only provenance of accepted GIF IMAGE/IMAGE2 payload bytes.
    // These bytes are not necessarily visible texture/framebuffer pixels.
    uint64_t m_image_nonzero_bytes = 0u;
    uint64_t m_image_payload_fnv1a64 = 0u;
};

struct s_ee_gif_path3_state
{
    uint32_t m_nloop = 0;
    uint32_t m_loop = 0;
    uint32_t m_nreg = 0;
    uint32_t m_reg = 0;
    uint32_t m_flg = 0;
    uint64_t m_regs = 0;
    bool m_active = false;
    bool m_eop = false;
};

struct s_ee_gif_segment
{
    uint32_t m_path = 0u;
    const uint8_t* m_data = nullptr;
    uint32_t m_qwc = 0u;
};

// Stateful PATH3 parser based on ps2-web-recomp/runtime/src/gs/gif.cpp::walk.
// The caller owns the event sink and must commit or reject a whole batch
// atomically. A rejected chunk preserves parser state and (when used through
// the DMAC callback) leaves channel MADR/QWC/STR unchanged.
class c_ee_gif_path3
{
public:
    using t_event_callback = bool (*)(
        void* user, const s_ee_gif_event* events, size_t event_count,
        const uint8_t* data, uint32_t qwc
    );

    void bind_event_sink(void* user, t_event_callback callback);
    void reset();
    const s_ee_gif_path3_state& state() const;
    const s_ee_gif_path_counters& counters() const;

    bool consume(const uint8_t* data, uint32_t qwc);

    // Mixed PATH1/PATH2 input uses a shared transactional sink. Each
    // segment is parsed with its own path state, but GS sees one ordered
    // event stream. No parser or GS state commits on rejection.
    static bool consume_ordered(c_ee_gif_path3& path1, c_ee_gif_path3& path2,
                                const s_ee_gif_segment* segments, size_t count);

    // Compatible with c_ee_dmac::t_device_transfer_callback.
    // Only channel 2 (GIF PATH3) is accepted.
    static bool dma_transfer(
        void* user, uint32_t channel, const uint8_t* data, uint32_t qwc
    );

private:
    s_ee_gif_path3_state m_state{};
    s_ee_gif_path_counters m_counters{};
    void* m_sink_user = nullptr;
    t_event_callback m_sink = nullptr;
};
} // namespace ee
