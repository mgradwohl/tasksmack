#pragma once

// The version resource (RT_VERSION) of a PE file, found in the file's own bytes (#4057): the Publisher column reads
// it from a data mapping of the file the signature check holds, never through GetFileVersionInfoW by a name, which a
// per-user drive letter or subst could point elsewhere by the time it is opened, and which may take a language .mui
// file's version info instead. The bytes are the file's, so anyone can have written them: every offset and size is
// checked against the file before it is read, the walk is the resource directory's fixed three levels (type, name,
// language) with no recursion, so a directory that points back at itself can't loop, and the answer is either a span
// inside the file or none. Plain bytes, so it reaches no Windows header and its tests build and run on every platform
// (#1133); WindowsImageFile.cpp maps the file and reads the CompanyName out of the block with VerQueryValueW.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Platform::VersionResourceMath
{

inline constexpr std::uint16_t DOS_MAGIC = 0x5A4D;          ///< "MZ"
inline constexpr std::uint32_t PE_SIGNATURE = 0x00004550;   ///< "PE\0\0"
inline constexpr std::uint16_t PE32_MAGIC = 0x010B;         ///< IMAGE_NT_OPTIONAL_HDR32_MAGIC
inline constexpr std::uint16_t PE32_PLUS_MAGIC = 0x020B;    ///< IMAGE_NT_OPTIONAL_HDR64_MAGIC
inline constexpr std::uint32_t RESOURCE_DIRECTORY_SLOT = 2; ///< IMAGE_DIRECTORY_ENTRY_RESOURCE
inline constexpr std::uint32_t RT_VERSION_ID = 16;          ///< RT_VERSION
inline constexpr std::uint32_t VS_VERSION_INFO_ID = 1;      ///< VS_VERSION_INFO, the name GetFileVersionInfoW finds
inline constexpr std::uint32_t LANGUAGE_NEUTRAL = 0;        ///< LANG_NEUTRAL, SUBLANG_NEUTRAL
inline constexpr std::uint32_t HIGH_BIT = 0x80000000U;      ///< a name string, or a subdirectory

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

/// What pickEntry()'s chooser makes of an entry: not it, it, or it unless a later entry is (a fallback).
enum class Choice : std::uint8_t
{
    Skip,
    Take,
    Fallback,
};

/// One entry of a resource directory: its name or ID, and its offset (a subdirectory's with HIGH_BIT).
struct DirectoryEntry
{
    std::uint32_t name = 0;
    std::uint32_t offset = 0;
};

/// The entry of the directory at @p at in @p resources (the resource section, from the directory's root) that
/// @p pick chooses, or nullopt when the directory doesn't fit or none is chosen. @p pick sees each entry, named ones
/// first as the file orders them, and returns a Choice: the first entry it takes, else the first fallback.
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
    std::optional<DirectoryEntry> fallback;
    for (std::uint64_t i = 0; i < count; ++i)
    {
        const DirectoryEntry entry{
            .name = readLe<std::uint32_t>(resources, first + (i * ENTRY_SIZE)).value_or(0), // in range: checked above
            .offset = readLe<std::uint32_t>(resources, first + (i * ENTRY_SIZE) + 4).value_or(0),
        };
        const Choice choice = pick(entry);
        if (choice == Choice::Take)
        {
            return entry;
        }
        if (choice == Choice::Fallback && !fallback.has_value())
        {
            fallback = entry;
        }
    }
    return fallback;
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

} // namespace Detail

/// The VS_VERSIONINFO block of the PE file @p file (its RT_VERSION resource, named VS_VERSION_INFO as
/// GetFileVersionInfoW looks it up, in the neutral language if it has one, else its first), exactly the block's own
/// length (wLength) and inside @p file; nullopt when @p file isn't a PE file, has no such resource, or anything on the
/// way doesn't fit in it. Only a 32-bit (Unicode) block, whose key is "VS_VERSION_INFO", is taken: a PE file's.
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
    const std::optional<Detail::DirectoryEntry> language =
        Detail::pickEntry(resources,
                          *languages,
                          [](const Detail::DirectoryEntry& entry)
                          { return entry.name == LANGUAGE_NEUTRAL ? Detail::Choice::Take : Detail::Choice::Fallback; });
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
    return block.first(*length);
}

} // namespace Platform::VersionResourceMath
