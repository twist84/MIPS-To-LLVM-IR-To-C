#include "ee_hot_loop_diagnostics.h"
#include <cassert>
#include <cstdio>
#include <string>

static std::string serialize(const ee::c_ee_dispatch_hotspots& hotspots)
{
    FILE* file = std::tmpfile();
    assert(file != nullptr);
    hotspots.write_json(file);
    assert(std::fflush(file) == 0);
    assert(std::fseek(file, 0, SEEK_SET) == 0);
    std::string json;
    char buffer[2048];
    size_t count = 0u;
    while ((count = std::fread(buffer, 1u, sizeof(buffer), file)) != 0u)
    {
        json.append(buffer, count);
    }
    assert(std::fclose(file) == 0);
    return json;
}

int main()
{
    ee::c_bounded_diagnostic_history<uint64_t, 64u> history;
    assert(history.empty());
    for (uint64_t i = 0u; i < 257u; ++i)
    {
        history.push_back(i);
        assert(history.size() == (i < 64u ? i + 1u : 64u));
        assert(history[history.size() - 1u] == i);
    }
    for (size_t i = 0u; i < history.size(); ++i)
    {
        assert(history[i] == 193u + i);
    }

    ee::c_ee_dispatch_hotspots zero(0u, 0u);
    zero.record(1u, 4u, 8u, {});
    assert(zero.untracked_pc_samples() == 1u);
    assert(zero.untracked_edge_samples() == 1u);

    // Ties must serialize in key order independent of insertion order.
    ee::c_ee_dispatch_hotspots ties;
    ties.record(1u, 16u, 24u, {});
    ties.record(2u, 4u, 12u, {});
    ties.record(3u, 4u, 8u, {});
    ties.record(4u, 16u, 20u, {});
    const std::string ordered = serialize(ties);
    const auto pc_a = ordered.find("\"pc\": 4, \"visits\": 2");
    const auto pc_b = ordered.find("\"pc\": 16, \"visits\": 2");
    assert(pc_a < pc_b && pc_b != std::string::npos);
    const auto edge_a = ordered.find("\"from_pc\": 4, \"to_pc\": 8");
    const auto edge_b = ordered.find("\"from_pc\": 4, \"to_pc\": 12");
    const auto edge_c = ordered.find("\"from_pc\": 16, \"to_pc\": 20");
    const auto edge_d = ordered.find("\"from_pc\": 16, \"to_pc\": 24");
    assert(edge_a < edge_b && edge_b < edge_c && edge_c < edge_d &&
        edge_d != std::string::npos);

    ee::c_ee_dispatch_hotspots empty;
    const std::string baseline = serialize(empty);
    assert(baseline.find("\"schema\": \"ee-m2-dispatch-hotspots-v1\"") != std::string::npos);
    assert(baseline.find("\"observations\": 0") != std::string::npos);
    assert(baseline.find("\"latest_state\": null") != std::string::npos);

    ee::c_ee_dispatch_hotspots hotspots(2u, 2u);
    ee::s_ee_hot_loop_state initial{};
    initial.m_intc_mask = 7u;
    hotspots.record(1u, 0x172e20u, 0x172e48u, initial);
    hotspots.record(2u, 0x172e20u, 0x172e48u, initial);
    ee::s_ee_hot_loop_state changed = initial;
    changed.m_gs_fields = 1u;
    changed.m_gif_chcr = 0x100u;
    changed.m_gif_write_sequence = 4u;
    hotspots.record(3u, 0x1730c0u, 0x172e20u, changed);
    hotspots.record(4u, 0x102670u, 0x1730c0u, changed);
    hotspots.record(5u, 0x172e20u, 0x172e48u, changed);
    assert(hotspots.observations() == 5u);
    assert(hotspots.untracked_pc_samples() == 1u);
    assert(hotspots.untracked_edge_samples() == 1u);
    const std::string json = serialize(hotspots);
    assert(json.find("\"visits\": 3") != std::string::npos);
    assert(json.find("\"untracked_pc_samples\": 1") != std::string::npos);
    assert(json.find("\"total_state_changes\": 1") != std::string::npos);
    assert(json.find("\"gs_fields\": 1") != std::string::npos);
    assert(json.find("\"gif_chcr\": 256") != std::string::npos);
    assert(json.find("\"gif_write_sequence\": 4") != std::string::npos);

    // Zero, unaligned addresses, and untracked keys after capacity fills
    // must preserve the exact bounded map counts and ordering.
    ee::c_ee_dispatch_hotspots collision(3u, 3u);
    for (uint32_t i = 0u; i < 100u; ++i)
    {
        collision.record(i * 5u + 1u, 0u, 0u, {});
        collision.record(i * 5u + 2u, 0x1000u, 0x1000u, {});
        collision.record(i * 5u + 3u, 0x2001u, 0x2001u, {});
        collision.record(i * 5u + 4u, 0x3000u, 0x3000u, {});
        collision.record(i * 5u + 5u, 0x3000u, 0x3000u, {});
    }
    const std::string collided = serialize(collision);
    assert(collision.observations() == 500u);
    assert(collision.untracked_pc_samples() == 200u);
    assert(collision.untracked_edge_samples() == 200u);
    assert(collided.find("\"pc\": 0, \"visits\": 100") != std::string::npos);
    assert(collided.find("\"pc\": 4096, \"visits\": 100") != std::string::npos);
    assert(collided.find("\"pc\": 8193, \"visits\": 100") != std::string::npos);
    assert(collided.find("\"from_pc\": 0, \"to_pc\": 0, \"visits\": 100") != std::string::npos);
    assert(collided.find("\"from_pc\": 4096, \"to_pc\": 4096, \"visits\": 100") != std::string::npos);
    assert(collided.find("\"from_pc\": 8193, \"to_pc\": 8193, \"visits\": 100") != std::string::npos);
    assert(collided.find("\"total_state_changes\": 0") != std::string::npos);

    // The low address bits differ but the word-based cache slots match.
    // Repeated rejected keys must remain cacheable after capacity fills.
    ee::c_ee_dispatch_hotspots cache(1u, 1u);
    cache.record(1u, 0u, 0u, {});
    cache.record(2u, 0u, 0u, {});
    cache.record(3u, 1u, 1u, {});
    cache.record(4u, 1u, 1u, {});
    cache.record(5u, 0u, 0u, {});
    const std::string cached = serialize(cache);
    assert(cache.untracked_pc_samples() == 2u);
    assert(cache.untracked_edge_samples() == 2u);
    assert(cached.find("\"pc\": 0, \"visits\": 3") != std::string::npos);
    assert(cached.find("\"pc_cache_misses\": 3") != std::string::npos);
    assert(cached.find("\"edge_cache_misses\": 3") != std::string::npos);

    ee::c_ee_dispatch_hotspots changes;
    ee::s_ee_hot_loop_state state{};
    for (uint32_t i = 0u; i < 100u; ++i)
    {
        state.m_gs_fields = i;
        changes.record(i + 1u, 0x172e20u, 0x172e48u, state);
    }
    const std::string bounded = serialize(changes);
    assert(bounded.find("\"total_state_changes\": 99") != std::string::npos);
    assert(bounded.find("\"dispatch\": 2,") == std::string::npos);
    size_t offset = 0u;
    for (uint32_t i = 69u; i <= 100u; ++i)
    {
        offset = bounded.find("\"dispatch\": " + std::to_string(i) + ",", offset);
        assert(offset != std::string::npos);
        ++offset;
    }
    assert(bounded.find("\"dispatch\": 68,") == std::string::npos);
    return 0;
}
