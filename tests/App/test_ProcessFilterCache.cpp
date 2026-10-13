/// @file test_ProcessFilterCache.cpp
/// @brief Tests for App::ProcessFilterCache::isStale(): the Processes table's filtered indices are
/// keyed on the snapshot generation actually adopted (#1394)

#include "App/Panels/ProcessFilterCache.h"
#include "Domain/ProcessModel.h"
#include "Domain/ProcessSnapshot.h"
#include "Mocks/MockProbes.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using App::ProcessFilterCache::isStale;
using TestMocks::makeProcessCounters;
using TestMocks::MockProcessProbe;

TEST(ProcessFilterCacheTest, SameGenerationAndSearchIsFresh)
{
    EXPECT_FALSE(isStale(7, 7, "bash", "bash"));
    EXPECT_FALSE(isStale(7, 7, "", ""));
}

TEST(ProcessFilterCacheTest, NewGenerationOrSearchIsStale)
{
    EXPECT_TRUE(isStale(8, 7, "bash", "bash"));
    EXPECT_TRUE(isStale(7, 7, "bas", "bash"));
    EXPECT_TRUE(isStale(7, 8, "", "")); // Any mismatch, not only a newer generation
}

TEST(ProcessFilterCacheTest, AGenerationPublishedBeforeAdoptionMakesTheIndicesStale)
{
    // The race in #1394: renderContent() read snapshotVersion(), a new generation was published,
    // and only then were the snapshots adopted. Keyed on the version read first, the filter cache
    // looked fresh while the adopted vector had shrunk under its indices.
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    rawProbe->setCounters({makeProcessCounters(100, "a", 'S', 1000, 0, 1000),
                           makeProcessCounters(200, "b", 'S', 1000, 0, 1000),
                           makeProcessCounters(300, "c", 'S', 1000, 0, 1000)});
    rawProbe->setTotalCpuTime(100000);
    Domain::ProcessModel model(std::move(probe));
    model.refresh();

    // Adopt the first generation and build "indices" for it, as the panel does.
    std::shared_ptr<const std::vector<Domain::ProcessSnapshot>> adopted;
    std::uint64_t adoptedVersion = 0;
    ASSERT_TRUE(model.tryCopySnapshotsIfNewer(0, adopted, adoptedVersion));
    ASSERT_EQ(adopted->size(), 3U);
    const std::uint64_t filterVersion = adoptedVersion;
    const std::vector<std::size_t> filteredIndices{0, 1, 2};

    // Next frame: the version is read, then a shorter generation lands before adoption.
    const std::uint64_t versionReadBeforeAdopting = model.snapshotVersion();
    rawProbe->setCounters({makeProcessCounters(100, "a", 'S', 2000, 0, 1000)});
    rawProbe->setTotalCpuTime(200000);
    model.refresh();
    ASSERT_TRUE(model.tryCopySnapshotsIfNewer(adoptedVersion, adopted, adoptedVersion));
    ASSERT_EQ(adopted->size(), 1U);

    // The old key says the indices still fit, though two of them are now out of range...
    EXPECT_FALSE(isStale(versionReadBeforeAdopting, filterVersion, "", ""));
    EXPECT_GE(filteredIndices.back(), adopted->size());
    // ...the adopted generation says rebuild.
    EXPECT_TRUE(isStale(adoptedVersion, filterVersion, "", ""));
}

// The filter's match (#838): the name or the command line, ASCII case aside, never allocating.
static_assert(App::ProcessFilterCache::containsIgnoreCase("Microsoft Edge", "EDGE"));
static_assert(App::ProcessFilterCache::matches("msedge.exe", R"("C:\Program Files\msedge.exe" --type=renderer)", "renderer"));

TEST(ProcessFilterMatchTest, ContainsIgnoresAsciiCaseAnywhereInTheText)
{
    using App::ProcessFilterCache::containsIgnoreCase;
    EXPECT_TRUE(containsIgnoreCase("rust-analyzer", "Analyzer"));
    EXPECT_TRUE(containsIgnoreCase("rust-analyzer", "RUST"));
    EXPECT_TRUE(containsIgnoreCase("rust-analyzer", "rust-analyzer"));
    EXPECT_TRUE(containsIgnoreCase("aaab", "aab")); // a partial match before the real one
    EXPECT_FALSE(containsIgnoreCase("rust-analyzer", "rust-analyzerX"));
    EXPECT_FALSE(containsIgnoreCase("rust", "rusty"));
    EXPECT_FALSE(containsIgnoreCase("", "a"));
}

TEST(ProcessFilterMatchTest, AnEmptyTermMatchesEverything)
{
    using App::ProcessFilterCache::containsIgnoreCase;
    using App::ProcessFilterCache::matches;
    EXPECT_TRUE(containsIgnoreCase("", ""));
    EXPECT_TRUE(containsIgnoreCase("bash", ""));
    EXPECT_TRUE(matches("", "", ""));
}

TEST(ProcessFilterMatchTest, OnlyAsciiLettersFold)
{
    using App::ProcessFilterCache::containsIgnoreCase;
    EXPECT_TRUE(containsIgnoreCase("Résumé.exe", "RéSUMé"));          // the ASCII letters fold, é matches itself
    EXPECT_FALSE(containsIgnoreCase("Résumé.exe", "RÉSUMÉ"));         // É is not folded to é
    EXPECT_TRUE(containsIgnoreCase("C:\\Tools\\[x]_1.exe", "[X]_1")); // punctuation and digits as they are
}

TEST(ProcessFilterMatchTest, TheFilterMatchesTheNameOrTheCommandLine)
{
    using App::ProcessFilterCache::matches;
    constexpr std::string_view NAME = "python3";
    constexpr std::string_view COMMAND = "/usr/bin/python3 -m http.server 8080";
    EXPECT_TRUE(matches(NAME, COMMAND, "PYTHON"));      // in both
    EXPECT_TRUE(matches(NAME, COMMAND, "http.server")); // only in the command line
    EXPECT_TRUE(matches(NAME, COMMAND, "8080"));
    EXPECT_FALSE(matches(NAME, COMMAND, "node"));
    EXPECT_TRUE(matches(NAME, "", "pyth")); // a process whose command line could not be read
}

} // namespace
