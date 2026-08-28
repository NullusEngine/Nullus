#pragma once

#include "RenderDef.h"
#include "UIDrawSnapshot.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace NLS::Render::UI
{
    enum class UITextureKind : uint8_t
    {
        Image,
        FontAtlas,
        RenderTarget
    };

    struct NLS_RENDER_API UITextureHandle
    {
        uint64_t value = 0u;
        uint32_t generation = 0u;

        [[nodiscard]] bool IsValid() const { return value != 0u && generation != 0u; }
        friend constexpr bool operator==(UITextureHandle, UITextureHandle) = default;
    };

    struct NLS_RENDER_API UITextureResource
    {
        UITextureHandle handle;
        uint64_t resourceIdentity = 0u;
        UITextureKind kind = UITextureKind::Image;
        uint64_t lastUsedFrame = 0u;
        bool releaseRequested = false;
        uint64_t safeToReleaseFrame = 0u;
    };

    // Backend-neutral lifetime registry used by UIDrawSnapshot producers. The
    // RHI renderer resolves the resource identity to an API texture view at
    // submission time; gameplay code never stores an RHI pointer in a document.
    class NLS_RENDER_API UITextureRegistry final
    {
    public:
        UITextureHandle Register(uint64_t resourceIdentity, UITextureKind kind = UITextureKind::Image);
        bool MarkUsed(UITextureHandle handle, uint64_t frameId);
        bool Retire(UITextureHandle handle, uint64_t safeToReleaseFrame);
        void CollectRetired(uint64_t completedFrameId);

        [[nodiscard]] std::optional<UITextureResource> Resolve(UITextureHandle handle) const;
        [[nodiscard]] bool IsLive(UITextureHandle handle, uint64_t frameId = 0u) const;
        [[nodiscard]] std::vector<UITextureHandle> CollectReferenced(
            const UIDrawSnapshot& snapshot,
            uint64_t frameId) const;

#if defined(NLS_ENABLE_TEST_HOOKS)
        [[nodiscard]] size_t GetLiveCountForTesting() const;
        [[nodiscard]] size_t GetRetiredCountForTesting() const;
#endif

    private:
        mutable std::mutex m_mutex;
        uint64_t m_nextValue = 1u;
        std::unordered_map<uint64_t, UITextureResource> m_resources;
        std::unordered_map<uint64_t, UITextureHandle> m_handleByResource;
    };
}
