#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <unordered_set>
#include <vector>

namespace NLS::Editor::Core
{
// Editor-only visibility. Object activation and the shared render store are untouched.
class SceneVisibilityState
{
public:
    void BindScene(const void* scene, int32_t instanceId = 0)
    {
        if (scene == m_scene && instanceId == m_sceneInstanceId) return;
        m_scene = scene;
        m_sceneInstanceId = instanceId;
        ShowAll();
    }
    void SetHidden(std::span<const int32_t> ids, bool hidden)
    {
        bool changed = false;
        for (const auto id : ids)
            if (id != 0) changed |= hidden ? m_hidden.insert(id).second : m_hidden.erase(id) != 0;
        if (changed) ++m_revision;
    }
    void Isolate(std::span<const int32_t> visibleIds)
    {
        m_isolated = {visibleIds.begin(), visibleIds.end()};
        m_isolating = !m_isolated.empty();
        ++m_revision;
    }
    void ExitIsolation()
    {
        if (!m_isolating) return;
        m_isolating = false;
        m_isolated.clear();
        ++m_revision;
    }
    void ShowAll()
    {
        m_hidden.clear();
        m_isolated.clear();
        m_isolating = false;
        ++m_revision;
    }
    bool IsHidden(int32_t id) const
    {
        return m_hidden.contains(id) || (m_isolating && !m_isolated.contains(id));
    }
    bool HasHiddenObjects() const { return !m_hidden.empty() || m_isolating; }
    bool IsIsolating() const { return m_isolating; }
    uint64_t Revision() const { return m_revision; }
    std::vector<int32_t> CaptureHiddenIds(std::span<const int32_t> sceneIds, bool enabled) const
    {
        std::vector<int32_t> result;
        if (enabled)
            for (const auto id : sceneIds)
                if (IsHidden(id)) result.push_back(id);
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }
private:
    const void* m_scene = nullptr;
    int32_t m_sceneInstanceId = 0;
    std::unordered_set<int32_t> m_hidden;
    std::unordered_set<int32_t> m_isolated;
    uint64_t m_revision = 0;
    bool m_isolating = false;
};
}
