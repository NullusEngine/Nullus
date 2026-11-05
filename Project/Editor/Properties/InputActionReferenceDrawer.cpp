#include "Inputs/InputActionReferencePicker.h"
#include "Properties/PropertyDrawerRegistry.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Serialization/SerializedProperty.h"
#include <Core/ResourceManagement/ShaderManager.h>
#include <imgui.h>
#include <algorithm>

namespace NLS::Editor::Properties
{
namespace
{
struct Catalog
{
    std::filesystem::path root;
    std::vector<Inputs::InputActionChoice> choices;
    std::vector<std::string> diagnostics;
    bool initialized = false;
};
struct PickerState { std::filesystem::path droppedAsset; };
Catalog& GetCatalog(bool refresh = false)
{
    static Catalog catalog;
    const std::filesystem::path root = NLS::Core::ResourceManagement::ShaderManager::ProjectAssetsRoot();
    if (!catalog.initialized || catalog.root != root || refresh)
    {
        catalog = {}; catalog.initialized = true; catalog.root = root;
        catalog.choices = Inputs::BuildInputActionChoices(root, catalog.diagnostics);
    }
    return catalog;
}
bool Draw(PropertyDrawerContext& context)
{
    auto& catalog = GetCatalog();
    auto& state = context.GetState<PickerState>();
    const auto current = context.value.GetValue<std::string>();
    const auto selected = std::find_if(catalog.choices.begin(), catalog.choices.end(), [&](const auto& choice) { return choice.reference == current; });
    const bool mixed = context.serializedProperty && context.serializedProperty->HasMultipleDifferentValues();
    const std::string label = mixed ? "Mixed" : current.empty() ? "None (Input Action)" : selected == catalog.choices.end() ? "Missing Input Action" : selected->displayName;
    const auto assign = [&](std::string value)
    {
        if (context.readOnly) return;
        context.interaction = {true, false, true, true, false, 1};
        context.changed = context.preview(meta::Variant(std::move(value)), "Assign Input Action");
    };
    ImGui::BeginDisabled(context.readOnly);
    if (ImGui::BeginCombo(context.widgetId.c_str(), label.c_str()))
    {
        if (ImGui::IsWindowAppearing()) GetCatalog(true);
        if (ImGui::Selectable("None", current.empty())) assign("");
        for (const auto& choice : catalog.choices)
        {
            ImGui::PushID(choice.reference.c_str());
            if (ImGui::Selectable(choice.displayName.c_str(), choice.reference == current)) assign(choice.reference);
            ImGui::PopID();
        }
        if (catalog.choices.empty()) ImGui::TextDisabled("No .inputactions assets in this project.");
        for (const auto& diagnostic : catalog.diagnostics) ImGui::TextWrapped("%s", diagnostic.c_str());
        ImGui::EndCombo();
    }
    if (ImGui::BeginDragDropTarget())
    {
        if (const auto* payload = ImGui::AcceptDragDropPayload(Assets::kEditorAssetDragPayloadType);
            payload && payload->Data && payload->DataSize == sizeof(Assets::EditorAssetDragPayload))
        {
            const auto path = Assets::GetEditorAssetDragPayloadPath(*static_cast<const Assets::EditorAssetDragPayload*>(payload->Data));
            GetCatalog(true);
            // A single-action asset can be assigned directly without guessing.
            std::vector<const Inputs::InputActionChoice*> matches;
            for (const auto& choice : catalog.choices)
                if (choice.assetPath.generic_string() == path || choice.assetPath.lexically_relative(catalog.root.parent_path()).generic_string() == path)
                    matches.push_back(&choice);
            if (matches.size() == 1) assign(matches[0]->reference);
            else if (!matches.empty())
            {
                state.droppedAsset = matches[0]->assetPath;
                ImGui::OpenPopup(("Choose action##" + context.widgetId).c_str());
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopup(("Choose action##" + context.widgetId).c_str()))
    {
        ImGui::TextUnformatted("Choose an action from the dropped asset");
        for (const auto& choice : catalog.choices) if (choice.assetPath == state.droppedAsset)
        {
            ImGui::PushID(choice.reference.c_str());
            if (ImGui::Selectable(choice.displayName.c_str(), choice.reference == current)) { assign(choice.reference); ImGui::CloseCurrentPopup(); }
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();
    if (!current.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", current.c_str());
    return true;
}
}
void RegisterInputActionReferenceDrawer()
{
    static auto token = []
    {
        PropertyDrawerRegistration registration;
        registration.scope = DrawerScope::ExplicitMetadata;
        registration.identifier = "builtin.input-action-reference";
        registration.metadataKey = "InputActionReference";
        registration.draw = Draw;
        return PropertyDrawerRegistry::Get().Register(std::move(registration));
    }();
}
}
