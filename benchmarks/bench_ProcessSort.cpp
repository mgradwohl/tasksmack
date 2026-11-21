// Cost of sorting the Processes table by a text column (#4509).
//
// The Name, Command and User columns compare by Unicode simple case folding then bytes
// (Platform::compareFoldedCase, #4324), not raw bytes. ASCII text takes the ASCII compare; text with non-ASCII
// letters is decoded on every comparison. A sort is O(n log n) comparisons of two keys, so this measures 2,000 rows
// (a busy machine) by Name and by Command, the long one, for ASCII-only text and for text where every row holds a
// non-ASCII letter (a user profile path in the command line). It runs on a sort change or an adopted snapshot,
// not per frame.

#include "App/Panels/ProcessSortUtils.h"
#include "App/ProcessColumnConfig.h"
#include "Domain/ProcessSnapshot.h"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

namespace
{

/// @p count processes with names and long command lines; with @p nonAscii each one also holds non-ASCII letters,
/// as a path under C:\Users\Émile does. Names repeat in case-only variants ("svchost.exe", "SvcHost.exe"), as on a
/// real machine, so the folded compare has to look past the first letters.
[[nodiscard]] std::vector<Domain::ProcessSnapshot> makeRows(std::size_t count, bool nonAscii)
{
    static constexpr std::array<std::string_view, 6> NAMES{"svchost.exe", "SvcHost.exe", "chrome.exe", "Code.exe", "bash", "Bash"};
    std::vector<Domain::ProcessSnapshot> rows(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        Domain::ProcessSnapshot& row = rows[i];
        row.pid = static_cast<std::int32_t>(i + 1);
        const std::string_view base = NAMES[i % NAMES.size()];
        row.name = nonAscii ? std::format("{}-\xC3\xA9{}", base, i % 97) : std::format("{}-e{}", base, i % 97);
        const std::string_view user = nonAscii ? "\xC3\x89mile" : "Emile";
        row.command = std::format(
            R"("C:\Users\{}\AppData\Local\Programs\App{}\{}" --type=renderer --lang=en-US --field-trial-handle={},i,{} --mojo-platform-channel-handle={})",
            user,
            i % 13,
            base,
            i * 7919 % 100003,
            i % 31,
            i);
        row.user = std::string(user);
    }
    return rows;
}

/// How the sort compares: Bytes is the raw byte compare the table used before #4509 (kept here as the baseline);
/// Folded is the panel's sortByColumn(), each row's key folded once per sort.
enum class Way : std::uint8_t
{
    Bytes,
    Folded,
};

void runSort(benchmark::State& state, App::ProcessColumn column, bool nonAscii, Way way)
{
    const auto count = static_cast<std::size_t>(state.range(0));
    const std::vector<Domain::ProcessSnapshot> rows = makeRows(count, nonAscii);
    const auto text = [column](const Domain::ProcessSnapshot& row) -> const std::string&
    {
        return column == App::ProcessColumn::Name ? row.name : row.command;
    };
    std::vector<std::size_t> order(count);
    for (auto _ : state)
    {
        std::ranges::iota(order, std::size_t{0});
        if (way == Way::Folded)
        {
            App::ProcessSortUtils::sortByColumn(rows, order, column, /*ascending=*/true);
        }
        else
        {
            std::ranges::sort(order,
                              [&rows, &text](std::size_t a, std::size_t b)
                              {
                                  const std::strong_ordering byText = text(rows[a]) <=> text(rows[b]);
                                  return byText != 0 ? byText < 0 : rows[a].pid < rows[b].pid;
                              });
        }
        benchmark::DoNotOptimize(order.data());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(count));
}

void BM_ProcessSort_Name_Ascii_Bytes(benchmark::State& state)
{
    runSort(state, App::ProcessColumn::Name, /*nonAscii=*/false, Way::Bytes);
}
void BM_ProcessSort_Name_Ascii_Folded(benchmark::State& state)
{
    runSort(state, App::ProcessColumn::Name, /*nonAscii=*/false, Way::Folded);
}
void BM_ProcessSort_Name_NonAscii_Bytes(benchmark::State& state)
{
    runSort(state, App::ProcessColumn::Name, /*nonAscii=*/true, Way::Bytes);
}
void BM_ProcessSort_Name_NonAscii_Folded(benchmark::State& state)
{
    runSort(state, App::ProcessColumn::Name, /*nonAscii=*/true, Way::Folded);
}
void BM_ProcessSort_Command_Ascii_Bytes(benchmark::State& state)
{
    runSort(state, App::ProcessColumn::Command, /*nonAscii=*/false, Way::Bytes);
}
void BM_ProcessSort_Command_Ascii_Folded(benchmark::State& state)
{
    runSort(state, App::ProcessColumn::Command, /*nonAscii=*/false, Way::Folded);
}
void BM_ProcessSort_Command_NonAscii_Bytes(benchmark::State& state)
{
    runSort(state, App::ProcessColumn::Command, /*nonAscii=*/true, Way::Bytes);
}
void BM_ProcessSort_Command_NonAscii_Folded(benchmark::State& state)
{
    runSort(state, App::ProcessColumn::Command, /*nonAscii=*/true, Way::Folded);
}

} // namespace

BENCHMARK(BM_ProcessSort_Name_Ascii_Bytes)->Arg(500)->Arg(2000)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_ProcessSort_Name_Ascii_Folded)->Arg(500)->Arg(2000)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_ProcessSort_Name_NonAscii_Bytes)->Arg(500)->Arg(2000)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_ProcessSort_Name_NonAscii_Folded)->Arg(500)->Arg(2000)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_ProcessSort_Command_Ascii_Bytes)->Arg(500)->Arg(2000)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_ProcessSort_Command_Ascii_Folded)->Arg(500)->Arg(2000)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_ProcessSort_Command_NonAscii_Bytes)->Arg(500)->Arg(2000)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_ProcessSort_Command_NonAscii_Folded)->Arg(500)->Arg(2000)->Unit(benchmark::kMicrosecond);
