#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <vector>

namespace NLS::Editor::Core
{
class AssetFileWatcher
{
public:
    AssetFileWatcher();
    ~AssetFileWatcher();

    AssetFileWatcher(const AssetFileWatcher&) = delete;
    AssetFileWatcher& operator=(const AssetFileWatcher&) = delete;
    AssetFileWatcher(AssetFileWatcher&&) noexcept;
    AssetFileWatcher& operator=(AssetFileWatcher&&) noexcept;

    bool Start(const std::filesystem::path& root);
    void Stop();
    bool ConsumeChanged();
    std::vector<std::filesystem::path> ConsumeChangedPaths();
    bool IsRunning() const;
    // True after the platform watcher has registered/armed its first change request.
    bool IsReady() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace NLS::Editor::Core
