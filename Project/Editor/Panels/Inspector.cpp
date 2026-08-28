#include "Panels/Inspector.h"

#include <UI/Widgets/Buttons/Button.h>
#include <UI/Widgets/Buttons/ButtonSmall.h>
#include <UI/Widgets/Layout/Spacing.h>
#include <UI/Widgets/Layout/Columns.h>
#include <UI/Widgets/Texts/Text.h>
#include <UI/Widgets/Texts/TextColored.h>
#include <UI/Widgets/Texts/TextDisabled.h>
#include <UI/Widgets/Visual/Separator.h>
#include <UI/Widgets/Layout/GroupCollapsable.h>
#include <UI/Widgets/Selection/ComboBox.h>
#include <UI/Plugins/DDTarget.h>
#include <UI/GUIDrawer.h>
#include <ServiceLocator.h>
#include <ResourceManagement/TextureManager.h>
#include <ResourceManagement/ShaderManager.h>
#include <Reflection/Variant.h>
#include <imgui.h>
#include "Assets/PrefabUtilityFacade.h"
#include "Assets/AssetBrowserPresentation.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Core/EditorActions.h"
#include "Components/TransformComponent.h"
#include "Components/UICanvasComponent.h"
#include "Panels/ComponentSearchPanel.h"
#include "Panels/ReflectedPropertyDrawer.h"
#include "Panels/UIDocumentDesigner.h"
#include "Assets/ScriptAssetUtility.h"
#include <Scripting/SerializedObject.h>
#include <Scripting/ScriptComponent.h>
#include "GameObject.h"
#include "Settings/TagLayerSettings.h"
#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <memory>
#include <filesystem>
#include <system_error>
#include <variant>

namespace
{
using namespace NLS;

std::string GetComponentDisplayName(const NLS::Engine::Components::Component& component)
{
    std::string displayName = NLS::Editor::Panels::ComponentSearchPanel::MakeDisplayName(component.GetType());
    if (displayName.empty())
        displayName = "Component";

    return displayName;
}

void DrawComponentFallback(NLS::UI::Internal::WidgetContainer &root, Engine::Components::Component &component)
{
    root.CreateWidget<NLS::UI::Widgets::Text>("No reflected fields");
    root.CreateWidget<NLS::UI::Widgets::Text>(component.GetType().GetName());
}

void DrawSerializedScriptObject(
    NLS::UI::Internal::WidgetContainer& root,
    const std::shared_ptr<NLS::Scripting::SerializedObject>& serializedObject,
    const std::function<void()>& onApplied)
{
    if (!serializedObject || !serializedObject->IsValid())
    {
        root.CreateWidget<NLS::UI::Widgets::Text>("Script serialization is unavailable");
        return;
    }

    const auto* descriptor = serializedObject->GetTarget()->GetScriptTypeDescriptor();
    if (!descriptor)
    {
        root.CreateWidget<NLS::UI::Widgets::Text>("No script schema");
        return;
    }

    const auto commit = [serializedObject, onApplied](NLS::Scripting::ScriptFieldId fieldId, const NLS::Scripting::ScriptValue& value)
    {
        auto* property = serializedObject->FindProperty(fieldId);
        if (property && property->SetValue(value) && serializedObject->ApplyModifiedProperties() && onApplied)
            onApplied();
    };

    // Script fields use the same two-column layout as native reflected
    // components.  Keeping the label width and GUIDrawer controls identical
    // makes C# / Lua fields visually indistinguishable from C++ fields.
    auto& fields = root.CreateWidget<NLS::UI::Widgets::Columns>(2);
    fields.widths[0] = NLS::Editor::Panels::ReflectedPropertyDrawerOptions{}.labelWidth;

    for (const auto& field : descriptor->fields)
    {
        if (!field.serialized)
            continue;
        const auto fieldId = field.id;
        const std::string label = NLS::Editor::Panels::FormatReflectedFieldLabel(field.name);
        switch (field.type.kind)
        {
        case NLS::Scripting::ScriptValueKind::Bool:
            NLS::UI::GUIDrawer::DrawBoolean(
                fields,
                label,
                [serializedObject, fieldId]
                {
                    const auto* property = serializedObject->FindProperty(fieldId);
                    const auto* value = property ? std::get_if<bool>(&property->GetValue()) : nullptr;
                    return value ? *value : false;
                },
                [commit, fieldId](bool value) { commit(fieldId, value); });
            break;
        case NLS::Scripting::ScriptValueKind::Int8:
            NLS::UI::GUIDrawer::DrawScalar<int8_t>(fields, label,
                [serializedObject, fieldId] { const auto* p = serializedObject->FindProperty(fieldId); const auto* v = p ? std::get_if<int8_t>(&p->GetValue()) : nullptr; return v ? *v : int8_t{}; },
                [commit, fieldId](int8_t value) { commit(fieldId, value); });
            break;
        case NLS::Scripting::ScriptValueKind::UInt8:
            NLS::UI::GUIDrawer::DrawScalar<uint8_t>(fields, label,
                [serializedObject, fieldId] { const auto* p = serializedObject->FindProperty(fieldId); const auto* v = p ? std::get_if<uint8_t>(&p->GetValue()) : nullptr; return v ? *v : uint8_t{}; },
                [commit, fieldId](uint8_t value) { commit(fieldId, value); });
            break;
        case NLS::Scripting::ScriptValueKind::Int16:
            NLS::UI::GUIDrawer::DrawScalar<int16_t>(fields, label,
                [serializedObject, fieldId] { const auto* p = serializedObject->FindProperty(fieldId); const auto* v = p ? std::get_if<int16_t>(&p->GetValue()) : nullptr; return v ? *v : int16_t{}; },
                [commit, fieldId](int16_t value) { commit(fieldId, value); });
            break;
        case NLS::Scripting::ScriptValueKind::UInt16:
            NLS::UI::GUIDrawer::DrawScalar<uint16_t>(fields, label,
                [serializedObject, fieldId] { const auto* p = serializedObject->FindProperty(fieldId); const auto* v = p ? std::get_if<uint16_t>(&p->GetValue()) : nullptr; return v ? *v : uint16_t{}; },
                [commit, fieldId](uint16_t value) { commit(fieldId, value); });
            break;
        case NLS::Scripting::ScriptValueKind::Int32:
            NLS::UI::GUIDrawer::DrawScalar<int32_t>(fields, label,
                [serializedObject, fieldId] { const auto* p = serializedObject->FindProperty(fieldId); const auto* v = p ? std::get_if<int32_t>(&p->GetValue()) : nullptr; return v ? *v : int32_t{}; },
                [commit, fieldId](int32_t value) { commit(fieldId, value); });
            break;
        case NLS::Scripting::ScriptValueKind::UInt32:
            NLS::UI::GUIDrawer::DrawScalar<uint32_t>(fields, label,
                [serializedObject, fieldId] { const auto* p = serializedObject->FindProperty(fieldId); const auto* v = p ? std::get_if<uint32_t>(&p->GetValue()) : nullptr; return v ? *v : uint32_t{}; },
                [commit, fieldId](uint32_t value) { commit(fieldId, value); });
            break;
        case NLS::Scripting::ScriptValueKind::Int64:
            NLS::UI::GUIDrawer::DrawScalar<int64_t>(fields, label,
                [serializedObject, fieldId] { const auto* p = serializedObject->FindProperty(fieldId); const auto* v = p ? std::get_if<int64_t>(&p->GetValue()) : nullptr; return v ? *v : int64_t{}; },
                [commit, fieldId](int64_t value) { commit(fieldId, value); });
            break;
        case NLS::Scripting::ScriptValueKind::UInt64:
            NLS::UI::GUIDrawer::DrawScalar<uint64_t>(fields, label,
                [serializedObject, fieldId] { const auto* p = serializedObject->FindProperty(fieldId); const auto* v = p ? std::get_if<uint64_t>(&p->GetValue()) : nullptr; return v ? *v : uint64_t{}; },
                [commit, fieldId](uint64_t value) { commit(fieldId, value); });
            break;
        case NLS::Scripting::ScriptValueKind::Float:
            NLS::UI::GUIDrawer::DrawScalar<float>(fields, label,
                [serializedObject, fieldId] { const auto* p = serializedObject->FindProperty(fieldId); const auto* v = p ? std::get_if<float>(&p->GetValue()) : nullptr; return v ? *v : 0.0f; },
                [commit, fieldId](float value) { commit(fieldId, value); });
            break;
        case NLS::Scripting::ScriptValueKind::Double:
            NLS::UI::GUIDrawer::DrawScalar<double>(fields, label,
                [serializedObject, fieldId] { const auto* p = serializedObject->FindProperty(fieldId); const auto* v = p ? std::get_if<double>(&p->GetValue()) : nullptr; return v ? *v : 0.0; },
                [commit, fieldId](double value) { commit(fieldId, value); });
            break;
        case NLS::Scripting::ScriptValueKind::String:
            NLS::UI::GUIDrawer::DrawString(
                fields,
                label,
                [serializedObject, fieldId]
                {
                    const auto* property = serializedObject->FindProperty(fieldId);
                    const auto* value = property ? std::get_if<std::string>(&property->GetValue()) : nullptr;
                    return value ? *value : std::string{};
                },
                [commit, fieldId](std::string value) { commit(fieldId, value); });
            break;
        case NLS::Scripting::ScriptValueKind::Vector2:
            NLS::UI::GUIDrawer::DrawVec2(fields, label,
                [serializedObject, fieldId] { const auto* p = serializedObject->FindProperty(fieldId); const auto* v = p ? std::get_if<NLS::Maths::Vector2>(&p->GetValue()) : nullptr; return v ? *v : NLS::Maths::Vector2{}; },
                [commit, fieldId](NLS::Maths::Vector2 value) { commit(fieldId, value); });
            break;
        case NLS::Scripting::ScriptValueKind::Vector3:
            NLS::UI::GUIDrawer::DrawVec3(fields, label,
                [serializedObject, fieldId] { const auto* p = serializedObject->FindProperty(fieldId); const auto* v = p ? std::get_if<NLS::Maths::Vector3>(&p->GetValue()) : nullptr; return v ? *v : NLS::Maths::Vector3{}; },
                [commit, fieldId](NLS::Maths::Vector3 value) { commit(fieldId, value); });
            break;
        case NLS::Scripting::ScriptValueKind::Vector4:
            NLS::UI::GUIDrawer::DrawVec4(fields, label,
                [serializedObject, fieldId] { const auto* p = serializedObject->FindProperty(fieldId); const auto* v = p ? std::get_if<NLS::Maths::Vector4>(&p->GetValue()) : nullptr; return v ? *v : NLS::Maths::Vector4{}; },
                [commit, fieldId](NLS::Maths::Vector4 value) { commit(fieldId, value); });
            break;
        case NLS::Scripting::ScriptValueKind::Quaternion:
            NLS::UI::GUIDrawer::DrawQuat(fields, label,
                [serializedObject, fieldId] { const auto* p = serializedObject->FindProperty(fieldId); const auto* v = p ? std::get_if<NLS::Maths::Quaternion>(&p->GetValue()) : nullptr; return v ? *v : NLS::Maths::Quaternion{}; },
                [commit, fieldId](NLS::Maths::Quaternion value) { commit(fieldId, value); });
            break;
        case NLS::Scripting::ScriptValueKind::Color:
            NLS::UI::GUIDrawer::DrawColor(fields, label,
                [serializedObject, fieldId] { const auto* p = serializedObject->FindProperty(fieldId); const auto* v = p ? std::get_if<NLS::Maths::Color>(&p->GetValue()) : nullptr; return v ? *v : NLS::Maths::Color{}; },
                [commit, fieldId](NLS::Maths::Color value) { commit(fieldId, value); },
                true);
            break;
        case NLS::Scripting::ScriptValueKind::Enum:
            NLS::UI::GUIDrawer::DrawScalar<int64_t>(fields, label,
                [serializedObject, fieldId] { const auto* p = serializedObject->FindProperty(fieldId); const auto* v = p ? std::get_if<NLS::Scripting::ScriptEnumValue>(&p->GetValue()) : nullptr; return v ? v->value : int64_t{}; },
                [commit, fieldId, typeId = field.type.id](int64_t value) { commit(fieldId, NLS::Scripting::ScriptEnumValue{typeId, value}); });
            break;
        case NLS::Scripting::ScriptValueKind::ObjectReference:
        case NLS::Scripting::ScriptValueKind::Struct:
        case NLS::Scripting::ScriptValueKind::Null:
        default:
            fields.CreateWidget<NLS::UI::Widgets::Text>(label + " (unsupported script field)");
            break;
        }
    }
}

void DrawScriptAssetSlot(
    NLS::UI::Internal::WidgetContainer& root,
    NLS::Scripting::ScriptComponent& component,
    const std::function<void()>& onApplied)
{
    NLS::UI::GUIDrawer::CreateTitle(root, "Script Asset");
    auto& slot = root.CreateWidget<NLS::UI::Widgets::Group>();
    auto& display = slot.CreateWidget<NLS::UI::Widgets::Text>(
        component.GetScriptAsset().sourcePath.empty()
            ? "Drop a .cs or .lua script here"
            : component.GetScriptAsset().sourcePath);
    display.lineBreak = false;
    auto& dispatcher = display.AddPlugin<NLS::UI::DataDispatcher<std::string>>();
    dispatcher.RegisterGatherer([&component]
    {
        return component.GetScriptAsset().sourcePath.empty()
            ? std::string("Drop a .cs or .lua script here")
            : component.GetScriptAsset().sourcePath;
    });
    display.AddPlugin<NLS::UI::DDTarget<NLS::Editor::Assets::EditorAssetDragPayload>>(
        NLS::Editor::Assets::kEditorAssetDragPayloadType).DataReceivedEvent +=
        [&component, onApplied](NLS::Editor::Assets::EditorAssetDragPayload payload)
    {
        const auto asset = NLS::Editor::Assets::LoadScriptAsset(
            std::filesystem::path(EDITOR_CONTEXT(projectAssetsPath)), payload);
        if (!asset.has_value() || !asset->isComponent)
            return;
        component.SetScriptAsset(*asset);
        if (onApplied)
            onApplied();
    };
}

const char* ToPrefabConnectionText(NLS::Editor::Assets::PrefabEditorConnectionState state)
{
    using NLS::Editor::Assets::PrefabEditorConnectionState;
    switch (state)
    {
    case PrefabEditorConnectionState::Connected:
        return "Connected";
    case PrefabEditorConnectionState::MissingSource:
        return "Missing Source";
    case PrefabEditorConnectionState::Disconnected:
        return "Disconnected";
    case PrefabEditorConnectionState::Unpacked:
        return "Unpacked";
    case PrefabEditorConnectionState::Pending:
        return "Pending";
    case PrefabEditorConnectionState::Invalid:
        return "Invalid";
    case PrefabEditorConnectionState::NotAPrefab:
    default:
        return "Not a Prefab";
    }
}

const char* ToPrefabResourceText(NLS::Editor::Assets::PrefabEditorResourceState state)
{
    using NLS::Editor::Assets::PrefabEditorResourceState;
    switch (state)
    {
    case PrefabEditorResourceState::Pending:
        return "Pending";
    case PrefabEditorResourceState::Failed:
        return "Failed";
    case PrefabEditorResourceState::Cancelled:
        return "Cancelled";
    case PrefabEditorResourceState::Ready:
    default:
        return "Ready";
    }
}

NLS::Maths::Color PrefabStateColor(const NLS::Editor::Assets::PrefabEditorState& state)
{
    using NLS::Editor::Assets::PrefabEditorConnectionState;
    using NLS::Editor::Assets::PrefabEditorResourceState;
    if (state.connectionState == PrefabEditorConnectionState::MissingSource ||
        state.connectionState == PrefabEditorConnectionState::Invalid ||
        state.resourceState == PrefabEditorResourceState::Failed)
    {
        return {0.95f, 0.34f, 0.34f, 1.0f};
    }

    if (state.resourceState == PrefabEditorResourceState::Pending)
        return {0.68f, 0.67f, 0.86f, 1.0f};

    if (state.hasOverrides)
        return {0.92f, 0.70f, 0.31f, 1.0f};

    if (state.generatedReadOnly)
        return {0.54f, 0.82f, 0.78f, 1.0f};

    return {0.58f, 0.75f, 0.98f, 1.0f};
}

class HeaderComboBox final : public NLS::UI::Widgets::ComboBox
{
public:
    HeaderComboBox(
        std::map<int, std::string> p_choices,
        std::function<int()> p_gatherer,
        std::function<void(int)> p_provider)
        : HeaderComboBox(
              [choices = std::move(p_choices)] { return choices; },
              std::move(p_gatherer),
              std::move(p_provider))
    {
    }

    HeaderComboBox(
        std::function<std::map<int, std::string>()> p_choicesProvider,
        std::function<int()> p_gatherer,
        std::function<void(int)> p_provider)
        : NLS::UI::Widgets::ComboBox(0),
          m_choicesProvider(std::move(p_choicesProvider)),
          m_gatherer(std::move(p_gatherer)),
          m_provider(std::move(p_provider))
    {
        ValueChangedEvent += [this](int p_choice)
        {
            if (m_provider)
                m_provider(p_choice);
        };
    }

protected:
    void _Draw_Impl() override
    {
        if (m_choicesProvider)
            choices = m_choicesProvider();
        if (m_gatherer)
            currentChoice = m_gatherer();
        NLS::UI::Widgets::ComboBox::_Draw_Impl();
    }

private:
    std::function<std::map<int, std::string>()> m_choicesProvider;
    std::function<int()> m_gatherer;
    std::function<void(int)> m_provider;
};

bool IsUIDocumentAssetPath(const std::filesystem::path& path)
{
    auto extension = path.extension().string();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return extension == ".ui";
}

bool IsPathWithinProjectAssets(
    const std::filesystem::path& projectAssetsPath,
    const std::filesystem::path& resolvedPath)
{
    const auto relativePath = resolvedPath.lexically_relative(projectAssetsPath);
    if (relativePath.empty() || relativePath.is_absolute())
        return false;
    for (const auto& part : relativePath)
    {
        if (part == "..")
            return false;
    }
    return true;
}

std::filesystem::path ResolveProjectUIDocumentPath(
    const std::filesystem::path& projectAssetsPath,
    const std::string& documentPath)
{
    if (projectAssetsPath.empty() || documentPath.empty())
        return {};

    const auto normalizedAssetsPath = projectAssetsPath.lexically_normal();
    const std::filesystem::path sourcePath(documentPath);
    const auto normalizedSourcePath = sourcePath.lexically_normal();
    const auto genericSourcePath = normalizedSourcePath.generic_string();
    const auto resolvedPath = sourcePath.is_absolute()
        ? normalizedSourcePath
        : (genericSourcePath == "Assets" || genericSourcePath.rfind("Assets/", 0u) == 0u
            ? (normalizedAssetsPath.parent_path() / normalizedSourcePath).lexically_normal()
            : (normalizedAssetsPath / normalizedSourcePath).lexically_normal());
    return IsPathWithinProjectAssets(normalizedAssetsPath, resolvedPath)
        ? resolvedPath
        : std::filesystem::path {};
}

std::string NormalizeProjectUIDocumentReference(
    const std::filesystem::path& projectAssetsPath,
    const std::string& documentPath)
{
    const auto resolvedPath = ResolveProjectUIDocumentPath(projectAssetsPath, documentPath);
    if (resolvedPath.empty() || !IsUIDocumentAssetPath(resolvedPath))
        return {};

    const auto normalizedAssetsPath = projectAssetsPath.lexically_normal();
    const auto relativePath = resolvedPath.lexically_relative(normalizedAssetsPath);
    return IsPathWithinProjectAssets(normalizedAssetsPath, resolvedPath)
        ? (std::filesystem::path("Assets") / relativePath).generic_string()
        : std::string {};
}

void AppendUIDocumentChoice(
    std::vector<std::string>& documents,
    const std::filesystem::path& projectAssetsPath,
    const std::string& documentPath)
{
    const auto normalizedPath = NormalizeProjectUIDocumentReference(projectAssetsPath, documentPath);
    if (!normalizedPath.empty())
        documents.push_back(normalizedPath);
}

std::map<int, std::string> BuildUIDocumentChoices(
    const std::filesystem::path& projectAssetsPath,
    const std::string& selectedDocumentPath = {})
{
    std::map<int, std::string> choices {{ 0, "None" }};
    std::vector<std::string> documents;
    for (const auto& entry : NLS::Editor::Assets::GetObjectReferencePickerEntries())
    {
        if (NLS::Editor::Assets::GetEditorAssetDragPayloadArtifactType(entry.payload) !=
            NLS::Core::Assets::ArtifactType::UIDocument)
            continue;

        AppendUIDocumentChoice(
            documents,
            projectAssetsPath,
            NLS::Editor::Assets::GetEditorAssetDragPayloadPath(entry.payload));
    }

    // The published Asset Browser index is normally ready before Inspector
    // interaction. Keep a narrow source scan for documents created or copied
    // during the import hand-off, so an empty index never hides valid .ui files.
    if (documents.empty() &&
        !projectAssetsPath.empty() &&
        std::filesystem::is_directory(projectAssetsPath))
    {
        std::error_code error;
        std::filesystem::recursive_directory_iterator iterator(
            projectAssetsPath,
            std::filesystem::directory_options::skip_permission_denied,
            error);
        const std::filesystem::recursive_directory_iterator end;
        for (; iterator != end && !error; iterator.increment(error))
        {
            const auto status = iterator->symlink_status(error);
            if (error)
            {
                error.clear();
                continue;
            }
            if (!std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status) ||
                !IsUIDocumentAssetPath(iterator->path()))
            {
                continue;
            }

            AppendUIDocumentChoice(documents, projectAssetsPath, iterator->path().string());
        }
    }

    // The Asset Browser index is rebuilt asynchronously. Keep the currently
    // assigned asset visible while that snapshot catches up.
    AppendUIDocumentChoice(documents, projectAssetsPath, selectedDocumentPath);

    std::sort(documents.begin(), documents.end());
    documents.erase(std::unique(documents.begin(), documents.end()), documents.end());
    for (size_t index = 0; index < documents.size(); ++index)
        choices.emplace(static_cast<int>(index + 1u), std::move(documents[index]));
    return choices;
}

int FindUIDocumentChoice(
    const std::map<int, std::string>& choices,
    const std::filesystem::path& projectAssetsPath,
    const std::string& documentPath)
{
    const auto normalizedPath = NormalizeProjectUIDocumentReference(projectAssetsPath, documentPath);
    if (normalizedPath.empty())
        return 0;
    for (const auto& [value, path] : choices)
    {
        if (path == normalizedPath)
            return value;
    }
    return 0;
}

bool AssignUIDocumentReference(
    NLS::Engine::Components::UICanvasComponent& canvas,
    const std::filesystem::path& projectAssetsPath,
    const std::string& documentPath)
{
    if (documentPath.empty())
    {
        canvas.SetDocumentPath({});
        return true;
    }

    const auto normalizedPath = NormalizeProjectUIDocumentReference(projectAssetsPath, documentPath);
    const auto resolvedPath = ResolveProjectUIDocumentPath(projectAssetsPath, normalizedPath);
    std::error_code error;
    if (normalizedPath.empty() || resolvedPath.empty() ||
        !std::filesystem::is_regular_file(resolvedPath, error) || error)
        return false;

    // Scene serialization keeps a logical Assets/... path. Loading uses the
    // resolved local source path so the Inspector can preview the assignment
    // immediately without leaking an absolute machine-specific reference.
    canvas.SetDocumentPath(normalizedPath);
    return canvas.LoadDocumentResolvedPath(resolvedPath.string(), nullptr);
}

bool AssignUIDocumentChoice(
    NLS::Engine::Components::UICanvasComponent& canvas,
    const std::filesystem::path& projectAssetsPath,
    const std::map<int, std::string>& choices,
    const int value)
{
    const auto choice = choices.find(value);
    return AssignUIDocumentReference(
        canvas,
        projectAssetsPath,
        choice == choices.end() ? std::string {} : choice->second);
}

void DrawUICanvasDocumentSlot(
    NLS::UI::Internal::WidgetContainer& root,
    NLS::Engine::Components::UICanvasComponent& canvas,
    const std::function<void()>& onApplied)
{
    const std::filesystem::path projectAssetsPath(EDITOR_CONTEXT(projectAssetsPath));
    NLS::UI::GUIDrawer::CreateTitle(root, "Document");
    auto& slot = root.CreateWidget<NLS::UI::Widgets::Group>();
    // The Asset Browser publishes its picker index asynchronously. Keep one
    // map for a whole ImGui draw so a click is resolved against the item that
    // was actually displayed, rather than a newly rebuilt numeric ordering.
    auto choicesSnapshot = std::make_shared<std::map<int, std::string>>();
    auto& picker = slot.CreateWidget<HeaderComboBox>(
        [projectAssetsPath, &canvas, choicesSnapshot]
        {
            *choicesSnapshot = BuildUIDocumentChoices(projectAssetsPath, canvas.GetDocumentPath());
            return *choicesSnapshot;
        },
        [projectAssetsPath, &canvas, choicesSnapshot]
        {
            return FindUIDocumentChoice(*choicesSnapshot, projectAssetsPath, canvas.GetDocumentPath());
        },
        [projectAssetsPath, &canvas, onApplied, choicesSnapshot](const int value)
        {
            if (AssignUIDocumentChoice(canvas, projectAssetsPath, *choicesSnapshot, value) && onApplied)
                onApplied();
        });
    picker.AddPlugin<NLS::UI::DDTarget<NLS::Editor::Assets::EditorAssetDragPayload>>(
        NLS::Editor::Assets::kEditorAssetDragPayloadType).DataReceivedEvent +=
        [projectAssetsPath, &canvas, onApplied](NLS::Editor::Assets::EditorAssetDragPayload payload)
    {
        const auto documentPath = NLS::Editor::Assets::GetEditorAssetDragPayloadPath(payload);
        const auto artifactType = NLS::Editor::Assets::GetEditorAssetDragPayloadArtifactType(payload);
        if (artifactType != NLS::Core::Assets::ArtifactType::Unknown &&
            artifactType != NLS::Core::Assets::ArtifactType::UIDocument)
        {
            return;
        }
        if (!IsUIDocumentAssetPath(documentPath))
            return;
        if (AssignUIDocumentReference(canvas, projectAssetsPath, documentPath) && onApplied)
            onApplied();
    };

    auto& openButton = slot.CreateWidget<NLS::UI::Widgets::ButtonSmall>("Open Designer");
    openButton.ClickedEvent += [projectAssetsPath, &canvas]
    {
        const auto documentPath = canvas.GetDocumentPath();
        const auto resolvedPath = ResolveProjectUIDocumentPath(projectAssetsPath, documentPath);
        std::error_code error;
        if (resolvedPath.empty() || !std::filesystem::is_regular_file(resolvedPath, error) || error)
            return;
        auto& designer = EDITOR_PANEL(NLS::Editor::Panels::UIDocumentDesigner, "UI Designer");
        (void)designer.OpenDocument(resolvedPath);
    };
}

void DrawUICanvasSettings(
    NLS::UI::Internal::WidgetContainer& root,
    NLS::Engine::Components::UICanvasComponent& canvas,
    const std::function<void()>& onApplied,
    const std::function<void()>& onFieldLayoutChanged)
{
    using Canvas = NLS::Engine::Components::UICanvasComponent;

    NLS::UI::GUIDrawer::CreateTitle(root, "Render Mode");
    root.CreateWidget<HeaderComboBox>(
        std::map<int, std::string> {
            { static_cast<int>(Canvas::RenderMode::ScreenOverlay), "Screen Overlay" },
            { static_cast<int>(Canvas::RenderMode::ScreenCamera), "Screen Camera" },
            { static_cast<int>(Canvas::RenderMode::World), "World" }
        },
        [&canvas] { return static_cast<int>(canvas.GetRenderMode()); },
        [&canvas, onApplied, onFieldLayoutChanged](const int value)
        {
            canvas.SetRenderModeValue(static_cast<uint32_t>(value));
            if (onApplied)
                onApplied();
            if (onFieldLayoutChanged)
                onFieldLayoutChanged();
        });

    NLS::UI::GUIDrawer::DrawScalar<int>(
        root,
        "Sort Order",
        [&canvas] { return canvas.GetSortOrder(); },
        [&canvas, onApplied](const int value)
        {
            canvas.SetSortOrder(value);
            if (onApplied)
                onApplied();
        });

    if (canvas.GetRenderMode() != Canvas::RenderMode::World)
        return;

    NLS::UI::GUIDrawer::CreateTitle(root, "World Path");
    root.CreateWidget<HeaderComboBox>(
        std::map<int, std::string> {
            { static_cast<int>(Canvas::WorldRenderPath::Auto), "Auto" },
            { static_cast<int>(Canvas::WorldRenderPath::Direct), "Direct" },
            { static_cast<int>(Canvas::WorldRenderPath::RenderTarget), "Render Target" }
        },
        [&canvas] { return static_cast<int>(canvas.GetWorldRenderPath()); },
        [&canvas, onApplied](const int value)
        {
            canvas.SetWorldRenderPathValue(static_cast<uint32_t>(value));
            if (onApplied)
                onApplied();
        });

    NLS::UI::GUIDrawer::DrawScalar<float>(
        root,
        "Pixels Per Unit",
        [&canvas] { return canvas.GetPixelsPerUnit(); },
        [&canvas, onApplied](const float value)
        {
            canvas.SetPixelsPerUnit(value);
            if (onApplied)
                onApplied();
        },
        1.0f,
        1.0f,
        10000.0f);
    NLS::UI::GUIDrawer::DrawBoolean(
        root,
        "Depth Test",
        [&canvas] { return canvas.GetDepthTest(); },
        [&canvas, onApplied](const bool value)
        {
            canvas.SetDepthTest(value);
            if (onApplied)
                onApplied();
        });
    NLS::UI::GUIDrawer::DrawBoolean(
        root,
        "Always On Top",
        [&canvas] { return canvas.GetAlwaysOnTop(); },
        [&canvas, onApplied](const bool value)
        {
            canvas.SetAlwaysOnTop(value);
            if (onApplied)
                onApplied();
        });
}
} // namespace

#if defined(NLS_ENABLE_TEST_HOOKS)
namespace NLS::Editor::Panels::TestHooks
{
std::map<int, std::string> BuildUICanvasDocumentChoicesForTesting(
    const std::filesystem::path& projectAssetsPath,
    const std::string& selectedDocumentPath)
{
    return BuildUIDocumentChoices(projectAssetsPath, selectedDocumentPath);
}

int FindUICanvasDocumentChoiceForTesting(
    const std::map<int, std::string>& choices,
    const std::filesystem::path& projectAssetsPath,
    const std::string& documentPath)
{
    return FindUIDocumentChoice(choices, projectAssetsPath, documentPath);
}

bool AssignUICanvasDocumentReferenceForTesting(
    Engine::Components::UICanvasComponent& canvas,
    const std::filesystem::path& projectAssetsPath,
    const std::string& documentPath)
{
    return AssignUIDocumentReference(canvas, projectAssetsPath, documentPath);
}

bool AssignUICanvasDocumentChoiceForTesting(
    Engine::Components::UICanvasComponent& canvas,
    const std::filesystem::path& projectAssetsPath,
    const std::map<int, std::string>& choices,
    const int value)
{
    return AssignUIDocumentChoice(canvas, projectAssetsPath, choices, value);
}
}
#endif

namespace NLS::Editor::Panels
{
Inspector::Inspector(const std::string& p_title, bool p_opened, const NLS::UI::PanelWindowSettings& p_windowSettings)
    : NLS::UI::PanelWindow(p_title, p_opened, p_windowSettings)
{
    m_lifetimeToken = std::make_shared<bool>(true);
    m_inspectorHeader = &CreateWidget<UI::Widgets::Group>();
    m_inspectorHeader->enabled = false;
    m_gameObjectInfo = &CreateWidget<UI::Widgets::Group>();

    auto& headerColumns = m_inspectorHeader->CreateWidget<UI::Widgets::Columns>(2);
    headerColumns.widths[0] = 88;

    /* Name field */
    auto nameGatherer = [this]
    { return m_targetGameObject ? m_targetGameObject->GetName() : "%undef%"; };
    auto nameProvider = [this](const std::string& p_newName)
    {
        if (m_targetGameObject)
        {
            m_targetGameObject->SetName(p_newName);
            MarkTargetSceneDirty();
        }
    };
    UI::GUIDrawer::DrawString(headerColumns, "Name", nameGatherer, nameProvider);

    /* Tag field */
    UI::GUIDrawer::CreateTitle(headerColumns, "Tag");
    headerColumns.CreateWidget<HeaderComboBox>(
        [this]()
        {
            auto choices = Settings::TagLayerSettings::BuildTagChoices();
            if (m_targetGameObject)
            {
                const auto tag = m_targetGameObject->GetTag();
                if (Settings::TagLayerSettings::FindTagIndex(tag) == 0 &&
                    tag != Settings::TagLayerSettings::GetTagAt(0))
                    choices.emplace(-1, tag);
            }
            return choices;
        },
        [this]()
        {
            if (!m_targetGameObject)
                return 0;

            const auto tag = m_targetGameObject->GetTag();
            if (Settings::TagLayerSettings::FindTagIndex(tag) == 0 &&
                tag != Settings::TagLayerSettings::GetTagAt(0))
                return -1;
            return Settings::TagLayerSettings::FindTagIndex(tag);
        },
        [this](int p_choice)
        {
            if (m_targetGameObject)
            {
                if (p_choice < 0)
                    return;
                m_targetGameObject->SetTag(Settings::TagLayerSettings::GetTagAt(p_choice));
                MarkTargetSceneDirty();
            }
        });

    /* Layer field */
    UI::GUIDrawer::CreateTitle(headerColumns, "Layer");
    headerColumns.CreateWidget<HeaderComboBox>(
        Settings::TagLayerSettings::BuildLayerChoices(),
        [this]()
        {
            if (!m_targetGameObject)
                return 0;

            const int layer = m_targetGameObject->GetLayer();
            if (layer < 0 || layer >= static_cast<int>(Settings::TagLayerSettings::LayerCount))
                return 0;
            return layer;
        },
        [this](int p_choice)
        {
            if (m_targetGameObject)
            {
                m_targetGameObject->SetLayer(p_choice);
                MarkTargetSceneDirty();
            }
        });

    /* Active field */
    auto activeGatherer = [this]
    { return m_targetGameObject ? m_targetGameObject->IsSelfActive() : false; };
    auto activeProvider = [this](bool p_active)
    {
        if (m_targetGameObject)
        {
            m_targetGameObject->SetActive(p_active);
            MarkTargetSceneDirty();
        }
    };
    UI::GUIDrawer::DrawBoolean(headerColumns, "Active", activeGatherer, activeProvider);

    auto& addComponentButton = m_inspectorHeader->CreateWidget<UI::Widgets::Button>("Add Component", Maths::Vector2{118.f, 0});
    addComponentButton.idleBackgroundColor = Maths::Color{0.23f, 0.49f, 0.82f};
    addComponentButton.hoveredBackgroundColor = Maths::Color{0.29f, 0.58f, 0.93f};
    addComponentButton.clickedBackgroundColor = Maths::Color{0.18f, 0.41f, 0.71f};
    addComponentButton.textColor = Maths::Color::White;
    addComponentButton.ClickedEvent += [this]
    {
        SyncComponentPicker();

        if (m_targetGameObject && m_componentPicker)
        {
            m_componentPicker->SetAnchorRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            m_componentPicker->OpenForGameObject(m_targetGameObject);
        }
    };

    m_inspectorHeader->CreateWidget<UI::Widgets::Separator>();
    m_inspectorHeader->CreateWidget<UI::Widgets::Spacing>(1);
    m_componentPicker = &m_inspectorHeader->CreateWidget<ComponentSearchPanel>();
    m_componentPicker->ComponentAddedEvent += [this]
    {
        MarkTargetSceneDirty();
        Refresh();
    };

    m_destroyedListener = Engine::GameObject::DestroyedEvent += [this](Engine::GameObject& p_destroyed)
    {
        if (&p_destroyed == m_targetGameObject)
            UnFocus();
    };
}
Inspector::~Inspector()
{
    if (m_lifetimeToken)
        *m_lifetimeToken = false;
    Engine::GameObject::DestroyedEvent -= m_destroyedListener;

    UnFocus();
}

void Inspector::FocusGameObject(Engine::GameObject& p_target)
{
    if (m_targetGameObject)
        UnFocus();

    m_gameObjectInfo->RemoveAllWidgets();

    m_targetGameObject = &p_target;

    m_componentAddedListener = m_targetGameObject->ComponentAddedEvent += [this](auto useless)
    {
        auto lifetimeToken = m_lifetimeToken;
        EDITOR_EXEC(DelayAction([this, lifetimeToken]
        {
            if (lifetimeToken && *lifetimeToken)
                Refresh();
        }));
    };

    m_componentRemovedListener = m_targetGameObject->ComponentRemovedEvent += [this](auto useless)
    {
        m_gameObjectInfo->RemoveAllWidgets();
        auto lifetimeToken = m_lifetimeToken;
        EDITOR_EXEC(DelayAction([this, lifetimeToken]
        {
            if (lifetimeToken && *lifetimeToken)
                Refresh();
        }));
    };

    m_inspectorHeader->enabled = true;

    CreateGameObjectInspector(p_target);
    SyncComponentPicker();

    EDITOR_EVENT(GameObjectSelectedEvent).Invoke(*m_targetGameObject);
}

void Inspector::UnFocus()
{
    if (m_targetGameObject)
    {
        m_targetGameObject->ComponentAddedEvent -= m_componentAddedListener;
        m_targetGameObject->ComponentRemovedEvent -= m_componentRemovedListener;
    }

    SoftUnFocus();
}

void Inspector::SoftUnFocus()
{
    if (m_targetGameObject)
    {
        EDITOR_EVENT(GameObjectUnselectedEvent).Invoke(*m_targetGameObject);
        if (m_componentPicker)
            m_componentPicker->ClearTarget();
        m_inspectorHeader->enabled = false;
        m_targetGameObject = nullptr;
        m_gameObjectInfo->RemoveAllWidgets();
    }
}

Engine::GameObject* Inspector::GetTargetGameObject() const
{
    return m_targetGameObject;
}

void Inspector::CreateGameObjectInspector(Engine::GameObject& p_target)
{
    using namespace NLS::Engine::Components;
    std::vector<Component*> components;

    DrawPrefabState(p_target);

    for (const auto& component : p_target.GetComponents())
    {
        if (!component)
            continue;

        auto* rawComponent = component.get();
        if (rawComponent->GetType() == NLS_TYPEOF(TransformComponent))
            continue;

        components.push_back(rawComponent);
    }

    std::sort(
        components.begin(),
        components.end(),
        [](const Component* p_left, const Component* p_right)
        {
            if (p_left == nullptr || p_right == nullptr)
                return p_left < p_right;

            return GetComponentDisplayName(*p_left) < GetComponentDisplayName(*p_right);
        });

    if (auto* transform = p_target.GetComponent<TransformComponent>())
        DrawComponent(transform);

    for (auto* instance : components)
        DrawComponent(instance);
}

void Inspector::DrawPrefabState(Engine::GameObject& p_target)
{
    auto* instance = EDITOR_CONTEXT(prefabInstanceRegistry).FindInstance(p_target);
    if (!instance)
        return;

    NLS::Engine::Assets::PrefabArtifact sourcePrefab;
    sourcePrefab.assetId = instance->prefabAssetId;
    sourcePrefab.graph = instance->SourceGraph();
    sourcePrefab.generatedModelPrefab = instance->generatedReadOnly;

    const auto presentation = EDITOR_CONTEXT(prefabInstanceRegistry).GetPresentation(p_target);
    NLS::Editor::Assets::PrefabEditorStateQuery query;
    query.instance = instance;
    query.prefab = sourcePrefab.graph.objects.empty() ? nullptr : &sourcePrefab;
    query.sourceAssetExists = !presentation.missingAsset;
    query.resourcesPending = presentation.pendingResources;
    query.unpacked = instance->unpacked;
    query.editableSourceArtifactContext = false;
    query.includeDefaultOverrides = true;
    const auto state = NLS::Editor::Assets::PrefabUtilityFacade().GetPrefabEditorState(query);

    auto& prefabGroup = m_gameObjectInfo->CreateWidget<UI::Widgets::GroupCollapsable>("Prefab");
    prefabGroup.opened = true;
    prefabGroup.closable = false;
    prefabGroup.CreateWidget<UI::Widgets::TextColored>(
        std::string("State: ") + ToPrefabConnectionText(state.connectionState),
        PrefabStateColor(state));
    prefabGroup.CreateWidget<UI::Widgets::Text>(
        std::string("Source: ") +
        (instance->prefabSubAssetKey.empty() ? std::string("<unknown>") : instance->prefabSubAssetKey));
    prefabGroup.CreateWidget<UI::Widgets::Text>(
        std::string("Resources: ") + ToPrefabResourceText(state.resourceState));

    if (state.hasOverrides)
    {
        prefabGroup.CreateWidget<UI::Widgets::TextColored>(
            "Overrides: " + std::to_string(state.overrideCount),
            Maths::Color{0.92f, 0.70f, 0.31f, 1.0f});
    }
    else
    {
        prefabGroup.CreateWidget<UI::Widgets::TextDisabled>("Overrides: None");
    }

    if (state.generatedReadOnly)
        prefabGroup.CreateWidget<UI::Widgets::TextColored>(
            "Model prefab source is read-only",
            Maths::Color{0.54f, 0.82f, 0.78f, 1.0f});

    for (const auto& diagnostic : state.diagnostics)
        prefabGroup.CreateWidget<UI::Widgets::TextColored>(
            diagnostic.message.empty() ? diagnostic.code : diagnostic.message,
            Maths::Color{0.95f, 0.56f, 0.34f, 1.0f});

    auto& actions = prefabGroup.CreateWidget<UI::Widgets::Columns>(2);
    auto& applyButton = actions.CreateWidget<UI::Widgets::Button>("Apply", Maths::Vector2{76.f, 0.f});
    applyButton.disabled = true;
    if (applyButton.disabled)
        applyButton.textColor = Maths::Color{0.62f, 0.62f, 0.62f, 1.0f};
    prefabGroup.CreateWidget<UI::Widgets::TextDisabled>("Apply from Inspector is unavailable until an editable source asset context is open.");

    auto& revertButton = actions.CreateWidget<UI::Widgets::Button>("Revert", Maths::Vector2{76.f, 0.f});
    revertButton.disabled = !state.canRevert;
    if (revertButton.disabled)
        revertButton.textColor = Maths::Color{0.62f, 0.62f, 0.62f, 1.0f};
    revertButton.ClickedEvent += [this, instance]
    {
        if (!instance)
            return;

        auto result = NLS::Editor::Assets::PrefabUtilityFacade().RevertPrefabInstance(*instance);
        if (result.status == NLS::Editor::Assets::PrefabOperationStatus::Committed)
        {
            if (instance->instanceRoot)
                EDITOR_EXEC(MarkOwningSceneDirty(*instance->instanceRoot));
            Refresh();
        }
    };

    m_gameObjectInfo->CreateWidget<UI::Widgets::Spacing>(1);
}

void Inspector::DrawComponent(Engine::Components::Component* p_component)
{
    using namespace NLS::Engine::Components;
    if (!p_component)
        return;

    const bool isTransform = p_component->GetType() == NLS_TYPEOF(TransformComponent);
    std::string title = GetComponentDisplayName(*p_component);
    if (title.empty())
        title = "Component";

    auto& header = m_gameObjectInfo->CreateWidget<UI::Widgets::GroupCollapsable>(title);
    header.closable = !isTransform;
    header.opened = true;
    header.CloseEvent += [this, p_component]
    {
        if (!p_component)
            return;

        auto* owner = p_component->gameobject();
        if (!owner)
            return;

        const auto ownerInstanceID = owner->GetInstanceID();
        const auto componentType = p_component->GetType();
        auto lifetimeToken = m_lifetimeToken;
        EDITOR_EXEC(DelayAction([this, lifetimeToken, ownerInstanceID, componentType]
        {
            if (!lifetimeToken || !*lifetimeToken)
                return;

            auto* owner = NLS::Object::IDToPointer<NLS::Engine::GameObject>(ownerInstanceID);
            if (!owner)
                return;

            auto* component = owner->GetComponent(componentType, true);
            if (!component)
                return;

            if (m_targetGameObject == owner)
                m_gameObjectInfo->RemoveAllWidgets();

            if (owner->RemoveComponent(component))
            {
                EDITOR_EXEC(MarkOwningSceneDirty(*owner));
                SyncComponentPicker();
                Refresh();
            }
        }));
    };

    if (auto* scriptComponent = dynamic_cast<NLS::Scripting::ScriptComponent*>(p_component))
    {
        DrawScriptAssetSlot(
            header,
            *scriptComponent,
            [this] { MarkTargetSceneDirty(); });
        auto serializedObject = std::make_shared<NLS::Scripting::SerializedObject>(*scriptComponent);
        DrawSerializedScriptObject(
            header,
            serializedObject,
            [this] { MarkTargetSceneDirty(); });
        m_gameObjectInfo->CreateWidget<UI::Widgets::Spacing>(1);
        return;
    }

    auto* uiCanvas = dynamic_cast<UICanvasComponent*>(p_component);
    if (uiCanvas)
    {
        const auto onCanvasApplied = [this]
        {
            MarkTargetSceneDirty();
        };
        const auto onCanvasFieldLayoutChanged = [this]
        {
            auto lifetimeToken = m_lifetimeToken;
            EDITOR_EXEC(DelayAction([this, lifetimeToken]
            {
                if (lifetimeToken && *lifetimeToken)
                    Refresh();
            }));
        };
        DrawUICanvasDocumentSlot(header, *uiCanvas, onCanvasApplied);
        DrawUICanvasSettings(header, *uiCanvas, onCanvasApplied, onCanvasFieldLayoutChanged);
    }

    const auto componentType = p_component->GetType();
    const auto &fields = componentType.GetFields();
    const bool hasVisibleFields = std::any_of(
        fields.begin(),
        fields.end(),
        [uiCanvas](const meta::Field& field)
        {
            return !uiCanvas || field.GetName() != "documentPath";
        });
    if (!hasVisibleFields)
    {
        if (!uiCanvas)
            DrawComponentFallback(header, *p_component);
        m_gameObjectInfo->CreateWidget<UI::Widgets::Spacing>(1);
        return;
    }

    ReflectedPropertyDrawerOptions options;
    options.labelWidth = 104.0f;
    options.sceneObjectPickerScene = p_component->gameobject()
        ? p_component->gameobject()->GetScene()
        : nullptr;
    options.onFieldChanged = [this](const meta::Field&)
    {
        MarkTargetSceneDirty();
    };
    options.onFieldLayoutChanged = [this](const meta::Field&)
    {
        MarkTargetSceneDirty();
        auto lifetimeToken = m_lifetimeToken;
        EDITOR_EXEC(DelayAction([this, lifetimeToken]
        {
            if (lifetimeToken && *lifetimeToken)
                Refresh();
        }));
    };
    if (uiCanvas)
    {
        options.shouldDrawField = [](const meta::Field& field)
        {
            return field.GetName() != "documentPath";
        };
    }
    DrawReflectedObject(header, meta::Variant(p_component, meta::variant_policy::WrapObject {}), options);

    m_gameObjectInfo->CreateWidget<UI::Widgets::Spacing>(1);
}

void Inspector::MarkTargetSceneDirty() const
{
    if (m_targetGameObject)
        EDITOR_EXEC(MarkOwningSceneDirty(*m_targetGameObject));
}

void Inspector::Refresh()
{
    if (m_targetGameObject)
    {
        m_gameObjectInfo->RemoveAllWidgets();
        CreateGameObjectInspector(*m_targetGameObject);
        SyncComponentPicker();
    }
}

void Inspector::SyncComponentPicker()
{
    if (!m_componentPicker)
        return;

    if (m_targetGameObject)
    {
        if (m_componentPicker->GetTargetGameObject() != m_targetGameObject)
            m_componentPicker->SetTargetGameObject(m_targetGameObject);
        else
            m_componentPicker->NotifyGameObjectComponentsChanged();
    }
    else
    {
        m_componentPicker->ClearTarget();
    }
}
} // namespace NLS::Editor::Panels
