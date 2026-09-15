#pragma once

#include <string>
#include <string_view>

#include <Reflection/Field.h>

#include "Inspector/InspectorTypes.h"

namespace NLS::Scripting
{
struct ScriptFieldDescriptor;
}

namespace NLS::Editor::Inspector
{
[[nodiscard]] InspectorPropertyKind GetInspectorPropertyKind(const meta::Type& type);
[[nodiscard]] EditorPropertyMetadata BuildEditorPropertyMetadata(const meta::Field& field);
[[nodiscard]] EditorPropertyMetadata BuildEditorPropertyMetadata(
    const NLS::Scripting::ScriptFieldDescriptor& field,
    std::string_view ownerTypeName);
[[nodiscard]] EditorPropertyMetadata BuildEditorPropertyMetadata(
    std::string_view ownerTypeName,
    std::string_view fieldName,
    EditorPropertyMetadata reflectedMetadata = {});
[[nodiscard]] std::string FormatInspectorDisplayName(std::string_view name);

/**
 * Metadata overrides are deliberately independent of the ImGui renderer.  It
 * lets native fields and ScriptFieldDescriptor entries share one schema without
 * letting managed code execute editor UI directly.
 */
class InspectorMetadataRegistry final
{
public:
    static InspectorMetadataRegistry& Get();

    void Register(std::string typeName, std::string fieldName, EditorPropertyMetadata metadata);
    [[nodiscard]] EditorPropertyMetadata Resolve(
        const meta::Type& type,
        std::string_view fieldName,
        const EditorPropertyMetadata& reflectedMetadata = {}) const;
    [[nodiscard]] EditorPropertyMetadata Resolve(
        std::string_view typeName,
        std::string_view fieldName,
        const EditorPropertyMetadata& reflectedMetadata = {}) const;
    [[nodiscard]] uint64_t GetVersion() const;

private:
    struct Entry
    {
        std::string typeName;
        std::string fieldName;
        EditorPropertyMetadata metadata;
    };

    std::vector<Entry> m_entries;
    uint64_t m_version = 1u;
};
} // namespace NLS::Editor::Inspector
