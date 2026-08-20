#pragma once

#include "Assets/EditorAssetDragPayload.h"
#include <Scripting/ScriptTypes.h>

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace NLS::Editor::Assets
{
bool IsScriptAssetPath(const std::filesystem::path& path);

// Performs the inexpensive source-shape check used by editor component
// workflows. C# requires a public concrete Behaviour class; Lua requires a
// module table returned as the final top-level expression.
bool IsScriptComponentSource(
    NLS::Scripting::ScriptLanguage language,
    std::string_view source);

std::filesystem::path ResolveProjectScriptPath(
    const std::filesystem::path& projectAssetsFolder,
    const EditorAssetDragPayload& payload);

std::optional<NLS::Scripting::ScriptAsset> LoadScriptAsset(
    const std::filesystem::path& projectAssetsFolder,
    const EditorAssetDragPayload& payload);

// Returns imported project scripts with a valid .meta AssetId. The returned
// source paths use the same Assets/... form as Asset Browser drag payloads.
std::vector<NLS::Scripting::ScriptAsset> FindProjectScriptAssets(
    const std::filesystem::path& projectAssetsFolder);
}
