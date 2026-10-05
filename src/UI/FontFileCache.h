#pragma once

// Font files read into memory once and shared by every font built from them (#1170).
//
// UILayer::loadAllFonts() adds each face once per Font Size preset and size, so the same few TTF
// files back about thirty ImFontConfig sources. AddFontFromFileTTF() read the whole file again for
// each of them -- Inter twelve times, Font Awesome thirteen, the monospace face six -- at startup and
// again on every display-scale change. The fonts are now added from memory with
// FontDataOwnedByAtlas = false, all pointing at one buffer per file kept here.
//
// ImGui 1.92 keeps reading a font's data for as long as the atlas holds the font (glyphs are baked
// lazily), so the buffers handed out must outlive every atlas that uses them: there is no way to
// drop an entry, and the cache must be destroyed only after the atlas is. The file reader is
// injectable so the caching itself can be unit-tested without touching the file system.

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <map>
#include <optional>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace UI
{

/// Read the whole of @p path into memory. std::nullopt when it cannot be opened or read, or is empty.
[[nodiscard]] inline auto readFontFile(const std::filesystem::path& path) -> std::optional<std::vector<std::byte>>
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0)
    {
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return std::nullopt;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - istream::read takes char*
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file || file.gcount() != static_cast<std::streamsize>(bytes.size()))
    {
        return std::nullopt;
    }
    return bytes;
}

/// Each font file's bytes, read on first use and kept for the cache's lifetime.
class FontFileCache
{
  public:
    using Reader = std::function<std::optional<std::vector<std::byte>>(const std::filesystem::path&)>;

    FontFileCache() : m_Reader(readFontFile)
    {}

    explicit FontFileCache(Reader reader) : m_Reader(std::move(reader))
    {}

    /// The bytes of @p path, read the first time it is asked for and the same buffer every time
    /// after. Empty when the file cannot be read; a failure is not remembered, so a later call
    /// (the next display-scale rebuild) tries the file again.
    [[nodiscard]] auto get(const std::filesystem::path& path) -> std::span<std::byte>
    {
        if (const auto found = m_Files.find(path); found != m_Files.end())
        {
            return found->second;
        }
        auto bytes = m_Reader ? m_Reader(path) : std::nullopt;
        if (!bytes.has_value() || bytes->empty())
        {
            return {};
        }
        // std::map never moves its nodes, so the span stays valid as other files are added.
        return m_Files.emplace(path, std::move(*bytes)).first->second;
    }

    /// Number of files held.
    [[nodiscard]] auto fileCount() const noexcept -> std::size_t
    {
        return m_Files.size();
    }

  private:
    Reader m_Reader;
    std::map<std::filesystem::path, std::vector<std::byte>> m_Files;
};

} // namespace UI
