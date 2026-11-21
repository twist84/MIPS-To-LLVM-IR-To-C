#include "iop_guest_fault_probe.h"

#include <cctype>
#include <cstdint>
#include <limits>

namespace iop
{
namespace
{
bool is_guest_ram_word(uint32_t address)
{
    const uint32_t region = address & 0xe0000000u;
    const uint32_t physical = address & 0x1fffffffu;
    return (region == 0u || region == 0x80000000u ||
            region == 0xa0000000u) &&
        (address & 3u) == 0u &&
        physical < c_cpu::k_ram_mirror_size &&
        (physical & (c_cpu::k_ram_size - 1u)) <=
            c_cpu::k_ram_size - 4u;
}

void format_word_ascii(uint32_t opcode, char (&ascii)[5])
{
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        const unsigned char byte = static_cast<unsigned char>(
            opcode >> (8u * index));
        ascii[index] = (std::isalnum(byte) || byte == '_' ||
            byte == '-' || byte == ' ') ?
                static_cast<char>(byte) : '.';
    }
    ascii[4] = '\0';
}
} // namespace

bool c_iop_guest_fault_probe::capture(
    const c_kernel& kernel, const s_kernel_run_result& result)
{
    m_captured = false;
    if (result.m_stop == e_kernel_stop::budget_exhausted ||
        result.m_stop == e_kernel_stop::idle)
    {
        return false;
    }
    const s_kernel_thread* thread = kernel.thread(result.m_thread_id);
    if (thread == nullptr)
    {
        return false;
    }

    // Copy live guest context before any observational memory reads.
    // A read-only code-window fetch must never replace the original
    // memory-fault evidence from the guest's failing instruction.
    m_memory_fault = kernel.cpu().last_memory_fault();
    m_fault = result;
    m_thread = *thread;
    m_pc_module = matching_module(kernel, result.m_guest_pc);
    m_entry_module = matching_module(kernel, m_thread.m_entry);
    m_ra_module = matching_module(kernel, m_thread.m_cpu.m_gpr[31]);
    capture_window(kernel, result.m_guest_pc, m_pc_words);
    capture_window(kernel, m_thread.m_entry, m_entry_words);
    capture_window(kernel, m_thread.m_cpu.m_gpr[31], m_ra_words);
    capture_window(kernel, m_thread.m_cpu.m_gpr[29], m_sp_words);
    m_captured = true;
    return true;
}

bool c_iop_guest_fault_probe::captured() const
{
    return m_captured;
}

uint32_t c_iop_guest_fault_probe::thread_id() const
{
    return m_fault.m_thread_id;
}

uint32_t c_iop_guest_fault_probe::guest_pc() const
{
    return m_fault.m_guest_pc;
}

uint32_t c_iop_guest_fault_probe::entry() const
{
    return m_thread.m_entry;
}

uint32_t c_iop_guest_fault_probe::recent_count() const
{
    return m_thread.m_cpu.m_recent_count;
}

const std::array<s_iop_fault_word, c_iop_guest_fault_probe::k_window_words>&
c_iop_guest_fault_probe::pc_words() const
{
    return m_pc_words;
}

uint32_t c_iop_guest_fault_probe::matching_module(
    const c_kernel& kernel, uint32_t address)
{
    if (!is_guest_ram_word(address))
    {
        return 0u;
    }

    const uint32_t backing = (address & 0x1fffffffu) &
        (c_cpu::k_ram_size - 1u);
    for (const auto& entry : kernel.modules())
    {
        const s_irx_module& module = entry.second.m_irx;
        if (backing >= module.m_base &&
            backing - module.m_base < module.m_size)
        {
            return entry.first;
        }
    }
    return 0u;
}

void c_iop_guest_fault_probe::capture_window(
    const c_kernel& kernel, uint32_t center,
    std::array<s_iop_fault_word, k_window_words>& out)
{
    for (size_t index = 0u; index < out.size(); ++index)
    {
        s_iop_fault_word& word = out[index];
        word = {};
        const int64_t relative =
            (static_cast<int64_t>(index) - 3) * 4;
        const int64_t candidate =
            static_cast<int64_t>(center) + relative;
        if (candidate < 0 ||
            candidate > std::numeric_limits<uint32_t>::max())
        {
            continue;
        }
        word.m_address = static_cast<uint32_t>(candidate);
        if (is_guest_ram_word(word.m_address))
        {
            word.m_readable =
                kernel.cpu().read32(word.m_address, &word.m_opcode);
        }
    }
}

void c_iop_guest_fault_probe::write_words_text(
    FILE* output, const char* name,
    const std::array<s_iop_fault_word, k_window_words>& words)
{
    for (const s_iop_fault_word& word : words)
    {
        char ascii[5]{};
        format_word_ascii(word.m_opcode, ascii);
        std::fprintf(output,
            "IOP fault %s: address=0x%08x fetched=%u "
            "opcode=0x%08x ascii=%s\n",
            name, word.m_address, word.m_readable ? 1u : 0u,
            word.m_opcode, word.m_readable ? ascii : "....");
    }
}

void c_iop_guest_fault_probe::write_text(FILE* output) const
{
    if (output == nullptr || !m_captured)
    {
        return;
    }
    const s_cpu_state& cpu = m_thread.m_cpu;
    const s_guest_callback_origin& origin = m_thread.m_callback_origin;
    std::fprintf(output,
        "IOP fault callback registration: source=%u "
        "thread=%u syscall_pc=0x%08x ra=0x%08x gp=0x%08x "
        "a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x "
        "recent_count=%u\n",
        static_cast<uint32_t>(origin.m_source),
        origin.m_registration_thread_id, origin.m_registration_pc,
        origin.m_registration_ra, origin.m_registration_gp,
        origin.m_registration_a0, origin.m_registration_a1,
        origin.m_registration_a2, origin.m_registration_a3,
        origin.m_registration_recent_count);
    for (uint32_t index = 0u;
         index < origin.m_registration_recent_count; ++index)
    {
        std::fprintf(output,
            "IOP fault registration history: index=%u "
            "pc=0x%08x opcode=0x%08x\n",
            index, origin.m_registration_recent_pc[index],
            origin.m_registration_recent_opcode[index]);
    }
    std::fprintf(output,
        "IOP fault provenance: thread=%u stop=%u pc=0x%08x "
        "opcode=0x%08x entry=0x%08x status=%u priority=%u "
        "module=%u entry_module=%u pc_module=%u ra_module=%u "
        "creator_thread=%u creator_pc=0x%08x created_cycle=%llu "
        "alarm=%u callback=%u interrupt=%u interrupt_line=%u "
        "module_start=%u stack=0x%08x/0x%08x "
        "gp=0x%08x sp=0x%08x ra=0x%08x v0=0x%08x "
        "a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x "
        "next=0x%08x delay=%u branch=0x%08x "
        "insns=%llu slices=%llu self_restarts=%llu "
        "last_syscall=0x%08x last_syscall_pc=0x%08x "
        "last_syscall_ra=0x%08x "
        "memory_fault=%u/%u/0x%08x/%u\n",
        m_fault.m_thread_id, static_cast<uint32_t>(m_fault.m_stop),
        m_fault.m_guest_pc, m_fault.m_opcode,
        m_thread.m_entry, static_cast<uint32_t>(m_thread.m_status),
        m_thread.m_priority, m_thread.m_module_id, m_entry_module,
        m_pc_module, m_ra_module,
        m_thread.m_creator_thread_id, m_thread.m_creator_pc,
        static_cast<unsigned long long>(m_thread.m_created_cycle),
        m_thread.m_alarm_id, m_thread.m_hle_guest_callback ? 1u : 0u,
        m_thread.m_interrupt_callback ? 1u : 0u,
        m_thread.m_interrupt_line, m_thread.m_module_entry ? 1u : 0u,
        m_thread.m_stack_base, m_thread.m_stack_size,
        cpu.m_gpr[28], cpu.m_gpr[29], cpu.m_gpr[31], cpu.m_gpr[2],
        cpu.m_gpr[4], cpu.m_gpr[5], cpu.m_gpr[6], cpu.m_gpr[7],
        cpu.m_next_pc, cpu.m_in_delay_slot ? 1u : 0u, cpu.m_branch_pc,
        static_cast<unsigned long long>(m_thread.m_guest_instructions),
        static_cast<unsigned long long>(m_thread.m_guest_slices),
        static_cast<unsigned long long>(m_thread.m_self_restarts),
        m_thread.m_last_syscall_number,
        m_thread.m_last_syscall_pc, m_thread.m_last_syscall_ra,
        m_memory_fault.m_valid ? 1u : 0u, m_memory_fault.m_width,
        m_memory_fault.m_address, m_memory_fault.m_write ? 1u : 0u);
    const uint32_t count = cpu.m_recent_count;
    const uint32_t capacity = s_cpu_state::k_recent_pc_capacity;
    const uint32_t first = (cpu.m_recent_next + capacity - count) % capacity;
    for (uint32_t index = 0u; index < count; ++index)
    {
        const uint32_t slot = (first + index) % capacity;
        std::fprintf(output,
            "IOP fault execution history: index=%u pc=0x%08x opcode=0x%08x\n",
            index, cpu.m_recent_pc[slot], cpu.m_recent_opcode[slot]);
    }
    write_words_text(output, "pc", m_pc_words);
    write_words_text(output, "entry", m_entry_words);
    write_words_text(output, "ra", m_ra_words);
    write_words_text(output, "stack", m_sp_words);
}

void c_iop_guest_fault_probe::write_words_json(
    FILE* output,
    const std::array<s_iop_fault_word, k_window_words>& words)
{
    std::fputs("[", output);
    for (size_t index = 0u; index < words.size(); ++index)
    {
        const s_iop_fault_word& word = words[index];
        std::fprintf(output,
            "%s{\"address\": \"0x%08x\", \"fetched\": %s, "
            "\"word\": \"0x%08x\"}",
            index == 0u ? "" : ", ",
            word.m_address, word.m_readable ? "true" : "false",
            word.m_opcode);
    }
    std::fputs("]", output);
}

void c_iop_guest_fault_probe::write_json(FILE* output) const
{
    if (output == nullptr)
    {
        return;
    }
    if (!m_captured)
    {
        std::fputs("null", output);
        return;
    }
    const s_cpu_state& cpu = m_thread.m_cpu;
    const s_guest_callback_origin& origin = m_thread.m_callback_origin;
    std::fprintf(output,
        "{\n    \"schema\": \"iop-guest-fault-provenance-v1\","
        "\n    \"thread\": %u, \"stop_reason\": %u,"
        "\n    \"pc\": \"0x%08x\", \"opcode\": \"0x%08x\","
        "\n    \"entry\": \"0x%08x\","
        "\n    \"entry_module\": %u, \"pc_module\": %u,"
        "\n    \"ra_module\": %u, \"module_start_id\": %u,"
        "\n    \"creator_thread\": %u,"
        "\n    \"creator_resume_pc\": \"0x%08x\","
        "\n    \"created_cycle\": %llu,"
        "\n    \"alarm_id\": %u,"
        "\n    \"hle_guest_callback\": %s,"
        "\n    \"interrupt_callback\": %s,"
        "\n    \"interrupt_line\": %u,"
        "\n    \"priority\": %u, \"status\": %u,"
        "\n    \"stack_base\": \"0x%08x\","
        "\n    \"stack_size\": %u,"
        "\n    \"sp\": \"0x%08x\", \"ra\": \"0x%08x\","
        "\n    \"gp\": \"0x%08x\", \"next_pc\": \"0x%08x\","
        "\n    \"in_delay_slot\": %s, \"branch_pc\": \"0x%08x\","
        "\n    \"last_syscall_pc\": \"0x%08x\","
        "\n    \"last_syscall_ra\": \"0x%08x\","
        "\n    \"guest_instructions\": %llu,"
        "\n    \"guest_slices\": %llu,"
        "\n    \"memory_fault\": {\"valid\": %s, \"address\": \"0x%08x\","
        "\"width\": %u, \"write\": %s},"
        "\n    \"gpr\": [",
        m_fault.m_thread_id, static_cast<uint32_t>(m_fault.m_stop),
        m_fault.m_guest_pc, m_fault.m_opcode,
        m_thread.m_entry, m_entry_module, m_pc_module,
        m_ra_module, m_thread.m_module_id,
        m_thread.m_creator_thread_id, m_thread.m_creator_pc,
        static_cast<unsigned long long>(m_thread.m_created_cycle),
        m_thread.m_alarm_id,
        m_thread.m_hle_guest_callback ? "true" : "false",
        m_thread.m_interrupt_callback ? "true" : "false",
        m_thread.m_interrupt_line, m_thread.m_priority,
        static_cast<uint32_t>(m_thread.m_status),
        m_thread.m_stack_base, m_thread.m_stack_size,
        cpu.m_gpr[29], cpu.m_gpr[31], cpu.m_gpr[28],
        cpu.m_next_pc, cpu.m_in_delay_slot ? "true" : "false",
        cpu.m_branch_pc, m_thread.m_last_syscall_pc,
        m_thread.m_last_syscall_ra,
        static_cast<unsigned long long>(m_thread.m_guest_instructions),
        static_cast<unsigned long long>(m_thread.m_guest_slices),
        m_memory_fault.m_valid ? "true" : "false",
        m_memory_fault.m_address, m_memory_fault.m_width,
        m_memory_fault.m_write ? "true" : "false");
    for (uint32_t index = 0u; index < 32u; ++index)
    {
        std::fprintf(output, "%s\"0x%08x\"",
            index == 0u ? "" : ", ", cpu.m_gpr[index]);
    }
    std::fprintf(output,
        "],\n    \"callback_registration\": {"
        "\"source\": %u, \"thread\": %u,"
        "\"syscall_pc\": \"0x%08x\","
        "\"ra\": \"0x%08x\", \"gp\": \"0x%08x\","
        "\"a0\": \"0x%08x\", \"a1\": \"0x%08x\","
        "\"a2\": \"0x%08x\", \"a3\": \"0x%08x\","
        "\"recent_executed\": [",
        static_cast<uint32_t>(origin.m_source),
        origin.m_registration_thread_id,
        origin.m_registration_pc, origin.m_registration_ra,
        origin.m_registration_gp,
        origin.m_registration_a0, origin.m_registration_a1,
        origin.m_registration_a2, origin.m_registration_a3);
    for (uint32_t index = 0u;
         index < origin.m_registration_recent_count; ++index)
    {
        std::fprintf(output,
            "%s{\"pc\": \"0x%08x\", \"opcode\": \"0x%08x\"}",
            index != 0u ? ", " : "",
            origin.m_registration_recent_pc[index],
            origin.m_registration_recent_opcode[index]);
    }
    std::fputs("]},\n    \"recent_executed\": [", output);
    const uint32_t count = cpu.m_recent_count;
    const uint32_t capacity = s_cpu_state::k_recent_pc_capacity;
    const uint32_t first = (cpu.m_recent_next + capacity - count) % capacity;
    for (uint32_t index = 0u; index < count; ++index)
    {
        const uint32_t slot = (first + index) % capacity;
        std::fprintf(output,
            "%s{\"pc\": \"0x%08x\", \"opcode\": \"0x%08x\"}",
            index == 0u ? "" : ", ",
            cpu.m_recent_pc[slot], cpu.m_recent_opcode[slot]);
    }
    std::fputs("],\n    \"pc_window\": ", output);
    write_words_json(output, m_pc_words);
    std::fputs(",\n    \"entry_window\": ", output);
    write_words_json(output, m_entry_words);
    std::fputs(",\n    \"ra_window\": ", output);
    write_words_json(output, m_ra_words);
    std::fputs(",\n    \"stack_window\": ", output);
    write_words_json(output, m_sp_words);
    std::fputs(
        ",\n    \"limitations\": ["
        "\"host_only_read_only_context\","
        "\"not_a_guest_exception_handler\","
        "\"module_membership_does_not_prove_valid_executable_code\","
        "\"creator_resume_pc_is_not_exact_syscall_pc\"]\n  }",
        output);
}
} // namespace iop
