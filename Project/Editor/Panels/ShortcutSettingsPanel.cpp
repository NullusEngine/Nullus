#include "Panels/ShortcutSettingsPanel.h"

#include <algorithm>
#include <array>
#include <set>
#include <string_view>

#include <imgui.h>
#include <ServiceLocator.h>

#include "Core/EditorInteractionBlocker.h"
#include "Shortcuts/EditorShortcutService.h"

namespace NLS::Editor::Panels
{
    namespace
    {
        using NLS::Windowing::Inputs::EKey;

        constexpr float kKeyHeight = 32.0f;
        constexpr float kKeyGap = 4.0f;
        constexpr float kKeyboardVirtualWidth = 720.0f;
        constexpr float kKeyboardVirtualHeight = 228.0f;

        bool IsCommandAssignedToKey(
            const std::vector<const Shortcuts::ShortcutCommand*>& p_commands,
            const EKey p_key)
        {
            const auto& shortcutService = NLS::Core::ServiceLocator::Get<Shortcuts::EditorShortcutService>();
            return std::any_of(
                p_commands.begin(),
                p_commands.end(),
                [&shortcutService, p_key](const Shortcuts::ShortcutCommand* p_command)
                {
                    const auto binding = shortcutService.GetBinding(p_command->id);
                    return binding.assigned && binding.primaryKey == p_key;
                });
        }

        bool IsGlobalKey(
            const std::vector<const Shortcuts::ShortcutCommand*>& p_commands,
            const EKey p_key)
        {
            const auto& shortcutService = NLS::Core::ServiceLocator::Get<Shortcuts::EditorShortcutService>();
            return std::any_of(
                p_commands.begin(),
                p_commands.end(),
                [&shortcutService, p_key](const Shortcuts::ShortcutCommand* p_command)
                {
                    const auto binding = shortcutService.GetBinding(p_command->id);
                    return binding.assigned &&
                        binding.primaryKey == p_key &&
                        p_command->context == Shortcuts::ShortcutContexts::Global;
                });
        }

        EKey KeyFromLabel(const char* p_label)
        {
            if (p_label[0] >= 'A' && p_label[0] <= 'Z' && p_label[1] == '\0')
                return static_cast<EKey>(static_cast<int>(EKey::KEY_A) + p_label[0] - 'A');
            if (p_label[0] >= '0' && p_label[0] <= '9' && p_label[1] == '\0')
                return static_cast<EKey>(static_cast<int>(EKey::KEY_0) + p_label[0] - '0');
            if (std::string_view(p_label) == "Esc")
                return EKey::KEY_ESCAPE;
            if (std::string_view(p_label) == "F5")
                return EKey::KEY_F5;
            if (std::string_view(p_label) == "F11")
                return EKey::KEY_F11;
            if (std::string_view(p_label) == "Del")
                return EKey::KEY_DELETE;
            return EKey::KEY_UNKNOWN;
        }

        void DrawKey(
            const char* p_label,
            const char* p_id,
            const float p_width,
            const float p_height,
            const std::vector<const Shortcuts::ShortcutCommand*>& p_commands)
        {
            const EKey key = KeyFromLabel(p_label);
            const bool assigned = key != EKey::KEY_UNKNOWN && IsCommandAssignedToKey(p_commands, key);
            const bool global = key != EKey::KEY_UNKNOWN && IsGlobalKey(p_commands, key);

            const ImVec4 normalColor = ImVec4(0.31f, 0.31f, 0.31f, 1.0f);
            const ImVec4 assignedColor = ImVec4(0.55f, 0.62f, 0.70f, 1.0f);
            const ImVec4 globalColor = ImVec4(0.70f, 0.61f, 0.57f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, global ? globalColor : (assigned ? assignedColor : normalColor));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, global ? globalColor : (assigned ? assignedColor : normalColor));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, global ? globalColor : (assigned ? assignedColor : normalColor));
            ImGui::PushID(p_id);
            ImGui::Button(p_label, ImVec2(p_width, p_height));
            ImGui::PopID();
            ImGui::PopStyleColor(3);
        }

        void DrawKeyAt(
            const ImVec2& p_origin,
            const float p_scale,
            const float p_x,
            const float p_y,
            const char* p_label,
            const char* p_id,
            const float p_width,
            const std::vector<const Shortcuts::ShortcutCommand*>& p_commands)
        {
            ImGui::SetCursorScreenPos(ImVec2(p_origin.x + p_x * p_scale, p_origin.y + p_y * p_scale));
            DrawKey(p_label, p_id, p_width * p_scale, kKeyHeight * p_scale, p_commands);
        }

        bool CommandMatchesCategory(
            const Shortcuts::ShortcutCommand& p_command,
            const std::string& p_selectedCategory)
        {
            return p_selectedCategory.empty() || p_selectedCategory == "All" || p_command.category == p_selectedCategory;
        }

        std::string ContextDisplayName(const Shortcuts::ShortcutContextId& p_context)
        {
            if (p_context == Shortcuts::ShortcutContexts::SceneView)
                return "Scene View";
            if (p_context == Shortcuts::ShortcutContexts::Hierarchy)
                return "Hierarchy";
            if (p_context == Shortcuts::ShortcutContexts::AssetBrowser)
                return "Asset Browser";
            if (p_context == Shortcuts::ShortcutContexts::Inspector)
                return "Inspector";
            if (p_context == Shortcuts::ShortcutContexts::GameView)
                return "Game View";
            if (p_context == Shortcuts::ShortcutContexts::Menu)
                return "Menu";
            if (p_context == Shortcuts::ShortcutContexts::TextInput)
                return "Text Input";
            return "Global";
        }
    }

    void ShortcutSettingsPanel::Open()
    {
        m_opened = true;
        NLS::Editor::Core::SetShortcutSettingsWindowBlocksSceneInput(true);
    }

    bool ShortcutSettingsPanel::IsOpened() const
    {
        return m_opened;
    }

    void ShortcutSettingsPanel::Draw()
    {
        NLS::Editor::Core::SetShortcutSettingsWindowBlocksSceneInput(m_opened);
        if (!m_opened)
            return;

        ImGui::SetNextWindowSize(ImVec2(780.0f, 620.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(760.0f, 560.0f), ImVec2(1100.0f, 800.0f));
        if (!ImGui::Begin("Shortcuts", &m_opened, ImGuiWindowFlags_NoDocking))
        {
            ImGui::End();
            NLS::Editor::Core::SetShortcutSettingsWindowBlocksSceneInput(m_opened);
            return;
        }

        auto& shortcutService = NLS::Core::ServiceLocator::Get<Shortcuts::EditorShortcutService>();
        const auto commands = shortcutService.GetCommands();
        if (m_selectedCategory.empty())
            m_selectedCategory = "All";

        DrawProfileRow();
        DrawKeyboardPreview(commands);
        ImGui::Spacing();

        const float searchWidth = 150.0f;
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - searchWidth);
        ImGui::SetNextItemWidth(searchWidth);
        char searchBuffer[128] = {};
        const auto copyLength = (std::min)(m_searchText.size(), sizeof(searchBuffer) - 1);
        std::copy_n(m_searchText.data(), copyLength, searchBuffer);
        if (ImGui::InputText("##ShortcutSearch", searchBuffer, sizeof(searchBuffer)))
            m_searchText = searchBuffer;

        const float bottomHeight = ImGui::GetContentRegionAvail().y;
        const float categoryWidth = 160.0f;
        ImGui::BeginChild("##ShortcutCategoryPane", ImVec2(categoryWidth, bottomHeight), true);
        DrawCategoryList(commands);
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##ShortcutCommandPane", ImVec2(0.0f, bottomHeight), true);
        DrawCommandList(commands);
        ImGui::EndChild();

        if (!m_recordingCommandId.empty())
        {
            const auto binding = CapturePressedBinding();
            if (binding.assigned)
            {
                const auto conflicts = shortcutService.ValidateBinding(m_recordingCommandId, binding);
                if (conflicts.empty() && shortcutService.SetBinding(m_recordingCommandId, binding))
                {
                    m_selectedCommandId = m_recordingCommandId;
                    m_recordingCommandId.clear();
                    m_conflictText.clear();
                }
                else if (!conflicts.empty())
                {
                    m_conflictText = conflicts.front().message;
                }
            }
        }

        ImGui::End();
        NLS::Editor::Core::SetShortcutSettingsWindowBlocksSceneInput(m_opened);
    }

    void ShortcutSettingsPanel::DrawProfileRow()
    {
        ImGui::SetNextItemWidth(115.0f);
        if (ImGui::BeginCombo("##ShortcutProfile", "Default"))
        {
            ImGui::Selectable("Default", true);
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        DrawLegend();
    }

    void ShortcutSettingsPanel::DrawLegend()
    {
        const auto legend = [](const ImVec4& p_color, const char* p_label)
        {
            ImGui::ColorButton(p_label, p_color, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(14.0f, 14.0f));
            ImGui::SameLine(0.0f, 5.0f);
            ImGui::TextUnformatted(p_label);
        };

        ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - 355.0f));
        legend(ImVec4(0.31f, 0.31f, 0.31f, 1.0f), "Unassigned Key");
        ImGui::SameLine(0.0f, 18.0f);
        legend(ImVec4(0.55f, 0.62f, 0.70f, 1.0f), "Assigned Key");
        ImGui::SameLine(0.0f, 18.0f);
        legend(ImVec4(0.70f, 0.61f, 0.57f, 1.0f), "Global Key");
    }

    void ShortcutSettingsPanel::DrawKeyboardPreview(const std::vector<const Shortcuts::ShortcutCommand*>& p_commands)
    {
        constexpr float key = 32.0f;
        constexpr float gap = 4.0f;
        constexpr float row = kKeyHeight + 6.0f;
        constexpr float mainX = 0.0f;
        constexpr float navX = 580.0f;
        constexpr float arrowX = 620.0f;

        const float availableWidth = ImGui::GetContentRegionAvail().x;
        const float scale = (std::min)(1.0f, availableWidth / kKeyboardVirtualWidth);
        const float previewHeight = kKeyboardVirtualHeight * scale + 8.0f;

        ImGui::BeginChild("##ShortcutKeyboard", ImVec2(0.0f, previewHeight), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 origin = ImGui::GetCursorScreenPos();

        DrawKeyAt(origin, scale, mainX, 0.0f, "Esc", "Esc", key, p_commands);
        for (int i = 1; i <= 8; ++i)
        {
            const auto label = "F" + std::to_string(i);
            DrawKeyAt(origin, scale, 90.0f + static_cast<float>(i - 1) * (key + gap), 0.0f, label.c_str(), label.c_str(), key, p_commands);
        }
        for (int i = 9; i <= 12; ++i)
        {
            const auto label = "F" + std::to_string(i);
            DrawKeyAt(origin, scale, 430.0f + static_cast<float>(i - 9) * (key + gap), 0.0f, label.c_str(), label.c_str(), key, p_commands);
        }

        const std::array<const char*, 13> numberRow = { "`", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=" };
        for (size_t i = 0; i < numberRow.size(); ++i)
            DrawKeyAt(origin, scale, mainX + static_cast<float>(i) * (key + gap), row, numberRow[i], numberRow[i], key, p_commands);
        DrawKeyAt(origin, scale, 13.0f * (key + gap), row, "Backspace", "Backspace", 74.0f, p_commands);
        DrawKeyAt(origin, scale, navX, row, "Ins", "Ins", key, p_commands);
        DrawKeyAt(origin, scale, navX + 38.0f, row, "Home", "Home", 42.0f, p_commands);
        DrawKeyAt(origin, scale, navX + 86.0f, row, "Pg\nUp", "PageUp", key, p_commands);

        DrawKeyAt(origin, scale, mainX, row * 2.0f, "Tab", "Tab", 72.0f, p_commands);
        const std::array<const char*, 10> topLetters = { "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P" };
        for (size_t i = 0; i < topLetters.size(); ++i)
            DrawKeyAt(origin, scale, 76.0f + static_cast<float>(i) * (key + gap), row * 2.0f, topLetters[i], topLetters[i], key, p_commands);
        DrawKeyAt(origin, scale, 76.0f + 10.0f * (key + gap), row * 2.0f, "[", "LeftBracket", key, p_commands);
        DrawKeyAt(origin, scale, 76.0f + 11.0f * (key + gap), row * 2.0f, "]", "RightBracket", key, p_commands);
        DrawKeyAt(origin, scale, 76.0f + 12.0f * (key + gap), row * 2.0f, "\\", "Backslash", 40.0f, p_commands);
        DrawKeyAt(origin, scale, navX, row * 2.0f, "Del", "Delete", key, p_commands);
        DrawKeyAt(origin, scale, navX + 38.0f, row * 2.0f, "End", "End", 42.0f, p_commands);
        DrawKeyAt(origin, scale, navX + 86.0f, row * 2.0f, "Pg\nDn", "PageDown", key, p_commands);

        DrawKeyAt(origin, scale, mainX, row * 3.0f, "Caps Lock", "CapsLock", 72.0f, p_commands);
        const std::array<const char*, 9> middleLetters = { "A", "S", "D", "F", "G", "H", "J", "K", "L" };
        for (size_t i = 0; i < middleLetters.size(); ++i)
            DrawKeyAt(origin, scale, 76.0f + static_cast<float>(i) * (key + gap), row * 3.0f, middleLetters[i], middleLetters[i], key, p_commands);
        DrawKeyAt(origin, scale, 76.0f + 9.0f * (key + gap), row * 3.0f, ";", "Semicolon", key, p_commands);
        DrawKeyAt(origin, scale, 76.0f + 10.0f * (key + gap), row * 3.0f, "'", "Apostrophe", key, p_commands);
        DrawKeyAt(origin, scale, 76.0f + 11.0f * (key + gap), row * 3.0f, "Return", "Return", 76.0f, p_commands);

        DrawKeyAt(origin, scale, mainX, row * 4.0f, "Shift", "LeftShift", 72.0f, p_commands);
        const std::array<const char*, 7> bottomLetters = { "Z", "X", "C", "V", "B", "N", "M" };
        for (size_t i = 0; i < bottomLetters.size(); ++i)
            DrawKeyAt(origin, scale, 76.0f + static_cast<float>(i) * (key + gap), row * 4.0f, bottomLetters[i], bottomLetters[i], key, p_commands);
        DrawKeyAt(origin, scale, 76.0f + 7.0f * (key + gap), row * 4.0f, ",", "Comma", key, p_commands);
        DrawKeyAt(origin, scale, 76.0f + 8.0f * (key + gap), row * 4.0f, ".", "Period", key, p_commands);
        DrawKeyAt(origin, scale, 76.0f + 9.0f * (key + gap), row * 4.0f, "/", "Slash", 64.0f, p_commands);
        DrawKeyAt(origin, scale, 76.0f + 9.0f * (key + gap) + 68.0f, row * 4.0f, "Shift", "RightShift", 72.0f, p_commands);
        DrawKeyAt(origin, scale, arrowX + 38.0f, row * 4.0f, "Up", "ArrowUp", key, p_commands);

        DrawKeyAt(origin, scale, mainX, row * 5.0f, "Control", "LeftControl", 64.0f, p_commands);
        DrawKeyAt(origin, scale, 68.0f, row * 5.0f, "Alt", "LeftAlt", 64.0f, p_commands);
        DrawKeyAt(origin, scale, 136.0f, row * 5.0f, "Space", "Space", 260.0f, p_commands);
        DrawKeyAt(origin, scale, 400.0f, row * 5.0f, "Alt", "RightAlt", 64.0f, p_commands);
        DrawKeyAt(origin, scale, 468.0f, row * 5.0f, "Control", "RightControl", 64.0f, p_commands);
        DrawKeyAt(origin, scale, arrowX, row * 5.0f, "Left", "ArrowLeft", key, p_commands);
        DrawKeyAt(origin, scale, arrowX + 38.0f, row * 5.0f, "Down", "ArrowDown", key, p_commands);
        DrawKeyAt(origin, scale, arrowX + 76.0f, row * 5.0f, "Right", "ArrowRight", key, p_commands);

        ImGui::Dummy(ImVec2(kKeyboardVirtualWidth * scale, kKeyboardVirtualHeight * scale));
        ImGui::EndChild();
    }

    void ShortcutSettingsPanel::DrawCategoryList(const std::vector<const Shortcuts::ShortcutCommand*>& p_commands)
    {
        ImGui::TextUnformatted("Category");
        ImGui::Separator();

        std::set<std::string> categories;
        for (const auto* command : p_commands)
            categories.insert(command->category);

        const bool allSelected = m_selectedCategory == "All";
        if (ImGui::Selectable("All", allSelected))
            m_selectedCategory = "All";

        for (const auto& category : categories)
        {
            const bool selected = m_selectedCategory == category;
            if (ImGui::Selectable(category.c_str(), selected))
                m_selectedCategory = category;
        }
    }

    void ShortcutSettingsPanel::DrawCommandList(const std::vector<const Shortcuts::ShortcutCommand*>& p_commands)
    {
        auto& shortcutService = NLS::Core::ServiceLocator::Get<Shortcuts::EditorShortcutService>();
        const auto queryMatches = shortcutService.SearchCommands(m_searchText);

        ImGui::Columns(2, "##ShortcutCommandColumns", true);
        ImGui::TextUnformatted("Command");
        ImGui::NextColumn();
        ImGui::TextUnformatted("Shortcut");
        ImGui::NextColumn();
        ImGui::Separator();

        for (const auto* command : queryMatches)
        {
            if (!CommandMatchesCategory(*command, m_selectedCategory))
                continue;

            constexpr ImGuiSelectableFlags selectableFlags =
                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick;
            const bool selected = m_selectedCommandId == command->id;
            const bool activated = ImGui::Selectable(
                (GetCommandListDisplayName(*command, queryMatches) + "##" + command->id).c_str(),
                selected,
                selectableFlags);
            const bool doubleClicked = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            if (activated || doubleClicked)
            {
                m_selectedCommandId = command->id;
                m_conflictText.clear();
                if (doubleClicked)
                {
                    shortcutService.ClearBinding(command->id);
                    m_recordingCommandId = command->id;
                }
            }
            ImGui::NextColumn();
            if (m_recordingCommandId == command->id)
                ImGui::TextColored(ImVec4(0.55f, 0.70f, 1.0f, 1.0f), "Press shortcut...");
            else
                ImGui::TextUnformatted(shortcutService.GetBindingDisplayText(command->id).c_str());
            ImGui::NextColumn();
        }

        ImGui::Columns(1);

        if (!m_conflictText.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "%s", m_conflictText.c_str());
    }

    std::string ShortcutSettingsPanel::GetCommandListDisplayName(
        const Shortcuts::ShortcutCommand& p_command,
        const std::vector<const Shortcuts::ShortcutCommand*>& p_visibleCommands)
    {
        const bool hasDuplicateDisplayName = std::any_of(
            p_visibleCommands.begin(),
            p_visibleCommands.end(),
            [&p_command](const Shortcuts::ShortcutCommand* p_other)
            {
                return p_other != nullptr &&
                    p_other->id != p_command.id &&
                    p_other->displayName == p_command.displayName;
            });

        if (!hasDuplicateDisplayName)
            return p_command.displayName;

        return p_command.displayName + " (" + ContextDisplayName(p_command.context) + ")";
    }

    Shortcuts::ShortcutBinding ShortcutSettingsPanel::CapturePressedBinding() const
    {
        using namespace Shortcuts;
        using Windowing::Inputs::EKey;

        const auto& io = ImGui::GetIO();
        ShortcutModifiers modifiers = 0;
        if (io.KeyCtrl)
            modifiers = modifiers | EShortcutModifier::Ctrl;
        if (io.KeyShift)
            modifiers = modifiers | EShortcutModifier::Shift;
        if (io.KeyAlt)
            modifiers = modifiers | EShortcutModifier::Alt;
        if (io.KeySuper)
            modifiers = modifiers | EShortcutModifier::Super;

        const auto makeBinding = [modifiers](const EKey p_key)
        {
            return ShortcutBinding::FromKey(p_key, modifiers);
        };

        for (int offset = 0; offset < 26; ++offset)
        {
            const auto imguiKey = static_cast<ImGuiKey>(ImGuiKey_A + offset);
            if (ImGui::IsKeyPressed(imguiKey))
                return makeBinding(static_cast<EKey>(static_cast<int>(EKey::KEY_A) + offset));
        }

        for (int offset = 0; offset < 10; ++offset)
        {
            const auto imguiKey = static_cast<ImGuiKey>(ImGuiKey_0 + offset);
            if (ImGui::IsKeyPressed(imguiKey))
                return makeBinding(static_cast<EKey>(static_cast<int>(EKey::KEY_0) + offset));
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            return makeBinding(EKey::KEY_ESCAPE);
        if (ImGui::IsKeyPressed(ImGuiKey_F5))
            return makeBinding(EKey::KEY_F5);
        if (ImGui::IsKeyPressed(ImGuiKey_F11))
            return makeBinding(EKey::KEY_F11);
        if (ImGui::IsKeyPressed(ImGuiKey_Delete))
            return makeBinding(EKey::KEY_DELETE);

        return ShortcutBinding::Unassigned();
    }
}
