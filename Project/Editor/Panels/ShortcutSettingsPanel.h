#pragma once

#include <string>
#include <vector>

#include "Shortcuts/EditorShortcutCommand.h"
#include "Shortcuts/EditorShortcutBinding.h"
#include "Shortcuts/EditorShortcutService.h"

namespace NLS::Editor::Panels
{
    class ShortcutSettingsPanel
    {
    public:
        void Open();
        void Draw();
        bool IsOpened() const;

        static std::string GetCommandListDisplayName(
            const Shortcuts::ShortcutCommand& p_command,
            const std::vector<const Shortcuts::ShortcutCommand*>& p_visibleCommands);

	private:
        void DrawProfileRow();
        void DrawKeyboardPreview(const std::vector<const Shortcuts::ShortcutCommand*>& p_commands);
        void DrawLegend();
        void DrawCategoryList(const std::vector<const Shortcuts::ShortcutCommand*>& p_commands);
        void DrawCommandList(const std::vector<const Shortcuts::ShortcutCommand*>& p_commands);
		Shortcuts::ShortcutBinding CapturePressedBinding() const;

	private:
        bool m_opened = false;
		std::string m_searchText;
        std::string m_selectedCategory;
        std::string m_selectedCommandId;
		std::string m_recordingCommandId;
		std::string m_conflictText;
	};
}
