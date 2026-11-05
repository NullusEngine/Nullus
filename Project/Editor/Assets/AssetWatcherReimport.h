#pragma once

#include <algorithm>
#include <filesystem>
#include <functional>
#include <utility>
#include <vector>

namespace NLS::Editor::Assets
{
// Notifications have no operation identity. Compare their contents after the
// active import publishes its final metadata and Undo completion. Genuine
// external edits remain queued and are compared at that same boundary.
inline void DeferInProgressAssetWatcherChanges(
    std::vector<std::filesystem::path>& changes,
    std::vector<std::filesystem::path>& deferred,
    const std::function<bool(const std::filesystem::path&)>& inProgress)
{
    std::vector<std::filesystem::path> ready;
    ready.reserve(changes.size());
    for(auto& path:changes)
    {
        if(inProgress(path))
        {
            if(std::find(deferred.begin(),deferred.end(),path)==deferred.end())
                deferred.push_back(std::move(path));
        }
        else ready.push_back(std::move(path));
    }
    changes=std::move(ready);
}
}
