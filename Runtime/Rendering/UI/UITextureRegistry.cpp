#include "UITextureRegistry.h"

#include <algorithm>

namespace NLS::Render::UI
{
    UITextureHandle UITextureRegistry::Register(
        const uint64_t resourceIdentity,
        const UITextureKind kind)
    {
        if (resourceIdentity == 0u)
            return {};

        std::lock_guard lock(m_mutex);
        if (const auto existing = m_handleByResource.find(resourceIdentity);
            existing != m_handleByResource.end())
        {
            const auto resource = m_resources.find(existing->second.value);
            if (resource != m_resources.end() && !resource->second.releaseRequested)
            {
                resource->second.kind = kind;
                return resource->second.handle;
            }
            m_handleByResource.erase(existing);
        }

        const UITextureHandle handle { m_nextValue++, 1u };
        m_resources.emplace(handle.value, UITextureResource {
            handle, resourceIdentity, kind, 0u, false, 0u });
        m_handleByResource[resourceIdentity] = handle;
        return handle;
    }

    bool UITextureRegistry::MarkUsed(const UITextureHandle handle, const uint64_t frameId)
    {
        if (!handle.IsValid())
            return false;
        std::lock_guard lock(m_mutex);
        const auto iterator = m_resources.find(handle.value);
        if (iterator == m_resources.end() || iterator->second.handle.generation != handle.generation ||
            iterator->second.releaseRequested)
            return false;
        iterator->second.lastUsedFrame = std::max(iterator->second.lastUsedFrame, frameId);
        return true;
    }

    bool UITextureRegistry::Retire(const UITextureHandle handle, const uint64_t safeToReleaseFrame)
    {
        if (!handle.IsValid())
            return false;
        std::lock_guard lock(m_mutex);
        const auto iterator = m_resources.find(handle.value);
        if (iterator == m_resources.end() || iterator->second.handle.generation != handle.generation)
            return false;
        iterator->second.releaseRequested = true;
        iterator->second.safeToReleaseFrame = safeToReleaseFrame;
        if (const auto byResource = m_handleByResource.find(iterator->second.resourceIdentity);
            byResource != m_handleByResource.end() && byResource->second == handle)
            m_handleByResource.erase(byResource);
        return true;
    }

    void UITextureRegistry::CollectRetired(const uint64_t completedFrameId)
    {
        std::lock_guard lock(m_mutex);
        for (auto iterator = m_resources.begin(); iterator != m_resources.end();)
        {
            if (iterator->second.releaseRequested &&
                iterator->second.safeToReleaseFrame <= completedFrameId)
                iterator = m_resources.erase(iterator);
            else
                ++iterator;
        }
    }

    std::optional<UITextureResource> UITextureRegistry::Resolve(const UITextureHandle handle) const
    {
        if (!handle.IsValid())
            return std::nullopt;
        std::lock_guard lock(m_mutex);
        const auto iterator = m_resources.find(handle.value);
        if (iterator == m_resources.end() || iterator->second.handle.generation != handle.generation ||
            iterator->second.releaseRequested)
            return std::nullopt;
        return iterator->second;
    }

    bool UITextureRegistry::IsLive(const UITextureHandle handle, const uint64_t frameId) const
    {
        if (!handle.IsValid())
            return false;
        std::lock_guard lock(m_mutex);
        const auto iterator = m_resources.find(handle.value);
        if (iterator == m_resources.end() || iterator->second.handle.generation != handle.generation)
            return false;
        if (!iterator->second.releaseRequested)
            return true;
        return frameId != 0u && frameId <= iterator->second.safeToReleaseFrame;
    }

    std::vector<UITextureHandle> UITextureRegistry::CollectReferenced(
        const UIDrawSnapshot& snapshot,
        const uint64_t frameId) const
    {
        std::vector<UITextureHandle> result;
        std::lock_guard lock(m_mutex);
        for (const auto& batch : snapshot.batches)
        {
            if (batch.textureId == 0u)
                continue;
            const UITextureHandle handle {
                batch.textureId,
                batch.textureGeneration == 0u ? 1u : batch.textureGeneration };
            const auto iterator = m_resources.find(handle.value);
            if (iterator == m_resources.end() || iterator->second.handle.generation != handle.generation)
                continue;
            const auto& resource = iterator->second;
            if (resource.releaseRequested &&
                (frameId == 0u || frameId > resource.safeToReleaseFrame))
                continue;
            if (std::find(result.begin(), result.end(), handle) == result.end())
                result.push_back(handle);
        }
        return result;
    }

#if defined(NLS_ENABLE_TEST_HOOKS)
    size_t UITextureRegistry::GetLiveCountForTesting() const
    {
        std::lock_guard lock(m_mutex);
        return static_cast<size_t>(std::count_if(
            m_resources.begin(), m_resources.end(),
            [](const auto& entry) { return !entry.second.releaseRequested; }));
    }

    size_t UITextureRegistry::GetRetiredCountForTesting() const
    {
        std::lock_guard lock(m_mutex);
        return static_cast<size_t>(std::count_if(
            m_resources.begin(), m_resources.end(),
            [](const auto& entry) { return entry.second.releaseRequested; }));
    }
#endif
}
