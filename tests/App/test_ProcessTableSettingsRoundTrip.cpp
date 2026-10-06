/// @file test_ProcessTableSettingsRoundTrip.cpp
/// @brief Saves a table's layout from one ImGui context and restores it into a fresh one, through
/// the same ProcessTableSettings filters ProcessesPanel uses, and checks the column order ImGui
/// actually shows (#1393).
///
/// The pure tests in test_ProcessTableSettings.cpp check the text; these check what ImGui's own
/// settings loader makes of it. No window or renderer is needed: a context with a display size and a
/// renderer that claims dynamic textures runs whole frames headless.

#include "App/Panels/ProcessTableFlags.h"
#include "App/Panels/ProcessTableSettings.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace App
{
namespace
{

constexpr int COLUMN_COUNT = 32;
constexpr int SORTED_COLUMN = 8; // CPU % in the reported layout

/// One ImGui context for the life of the object, with no ini file and no backend.
class HeadlessImGui
{
  public:
    HeadlessImGui()
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1920.0F, 1080.0F);
        io.DeltaTime = 1.0F / 60.0F;
        // Lets NewFrame() run without a renderer having uploaded the font atlas.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    }

    ~HeadlessImGui()
    {
        ImGui::DestroyContext(m_Context);
    }

    HeadlessImGui(const HeadlessImGui&) = delete;
    HeadlessImGui& operator=(const HeadlessImGui&) = delete;
    HeadlessImGui(HeadlessImGui&&) = delete;
    HeadlessImGui& operator=(HeadlessImGui&&) = delete;

    /// Renders one frame of a Processes-like table: the Processes table's flags, 32 columns, and
    /// column 8 sorted descending by default. Returns the table's ID. `change`, if given, runs once
    /// the columns are set up and before the layout is locked, where a resize or reorder is allowed.
    std::uint32_t frame(const std::function<void(ImGuiTable&)>& change = {}) const
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(1900.0F, 1000.0F));
        ImGui::Begin("Processes");
        std::uint32_t tableId = 0;
        if (ImGui::BeginTable("ProcessTable", COLUMN_COUNT, ProcessTableFlags::forProcessTable(false)))
        {
            for (int column = 0; column < COLUMN_COUNT; ++column)
            {
                const ImGuiTableColumnFlags flags = (column == SORTED_COLUMN)
                                                      ? (ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending)
                                                      : ImGuiTableColumnFlags_None;
                ImGui::TableSetupColumn(std::format("Column{}", column).c_str(), flags, 60.0F);
            }
            ImGuiTable* table = ImGui::GetCurrentTable();
            if (change)
            {
                change(*table);
            }
            ImGui::TableHeadersRow();
            tableId = table->ID;
            ImGui::EndTable();
        }
        ImGui::End();
        ImGui::Render();
        return tableId;
    }

    /// The table's columns, by display position.
    [[nodiscard]] static std::vector<int> displayOrder(std::uint32_t tableId)
    {
        std::vector<int> order;
        const ImGuiTable* table = ImGui::TableFindByID(tableId);
        if (table == nullptr)
        {
            return order;
        }
        for (int position = 0; position < table->ColumnsCount; ++position)
        {
            order.push_back(table->DisplayOrderToIndex[position]);
        }
        return order;
    }

    [[nodiscard]] static const ImGuiTableColumn* column(std::uint32_t tableId, int index)
    {
        const ImGuiTable* table = ImGui::TableFindByID(tableId);
        return table == nullptr ? nullptr : &table->Columns[index];
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

/// Runs a session with the default layout -- or with `change` applied to it -- and returns the
/// table's section as captureTableLayout() stores it: extracted, then with the default order made
/// explicit. `rawOut`, if given, receives the section as extracted, before that last step.
std::string captureSession(const std::function<void(ImGuiTable&)>& change = {}, std::string* rawOut = nullptr)
{
    const HeadlessImGui imgui;
    std::uint32_t tableId = 0;
    for (int frame = 0; frame < 3; ++frame)
    {
        tableId = imgui.frame((frame == 1) ? change : std::function<void(ImGuiTable&)>{});
    }
    std::size_t size = 0;
    const char* ini = ImGui::SaveIniSettingsToMemory(&size);
    std::string raw = ProcessTableSettings::extractTableSection(std::string_view(ini, size), tableId);
    if (rawOut != nullptr)
    {
        *rawOut = raw;
    }
    return ProcessTableSettings::withExplicitOrder(raw);
}

/// The display order a fresh context shows after loading `stored` the way `prepare` hands it over.
std::vector<int> restoreSession(const std::string& stored, std::string (*prepare)(std::string_view), int& sortOrder, int& sortDirection)
{
    const HeadlessImGui imgui;
    const std::string layout = prepare(stored);
    ImGui::LoadIniSettingsFromMemory(layout.data(), layout.size());
    std::uint32_t tableId = 0;
    for (int frame = 0; frame < 3; ++frame)
    {
        tableId = imgui.frame();
    }
    const ImGuiTableColumn* sorted = HeadlessImGui::column(tableId, SORTED_COLUMN);
    sortOrder = (sorted == nullptr) ? -2 : sorted->SortOrder;
    sortDirection = (sorted == nullptr) ? -2 : static_cast<int>(sorted->SortDirection);
    return HeadlessImGui::displayOrder(tableId);
}

std::vector<int> identityOrder()
{
    std::vector<int> order;
    for (int column = 0; column < COLUMN_COUNT; ++column)
    {
        order.push_back(column);
    }
    return order;
}

std::string sanitizeOnly(std::string_view stored)
{
    return ProcessTableSettings::sanitize(stored);
}

std::string withExplicitOrder(std::string_view stored)
{
    return ProcessTableSettings::withExplicitOrder(stored);
}

// The harness reproduces the report: ImGui saves only the sorted column's line, and loading that
// as it is moves the sorted column to the front.
TEST(ProcessTableSettingsRoundTripTest, ASortOnlySectionLoadedAsItIsMovesTheSortedColumnFirst)
{
    std::string raw;
    (void) captureSession({}, &raw);
    ASSERT_FALSE(raw.empty());
    EXPECT_FALSE(raw.contains("Order=")) << raw;
    EXPECT_TRUE(raw.contains(std::format("Column {}  Sort=0^", SORTED_COLUMN))) << raw;

    int sortOrder = 0;
    int sortDirection = 0;
    const std::vector<int> order = restoreSession(raw, &sanitizeOnly, sortOrder, sortDirection);
    ASSERT_EQ(order.size(), static_cast<std::size_t>(COLUMN_COUNT));
    EXPECT_EQ(order.front(), SORTED_COLUMN);
}

// The fix, end to end: the default layout comes back in its own order, still sorted.
TEST(ProcessTableSettingsRoundTripTest, TheDefaultLayoutComesBackInItsOwnOrderWithItsSort)
{
    const std::string stored = captureSession();
    int sortOrder = -1;
    int sortDirection = -1;
    EXPECT_EQ(restoreSession(stored, &withExplicitOrder, sortOrder, sortDirection), identityOrder());
    EXPECT_EQ(sortOrder, 0);
    EXPECT_EQ(sortDirection, static_cast<int>(ImGuiSortDirection_Descending));
}

// A layout an older version saved -- the raw sort-only section -- is repaired on restore.
TEST(ProcessTableSettingsRoundTripTest, ASortOnlySectionSavedByAnOlderVersionIsRestoredInOrder)
{
    std::string raw;
    (void) captureSession({}, &raw);
    int sortOrder = -1;
    int sortDirection = -1;
    EXPECT_EQ(restoreSession(raw, &withExplicitOrder, sortOrder, sortDirection), identityOrder());
    EXPECT_EQ(sortOrder, 0);
    EXPECT_EQ(sortDirection, static_cast<int>(ImGuiSortDirection_Descending));
}

// A column the user moved stays where they put it.
TEST(ProcessTableSettingsRoundTripTest, AUserReorderIsKept)
{
    constexpr int MOVED = 5;
    const std::string stored = captureSession([](ImGuiTable& table) { ImGui::TableSetColumnDisplayOrder(&table, MOVED, 0); });
    ASSERT_TRUE(stored.contains("Order=")) << stored;

    std::vector<int> expected = identityOrder();
    expected.erase(expected.begin() + MOVED);
    expected.insert(expected.begin(), MOVED);

    int sortOrder = -1;
    int sortDirection = -1;
    EXPECT_EQ(restoreSession(stored, &withExplicitOrder, sortOrder, sortDirection), expected);
    EXPECT_EQ(sortOrder, 0);
    EXPECT_EQ(sortDirection, static_cast<int>(ImGuiSortDirection_Descending));
}

// A resized column keeps its width and the default order.
TEST(ProcessTableSettingsRoundTripTest, AResizedColumnKeepsItsWidthAndTheDefaultOrder)
{
    constexpr int RESIZED = 3;
    constexpr float WIDTH = 140.0F;
    const std::string stored = captureSession([](ImGuiTable& /*table*/) { ImGui::TableSetColumnWidth(RESIZED, WIDTH); });
    ASSERT_TRUE(stored.contains(std::format("Column {}  Width=140", RESIZED))) << stored;

    const HeadlessImGui imgui;
    const std::string layout = ProcessTableSettings::withExplicitOrder(stored);
    ImGui::LoadIniSettingsFromMemory(layout.data(), layout.size());
    std::uint32_t tableId = 0;
    for (int frame = 0; frame < 3; ++frame)
    {
        tableId = imgui.frame();
    }
    EXPECT_EQ(HeadlessImGui::displayOrder(tableId), identityOrder());
    const ImGuiTableColumn* resized = HeadlessImGui::column(tableId, RESIZED);
    ASSERT_NE(resized, nullptr);
    EXPECT_FLOAT_EQ(resized->WidthRequest, WIDTH);
}

} // namespace
} // namespace App
