#include "iop_disc_loader.h"

#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace iop
{
namespace
{
char ascii_lower(char ch)
{
    return ch >= 'A' && ch <= 'Z'
        ? static_cast<char>(ch + ('a' - 'A')) : ch;
}

std::string lower(const std::string& value)
{
    std::string result;
    result.reserve(value.size());
    for (char ch : value)
    {
        result.push_back(ascii_lower(ch));
    }
    return result;
}

bool is_ascii_name(const std::string& value)
{
    if (value.empty() || value.size() > 128u ||
        value == "." || value == "..")
    {
        return false;
    }

    for (unsigned char ch : value)
    {
        if (ch < 0x21u || ch > 0x7eu ||
            ch == ':' || ch == '\\' || ch == '/' ||
            ch == '*' || ch == '?' || ch == '"' ||
            ch == '<' || ch == '>' || ch == '|')
        {
            return false;
        }
    }
    return true;
}

e_disc_load_result parse_guest_path(
    const std::string& guest,
    std::vector<std::string>* parts
)
{
    if (parts == nullptr)
    {
        return e_disc_load_result::invalid_guest_path;
    }

    if (guest.size() < 8u ||
        lower(guest.substr(0u, 7u)) != "cdrom0:")
    {
        return e_disc_load_result::unsupported_device;
    }

    std::string relative = guest.substr(7u);
    if (!relative.empty() &&
        (relative[0] == '\\' || relative[0] == '/'))
    {
        relative.erase(0u, 1u);
    }
    if (relative.empty() || relative.size() > 1024u)
    {
        return e_disc_load_result::invalid_guest_path;
    }

    for (char& ch : relative)
    {
        if (ch == '\\')
        {
            ch = '/';
        }
    }
    size_t start = 0u;
    while (start < relative.size())
    {
        const size_t slash = relative.find('/', start);
        const size_t end = slash == std::string::npos
            ? relative.size() : slash;
        std::string item = relative.substr(start, end - start);
        if (!is_ascii_name(item) || parts->size() >= 16u)
        {
            return e_disc_load_result::invalid_guest_path;
        }
        parts->push_back(std::move(item));
        if (slash == std::string::npos)
        {
            break;
        }
        start = slash + 1u;
        if (start == relative.size())
        {
            return e_disc_load_result::invalid_guest_path;
        }
    }

    std::string name = lower(parts->back());
    const size_t suffix = name.find(';');
    if (suffix != std::string::npos)
    {
        if (suffix + 1u == name.size())
        {
            return e_disc_load_result::invalid_guest_path;
        }
        for (size_t index = suffix + 1u; index < name.size(); ++index)
        {
            if (name[index] < '0' || name[index] > '9')
            {
                return e_disc_load_result::invalid_guest_path;
            }
        }
        name.erase(suffix);
    }
    if (name.size() <= 4u ||
        name.compare(name.size() - 4u, 4u, ".irx") != 0)
    {
        return e_disc_load_result::invalid_guest_path;
    }
    return e_disc_load_result::success;
}

e_disc_load_result resolve(
    const std::filesystem::path& root,
    const std::vector<std::string>& parts,
    std::filesystem::path* result
)
{
    std::error_code error;
    if (!std::filesystem::is_directory(root, error) || error)
    {
        return e_disc_load_result::io_error;
    }

    std::filesystem::path current = root;
    for (size_t index = 0u; index < parts.size(); ++index)
    {
        const std::string requested = lower(parts[index]);
        std::string unversioned = requested;
        if (index + 1u == parts.size())
        {
            const size_t semicolon = unversioned.find(';');
            if (semicolon != std::string::npos)
            {
                unversioned.erase(semicolon);
            }
        }

        uint32_t matches = 0u;
        std::filesystem::path found;
        for (std::filesystem::directory_iterator iterator(
                 current, error), end;
             !error && iterator != end; iterator.increment(error))
        {
            const std::string candidate = lower(
                iterator->path().filename().string()
            );
            if (candidate == requested ||
                (index + 1u == parts.size() &&
                 candidate == unversioned))
            {
                ++matches;
                found = iterator->path();
                if (matches > 1u)
                {
                    return e_disc_load_result::ambiguous_file;
                }
            }
        }
        if (error)
        {
            return e_disc_load_result::io_error;
        }
        if (matches == 0u)
        {
            return e_disc_load_result::missing_file;
        }

        // A mounted/extracted disc cannot escape its root through a
        // nested symlink, even if the symlink's target is otherwise valid.
        if (std::filesystem::is_symlink(
                std::filesystem::symlink_status(found, error)) || error)
        {
            return e_disc_load_result::invalid_guest_path;
        }
        if (index + 1u < parts.size() &&
            !std::filesystem::is_directory(found, error))
        {
            return e_disc_load_result::missing_file;
        }
        if (error)
        {
            return e_disc_load_result::io_error;
        }
        current = std::move(found);
    }

    if (!std::filesystem::is_regular_file(current, error) || error)
    {
        return e_disc_load_result::missing_file;
    }
    *result = current;
    return e_disc_load_result::success;
}
} // namespace

c_disc_loader::c_disc_loader(
    c_kernel& kernel, const std::filesystem::path& disc_root
)
    : m_kernel(kernel), m_disc_root(disc_root)
{
}

s_disc_load_result c_disc_loader::load_requested(
    const std::string& guest_path, bool start_entrypoint,
    uint32_t priority, const std::vector<std::string>& arguments
)
{
    s_disc_load_result output{};
    std::vector<std::string> parts;
    output.m_result = parse_guest_path(guest_path, &parts);
    if (output.m_result != e_disc_load_result::success)
    {
        return output;
    }

    output.m_result = resolve(
        m_disc_root, parts, &output.m_host_path
    );
    if (output.m_result != e_disc_load_result::success)
    {
        return output;
    }

    std::error_code error;
    const uint64_t size = std::filesystem::file_size(
        output.m_host_path, error
    );
    if (error)
    {
        output.m_result = e_disc_load_result::io_error;
        return output;
    }
    if (size < 52u || size > k_max_irx_bytes)
    {
        output.m_result = e_disc_load_result::invalid_file;
        return output;
    }

    std::ifstream stream(
        output.m_host_path, std::ios::binary
    );
    if (!stream)
    {
        output.m_result = e_disc_load_result::io_error;
        return output;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!stream.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size())))
    {
        output.m_result = e_disc_load_result::io_error;
        return output;
    }

    const uint16_t type = static_cast<uint16_t>(
        bytes[16] | (static_cast<uint32_t>(bytes[17]) << 8u)
    );
    if (bytes[0] != 0x7fu || bytes[1] != 'E' ||
        bytes[2] != 'L' || bytes[3] != 'F' ||
        bytes[4] != 1u || bytes[5] != 1u ||
        bytes[18] != 8u || bytes[19] != 0u ||
        (type != 0xff80u && type != 0xff81u))
    {
        output.m_result = e_disc_load_result::invalid_file;
        return output;
    }

    // The guest-provided path, not the ELF's string scan or catalog,
    // chooses the IRX. The existing kernel validates and links imports
    // to actual registered guest/HLE exports; missing providers fail.
    const s_module_load_result loaded = m_kernel.load_module(bytes, parts.back());
    output.m_link_status = loaded.m_status;
    output.m_link_diagnostic = loaded.m_link_diagnostic;
    if (loaded.m_status != e_irx_load_result::success)
    {
        output.m_result = e_disc_load_result::link_failed;
        return output;
    }

    output.m_module_id = loaded.m_id;
    if (start_entrypoint)
    {
        output.m_thread_id = m_kernel.create_module_thread(
            loaded.m_id, priority
        );
        if (output.m_thread_id == 0u ||
            !m_kernel.start_module_thread_with_arguments(
                output.m_thread_id, guest_path, arguments))
        {
            output.m_result = e_disc_load_result::thread_failed;
            return output;
        }
    }
    output.m_result = e_disc_load_result::success;
    return output;
}
} // namespace iop
