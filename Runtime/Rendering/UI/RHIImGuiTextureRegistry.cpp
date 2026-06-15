#include "Rendering/UI/RHIImGuiTextureRegistry.h"

#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIResource.h"

namespace NLS::Render::UI
{
    namespace
    {
        bool HasResourceStateFlag(const RHI::ResourceState state, const RHI::ResourceState flag)
        {
            return (static_cast<uint32_t>(state) & static_cast<uint32_t>(flag)) != 0u;
        }

        bool IsPreviousFrameOrStaticTextureReadable(const std::shared_ptr<RHI::RHITextureView>& textureView)
        {
            if (textureView == nullptr || textureView->GetTexture() == nullptr)
                return false;

            return HasResourceStateFlag(textureView->GetTexture()->GetState(), RHI::ResourceState::ShaderRead);
        }
    }

    UiTextureId RHIImGuiTextureRegistry::RegisterTextureView(
        const std::shared_ptr<RHI::RHITextureView>& textureView,
        const UiTextureSynchronizationScope synchronizationScope)
    {
        if (textureView == nullptr ||
            synchronizationScope != UiTextureSynchronizationScope::PreviousFrameOrStatic ||
            !IsPreviousFrameOrStaticTextureReadable(textureView))
        {
            return {};
        }

        std::lock_guard lock(m_mutex);
        const auto liveViewKey = textureView.get();
        const auto liveEntry = m_liveViewToId.find(liveViewKey);
        if (liveEntry != m_liveViewToId.end())
        {
            const auto found = m_entries.find(liveEntry->second);
            if (found != m_entries.end() &&
                !found->second.releaseRequested &&
                found->second.textureView == textureView)
            {
                return found->second.id;
            }

            m_liveViewToId.erase(liveEntry);
        }

        const UiTextureId id { m_nextId++, 1u };
        RHIImGuiTextureRegistryEntry entry;
        entry.id = id;
        entry.textureView = textureView;
        entry.synchronizationScope = synchronizationScope;
        m_entries.emplace(id.value, std::move(entry));
        m_liveViewToId.emplace(liveViewKey, id.value);
        return id;
    }

    bool RHIImGuiTextureRegistry::EnsureBindingSet(
        RHI::RHIDevice& device,
        const std::shared_ptr<RHI::RHIBindingLayout>& bindingLayout,
        const std::shared_ptr<RHI::RHISampler>& sampler,
        const UiTextureId id,
        std::string& errorMessage)
    {
        if (!id.IsValid())
            return true;

        if (bindingLayout == nullptr)
        {
            errorMessage = "UI texture binding layout is not prepared";
            return false;
        }

        if (sampler == nullptr)
        {
            errorMessage = "UI texture sampler is not prepared";
            return false;
        }

        std::lock_guard lock(m_mutex);
        const auto found = m_entries.find(id.value);
        if (found == m_entries.end() ||
            found->second.id.generation != id.generation ||
            found->second.textureView == nullptr ||
            found->second.releaseRequested)
        {
            return true;
        }

        auto& entry = found->second;
        const uint64_t deviceCacheIdentity = device.GetCacheIdentity();
        if (entry.bindingSet != nullptr &&
            entry.bindingSetDeviceCacheIdentity == deviceCacheIdentity)
            return true;

        entry.bindingSet.reset();
        entry.bindingSetDeviceCacheIdentity = 0u;

        RHI::RHIBindingSetDesc bindingSetDesc;
        bindingSetDesc.layout = bindingLayout;
        bindingSetDesc.debugName = "RHIImGuiTextureBindingSet";
        bindingSetDesc.entries.push_back({
            0u,
            RHI::BindingType::Texture,
            nullptr,
            0u,
            0u,
            0u,
            entry.textureView,
            nullptr
        });
        bindingSetDesc.entries.push_back({
            1u,
            RHI::BindingType::Sampler,
            nullptr,
            0u,
            0u,
            0u,
            nullptr,
            sampler
        });

        entry.bindingSet = device.CreateBindingSet(bindingSetDesc);
        if (entry.bindingSet == nullptr)
        {
            errorMessage = "failed to create UI texture binding set";
            return false;
        }

        entry.bindingSetDeviceCacheIdentity = deviceCacheIdentity;
        return true;
    }

    void RHIImGuiTextureRegistry::ReleaseTextureView(
        const std::shared_ptr<RHI::RHITextureView>& textureView,
        const uint64_t frameId)
    {
        if (textureView == nullptr)
            return;

        std::lock_guard lock(m_mutex);
        const auto liveViewKey = textureView.get();
        const auto liveEntry = m_liveViewToId.find(liveViewKey);
        if (liveEntry == m_liveViewToId.end())
            return;

        const auto found = m_entries.find(liveEntry->second);
        m_liveViewToId.erase(liveEntry);
        if (found == m_entries.end())
            return;

        auto& entry = found->second;
        if (entry.textureView != textureView)
            return;

        entry.releaseRequested = true;
        entry.safeToReleaseFrameId = frameId;
        m_retiredEntryIdsByFrameId.emplace(frameId, entry.id.value);
    }

    void RHIImGuiTextureRegistry::ReleaseRetiredTextureViewsUpTo(const uint64_t completedFrameId)
    {
        std::lock_guard lock(m_mutex);
        for (auto retiredIt = m_retiredEntryIdsByFrameId.begin();
            retiredIt != m_retiredEntryIdsByFrameId.end() && retiredIt->first <= completedFrameId;)
        {
            const auto entryId = retiredIt->second;
            retiredIt = m_retiredEntryIdsByFrameId.erase(retiredIt);

            const auto found = m_entries.find(entryId);
            if (found == m_entries.end())
                continue;

            const auto& entry = found->second;
            if (entry.releaseRequested && entry.safeToReleaseFrameId <= completedFrameId)
            {
                m_entries.erase(found);
            }
        }
    }

    std::optional<RHIImGuiTextureRegistryEntry> RHIImGuiTextureRegistry::Resolve(const UiTextureId id) const
    {
        if (!id.IsValid())
            return std::nullopt;

        std::lock_guard lock(m_mutex);
        const auto found = m_entries.find(id.value);
        if (found == m_entries.end() ||
            found->second.id.generation != id.generation ||
            found->second.textureView == nullptr ||
            found->second.releaseRequested)
        {
            return std::nullopt;
        }
        return found->second;
    }

    std::optional<RHIImGuiTextureRegistryEntry> RHIImGuiTextureRegistry::ResolveForFrame(
        const UiTextureId id,
        const uint64_t frameId) const
    {
        if (!id.IsValid() || frameId == 0u)
            return std::nullopt;

        std::lock_guard lock(m_mutex);
        const auto found = m_entries.find(id.value);
        if (found == m_entries.end() ||
            found->second.id.generation != id.generation ||
            found->second.textureView == nullptr)
        {
            return std::nullopt;
        }

        if (!found->second.releaseRequested)
            return found->second;

        if (frameId <= found->second.safeToReleaseFrameId)
            return found->second;

        return std::nullopt;
    }
}
