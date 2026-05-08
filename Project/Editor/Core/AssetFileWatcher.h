#pragma once

#include <atomic>
#include <filesystem>
#include <memory>

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
    bool IsRunning() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace NLS::Editor::Core
