#include "Inspector/ScriptSerializedAdapter.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <Scripting/ScriptComponent.h>

#include "Inspector/PropertyTypeTraits.h"

namespace NLS::Editor::Inspector
{
namespace
{
uint64_t HashCombine(const uint64_t seed, const uint64_t value)
{
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U));
}

uint64_t HashString(const std::string_view value)
{
    return static_cast<uint64_t>(std::hash<std::string_view> {}(value));
}

uint64_t HashScriptMetadata(const NLS::Scripting::ScriptFieldMetadata& metadata)
{
    uint64_t version = 0xcbf29ce484222325ULL;
    version = HashCombine(version, HashString(metadata.displayName));
    version = HashCombine(version, HashString(metadata.tooltip));
    version = HashCombine(version, HashString(metadata.header));
    version = HashCombine(version, static_cast<uint64_t>(std::hash<float> {}(metadata.spaceBefore)));
    const auto appendOptionalFloat = [&version](const std::optional<float>& value)
    {
        version = HashCombine(version, value.has_value() ? 1u : 0u);
        if (value.has_value())
            version = HashCombine(version, static_cast<uint64_t>(std::hash<float> {}(*value)));
    };
    appendOptionalFloat(metadata.rangeMin);
    appendOptionalFloat(metadata.rangeMax);
    appendOptionalFloat(metadata.min);
    appendOptionalFloat(metadata.max);
    version = HashCombine(version, metadata.multiline ? 1u : 0u);
    version = HashCombine(version, metadata.readOnly ? 1u : 0u);
    version = HashCombine(version, metadata.hideInInspector ? 1u : 0u);
    version = HashCombine(version, metadata.flags ? 1u : 0u);
    version = HashCombine(version, metadata.colorUsage ? 1u : 0u);
    return HashCombine(version, HashString(metadata.referenceTypeConstraint));
}

uint64_t HashScriptField(const NLS::Scripting::ScriptFieldDescriptor& field)
{
    uint64_t version = field.id;
    version = HashCombine(version, HashString(field.name));
    version = HashCombine(version, field.type.id);
    version = HashCombine(version, static_cast<uint64_t>(field.type.kind));
    version = HashCombine(version, HashString(field.type.name));
    version = HashCombine(version, field.type.size);
    version = HashCombine(version, field.type.alignment);
    version = HashCombine(version, field.type.portable ? 1u : 0u);
    version = HashCombine(version, field.serialized ? 1u : 0u);
    return HashCombine(version, HashScriptMetadata(field.metadata));
}

std::string GetSimpleScriptTypeName(std::string_view typeName)
{
    while (typeName.starts_with("global::"))
        typeName.remove_prefix(8u);

    if (const auto namespaceSeparator = typeName.rfind("::"); namespaceSeparator != std::string_view::npos)
        typeName.remove_prefix(namespaceSeparator + 2u);
    if (const auto dotSeparator = typeName.rfind('.'); dotSeparator != std::string_view::npos)
        typeName.remove_prefix(dotSeparator + 1u);
    if (const auto nestedTypeSeparator = typeName.rfind('+'); nestedTypeSeparator != std::string_view::npos)
        typeName.remove_prefix(nestedTypeSeparator + 1u);

    // C# metadata can carry a generic arity suffix (for example `Foo`1`) or
    // an assembly qualification. Neither belongs in an Inspector title.
    if (const auto assemblySeparator = typeName.find(','); assemblySeparator != std::string_view::npos)
        typeName = typeName.substr(0u, assemblySeparator);
    if (const auto genericArity = typeName.find('`'); genericArity != std::string_view::npos)
        typeName = typeName.substr(0u, genericArity);
    return std::string(typeName);
}

bool IsIdentifierStart(const char value)
{
    return std::isalpha(static_cast<unsigned char>(value)) != 0 || value == '_';
}

bool IsIdentifierPart(const char value)
{
    return IsIdentifierStart(value) || std::isdigit(static_cast<unsigned char>(value)) != 0;
}

std::vector<std::string> TokenizeScriptSource(const std::string_view source)
{
    std::vector<std::string> tokens;
    size_t cursor = 0u;
    while (cursor < source.size())
    {
        const char current = source[cursor];
        if (std::isspace(static_cast<unsigned char>(current)) != 0)
        {
            ++cursor;
            continue;
        }

        const bool lineComment = cursor + 1u < source.size() &&
            ((current == '/' && source[cursor + 1u] == '/') ||
                (current == '-' && source[cursor + 1u] == '-'));
        if (lineComment)
        {
            cursor += 2u;
            while (cursor < source.size() && source[cursor] != '\n')
                ++cursor;
            continue;
        }

        if (current == '/' && cursor + 1u < source.size() && source[cursor + 1u] == '*')
        {
            cursor += 2u;
            while (cursor + 1u < source.size() && !(source[cursor] == '*' && source[cursor + 1u] == '/'))
                ++cursor;
            cursor = (std::min)(source.size(), cursor + 2u);
            continue;
        }

        if (current == '\'' || current == '"')
        {
            const char quote = current;
            ++cursor;
            while (cursor < source.size())
            {
                if (source[cursor] == '\\')
                {
                    cursor += (std::min)(size_t {2u}, source.size() - cursor);
                    continue;
                }
                if (source[cursor++] == quote)
                    break;
            }
            continue;
        }

        if (IsIdentifierStart(current))
        {
            const size_t start = cursor++;
            while (cursor < source.size() && IsIdentifierPart(source[cursor]))
                ++cursor;
            tokens.emplace_back(source.substr(start, cursor - start));
            continue;
        }

        tokens.emplace_back(1u, current);
        ++cursor;
    }
    return tokens;
}

bool IsGenericScriptTypeName(const std::string_view name)
{
    const auto simpleName = GetSimpleScriptTypeName(name);
    return simpleName.empty() || simpleName == "Script" || simpleName == "ScriptComponent";
}

std::optional<std::string> ExtractDeclaredScriptTypeName(const NLS::Scripting::ScriptAsset& asset)
{
    if (asset.sourceText.empty())
        return std::nullopt;

    const auto tokens = TokenizeScriptSource(asset.sourceText);
    if (asset.language == NLS::Scripting::ScriptLanguage::CSharp)
    {
        for (size_t index = 0u; index + 2u < tokens.size(); ++index)
        {
            if (tokens[index] != "class" || tokens[index + 1u].empty() ||
                !IsIdentifierStart(tokens[index + 1u].front()))
            {
                continue;
            }

            bool derivesFromBehaviour = false;
            for (size_t cursor = index + 2u; cursor < tokens.size(); ++cursor)
            {
                if (tokens[cursor] == "{" || tokens[cursor] == ";")
                    break;
                if (tokens[cursor] == "Behaviour")
                {
                    derivesFromBehaviour = true;
                    break;
                }
            }
            if (derivesFromBehaviour)
                return tokens[index + 1u];
        }
    }
    else if (asset.language == NLS::Scripting::ScriptLanguage::Lua)
    {
        // Lua scripts conventionally return a module table. Prefer a named
        // local table when present, then use `return Name` for module styles
        // that construct the table elsewhere.
        for (size_t index = 0u; index + 2u < tokens.size(); ++index)
        {
            if (tokens[index] == "local" &&
                !tokens[index + 1u].empty() &&
                IsIdentifierStart(tokens[index + 1u].front()) &&
                tokens[index + 2u] == "=")
            {
                return tokens[index + 1u];
            }
        }
        for (size_t index = 0u; index + 1u < tokens.size(); ++index)
        {
            if (tokens[index] == "return" &&
                !tokens[index + 1u].empty() &&
                IsIdentifierStart(tokens[index + 1u].front()))
            {
                return tokens[index + 1u];
            }
        }
    }
    return std::nullopt;
}

const char* GetScriptLanguageLabel(const NLS::Scripting::ScriptLanguage language)
{
    switch (language)
    {
    case NLS::Scripting::ScriptLanguage::CSharp: return "C#";
    case NLS::Scripting::ScriptLanguage::Lua: return "Lua";
    case NLS::Scripting::ScriptLanguage::Unknown:
    default: return "Script";
    }
}

} // namespace

std::string ScriptSerializedAdapter::GetComponentDisplayName(const NLS::Scripting::ScriptComponent& component)
{
    const auto& asset = component.GetScriptAsset();
    std::string name;
    if (const auto* descriptor = component.GetScriptTypeDescriptor(); descriptor != nullptr)
    {
        const auto descriptorName = GetSimpleScriptTypeName(descriptor->name);
        if (!IsGenericScriptTypeName(descriptorName))
            name = descriptorName;
    }

    // A component can be selected before its backend has been bound (for
    // example immediately after scene load or while a script assembly is
    // reloading). Keep the Inspector useful in that window by resolving the
    // declaration from the imported source payload before falling back to the
    // file stem. C# generation enforces file/type parity, while Lua modules
    // may use a local table name, so both paths are intentionally supported.
    if (name.empty())
    {
        if (const auto declaredName = ExtractDeclaredScriptTypeName(asset);
            declaredName.has_value() && !IsGenericScriptTypeName(*declaredName))
        {
            name = *declaredName;
        }
    }
    if (name.empty() && !asset.sourcePath.empty())
        name = std::filesystem::path(asset.sourcePath).stem().string();
    if (name.empty())
        return "Missing Script";

    return FormatInspectorDisplayName(name) + " (" + GetScriptLanguageLabel(asset.language) + ")";
}

uint64_t ScriptSerializedAdapter::GetComponentStructureVersion(const NLS::Scripting::ScriptComponent& component)
{
    const auto& asset = component.GetScriptAsset();
    uint64_t version = 0xcbf29ce484222325ULL;
    version = HashCombine(version, HashString(asset.assetId.ToString()));
    version = HashCombine(version, HashString(asset.sourcePath));
    version = HashCombine(version, static_cast<uint64_t>(asset.language));
    version = HashCombine(version, asset.scriptType);
    version = HashCombine(version, asset.isComponent ? 1u : 0u);
    if (const auto* descriptor = component.GetScriptTypeDescriptor(); descriptor != nullptr)
    {
        version = HashCombine(version, descriptor->id);
        version = HashCombine(version, HashString(descriptor->name));
        for (const auto& field : descriptor->fields)
            version = HashCombine(version, HashScriptField(field));
    }
    return version;
}

void ScriptSerializedAdapter::ApplyPropertyPresentation(
    const NLS::Scripting::ScriptComponent& component,
    PropertyNode& node)
{
    if (node.fieldName == "scriptAsset")
    {
        node.displayName = "Script Asset";
        node.readOnly = true;
        if (component.GetScriptAsset().sourcePath.empty())
        {
            node.diagnostic =
                "The Script Asset metadata is unavailable. Reimport or assign a valid script asset.";
        }
        return;
    }

    const auto* descriptor = component.GetScriptTypeDescriptor();
    if (descriptor == nullptr)
        return;

    const auto field = std::find_if(
        descriptor->fields.begin(), descriptor->fields.end(), [&node](const NLS::Scripting::ScriptFieldDescriptor& candidate)
        {
            return candidate.name == node.fieldName ||
                std::find(candidate.aliases.begin(), candidate.aliases.end(), node.fieldName) != candidate.aliases.end();
        });
    if (field == descriptor->fields.end())
        return;

    node.fieldId = field->id;
    node.aliases = field->aliases;
    node.metadata = BuildEditorPropertyMetadata(*field, descriptor->name);
    if (!node.metadata.displayName.empty())
        node.displayName = node.metadata.displayName;
    else
        node.displayName = FormatInspectorDisplayName(field->name);
    node.readOnly = node.readOnly || node.metadata.readOnly;
    if (node.kind == InspectorPropertyKind::Enum && node.metadata.flags)
        node.kind = InspectorPropertyKind::Flags;
}
} // namespace NLS::Editor::Inspector
