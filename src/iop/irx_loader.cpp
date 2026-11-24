#include "irx_loader.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <set>
#include <utility>

namespace iop
{
namespace
{
constexpr uint32_t k_elf_header_size = 52u;
constexpr uint32_t k_program_header_size = 32u;
constexpr uint32_t k_section_header_size = 40u;
constexpr uint32_t k_program_load = 1u;
constexpr uint32_t k_program_iopmod = 0x70000080u;
constexpr uint32_t k_section_rel = 9u;
constexpr uint32_t k_elf_ioprel = 0xff80u;
constexpr uint32_t k_elf_ioprel_v2 = 0xff81u;
constexpr uint32_t k_import_marker = 0x41e00000u;
constexpr uint32_t k_export_marker = 0x41c00000u;
constexpr uint32_t k_jr_ra = 0x03e00008u;
constexpr uint32_t k_import_ordinal = 0x24000000u;
constexpr uint32_t k_user_ram_start = 0x00020000u;

bool valid_range(size_t size, uint64_t offset, uint64_t count)
{
    return offset <= size && count <= size - offset;
}

uint32_t read32(const std::vector<uint8_t>& bytes, size_t offset)
{
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
           (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
           (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
}

uint16_t read16(const std::vector<uint8_t>& bytes, size_t offset)
{
    return static_cast<uint16_t>(
        bytes[offset] | (static_cast<uint32_t>(bytes[offset + 1u]) << 8u)
    );
}

void write32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value)
{
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1u] = static_cast<uint8_t>(value >> 8u);
    bytes[offset + 2u] = static_cast<uint8_t>(value >> 16u);
    bytes[offset + 3u] = static_cast<uint8_t>(value >> 24u);
}

bool valid_word(const std::vector<uint8_t>& bytes, uint64_t offset)
{
    return (offset & 3u) == 0u && valid_range(bytes.size(), offset, 4u);
}

struct s_hi_relocation
{
    uint32_t m_offset;
    uint32_t m_instruction;
};

// Relocation offsets are signed relative to the start of the loaded
// segment. The PS2 IOP relocatable ELF uses REL (in-place addends).
e_irx_load_result relocate(
    const std::vector<uint8_t>& elf,
    std::vector<uint8_t>& staged,
    uint32_t base,
    bool version_two
)
{
    const uint32_t shoff = read32(elf, 32u);
    const uint32_t shnum = read16(elf, 48u);
    const uint32_t shentsize = read16(elf, 46u);
    if (shnum == 0u)
    {
        return e_irx_load_result::success;
    }
    if (shentsize < k_section_header_size ||
        !valid_range(elf.size(), shoff,
                     static_cast<uint64_t>(shentsize) * shnum))
    {
        return e_irx_load_result::invalid_bounds;
    }

    for (uint32_t section = 0u; section < shnum; ++section)
    {
        const size_t header =
            static_cast<size_t>(shoff) + section * shentsize;
        if (read32(elf, header + 4u) != k_section_rel)
        {
            continue;
        }

        const uint32_t offset = read32(elf, header + 16u);
        const uint32_t size = read32(elf, header + 20u);
        if ((size & 7u) != 0u ||
            !valid_range(elf.size(), offset, size))
        {
            return e_irx_load_result::invalid_bounds;
        }

        std::vector<s_hi_relocation> pending_hi;
        const uint32_t count = size / 8u;
        for (uint32_t index = 0u; index < count; ++index)
        {
            const size_t item =
                static_cast<size_t>(offset) + index * 8u;
            const int32_t address =
                static_cast<int32_t>(read32(elf, item));
            const uint32_t type = read32(elf, item + 4u) & 0xffu;
            if (address < 0 ||
                !valid_word(staged, static_cast<uint32_t>(address)))
            {
                return e_irx_load_result::invalid_bounds;
            }

            const size_t at = static_cast<size_t>(address);
            const uint32_t instruction = read32(staged, at);
            switch (type)
            {
            case 0u: // R_MIPS_NONE
                break;
            case 2u: // R_MIPS_32
                write32(staged, at, instruction + base);
                break;
            case 4u: // R_MIPS_26
                write32(
                    staged, at,
                    (instruction & 0xfc000000u) |
                    (((instruction & 0x03ffffffu) +
                      (base >> 2u)) & 0x03ffffffu)
                );
                break;
            case 5u: // R_MIPS_HI16
                if (version_two)
                {
                    if (index + 1u >= count)
                    {
                        return e_irx_load_result::unsupported_relocation;
                    }
                    const size_t next =
                        static_cast<size_t>(offset) + (index + 1u) * 8u;
                    const int32_t lo_offset =
                        static_cast<int32_t>(read32(elf, next));
                    if ((read32(elf, next + 4u) & 0xffu) != 6u ||
                        lo_offset < 0 ||
                        !valid_word(staged, static_cast<uint32_t>(lo_offset)))
                    {
                        return e_irx_load_result::unsupported_relocation;
                    }

                    const uint32_t lo = read32(
                        staged, static_cast<uint32_t>(lo_offset)
                    );
                    const uint32_t absolute =
                        ((instruction & 0xffffu) << 16u) +
                        static_cast<uint32_t>(
                            static_cast<int32_t>(
                                static_cast<int16_t>(lo & 0xffffu)
                            )
                        ) + base;
                    write32(
                        staged, at, (instruction & 0xffff0000u) |
                        (((absolute + 0x8000u) >> 16u) & 0xffffu)
                    );
                }
                else
                {
                    pending_hi.push_back({
                        static_cast<uint32_t>(at), instruction
                    });
                }
                break;
            case 6u: // R_MIPS_LO16
            {
                const uint32_t signed_low = static_cast<uint32_t>(
                    static_cast<int32_t>(
                        static_cast<int16_t>(instruction & 0xffffu)
                    )
                );
                uint32_t relocated = signed_low + base;
                if (!version_two && !pending_hi.empty())
                {
                    // One LO16 may pair with several HI16 fixups.
                    // Relocate every pending high using its own addend.
                    for (const s_hi_relocation& high : pending_hi)
                    {
                        const uint32_t absolute =
                            ((high.m_instruction & 0xffffu) << 16u) +
                            signed_low + base;
                        const uint32_t patched =
                            (high.m_instruction & 0xffff0000u) |
                            (((absolute + 0x8000u) >> 16u) & 0xffffu);
                        write32(staged, high.m_offset, patched);
                        relocated = absolute;
                    }
                    pending_hi.clear();
                }
                write32(
                    staged, at, (instruction & 0xffff0000u) |
                    (relocated & 0xffffu)
                );
                break;
            }
            default:
                return e_irx_load_result::unsupported_relocation;
            }
        }

        if (!pending_hi.empty())
        {
            return e_irx_load_result::unsupported_relocation;
        }
    }

    return e_irx_load_result::success;
}

e_irx_load_result resolve_imports(
    std::vector<uint8_t>& staged,
    uint32_t base,
    const t_irx_import_resolver& resolver,
    s_irx_link_diagnostic* diagnostic
)
{
    for (size_t offset = 0u; offset + 20u <= staged.size();
         offset += 4u)
    {
        if (read32(staged, offset) != k_import_marker)
        {
            continue;
        }

        char library_name[9]{};
        std::memcpy(library_name, staged.data() + offset + 12u, 8u);
        const size_t library_length = std::strlen(library_name);
        if (library_length == 0u)
        {
            return e_irx_load_result::invalid_module;
        }
        const std::string library(library_name, library_length);

        for (size_t stub = offset + 20u;
             stub + 8u <= staged.size(); stub += 8u)
        {
            const uint32_t first = read32(staged, stub);
            const uint32_t second = read32(staged, stub + 4u);
            if (first != k_jr_ra ||
                (second & 0xffff0000u) != k_import_ordinal)
            {
                break;
            }

            const uint16_t ordinal =
                static_cast<uint16_t>(second & 0xffffu);
            uint32_t target = 0u;
            if (!resolver || !resolver(library, ordinal, &target))
            {
                if (diagnostic != nullptr)
                {
                    diagnostic->m_unresolved_library = library;
                    diagnostic->m_unresolved_ordinal = ordinal;
                    diagnostic->m_result =
                        e_irx_load_result::unresolved_import;
                }
                return e_irx_load_result::unresolved_import;
            }

            // MIPS J inherits high address bits from PC + 4.
            // Do not silently encode an unreachable import destination.
            const uint32_t guest_stub = base +
                static_cast<uint32_t>(stub);
            if ((target & 3u) != 0u ||
                ((target ^ (guest_stub + 4u)) & 0xf0000000u) != 0u ||
                (target & 0x1fffffffu) >= c_cpu::k_ram_size)
            {
                return e_irx_load_result::invalid_import_target;
            }
            write32(
                staged, stub,
                0x08000000u | ((target >> 2u) & 0x03ffffffu)
            );
            write32(staged, stub + 4u, 0u);
        }
    }
    return e_irx_load_result::success;
}
// Sony IOP exports: a 20-byte 0x41c00000 header followed by a
// zero-terminated table of relocated function pointers. Word N is
// the callable entry for ordinal N.
e_irx_load_result collect_exports(
    const std::vector<uint8_t>& staged,
    uint32_t base,
    std::vector<s_irx_export>* exports
)
{
    std::set<std::pair<std::string, uint16_t>> seen;
    for (size_t offset = 0u; offset + 20u <= staged.size();
         offset += 4u)
    {
        if (read32(staged, offset) != k_export_marker)
        {
            continue;
        }
        std::string library;
        for (size_t index = 0u; index < 8u; ++index)
        {
            const uint8_t ch = staged[offset + 12u + index];
            if (ch == 0u)
            {
                break;
            }
            if (ch < 0x21u || ch > 0x7eu)
            {
                return e_irx_load_result::invalid_export;
            }
            library.push_back(static_cast<char>(ch));
        }
        if (library.empty())
        {
            return e_irx_load_result::invalid_export;
        }
        bool terminated = false;
        for (uint32_t ordinal = 0u; ordinal < 1024u; ++ordinal)
        {
            const size_t at = offset + 20u +
                static_cast<size_t>(ordinal) * 4u;
            if (!valid_word(staged, at))
            {
                break;
            }
            const uint32_t entry = read32(staged, at);
            if (entry == 0u)
            {
                // Retail SDBZ exports have holes in the ordinal table:
                // sio2man has unused #4/#27 but valid #5..#63.
                // A zero is a hole only when the next word is another
                // in-image callable pointer. Otherwise the table ends.
                const size_t following = at + 4u;
                uint32_t next = 0u;
                if (valid_word(staged, following))
                {
                    next = read32(staged, following);
                }
                const uint32_t physical_next = next & 0x1fffffffu;
                if (next == 0u || (next & 3u) != 0u ||
                    physical_next < base ||
                    static_cast<uint64_t>(physical_next) >=
                        static_cast<uint64_t>(base) + staged.size())
                {
                    terminated = true;
                    break;
                }
                continue;
            }
            const uint32_t physical = entry & 0x1fffffffu;
            if ((entry & 3u) != 0u ||
                physical < base ||
                static_cast<uint64_t>(physical) >=
                    static_cast<uint64_t>(base) + staged.size() ||
                !seen.emplace(library, static_cast<uint16_t>(ordinal)).second)
            {
                return e_irx_load_result::invalid_export;
            }
            exports->push_back({
                library, static_cast<uint16_t>(ordinal), entry
            });
        }
        if (!terminated)
        {
            return e_irx_load_result::invalid_export;
        }
    }
    return e_irx_load_result::success;
}

} // namespace

e_irx_load_result load_irx(
    c_cpu& cpu,
    const std::vector<uint8_t>& bytes,
    uint32_t base,
    s_irx_module* module,
    const t_irx_import_resolver& resolver,
    const t_irx_export_validator& validate_exports,
    s_irx_link_diagnostic* diagnostic
)
{
    if (diagnostic != nullptr)
    {
        *diagnostic = {};
    }
    if (module == nullptr)
    {
        return e_irx_load_result::invalid_module;
    }
    if (bytes.size() < k_elf_header_size ||
        bytes[0] != 0x7fu || bytes[1] != 'E' ||
        bytes[2] != 'L' || bytes[3] != 'F' ||
        bytes[4] != 1u || bytes[5] != 1u ||
        read16(bytes, 18u) != 8u ||
        (read16(bytes, 16u) != k_elf_ioprel &&
         read16(bytes, 16u) != k_elf_ioprel_v2))
    {
        return e_irx_load_result::invalid_elf;
    }

    const uint32_t program_offset = read32(bytes, 28u);
    const uint32_t program_count = read16(bytes, 44u);
    const uint32_t program_size = read16(bytes, 42u);
    if (program_size < k_program_header_size ||
        program_count == 0u ||
        !valid_range(bytes.size(), program_offset,
                     static_cast<uint64_t>(program_count) *
                     program_size))
    {
        return e_irx_load_result::invalid_bounds;
    }

    uint32_t load_offset = 0u;
    uint32_t load_size = 0u;
    uint32_t mem_size = 0u;
    uint32_t iopmod_offset = 0u;
    uint32_t load_count = 0u;
    bool found_iopmod = false;

    for (uint32_t index = 0u; index < program_count; ++index)
    {
        const size_t header =
            static_cast<size_t>(program_offset) + index * program_size;
        const uint32_t kind = read32(bytes, header);
        if (kind == k_program_iopmod)
        {
            if (found_iopmod)
            {
                return e_irx_load_result::invalid_module;
            }
            found_iopmod = true;
            iopmod_offset = read32(bytes, header + 4u);
        }
        else if (kind == k_program_load)
        {
            ++load_count;
            load_offset = read32(bytes, header + 4u);
            load_size = read32(bytes, header + 16u);
            mem_size = read32(bytes, header + 20u);
        }
    }

    if (!found_iopmod || load_count != 1u ||
        !valid_range(bytes.size(), iopmod_offset, 26u) ||
        !valid_range(bytes.size(), load_offset, load_size) ||
        load_size == 0u || mem_size < load_size)
    {
        return e_irx_load_result::invalid_module;
    }

    const uint32_t entry_offset = read32(bytes, iopmod_offset + 4u);
    const uint32_t gp_offset = read32(bytes, iopmod_offset + 8u);
    const uint64_t segment_total =
        static_cast<uint64_t>(read32(bytes, iopmod_offset + 12u)) +
        read32(bytes, iopmod_offset + 16u) +
        read32(bytes, iopmod_offset + 20u);
    const uint64_t total = std::max<uint64_t>(
        mem_size, segment_total
    );
    const uint64_t aligned = (total + 15u) & ~15ull;
    if ((base & 15u) != 0u ||
        base < k_user_ram_start ||
        aligned == 0u ||
        static_cast<uint64_t>(base) + aligned > c_cpu::k_ram_size)
    {
        return e_irx_load_result::insufficient_ram;
    }
    // Sony IOP IRX GP is an anchor for signed small-data offsets,
    // not necessarily an address inside PT_LOAD. Retail SDBZ:
    // SIO2MAN GP=0x8FB0, image=0x0FF0; PADMAN GP=0x10030,
    // image=0x9C20. Check the translated address is addressable.
    if (diagnostic != nullptr)
    {
        diagnostic->m_entry_offset = entry_offset;
        diagnostic->m_gp_offset = gp_offset;
        diagnostic->m_image_size = static_cast<uint32_t>(aligned);
    }
    if (entry_offset >= total || (entry_offset & 3u) != 0u ||
        static_cast<uint64_t>(base) + gp_offset >= c_cpu::k_ram_size)
    {
        return e_irx_load_result::invalid_module;
    }

    std::vector<uint8_t> staged(static_cast<size_t>(aligned), 0u);
    std::copy(
        bytes.begin() + load_offset,
        bytes.begin() + load_offset + load_size,
        staged.begin()
    );

    e_irx_load_result result = relocate(
        bytes, staged, base,
        read16(bytes, 16u) == k_elf_ioprel_v2
    );
    if (result != e_irx_load_result::success)
    {
        return result;
    }

    result = resolve_imports(staged, base, resolver, diagnostic);
    if (result != e_irx_load_result::success)
    {
        return result;
    }

    std::vector<s_irx_export> exports;
    result = collect_exports(staged, base, &exports);
    if (result != e_irx_load_result::success)
    {
        return result;
    }
    if (validate_exports && !validate_exports(exports))
    {
        return e_irx_load_result::duplicate_export;
    }

    // All memory/range checks have succeeded. No writes occur before
    // this point; malformed IRX bytes never partially modify IOP RAM.
    for (size_t offset = 0u; offset < staged.size(); offset += 4u)
    {
        const bool written = cpu.write32(
            base + static_cast<uint32_t>(offset),
            read32(staged, offset)
        );
        if (!written)
        {
            // A checked in-bounds aligned RAM write cannot fail.
            return e_irx_load_result::insufficient_ram;
        }
    }

    std::string name;
    for (size_t offset = static_cast<size_t>(iopmod_offset) + 26u;
         offset < bytes.size() && offset <
         static_cast<size_t>(iopmod_offset) + 82u &&
         bytes[offset] != 0u; ++offset)
    {
        name.push_back(static_cast<char>(bytes[offset]));
    }
    if (name.empty())
    {
        name = "IRX";
    }
    module->m_name = name;
    module->m_base = base;
    module->m_size = static_cast<uint32_t>(aligned);
    module->m_text_size = std::min<uint32_t>(
        read32(bytes, iopmod_offset + 12u), static_cast<uint32_t>(aligned));
    module->m_entry = base + entry_offset;
    module->m_gp = base + gp_offset;
    module->m_exports = std::move(exports);
    if (diagnostic != nullptr)
    {
        diagnostic->m_result = e_irx_load_result::success;
    }
    return e_irx_load_result::success;
}
} // namespace iop
