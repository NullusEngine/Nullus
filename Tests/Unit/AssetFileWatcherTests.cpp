#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "Core/AssetFileWatcher.h"
#include "Guid.h"

namespace
{
std::filesystem::path MakeWatcherTestRoot()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_asset_watcher_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);
    return root;
}

void WriteText(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

bool WaitForChange(NLS::Editor::Core::AssetFileWatcher& watcher)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (watcher.ConsumeChanged())
            return true;

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    return false;
}
} // namespace

TEST(AssetFileWatcherTests, ReportsCreateModifyRenameMoveAndDeleteEvents)
{
    using NLS::Editor::Core::AssetFileWatcher;

    const auto root = MakeWatcherTestRoot();
    AssetFileWatcher watcher;
    ASSERT_TRUE(watcher.Start(root));
    ASSERT_TRUE(watcher.IsRunning());

    const auto scene = root / "Scenes" / "New.scene";
    WriteText(scene, "one");
    EXPECT_TRUE(WaitForChange(watcher));

    WriteText(scene, "two");
    EXPECT_TRUE(WaitForChange(watcher));

    const auto renamed = root / "Scenes" / "Renamed.scene";
    std::filesystem::rename(scene, renamed);
    EXPECT_TRUE(WaitForChange(watcher));

    const auto moved = root / "Moved" / "Renamed.scene";
    std::filesystem::create_directories(moved.parent_path());
    std::filesystem::rename(renamed, moved);
    EXPECT_TRUE(WaitForChange(watcher));

    std::filesystem::remove(moved);
    EXPECT_TRUE(WaitForChange(watcher));

    watcher.Stop();
    std::filesystem::remove_all(root);
}
