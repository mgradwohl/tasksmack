#pragma once

/// @file PeImageBuilder.h
/// @brief A minimal PE file with a version resource, built byte by byte for the version resource walk's tests
/// (#4057, #4373): VersionResourceMath::findVersionResource() and fixedFileVersion() on every platform, and
/// Windows::heldFilePublisher() and heldFileVersion() on a file a test writes. Only what the walk reads is filled in:
/// the DOS header's e_lfanew, the PE signature, the file header's section count and optional header size, the
/// optional header's magic and resource directory, one .rsrc section, and
/// the type, name and language directories down to each language's VS_VERSIONINFO block. The offsets of each piece
/// are kept, so a test can break one. No Windows header.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace TestSupport
{

/// Little-endian writes into a byte buffer, growing it as needed.
inline void putLe(std::vector<std::byte>& bytes, std::size_t at, std::uint64_t value, std::size_t width)
{
    if (bytes.size() < at + width)
    {
        bytes.resize(at + width);
    }
    for (std::size_t i = 0; i < width; ++i)
    {
        bytes[at + i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
    }
}
inline void put16(std::vector<std::byte>& bytes, std::size_t at, std::uint64_t value)
{
    putLe(bytes, at, value, 2);
}
inline void put32(std::vector<std::byte>& bytes, std::size_t at, std::uint64_t value)
{
    putLe(bytes, at, value, 4);
}

/// One version info node (VS_VERSIONINFO, StringFileInfo, StringTable, String, VarFileInfo, Var): its header, its
/// UTF-16 key, its value and its children, each DWORD-aligned, as the resource compiler lays them out.
struct VersionNode // NOLINT(misc-no-recursion) - a tree, built and written recursively
{
    std::u16string key;
    std::vector<std::byte> value;
    bool text = false; ///< the value is a UTF-16 string (wValueLength in characters, wType 1)
    std::vector<VersionNode> children;

    [[nodiscard]] std::vector<std::byte> bytes() const // NOLINT(misc-no-recursion) - one call per node
    {
        std::vector<std::byte> out(6);
        for (const char16_t c : key)
        {
            put16(out, out.size(), c);
        }
        put16(out, out.size(), 0);
        out.resize((out.size() + 3U) & ~std::size_t{3});
        out.insert(out.end(), value.begin(), value.end());
        for (const VersionNode& child : children)
        {
            out.resize((out.size() + 3U) & ~std::size_t{3});
            const std::vector<std::byte> inner = child.bytes();
            out.insert(out.end(), inner.begin(), inner.end());
        }
        put16(out, 0, out.size());
        put16(out, 2, text ? value.size() / 2 : value.size());
        put16(out, 4, text ? 1 : 0);
        return out;
    }
};

/// @p text as a String node's value: UTF-16 with its terminator.
[[nodiscard]] inline std::vector<std::byte> utf16Value(std::u16string_view text)
{
    std::vector<std::byte> out;
    for (const char16_t c : text)
    {
        put16(out, out.size(), c);
    }
    put16(out, out.size(), 0);
    return out;
}

/// A VS_VERSIONINFO block naming @p companyName in the US English Unicode table (040904B0), with a translation
/// table that lists it, and @p fileVersionMs and @p fileVersionLs as its VS_FIXEDFILEINFO's file version (#4373).
[[nodiscard]] inline std::vector<std::byte>
versionInfoBlock(std::u16string_view companyName, std::uint32_t fileVersionMs = 0, std::uint32_t fileVersionLs = 0)
{
    std::vector<std::byte> fixed;
    put32(fixed, 0, 0xFEEF04BDU); // VS_FIXEDFILEINFO's dwSignature
    put32(fixed, 4, 0x00010000U); // dwStrucVersion
    put32(fixed, 8, fileVersionMs);
    put32(fixed, 12, fileVersionLs);
    fixed.resize(52);
    std::vector<std::byte> translation;
    put32(translation, 0, 0x04B00409U); // US English, Unicode
    const VersionNode root{
        .key = u"VS_VERSION_INFO",
        .value = fixed,
        .text = false,
        .children =
            {
                VersionNode{
                    .key = u"StringFileInfo",
                    .value = {},
                    .text = true,
                    .children =
                        {
                            VersionNode{
                                .key = u"040904B0",
                                .value = {},
                                .text = true,
                                .children =
                                    {
                                        VersionNode{.key = u"CompanyName", .value = utf16Value(companyName), .text = true, .children = {}},
                                    },
                            },
                        },
                },
                VersionNode{
                    .key = u"VarFileInfo",
                    .value = {},
                    .text = true,
                    .children = {VersionNode{.key = u"Translation", .value = translation, .text = false, .children = {}}},
                },
            },
    };
    return root.bytes();
}

/// The built file and where each of its pieces is.
struct PeImage
{
    std::vector<std::byte> bytes;
    std::size_t optionalHeader = 0;
    std::size_t resourceSlot = 0; ///< the optional header's resource directory entry (RVA, size)
    std::size_t section = 0;      ///< the .rsrc section header
    std::size_t resources = 0;    ///< the resource directory's root, in the file
    std::size_t typeEntry = 0;    ///< the root's RT_VERSION entry
    std::size_t nameDirectory = 0;
    std::size_t nameEntry = 0;
    std::size_t languageDirectory = 0;
    std::vector<std::size_t> languageEntries;
    std::vector<std::size_t> dataEntries;
    std::vector<std::size_t> blocks;
};

inline constexpr std::uint32_t RESOURCE_RVA = 0x1000;
inline constexpr std::size_t RESOURCE_RAW = 0x200;

/// A PE32+ file (or with @p pe32, PE32) whose RT_VERSION / VS_VERSION_INFO resource has one language per entry of
/// @p languages, each with its own block. With @p iconFirst, the root has an RT_ICON type ahead of RT_VERSION.
[[nodiscard]] inline PeImage
buildPeImage(const std::vector<std::pair<std::uint32_t, std::vector<std::byte>>>& languages, bool pe32 = false, bool iconFirst = false)
{
    PeImage image;
    std::vector<std::byte>& b = image.bytes;
    b.resize(RESOURCE_RAW);
    put16(b, 0, 0x5A4D);  // "MZ"
    put32(b, 0x3C, 0x40); // e_lfanew
    put32(b, 0x40, 0x00004550);
    const std::size_t fileHeader = 0x44;
    const std::size_t optionalSize = pe32 ? 96 + 128 : 112 + 128;
    put16(b, fileHeader, pe32 ? 0x014C : 0x8664);
    put16(b, fileHeader + 2, 1); // one section
    put16(b, fileHeader + 16, optionalSize);
    image.optionalHeader = fileHeader + 20;
    put16(b, image.optionalHeader, pe32 ? 0x010B : 0x020B);
    const std::size_t directories = image.optionalHeader + (pe32 ? 96 : 112);
    put32(b, directories - 4, 16);         // NumberOfRvaAndSizes
    image.resourceSlot = directories + 16; // the third directory entry, IMAGE_DIRECTORY_ENTRY_RESOURCE
    image.section = image.optionalHeader + optionalSize;

    // The resource section: the root, the names under RT_VERSION, the languages under VS_VERSION_INFO, their data
    // entries, then their blocks.
    const std::size_t root = 0;
    const std::size_t rootEntries = iconFirst ? 2 : 1;
    const std::size_t names = root + 16 + (rootEntries * 8);
    const std::size_t langs = names + 16 + 8;
    const std::size_t data = langs + 16 + (languages.size() * 8);
    std::size_t blockAt = data + (languages.size() * 16);
    std::vector<std::byte> rsrc;
    put16(rsrc, root + 14, rootEntries); // ID entries
    std::size_t entry = root + 16;
    if (iconFirst)
    {
        put32(rsrc, entry, 3);                       // RT_ICON
        put32(rsrc, entry + 4, 0x80000000U | names); // a subdirectory, which the walk never takes
        entry += 8;
    }
    image.typeEntry = RESOURCE_RAW + entry;
    put32(rsrc, entry, 16); // RT_VERSION
    put32(rsrc, entry + 4, 0x80000000U | names);
    put16(rsrc, names + 14, 1);
    image.nameDirectory = RESOURCE_RAW + names;
    image.nameEntry = RESOURCE_RAW + names + 16;
    put32(rsrc, names + 16, 1); // VS_VERSION_INFO
    put32(rsrc, names + 20, 0x80000000U | langs);
    put16(rsrc, langs + 14, languages.size());
    image.languageDirectory = RESOURCE_RAW + langs;
    for (std::size_t i = 0; i < languages.size(); ++i)
    {
        const std::size_t languageEntry = langs + 16 + (i * 8);
        const std::size_t dataEntry = data + (i * 16);
        put32(rsrc, languageEntry, languages[i].first);
        put32(rsrc, languageEntry + 4, dataEntry);
        blockAt = (blockAt + 3U) & ~std::size_t{3};
        put32(rsrc, dataEntry, RESOURCE_RVA + blockAt);
        put32(rsrc, dataEntry + 4, languages[i].second.size());
        rsrc.resize(blockAt);
        rsrc.insert(rsrc.end(), languages[i].second.begin(), languages[i].second.end());
        image.languageEntries.push_back(RESOURCE_RAW + languageEntry);
        image.dataEntries.push_back(RESOURCE_RAW + dataEntry);
        image.blocks.push_back(RESOURCE_RAW + blockAt);
        blockAt += languages[i].second.size();
    }
    image.resources = RESOURCE_RAW + root;

    put32(b, image.resourceSlot, RESOURCE_RVA);
    put32(b, image.resourceSlot + 4, rsrc.size());
    constexpr std::string_view SECTION_NAME = ".rsrc";
    for (std::size_t i = 0; i < SECTION_NAME.size(); ++i)
    {
        b[image.section + i] = static_cast<std::byte>(SECTION_NAME[i]);
    }
    put32(b, image.section + 8, rsrc.size());   // VirtualSize
    put32(b, image.section + 12, RESOURCE_RVA); // VirtualAddress
    put32(b, image.section + 16, rsrc.size());  // SizeOfRawData
    put32(b, image.section + 20, RESOURCE_RAW); // PointerToRawData
    b.insert(b.end(), rsrc.begin(), rsrc.end());
    return image;
}

/// A PE32+ file with one US English version block naming @p companyName.
[[nodiscard]] inline PeImage buildPeImage(std::u16string_view companyName)
{
    return buildPeImage({{0x0409U, versionInfoBlock(companyName)}});
}

} // namespace TestSupport
