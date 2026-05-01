#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "ReflectionTestUtils.h"

namespace
{
    std::filesystem::path GetRepositoryRoot()
    {
        return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    }

    std::string ReadRepositorySource(std::string_view relativePath)
    {
        return NLS::Tests::Reflection::ReadAllText(GetRepositoryRoot() / relativePath);
    }
}

TEST(UIWidgetButtonContractTests, ButtonUsesStableDisabledSnapshotAcrossClickCallbacks)
{
    const auto buttonSource = ReadRepositorySource("Runtime/UI/Widgets/Buttons/Button.cpp");

    EXPECT_NE(buttonSource.find("const bool wasDisabled = disabled;"), std::string::npos);
    EXPECT_NE(buttonSource.find("if (wasDisabled)\n\t\t\tImGui::BeginDisabled();"), std::string::npos);
    EXPECT_NE(buttonSource.find("if (wasDisabled)\n\t\t\tImGui::EndDisabled();"), std::string::npos);
    EXPECT_EQ(buttonSource.find("if (disabled)\n\t\t\tImGui::BeginDisabled();"), std::string::npos);
    EXPECT_EQ(buttonSource.find("if (disabled)\n\t\t\tImGui::EndDisabled();"), std::string::npos);
}
