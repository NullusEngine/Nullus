#include <gtest/gtest.h>

#include "Core/LauncherNavigation.h"

TEST(LauncherNavigationTests, FirstPassOnlyShowsProjectsAndInstalls)
{
    const auto entries = NLS::GetLauncherNavigationEntries();

    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].item, NLS::LauncherNavigationItem::Projects);
    EXPECT_EQ(entries[1].item, NLS::LauncherNavigationItem::Installs);
}
