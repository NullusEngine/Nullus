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

    using Canvas = Engine::Components::UICanvasComponent;
    const auto readEnum = [this](const std::string_view path, const auto fallback)
    {
        const auto property = serializedObject().FindProperty(path);
        const auto value = property.GetValue();
        return value.IsValid() ? value.GetValue<decltype(fallback)>() : fallback;
    };
    const auto renderMode = readEnum("renderModeValue", Canvas::RenderMode::ScreenOverlay);
    const auto dpiScaleMode = readEnum("dpiScaleMode", Canvas::DpiScaleMode::ConstantPixelSize);

    PropertyField("renderModeValue", ComponentEditorPropertyOptions {.label = "Render Mode"});
    if (renderMode == Canvas::RenderMode::World)
    {
        PropertyField("worldRenderPathValue", ComponentEditorPropertyOptions {.label = "World Render Path"});
    }

    PropertyField("sortOrderScript", ComponentEditorPropertyOptions {.label = "Sort Order"});
    PropertyField("sortLayer", ComponentEditorPropertyOptions {.label = "Sort Layer"});
    if (renderMode == Canvas::RenderMode::ScreenCamera)
        PropertyField("cameraId", ComponentEditorPropertyOptions {.label = "Camera Id"});

    PropertyField("updatePolicy", ComponentEditorPropertyOptions {.label = "Update Policy"});
    PropertyField("dpiScaleMode", ComponentEditorPropertyOptions {.label = "DPI Scale Mode"});
    if (dpiScaleMode == Canvas::DpiScaleMode::ScaleWithScreenSize)
    {
        PropertyField("referenceResolution", ComponentEditorPropertyOptions {.label = "Reference Resolution"});
        PropertyField("dpiMatch", ComponentEditorPropertyOptions {.label = "DPI Match"});
    }
    if (renderMode == Canvas::RenderMode::World)
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
