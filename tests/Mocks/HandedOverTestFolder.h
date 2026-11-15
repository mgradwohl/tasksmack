#pragma once

// A handed-over config folder of a test's own (#3774, #3787), for UserConfig and the instance lock on every platform:
// it reads and writes by path in @p path, as PinnedFolder does through the held handle, and refuses what PinnedFolder
// refuses there: a link in place of a file, and a folder that is no longer the plain folder it was given. Counts
// each call, so a test can tell the hand-off went through it.

#include "Platform/IHandedOverFolder.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace TestSupport
{

class HandedOverTestFolder final : public Platform::IHandedOverFolder
{
  public:
    explicit HandedOverTestFolder(std::filesystem::path path) : m_Path(std::move(path))
    {}

    [[nodiscard]] const std::filesystem::path& path() const noexcept override
    {
        return m_Path;
    }

    [[nodiscard]] std::expected<void*, std::string> openLockFile(const std::filesystem::path& /*name*/) const override
    {
        ++m_Calls;
        return std::unexpected(std::string("a test folder holds no lock"));
    }

    [[nodiscard]] std::expected<std::optional<std::string>, std::string> readFile(const std::filesystem::path& relative) const override
    {
        ++m_Calls;
        if (auto held = stillHeld(); !held.has_value())
        {
            return std::unexpected(std::move(held.error()));
        }
        std::error_code ec;
        const std::filesystem::path file = m_Path / relative;
        const std::filesystem::file_status status = std::filesystem::symlink_status(file, ec);
        if (status.type() == std::filesystem::file_type::not_found)
        {
            return std::optional<std::string>{};
        }
        if (ec || status.type() != std::filesystem::file_type::regular)
        {
            return std::unexpected(file.string() + " is a link, or no plain file");
        }
        std::ifstream in(file, std::ios::binary);
        return std::optional<std::string>(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
    }

    [[nodiscard]] std::expected<void, std::string> replaceFile(const std::filesystem::path& name, std::string_view contents) const override
    {
        ++m_Calls;
        if (auto held = stillHeld(); !held.has_value())
        {
            return std::unexpected(std::move(held.error()));
        }
        if (m_DuringReplace)
        {
            const std::function<void()> during = m_DuringReplace; // a copy: it may let go of this folder's last owner
            during();
        }
        const std::filesystem::path staging = m_Path / ".tasksmack-config.test.tmp";
        {
            std::ofstream out(staging, std::ios::binary | std::ios::trunc);
            out << contents;
            if (!out)
            {
                return std::unexpected(std::string("couldn't write ") + staging.string());
            }
        }
        std::error_code ec;
        std::filesystem::rename(staging, m_Path / name, ec); // over the name itself, never through a link there
        if (ec)
        {
            std::filesystem::remove(staging, ec);
            return std::unexpected(std::string("couldn't replace ") + (m_Path / name).string());
        }
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> createFolder(const std::filesystem::path& name) const override
    {
        ++m_Calls;
        std::error_code ec;
        std::filesystem::create_directory(m_Path / name, ec);
        return ec ? std::expected<void, std::string>(std::unexpected(ec.message())) : std::expected<void, std::string>{};
    }

    [[nodiscard]] std::expected<std::vector<std::filesystem::path>, std::string> listFiles(const std::filesystem::path& name) const override
    {
        ++m_Calls;
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(m_Path / name, ec), end; !ec && it != end; it.increment(ec))
        {
            if (it->symlink_status(ec).type() == std::filesystem::file_type::regular)
            {
                files.push_back(it->path().filename());
            }
        }
        return files;
    }

    /// Runs @p during in each replaceFile(), before it writes: what a test does mid-save.
    void setDuringReplace(std::function<void()> during)
    {
        m_DuringReplace = std::move(during);
    }

    /// How many times the folder was asked for anything.
    [[nodiscard]] std::uint32_t calls() const noexcept
    {
        return m_Calls;
    }

  private:
    [[nodiscard]] std::expected<void, std::string> stillHeld() const
    {
        std::error_code ec;
        if (std::filesystem::symlink_status(m_Path, ec).type() != std::filesystem::file_type::directory)
        {
            return std::unexpected(m_Path.string() + " is no longer the folder it was");
        }
        return {};
    }

    std::filesystem::path m_Path;
    mutable std::uint32_t m_Calls = 0;
    std::function<void()> m_DuringReplace;
};

} // namespace TestSupport
