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

    // A cold zero-key slot must miss even when capacity is zero.
    // Cached rejections update the correct independent untracked counter.
    ee::c_ee_dispatch_hotspots no_pc(0u, 1u);
    no_pc.record(1u, 0u, 0u, {});
    no_pc.record(2u, 0u, 0u, {});
    no_pc.record(3u, 0u, 1u, {});
    no_pc.record(4u, 0u, 1u, {});
    no_pc.record(5u, 0u, 0u, {});
    const auto no_pc_json = serialize(no_pc);
    assert(no_pc.untracked_pc_samples() == 5u);
    assert(no_pc.untracked_edge_samples() == 2u);
    assert(no_pc_json.find("\"pc_cache_misses\": 1") != std::string::npos);
    assert(no_pc_json.find("\"edge_cache_misses\": 3") != std::string::npos);
    assert(no_pc_json.find("\"from_pc\": 0, \"to_pc\": 0, \"visits\": 3") != std::string::npos);

    ee::c_ee_dispatch_hotspots no_edge(1u, 0u);
    no_edge.record(1u, 0u, 0u, {});
    no_edge.record(2u, 0u, 0u, {});
    assert(no_edge.untracked_pc_samples() == 0u);
    assert(no_edge.untracked_edge_samples() == 2u);
    const auto no_edge_json = serialize(no_edge);
    assert(no_edge_json.find("\"pc\": 0, \"visits\": 2") != std::string::npos);
    assert(no_edge_json.find("\"edge_cache_misses\": 1") != std::string::npos);

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

    // More than sixteen distinct keys: highest counts first, then
    // ascending full-width keys at the cutoff, independent of insertion.
    ee::c_ee_dispatch_hotspots cutoff;
    uint64_t dispatch = 0u;
    for (uint32_t i = 32u; i != 0u; --i)
    {
        const uint32_t visits = i % 5u == 0u ? 3u : 1u;
        for (uint32_t j = 0u; j < visits; ++j)
        {
            cutoff.record(++dispatch, i * 4u, i * 4u + 4u, {});
        }
    }
    const std::string cutoff_json = serialize(cutoff);
    size_t pc_offset = cutoff_json.find("\"hot_pcs\": [");
    size_t edge_offset = cutoff_json.find("\"hot_transitions\": [");
    const size_t pc_end = edge_offset;
    const size_t edge_end = cutoff_json.find("\"latest_state\":");
    const auto check_rank = [&](uint32_t i, uint32_t visits)
    {
        const std::string count = std::to_string(visits);
        const std::string pc = std::to_string(i * 4u);
        pc_offset = cutoff_json.find(
            "{\"pc\": " + pc + ", \"visits\": " + count + "}", pc_offset);
        edge_offset = cutoff_json.find(
            "{\"from_pc\": " + pc + ", \"to_pc\": " +
            std::to_string(i * 4u + 4u) + ", \"visits\": " + count + "}", edge_offset);
        assert(pc_offset < pc_end && edge_offset < edge_end);
        ++pc_offset;
        ++edge_offset;
    };
    for (uint32_t i = 5u; i <= 30u; i += 5u)
    {
        check_rank(i, 3u);
    }
    uint32_t remaining = 10u;
    for (uint32_t i = 1u; remaining != 0u; ++i)
    {
        if (i % 5u != 0u)
        {
            check_rank(i, 1u);
            --remaining;
        }
    }
    assert(cutoff_json.find("{\"pc\":", pc_offset) >= pc_end);
    assert(cutoff_json.find("{\"from_pc\":", edge_offset) >= edge_end);
    assert(cutoff.observations() == 44u);

    // Exercise the expanded default working set beyond both old
    // limits, including a second pass through the full counter caches.
    ee::c_ee_dispatch_hotspots retail;
    for (uint32_t pass = 0u; pass < 2u; ++pass)
    {
        for (uint32_t i = 0u; i < 9000u; ++i)
        {
            retail.record(pass * 9000u + i + 1u, i * 4u,
                0x100000u + i * 4u, {});
        }
    }
    assert(retail.observations() == 18000u);
    assert(retail.untracked_pc_samples() == 0u);
    assert(retail.untracked_edge_samples() == 0u);
    const auto retail_json = serialize(retail);
    assert(retail_json.find("\"tracked_pcs\": 9000") != std::string::npos);
    assert(retail_json.find("\"tracked_edges\": 9000") != std::string::npos);
    assert(retail_json.find("\"pc\": 0, \"visits\": 2") != std::string::npos);
    assert(retail_json.find("\"from_pc\": 0, \"to_pc\": 1048576, \"visits\": 2") != std::string::npos);

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

    // Packed edge keys retain all 64 bits and the original (from, to)
    // tie order, even when one half is zero or the full uint32_t range.
    ee::c_ee_dispatch_hotspots full_width;
    full_width.record(1u, 0xffffffffu, 0u, {});
    full_width.record(2u, 0u, 0xffffffffu, {});
    full_width.record(3u, 0xffffffffu, 0xffffffffu, {});
    full_width.record(4u, 0u, 0u, {});
    full_width.record(5u, 0xffffffffu, 0xffffffffu, {});
    const std::string full_json = serialize(full_width);
    const auto full_first = full_json.find(
        "\"from_pc\": 4294967295, \"to_pc\": 4294967295, \"visits\": 2");
    const auto full_zero = full_json.find(
        "\"from_pc\": 0, \"to_pc\": 0, \"visits\": 1");
    const auto full_low = full_json.find(
        "\"from_pc\": 0, \"to_pc\": 4294967295, \"visits\": 1");
    const auto full_high = full_json.find(
        "\"from_pc\": 4294967295, \"to_pc\": 0, \"visits\": 1");
    assert(full_first < full_zero && full_zero < full_low &&
        full_low < full_high && full_high != std::string::npos);

    // Every state field independently participates in the equality
    // reduction, including the upper half of each 64-bit field.
    const ee::s_ee_hot_loop_state unchanged{};
    assert(unchanged == ee::s_ee_hot_loop_state{});
    const auto verify_field = [&](auto member, auto value)
    {
        auto different = unchanged;
        different.*member = value;
        assert(unchanged != different);
        assert(different != unchanged);
        assert(different == different);
    };
    verify_field(&ee::s_ee_hot_loop_state::m_thread_id, 0x80000000u);
    verify_field(&ee::s_ee_hot_loop_state::m_cop0_status, 0x80000000u);
    verify_field(&ee::s_ee_hot_loop_state::m_cop0_cause, 0x80000000u);
    verify_field(&ee::s_ee_hot_loop_state::m_intc_status, 0x80000000u);
    verify_field(&ee::s_ee_hot_loop_state::m_intc_mask, 0x80000000u);
    verify_field(&ee::s_ee_hot_loop_state::m_dmac_status, 0x80000000u);
    verify_field(&ee::s_ee_hot_loop_state::m_dmac_pending, 0x80000000u);
    verify_field(&ee::s_ee_hot_loop_state::m_gif_chcr, 0x80000000u);
    verify_field(&ee::s_ee_hot_loop_state::m_gif_madr, 0x80000000u);
    verify_field(&ee::s_ee_hot_loop_state::m_gif_qwc, 0x80000000u);
    verify_field(&ee::s_ee_hot_loop_state::m_gif_tadr, 0x80000000u);
    verify_field(&ee::s_ee_hot_loop_state::m_gif_reject, 0x80000000u);
    verify_field(&ee::s_ee_hot_loop_state::m_gif_write_sequence, 0x8000000000000000ull);
    verify_field(&ee::s_ee_hot_loop_state::m_gs_fields, 0x8000000000000000ull);
    verify_field(&ee::s_ee_hot_loop_state::m_gs_csr, 0x8000000000000000ull);
    verify_field(&ee::s_ee_hot_loop_state::m_iop_rpc_sequence, 0x8000000000000000ull);

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
