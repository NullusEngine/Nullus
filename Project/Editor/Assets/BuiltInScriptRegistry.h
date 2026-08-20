#pragma once

#include <Scripting/ScriptTypes.h>

#include <filesystem>
#include <string>
#include <vector>

namespace NLS::Editor::Assets
{
// Metadata for a script shipped by Nullus rather than imported from a game
// project's Assets folder.  The ScriptAsset is still the single payload used
// by ScriptComponent, so mounting and runtime loading stay identical for both
// sources.
struct BuiltInScriptDescriptor
{
    NLS::Scripting::ScriptAsset asset;
    std::string menuPath;
    std::filesystem::path sourceFile;
};

class BuiltInScriptRegistry final
{
public:
    // Registers one descriptor for tests and for future generated registries.
    // Existing entries with the same AssetId are replaced deterministically.
    static void Register(BuiltInScriptDescriptor descriptor);

    // Rebuilds the built-in registry from an installation/repository root.
    // This is intentionally explicit so an editor context can refresh after
    // resolving its actual install root without relying on the process cwd.
    static void Refresh(const std::filesystem::path& installRoot);

    static const std::vector<BuiltInScriptDescriptor>& GetAll();
    static const BuiltInScriptDescriptor* FindByAssetId(
        const NLS::Core::Assets::AssetId& assetId);
    static void Clear();
};
}
