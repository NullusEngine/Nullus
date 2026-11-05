#pragma once

#include "UIInputRouter.h"
#include "Rendering/UI/UIDrawSnapshot.h"
#include "EngineDef.h"

#include <cstdint>
#include <memory>
#include <vector>
#include <array>
#include <optional>

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
        void CancelInput(bool clearFocus = true);
        bool WantsKeyboardInput() const;
        bool WantsPointerInput() const;
        std::optional<UIRect> GetTextInputRect() const;
        void SetInputDisplay(uint32_t display);
        void ConfigureOutput(uint32_t display,UIVector2 size,float dpi);
        [[nodiscard]] std::shared_ptr<const NLS::Render::UI::UIDrawSnapshot> BuildScreenSnapshot(
            uint32_t viewportWidth,
            uint32_t viewportHeight,
            float dpiScale,
            uint64_t frameId,uint32_t display=0) const;
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
        struct Output { UIVector2 size{};float dpi=1;bool configured=false; };
        std::array<Output,8> m_outputs{};
        uint32_t m_inputDisplay=0;
    };
}
