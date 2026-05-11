#include "Panels/ProjectSettings.h"

#include <Reflection/Field.h>
#include <Reflection/RuntimeMetaProperties.h>
#include <Reflection/Variant.h>
#include <ServiceLocator.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstring>

#include "../Core/Context.h"
#include "Core/EditorInteractionBlocker.h"
#include "Panels/ReflectedPropertyDrawer.h"
#include "Settings/EditorSettings.h"
#include "Settings/EditorSettingsPersistence.h"

namespace
{
constexpr float kSettingsWidth = 760.0f;
constexpr float kSettingsHeight = 560.0f;
constexpr float kCategoryWidth = 210.0f;

void CopyToBuffer(const std::string& p_value, char (&p_buffer)[128])
{
    std::fill(std::begin(p_buffer), std::end(p_buffer), '\0');
    const auto copyLength = (std::min)(p_value.size(), sizeof(p_buffer) - 1);
    if (copyLength > 0)
        std::memcpy(p_buffer, p_value.data(), copyLength);
}

bool RequiresRestart(const NLS::meta::Field& p_field)
{
    return p_field.GetMeta().GetProperty<NLS::meta::RequiresRestart>() != nullptr;
}
}

namespace NLS::Editor::Panels
{
ProjectSettings::ProjectSettings(
    const std::string& p_title,
    bool p_opened,
    const UI::PanelWindowSettings& p_windowSettings)
    : UI::PanelWindow(p_title, false, p_windowSettings),
      m_opened(p_opened)
{
}

void ProjectSettings::Open()
{
    EnsureInitialized();
    if (!m_opened)
    {
        m_opened = true;
        Core::SetSettingsWindowBlocksSceneInput(true);
    }
}

void ProjectSettings::Close()
{
    if (m_opened)
    {
        SaveIfDirty();
        m_opened = false;
        Core::SetSettingsWindowBlocksSceneInput(false);
    }
}

void ProjectSettings::DrawModal()
{
    DrawSettingsModal();
}

bool ProjectSettings::IsModalOpen() const
{
    return m_opened;
}

std::string ProjectSettings::ChooseSelectionAfterSearch(
    const Settings::EditorSettingsRegistry& p_registry,
    const std::string& p_currentSelection,
    const std::string& p_searchText)
{
    const auto visible = p_registry.Search(p_searchText);
    if (visible.empty())
        return {};

    const auto currentIt = std::find_if(
        visible.begin(),
        visible.end(),
        [&p_currentSelection](const Settings::EditorSettingObject* p_object)
        {
            return p_object != nullptr && p_object->id == p_currentSelection;
        });
    if (currentIt != visible.end())
        return p_currentSelection;

    return visible.front()->id;
}

void ProjectSettings::_Draw_Impl()
{
    DrawSettingsModal();
}

void ProjectSettings::EnsureInitialized()
{
    if (m_initialized)
        return;

    Settings::EditorSettings::RegisterSettingObjects(m_registry);
    Settings::EditorSettingsPersistence::Load(GetSettingsPath(), m_registry);
    m_selectedSettingId = ChooseSelectionAfterSearch(m_registry, m_selectedSettingId, m_searchText);
    m_initialized = true;
}

void ProjectSettings::DrawSettingsModal()
{
    Core::SetSettingsWindowBlocksSceneInput(m_opened);
    if (!m_opened)
        return;

    EnsureInitialized();
    ImGui::SetNextWindowSize(ImVec2(kSettingsWidth, kSettingsHeight), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(680.0f, 460.0f), ImVec2(1100.0f, 820.0f));
    if (!ImGui::Begin("Settings", &m_opened, ImGuiWindowFlags_NoDocking))
    {
        ImGui::End();
        Core::SetSettingsWindowBlocksSceneInput(m_opened);
        if (!m_opened)
            SaveIfDirty();
        return;
    }

    DrawSearchRow();
    ImGui::Separator();

    const auto visibleObjects = m_registry.Search(m_searchText);
    m_selectedSettingId = ChooseSelectionAfterSearch(m_registry, m_selectedSettingId, m_searchText);

    const float contentHeight = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##SettingsCategories", ImVec2(kCategoryWidth, contentHeight), true);
    DrawCategoryList(visibleObjects);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##SettingsDetails", ImVec2(0.0f, contentHeight), true);
    DrawSelectedSettings();
    ImGui::EndChild();

    ImGui::End();

    if (!m_opened)
        SaveIfDirty();
    Core::SetSettingsWindowBlocksSceneInput(m_opened);
}

void ProjectSettings::DrawSearchRow()
{
    char searchBuffer[128];
    CopyToBuffer(m_searchText, searchBuffer);

    ImGui::SetNextItemWidth((std::min)(360.0f, ImGui::GetContentRegionAvail().x));
    if (ImGui::InputTextWithHint("##SettingsSearch", "Search", searchBuffer, sizeof(searchBuffer)))
        m_searchText = searchBuffer;
}

void ProjectSettings::DrawCategoryList(const std::vector<const Settings::EditorSettingObject*>& p_visibleObjects)
{
    if (p_visibleObjects.empty())
    {
        ImGui::TextUnformatted("No results");
        return;
    }

    std::string lastCategory;
    for (const auto* object : p_visibleObjects)
    {
        if (object == nullptr)
            continue;

        if (lastCategory != object->categoryPath)
        {
            if (!lastCategory.empty())
                ImGui::Spacing();
            lastCategory = object->categoryPath;
            ImGui::TextColored(ImVec4(0.72f, 0.72f, 0.72f, 1.0f), "%s", lastCategory.c_str());
        }

        const bool selected = m_selectedSettingId == object->id;
        if (ImGui::Selectable((object->displayName + "##" + object->id).c_str(), selected))
            m_selectedSettingId = object->id;
    }
}

void ProjectSettings::DrawSelectedSettings()
{
    auto* selected = m_registry.Find(m_selectedSettingId);
    if (selected == nullptr)
    {
        RemoveAllWidgets();
        m_reflectedWidgetsSelectionId.clear();
        m_reflectedWidgetsSearchText.clear();
        ImGui::TextUnformatted("No settings selected");
        return;
    }

    ImGui::TextUnformatted(selected->displayName.c_str());
    ImGui::Separator();

    const bool needsRebuild =
        m_reflectedWidgetsSelectionId != selected->id
        || m_reflectedWidgetsSearchText != m_searchText
        || GetWidgets().empty();

    if (needsRebuild)
    {
        RemoveAllWidgets();

        auto instance = selected->makeVariant();
        ReflectedPropertyDrawerOptions options;
        options.labelWidth = 180.0f;
        options.searchText = m_searchText;
        options.fieldBadgeProvider = [](const meta::Field& field)
        {
            return RequiresRestart(field) ? std::string("Requires restart") : std::string{};
        };
        options.onFieldChanged = [this, id = selected->id](const meta::Field& field)
        {
            m_dirtySettings.insert(id);
            if (RequiresRestart(field))
                m_restartRequiredFields.insert(id + "." + field.GetName());
            ApplyLiveSettings();
        };
        DrawReflectedObject(*this, instance, options);
        m_reflectedWidgetsSelectionId = selected->id;
        m_reflectedWidgetsSearchText = m_searchText;
    }

    DrawWidgets();

    if (!m_restartRequiredFields.empty())
    {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(
            ImVec4(1.0f, 0.68f, 0.26f, 1.0f),
            "Some changes require restart to take effect.");
    }
}

void ProjectSettings::ApplyLiveSettings()
{
    if (NLS::Core::ServiceLocator::Contains<NLS::Editor::Core::Context>())
        NLS::Core::ServiceLocator::Get<NLS::Editor::Core::Context>().ApplyEditorSettings();
}

void ProjectSettings::SaveIfDirty()
{
    if (m_dirtySettings.empty())
        return;

    Settings::EditorSettingsPersistence::Save(GetSettingsPath(), m_registry);
    m_dirtySettings.clear();
}

std::filesystem::path ProjectSettings::GetSettingsPath() const
{
    if (NLS::Core::ServiceLocator::Contains<NLS::Editor::Core::Context>())
    {
        const auto& context = NLS::Core::ServiceLocator::Get<NLS::Editor::Core::Context>();
        return std::filesystem::path(context.projectPath) / "UserSettings" / "editor-settings.json";
    }

    return std::filesystem::path("UserSettings") / "editor-settings.json";
}
}
