#include "iop_guest_fault_probe.h"

#include <cstdio>
#include <string>

namespace
{
bool contains(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

int fail(const char* reason)
{
    std::fprintf(stderr, "IOP guest fault provenance: %s\n", reason);
    return 1;
}

std::string read_file(FILE* file)
{
    if (file == nullptr)
    {
        return {};
    }
    std::fflush(file);
    std::rewind(file);
    char buffer[16384]{};
    const size_t length = std::fread(buffer, 1u, sizeof(buffer) - 1u, file);
    return std::string(buffer, length);
}
} // namespace

int main()
{
    iop::c_kernel kernel{};
    iop::c_iop_guest_fault_probe probe{};

    iop::s_kernel_run_result idle{};
    idle.m_stop = iop::e_kernel_stop::idle;
    if (probe.capture(kernel, idle) || probe.captured())
    {
        return fail("idle was incorrectly captured as a fault");
    }

    // The exact observed 0x6e736168 invalid instruction is deliberately
    // executed as guest R3000 code. No host callback is allowed to advance
    // the thread beyond it or reinterpret the ASCII bytes as an opcode.
    const uint32_t entry = 0x00014000u;
    if (!kernel.cpu().write32(entry, 0x24020001u) || // addiu v0, zero, 1
        !kernel.cpu().write32(entry + 4u, 0x24420002u) || // addiu v0, v0, 2
        !kernel.cpu().write32(entry + 8u, 0x6e736168u))
    {
        return fail("could not install guest instruction fixture");
    }
    const uint32_t thread_id =
        kernel.create_thread(entry, 0x00015000u, 44u, 0x400u);
    if (thread_id == 0u || !kernel.start_thread(thread_id))
    {
        return fail("could not start original guest thread");
    }
    const iop::s_kernel_run_result stopped = kernel.run(32u, 32u);
    if (stopped.m_stop != iop::e_kernel_stop::unsupported_instruction ||
        stopped.m_thread_id != thread_id ||
        stopped.m_guest_pc != entry + 8u ||
        stopped.m_opcode != 0x6e736168u)
    {
        return fail("unsupported guest instruction was not fail-closed");
    }
    const iop::s_kernel_thread* thread = kernel.thread(thread_id);
    if (thread == nullptr || thread->m_cpu.m_gpr[2] != 3u ||
        thread->m_cpu.m_recent_count != 3u ||
        thread->m_cpu.m_recent_pc[0] != entry ||
        thread->m_cpu.m_recent_pc[1] != entry + 4u ||
        thread->m_cpu.m_recent_pc[2] != entry + 8u ||
        thread->m_cpu.m_recent_opcode[2] != 0x6e736168u)
    {
        return fail("saved-thread execution history or guest GPR corrupted");
    }
    if (!probe.capture(kernel, stopped) || !probe.captured() ||
        probe.thread_id() != thread_id ||
        probe.guest_pc() != entry + 8u ||
        probe.entry() != entry ||
        probe.recent_count() != 3u ||
        !probe.pc_words()[3].m_readable ||
        probe.pc_words()[3].m_opcode != 0x6e736168u)
    {
        return fail("stopped-thread snapshot missing exact evidence");
    }

    FILE* file = std::tmpfile();
    if (file == nullptr)
    {
        return fail("could not create temporary JSON diagnostic output");
    }
    probe.write_json(file);
    const std::string json = read_file(file);
    std::fclose(file);
    if (!contains(json, "\"schema\": \"iop-guest-fault-provenance-v1\"") ||
        !contains(json, "\"pc\": \"0x00014008\"") ||
        !contains(json, "\"opcode\": \"0x6e736168\"") ||
        !contains(json, "\"recent_executed\": [") ||
        !contains(json, "\"entry_module\": 0") ||
        !contains(json, "\"pc_module\": 0") ||
        !contains(json, "\"creator_thread\": 0") ||
        !contains(json, "\"memory_fault\": {\"valid\": false"))
    {
        return fail("JSON omitted required thread, PC or source evidence");
    }

    // Snapshot contents must not depend on mutable guest memory after
    // capture. The original R3000 halt remains the definitive result.
    if (!kernel.cpu().write32(entry + 8u, 0u) ||
        !probe.pc_words()[3].m_readable ||
        probe.pc_words()[3].m_opcode != 0x6e736168u ||
        kernel.thread(thread_id)->m_status !=
            iop::e_thread_status::faulted)
    {
        return fail("diagnostic capture mutated guest execution");
    }

    file = std::tmpfile();
    if (file == nullptr)
    {
        return fail("could not create temporary text diagnostic output");
    }
    probe.write_text(file);
    const std::string details = read_file(file);
    std::fclose(file);
    if (!contains(details, "pc=0x00014008") ||
        !contains(details, "opcode=0x6e736168") ||
        !contains(details, "IOP fault execution history: index=2") ||
        !contains(details, "ascii=hasn"))
    {
        return fail("text diagnostic omitted invalid-code provenance");
    }

    // A fresh thread must not inherit the faulting thread's PC history.
    const uint32_t next_id =
        kernel.create_thread(0x00016000u, 0u, 45u, 0x400u);
    const iop::s_kernel_thread* next = kernel.thread(next_id);
    if (next == nullptr || next->m_cpu.m_recent_count != 0u)
    {
        return fail("new guest thread inherited execution history");
    }

    // Deferred callbacks must retain their original guest registration
    // source, ABI argument values, RA and preceding R3000 instructions.
    iop::s_guest_callback_origin origin{};
    origin.m_source = iop::e_guest_callback_source::vblank_end;
    origin.m_registration_thread_id = 52u;
    origin.m_registration_pc = 0x001cab00u;
    origin.m_registration_ra = 0x001cab18u;
    origin.m_registration_gp = 0x001d5ff0u;
    origin.m_registration_a0 = 1u;
    origin.m_registration_a1 = 46u;
    origin.m_registration_a2 = 0x001cdf70u;
    origin.m_registration_a3 = 1u;
    origin.m_registration_recent_count = 2u;
    origin.m_registration_recent_pc[0] = 0x001caafcu;
    origin.m_registration_recent_opcode[0] = 0x03e00008u;
    origin.m_registration_recent_pc[1] = 0x001cab00u;
    origin.m_registration_recent_opcode[1] = 0x0000000cu;
    if (!kernel.cpu().write32(0x00017000u, 0x6e736168u) ||
        !kernel.start_guest_callback(
            0x00017000u, 0x001d5ff0u, 1u, origin))
    {
        return fail("could not schedule guest callback");
    }
    const iop::s_kernel_thread& callback_thread =
        kernel.threads().rbegin()->second;
    if (!callback_thread.m_hle_guest_callback ||
        callback_thread.m_callback_origin.m_source !=
            iop::e_guest_callback_source::vblank_end ||
        callback_thread.m_callback_origin.m_registration_thread_id != 52u ||
        callback_thread.m_callback_origin.m_registration_recent_count != 2u ||
        callback_thread.m_callback_origin.m_registration_recent_pc[1] !=
            0x001cab00u ||
        callback_thread.m_callback_origin.m_registration_a2 !=
            0x001cdf70u ||
        callback_thread.m_cpu.m_gpr[4] != 1u)
    {
        return fail("deferred guest callback registration provenance lost");
    }

    // The actual deferred callback may still fault. The snapshot must
    // include its original registration provenance and not rewrite the
    // unsupported R3000 instruction into an emulated success.
    const iop::s_kernel_run_result callback_stop = kernel.run(8u, 8u);
    if (callback_stop.m_stop !=
            iop::e_kernel_stop::unsupported_instruction ||
        callback_stop.m_guest_pc != 0x00017000u ||
        callback_stop.m_opcode != 0x6e736168u ||
        !probe.capture(kernel, callback_stop))
    {
        return fail("bad deferred callback did not fail closed");
    }
    FILE* origin_file = std::tmpfile();
    if (origin_file == nullptr)
    {
        return fail("could not create callback provenance JSON output");
    }
    probe.write_json(origin_file);
    const std::string origin_json = read_file(origin_file);
    std::fclose(origin_file);
    if (!contains(origin_json, "\"callback_registration\": {") ||
        !contains(origin_json, "\"source\": 2") ||
        !contains(origin_json, "\"thread\": 52") ||
        !contains(origin_json, "\"a2\": \"0x001cdf70\"") ||
        !contains(origin_json, "\"pc\": \"0x001cab00\""))
    {
        return fail("failing guest callback lost original registration");
    }

    std::puts("IOP guest stopped-thread provenance: PASS");
    return 0;
}
