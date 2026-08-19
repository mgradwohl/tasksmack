// TaskSmack Test Suite
// This file can contain tests that don't fit in a specific category.
// Most tests should be organized in subdirectories by component:
//   - Domain/     : ProcessModel, SystemModel, History, BackgroundSampler
//   - Platform/   : Linux/Windows parsing and probe tests
//   - Integration/: Cross-component tests

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

namespace
{

// Suppress all spdlog output during test runs to keep --output-on-failure clean.
// Log level is restored when this object is destroyed (per-test or global).
class TestLogSuppressor : public ::testing::Environment
{
public:
    void SetUp() override
    {
        m_savedLevel = spdlog::get_level();
        spdlog::set_level(spdlog::level::off);
    }

    void TearDown() override { spdlog::set_level(m_savedLevel); }

private:
    spdlog::level::level_enum m_savedLevel{spdlog::level::info};
};

} // namespace

// Placeholder test to ensure the test framework is working
TEST(SmokeTest, TestFrameworkWorks)
{
    EXPECT_TRUE(true);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    // Register the global log suppressor so all tests run with spdlog silenced.
    ::testing::AddGlobalTestEnvironment(new TestLogSuppressor());
    return RUN_ALL_TESTS();
}
