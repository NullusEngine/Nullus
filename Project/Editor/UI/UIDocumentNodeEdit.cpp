#include "UI/UIDocumentNodeEdit.h"

#include <algorithm>
#include <mutex>
#include <optional>

#include <Inspector/EditorReflection.h>
#include <Inspector/PropertyTypeTraits.h>
#include <Reflection/ReflectionDatabase.h>

#include "UI/UIDocumentDesignerModel.h"

namespace NLS::Editor::UIDocumentTools
{
    namespace
    {
        using Json = UIDocumentDesignerModel::Json;

        const Json* FindSection(const Json& node, const char* name)
        {
            const auto section = node.find(name);
            return section == node.end() || !section->is_object() ? nullptr : &*section;
        }

        float ReadFloat(const Json* section, const char* key, const float fallback)
        {
            if (section == nullptr)
                return fallback;
            const auto value = section->find(key);
            return value == section->end() || !value->is_number()
                ? fallback
                : value->get<float>();
        }

        uint32_t ReadUInt(const Json* section, const char* key, const uint32_t fallback)
        {
            if (section == nullptr)
                return fallback;
            const auto value = section->find(key);
            return value == section->end() || !value->is_number()
                ? fallback
                : static_cast<uint32_t>(std::max<int64_t>(0, value->get<int64_t>()));
        }

        bool ReadBool(const Json* section, const char* key, const bool fallback)
        {
            if (section == nullptr)
                return fallback;
            const auto value = section->find(key);
            return value == section->end() || !value->is_boolean()
                ? fallback
                : value->get<bool>();
        }

        std::string ReadString(const Json* section, const char* key, const std::string& fallback)
        {
            if (section == nullptr)
                return fallback;
            const auto value = section->find(key);
            return value == section->end() || !value->is_string()
                ? fallback
                : value->get<std::string>();
        }

        Maths::Vector2 ReadVector2(
            const Json* section,
            const char* key,
            const Maths::Vector2& fallback)
        {
            if (section == nullptr)
                return fallback;
            const auto value = section->find(key);
            if (value == section->end() || !value->is_array() || value->size() < 2u)
                return fallback;
            return {
                (*value)[0].is_number() ? (*value)[0].get<float>() : fallback.x,
                (*value)[1].is_number() ? (*value)[1].get<float>() : fallback.y
            };
        }

        Maths::Color ReadColor(const Json* section, const char* key, const Maths::Color& fallback)
        {
            if (section == nullptr)
                return fallback;
            const auto value = section->find(key);
            if (value == section->end() || !value->is_array() || value->size() < 3u)
                return fallback;
            return {
                (*value)[0].is_number() ? (*value)[0].get<float>() : fallback.r,
                (*value)[1].is_number() ? (*value)[1].get<float>() : fallback.g,
                (*value)[2].is_number() ? (*value)[2].get<float>() : fallback.b,
                value->size() > 3u && (*value)[3].is_number()
                    ? (*value)[3].get<float>()
                    : fallback.a
            };
        }

        void ApplyVector2(
            UIDocumentDesignerModel& model,
            const char* path,
            const Maths::Vector2& value,
            const bool recordUndo)
        {
            model.SetNodePropertyOnSelection(
                path,
                Json::array({ value.x, value.y }),
                recordUndo);
        }

        void ApplyColor(
            UIDocumentDesignerModel& model,
            const char* path,
            const Maths::Color& value,
            const bool recordUndo)
        {
            model.SetNodePropertyOnSelection(
                path,
                Json::array({ value.r, value.g, value.b, value.a }),
                recordUndo);
        }
    }

    inline void RegisterUIDocumentInspectorReflection(
        meta::ReflectionDatabase& db,
        const meta::ReflectionRegistrationPhase phase)
    {
        NLS_META_EXTERNAL_BEGIN(NLS::Editor::UIDocumentTools::UIDocumentLayoutModeEdit)
            metaExternalType.SetEnum<UIDocumentLayoutModeEdit>(metaExternalTypeName, {
                { "Absolute", UIDocumentLayoutModeEdit::Absolute },
                { "Stack", UIDocumentLayoutModeEdit::Stack },
                { "Flex", UIDocumentLayoutModeEdit::Flex },
                { "Grid", UIDocumentLayoutModeEdit::Grid },
                { "Overlay", UIDocumentLayoutModeEdit::Overlay },
                { "Scroll", UIDocumentLayoutModeEdit::Scroll }
            });
        NLS_META_EXTERNAL_END();

        NLS_META_EXTERNAL_BEGIN(NLS::Editor::UIDocumentTools::UIDocumentDirectionEdit)
            metaExternalType.SetEnum<UIDocumentDirectionEdit>(metaExternalTypeName, {
                { "Vertical", UIDocumentDirectionEdit::Vertical },
                { "Horizontal", UIDocumentDirectionEdit::Horizontal }
            });
        NLS_META_EXTERNAL_END();

        NLS_META_EXTERNAL_BEGIN(NLS::Editor::UIDocumentTools::UIDocumentAlignEdit)
            metaExternalType.SetEnum<UIDocumentAlignEdit>(metaExternalTypeName, {
                { "Start", UIDocumentAlignEdit::Start },
                { "Center", UIDocumentAlignEdit::Center },
                { "End", UIDocumentAlignEdit::End },
                { "Stretch", UIDocumentAlignEdit::Stretch }
            });
        NLS_META_EXTERNAL_END();

        NLS_META_EXTERNAL_BEGIN(NLS::Editor::UIDocumentTools::UIDocumentElementEdit)
            NLS_META_EXTERNAL_FIELD(std::string, name);
            NLS_META_EXTERNAL_FIELD(std::string, className);
        NLS_META_EXTERNAL_END();

        NLS_META_EXTERNAL_BEGIN(NLS::Editor::UIDocumentTools::UIDocumentRectEdit)
            NLS_META_EXTERNAL_FIELD(NLS::Maths::Vector2, anchorMin);
            NLS_META_EXTERNAL_FIELD(NLS::Maths::Vector2, anchorMax);
            NLS_META_EXTERNAL_FIELD(NLS::Maths::Vector2, pivot);
            NLS_META_EXTERNAL_FIELD(NLS::Maths::Vector2, offsetMin);
            NLS_META_EXTERNAL_FIELD(NLS::Maths::Vector2, offsetMax);
            NLS_META_EXTERNAL_FIELD(NLS::Maths::Vector2, size);
            NLS_META_EXTERNAL_FIELD(NLS::Maths::Vector2, minSize);
            NLS_META_EXTERNAL_FIELD(NLS::Maths::Vector2, maxSize);
            NLS_META_EXTERNAL_FIELD(float, aspectRatio);
        NLS_META_EXTERNAL_END();

        NLS_META_EXTERNAL_BEGIN(NLS::Editor::UIDocumentTools::UIDocumentLayoutEdit)
            NLS_META_EXTERNAL_FIELD(UIDocumentLayoutModeEdit, mode);
            NLS_META_EXTERNAL_FIELD(UIDocumentDirectionEdit, direction);
            NLS_META_EXTERNAL_FIELD(UIDocumentAlignEdit, align);
            NLS_META_EXTERNAL_FIELD(float, spacing);
            NLS_META_EXTERNAL_FIELD(float, flexGrow);
            NLS_META_EXTERNAL_FIELD(uint32_t, gridColumns);
            NLS_META_EXTERNAL_FIELD(NLS::Maths::Vector2, padding);
            NLS_META_EXTERNAL_FIELD(NLS::Maths::Vector2, margin);
            NLS_META_EXTERNAL_FIELD(bool, clipChildren);
        NLS_META_EXTERNAL_END();

        NLS_META_EXTERNAL_BEGIN(NLS::Editor::UIDocumentTools::UIDocumentStyleEdit)
            NLS_META_EXTERNAL_FIELD(NLS::Maths::Color, backgroundColor);
            NLS_META_EXTERNAL_FIELD(NLS::Maths::Color, color);
            NLS_META_EXTERNAL_FIELD(float, opacity);
            NLS_META_EXTERNAL_FIELD(bool, visible);
            NLS_META_EXTERNAL_FIELD(bool, enabled);
            NLS_META_EXTERNAL_FIELD(bool, interactive);
        NLS_META_EXTERNAL_END();

        NLS_META_EXTERNAL_BEGIN(NLS::Editor::UIDocumentTools::UIDocumentContentEdit)
            NLS_META_EXTERNAL_FIELD(std::string, text);
            NLS_META_EXTERNAL_FIELD(bool, password);
            NLS_META_EXTERNAL_FIELD(NLS::Engine::UI::UITextureReference, texture);
            NLS_META_EXTERNAL_FIELD(NLS::Engine::UI::UIFontReference, font);
            NLS_META_EXTERNAL_FIELD(NLS::Editor::UIDocumentTools::UIDocumentNineSliceEdit, nineSlice);
            NLS_META_EXTERNAL_FIELD(uint64_t, fontAssetId);
            NLS_META_EXTERNAL_FIELD(uint32_t, fontPixelSize);
            NLS_META_EXTERNAL_FIELD(bool, requiresRenderTarget);
        NLS_META_EXTERNAL_END();

        NLS_META_EXTERNAL_BEGIN(NLS::Editor::UIDocumentTools::UIDocumentNineSliceEdit)
            NLS_META_EXTERNAL_FIELD(float, left);
            NLS_META_EXTERNAL_FIELD(float, top);
            NLS_META_EXTERNAL_FIELD(float, right);
            NLS_META_EXTERNAL_FIELD(float, bottom);
        NLS_META_EXTERNAL_END();
    }

    NLS_META_EXTERNAL_MODULE(RegisterUIDocumentInspectorReflection)

    void EnsureUIDocumentInspectorReflection()
    {
        (void)meta::ReflectionDatabase::Instance();
        static std::once_flag once;
        std::call_once(once, []()
        {
            Inspector::RegisterEditorStructMetadata(
                NLS_TYPEOF(UIDocumentElementEdit),
                {
                    { "name", "Name" },
                    { "className", "Class" }
                });
            Inspector::RegisterEditorStructMetadata(
                NLS_TYPEOF(UIDocumentRectEdit),
                {
                    { "anchorMin", "Anchor Min" },
                    { "anchorMax", "Anchor Max" },
                    { "pivot", "Pivot" },
                    { "offsetMin", "Offset Min" },
                    { "offsetMax", "Offset Max" },
                    { "size", "Size" },
                    { "minSize", "Min Size" },
                    { "maxSize", "Max Size" },
                    { "aspectRatio", "Aspect" }
                });
            Inspector::RegisterEditorStructMetadata(
                NLS_TYPEOF(UIDocumentLayoutEdit),
                {
                    { "mode", "Mode" },
                    { "direction", "Direction" },
                    { "align", "Align" },
                    { "spacing", "Spacing" },
                    { "flexGrow", "Flex Grow" },
                    { "gridColumns", "Columns" },
                    { "padding", "Padding" },
                    { "margin", "Margin" },
                    { "clipChildren", "Clip Children" }
                });
            Inspector::RegisterEditorStructMetadata(
                NLS_TYPEOF(UIDocumentStyleEdit),
                {
                    { "backgroundColor", "Background" },
                    { "color", "Text Color" },
                    { "opacity", "Opacity", "", false, 0.0f, 1.0f },
                    { "visible", "Visible" },
                    { "enabled", "Enabled" },
                    { "interactive", "Interactive" }
                });
            Inspector::RegisterEditorStructMetadata(
                NLS_TYPEOF(UIDocumentNineSliceEdit),
                {
                    { "left", "Left" },
                    { "top", "Top" },
                    { "right", "Right" },
                    { "bottom", "Bottom" }
                });
            Inspector::RegisterEditorStructMetadata(
                NLS_TYPEOF(UIDocumentContentEdit),
                {
                    { "text", "Text", "", true },
                    { "password", "Password" },
                    { "texture", "Texture" },
                    { "font", "Font" },
                    { "nineSlice", "Nine Slice" },
                    { "fontAssetId", "Legacy Font ID" },
                    { "fontPixelSize", "Pixel Size", "", false, 1.0f, 512.0f },
                    { "requiresRenderTarget", "Render Target" }
                });
        });
    }

    bool IsTextLikeElement(const std::string_view type)
    {
        return type == "Text" || type == "RichText" || type == "Button" ||
            type == "TextField" || type == "Dropdown";
    }

    const char* ToJson(const UIDocumentLayoutModeEdit value)
    {
        switch (value)
        {
        case UIDocumentLayoutModeEdit::Absolute: return "Absolute";
        case UIDocumentLayoutModeEdit::Stack: return "Stack";
        case UIDocumentLayoutModeEdit::Flex: return "Flex";
        case UIDocumentLayoutModeEdit::Grid: return "Grid";
        case UIDocumentLayoutModeEdit::Overlay: return "Overlay";
        case UIDocumentLayoutModeEdit::Scroll: return "Scroll";
        }
        return "Absolute";
    }

    const char* ToJson(const UIDocumentDirectionEdit value)
    {
        return value == UIDocumentDirectionEdit::Horizontal ? "Horizontal" : "Vertical";
    }

    const char* ToJson(const UIDocumentAlignEdit value)
    {
        switch (value)
        {
        case UIDocumentAlignEdit::Start: return "Start";
        case UIDocumentAlignEdit::Center: return "Center";
        case UIDocumentAlignEdit::End: return "End";
        case UIDocumentAlignEdit::Stretch: return "Stretch";
        }
        return "Start";
    }

    UIDocumentLayoutModeEdit LayoutModeFromJson(const std::string_view value)
    {
        if (value == "Stack") return UIDocumentLayoutModeEdit::Stack;
        if (value == "Flex") return UIDocumentLayoutModeEdit::Flex;
        if (value == "Grid") return UIDocumentLayoutModeEdit::Grid;
        if (value == "Overlay") return UIDocumentLayoutModeEdit::Overlay;
        if (value == "Scroll") return UIDocumentLayoutModeEdit::Scroll;
        return UIDocumentLayoutModeEdit::Absolute;
    }

    UIDocumentDirectionEdit DirectionFromJson(const std::string_view value)
    {
        return value == "Horizontal"
            ? UIDocumentDirectionEdit::Horizontal
            : UIDocumentDirectionEdit::Vertical;
    }

    UIDocumentAlignEdit AlignFromJson(const std::string_view value)
    {
        if (value == "Center") return UIDocumentAlignEdit::Center;
        if (value == "End") return UIDocumentAlignEdit::End;
        if (value == "Stretch") return UIDocumentAlignEdit::Stretch;
        return UIDocumentAlignEdit::Start;
    }

    UIDocumentElementEdit ReadElementEdit(const nlohmann::json& node)
    {
        UIDocumentElementEdit value;
        value.name = node.value("name", std::string {});
        value.className = node.value("className", std::string {});
        return value;
    }

    UIDocumentRectEdit ReadRectEdit(const nlohmann::json& node)
    {
        const auto* layout = FindSection(node, "layout");
        UIDocumentRectEdit value;
        value.anchorMin = ReadVector2(layout, "anchorMin", value.anchorMin);
        value.anchorMax = ReadVector2(layout, "anchorMax", value.anchorMax);
        value.pivot = ReadVector2(layout, "pivot", value.pivot);
        value.offsetMin = ReadVector2(layout, "offsetMin", value.offsetMin);
        value.offsetMax = ReadVector2(layout, "offsetMax", value.offsetMax);
        value.size = ReadVector2(layout, "size", value.size);
        value.minSize = ReadVector2(layout, "minSize", value.minSize);
        value.maxSize = ReadVector2(layout, "maxSize", value.maxSize);
        value.aspectRatio = ReadFloat(layout, "aspectRatio", value.aspectRatio);
        return value;
    }

    UIDocumentLayoutEdit ReadLayoutEdit(const nlohmann::json& node)
    {
        const auto* layout = FindSection(node, "layout");
        UIDocumentLayoutEdit value;
        value.mode = LayoutModeFromJson(
            ReadString(layout, "mode", ToJson(value.mode)));
        value.direction = DirectionFromJson(
            ReadString(layout, "direction", ToJson(value.direction)));
        value.align = AlignFromJson(
            ReadString(layout, "align", ToJson(value.align)));
        value.spacing = ReadFloat(layout, "spacing", value.spacing);
        value.flexGrow = ReadFloat(layout, "flexGrow", value.flexGrow);
        value.gridColumns = std::max(1u, ReadUInt(layout, "gridColumns", value.gridColumns));
        value.padding = ReadVector2(layout, "padding", value.padding);
        value.margin = ReadVector2(layout, "margin", value.margin);
        value.clipChildren = ReadBool(layout, "clipChildren", value.clipChildren);
        return value;
    }

    UIDocumentStyleEdit ReadStyleEdit(const nlohmann::json& node)
    {
        const auto* style = FindSection(node, "style");
        UIDocumentStyleEdit value;
        value.backgroundColor = ReadColor(style, "backgroundColor", value.backgroundColor);
        value.color = ReadColor(style, "color", value.color);
        value.opacity = ReadFloat(style, "opacity", value.opacity);
        value.visible = ReadBool(style, "visible", value.visible);
        value.enabled = ReadBool(style, "enabled", value.enabled);
        value.interactive = ReadBool(style, "interactive", value.interactive);
        return value;
    }

    UIDocumentContentEdit ReadContentEdit(const nlohmann::json& node)
    {
        UIDocumentContentEdit value;
        value.text = node.value("text", std::string {});
        value.password = node.value("password", false);

        if (const auto* texture = FindSection(node, "texture"))
        {
            value.texture.SetAssetIdString(ReadString(texture, "assetId", {}));
            value.texture.SetSubAssetKey(ReadString(texture, "subAssetKey", {}));
            value.texture.SetResourcePath(ReadString(texture, "path", {}));
        }
        if (const auto* font = FindSection(node, "font"))
        {
            value.font.SetAssetIdString(ReadString(font, "assetId", {}));
            value.font.SetSubAssetKey(ReadString(font, "subAssetKey", {}));
            value.font.SetResourcePath(ReadString(font, "path", {}));
        }
        if (const auto* nineSlice = FindSection(node, "nineSlice"))
        {
            value.nineSlice.left = std::max(0.0f, ReadFloat(nineSlice, "left", 0.0f));
            value.nineSlice.top = std::max(0.0f, ReadFloat(nineSlice, "top", 0.0f));
            value.nineSlice.right = std::max(0.0f, ReadFloat(nineSlice, "right", 0.0f));
            value.nineSlice.bottom = std::max(0.0f, ReadFloat(nineSlice, "bottom", 0.0f));
        }

        if (const auto fontAssetId = node.find("fontAssetId");
            fontAssetId != node.end() && fontAssetId->is_number())
        {
            value.fontAssetId = fontAssetId->get<uint64_t>();
        }
        if (const auto pixelSize = node.find("fontPixelSize");
            pixelSize != node.end() && pixelSize->is_number())
        {
            value.fontPixelSize = static_cast<uint32_t>(
                std::max<int64_t>(1, pixelSize->get<int64_t>()));
        }
        value.requiresRenderTarget = node.value("requiresRenderTarget", false);
        return value;
    }

    void ApplyElementEdit(
        UIDocumentDesignerModel& model,
        const UIDocumentElementEdit& value,
        const std::vector<std::string>& changedPaths,
        const bool recordUndo)
    {
        if (UIDocumentEditChanged(changedPaths, "name"))
            model.SetNodePropertyOnSelection("name", value.name, recordUndo, "Rename UI Element");
        if (UIDocumentEditChanged(changedPaths, "className"))
            model.SetNodePropertyOnSelection("className", value.className, recordUndo, "Edit UI Class");
    }

    void ApplyRectEdit(
        UIDocumentDesignerModel& model,
        const UIDocumentRectEdit& value,
        const std::vector<std::string>& changedPaths,
        const bool recordUndo)
    {
        if (UIDocumentEditChanged(changedPaths, "anchorMin"))
            ApplyVector2(model, "layout.anchorMin", value.anchorMin, recordUndo);
        if (UIDocumentEditChanged(changedPaths, "anchorMax"))
            ApplyVector2(model, "layout.anchorMax", value.anchorMax, recordUndo);
        if (UIDocumentEditChanged(changedPaths, "pivot"))
            ApplyVector2(model, "layout.pivot", value.pivot, recordUndo);
        if (UIDocumentEditChanged(changedPaths, "offsetMin"))
            ApplyVector2(model, "layout.offsetMin", value.offsetMin, recordUndo);
        if (UIDocumentEditChanged(changedPaths, "offsetMax"))
            ApplyVector2(model, "layout.offsetMax", value.offsetMax, recordUndo);
        if (UIDocumentEditChanged(changedPaths, "size"))
            ApplyVector2(model, "layout.size", value.size, recordUndo);
        if (UIDocumentEditChanged(changedPaths, "minSize"))
            ApplyVector2(model, "layout.minSize", value.minSize, recordUndo);
        if (UIDocumentEditChanged(changedPaths, "maxSize"))
            ApplyVector2(model, "layout.maxSize", value.maxSize, recordUndo);
        if (UIDocumentEditChanged(changedPaths, "aspectRatio"))
            model.SetNodePropertyOnSelection("layout.aspectRatio", value.aspectRatio, recordUndo);
    }

    void ApplyLayoutEdit(
        UIDocumentDesignerModel& model,
        const UIDocumentLayoutEdit& value,
        const std::vector<std::string>& changedPaths,
        const bool recordUndo)
    {
        if (UIDocumentEditChanged(changedPaths, "mode"))
        {
            model.SetNodePropertyOnSelection(
                "layout.mode",
                std::string(ToJson(value.mode)),
                recordUndo,
                "Edit UI Layout Mode");
        }
        if (UIDocumentEditChanged(changedPaths, "direction"))
        {
            model.SetNodePropertyOnSelection(
                "layout.direction",
                std::string(ToJson(value.direction)),
                recordUndo,
                "Edit UI Layout Direction");
        }
        if (UIDocumentEditChanged(changedPaths, "align"))
        {
            model.SetNodePropertyOnSelection(
                "layout.align",
                std::string(ToJson(value.align)),
                recordUndo,
                "Edit UI Layout Align");
        }
        if (UIDocumentEditChanged(changedPaths, "spacing"))
            model.SetNodePropertyOnSelection("layout.spacing", value.spacing, recordUndo);
        if (UIDocumentEditChanged(changedPaths, "flexGrow"))
            model.SetNodePropertyOnSelection("layout.flexGrow", value.flexGrow, recordUndo);
        if (UIDocumentEditChanged(changedPaths, "gridColumns"))
            model.SetNodePropertyOnSelection("layout.gridColumns", value.gridColumns, recordUndo);
        if (UIDocumentEditChanged(changedPaths, "padding"))
            ApplyVector2(model, "layout.padding", value.padding, recordUndo);
        if (UIDocumentEditChanged(changedPaths, "margin"))
            ApplyVector2(model, "layout.margin", value.margin, recordUndo);
        if (UIDocumentEditChanged(changedPaths, "clipChildren"))
            model.SetNodePropertyOnSelection("layout.clipChildren", value.clipChildren, recordUndo);
    }

    void ApplyStyleEdit(
        UIDocumentDesignerModel& model,
        const UIDocumentStyleEdit& value,
        const std::vector<std::string>& changedPaths,
        const bool recordUndo)
    {
        if (UIDocumentEditChanged(changedPaths, "backgroundColor"))
            ApplyColor(model, "style.backgroundColor", value.backgroundColor, recordUndo);
        if (UIDocumentEditChanged(changedPaths, "color"))
            ApplyColor(model, "style.color", value.color, recordUndo);
        if (UIDocumentEditChanged(changedPaths, "opacity"))
            model.SetNodePropertyOnSelection("style.opacity", value.opacity, recordUndo);
        if (UIDocumentEditChanged(changedPaths, "visible"))
            model.SetNodePropertyOnSelection("style.visible", value.visible, recordUndo);
        if (UIDocumentEditChanged(changedPaths, "enabled"))
            model.SetNodePropertyOnSelection("style.enabled", value.enabled, recordUndo);
        if (UIDocumentEditChanged(changedPaths, "interactive"))
            model.SetNodePropertyOnSelection("style.interactive", value.interactive, recordUndo);
    }

    void ApplyContentEdit(
        UIDocumentDesignerModel& model,
        const UIDocumentContentEdit& value,
        const std::vector<std::string>& changedPaths,
        const bool recordUndo)
    {
        if (UIDocumentEditChanged(changedPaths, "text"))
            model.SetNodePropertyOnSelection("text", value.text, recordUndo, "Edit UI Text");
        if (UIDocumentEditChanged(changedPaths, "password"))
            model.SetNodePropertyOnSelection("password", value.password, recordUndo);

        const bool textureChanged = UIDocumentEditChanged(changedPaths, "texture");
        if (textureChanged || UIDocumentEditChanged(changedPaths, "texture.assetId"))
        {
            model.SetNodePropertyOnSelection(
                "texture.assetId",
                value.texture.GetAssetIdString(),
                recordUndo,
                "Edit UI Texture");
        }
        if (textureChanged || UIDocumentEditChanged(changedPaths, "texture.subAssetKey"))
        {
            model.SetNodePropertyOnSelection(
                "texture.subAssetKey",
                value.texture.GetSubAssetKey(),
                recordUndo,
                "Edit UI Texture");
        }
        if (textureChanged || UIDocumentEditChanged(changedPaths, "texture.path"))
        {
            model.SetNodePropertyOnSelection(
                "texture.path",
                value.texture.GetResourcePath(),
                recordUndo,
                "Edit UI Texture");
        }

        const bool fontChanged = UIDocumentEditChanged(changedPaths, "font");
        if (fontChanged || UIDocumentEditChanged(changedPaths, "font.assetId"))
        {
            model.SetNodePropertyOnSelection(
                "font.assetId",
                value.font.GetAssetIdString(),
                recordUndo,
                "Edit UI Font");
        }
        if (fontChanged || UIDocumentEditChanged(changedPaths, "font.subAssetKey"))
        {
            model.SetNodePropertyOnSelection(
                "font.subAssetKey",
                value.font.GetSubAssetKey(),
                recordUndo,
                "Edit UI Font");
        }
        if (fontChanged || UIDocumentEditChanged(changedPaths, "font.path"))
        {
            model.SetNodePropertyOnSelection(
                "font.path",
                value.font.GetResourcePath(),
                recordUndo,
                "Edit UI Font");
        }

        const bool nineSliceChanged = UIDocumentEditChanged(changedPaths, "nineSlice");
        if (nineSliceChanged || UIDocumentEditChanged(changedPaths, "nineSlice.left"))
        {
            model.SetNodePropertyOnSelection(
                "nineSlice.left",
                value.nineSlice.left,
                recordUndo,
                "Edit UI Nine Slice");
        }
        if (nineSliceChanged || UIDocumentEditChanged(changedPaths, "nineSlice.top"))
        {
            model.SetNodePropertyOnSelection(
                "nineSlice.top",
                value.nineSlice.top,
                recordUndo,
                "Edit UI Nine Slice");
        }
        if (nineSliceChanged || UIDocumentEditChanged(changedPaths, "nineSlice.right"))
        {
            model.SetNodePropertyOnSelection(
                "nineSlice.right",
                value.nineSlice.right,
                recordUndo,
                "Edit UI Nine Slice");
        }
        if (nineSliceChanged || UIDocumentEditChanged(changedPaths, "nineSlice.bottom"))
        {
            model.SetNodePropertyOnSelection(
                "nineSlice.bottom",
                value.nineSlice.bottom,
                recordUndo,
                "Edit UI Nine Slice");
        }

        if (UIDocumentEditChanged(changedPaths, "fontAssetId"))
            model.SetNodePropertyOnSelection("fontAssetId", value.fontAssetId, recordUndo);
        if (UIDocumentEditChanged(changedPaths, "fontPixelSize"))
            model.SetNodePropertyOnSelection("fontPixelSize", value.fontPixelSize, recordUndo);
        if (UIDocumentEditChanged(changedPaths, "requiresRenderTarget"))
        {
            model.SetNodePropertyOnSelection(
                "requiresRenderTarget",
                value.requiresRenderTarget,
                recordUndo);
        }
    }

    const std::vector<UIDocumentEditField>& GetUIDocumentEditFields()
    {
        static const std::vector<UIDocumentEditField> fields {
            { "name", "name" },
            { "className", "className" },
            { "anchorMin", "layout.anchorMin" },
            { "anchorMax", "layout.anchorMax" },
            { "pivot", "layout.pivot" },
            { "offsetMin", "layout.offsetMin" },
            { "offsetMax", "layout.offsetMax" },
            { "size", "layout.size" },
            { "minSize", "layout.minSize" },
            { "maxSize", "layout.maxSize" },
            { "aspectRatio", "layout.aspectRatio" },
            { "mode", "layout.mode" },
            { "direction", "layout.direction" },
            { "align", "layout.align" },
            { "spacing", "layout.spacing" },
            { "flexGrow", "layout.flexGrow" },
            { "gridColumns", "layout.gridColumns" },
            { "padding", "layout.padding" },
            { "margin", "layout.margin" },
            { "clipChildren", "layout.clipChildren" },
            { "backgroundColor", "style.backgroundColor" },
            { "color", "style.color" },
            { "opacity", "style.opacity" },
            { "visible", "style.visible" },
            { "enabled", "style.enabled" },
            { "interactive", "style.interactive" },
            { "text", "text" },
            { "password", "password" },
            { "texture", "texture.assetId" },
            { "font", "font.assetId" },
            { "nineSlice", "nineSlice.left" },
            { "fontAssetId", "fontAssetId" },
            { "fontPixelSize", "fontPixelSize" },
            { "requiresRenderTarget", "requiresRenderTarget" }
        };
        return fields;
    }

    bool UIDocumentEditChanged(
        const std::vector<std::string>& changedPaths,
        const std::string_view field)
    {
        return std::any_of(
            changedPaths.begin(),
            changedPaths.end(),
            [field](const std::string& path)
            {
                return path == field || path.ends_with("." + std::string(field));
            });
    }
}
