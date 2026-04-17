#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "Core/LauncherLocalization.h"

namespace
{
std::filesystem::path MakeTempLocalizationRoot()
{
    auto root = std::filesystem::temp_directory_path() / "NullusLauncherLocalizationTests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}
}

TEST(LauncherLocalizationTests, LoadsTextByKeyFromLocaleResource)
{
    const auto root = MakeTempLocalizationRoot();
    {
        std::ofstream resource(root / "strings.csv", std::ios::binary);
        resource << "key,en-US,zh-CN\n";
        resource << "projects.title,Projects,项目\n";
        resource << "projects.search_hint,Search projects,搜索项目\n";
    }

    NLS::LauncherLocalization localization;
    EXPECT_TRUE(localization.Load(root, "en-US"));

    EXPECT_EQ(localization.Text(NLS::LauncherTextKey::ProjectsTitle), "Projects");
    EXPECT_EQ(localization.Text(NLS::LauncherTextKey::ProjectsSearchHint), "Search projects");
}

TEST(LauncherLocalizationTests, FallsBackToEnglishThenAsciiKeyName)
{
    const auto root = MakeTempLocalizationRoot();
    {
        std::ofstream table(root / "strings.csv", std::ios::binary);
        table << "key,en-US,zh-CN\n";
        table << "projects.title,Projects,\n";
        table << "launcher.title,Nullus Hub,Nullus Hub CN\n";
    }

    NLS::LauncherLocalization localization;
    EXPECT_TRUE(localization.Load(root, "zh-CN"));

    EXPECT_EQ(localization.Text(NLS::LauncherTextKey::LauncherTitle), "Nullus Hub CN");
    EXPECT_EQ(localization.Text(NLS::LauncherTextKey::ProjectsTitle), "Projects");
    EXPECT_EQ(localization.Text(NLS::LauncherTextKey::MissingText), "missing.text");
}

TEST(LauncherLocalizationTests, NormalizesLocaleNamesFromEnvironmentOrSystemFormats)
{
    EXPECT_EQ(NLS::NormalizeLocaleName("zh_CN.UTF-8"), "zh-CN");
    EXPECT_EQ(NLS::NormalizeLocaleName("en_US"), "en-US");
    EXPECT_EQ(NLS::NormalizeLocaleName("zh"), "zh-CN");
    EXPECT_EQ(NLS::NormalizeLocaleName("Chinese_China.936"), "zh-CN");
    EXPECT_EQ(NLS::NormalizeLocaleName("C"), "");
}
