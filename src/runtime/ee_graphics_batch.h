#pragma once

#include <cstdint>

namespace ee
{
// QWC in an individual DMAC tag is 16 bits. A transaction assembled from
// multiple tags is a host batch, not a single guest QWC register value.
// Bound its storage independently (16 MiB), including synthetic TTE words.
// GIF parsing and GS staging share the same bound to preserve atomicity.
inline constexpr uint32_t k_max_graphics_batch_qwc = 0x100000u;
}
