#pragma once

#include <cstdint>

#include "ImGui/imgui.h"

#include "Icons/EditorIconCatalog.h"
#include "Icons/EditorTheme.h"
#include "Icons/IconService.h"

namespace NLS::Editor::Icons
{
    struct IconMetrics
    {
        float uiScale = 1.0f;          // Layout scale (UIManager::GetScale).
        float framebufferScale = 1.0f; // Physical pixels per UI unit (ImGui IO).
    };

    // Reads the live UI metrics. Editor code must not multiply GetScale() into
    // icon sizes itself; IconPainter owns the logical-to-raster conversion.
    IconMetrics CurrentIconMetrics();

    // tokenPixels * uiScale * framebufferScale. The product is the physical
    // raster target used for tier selection.
    std::uint32_t ComputeIconTargetPixels(float p_logicalPixels, const IconMetrics& p_metrics);

    struct IconButtonOptions
    {
        const char* tooltip = nullptr;
        bool selected = false;
        bool enabled = true;
        bool danger = false;
    };

    /**
    * The only supported editor entry point for drawing icons. Widget ids are
    * supplied by the caller and must never be derived from the icon id.
    */
    class IconPainter
    {
    public:
        explicit IconPainter(IconService& p_service);

        // Refreshes metrics and retires stale GPU resources once per frame.
        void BeginFrame();

        bool Button(
            const char* p_widgetId,
            IconId p_icon,
            IconToken p_token,
            const IconButtonOptions& p_options = {});

        bool Toggle(
            const char* p_widgetId,
            IconId p_icon,
            IconToken p_token,
            bool p_value,
            const IconButtonOptions& p_options = {});

        bool TextButton(
            const char* p_widgetId,
            IconId p_icon,
            const char* p_label,
            IconToken p_token,
            const IconButtonOptions& p_options = {});

        bool MenuItem(
            const char* p_widgetId,
            IconId p_icon,
            const char* p_label,
            const char* p_shortcut,
            bool p_selected,
            bool p_enabled);

        bool Draw(
            ImDrawList& p_drawList,
            const ImVec2& p_topLeft,
            const ImVec2& p_size,
            IconId p_icon,
            IconState p_state,
            const IconMetrics& p_metrics);

        void Draw(
            ImDrawList& p_drawList,
            const ImVec2& p_topLeft,
            IconId p_icon,
            IconToken p_token,
            IconState p_state,
            const IconMetrics& p_metrics);

        IconService& GetService() { return m_service; }
        const IconMetrics& GetMetrics() const { return m_metrics; }

    private:
        IconMetrics m_metrics;
        IconService& m_service;
        int m_metricsFrame = -1;
    };
}
