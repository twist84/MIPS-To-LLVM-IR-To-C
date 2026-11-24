#include "ee_scanout_clock.h"
#include "ee_hw_bus.h"
#include "ee_intc.h"
#include "ee_vsync_flags.h"
#include "ee_memory.h"

#include <cstdint>
#include <cstdio>
#include <limits>

namespace
{
int g_failed = 0;

void check(bool result, const char* label)
{
    if (!result)
    {
        std::printf("FAIL: %s\n", label);
        ++g_failed;
    }
}
}

int main()
{
    ee::c_ee_scanout_clock clock{};
    ee::c_ee_intc intc{};
    ee::ee_memory memory(0x1000u);
    ee::c_ee_vsync_flags flags{};
    ee::s_ee_cpu_state cpu{};
    intc.bind_cpu(&cpu);

    check(!clock.advance_to(0u, &intc, &flags, &memory) &&
          !clock.enabled() && intc.status() == 0u &&
          clock.next_event_cycle() == std::numeric_limits<uint64_t>::max(),
          "unconfigured scanout never emits an event");
    check(!clock.configure(1u, 0x1au, 0u, 0u),
          "unsupported VESA timing has no invented clock");
    check(!clock.configure(1u, 2u, 2u, 0u),
          "invalid FFMD mode is rejected");

    const uint64_t start = 1000u;
    check(clock.configure(1u, 2u, 0u, start) &&
          clock.next_field_cycle() == start + 4920115ull &&
          clock.next_event_cycle() == clock.next_field_cycle() &&
          clock.fields_started() == 0u,
          "guest NTSC mode 2 schedules precise first 60000/1001 field");

    memory.write32(0x100u, 0x12345678u);
    memory.write64(0x108u, 0xfedcba9876543210ull);
    check(flags.register_guest_pointers(&memory, 0x100u, 0x108u),
          "store guest VSync registration without writing memory");
    const uint64_t first_field = clock.next_field_cycle();
    check(clock.advance_to(first_field - 1u, &intc, &flags, &memory) &&
          clock.fields_started() == 0u &&
          intc.status() == 0u && memory.read32(0x100u) == 0x12345678u &&
          memory.read64(0x108u) == 0xfedcba9876543210ull,
          "no VSync, CSR or guest flag before actual EE field boundary");

    check(clock.advance_to(first_field, &intc, &flags, &memory) &&
          clock.fields_started() == 1u &&
          intc.status() == 4u && intc.pending() == 0u &&
          memory.read32(0x100u) == 1u &&
          memory.read64(0x108u) == clock.csr() &&
          (clock.csr() & ee::c_ee_scanout_clock::k_csr_vsint) != 0u &&
          (clock.csr() & ee::c_ee_scanout_clock::k_csr_field) != 0u &&
          flags.flag_address() == 0u,
          "field start latches GS VSINT, asserts INTC_STAT and consumes callback");

    // INTC mask gates only the pending CPU interrupt; not INTC_STAT.
    const ee::s_ee_u128 stat_ack{4u, 0u};
    const ee::s_ee_u128 mask_toggle{4u, 0u};
    check(ee::c_ee_intc::mmio_write(
              &intc, ee::c_ee_intc::k_stat_address, 4u, &stat_ack) &&
          intc.status() == 0u,
          "INTC_STAT acknowledges only by write-one-to-clear");
    check(ee::c_ee_intc::mmio_write(
              &intc, ee::c_ee_intc::k_mask_address, 4u, &mask_toggle) &&
          intc.mask() == 4u && intc.pending() == 0u,
          "INTC_MASK toggles without inventing an interrupt");

    memory.write32(0x100u, 0x22222222u);
    memory.write64(0x108u, 0x3333333333333333ull);
    const uint64_t first_end =
        first_field + (4920115ull * 44u / 525u);
    check(clock.advance_to(first_end - 1u, &intc, &flags, &memory) &&
          clock.fields_ended() == 0u &&
          clock.next_event_cycle() == first_end &&
          clock.last_ee_cycle() == first_end - 1u &&
          clock.delivered_vblank_edges().empty(),
          "VBlank end never fires before its guest deadline");
    check(clock.advance_to(first_end, &intc, &flags, &memory) &&
          clock.fields_ended() == 1u &&
          clock.delivered_vblank_edges().size() == 1u &&
          clock.delivered_vblank_edges()[0] == 0u &&
          clock.fields_started() == 1u &&
          clock.next_event_cycle() == clock.next_field_cycle(),
          "VBlank end fires from the actual field-start clock");
    const uint64_t second_field = clock.next_field_cycle();
    clock.acknowledge_csr(ee::c_ee_scanout_clock::k_csr_vsint);
    check((clock.csr() & ee::c_ee_scanout_clock::k_csr_vsint) == 0u &&
          (clock.csr() & ee::c_ee_scanout_clock::k_csr_field) != 0u,
          "CSR W1C clears VSINT without touching field parity");
    check(clock.advance_to(second_field, &intc, &flags, &memory) &&
          clock.fields_started() == 2u && intc.pending() == 4u &&
          memory.read32(0x100u) == 0x22222222u &&
          memory.read64(0x108u) == 0x3333333333333333ull &&
          (clock.csr() & ee::c_ee_scanout_clock::k_csr_field) == 0u,
          "one-shot callback is not reissued on subsequent field");
    check(clock.next_field_cycle() == start + 3u * 4920115ull,
          "third NTSC field has correct rational phase");
    check(clock.advance_to(start + 5ull * 4920115ull + 1u,
                           &intc, &flags, &memory) &&
          clock.fields_started() == 5u &&
          clock.next_field_cycle() == start + 6ull * 4920115ull + 1u,
          "NTSC field cycle fractions accumulate without timing drift");

    const uint64_t prev_cycle = clock.last_ee_cycle();
    const uint64_t prev_fields = clock.fields_started();
    check(!clock.advance_to(prev_cycle - 1u, &intc, &flags, &memory) &&
          clock.last_ee_cycle() == prev_cycle &&
          clock.fields_started() == prev_fields,
          "non-monotonic EE clock is rejected without mutation");
    check(!clock.configure(1u, 0x1au, 0u, prev_cycle) &&
          clock.last_ee_cycle() == prev_cycle &&
          clock.fields_started() == prev_fields,
          "unsupported CRT mode preserves current timing state");

    ee::ee_memory other(0x80u);
    check(flags.register_guest_pointers(&memory, 0x100u, 0x108u),
          "re-arm VSync callback for bounds test");
    const uint64_t due = clock.next_field_cycle();
    const uint64_t prior_csr = clock.csr();
    const uint32_t prior_status = intc.status();
    check(!clock.advance_to(due, &intc, &flags, &other) &&
          clock.next_field_cycle() == due &&
          clock.fields_started() == prev_fields &&
          intc.status() == prior_status && clock.csr() == prior_csr,
          "stale guest RAM backing refuses event and commits no state");
    check(clock.advance_to(due, &intc, &flags, &memory) &&
          memory.read32(0x100u) == 1u &&
          memory.read64(0x108u) == clock.csr(),
          "backed guest event retries without losing registration");

    check(clock.configure(1u, 3u, 0u, 0u) &&
          clock.next_field_cycle() == 5898240ull &&
          clock.advance_to(5898239ull, &intc, &flags, &memory) &&
          clock.fields_started() == 0u &&
          clock.advance_to(5898240ull, &intc, &flags, &memory) &&
          clock.fields_started() == 1u,
          "PAL CRT mode 3 schedules a real 50 Hz field boundary");
    // The HLE EE bus now exposes privileged GS_CSR/GS_IMR through
    // the same scanout state used for real VBlank callbacks. No manual
    // CSR store can generate VBlank or set an interrupt.
    ee::c_ee_hw_bus bus{};
    ee::s_ee_cpu_state bus_cpu{};
    bus.bind_cpu(&bus_cpu);
    ee::c_ee_vsync_flags bus_flags{};
    ee::ee_memory bus_ram(0x1000u);
    ee::s_ee_u128 read_value{};
    const ee::s_ee_u128 imr_write{0x0000ab00u, 0u};
    const ee::s_ee_u128 invalid_write{0x00000008u, 0u};
    check(!bus.scanout().enabled() &&
          !ee::c_ee_hw_bus::mmio_read(
              &bus, ee::c_ee_hw_bus::k_gs_csr, 2u, &read_value) &&
          ee::c_ee_hw_bus::mmio_write(
              &bus, ee::c_ee_hw_bus::k_gs_imr, 8u, &imr_write) &&
          ee::c_ee_hw_bus::mmio_read(
              &bus, ee::c_ee_hw_bus::k_gs_imr, 8u, &read_value) &&
          read_value.m_lo == 0x0000ab00u &&
          !ee::c_ee_hw_bus::mmio_write(
              &bus, ee::c_ee_hw_bus::k_gs_imr + 2u, 2u, &invalid_write) &&
          ee::c_ee_hw_bus::mmio_write(
              &bus, ee::c_ee_hw_bus::k_gs_csr, 8u, &invalid_write) &&
          bus.intc().status() == 0u &&
          bus.scanout().fields_started() == 0u,
          "GS MMIO registers retain masks without inventing scanout");

    check(bus.set_gs_crt(1u, 2u, 0u) &&
          bus.scanout().enabled() &&
          bus.scanout().next_field_cycle() == 4920115ull &&
          bus_flags.register_guest_pointers(&bus_ram, 0x100u, 0x108u) &&
          bus_ram.read32(0x100u) == 0u &&
          bus.scanout().fields_started() == 0u,
          "SetGsCrt schedules a field but never produces it");

    const uint64_t event_cycle = bus.scanout().next_field_cycle();
    check(bus.scanout().advance_to(
              event_cycle, &bus.intc(), &bus_flags, &bus_ram) &&
          bus.scanout().fields_started() == 1u &&
          bus_ram.read32(0x100u) == 1u &&
          bus.intc().status() == 4u &&
          ee::c_ee_hw_bus::mmio_read(
              &bus, ee::c_ee_hw_bus::k_gs_csr, 8u, &read_value) &&
          (read_value.m_lo & ee::c_ee_scanout_clock::k_csr_vsint) != 0u &&
          (read_value.m_lo & ee::c_ee_scanout_clock::k_csr_field) != 0u,
          "only an explicitly supplied field boundary delivers VBlank");

    const ee::s_ee_u128 csr_ack{
        ee::c_ee_scanout_clock::k_csr_vsint, 0u
    };
    check(ee::c_ee_hw_bus::mmio_write(
              &bus, ee::c_ee_hw_bus::k_gs_csr, 4u, &csr_ack) &&
          ee::c_ee_hw_bus::mmio_read(
              &bus, ee::c_ee_hw_bus::k_gs_csr, 8u, &read_value) &&
          (read_value.m_lo & ee::c_ee_scanout_clock::k_csr_vsint) == 0u &&
          (read_value.m_lo & ee::c_ee_scanout_clock::k_csr_field) != 0u &&
          bus.intc().status() == 4u,
          "GS_CSR VSINT W1C never clears independent INTC_STAT");
    bus.reset();
    check(!bus.scanout().enabled() && bus.scanout().csr() == 0u &&
          bus.gs_imr() == 0u && bus.intc().status() == 0u,
          "hardware reset clears GS timing, CSR and interrupt state");

    // CRT configuration occurs at the actual EE cycle, not at an
    // implicit host-dispatch or boot-zero epoch once timing is available.
    ee::c_ee_hw_bus delayed_bus{};
    check(delayed_bus.set_gs_crt(1u, 2u, 0u, 1234567ull) &&
          delayed_bus.scanout().next_field_cycle() == 1234567ull + 4920115ull &&
          delayed_bus.scanout().fields_started() == 0u,
          "late CRT configuration uses the supplied EE time origin");
    check(delayed_bus.set_gs_crt(0u, 0x1au, 0u) &&
          !delayed_bus.scanout().enabled() &&
          delayed_bus.scanout().fields_started() == 0u,
          "unsupported CRT mode cannot retain the previous scanout schedule");

    ee::c_ee_scanout_clock overflow_guard{};
    const uint64_t origin =
        std::numeric_limits<uint64_t>::max() -
        2u * ee::c_ee_scanout_clock::k_ee_clock_hz;
    check(overflow_guard.configure(1u, 2u, 0u, origin) &&
          !overflow_guard.advance_to(
              std::numeric_limits<uint64_t>::max(),
              &intc, &flags, &memory) &&
          overflow_guard.fields_started() == 0u &&
          overflow_guard.last_ee_cycle() == origin,
          "overflowing advance is rejected without delivering a field");

    std::printf("ee_scanout_clock_test: %d failure(s)\n", g_failed);
    return g_failed ? 1 : 0;
}
