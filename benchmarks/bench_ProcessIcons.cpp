// Headless cost of the process icons beside the Processes table's names (#1800).
//
// Each iteration builds and renders one whole ImGui frame (NewFrame() through Render()) of a table
// shaped like the Processes table's PID and Name columns: 1000 processes behind an ImGuiListClipper.
// The IconsOff/IconsOn pair isolates what an icon adds per visible row in the steady state -- every
// icon loaded, every texture made (a fake texture backend; there is no GL here) -- one cache lookup
// and one texture lookup and an AddImage. IconsGeneric is the same with no program having an icon:
// the generic glyph in every row.
//
// CacheMemory fills the Domain cache with 1000 processes running 200 distinct executables (a busy
// desktop has 150-300) and reports what it holds: the icons' pixel bytes and an estimate of the maps'
// own overhead.

#include "App/Panels/ProcessIconView.h"
#include "ChartGeometryScenes.h" // HeadlessChartContext
#include "Domain/ProcessIconCache.h"
#include "Domain/ProcessSnapshot.h"
#include "Platform/IProcessIconSource.h"
#include "UI/IconTextureCache.h"

#include <benchmark/benchmark.h>
#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

constexpr int PROCESS_COUNT = 1000;
constexpr int EXECUTABLE_COUNT = 200;

/// Process PID p runs executable p % EXECUTABLE_COUNT; every executable has an icon, or none does.
class BenchIconSource final : public Platform::IProcessIconSource
{
  public:
    explicit BenchIconSource(bool icons) : m_Icons(icons)
    {}

    [[nodiscard]] std::string executablePath(std::int32_t pid) override
    {
        return std::format("C:\\Program Files\\App{}\\app.exe", pid % EXECUTABLE_COUNT);
    }

    [[nodiscard]] std::optional<Platform::IconPixels>
    loadIcon(std::string_view /*executablePath*/, std::string_view /*processName*/, int sizePx) override
    {
        if (!m_Icons)
        {
            return std::nullopt;
        }
        const auto side = static_cast<std::size_t>(sizePx);
        return Platform::IconPixels{.width = sizePx, .height = sizePx, .rgba = std::vector<std::uint8_t>(side * side * 4U, 200U)};
    }

    [[nodiscard]] bool caseInsensitivePaths() const noexcept override
    {
        return true;
    }

  private:
    bool m_Icons;
};

[[nodiscard]] UI::IconTextureBackend fakeTextures()
{
    auto next = std::make_shared<std::uint32_t>(1U);
    return UI::IconTextureBackend{
        .create = [next](int /*width*/, int /*height*/, std::span<const std::uint8_t> /*rgba*/) -> std::uint32_t { return (*next)++; },
        .destroy = [](std::uint32_t /*texture*/) {},
    };
}

std::vector<Domain::ProcessSnapshot> makeSnapshots()
{
    std::vector<Domain::ProcessSnapshot> snapshots(PROCESS_COUNT);
    for (std::size_t i = 0; i < snapshots.size(); ++i)
    {
        auto& proc = snapshots[i];
        proc.pid = static_cast<std::int32_t>(i + 1);
        proc.uniqueKey = (i + 1) * 7919U;
        proc.name = std::format("process{}.exe", i % EXECUTABLE_COUNT);
    }
    return snapshots;
}

/// One frame of the table; returns the frame's vertex count.
int renderTableFrame(const std::vector<Domain::ProcessSnapshot>& snapshots, App::ProcessIconView* icons)
{
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(ImVec2(1600.0F, 1000.0F));
    ImGui::Begin("Processes", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    if (icons != nullptr)
    {
        icons->beginFrame();
    }
    if (ImGui::BeginTable("##processes", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg))
    {
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 60.0F);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 200.0F);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(snapshots.size()));
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const Domain::ProcessSnapshot& proc = snapshots[static_cast<std::size_t>(i)];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted("12345");
                ImGui::TableSetColumnIndex(1);
                if (icons != nullptr)
                {
                    icons->drawInline(proc);
                }
                ImGui::TextUnformatted(proc.name.c_str());
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
    ImGui::Render();
    return ImGui::GetDrawData()->TotalVtxCount;
}

void runTable(benchmark::State& state, std::optional<bool> programsHaveIcons)
{
    const ChartGeometry::HeadlessChartContext context;
    const auto snapshots = makeSnapshots();
    std::unique_ptr<App::ProcessIconView> icons;
    if (programsHaveIcons.has_value())
    {
        icons = std::make_unique<App::ProcessIconView>(fakeTextures(), Domain::ProcessIconCacheConfig{.startWorker = false});
        icons->attach(std::make_unique<BenchIconSource>(*programsHaveIcons));
    }
    // Let the clipper measure the rows, then load every visible row's icon and make its texture, so
    // the loop below is the steady state.
    int vertices = renderTableFrame(snapshots, icons.get());
    if (icons != nullptr)
    {
        static_cast<void>(icons->cache()->loadPending());
        vertices = renderTableFrame(snapshots, icons.get());
    }
    for (auto _ : state)
    {
        vertices = renderTableFrame(snapshots, icons.get());
        benchmark::DoNotOptimize(vertices);
    }
    state.counters["vertices"] = benchmark::Counter(static_cast<double>(vertices));
    if (icons != nullptr)
    {
        state.counters["loads"] = benchmark::Counter(static_cast<double>(icons->cache()->stats().loads));
    }
}

void BM_ProcessIcons_TableFrame_IconsOff(benchmark::State& state)
{
    runTable(state, std::nullopt);
}
BENCHMARK(BM_ProcessIcons_TableFrame_IconsOff)->Unit(benchmark::kMicrosecond);

void BM_ProcessIcons_TableFrame_IconsOn(benchmark::State& state)
{
    runTable(state, true);
}
BENCHMARK(BM_ProcessIcons_TableFrame_IconsOn)->Unit(benchmark::kMicrosecond);

void BM_ProcessIcons_TableFrame_IconsGeneric(benchmark::State& state)
{
    runTable(state, false);
}
BENCHMARK(BM_ProcessIcons_TableFrame_IconsGeneric)->Unit(benchmark::kMicrosecond);

// The cache after every process has been looked up once and loaded, at state.range(0) pixels: its
// icons' pixel bytes, and an estimate of the maps' overhead (a list node and a hash node per entry,
// plus each key string and each queued-free process entry).
void BM_ProcessIcons_CacheMemory(benchmark::State& state)
{
    const auto sizePx = static_cast<int>(state.range(0));
    const auto snapshots = makeSnapshots();
    Domain::ProcessIconCache::Stats stats;
    for (auto _ : state)
    {
        Domain::ProcessIconCache cache(std::make_unique<BenchIconSource>(true), Domain::ProcessIconCacheConfig{.startWorker = false});
        // The load queue holds 256 requests, so a first sight of 1000 processes takes a few passes,
        // as scrolling through them would.
        int pass = 0;
        do
        {
            for (const Domain::ProcessSnapshot& proc : snapshots)
            {
                static_cast<void>(cache.lookup(proc.uniqueKey, proc.pid, proc.name, sizePx));
            }
            ++pass;
        } while (cache.loadPending() > 0 && pass < 16);
        stats = cache.stats();
        benchmark::DoNotOptimize(stats.icons);
    }
    // Per process entry: list node (key + size + shared_ptr) and hash node, about 96 bytes. Per icon:
    // list and hash nodes with a ~48-character key, the shared image's control block and vector,
    // about 224 bytes beside its pixels.
    constexpr double PROCESS_ENTRY_BYTES = 96.0;
    constexpr double ICON_ENTRY_BYTES = 224.0;
    const double overhead =
        (static_cast<double>(stats.processes) * PROCESS_ENTRY_BYTES) + (static_cast<double>(stats.icons) * ICON_ENTRY_BYTES);
    state.counters["processes"] = benchmark::Counter(static_cast<double>(stats.processes));
    state.counters["icons"] = benchmark::Counter(static_cast<double>(stats.icons));
    state.counters["pixel_bytes"] = benchmark::Counter(static_cast<double>(stats.pixelBytes));
    state.counters["total_bytes_est"] = benchmark::Counter(static_cast<double>(stats.pixelBytes) + overhead);
}
BENCHMARK(BM_ProcessIcons_CacheMemory)->Arg(16)->Arg(24)->Arg(32)->Unit(benchmark::kMillisecond);

} // namespace
