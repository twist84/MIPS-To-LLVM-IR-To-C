#include "iop_folder_sector_source.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <queue>
#include <set>
#include <system_error>

namespace iop
{
namespace
{
constexpr uint32_t k_sector_size = c_iso_sector_source::k_sector_size;

uint64_t sectors(uint64_t size)
{
    return (size + k_sector_size - 1u) / k_sector_size;
}

uint32_t align_sector(uint32_t value)
{
    return static_cast<uint32_t>(sectors(value) * k_sector_size);
}

// ISO9660 validation and guest path matching operate on UTF-8 bytes,
// not the host's locale-dependent narrow filesystem encoding.
std::string path_utf8_bytes(const std::filesystem::path& path)
{
    const std::u8string utf8 = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

std::string ascii_upper(std::string value)
{
    for (char& ch : value)
    {
        if (ch >= 'a' && ch <= 'z')
        {
            ch = static_cast<char>(ch - ('a' - 'A'));
        }
    }
    return value;
}

bool valid_name(const std::string& name)
{
    if (name.empty() || name == "." || name == ".." ||
        name.size() > 200u)
    {
        return false;
    }
    for (unsigned char ch : name)
    {
        if (ch < 0x21u || ch > 0x7eu ||
            ch == ';' || ch == '/' || ch == '\\' || ch == ':')
        {
            return false;
        }
    }
    return true;
}

void put16(uint8_t* p, uint16_t value, bool big = false)
{
    p[big ? 1u : 0u] = static_cast<uint8_t>(value);
    p[big ? 0u : 1u] = static_cast<uint8_t>(value >> 8u);
}

void put32(uint8_t* p, uint32_t value, bool big = false)
{
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        p[big ? 3u - i : i] = static_cast<uint8_t>(value >> (i * 8u));
    }
}

void both16(uint8_t* p, uint16_t value)
{
    put16(p, value);
    put16(p + 2u, value, true);
}

void both32(uint8_t* p, uint32_t value)
{
    put32(p, value);
    put32(p + 4u, value, true);
}
} // namespace

c_folder_sector_source::c_folder_sector_source(const std::filesystem::path& root,
    const std::filesystem::path& mounted_card)
{
    m_valid = build(root, mounted_card);
}

bool c_folder_sector_source::valid() const
{
    return m_valid;
}

const std::string& c_folder_sector_source::error() const
{
    return m_error;
}

bool c_folder_sector_source::fail(const std::string& reason)
{
    m_error = reason;
    m_regions.clear();
    m_nodes.clear();
    m_sector_count = 0u;
    return false;
}

void c_folder_sector_source::add_memory(
    uint32_t lba, std::vector<uint8_t> bytes
)
{
    s_region region{};
    region.m_count = static_cast<uint32_t>(sectors(bytes.size()));
    region.m_size = static_cast<uint32_t>(bytes.size());
    region.m_memory = std::move(bytes);
    m_regions.emplace(lba, std::move(region));
}

std::vector<uint8_t> c_folder_sector_source::make_record(
    const s_node& node, const std::string& name
) const
{
    const size_t length = 33u + name.size() +
        ((name.size() & 1u) == 0u ? 1u : 0u);
    std::vector<uint8_t> bytes(length, 0u);
    bytes[0] = static_cast<uint8_t>(length);
    both32(bytes.data() + 2u, node.m_lba);
    both32(bytes.data() + 10u, node.m_size);
    bytes[18] = 126u;
    bytes[19] = 1u;
    bytes[20] = 1u;
    bytes[25] = node.m_directory ? 2u : 0u;
    both16(bytes.data() + 28u, 1u);
    bytes[32] = static_cast<uint8_t>(name.size());
    std::memcpy(bytes.data() + 33u, name.data(), name.size());
    return bytes;
}

std::vector<uint8_t> c_folder_sector_source::make_directory(
    const std::string& key
) const
{
    std::vector<uint8_t> bytes;
    const auto append = [&](const s_node& node, const std::string& name)
    {
        std::vector<uint8_t> entry = make_record(node, name);
        if ((bytes.size() % k_sector_size) + entry.size() > k_sector_size)
        {
            bytes.resize(static_cast<size_t>(sectors(bytes.size())) *
                         k_sector_size, 0u);
        }
        bytes.insert(bytes.end(), entry.begin(), entry.end());
    };
    const s_node& current = m_nodes.at(key);
    append(current, std::string(1u, '\0'));
    append(m_nodes.at(current.m_parent), std::string(1u, '\1'));
    for (const auto& item : m_nodes)
    {
        if (!item.first.empty() && item.second.m_parent == key)
        {
            append(item.second, item.second.m_name +
                (item.second.m_directory ? "" : ";1"));
        }
    }
    bytes.resize(static_cast<size_t>(sectors(bytes.size())) *
                 k_sector_size, 0u);
    return bytes;
}

bool c_folder_sector_source::build(const std::filesystem::path& root,
    const std::filesystem::path& mounted_card)
{
    namespace fs = std::filesystem;
    std::error_code error;
    if (!fs::is_directory(root, error) || error ||
        fs::is_symlink(fs::symlink_status(root, error)) || error)
    {
        return fail("disc root must be a real directory");
    }

    const std::string card_name = ascii_upper(path_utf8_bytes(mounted_card.filename()));
    m_nodes.emplace("", s_node{root, "", "", 0u, 0u, true});
    fs::recursive_directory_iterator it(root, error), end;
    if (error)
    {
        return fail("cannot enumerate disc root");
    }
    for (; it != end; it.increment(error))
    {
        if (error)
        {
            return fail("cannot enumerate extracted disc");
        }
        const fs::path relative = it->path().lexically_relative(root);
        if (relative.empty())
        {
            return fail("file outside extracted disc");
        }
        const std::string top = ascii_upper(
            path_utf8_bytes(*relative.begin())
        );
        if (top == "GAME" || top == "NATIVE" || top == ".PS2-DISC")
        {
            it.disable_recursion_pending();
            continue;
        }
        const fs::file_status status = it->symlink_status(error);
        if (error || fs::is_symlink(status))
        {
            return fail("symbolic link in extracted disc");
        }
        if (!fs::is_regular_file(status) && !fs::is_directory(status))
        {
            return fail("unsupported entry in extracted disc");
        }
        const std::string name = path_utf8_bytes(relative.filename());
        // The mounted card is host storage even when it sits beside the disc files.
        if (!card_name.empty() && fs::is_regular_file(status) &&
            ascii_upper(name) == card_name)
        {
            std::error_code identity_error;
            if (fs::equivalent(it->path(), mounted_card, identity_error) && !identity_error)
            {
                continue;
            }
        }
        if (!valid_name(name))
        {
            return fail("non-ISO9660 filename in extracted disc: " + path_utf8_bytes(relative));
        }
        std::string key;
        std::string parent;
        size_t depth = 0u;
        for (const fs::path& part : relative)
        {
            if (!key.empty())
            {
                parent = key;
                key += '/';
            }
            key += ascii_upper(path_utf8_bytes(part));
            if (++depth > 16u)
            {
                return fail("extracted disc path too deep");
            }
        }
        if (m_nodes.find(key) != m_nodes.end())
        {
            return fail("case-insensitive filename collision");
        }
        const bool directory = fs::is_directory(status);
        const uint64_t file_size = directory ? 0u : fs::file_size(it->path(), error);
        if (error || file_size > (std::numeric_limits<uint32_t>::max)())
        {
            return fail("cannot measure disc file or file exceeds 4 GiB");
        }
        m_nodes.emplace(key, s_node{
            it->path(), ascii_upper(name), parent,
            0u, static_cast<uint32_t>(file_size), directory
        });
    }
    if (error)
    {
        return fail("cannot enumerate extracted disc");
    }

    // Breadth-first directory order ensures ISO path-table parent numbers
    // always refer to an already numbered directory.
    std::vector<std::string> directories{""};
    std::map<std::string, uint16_t> ordinals{{"", 1u}};
    for (size_t index = 0u; index < directories.size(); ++index)
    {
        const std::string key = directories[index];
        for (const auto& item : m_nodes)
        {
            if (!item.first.empty() && item.second.m_directory &&
                item.second.m_parent == key)
            {
                if (directories.size() >= 65535u)
                {
                    return fail("too many ISO directories");
                }
                ordinals.emplace(item.first,
                    static_cast<uint16_t>(directories.size() + 1u));
                directories.push_back(item.first);
            }
        }
    }

    uint64_t path_bytes = 0u;
    for (const std::string& key : directories)
    {
        s_node& node = m_nodes.at(key);
        node.m_size = static_cast<uint32_t>(make_directory(key).size());
        const size_t length = key.empty() ? 1u : node.m_name.size();
        path_bytes += 8u + ((length + 1u) & ~size_t(1u));
    }
    if (path_bytes > (std::numeric_limits<uint32_t>::max)())
    {
        return fail("ISO path tables too large");
    }
    const uint64_t path_sectors = sectors(path_bytes);
    uint64_t next = 18u + 2u * path_sectors;
    for (const std::string& key : directories)
    {
        s_node& node = m_nodes.at(key);
        if (next + sectors(node.m_size) > (std::numeric_limits<uint32_t>::max)())
        {
            return fail("ISO directory extent overflow");
        }
        node.m_lba = static_cast<uint32_t>(next);
        next += sectors(node.m_size);
    }
    next = (std::max)(next, uint64_t(32u));
    for (auto& item : m_nodes)
    {
        s_node& node = item.second;
        if (node.m_directory)
        {
            continue;
        }
        const uint64_t needed = (std::max)(sectors(node.m_size), uint64_t(1u));
        if (next + needed > (std::numeric_limits<uint32_t>::max)())
        {
            return fail("ISO file extent overflow");
        }
        node.m_lba = static_cast<uint32_t>(next);
        next += needed;
    }
    m_sector_count = next;

    for (const std::string& key : directories)
    {
        const s_node& node = m_nodes.at(key);
        add_memory(node.m_lba, make_directory(key));
    }
    for (const auto& item : m_nodes)
    {
        const s_node& node = item.second;
        if (!node.m_directory)
        {
            s_region region{};
            region.m_count = static_cast<uint32_t>(
                (std::max)(sectors(node.m_size), uint64_t(1u))
            );
            region.m_size = node.m_size;
            region.m_host_path = node.m_host_path;
            m_regions.emplace(node.m_lba, std::move(region));
        }
    }

    for (bool big : {false, true})
    {
        std::vector<uint8_t> table(
            static_cast<size_t>(path_sectors * k_sector_size), 0u
        );
        size_t position = 0u;
        for (const std::string& key : directories)
        {
            const s_node& node = m_nodes.at(key);
            const std::string name = key.empty()
                ? std::string(1u, '\0') : node.m_name;
            table[position] = static_cast<uint8_t>(name.size());
            put32(table.data() + position + 2u, node.m_lba, big);
            put16(table.data() + position + 6u,
                  ordinals.at(node.m_parent), big);
            std::memcpy(table.data() + position + 8u,
                        name.data(), name.size());
            position += 8u + ((name.size() + 1u) & ~size_t(1u));
        }
        add_memory(static_cast<uint32_t>(
            18u + (big ? path_sectors : 0u)), std::move(table)
        );
    }

    std::vector<uint8_t> pvd(k_sector_size, 0u);
    std::vector<uint8_t> terminator(k_sector_size, 0u);
    pvd[0] = 1u;
    terminator[0] = 255u;
    std::memcpy(pvd.data() + 1u, "CD001", 5u);
    std::memcpy(terminator.data() + 1u, "CD001", 5u);
    pvd[6] = terminator[6] = 1u;
    std::memset(pvd.data() + 8u, ' ', 64u);
    std::memcpy(pvd.data() + 8u, "PLAYSTATION", 11u);
    std::memcpy(pvd.data() + 40u, "PS2_FOLDER", 10u);
    both32(pvd.data() + 80u, static_cast<uint32_t>(m_sector_count));
    both16(pvd.data() + 120u, 1u);
    both16(pvd.data() + 124u, 1u);
    both16(pvd.data() + 128u, k_sector_size);
    both32(pvd.data() + 132u, static_cast<uint32_t>(path_bytes));
    put32(pvd.data() + 140u, 18u);
    put32(pvd.data() + 148u, static_cast<uint32_t>(18u + path_sectors), true);
    for (unsigned offset : {813u, 830u, 847u, 864u})
    {
        std::memset(pvd.data() + offset, '0', 16u);
    }
    pvd[881] = 1u;
    const std::vector<uint8_t> root_record =
        make_record(m_nodes.at(""), std::string(1u, '\0'));
    std::memcpy(pvd.data() + 156u,
                root_record.data(), root_record.size());
    add_memory(16u, std::move(pvd));
    add_memory(17u, std::move(terminator));
    return true;
}

uint64_t c_folder_sector_source::sector_count() const
{
    return m_sector_count;
}

bool c_folder_sector_source::find_file(
    const std::string& guest_path, s_folder_file_info* output
) const
{
    if (!m_valid || output == nullptr || guest_path.size() > 1024u ||
        guest_path.size() < 8u ||
        ascii_upper(guest_path.substr(0u, 7u)) != "CDROM0:")
    {
        return false;
    }
    std::string path = guest_path.substr(7u);
    if (!path.empty() && (path.front() == '\\' || path.front() == '/'))
    {
        path.erase(path.begin());
    }
    if (path.empty())
    {
        return false;
    }
    for (char& ch : path)
    {
        if (ch == '\\')
        {
            ch = '/';
        }
    }
    if (path.back() == '/' || path.find("//") != std::string::npos)
    {
        return false;
    }
    std::string key;
    size_t start = 0u;
    while (start < path.size())
    {
        const size_t delimiter = path.find('/', start);
        const size_t end = delimiter == std::string::npos
            ? path.size() : delimiter;
        std::string part = path.substr(start, end - start);
        if (delimiter == std::string::npos)
        {
            const size_t version = part.find(';');
            if (version != std::string::npos)
            {
                if (part.substr(version) != ";1")
                {
                    return false;
                }
                part.resize(version);
            }
        }
        if (!valid_name(part))
        {
            return false;
        }
        if (!key.empty())
        {
            key += '/';
        }
        key += ascii_upper(part);
        if (delimiter == std::string::npos)
        {
            break;
        }
        start = delimiter + 1u;
    }
    const auto it = m_nodes.find(key);
    if (it == m_nodes.end() || it->second.m_directory)
    {
        return false;
    }
    output->m_lba = it->second.m_lba;
    output->m_size = it->second.m_size;
    output->m_name = it->second.m_name;
    return true;
}

bool c_folder_sector_source::read_sectors(
    uint32_t lba, uint32_t count, std::vector<uint8_t>& data
)
{
    const uint64_t end = static_cast<uint64_t>(lba) + count;
    if (!m_valid || count == 0u || end > m_sector_count ||
        count > (std::numeric_limits<uint32_t>::max)() / k_sector_size)
    {
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(count) * k_sector_size, 0u);
    uint64_t current = lba;
    while (current < end)
    {
        auto it = m_regions.upper_bound(static_cast<uint32_t>(current));
        if (it == m_regions.begin())
        {
            ++current;
            continue;
        }
        --it;
        const uint64_t region_end = static_cast<uint64_t>(it->first) +
            it->second.m_count;
        if (current >= region_end)
        {
            ++current;
            continue;
        }
        const s_region& region = it->second;
        const uint64_t span = (std::min)(end, region_end) - current;
        const uint64_t offset =
            (current - it->first) * k_sector_size;
        const size_t destination =
            static_cast<size_t>(current - lba) * k_sector_size;
        const size_t available = offset < region.m_size
            ? static_cast<size_t>((std::min)(
                span * k_sector_size, uint64_t(region.m_size) - offset))
            : 0u;
        if (!region.m_memory.empty())
        {
            if (available != 0u)
            {
                std::memcpy(bytes.data() + destination,
                    region.m_memory.data() + offset, available);
            }
        }
        else if (available != 0u)
        {
            std::error_code error;
            if (std::filesystem::is_symlink(
                    std::filesystem::symlink_status(region.m_host_path, error)) ||
                error ||
                std::filesystem::file_size(region.m_host_path, error) !=
                    region.m_size || error)
            {
                return false;
            }
            std::ifstream input(region.m_host_path, std::ios::binary);
            if (!input || !input.seekg(static_cast<std::streamoff>(offset)) ||
                !input.read(reinterpret_cast<char*>(
                    bytes.data() + destination),
                    static_cast<std::streamsize>(available)))
            {
                return false;
            }
        }
        current += span;
    }
    data = std::move(bytes);
    return true;
}
} // namespace iop
