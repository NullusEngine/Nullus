#pragma once

#include <array>

namespace NLS
{
enum class LauncherNavigationItem
{
    Projects,
    Installs
};

struct LauncherNavigationEntry
{
    LauncherNavigationItem item;
};

inline constexpr std::array<LauncherNavigationEntry, 2> GetLauncherNavigationEntries()
{
    return {{
        {LauncherNavigationItem::Projects},
        {LauncherNavigationItem::Installs},
    }};
}
} // namespace NLS
