#pragma once

#include "Inspector/ComponentEditor.h"
#include "Inspector/EditorExtensionMetadata.h"
#include "Reflection/Macros.h"
#include "Project/Editor/Inspector/UICanvasComponentEditor.generated.h"

namespace NLS::Editor::Inspector
{
// The component editor only determines which normal serialized fields belong
// to the current mode and contributes actions such as Open Designer.
CLASS(UICanvasComponentEditor) final : public ComponentEditor
{
public:
    GENERATED_BODY()
    void OnInspectorGUI() override;
};

NLS_EDITOR_CUSTOM_EDITOR(
    NLS::Engine::Components::UICanvasComponent,
    NLS::Editor::Inspector::UICanvasComponentEditor)
} // namespace NLS::Editor::Inspector
