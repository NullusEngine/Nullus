#pragma once

// Declarative field metadata for editor-side reflected types.
//
// External reflection registration still needs NLS_META_EXTERNAL_FIELD (the
// type and member pointer must be visible at compile time), but display names,
// tooltips, ranges and multiline flags can be declared in one table instead of
// one Register() call per field.

#include <initializer_list>
#include <optional>
#include <utility>

#include "Inspector/PropertyTypeTraits.h"

namespace NLS::Editor::Inspector
{
    struct EditorFieldRegistration
    {
        const char* field = "";
        const char* displayName = "";
        const char* tooltip = "";
        bool multiline = false;
        std::optional<float> min;
        std::optional<float> max;
        std::optional<float> rangeMin;
        std::optional<float> rangeMax;
    };

    inline void RegisterEditorStructMetadata(
        const meta::Type& type,
        const std::initializer_list<EditorFieldRegistration> fields)
    {
        auto& registry = InspectorMetadataRegistry::Get();
        for (const auto& field : fields)
        {
            EditorPropertyMetadata metadata;
            metadata.displayName = field.displayName;
            metadata.tooltip = field.tooltip;
            metadata.multiline = field.multiline;
            metadata.min = field.min;
            metadata.max = field.max;
            metadata.rangeMin = field.rangeMin;
            metadata.rangeMax = field.rangeMax;
            registry.Register(type.GetName(), field.field, std::move(metadata));
        }
    }
}
