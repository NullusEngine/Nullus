#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace NLS::Engine
{
class GameObject;
namespace SceneSystem
{
class Scene;
}
}

namespace NLS::Editor::Assets
{
/// Reuses the transient objects used to assemble thumbnail preview scenes.
///
/// A lease is deliberately separate from the pool slot. The lease can be moved
/// into a readback keep-alive object, so a completed GPU submission never races
/// with the next thumbnail reusing the same GameObject.
class ThumbnailPreviewProxyPool final
{
private:
    struct State;

public:
    class Lease final
    {
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        ~Lease();

        [[nodiscard]] NLS::Engine::GameObject* Get() const;
        [[nodiscard]] uint64_t Serial() const;
        [[nodiscard]] explicit operator bool() const;

    private:
        friend class ThumbnailPreviewProxyPool;
        Lease(std::shared_ptr<State> state, size_t slot, uint64_t serial);
        void Reset();

        std::shared_ptr<State> m_state;
        size_t m_slot = 0u;
        uint64_t m_serial = 0u;
    };

    explicit ThumbnailPreviewProxyPool(
        NLS::Engine::SceneSystem::Scene& scene,
        size_t maxObjects = 512u);
    ~ThumbnailPreviewProxyPool();

    ThumbnailPreviewProxyPool(const ThumbnailPreviewProxyPool&) = delete;
    ThumbnailPreviewProxyPool& operator=(const ThumbnailPreviewProxyPool&) = delete;

    [[nodiscard]] std::optional<Lease> Acquire(
        std::string_view debugName,
        bool allowReuse = true);
    [[nodiscard]] size_t GetObjectCount() const;
    [[nodiscard]] size_t GetActiveLeaseCount() const;
    [[nodiscard]] size_t GetReuseHitCount() const;
    [[nodiscard]] size_t GetAllocationCount() const;

private:
    struct Slot
    {
        NLS::Engine::GameObject* object = nullptr;
        uint64_t serial = 0u;
        bool leased = false;
        bool pooled = true;
    };

    struct State
    {
        NLS::Engine::SceneSystem::Scene* scene = nullptr;
        size_t maxObjects = 0u;
        std::vector<Slot> slots;
        size_t activeLeaseCount = 0u;
        size_t reuseHitCount = 0u;
        size_t allocationCount = 0u;

        ~State();
        // A lease may outlive the pool while a GPU readback is retiring.  The
        // scene is owned by the renderer, so late lease destruction must not
        // dereference it after the renderer has started tearing down.
        void DetachScene();
        void Release(size_t slot, uint64_t serial);
        void ResetSlot(Slot& slot);
    };

    std::shared_ptr<State> m_state;
};
}
