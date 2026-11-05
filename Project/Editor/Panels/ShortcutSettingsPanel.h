#pragma once
#include "Properties/PropertyPanel.h"

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
            const Shortcuts::ShortcutDescriptor& p_command,
            const std::vector<const Shortcuts::ShortcutDescriptor*>& p_visibleCommands);

	private:
        void DrawProfileRow();
        void DrawKeyboardPreview(const std::vector<const Shortcuts::ShortcutDescriptor*>& p_commands);
        void DrawLegend();
        void DrawCategoryList(const std::vector<const Shortcuts::ShortcutDescriptor*>& p_commands);
        void DrawCommandList(const std::vector<const Shortcuts::ShortcutDescriptor*>& p_commands);

	private:
        bool m_opened = false;
		std::string m_searchText;
        std::string m_selectedCategory;
        std::string m_selectedCommandId;
		std::string m_recordingCommandId;
        size_t m_recordingSlot = 0;
        std::unique_ptr<Input::RebindingSession> m_rebinding;
		std::string m_conflictText;
        Properties::PropertyPanel m_properties;
	};
}
