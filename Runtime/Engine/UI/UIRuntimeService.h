#pragma once

#include "UIInputRouter.h"
#include "Rendering/UI/UIDrawSnapshot.h"
#include "EngineDef.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace NLS::Engine::Components
{
    class UICanvasComponent;
}

namespace NLS::Engine::UI
{
    enum class UIWorldSnapshotPath : uint8_t
    {
        Direct,
        RenderTarget
    };

    struct NLS_ENGINE_API UIWorldDrawSnapshot
    {
        std::shared_ptr<const NLS::Render::UI::UIDrawSnapshot> snapshot;
        int32_t sortOrder = 0;
        UIWorldSnapshotPath path = UIWorldSnapshotPath::Direct;
        bool depthTest = true;
        bool depthWrite = false;
        bool alwaysOnTop = false;
        UIVector2 worldSize { 1.0f, 1.0f };
        float pixelsPerUnit = 100.0f;
    };

    class NLS_ENGINE_API UIRuntimeService final
    {
    public:
        void RegisterCanvas(Components::UICanvasComponent& canvas);
        void UnregisterCanvas(Components::UICanvasComponent& canvas);
        void SyncCanvases(const std::vector<Components::UICanvasComponent*>& canvases);

        void Tick(float deltaSeconds, UIVector2 viewportSize, float dpiScale, uint64_t frameId);
        bool DispatchInput(const UIInputEvent& event);
        uint32_t DispatchInputQueue(InputEventQueue& queue);
        [[nodiscard]] std::shared_ptr<const NLS::Render::UI::UIDrawSnapshot> BuildScreenSnapshot(
            uint32_t viewportWidth,
            uint32_t viewportHeight,
            float dpiScale,
            uint64_t frameId) const;
        [[nodiscard]] std::vector<UIWorldDrawSnapshot> BuildWorldSnapshots(
            uint32_t viewportWidth,
            uint32_t viewportHeight,
            float dpiScale,
            uint64_t frameId) const;
        [[nodiscard]] std::shared_ptr<const NLS::Render::UI::UIDrawSnapshot> GetLatestSnapshot() const
        {
            return m_latestSnapshot;
        }
        [[nodiscard]] const std::vector<Components::UICanvasComponent*>& GetCanvases() const { return m_canvases; }

    private:
        std::vector<Components::UICanvasComponent*> m_canvases;
        std::shared_ptr<const NLS::Render::UI::UIDrawSnapshot> m_latestSnapshot;
    };
}
