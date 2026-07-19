#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "RenderDef.h"
#include "Rendering/UI/UiDrawDataSnapshot.h"

namespace NLS::Render::RHI
{
    class RHIBindingLayout;
    class RHIBindingSet;
    class RHITextureView;
    class RHIDevice;
    class RHISampler;
}

namespace NLS::Render::UI
{
    struct NLS_RENDER_API RHIImGuiTextureRegistryEntry
    {
        UiTextureId id {};
        std::shared_ptr<RHI::RHITextureView> textureView;
        std::shared_ptr<RHI::RHIBindingSet> bindingSet;
        uint64_t bindingSetDeviceCacheIdentity = 0u;
        uint64_t lastUsedFrameId = 0u;
        UiTextureSynchronizationScope synchronizationScope =
            UiTextureSynchronizationScope::PreviousFrameOrStatic;
        bool releaseRequested = false;
        uint64_t safeToReleaseFrameId = 0u;
    };

    class NLS_RENDER_API RHIImGuiTextureRegistry
    {
    public:
        UiTextureId RegisterTextureView(
            const std::shared_ptr<RHI::RHITextureView>& textureView,
            UiTextureSynchronizationScope synchronizationScope);
        // Tests exact shared ownership identity and excludes entries pending retirement.
        [[nodiscard]] bool ContainsLiveTextureView(
            const std::shared_ptr<RHI::RHITextureView>& textureView) const;
        [[nodiscard]] bool ContainsSampledTextureIdentity(
            uint64_t textureIdentity,
            uint64_t frameId) const;
        [[nodiscard]] std::vector<uint64_t> CollectReferencedTextureIdentities(
            const UiDrawDataSnapshot& snapshot) const;
        [[nodiscard]] bool EnsureBindingSet(
            RHI::RHIDevice& device,
            const std::shared_ptr<RHI::RHIBindingLayout>& bindingLayout,
            const std::shared_ptr<RHI::RHISampler>& sampler,
            UiTextureId id,
            std::string& errorMessage);
        void ReleaseTextureView(const std::shared_ptr<RHI::RHITextureView>& textureView, uint64_t frameId);
        void ReleaseRetiredTextureViewsUpTo(uint64_t completedFrameId);
        [[nodiscard]] std::optional<RHIImGuiTextureRegistryEntry> Resolve(UiTextureId id) const;
        [[nodiscard]] std::optional<RHIImGuiTextureRegistryEntry> ResolveForFrame(
            UiTextureId id,
            uint64_t frameId) const;

#if defined(NLS_ENABLE_TEST_HOOKS)
        [[nodiscard]] size_t GetEntryCountForTesting() const
        {
            std::lock_guard lock(m_mutex);
            return m_entries.size();
        }

        [[nodiscard]] size_t GetPendingRetiredEntryCountForTesting() const
        {
            std::lock_guard lock(m_mutex);
            return m_retiredEntryIdsByFrameId.size();
        }
#endif

    private:
        mutable std::mutex m_mutex;
        uint64_t m_nextId = 1u;
        std::unordered_map<uint64_t, RHIImGuiTextureRegistryEntry> m_entries;
        std::unordered_map<const RHI::RHITextureView*, uint64_t> m_liveViewToId;
        std::multimap<uint64_t, uint64_t> m_retiredEntryIdsByFrameId;
    };
}
