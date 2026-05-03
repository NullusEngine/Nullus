#pragma once

#include <string>

#include "UI/Panels/APanel.h"

namespace NLS::UI
{
    class NLS_UI_API PanelViewportBar : public APanel
    {
    public:
        enum class EAnchor
        {
            TOP,
            BOTTOM
        };

        PanelViewportBar(
            const std::string& p_windowName,
            EAnchor p_anchor,
            float p_height,
            bool p_includeMenuBar = false);

        static float GetReservedTopHeight();
        static float GetReservedBottomHeight();
        static void ResetReservedHeights();

        void SetHeight(float p_height);
        float GetHeight() const;
        void RefreshReservedLayout() const;

    protected:
        void _Draw_Impl() override;
        virtual void DrawBarContent() = 0;
        float GetContentWidth() const;

    private:
        static void RefreshReservedHeight(EAnchor p_anchor, float p_height);

    protected:
        std::string m_windowName;
        EAnchor m_anchor;
        float m_height = 0.0f;
        bool m_includeMenuBar = false;

    private:
        inline static float s_reservedTopHeight = 0.0f;
        inline static float s_reservedBottomHeight = 0.0f;
    };
}
