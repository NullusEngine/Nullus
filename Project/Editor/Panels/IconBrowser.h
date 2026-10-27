#pragma once

#include <string>

#include <UI/Panels/PanelWindow.h>

namespace NLS::Editor::Icons
{
    struct IconDefinition;
}

namespace NLS::Editor::Panels
{
    /**
    * Development window for browsing, previewing and reloading editor icons.
    * Reads the runtime IconRegistry/IconService; the manifest is build-time only.
    */
    class IconBrowser : public UI::PanelWindow
    {
    public:
        IconBrowser(
            const std::string& p_title,
            bool p_opened,
            const UI::PanelWindowSettings& p_windowSettings);

    protected:
        void _Draw_Impl() override;

    private:
        bool MatchesFilter(const Icons::IconDefinition& p_definition);
        void DrawToolbar();
        void DrawGrid();
        void DrawDetails();

        char m_search[128] {};
        int m_tokenIndex = 1; // Small
        int m_stateIndex = 0; // Normal
        int m_usageFilter = 0; // 0 all, 1 used, 2 unused
        bool m_missingOnly = false;
        std::string m_selectedId;
    };
}
