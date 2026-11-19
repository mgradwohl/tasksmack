#pragma once

// The version resource (RT_VERSION) of a PE file, found in the file's own bytes (#4057): the Publisher column reads
// it from a data mapping of the file the signature check holds, never through GetFileVersionInfoW by a name, which a
// per-user drive letter or subst could point elsewhere by the time it is opened, and which may take a language .mui
// file's version info instead. The bytes are the file's, so anyone can have written them: every offset and size is
// checked against the file before it is read, the walk is the resource directory's fixed three levels (type, name,
// language) with no recursion, so a directory that points back at itself can't loop, and the answer is either a span
// inside the file or none. Plain bytes, so it reaches no Windows header and its tests build and run on every platform
// (#1133); WindowsHeldFileVersion.cpp maps the file, WindowsImageFile.cpp reads the CompanyName out of the block with
// VerQueryValueW, and the Modules section reads the file version out of it with fixedFileVersion() (#4373).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Platform::VersionResourceMath
{

inline constexpr std::uint16_t DOS_MAGIC = 0x5A4D;           ///< "MZ"
inline constexpr std::uint32_t PE_SIGNATURE = 0x00004550;    ///< "PE\0\0"
inline constexpr std::uint16_t PE32_MAGIC = 0x010B;          ///< IMAGE_NT_OPTIONAL_HDR32_MAGIC
inline constexpr std::uint16_t PE32_PLUS_MAGIC = 0x020B;     ///< IMAGE_NT_OPTIONAL_HDR64_MAGIC
inline constexpr std::uint32_t RESOURCE_DIRECTORY_SLOT = 2;  ///< IMAGE_DIRECTORY_ENTRY_RESOURCE
inline constexpr std::uint32_t RT_VERSION_ID = 16;           ///< RT_VERSION
inline constexpr std::uint32_t VS_VERSION_INFO_ID = 1;       ///< VS_VERSION_INFO, the name GetFileVersionInfoW finds
inline constexpr std::uint32_t LANGUAGE_NEUTRAL = 0;         ///< LANG_NEUTRAL, SUBLANG_NEUTRAL
inline constexpr std::uint32_t LANGUAGE_US_ENGLISH = 0x0409; ///< en-US
inline constexpr std::uint32_t HIGH_BIT = 0x80000000U;       ///< a name string, or a subdirectory

/// VS_VERSIONINFO's szKey, "VS_VERSION_INFO" in UTF-16 with its terminator, after wLength, wValueLength and wType.
inline constexpr std::array<char16_t, 16> VERSION_INFO_KEY{
    u'V',
    u'S',
    u'_',
    u'V',
    u'E',
    u'R',
    u'S',
    u'I',
    u'O',
    u'N',
    u'_',
    u'I',
    u'N',
    u'F',
    u'O',
    u'\0',
};
inline constexpr std::size_t VERSION_INFO_HEADER = 6;                                                ///< three WORDs
inline constexpr std::size_t VERSION_INFO_MIN = VERSION_INFO_HEADER + (VERSION_INFO_KEY.size() * 2); ///< and the key

namespace Detail
{

/// The little-endian unsigned @p T at @p at in @p bytes, or nullopt when it doesn't fit.
template<typename T> [[nodiscard]] constexpr std::optional<T> readLe(std::span<const std::byte> bytes, std::uint64_t at) noexcept
{
    if (at > bytes.size() || bytes.size() - at < sizeof(T))
    {
        return std::nullopt;
    }
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i)
    {
        value =
            static_cast<T>(value | (static_cast<T>(std::to_integer<std::uint8_t>(bytes[static_cast<std::size_t>(at) + i])) << (8U * i)));
    }
    return value;
}

/// Where an RVA's bytes are in the file: their offset, and how many bytes of its section follow it there.
struct FileRange
{
    std::uint64_t offset = 0;
    std::uint64_t available = 0;
};

/// The PE headers' facts the walk needs.
struct Sections
{
    std::uint64_t table = 0; ///< the section table's offset
    std::uint16_t count = 0;
};

/// Where @p rva is in @p file, by its section's raw data, with at least @p length bytes of that section after it in
/// the file; nullopt when no section holds it so.
[[nodiscard]] constexpr std::optional<FileRange>
rvaToFile(std::span<const std::byte> file, const Sections& sections, std::uint32_t rva, std::uint64_t length) noexcept
{
    constexpr std::uint64_t SECTION_SIZE = 40;
    for (std::uint64_t i = 0; i < sections.count; ++i)
    {
        const std::uint64_t at = sections.table + (i * SECTION_SIZE);
        const std::optional<std::uint32_t> virtualAddress = readLe<std::uint32_t>(file, at + 12);
        const std::optional<std::uint32_t> rawSize = readLe<std::uint32_t>(file, at + 16);
        const std::optional<std::uint32_t> rawPointer = readLe<std::uint32_t>(file, at + 20);
        if (!virtualAddress.has_value() || !rawSize.has_value() || !rawPointer.has_value())
        {
            return std::nullopt; // the table runs off the end of the file
        }
        if (rva < *virtualAddress || rva - *virtualAddress >= *rawSize)
        {
            continue;
        }
        const std::uint64_t into = rva - *virtualAddress;
        const std::uint64_t offset = std::uint64_t{*rawPointer} + into; // both 32-bit: no overflow in 64
        if (offset >= file.size())
        {
            return std::nullopt;
        }
        const std::uint64_t inSection = *rawSize - into;
        const std::uint64_t inFile = file.size() - offset;
        const std::uint64_t available = inSection < inFile ? inSection : inFile;
        if (available < length)
        {
            return std::nullopt;
        }
        return FileRange{.offset = offset, .available = available};
    }
    return std::nullopt;
}

/// What pickEntry()'s chooser makes of an entry: not it, or it.
enum class Choice : std::uint8_t
{
    Skip,
    Take,
};

/// One entry of a resource directory: its name or ID, and its offset (a subdirectory's with HIGH_BIT).
struct DirectoryEntry
{
    std::uint32_t name = 0;
    std::uint32_t offset = 0;
};

/// The entry of the directory at @p at in @p resources (the resource section, from the directory's root) that
/// @p pick chooses, or nullopt when the directory doesn't fit or none is chosen. @p pick sees each entry, named ones
/// first as the file orders them, and returns a Choice: the first entry it takes.
template<typename Pick>
[[nodiscard]] constexpr std::optional<DirectoryEntry> pickEntry(std::span<const std::byte> resources, std::uint64_t at, Pick pick) noexcept
{
    constexpr std::uint64_t DIRECTORY_SIZE = 16;
    constexpr std::uint64_t ENTRY_SIZE = 8;
    const std::optional<std::uint16_t> named = readLe<std::uint16_t>(resources, at + 12);
    const std::optional<std::uint16_t> ids = readLe<std::uint16_t>(resources, at + 14);
    if (!named.has_value() || !ids.has_value())
    {
        return std::nullopt;
    }
    const std::uint64_t count = std::uint64_t{*named} + *ids;
    const std::uint64_t first = at + DIRECTORY_SIZE;
    if (first > resources.size() || (resources.size() - first) / ENTRY_SIZE < count)
    {
        return std::nullopt; // more entries than the section holds
    }
    for (std::uint64_t i = 0; i < count; ++i)
    {
        const DirectoryEntry entry{
            .name = readLe<std::uint32_t>(resources, first + (i * ENTRY_SIZE)).value_or(0), // in range: checked above
            .offset = readLe<std::uint32_t>(resources, first + (i * ENTRY_SIZE) + 4).value_or(0),
        };
        if (pick(entry) == Choice::Take)
        {
            return entry;
        }
    }
    return std::nullopt;
}

/// The subdirectory @p entry names, as an offset into the resource section; nullopt when it names data instead.
[[nodiscard]] constexpr std::optional<std::uint64_t> subdirectory(const std::optional<DirectoryEntry>& entry) noexcept
{
    if (!entry.has_value() || (entry->offset & HIGH_BIT) == 0)
    {
        return std::nullopt;
    }
    return entry->offset & ~HIGH_BIT;
}

/// Where a version info node's children start, inside the node @p block[@p at, @p end): after its header, its
/// NUL-terminated UTF-16 key and its value, each DWORD-aligned; @p end when it has none. nullopt when the key isn't
/// terminated inside the node, or the value runs past its end. A text value (wType 1) counts wValueLength in
/// characters, as the resource compiler writes it, but some linkers write bytes, so only the smaller reading is held to
/// the node's end; a binary value (wType 0, such as Translation) counts bytes and is held to it exactly.
[[nodiscard]] constexpr std::optional<std::uint64_t>
nodeChildren(std::span<const std::byte> block, std::uint64_t at, std::uint64_t end) noexcept
{
    constexpr std::uint64_t HEADER = 6;
    const std::optional<std::uint16_t> valueLength = readLe<std::uint16_t>(block, at + 2);
    if (!valueLength.has_value())
    {
        return std::nullopt;
    }
    std::uint64_t pos = at + HEADER;
    for (;;)
    {
        if (pos + 2 > end)
        {
            return std::nullopt; // the key runs off the node
        }
        const std::uint16_t c = readLe<std::uint16_t>(block, pos).value_or(0);
        pos += 2;
        if (c == 0)
        {
            break;
        }
    }
    pos = (pos + 3U) & ~std::uint64_t{3};
    const std::optional<std::uint16_t> type = readLe<std::uint16_t>(block, at + 4);
    std::uint64_t valueBytes = *valueLength;
    if (type == 1 && pos + (2 * valueBytes) <= end)
    {
        valueBytes *= 2; // characters, as the resource compiler counts them
    }
    if (pos + valueBytes > end)
    {
        return std::nullopt; // the value, read the shorter way, runs off the node
    }
    pos = (pos + valueBytes + 3U) & ~std::uint64_t{3};
    return pos < end ? pos : end;
}

/// Whether the VS_VERSIONINFO @p block is a tree of nodes that each fit their parent (#4057): every child's wLength at
/// least a header and no more than its parent's remaining bytes, children DWORD-aligned, keys terminated and values
/// inside their nodes, and no deeper than VerQueryValueW's paths go. VerQueryValueW walks the block by these lengths
/// and returns the values it finds with their declared lengths, so a node that lies about either would make it read,
/// or hand back, bytes past the block.
[[nodiscard]] constexpr bool isVersionTree(std::span<const std::byte> block) noexcept
{
    constexpr std::uint64_t HEADER = 6;
    constexpr std::size_t MAX_DEPTH = 8; // VS_VERSIONINFO > StringFileInfo > StringTable > String: four levels
    struct Level
    {
        std::uint64_t pos = 0;
        std::uint64_t end = 0;
    };
    std::array<Level, MAX_DEPTH> levels{};
    const std::optional<std::uint64_t> rootChildren = nodeChildren(block, 0, block.size());
    if (!rootChildren.has_value())
    {
        return false;
    }
    levels[0] = Level{.pos = *rootChildren, .end = block.size()};
    std::size_t depth = 1;
    while (depth > 0)
    {
        Level& level = levels[depth - 1];
        if (level.pos + HEADER > level.end)
        {
            --depth; // the rest is padding
            continue;
        }
        const std::uint16_t length = readLe<std::uint16_t>(block, level.pos).value_or(0);
        if (length < HEADER || length > level.end - level.pos)
        {
            return false;
        }
        const std::uint64_t at = level.pos;
        const std::uint64_t end = at + length;
        level.pos = (end + 3U) & ~std::uint64_t{3};
        const std::optional<std::uint64_t> children = nodeChildren(block, at, end);
        if (!children.has_value())
        {
            return false;
        }
        if (*children < end)
        {
            if (depth == MAX_DEPTH)
            {
                return false;
            }
            levels[depth] = Level{.pos = *children, .end = end};
            ++depth;
        }
    }
    return true;
}

} // namespace Detail

/// The VS_VERSIONINFO block of the PE file @p file (its RT_VERSION resource, named VS_VERSION_INFO as
/// GetFileVersionInfoW looks it up), exactly the block's own length (wLength) and inside @p file; nullopt when @p file
/// isn't a PE file, has no such resource, or anything on the way doesn't fit in it. Only a 32-bit (Unicode) block,
/// whose key is "VS_VERSION_INFO" and whose every node fits its parent (Detail::isVersionTree()), is taken.
///
/// The language is the neutral one if there is one, else US English, else the first listed. GetFileVersionInfoW
/// chooses through FindResource's fallback instead (the thread's UI languages first), so for a file with version
/// info in several languages the by-path reads (no signature worker, no start time) can show another of the file's
/// own, self-asserted CompanyNames.
[[nodiscard]] constexpr std::optional<std::span<const std::byte>> findVersionResource(std::span<const std::byte> file) noexcept
{
    using Detail::readLe;
    if (readLe<std::uint16_t>(file, 0) != DOS_MAGIC)
    {
        return std::nullopt;
    }
    const std::optional<std::uint32_t> ntHeaders = readLe<std::uint32_t>(file, 0x3C); // e_lfanew
    if (!ntHeaders.has_value() || readLe<std::uint32_t>(file, *ntHeaders) != PE_SIGNATURE)
    {
        return std::nullopt;
    }
    const std::uint64_t fileHeader = std::uint64_t{*ntHeaders} + 4;
    const std::optional<std::uint16_t> sectionCount = readLe<std::uint16_t>(file, fileHeader + 2);
    const std::optional<std::uint16_t> optionalSize = readLe<std::uint16_t>(file, fileHeader + 16);
    const std::uint64_t optionalHeader = fileHeader + 20;
    const std::optional<std::uint16_t> magic = readLe<std::uint16_t>(file, optionalHeader);
    if (!sectionCount.has_value() || !optionalSize.has_value() || !magic.has_value())
    {
        return std::nullopt;
    }
    // Where NumberOfRvaAndSizes and the data directories are: after a PE32+ header's 64-bit ImageBase and stack and
    // heap sizes, 16 bytes later than a PE32 header's.
    std::uint64_t directoryCountAt = 0;
    if (*magic == PE32_MAGIC)
    {
        directoryCountAt = 92;
    }
    else if (*magic == PE32_PLUS_MAGIC)
    {
        directoryCountAt = 108;
    }
    else
    {
        return std::nullopt;
    }
    const std::uint64_t resourceSlotAt = directoryCountAt + 4 + (std::uint64_t{RESOURCE_DIRECTORY_SLOT} * 8);
    const std::optional<std::uint32_t> directoryCount = readLe<std::uint32_t>(file, optionalHeader + directoryCountAt);
    if (!directoryCount.has_value() || *directoryCount <= RESOURCE_DIRECTORY_SLOT || resourceSlotAt + 8 > *optionalSize)
    {
        return std::nullopt; // no resource directory in this header
    }
    const std::optional<std::uint32_t> resourceRva = readLe<std::uint32_t>(file, optionalHeader + resourceSlotAt);
    if (!resourceRva.has_value() || *resourceRva == 0)
    {
        return std::nullopt;
    }
    const Detail::Sections sections{.table = optionalHeader + *optionalSize, .count = *sectionCount};
    constexpr std::uint64_t SECTION_HEADER_SIZE = 40;
    if (sections.table > file.size() || (file.size() - sections.table) / SECTION_HEADER_SIZE < sections.count)
    {
        return std::nullopt; // a section table that runs off the end of the file: not a file Windows would load
    }
    const std::optional<Detail::FileRange> root = Detail::rvaToFile(file, sections, *resourceRva, 16);
    if (!root.has_value())
    {
        return std::nullopt;
    }
    // Every directory offset is from the root, and read only inside the root's section.
    const std::span<const std::byte> resources =
        file.subspan(static_cast<std::size_t>(root->offset), static_cast<std::size_t>(root->available));

    const std::optional<std::uint64_t> names = Detail::subdirectory(Detail::pickEntry(
        resources,
        0,
        [](const Detail::DirectoryEntry& entry) { return entry.name == RT_VERSION_ID ? Detail::Choice::Take : Detail::Choice::Skip; }));
    if (!names.has_value())
    {
        return std::nullopt;
    }
    const std::optional<std::uint64_t> languages =
        Detail::subdirectory(Detail::pickEntry(resources,
                                               *names,
                                               [](const Detail::DirectoryEntry& entry)
                                               { return entry.name == VS_VERSION_INFO_ID ? Detail::Choice::Take : Detail::Choice::Skip; }));
    if (!languages.has_value())
    {
        return std::nullopt;
    }
    const auto languageIs = [](std::uint32_t wanted)
    {
        return [wanted](const Detail::DirectoryEntry& entry)
        {
            return entry.name == wanted ? Detail::Choice::Take : Detail::Choice::Skip;
        };
    };
    std::optional<Detail::DirectoryEntry> language = Detail::pickEntry(resources, *languages, languageIs(LANGUAGE_NEUTRAL));
    if (!language.has_value())
    {
        language = Detail::pickEntry(resources, *languages, languageIs(LANGUAGE_US_ENGLISH));
    }
    if (!language.has_value())
    {
        language = Detail::pickEntry(resources, *languages, [](const Detail::DirectoryEntry&) { return Detail::Choice::Take; });
    }
    if (!language.has_value() || (language->offset & HIGH_BIT) != 0)
    {
        return std::nullopt; // a subdirectory where the data entry belongs
    }
    // IMAGE_RESOURCE_DATA_ENTRY: the data's RVA (not an offset from the root) and its size.
    const std::optional<std::uint32_t> dataRva = readLe<std::uint32_t>(resources, language->offset);
    const std::optional<std::uint32_t> dataSize = readLe<std::uint32_t>(resources, std::uint64_t{language->offset} + 4);
    if (!dataRva.has_value() || !dataSize.has_value() || *dataSize < VERSION_INFO_MIN)
    {
        return std::nullopt;
    }
    const std::optional<Detail::FileRange> data = Detail::rvaToFile(file, sections, *dataRva, *dataSize);
    if (!data.has_value())
    {
        return std::nullopt;
    }
    const std::span<const std::byte> block = file.subspan(static_cast<std::size_t>(data->offset), *dataSize);
    const std::optional<std::uint16_t> length = readLe<std::uint16_t>(block, 0); // wLength: the whole block's
    if (!length.has_value() || *length < VERSION_INFO_MIN || *length > block.size())
    {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < VERSION_INFO_KEY.size(); ++i)
    {
        if (readLe<std::uint16_t>(block, VERSION_INFO_HEADER + (i * 2)) != static_cast<std::uint16_t>(VERSION_INFO_KEY[i]))
        {
            return std::nullopt; // not a 32-bit VS_VERSIONINFO
        }
    }
    const std::span<const std::byte> versionInfo = block.first(*length);
    if (!Detail::isVersionTree(versionInfo))
    {
        return std::nullopt;
    }
    return versionInfo;
}

/// VS_FIXEDFILEINFO's dwSignature, telling a real fixed-info block from garbage.
inline constexpr std::uint32_t FIXED_FILE_INFO_SIGNATURE = 0xFEEF04BDU;
/// VS_FIXEDFILEINFO's size: thirteen DWORDs.
inline constexpr std::size_t FIXED_FILE_INFO_SIZE = 52;

/// A file's version as VS_FIXEDFILEINFO holds it: dwFileVersionMS (major, minor) and dwFileVersionLS (build, revision).
struct FixedFileVersion
{
    std::uint32_t mostSignificant = 0;
    std::uint32_t leastSignificant = 0;

    [[nodiscard]] friend constexpr bool operator==(const FixedFileVersion&, const FixedFileVersion&) noexcept = default;
};

/// The file version in the VS_VERSIONINFO @p block (as findVersionResource() finds it): its VS_FIXEDFILEINFO value,
/// right after the "VS_VERSION_INFO" key, DWORD-aligned. nullopt when the block is too short, declares a value shorter
/// than a VS_FIXEDFILEINFO, or the value's signature isn't 0xFEEF04BD (#4373: the Modules section reads a module's
/// version this way, from a file it holds, never through GetFileVersionInfoW by a name).
[[nodiscard]] constexpr std::optional<FixedFileVersion> fixedFileVersion(std::span<const std::byte> block) noexcept
{
    using Detail::readLe;
    constexpr std::uint64_t VALUE_AT = (VERSION_INFO_MIN + 3U) & ~std::uint64_t{3}; // the header and key, DWORD-aligned
    const std::optional<std::uint16_t> length = readLe<std::uint16_t>(block, 0);
    const std::optional<std::uint16_t> valueLength = readLe<std::uint16_t>(block, 2);
    if (!length.has_value() || !valueLength.has_value() || *valueLength < FIXED_FILE_INFO_SIZE || *length > block.size() ||
        *length < VALUE_AT + FIXED_FILE_INFO_SIZE)
    {
        return std::nullopt;
    }
    const std::span<const std::byte> value = block.subspan(static_cast<std::size_t>(VALUE_AT), FIXED_FILE_INFO_SIZE);
    const std::optional<std::uint32_t> signature = readLe<std::uint32_t>(value, 0);
    const std::optional<std::uint32_t> mostSignificant = readLe<std::uint32_t>(value, 8);
    const std::optional<std::uint32_t> leastSignificant = readLe<std::uint32_t>(value, 12);
    if (signature != FIXED_FILE_INFO_SIGNATURE || !mostSignificant.has_value() || !leastSignificant.has_value())
    {
        return std::nullopt;
    }
    return FixedFileVersion{.mostSignificant = *mostSignificant, .leastSignificant = *leastSignificant};
}

} // namespace Platform::VersionResourceMath
