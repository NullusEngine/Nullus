#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <Eventing/Event.h>
#include <UI/Widgets/AWidget.h>
#include <Reflection/Type.h>

struct ImVec2;

namespace NLS::Engine
{
class GameObject;
}

namespace NLS::Editor::Panels
{
struct ComponentSearchEntry
{
    meta::Type componentType;
    std::string displayName;
    std::string menuPath;
    std::string searchKey;
    bool isAddable = false;
    std::string availabilityReason;
};

struct ComponentCategoryNode
{
    std::string label;
    std::string fullPath;
    std::vector<ComponentCategoryNode> children;
    std::vector<ComponentSearchEntry> entries;
};

enum class ComponentPickerViewMode
{
    Categories,
    SearchResults
};

class ComponentSearchPanel : public NLS::UI::Widgets::AWidget
{
public:
    ComponentSearchPanel();

    void SetTargetActor(Engine::GameObject* p_actor);
    void OpenForActor(Engine::GameObject* p_actor);
    void RefreshEntries();
    void ClearTarget();
    void NotifyActorComponentsChanged();

    Engine::GameObject* GetTargetActor() const;
    void SetAnchorRect(const ImVec2& p_min, const ImVec2& p_max);

    static std::string NormalizeSearchText(std::string_view p_value);
    static std::string MakeDisplayName(const meta::Type& p_type);
    static std::string GetComponentMenuPath(const meta::Type& p_type);
    static bool IsTypeAddableToActor(const meta::Type& p_type, const Engine::GameObject* p_actor);
    static std::vector<ComponentSearchEntry> BuildComponentEntries(Engine::GameObject* p_actor, std::string_view p_query = {});
    static std::vector<ComponentCategoryNode> BuildCategoryTree(const std::vector<ComponentSearchEntry>& p_entries);
    static ComponentPickerViewMode GetViewModeForQuery(std::string_view p_query);
    static bool TryAddComponentFromEntry(Engine::GameObject* p_actor, const ComponentSearchEntry& p_entry);

    NLS::Event<> ComponentAddedEvent;

protected:
    void _Draw_Impl() override;

private:
    void ClosePopup();
    bool TryCommitEntry(const ComponentSearchEntry& p_entry);
    void DrawCategoryNodes(const std::vector<ComponentCategoryNode>& p_nodes);
    void DrawCategoryNode(const ComponentCategoryNode& p_node);
    void DrawSearchResults();
    void SyncQueryFromBuffer();
    void SetStatusMessage(std::string p_message, bool p_isError);

private:
    Engine::GameObject* m_targetActor = nullptr;
    std::vector<ComponentSearchEntry> m_entries;
    std::vector<ComponentCategoryNode> m_categories;
    std::string m_query;
    std::string m_statusMessage;
    bool m_statusIsError = false;
    bool m_popupRequested = false;
    bool m_popupOpen = false;
    float m_anchorMinX = 0.0f;
    float m_anchorMaxY = 0.0f;
    char m_queryBuffer[256] = {};
};
} // namespace NLS::Editor::Panels
