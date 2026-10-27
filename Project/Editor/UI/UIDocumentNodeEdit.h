#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <Json/json.hpp>
#include <Math/Color.h>
#include <Math/Vector2.h>
#include <Reflection/ExternalReflectionRegistration.h>

#include <Engine/UI/UIResourceReference.h>

namespace NLS::Editor::UIDocumentTools
{
    class UIDocumentDesignerModel;

    // Editor-only reflected view models for the UI document inspector.
    //
    // The .ui JSON remains the single source of truth. These structs are
    // transient editing copies consumed by Properties::PropertyPanel;
    // changes are written back through UIDocumentDesignerModel so validation,
    // global undo and the runtime schema stay authoritative.

    enum class UIDocumentLayoutModeEdit : uint8_t
    {
        Absolute,
        Stack,
        Flex,
        Grid,
        Overlay,
        Scroll
    };

    enum class UIDocumentDirectionEdit : uint8_t
    {
        Vertical,
        Horizontal
    };

    enum class UIDocumentAlignEdit : uint8_t
    {
        Start,
        Center,
        End,
        Stretch
    };

    struct UIDocumentElementEdit
    {
        std::string name;
        std::string className;
    };

    struct UIDocumentRectEdit
    {
        Maths::Vector2 anchorMin { 0.0f, 0.0f };
        Maths::Vector2 anchorMax { 0.0f, 0.0f };
        Maths::Vector2 pivot { 0.0f, 0.0f };
        Maths::Vector2 offsetMin { 0.0f, 0.0f };
        Maths::Vector2 offsetMax { 0.0f, 0.0f };
        Maths::Vector2 size { 0.0f, 0.0f };
        Maths::Vector2 minSize { 0.0f, 0.0f };
        Maths::Vector2 maxSize { 0.0f, 0.0f };
        float aspectRatio = 0.0f;
    };

    struct UIDocumentLayoutEdit
    {
        UIDocumentLayoutModeEdit mode = UIDocumentLayoutModeEdit::Absolute;
        UIDocumentDirectionEdit direction = UIDocumentDirectionEdit::Vertical;
        UIDocumentAlignEdit align = UIDocumentAlignEdit::Start;
        float spacing = 0.0f;
        float flexGrow = 0.0f;
        uint32_t gridColumns = 1u;
        Maths::Vector2 padding { 0.0f, 0.0f };
        Maths::Vector2 margin { 0.0f, 0.0f };
        bool clipChildren = false;
    };

    struct UIDocumentStyleEdit
    {
        Maths::Color backgroundColor { 0.0f, 0.0f, 0.0f, 0.0f };
        Maths::Color color { 1.0f, 1.0f, 1.0f, 1.0f };
        float opacity = 1.0f;
        bool visible = true;
        bool enabled = true;
        bool interactive = false;
    };

    struct UIDocumentNineSliceEdit
    {
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
    };

    struct UIDocumentContentEdit
    {
        std::string text;
        bool password = false;
        Engine::UI::UITextureReference texture;
        Engine::UI::UIFontReference font;
        UIDocumentNineSliceEdit nineSlice;
        uint64_t fontAssetId = 0u;
        uint32_t fontPixelSize = 16u;
        bool requiresRenderTarget = false;
    };

    NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::UIDocumentTools::UIDocumentLayoutModeEdit)
    NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::UIDocumentTools::UIDocumentDirectionEdit)
    NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::UIDocumentTools::UIDocumentAlignEdit)
    NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::UIDocumentTools::UIDocumentElementEdit)
    NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::UIDocumentTools::UIDocumentRectEdit)
    NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::UIDocumentTools::UIDocumentLayoutEdit)
    NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::UIDocumentTools::UIDocumentStyleEdit)
    NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::UIDocumentTools::UIDocumentNineSliceEdit)
    NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::UIDocumentTools::UIDocumentContentEdit)

    // Registers the external types, enum labels and inspector metadata.
    void EnsureUIDocumentInspectorReflection();

    [[nodiscard]] bool IsTextLikeElement(std::string_view type);

    [[nodiscard]] const char* ToJson(UIDocumentLayoutModeEdit value);
    [[nodiscard]] const char* ToJson(UIDocumentDirectionEdit value);
    [[nodiscard]] const char* ToJson(UIDocumentAlignEdit value);
    [[nodiscard]] UIDocumentLayoutModeEdit LayoutModeFromJson(std::string_view value);
    [[nodiscard]] UIDocumentDirectionEdit DirectionFromJson(std::string_view value);
    [[nodiscard]] UIDocumentAlignEdit AlignFromJson(std::string_view value);

    // Reads from the node JSON with runtime defaults. `node` is the node object
    // as it appears under "$.root..." in the document.
    [[nodiscard]] UIDocumentElementEdit ReadElementEdit(const nlohmann::json& node);
    [[nodiscard]] UIDocumentRectEdit ReadRectEdit(const nlohmann::json& node);
    [[nodiscard]] UIDocumentLayoutEdit ReadLayoutEdit(const nlohmann::json& node);
    [[nodiscard]] UIDocumentStyleEdit ReadStyleEdit(const nlohmann::json& node);
    [[nodiscard]] UIDocumentContentEdit ReadContentEdit(const nlohmann::json& node);

    // Applies only the fields reported by ReflectedPropertyEdit::paths. All
    // calls go through SetNodePropertyOnSelection, so multi-selection edits are
    // written as one document transaction by the caller.
    void ApplyElementEdit(
        UIDocumentDesignerModel& model,
        const UIDocumentElementEdit& value,
        const std::vector<std::string>& changedPaths,
        bool recordUndo);
    void ApplyRectEdit(
        UIDocumentDesignerModel& model,
        const UIDocumentRectEdit& value,
        const std::vector<std::string>& changedPaths,
        bool recordUndo);
    void ApplyLayoutEdit(
        UIDocumentDesignerModel& model,
        const UIDocumentLayoutEdit& value,
        const std::vector<std::string>& changedPaths,
        bool recordUndo);
    void ApplyStyleEdit(
        UIDocumentDesignerModel& model,
        const UIDocumentStyleEdit& value,
        const std::vector<std::string>& changedPaths,
        bool recordUndo);
    void ApplyContentEdit(
        UIDocumentDesignerModel& model,
        const UIDocumentContentEdit& value,
        const std::vector<std::string>& changedPaths,
        bool recordUndo);

    // Reflected field name → .ui JSON path. Kept as data so tests can assert
    // full coverage against UIDocumentDesignerModel::IsSupportedPropertyPath.
    struct UIDocumentEditField
    {
        const char* field;
        const char* jsonPath;
    };
    [[nodiscard]] const std::vector<UIDocumentEditField>& GetUIDocumentEditFields();

    // Matches ReflectedPropertyEdit::Changed() semantics (path or *.field).
    [[nodiscard]] bool UIDocumentEditChanged(
        const std::vector<std::string>& changedPaths,
        std::string_view field);
}
