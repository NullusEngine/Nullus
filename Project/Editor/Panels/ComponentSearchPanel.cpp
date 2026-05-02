#include "Panels/ComponentSearchPanel.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string_view>
#include <utility>

#include <imgui.h>

#include <Reflection/RuntimeMetaProperties.h>
#include <Reflection/TypeCreator.h>

#include "Components/CameraComponent.h"
#include "Components/Component.h"
#include "Components/LightComponent.h"
#include "Components/MaterialRenderer.h"
#include "Components/MeshRenderer.h"
#include "Components/SkyBoxComponent.h"
#include "Components/TransformComponent.h"
#include "GameObject.h"

namespace NLS::Editor::Panels
{
namespace
{
using NLS::Engine::Components::Component;
using NLS::Engine::Components::TransformComponent;

constexpr std::string_view kComponentSuffix = "Component";
constexpr std::string_view kPopupId = "##AddComponentPopup";

std::string GetSimpleTypeName(const NLS::meta::Type& p_type)
{
    std::string name = p_type.GetName();
    const size_t namespaceSeparator = name.rfind("::");
    if (namespaceSeparator != std::string::npos)
        name = name.substr(namespaceSeparator + 2);

    if (name.size() > kComponentSuffix.size()
        && name.compare(name.size() - kComponentSuffix.size(), kComponentSuffix.size(), kComponentSuffix) == 0)
    {
        name.erase(name.size() - kComponentSuffix.size());
    }

    return name;
}

std::string SplitCamelCase(std::string_view p_value)
{
    std::string result;
    result.reserve(p_value.size() + 4);

    for (size_t index = 0; index < p_value.size(); ++index)
    {
        const char current = p_value[index];
        const bool hasPrevious = index > 0;
        const bool hasNext = index + 1 < p_value.size();
        const char previous = hasPrevious ? p_value[index - 1] : '\0';
        const char next = hasNext ? p_value[index + 1] : '\0';

        const bool currentUpper = std::isupper(static_cast<unsigned char>(current)) != 0;
        const bool previousLower = hasPrevious && std::islower(static_cast<unsigned char>(previous)) != 0;
        const bool previousUpper = hasPrevious && std::isupper(static_cast<unsigned char>(previous)) != 0;
        const bool nextLower = hasNext && std::islower(static_cast<unsigned char>(next)) != 0;

        if (hasPrevious && currentUpper && (previousLower || (previousUpper && nextLower)))
            result.push_back(' ');

        result.push_back(current);
    }

    return result;
}

bool IsCandidateComponentType(const NLS::meta::Type& p_type)
{
    if (!p_type.IsValid() || !p_type.DerivesFrom(NLS_TYPEOF(Component)))
        return false;

    if (p_type == NLS_TYPEOF(Component) || p_type == NLS_TYPEOF(TransformComponent))
        return false;

    return !p_type.GetDynamicConstructors().empty();
}

std::vector<std::string> SplitMenuPath(std::string_view p_path)
{
    std::vector<std::string> tokens;
    size_t start = 0;
    while (start < p_path.size())
    {
        const size_t separator = p_path.find('/', start);
        const size_t length = separator == std::string_view::npos ? p_path.size() - start : separator - start;
        if (length > 0)
            tokens.emplace_back(p_path.substr(start, length));

        if (separator == std::string_view::npos)
            break;

        start = separator + 1;
    }

    return tokens;
}

ComponentCategoryNode* FindOrCreateChild(ComponentCategoryNode& p_parent, const std::string& p_label)
{
    const auto it = std::find_if(
        p_parent.children.begin(),
        p_parent.children.end(),
        [&p_label](const ComponentCategoryNode& p_node)
        {
            return p_node.label == p_label;
        });

    if (it != p_parent.children.end())
        return &(*it);

    ComponentCategoryNode node;
    node.label = p_label;
    node.fullPath = p_parent.fullPath.empty() ? p_label : (p_parent.fullPath + "/" + p_label);
    p_parent.children.push_back(std::move(node));
    return &p_parent.children.back();
}

void SortCategoryTree(std::vector<ComponentCategoryNode>& p_nodes)
{
    std::sort(
        p_nodes.begin(),
        p_nodes.end(),
        [](const ComponentCategoryNode& p_left, const ComponentCategoryNode& p_right)
        {
            return p_left.label < p_right.label;
        });

    for (ComponentCategoryNode& node : p_nodes)
    {
        std::sort(
            node.entries.begin(),
            node.entries.end(),
            [](const ComponentSearchEntry& p_left, const ComponentSearchEntry& p_right)
            {
                return p_left.displayName < p_right.displayName;
            });

        SortCategoryTree(node.children);
    }
}
} // namespace

ComponentSearchPanel::ComponentSearchPanel()
{
    lineBreak = true;
}

void ComponentSearchPanel::SetTargetActor(Engine::GameObject* p_actor)
{
    m_targetActor = p_actor;
    RefreshEntries();
}

void ComponentSearchPanel::OpenForActor(Engine::GameObject* p_actor)
{
    m_targetActor = p_actor;
    m_query.clear();
    std::memset(m_queryBuffer, 0, sizeof(m_queryBuffer));
    RefreshEntries();
    m_popupRequested = true;
}

void ComponentSearchPanel::RefreshEntries()
{
    m_entries = BuildComponentEntries(m_targetActor, m_query);
    m_categories = BuildCategoryTree(BuildComponentEntries(m_targetActor));

    if (!m_targetActor)
        SetStatusMessage("No actor selected", true);
    else if (GetViewModeForQuery(m_query) == ComponentPickerViewMode::SearchResults && m_entries.empty())
        SetStatusMessage("No components match your search", true);
    else if (m_categories.empty())
        SetStatusMessage("No components available", true);
    else
        SetStatusMessage({}, false);
}

void ComponentSearchPanel::ClearTarget()
{
    m_targetActor = nullptr;
    m_entries.clear();
    m_categories.clear();
    m_query.clear();
    std::memset(m_queryBuffer, 0, sizeof(m_queryBuffer));
    SetStatusMessage("No actor selected", true);
    ClosePopup();
}

void ComponentSearchPanel::NotifyActorComponentsChanged()
{
    RefreshEntries();
}

Engine::GameObject* ComponentSearchPanel::GetTargetActor() const
{
    return m_targetActor;
}

void ComponentSearchPanel::SetAnchorRect(const ImVec2& p_min, const ImVec2& p_max)
{
    m_anchorMinX = p_min.x;
    m_anchorMaxY = p_max.y;
}

std::string ComponentSearchPanel::NormalizeSearchText(std::string_view p_value)
{
    std::string normalized;
    normalized.reserve(p_value.size());

    for (char c : p_value)
    {
        if (std::isspace(static_cast<unsigned char>(c)) != 0)
            continue;

        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    return normalized;
}

std::string ComponentSearchPanel::MakeDisplayName(const meta::Type& p_type)
{
    if (!p_type.IsValid())
        return {};

    return SplitCamelCase(GetSimpleTypeName(p_type));
}

std::string ComponentSearchPanel::GetComponentMenuPath(const meta::Type& p_type)
{
    if (!p_type.IsValid())
        return {};

    if (const auto* menu = p_type.GetMeta().GetProperty<NLS::meta::ComponentMenu>())
    {
        if (!menu->path.empty())
            return menu->path;
    }

    return MakeDisplayName(p_type);
}

bool ComponentSearchPanel::IsTypeAddableToActor(const meta::Type& p_type, const Engine::GameObject* p_actor)
{
    if (!p_actor || !IsCandidateComponentType(p_type))
        return false;

    return p_actor->GetComponent(p_type, false) == nullptr;
}

std::vector<ComponentSearchEntry> ComponentSearchPanel::BuildComponentEntries(Engine::GameObject* p_actor, std::string_view p_query)
{
    std::vector<ComponentSearchEntry> entries;
    if (!p_actor)
        return entries;

    const std::string normalizedQuery = NormalizeSearchText(p_query);

    for (const meta::Type& type : meta::Type::GetTypes())
    {
        if (!IsCandidateComponentType(type))
            continue;

        ComponentSearchEntry entry;
        entry.componentType = type;
        entry.displayName = MakeDisplayName(type);
        entry.menuPath = GetComponentMenuPath(type);
        entry.searchKey = NormalizeSearchText(entry.displayName + " " + entry.menuPath);
        entry.isAddable = IsTypeAddableToActor(type, p_actor);
        if (!entry.isAddable)
            entry.availabilityReason = "Already added";

        if (!normalizedQuery.empty() && entry.searchKey.find(normalizedQuery) == std::string::npos)
            continue;

        entries.push_back(std::move(entry));
    }

    std::sort(
        entries.begin(),
        entries.end(),
        [](const ComponentSearchEntry& p_left, const ComponentSearchEntry& p_right)
        {
            if (p_left.displayName != p_right.displayName)
                return p_left.displayName < p_right.displayName;
            return p_left.menuPath < p_right.menuPath;
        });

    return entries;
}

std::vector<ComponentCategoryNode> ComponentSearchPanel::BuildCategoryTree(const std::vector<ComponentSearchEntry>& p_entries)
{
    ComponentCategoryNode root;
    for (const ComponentSearchEntry& entry : p_entries)
    {
        auto tokens = SplitMenuPath(entry.menuPath);
        if (tokens.empty())
            tokens = {entry.displayName};

        if (tokens.size() == 1)
        {
            ComponentCategoryNode* rootEntry = FindOrCreateChild(root, tokens.front());
            rootEntry->entries.push_back(entry);
            continue;
        }

        ComponentCategoryNode* current = &root;
        for (size_t index = 0; index + 1 < tokens.size(); ++index)
            current = FindOrCreateChild(*current, tokens[index]);

        current->entries.push_back(entry);
    }

    SortCategoryTree(root.children);
    return root.children;
}

ComponentPickerViewMode ComponentSearchPanel::GetViewModeForQuery(std::string_view p_query)
{
    return NormalizeSearchText(p_query).empty()
        ? ComponentPickerViewMode::Categories
        : ComponentPickerViewMode::SearchResults;
}

bool ComponentSearchPanel::TryAddComponentFromEntry(Engine::GameObject* p_actor, const ComponentSearchEntry& p_entry)
{
    if (!p_actor || !p_entry.componentType.IsValid() || !p_entry.isAddable)
        return false;

    if (!IsTypeAddableToActor(p_entry.componentType, p_actor))
        return false;

    return p_actor->AddComponent(p_entry.componentType) != nullptr;
}

void ComponentSearchPanel::_Draw_Impl()
{
    if (m_popupRequested)
    {
        ImGui::SetNextWindowPos(ImVec2{m_anchorMinX, m_anchorMaxY});
        ImGui::OpenPopup(kPopupId.data());
        m_popupRequested = false;
    }

    ImGui::SetNextWindowPos(ImVec2{m_anchorMinX, m_anchorMaxY}, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2{340.0f, 420.0f}, ImGuiCond_Appearing);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoSavedSettings;

    if (!ImGui::BeginPopup(kPopupId.data(), flags))
    {
        m_popupOpen = false;
        return;
    }

    m_popupOpen = true;

    if (ImGui::InputText("##ComponentSearchQuery", m_queryBuffer, sizeof(m_queryBuffer)))
    {
        SyncQueryFromBuffer();
        RefreshEntries();
    }

    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere(-1);

    ImGui::Separator();

    if (!m_statusMessage.empty())
    {
        if (m_statusIsError)
            ImGui::TextColored(ImVec4{0.96f, 0.56f, 0.56f, 1.0f}, "%s", m_statusMessage.c_str());
        else
            ImGui::TextUnformatted(m_statusMessage.c_str());
    }

    if (GetViewModeForQuery(m_query) == ComponentPickerViewMode::Categories)
        DrawCategoryNodes(m_categories);
    else
        DrawSearchResults();

    ImGui::EndPopup();
}

void ComponentSearchPanel::ClosePopup()
{
    if (m_popupOpen)
        ImGui::CloseCurrentPopup();

    m_popupOpen = false;
}

bool ComponentSearchPanel::TryCommitEntry(const ComponentSearchEntry& p_entry)
{
    if (TryAddComponentFromEntry(m_targetActor, p_entry))
    {
        SetStatusMessage("Added " + p_entry.displayName, false);
        RefreshEntries();
        ComponentAddedEvent.Invoke();
        ClosePopup();
        return true;
    }

    SetStatusMessage(
        p_entry.isAddable ? ("Failed to add " + p_entry.displayName)
                          : (p_entry.availabilityReason.empty() ? "Component is not available" : p_entry.availabilityReason),
        true);
    RefreshEntries();
    return false;
}

void ComponentSearchPanel::DrawCategoryNodes(const std::vector<ComponentCategoryNode>& p_nodes)
{
    for (const ComponentCategoryNode& node : p_nodes)
        DrawCategoryNode(node);
}

void ComponentSearchPanel::DrawCategoryNode(const ComponentCategoryNode& p_node)
{
    if (!p_node.children.empty())
    {
        if (ImGui::BeginMenu(p_node.label.c_str()))
        {
            for (const ComponentCategoryNode& child : p_node.children)
                DrawCategoryNode(child);

            for (const ComponentSearchEntry& entry : p_node.entries)
            {
                if (ImGui::MenuItem(entry.displayName.c_str(), nullptr, false, entry.isAddable))
                    TryCommitEntry(entry);
            }

            ImGui::EndMenu();
        }
        return;
    }

    if (!p_node.entries.empty())
    {
        if (p_node.entries.size() == 1)
        {
            const ComponentSearchEntry& entry = p_node.entries.front();
            if (ImGui::MenuItem(p_node.label.c_str(), nullptr, false, entry.isAddable))
                TryCommitEntry(entry);
            return;
        }

        if (ImGui::BeginMenu(p_node.label.c_str()))
        {
            for (const ComponentSearchEntry& entry : p_node.entries)
            {
                if (ImGui::MenuItem(entry.displayName.c_str(), nullptr, false, entry.isAddable))
                    TryCommitEntry(entry);
            }

            ImGui::EndMenu();
        }
    }
}

void ComponentSearchPanel::DrawSearchResults()
{
    for (const ComponentSearchEntry& entry : m_entries)
    {
        const std::string label = entry.menuPath + "##" + entry.displayName;
        if (ImGui::MenuItem(label.c_str(), nullptr, false, entry.isAddable))
            TryCommitEntry(entry);
    }
}

void ComponentSearchPanel::SyncQueryFromBuffer()
{
    m_query = m_queryBuffer;
}

void ComponentSearchPanel::SetStatusMessage(std::string p_message, bool p_isError)
{
    m_statusMessage = std::move(p_message);
    m_statusIsError = p_isError;
}
} // namespace NLS::Editor::Panels
