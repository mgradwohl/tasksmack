/// @file test_DesktopEntryIcons.cpp
/// @brief Finding a Linux process's icon through desktop entries and the hicolor theme (#1800):
/// parsing [Desktop Entry], the program an Exec line runs (through env and flatpak, never keyed on an
/// interpreter), the index's precedence and keys, and the icon file lookup by size against a temporary
/// directory tree. Pure std::filesystem, so it runs on every platform.

#include "Platform/Linux/DesktopEntryIcons.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform::DesktopEntryIcons
{
namespace
{

/// A fresh temporary directory, removed with everything in it.
class TempTree
{
  public:
    TempTree()
    {
        std::random_device random;
        const std::filesystem::path base = std::filesystem::temp_directory_path();
        int attempt = 0;
        while (m_Root.empty() && attempt < 100)
        {
            std::filesystem::path candidate = base / ("tasksmack_desktop_icons_" + std::to_string(random()));
            std::error_code ec;
            if (std::filesystem::create_directory(candidate, ec) && !ec)
            {
                m_Root = std::move(candidate);
            }
            ++attempt;
        }
    }

    ~TempTree()
    {
        try
        {
            std::error_code ec;
            std::filesystem::remove_all(m_Root, ec);
        }
        catch (...) // NOLINT(bugprone-empty-catch): best-effort cleanup; a destructor must not throw
        {}
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;
    TempTree(TempTree&&) = delete;
    TempTree& operator=(TempTree&&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept
    {
        return m_Root;
    }

    /// Writes @p text to @p relative under the root, making its directories.
    [[nodiscard]] std::filesystem::path write(const std::filesystem::path& relative, std::string_view text) const
    {
        const std::filesystem::path path = m_Root / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out << text;
        return path;
    }

  private:
    std::filesystem::path m_Root;
};

[[nodiscard]] std::string entry(std::string_view exec, std::string_view icon, std::string_view extra = {})
{
    return std::string("[Desktop Entry]\nType=Application\nName=App\nExec=") + std::string(exec) + "\nIcon=" + std::string(icon) + "\n" +
           std::string(extra);
}

TEST(DesktopEntryIconsTest, ParsesTheDesktopEntryGroupOnly)
{
    const auto parsed = parseDesktopEntry("# comment\n[Desktop Entry]\nName=Files\nName[de]=Dateien\nExec = nautilus --new-window %U\n"
                                          "Icon=org.gnome.Nautilus\nStartupWMClass=Org.gnome.Nautilus\nTryExec=nautilus\n\n"
                                          "[Desktop Action new-window]\nExec=other\nIcon=wrong\n");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->icon, "org.gnome.Nautilus");
    EXPECT_EQ(parsed->exec, "nautilus --new-window %U");
    EXPECT_EQ(parsed->startupWmClass, "Org.gnome.Nautilus");
    EXPECT_EQ(parsed->tryExec, "nautilus");
    EXPECT_TRUE(parsed->application);
    EXPECT_FALSE(parsed->hidden);
}

TEST(DesktopEntryIconsTest, AnEntryWithoutAnIconOrAGroupIsNone)
{
    EXPECT_FALSE(parseDesktopEntry("[Desktop Entry]\nExec=app\n").has_value());
    EXPECT_FALSE(parseDesktopEntry("Exec=app\nIcon=app\n").has_value());
    EXPECT_FALSE(parseDesktopEntry("").has_value());
}

TEST(DesktopEntryIconsTest, HiddenAndNonApplicationEntriesAreMarked)
{
    const auto hidden = parseDesktopEntry("[Desktop Entry]\nIcon=a\nHidden=true\r\n");
    ASSERT_TRUE(hidden.has_value());
    EXPECT_TRUE(hidden->hidden);
    const auto link = parseDesktopEntry("[Desktop Entry]\nIcon=a\nType=Link\n");
    ASSERT_TRUE(link.has_value());
    EXPECT_FALSE(link->application);
}

TEST(DesktopEntryIconsTest, TheProgramAnExecRuns)
{
    EXPECT_EQ(execProgramKeys("/usr/bin/Firefox %u"), (std::vector<std::string>{"firefox"}));
    EXPECT_EQ(execProgramKeys("\"/opt/My App/app\" --flag"), (std::vector<std::string>{"app"}));
    EXPECT_EQ(execProgramKeys("env GDK_BACKEND=x11 FOO=1 /usr/bin/code --unity-launch %F"), (std::vector<std::string>{"code"}));
    EXPECT_EQ(execProgramKeys("/usr/bin/flatpak run --branch=stable --arch=x86_64 --command=spotify com.spotify.Client"),
              (std::vector<std::string>{"spotify"}));
    EXPECT_TRUE(execProgramKeys("flatpak run org.example.App").empty()); // The app ID is the desktop ID
    EXPECT_TRUE(execProgramKeys("python3 /usr/share/app/app.py").empty());
    EXPECT_TRUE(execProgramKeys("sh -c \"exec app\"").empty());
    EXPECT_TRUE(execProgramKeys("").empty());
}

TEST(DesktopEntryIconsTest, TheIndexFindsAProcessByExecutableThenName)
{
    DesktopIndex index;
    index.add(*parseDesktopEntry(entry("/usr/lib/firefox/firefox %u", "firefox")), "firefox");
    index.add(*parseDesktopEntry(entry("code %F", "vscode", "StartupWMClass=Code\n")), "code");
    index.add(*parseDesktopEntry(entry("nautilus", "org.gnome.Nautilus")), "org.gnome.Nautilus");

    EXPECT_EQ(index.iconFor("/usr/lib/firefox/firefox", "Web Content"), "firefox");
    EXPECT_EQ(index.iconFor("", "code"), "vscode");                 // By name: unreadable exe
    EXPECT_EQ(index.iconFor("/usr/share/code/Code", ""), "vscode"); // By window class, case-folded
    EXPECT_EQ(index.iconFor("/usr/bin/NAUTILUS", ""), "org.gnome.Nautilus");
    EXPECT_FALSE(index.iconFor("/usr/sbin/sshd", "sshd").has_value());
}

TEST(DesktopEntryIconsTest, TheIndexKeysTheDesktopIdAndItsLastPart)
{
    DesktopIndex index;
    index.add(*parseDesktopEntry(entry("flatpak run org.mozilla.Thunderbird", "org.mozilla.Thunderbird")), "org.mozilla.Thunderbird");
    EXPECT_EQ(index.iconFor("/app/lib/thunderbird/thunderbird", "thunderbird"), "org.mozilla.Thunderbird");
    EXPECT_EQ(index.iconFor("", "org.mozilla.thunderbird"), "org.mozilla.Thunderbird");
}

TEST(DesktopEntryIconsTest, TheIndexSkipsHiddenEntriesAndKeepsTheFirstOfAKey)
{
    DesktopIndex index;
    index.add(*parseDesktopEntry(entry("app", "user-icon")), "app");
    index.add(*parseDesktopEntry(entry("app", "system-icon")), "app");
    index.add(*parseDesktopEntry(entry("ghost", "ghost", "Hidden=true\n")), "ghost");
    EXPECT_EQ(index.iconFor("/usr/bin/app", ""), "user-icon");
    EXPECT_FALSE(index.iconFor("/usr/bin/ghost", "ghost").has_value());
}

TEST(DesktopEntryIconsTest, DirectoriesAreReadInPrecedenceOrderWithSubdirectoryIds)
{
    const TempTree tree;
    ASSERT_FALSE(tree.root().empty());
    static_cast<void>(tree.write("user/applications/app.desktop", entry("app", "user-icon")));
    static_cast<void>(tree.write("system/applications/app.desktop", entry("app", "system-icon")));
    static_cast<void>(tree.write("system/applications/kde/kate.desktop", entry("kate -b %U", "kate")));
    static_cast<void>(tree.write("system/applications/readme.txt", entry("notes", "notes")));
    static_cast<void>(tree.write("system/applications/broken.desktop", "[Desktop Entry]\nExec=broken\n"));

    DesktopIndex index;
    const std::vector<std::filesystem::path> dirs{
        tree.root() / "user" / "applications", tree.root() / "system" / "applications", tree.root() / "missing" / "applications"};
    index.addDirectories(dirs);
    EXPECT_EQ(index.iconFor("/usr/bin/app", ""), "user-icon");
    EXPECT_EQ(index.iconFor("/usr/bin/kate", ""), "kate");
    EXPECT_EQ(index.iconFor("", "kde-kate"), "kate"); // The ID joins the subdirectory with '-'
    EXPECT_FALSE(index.iconFor("/usr/bin/notes", "").has_value());
    EXPECT_FALSE(index.iconFor("/usr/bin/broken", "").has_value());
}

TEST(DesktopEntryIconsTest, SizesAreTriedNearestLargerFirstThenLargestSmaller)
{
    EXPECT_EQ(sizesToTry(20), (std::vector<int>{22, 24, 32, 36, 48, 64, 72, 96, 128, 256, 16}));
    EXPECT_EQ(sizesToTry(16).front(), 16);
    EXPECT_EQ(sizesToTry(300), (std::vector<int>{256, 128, 96, 72, 64, 48, 36, 32, 24, 22, 16}));
}

TEST(DesktopEntryIconsTest, FindsTheThemeIconNearestTheSizeThenAPixmap)
{
    const TempTree tree;
    ASSERT_FALSE(tree.root().empty());
    const std::filesystem::path user = tree.root() / "user" / "icons";
    const std::filesystem::path system = tree.root() / "system" / "icons";
    const std::filesystem::path pixmaps = tree.root() / "pixmaps";
    static_cast<void>(tree.write("system/icons/hicolor/48x48/apps/app.png", "png"));
    const std::filesystem::path wanted = tree.write("system/icons/hicolor/24x24/apps/app.png", "png");
    static_cast<void>(tree.write("system/icons/hicolor/16x16/apps/app.png", "png"));
    static_cast<void>(tree.write("system/icons/hicolor/scalable/apps/vector.svg", "svg"));
    const std::filesystem::path pixmap = tree.write("pixmaps/old.png", "png");
    const std::filesystem::path userIcon = tree.write("user/icons/hicolor/32x32/apps/mine.png", "png");
    static_cast<void>(tree.write("system/icons/hicolor/32x32/apps/mine.png", "png"));

    const std::vector<std::filesystem::path> bases{user, system};
    const std::vector<std::filesystem::path> pixmapDirs{pixmaps};
    EXPECT_EQ(findIconFile("app", 20, bases, pixmapDirs), wanted);     // 24 is the nearest at or above 20
    EXPECT_EQ(findIconFile("app.png", 20, bases, pixmapDirs), wanted); // A name with its extension
    EXPECT_EQ(findIconFile("old", 16, bases, pixmapDirs), pixmap);
    EXPECT_EQ(findIconFile("mine", 16, bases, pixmapDirs), userIcon);        // The user's directory first
    EXPECT_FALSE(findIconFile("vector", 16, bases, pixmapDirs).has_value()); // SVG only: no decoder
    EXPECT_FALSE(findIconFile("missing", 16, bases, pixmapDirs).has_value());
    EXPECT_FALSE(findIconFile("", 16, bases, pixmapDirs).has_value());
    EXPECT_FALSE(findIconFile("sub/app", 16, bases, pixmapDirs).has_value());
}

#ifndef _WIN32
TEST(DesktopEntryIconsTest, AnAbsoluteIconPathIsUsedWhenItIsAPng)
{
    const TempTree tree;
    ASSERT_FALSE(tree.root().empty());
    const std::filesystem::path png = tree.write("opt/app/icon.png", "png");
    const std::filesystem::path svg = tree.write("opt/app/icon.svg", "svg");
    EXPECT_EQ(findIconFile(png.string(), 16, {}, {}), png);
    EXPECT_FALSE(findIconFile(svg.string(), 16, {}, {}).has_value());
    EXPECT_FALSE(findIconFile((tree.root() / "nothing.png").string(), 16, {}, {}).has_value());
}
#endif

TEST(DesktopEntryIconsTest, SearchDirsFollowXdgThenFlatpakAndSnap)
{
    const SearchDirs dirs = searchDirs("/home/u", "", "");
    const std::vector<std::filesystem::path> expectedApps{"/home/u/.local/share/applications",
                                                          "/home/u/.local/share/flatpak/exports/share/applications",
                                                          "/usr/local/share/applications",
                                                          "/usr/share/applications",
                                                          "/var/lib/flatpak/exports/share/applications",
                                                          "/var/lib/snapd/desktop/applications"};
    ASSERT_EQ(dirs.applications.size(), expectedApps.size());
    std::size_t i = 0;
    while (i < expectedApps.size())
    {
        EXPECT_EQ(dirs.applications[i].generic_string(), expectedApps[i].generic_string());
        ++i;
    }
    ASSERT_FALSE(dirs.iconBases.empty());
    EXPECT_EQ(dirs.iconBases.front().generic_string(), "/home/u/.icons");
    EXPECT_EQ(dirs.pixmaps.front().generic_string(), "/usr/share/pixmaps");

    const SearchDirs custom = searchDirs("", "/data/home", "/a:/b::");
    EXPECT_EQ(custom.applications[0].generic_string(), "/data/home/applications");
    EXPECT_EQ(custom.applications[1].generic_string(), "/a/applications");
    EXPECT_EQ(custom.applications[2].generic_string(), "/b/applications");
}

} // namespace
} // namespace Platform::DesktopEntryIcons
