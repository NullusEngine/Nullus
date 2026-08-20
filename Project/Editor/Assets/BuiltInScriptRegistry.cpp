#include "Assets/BuiltInScriptRegistry.h"

#include "Assets/ScriptAssetUtility.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>
#include <unordered_set>

namespace NLS::Editor::Assets
{
namespace
{
struct RegistryState
{
    std::vector<BuiltInScriptDescriptor> descriptors;
};

RegistryState& State()
{
    static RegistryState state;
    return state;
}

std::string LowerExtension(const std::filesystem::path& path)
{
    auto extension = path.extension().string();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension;
}

std::string CanonicalPathKey(const std::filesystem::path& path)
{
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error).lexically_normal();
    const auto value = (error ? path.lexically_normal() : absolute).generic_string();
#if defined(_WIN32)
    std::string normalized = value;
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return normalized;
#else
    return value;
#endif
}

std::string BuiltInSourcePath(
    const std::filesystem::path& file,
    const std::filesystem::path& root,
    const std::string& prefix)
{
    const auto relative = file.lexically_relative(root);
    const auto relativeText = relative.empty() || relative.is_absolute()
        ? file.filename().generic_string()
        : relative.generic_string();
    return prefix.empty() ? relativeText : prefix + relativeText;
}

void RegisterScriptFile(
    const std::filesystem::path& file,
    const std::filesystem::path& scriptRoot,
    const std::string& sourcePrefix)
{
    const auto extension = LowerExtension(file);
    if (extension != ".cs" && extension != ".lua")
        return;

    std::ifstream input(file, std::ios::binary);
    if (!input)
        return;
    const std::string source{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (source.empty())
        return;

    const auto language = extension == ".cs"
        ? NLS::Scripting::ScriptLanguage::CSharp
        : NLS::Scripting::ScriptLanguage::Lua;
    const auto sourcePath = BuiltInSourcePath(file, scriptRoot, sourcePrefix);

    BuiltInScriptDescriptor descriptor;
    descriptor.asset.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::NewDeterministic("Nullus.BuiltInScript:" + sourcePath));
    descriptor.asset.language = language;
    descriptor.asset.sourcePath = sourcePath;
    descriptor.asset.sourceText = source;
    descriptor.asset.scriptType = NLS::Scripting::MakeStableScriptId(
        std::string(language == NLS::Scripting::ScriptLanguage::CSharp ? "CSharp:" : "Lua:") + sourcePath);
    descriptor.asset.contentHash = NLS::Scripting::MakeScriptContentHash(source);
    descriptor.asset.isComponent = IsScriptComponentSource(language, source);
    descriptor.menuPath = "Scripts/Engine/" +
        std::string(language == NLS::Scripting::ScriptLanguage::CSharp ? "C#" : "Lua");
    descriptor.sourceFile = file.lexically_normal();
    BuiltInScriptRegistry::Register(std::move(descriptor));
}

void ScanScriptRoot(
    const std::filesystem::path& scriptRoot,
    const std::string& sourcePrefix,
    std::unordered_set<std::string>& visitedFiles)
{
    std::error_code error;
    if (!std::filesystem::is_directory(scriptRoot, error))
        return;

    std::filesystem::recursive_directory_iterator iterator(
        scriptRoot,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end && !error; iterator.increment(error))
    {
        const auto status = iterator->symlink_status(error);
        if (error)
        {
            error.clear();
            continue;
        }
        if (!std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status))
            continue;
        if (!IsScriptAssetPath(iterator->path()))
            continue;
        if (!visitedFiles.insert(CanonicalPathKey(iterator->path())).second)
            continue;
        RegisterScriptFile(iterator->path(), scriptRoot, sourcePrefix);
    }
}
}

void BuiltInScriptRegistry::Register(BuiltInScriptDescriptor descriptor)
{
    if (!descriptor.asset.assetId.IsValid() ||
        descriptor.asset.language == NLS::Scripting::ScriptLanguage::Unknown ||
        descriptor.asset.sourcePath.empty() ||
        descriptor.asset.sourceText.empty())
    {
        return;
    }

    auto& descriptors = State().descriptors;
    const auto found = std::find_if(
        descriptors.begin(),
        descriptors.end(),
        [&descriptor](const BuiltInScriptDescriptor& existing)
        {
            return existing.asset.assetId == descriptor.asset.assetId;
        });
    if (found != descriptors.end())
        *found = std::move(descriptor);
    else
        descriptors.push_back(std::move(descriptor));

    std::sort(
        descriptors.begin(),
        descriptors.end(),
        [](const BuiltInScriptDescriptor& left, const BuiltInScriptDescriptor& right)
        {
            if (left.menuPath != right.menuPath)
                return left.menuPath < right.menuPath;
            return left.asset.sourcePath < right.asset.sourcePath;
        });
}

void BuiltInScriptRegistry::Refresh(const std::filesystem::path& installRoot)
{
    Clear();
    if (installRoot.empty())
        return;

    const auto root = installRoot.lexically_normal();
    std::unordered_set<std::string> visitedFiles;
    // Source checkout layout.  This is the canonical location for scripts
    // that ship with the current Nullus build.
    ScanScriptRoot(root / "Managed" / "Nullus.EngineScripts" / "Scripts", {}, visitedFiles);
    // Packaged/editor layout.  Keep this path available for installations that
    // ship engine scripts as regular Engine assets instead of source projects.
    ScanScriptRoot(root / "Assets" / "Engine" / "Scripts", "Assets/Engine/", visitedFiles);
    ScanScriptRoot(root / "App" / "Assets" / "Engine" / "Scripts", "Assets/Engine/", visitedFiles);
}

const std::vector<BuiltInScriptDescriptor>& BuiltInScriptRegistry::GetAll()
{
    return State().descriptors;
}

const BuiltInScriptDescriptor* BuiltInScriptRegistry::FindByAssetId(
    const NLS::Core::Assets::AssetId& assetId)
{
    const auto& descriptors = State().descriptors;
    const auto found = std::find_if(
        descriptors.begin(),
        descriptors.end(),
        [&assetId](const BuiltInScriptDescriptor& descriptor)
        {
            return descriptor.asset.assetId == assetId;
        });
    return found == descriptors.end() ? nullptr : &(*found);
}

void BuiltInScriptRegistry::Clear()
{
    State().descriptors.clear();
}
}
