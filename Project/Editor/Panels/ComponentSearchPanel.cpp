#include "Panels/ComponentSearchPanel.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cstring>
#include <string_view>
#include <utility>

#include <imgui.h>

#include <Reflection/RuntimeMetaProperties.h>
#include <Reflection/TypeCreator.h>
#include <ServiceLocator.h>
#include <Scripting/ScriptComponent.h>
#include <Scripting/ScriptRuntime.h>

#include "Assets/BuiltInScriptRegistry.h"
#include "Assets/ScriptAssetUtility.h"
#include "Core/Context.h"
#include "Core/EditorActions.h"
#include "Inspector/ComponentStructureUndo.h"
#include "Inspector/InspectorUndo.h"
#include "Components/CameraComponent.h"
#include "Components/Component.h"
#include "Components/LightComponent.h"
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
using NLS::Scripting::ScriptComponent;

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

    if (p_type == NLS_TYPEOF(Component)
        || p_type == NLS_TYPEOF(TransformComponent)
        // ScriptComponent is the shared native implementation for concrete
        // C#/Lua assets, not an empty component users should add directly.
        || p_type == NLS_TYPEOF(ScriptComponent))
        return false;

    return !p_type.GetDynamicConstructors().empty();
}

bool DisallowsMultiple(const NLS::meta::Type& p_type)
{
    return p_type.IsValid() &&
        p_type.GetMeta().GetProperty<NLS::meta::DisallowMultipleComponent>() != nullptr;
}

std::vector<NLS::meta::Type> ResolveRequiredComponentTypes(const NLS::meta::Type& p_type)
{
    std::vector<NLS::meta::Type> result;
    const auto* requirement = p_type.IsValid()
        ? p_type.GetMeta().GetProperty<NLS::meta::RequireComponent>()
        : nullptr;
    if (requirement == nullptr || requirement->typeNames.empty())
        return result;

    size_t begin = 0u;
    while (begin < requirement->typeNames.size())
    {
        const size_t end = requirement->typeNames.find_first_of(";,", begin);
        const auto token = requirement->typeNames.substr(begin, end == std::string::npos
            ? std::string::npos
            : end - begin);
        if (!token.empty())
        {
            const auto type = NLS::meta::Type::GetFromName(token);
            if (type.IsValid() && type.DerivesFrom(NLS_TYPEOF(Component)))
                result.push_back(type);
        }
        if (end == std::string::npos)
            break;
        begin = end + 1u;
    }
    return result;
}

bool EnsureRequiredComponents(
    NLS::Engine::GameObject& p_gameObject,
    const NLS::meta::Type& p_type,
    std::vector<NLS::meta::Type>& p_visiting)
{
    if (std::find(p_visiting.begin(), p_visiting.end(), p_type) != p_visiting.end())
        return false;

    p_visiting.push_back(p_type);
    for (const auto& requiredType : ResolveRequiredComponentTypes(p_type))
    {
        if (p_gameObject.GetComponent(requiredType, false) != nullptr)
            continue;

        if (!EnsureRequiredComponents(p_gameObject, requiredType, p_visiting) ||
            p_gameObject.AddComponent(requiredType) == nullptr)
        {
            p_visiting.pop_back();
            return false;
        }
    }
    p_visiting.pop_back();
    return true;
}

bool ComponentMatchesRequiredType(const NLS::Engine::Components::Component& p_component, const NLS::meta::Type& p_requiredType)
{
    const auto type = p_component.GetType();
    return type == p_requiredType || type.DerivesFrom(p_requiredType);
}

bool HasScriptAsset(
    const NLS::Engine::GameObject* p_gameObject,
    const NLS::Scripting::ScriptAsset& p_asset)
{
    if (!p_gameObject || !p_asset.assetId.IsValid())
        return false;

    for (const auto& component : p_gameObject->GetComponents())
    {
        const auto* script = component
            ? dynamic_cast<const ScriptComponent*>(component.get())
            : nullptr;
        if (script != nullptr && script->GetScriptAsset().assetId == p_asset.assetId)
            return true;
    }
    return false;
}

std::string ScriptLanguageLabel(const NLS::Scripting::ScriptLanguage p_language)
{
    return p_language == NLS::Scripting::ScriptLanguage::CSharp ? "C#" : "Lua";
}

std::string ScriptDisplayName(const NLS::Scripting::ScriptAsset& p_asset)
{
    const auto stem = std::filesystem::path(p_asset.sourcePath).stem().string();
    const auto name = stem.empty() ? p_asset.sourcePath : stem;
    return name + " (" + ScriptLanguageLabel(p_asset.language) + ")";
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

std::string TruncateUtf8ForWidth(std::string_view p_text, const float p_width)
{
    if (p_width <= 0.0f || ImGui::CalcTextSize(p_text.data(), p_text.data() + p_text.size()).x <= p_width)
        return std::string(p_text);

    constexpr std::string_view suffix = "...";
    std::string result;
    result.reserve(p_text.size());
    for (size_t offset = 0u; offset < p_text.size();)
    {
        const unsigned char lead = static_cast<unsigned char>(p_text[offset]);
        const size_t length = (lead & 0x80u) == 0u ? 1u :
            (lead & 0xe0u) == 0xc0u ? 2u :
            (lead & 0xf0u) == 0xe0u ? 3u :
            (lead & 0xf8u) == 0xf0u ? 4u : 1u;
        if (offset + length > p_text.size())
            break;

        const auto candidate = result + std::string(p_text.substr(offset, length)) + std::string(suffix);
        if (ImGui::CalcTextSize(candidate.c_str()).x > p_width)
            break;

        result += p_text.substr(offset, length);
        offset += length;
    }

    return result.empty() ? std::string(suffix) : result + std::string(suffix);
}

std::string GetEntryCategoryPath(const ComponentSearchEntry& p_entry)
{
    if (p_entry.menuPath.empty() || p_entry.menuPath == p_entry.displayName)
        return {};

    const std::string entrySuffix = "/" + p_entry.displayName;
    if (p_entry.menuPath.size() > entrySuffix.size() &&
        p_entry.menuPath.compare(
            p_entry.menuPath.size() - entrySuffix.size(),
            entrySuffix.size(),
            entrySuffix) == 0)
    {
        return p_entry.menuPath.substr(0u, p_entry.menuPath.size() - entrySuffix.size());
    }

    return p_entry.menuPath;
}

float GetComponentPickerUiScale()
{
    if (NLS::Core::ServiceLocator::Contains<NLS::Editor::Core::Context>())
    {
        const auto& context = NLS::Core::ServiceLocator::Get<NLS::Editor::Core::Context>();
        if (context.uiManager)
            return (std::max)(0.75f, context.uiManager->GetScale());
    }

    return 1.0f;
}

struct ComponentPickerPopupPlacement
{
    ImVec2 position;
    ImVec2 pivot;
    ImVec2 size;
};

ComponentPickerPopupPlacement GetComponentPickerPopupPlacement(
    const float p_anchorMinX,
    const float p_anchorMinY,
    const float p_anchorMaxX,
    const float p_anchorMaxY)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float scale = GetComponentPickerUiScale();
    const float margin = 8.0f * scale;
    const float availableWidth = (std::max)(1.0f, viewport->WorkSize.x - margin * 2.0f);
    const float popupWidth = (std::min)(360.0f * scale, availableWidth);
    const float desiredHeight = 430.0f * scale;
    // Reserve both a viewport safety inset and a gap to the button.  This
    // lets the first popup frame be positioned directly instead of relying
    // on ImGui's deferred pivot layout, which briefly overlaps the trigger.
    const float minimumY = viewport->WorkPos.y + margin;
    const float maximumY = viewport->WorkPos.y + viewport->WorkSize.y - margin;
    const float aboveHeight = (std::max)(0.0f, p_anchorMinY - margin - minimumY);
    const float belowHeight = (std::max)(0.0f, maximumY - (p_anchorMaxY + margin));
    const bool openAbove = belowHeight < desiredHeight && aboveHeight > belowHeight;
    const float popupHeight = (std::max)(1.0f, (std::min)(desiredHeight, openAbove ? aboveHeight : belowHeight));

    const float minimumX = viewport->WorkPos.x + margin;
    const float maximumX = viewport->WorkPos.x + viewport->WorkSize.x - margin - popupWidth;
    const float centeredX = (p_anchorMinX + p_anchorMaxX - popupWidth) * 0.5f;
    const float x = (std::clamp)(centeredX, minimumX, maximumX);
    const float y = openAbove
        ? p_anchorMinY - margin - popupHeight
        : p_anchorMaxY + margin;
    return {
        ImVec2(x, y),
        ImVec2(0.0f, 0.0f),
        ImVec2(popupWidth, popupHeight)};
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

void ComponentSearchPanel::SetTargetGameObject(Engine::GameObject* p_GameObject)
{
    if (m_targetGameObject == p_GameObject)
        return;

    if (p_GameObject == nullptr)
    {
        ClearTarget();
        return;
    }

    m_targetGameObject = p_GameObject;
    RefreshEntries();
}

void ComponentSearchPanel::OpenForGameObject(Engine::GameObject* p_GameObject)
{
    m_targetGameObject = p_GameObject;
    m_query.clear();
    std::memset(m_queryBuffer, 0, sizeof(m_queryBuffer));
    RefreshEntries();
    m_popupDismissRequested = false;
    m_popupRequested = true;
}

void ComponentSearchPanel::RefreshEntries()
{
    std::filesystem::path projectAssetsFolder;
    const NLS::Scripting::ScriptRuntime* scriptRuntime = nullptr;
    if (NLS::Core::ServiceLocator::Contains<NLS::Editor::Core::Context>())
    {
        const auto& context = NLS::Core::ServiceLocator::Get<NLS::Editor::Core::Context>();
        projectAssetsFolder = context.projectAssetsPath;
        scriptRuntime = &context.scriptRuntime;
    }
    else if (NLS::Core::ServiceLocator::Contains<NLS::Editor::Core::EditorActions>())
    {
        projectAssetsFolder = std::filesystem::path(EDITOR_CONTEXT(projectAssetsPath));
        scriptRuntime = &EDITOR_CONTEXT(scriptRuntime);
    }

    m_entries = BuildComponentEntries(m_targetGameObject, m_query, projectAssetsFolder, scriptRuntime);
    m_categories = NormalizeSearchText(m_query).empty()
        ? BuildCategoryTree(m_entries)
        : std::vector<ComponentCategoryNode> {};

    if (!m_targetGameObject)
        SetStatusMessage("No GameObject selected", true);
    else if (GetViewModeForQuery(m_query) == ComponentPickerViewMode::SearchResults && m_entries.empty())
        SetStatusMessage("No components match your search", true);
    else if (GetViewModeForQuery(m_query) == ComponentPickerViewMode::Categories && m_categories.empty())
        SetStatusMessage("No components available", true);
    else
        SetStatusMessage({}, false);
}

void ComponentSearchPanel::ClearTarget()
{
    m_targetGameObject = nullptr;
    m_entries.clear();
    m_categories.clear();
    m_query.clear();
    std::memset(m_queryBuffer, 0, sizeof(m_queryBuffer));
    SetStatusMessage("No GameObject selected", true);
    m_popupRequested = false;
    m_popupDismissRequested = true;
    m_popupOpen = false;
}

void ComponentSearchPanel::NotifyGameObjectComponentsChanged()
{
    RefreshEntries();
}

Engine::GameObject* ComponentSearchPanel::GetTargetGameObject() const
{
    return m_targetGameObject;
}

void ComponentSearchPanel::SetAnchorRect(const ImVec2& p_min, const ImVec2& p_max)
{
    m_anchorMinX = p_min.x;
    m_anchorMinY = p_min.y;
    m_anchorMaxX = p_max.x;
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

bool ComponentSearchPanel::IsTypeAddableToGameObject(const meta::Type& p_type, const Engine::GameObject* p_GameObject)
{
    return GetAddabilityReason(p_type, p_GameObject).empty();
}

std::vector<meta::Type> ComponentSearchPanel::GetRequiredComponentTypes(const meta::Type& p_type)
{
    return ResolveRequiredComponentTypes(p_type);
}

std::string ComponentSearchPanel::GetAddabilityReason(const meta::Type& p_type, const Engine::GameObject* p_gameObject)
{
    if (p_gameObject == nullptr)
        return "No GameObject selected";
    if (!IsCandidateComponentType(p_type))
        return "This type cannot be added as a component";
    if (DisallowsMultiple(p_type) && p_gameObject->GetComponent(p_type, false) != nullptr)
        return "This component does not allow multiple instances";

    const auto* requirement = p_type.GetMeta().GetProperty<NLS::meta::RequireComponent>();
    if (requirement != nullptr && !requirement->typeNames.empty() && ResolveRequiredComponentTypes(p_type).empty())
        return "One or more required component types are unavailable";
    return {};
}

std::string ComponentSearchPanel::GetRemovalBlockReason(const Engine::Components::Component* p_component)
{
    if (p_component == nullptr || p_component->gameobject() == nullptr)
        return "The component no longer belongs to a GameObject";

    const auto* owner = p_component->gameobject();
    const auto targetType = p_component->GetType();
    for (const auto& candidate : owner->GetComponents())
    {
        const auto* dependent = candidate.get();
        if (dependent == nullptr || dependent == p_component)
            continue;

        for (const auto& requiredType : ResolveRequiredComponentTypes(dependent->GetType()))
        {
            if (targetType != requiredType && !targetType.DerivesFrom(requiredType))
                continue;

            const bool hasAlternateProvider = std::any_of(
                owner->GetComponents().begin(), owner->GetComponents().end(),
                [p_component, &requiredType](const auto& other)
                {
                    return other != nullptr && other.get() != p_component &&
                        ComponentMatchesRequiredType(*other, requiredType);
                });
            if (!hasAlternateProvider)
                return MakeDisplayName(dependent->GetType()) + " requires " + MakeDisplayName(requiredType);
        }
    }
    return {};
}

std::vector<ComponentSearchEntry> ComponentSearchPanel::BuildComponentEntries(
    Engine::GameObject* p_GameObject,
    std::string_view p_query,
    const std::filesystem::path& p_projectAssetsFolder,
    const NLS::Scripting::ScriptRuntime* p_scriptRuntime)
{
    std::vector<ComponentSearchEntry> entries;
    if (!p_GameObject)
        return entries;

    const std::string normalizedQuery = NormalizeSearchText(p_query);

    const auto appendEntry = [&](const meta::Type& type)
    {
        if (!IsCandidateComponentType(type))
            return;

        if (std::any_of(
                entries.begin(),
                entries.end(),
                [&type](const ComponentSearchEntry& entry)
                {
                    return entry.componentType == type;
                }))
        {
            return;
        }

        ComponentSearchEntry entry;
        entry.componentType = type;
        entry.displayName = MakeDisplayName(type);
        entry.menuPath = GetComponentMenuPath(type);
        entry.searchKey = NormalizeSearchText(entry.displayName + " " + entry.menuPath);
        entry.availabilityReason = GetAddabilityReason(type, p_GameObject);
        entry.isAddable = entry.availabilityReason.empty();

        if (!normalizedQuery.empty() && entry.searchKey.find(normalizedQuery) == std::string::npos)
            return;

        entries.push_back(std::move(entry));
    };

    for (const meta::Type& type : meta::Type::GetTypes())
        appendEntry(type);

    // ScriptComponent is the shared native type for concrete script assets.
    // Keep it out of the generic list; the entries below carry the actual
    // C#/Lua asset that should be mounted on the GameObject.
    const auto scriptType = meta::Type::GetFromName("NLS::Scripting::ScriptComponent");
    if (scriptType.IsValid())
    {
        const auto appendScriptEntry = [&](NLS::Scripting::ScriptAsset asset, std::string menuPath)
        {
            if (p_scriptRuntime)
            {
                if (const auto semanticComponent = p_scriptRuntime->IsScriptComponentAsset(asset); semanticComponent.has_value())
                    asset.isComponent = *semanticComponent;
            }
            if (!asset.isComponent)
                return;

            if (std::any_of(
                    entries.begin(),
                    entries.end(),
                    [&asset](const ComponentSearchEntry& entry)
                    {
                        if (!entry.scriptAsset.has_value())
                            return false;
                        if (asset.assetId.IsValid() && entry.scriptAsset->assetId.IsValid())
                            return entry.scriptAsset->assetId == asset.assetId;
                        return entry.scriptAsset->language == asset.language &&
                            entry.scriptAsset->sourcePath == asset.sourcePath;
                    }))
            {
                return;
            }

            ComponentSearchEntry entry;
            entry.componentType = scriptType;
            entry.scriptAsset = std::move(asset);
            entry.displayName = ScriptDisplayName(*entry.scriptAsset);
            entry.menuPath = menuPath.empty()
                ? "Scripts/" + ScriptLanguageLabel(entry.scriptAsset->language)
                : std::move(menuPath);
            entry.searchKey = NormalizeSearchText(
                entry.displayName + " " + entry.menuPath + " " + entry.scriptAsset->sourcePath);
            // Script assets are repeatable by default. A future script-side
            // DisallowMultiple declaration can feed the same metadata path as
            // native components without changing this picker contract.
            entry.isAddable = true;

            if (!normalizedQuery.empty() && entry.searchKey.find(normalizedQuery) == std::string::npos)
                return;
            entries.push_back(std::move(entry));
        };

        for (auto& asset : NLS::Editor::Assets::FindProjectScriptAssets(p_projectAssetsFolder))
            appendScriptEntry(std::move(asset), {});

        // Built-in scripts use the same concrete ScriptComponent entry as
        // project scripts.  Their descriptor owns a stable AssetId and source
        // payload, so selecting one is indistinguishable from selecting an
        // imported project asset for mounting and serialization.
        for (const auto& descriptor : NLS::Editor::Assets::BuiltInScriptRegistry::GetAll())
            appendScriptEntry(descriptor.asset, descriptor.menuPath);
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

bool ComponentSearchPanel::TryAddComponentFromEntry(Engine::GameObject* p_GameObject, const ComponentSearchEntry& p_entry)
{
    if (!p_GameObject || !p_entry.componentType.IsValid() || !p_entry.isAddable)
        return false;

    if (p_entry.scriptAsset.has_value())
    {
        if (!p_entry.scriptAsset->isComponent || !p_entry.scriptAsset->assetId.IsValid())
            return false;

        auto* component = p_GameObject->AddComponent(p_entry.componentType);
        auto* script = component ? dynamic_cast<ScriptComponent*>(component) : nullptr;
        if (!script)
        {
            if (component)
                p_GameObject->RemoveComponent(component);
            return false;
        }
        script->SetScriptAsset(*p_entry.scriptAsset);
        return true;
    }

    if (!IsTypeAddableToGameObject(p_entry.componentType, p_GameObject))
        return false;

    std::vector<meta::Type> visiting;
    if (!EnsureRequiredComponents(*p_GameObject, p_entry.componentType, visiting))
        return false;

    return p_GameObject->AddComponent(p_entry.componentType) != nullptr;
}

void ComponentSearchPanel::_Draw_Impl()
{
    if (m_popupRequested)
    {
        ImGui::OpenPopup(kPopupId.data());
        m_popupRequested = false;
    }

    const auto placement = GetComponentPickerPopupPlacement(
        m_anchorMinX,
        m_anchorMinY,
        m_anchorMaxX,
        m_anchorMaxY);
    ImGui::SetNextWindowPos(placement.position, ImGuiCond_Appearing, placement.pivot);
    ImGui::SetNextWindowSize(placement.size, ImGuiCond_Appearing);
    ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);

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

    if (m_popupDismissRequested || m_targetGameObject == nullptr)
    {
        m_popupDismissRequested = false;
        m_popupOpen = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint(
            "##ComponentSearchQuery",
            "Search components",
            m_queryBuffer,
            sizeof(m_queryBuffer)))
    {
        SyncQueryFromBuffer();
        RefreshEntries();
    }

    ImGui::Separator();

    if (!m_statusMessage.empty())
    {
        if (m_statusIsError)
            ImGui::TextColored(ImVec4{0.96f, 0.56f, 0.56f, 1.0f}, "%s", m_statusMessage.c_str());
        else
            ImGui::TextUnformatted(m_statusMessage.c_str());
    }

    ImGui::BeginChild("##ComponentPickerContent", ImVec2(0.0f, 0.0f), false);
    if (GetViewModeForQuery(m_query) == ComponentPickerViewMode::Categories)
        DrawCategoryNodes(m_categories);
    else
        DrawSearchResults();
    ImGui::EndChild();

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
    bool added = false;
    std::string failureReason;
    if (m_targetGameObject != nullptr && p_entry.isAddable &&
        NLS::Core::ServiceLocator::Contains<NLS::Editor::Undo::EditorUndoService>())
    {
        const auto owner = NLS::Editor::Inspector::InspectorTargetHandle::FromObject(
            *m_targetGameObject,
            NLS::Editor::Inspector::InspectorTargetKind::GameObject);
        auto info = NLS::Editor::Inspector::BuildInspectorUndoCommandInfo(
            "Add " + p_entry.displayName,
            {owner},
            NLS::Editor::Inspector::PropertyPath("GameObject.components"));
        const auto before = NLS::Editor::Inspector::CaptureComponentStructure(*m_targetGameObject);
        info.estimatedBytes = sizeof(NLS::Editor::Inspector::ComponentStructureSnapshot) +
            before.components.size() * sizeof(NLS::Editor::Inspector::ComponentSnapshot);

        NLS::Editor::Inspector::ComponentStructureEditOperation operation;
        operation.kind = NLS::Editor::Inspector::ComponentStructureEditKind::Add;
        operation.componentTypeName = p_entry.componentType.GetName();
        operation.scriptAsset = p_entry.scriptAsset;
        const auto ownerTarget = NLS::Editor::Inspector::CaptureComponentStructureOwner(
            *m_targetGameObject,
            info);
        if (!ownerTarget.has_value())
        {
            failureReason = "The component owner no longer has a valid Scene or Prefab Stage Undo scope.";
        }
        else
        {
            const auto result = NLS::Editor::Inspector::ExecuteComponentStructureEdit(
                NLS::Core::ServiceLocator::Get<NLS::Editor::Undo::EditorUndoService>(),
                std::move(info),
                *ownerTarget,
                before,
                std::move(operation));
            added = result.Succeeded();
            failureReason = result.message;
        }
    }

    if (added)
    {
        SetStatusMessage("Added " + p_entry.displayName, false);
        RefreshEntries();
        ComponentAddedEvent.Invoke();
        ClosePopup();
        return true;
    }

    SetStatusMessage(
        p_entry.isAddable ? (failureReason.empty() ? ("Failed to add " + p_entry.displayName) : failureReason)
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
                DrawMenuEntry(entry);

            ImGui::EndMenu();
        }
        return;
    }

    if (!p_node.entries.empty())
    {
        if (p_node.entries.size() == 1)
        {
            const ComponentSearchEntry& entry = p_node.entries.front();
            DrawMenuEntry(entry);
            return;
        }

        if (ImGui::BeginMenu(p_node.label.c_str()))
        {
            for (const ComponentSearchEntry& entry : p_node.entries)
                DrawMenuEntry(entry);

            ImGui::EndMenu();
        }
    }
}

void ComponentSearchPanel::DrawMenuEntry(const ComponentSearchEntry& p_entry)
{
    ImGui::PushID(p_entry.searchKey.c_str());
    ImGui::BeginDisabled(!p_entry.isAddable);
    const bool selected = ImGui::MenuItem(p_entry.displayName.c_str());
    ImGui::EndDisabled();
    if (!p_entry.isAddable &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
        !p_entry.availabilityReason.empty())
    {
        ImGui::SetTooltip("%s", p_entry.availabilityReason.c_str());
    }
    ImGui::PopID();

    if (selected)
        TryCommitEntry(p_entry);
}

void ComponentSearchPanel::DrawSearchResults()
{
    for (const ComponentSearchEntry& entry : m_entries)
        DrawSearchEntry(entry);
}

void ComponentSearchPanel::DrawSearchEntry(const ComponentSearchEntry& p_entry)
{
    const std::string categoryPath = GetEntryCategoryPath(p_entry);
    const auto& style = ImGui::GetStyle();
    const float lineHeight = ImGui::GetTextLineHeight();
    const float rowHeight = categoryPath.empty()
        ? lineHeight + style.FramePadding.y * 2.0f
        : lineHeight * 2.0f + style.FramePadding.y * 2.0f;

    ImGui::PushID(p_entry.searchKey.c_str());
    ImGui::BeginDisabled(!p_entry.isAddable);
    const bool selected = ImGui::Selectable(
        "##ComponentSearchEntry",
        false,
        0,
        ImVec2(0.0f, rowHeight));
    ImGui::EndDisabled();

    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();
    const float textWidth = (std::max)(0.0f, rowMax.x - rowMin.x - style.FramePadding.x * 2.0f);
    const auto primaryColor = ImGui::GetColorU32(
        p_entry.isAddable ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(rowMin.x + style.FramePadding.x, rowMin.y + style.FramePadding.y),
        primaryColor,
        TruncateUtf8ForWidth(p_entry.displayName, textWidth).c_str());
    if (!categoryPath.empty())
    {
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(rowMin.x + style.FramePadding.x, rowMin.y + style.FramePadding.y + lineHeight),
            ImGui::GetColorU32(ImGuiCol_TextDisabled),
            TruncateUtf8ForWidth(categoryPath, textWidth).c_str());
    }

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        if (!p_entry.isAddable && !p_entry.availabilityReason.empty())
            ImGui::SetTooltip("%s", p_entry.availabilityReason.c_str());
        else if (!categoryPath.empty())
            ImGui::SetTooltip("%s\n%s", p_entry.displayName.c_str(), categoryPath.c_str());
    }
    ImGui::PopID();

    if (selected)
        TryCommitEntry(p_entry);
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
