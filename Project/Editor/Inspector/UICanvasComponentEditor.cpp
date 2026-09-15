#include "Inspector/UICanvasComponentEditor.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <Components/UICanvasComponent.h>
#include <Core/ServiceLocator.h>
#include <Engine/UI/UIDocumentSourceResolver.h>

#include "Core/Context.h"
#include "Core/EditorActions.h"
#include "Inspector/NativeReflectionAdapter.h"
#include "Panels/UIDocumentDesigner.h"

namespace NLS::Editor::Inspector
{
namespace
{
std::filesystem::path ResolveDocumentSourcePath(const Engine::UI::UIDocumentReference& reference)
{
    // One resolution implementation for editor, designer and runtime. A second
    // join here used to fold the project assets folder into itself and produce
    // "<project>/Assets/Assets/<document>.ui" for "Assets/..." references.
    const auto resolved = Engine::UI::ResolveUIDocumentSourcePath(reference);
    if (resolved.empty())
    {
        // Keep an absolute reference usable for diagnostics even when the host
        // resolver cannot map it yet.
        const std::filesystem::path stored(reference.GetResourcePath());
        return stored.is_absolute() ? stored.lexically_normal() : std::filesystem::path {};
    }
    return std::filesystem::path(resolved).lexically_normal();
}

InspectorPropertyChoice CanvasChoice(const uint32_t value, const char* label)
{
    InspectorPropertyChoice choice;
    choice.value = std::to_string(value);
    choice.label = label;
    choice.typedValue = meta::Variant(value);
    return choice;
}
} // namespace

void UICanvasComponentEditor::OnInspectorGUI()
{
    serializedObject().Update();

    const auto document = serializedObject().FindProperty("document");
    if (!document)
    {
        HelpBox("UI Canvas target is no longer available.", ComponentEditorMessageKind::Error);
        return;
    }

    // This is deliberately the normal ObjectReference property field. Picker,
    // drag/drop, Clear, Ping, search and Undo all come from InspectorView.
    PropertyField(document, ComponentEditorPropertyOptions {.label = "Document"});

    const auto documentValue = document.GetValue();
    const auto reference = documentValue.IsValid()
        ? documentValue.GetValue<Engine::UI::UIDocumentReference>()
        : Engine::UI::UIDocumentReference {};
    const auto sourcePath = ResolveDocumentSourcePath(reference);
    const auto targetHandle = target() != nullptr
        ? std::optional<InspectorTargetHandle>(*target())
        : std::nullopt;
    Button(
        "Open Designer",
        [targetHandle]
        {
            if (!targetHandle.has_value())
                return;
            auto* canvas = dynamic_cast<Engine::Components::UICanvasComponent*>(
                NativeReflectionAdapter::ResolveObject(*targetHandle));
            if (canvas == nullptr)
                return;
            const auto source = ResolveDocumentSourcePath(canvas->GetDocumentReference());
            if (source.empty())
                return;
            auto& designer = EDITOR_PANEL(NLS::Editor::Panels::UIDocumentDesigner, "UI Document Designer");
            if (designer.OpenDocument(source))
            {
                designer.Open();
                designer.Focus();
            }
        },
        !sourcePath.empty(),
        sourcePath.empty() ? "Assign a UI document first" : "Open this UI document in the UI Designer");

    const auto readUnsigned = [this](const std::string_view path, const uint32_t fallback)
    {
        const auto property = serializedObject().FindProperty(path);
        const auto value = property.GetValue();
        return value.IsValid() ? static_cast<uint32_t>(value.ToInt()) : fallback;
    };
    const uint32_t renderMode = readUnsigned("renderModeValue", 0u);
    const uint32_t dpiScaleMode = readUnsigned("dpiScaleMode", 0u);

    PropertyField("renderModeValue", ComponentEditorPropertyOptions {
        .label = "Render Mode",
        .kind = InspectorPropertyKind::Choice,
        .choices = {CanvasChoice(0u, "Screen Overlay"), CanvasChoice(1u, "Screen Camera"), CanvasChoice(2u, "World")}});
    if (renderMode == 2u)
    {
        PropertyField("worldRenderPathValue", ComponentEditorPropertyOptions {
            .label = "World Render Path",
            .kind = InspectorPropertyKind::Choice,
            .choices = {CanvasChoice(0u, "Auto"), CanvasChoice(1u, "Direct"), CanvasChoice(2u, "Render Target")}});
    }

    PropertyField("sortOrderScript", ComponentEditorPropertyOptions {.label = "Sort Order"});
    PropertyField("sortLayer", ComponentEditorPropertyOptions {.label = "Sort Layer"});
    if (renderMode == 1u)
        PropertyField("cameraId", ComponentEditorPropertyOptions {.label = "Camera Id"});

    PropertyField("updatePolicy", ComponentEditorPropertyOptions {
        .label = "Update Policy",
        .kind = InspectorPropertyKind::Choice,
        .choices = {CanvasChoice(0u, "Every Frame"), CanvasChoice(1u, "On Demand"), CanvasChoice(2u, "Manual")}});
    PropertyField("dpiScaleMode", ComponentEditorPropertyOptions {
        .label = "DPI Scale Mode",
        .kind = InspectorPropertyKind::Choice,
        .choices = {CanvasChoice(0u, "Constant Pixel Size"), CanvasChoice(1u, "Scale With Screen Size"), CanvasChoice(2u, "Constant Physical Size")}});
    if (dpiScaleMode == 1u)
    {
        PropertyField("referenceResolution", ComponentEditorPropertyOptions {.label = "Reference Resolution"});
        PropertyField("dpiMatch", ComponentEditorPropertyOptions {.label = "DPI Match"});
    }
    if (renderMode == 2u)
    {
        PropertyField("worldSize", ComponentEditorPropertyOptions {.label = "World Size"});
        PropertyField("pixelsPerUnit", ComponentEditorPropertyOptions {.label = "Pixels Per Unit"});
        PropertyField("depthTest", ComponentEditorPropertyOptions {.label = "Depth Test"});
        PropertyField("depthWrite", ComponentEditorPropertyOptions {.label = "Depth Write"});
        PropertyField("alwaysOnTop", ComponentEditorPropertyOptions {.label = "Always On Top"});
    }

    (void)ApplyModifiedProperties();
}
} // namespace NLS::Editor::Inspector
