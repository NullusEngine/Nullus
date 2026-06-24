#include "Assets/AssetBrowserPresentation.h"

#include "Assets/AssetMeta.h"
#include "Assets/EditorAssetPath.h"
#include "Assets/NativeArtifactContainer.h"
#include "Profiling/Profiler.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace NLS::Editor::Assets
{
namespace
{
std::vector<EditorAssetRoot> g_objectReferencePickerAssetRoots;
std::vector<ObjectReferencePickerEntry> g_objectReferencePickerEntries;
ObjectReferencePickerEntriesProvider g_objectReferencePickerEntriesProvider;
bool g_objectReferencePickerEntriesDirty = false;
constexpr uint64_t kMaxDisplayNameArtifactProbeBytes = 1024ull * 1024ull;

bool IsInspectorReferenceableSubAsset(const NLS::Core::Assets::ArtifactType p_type)
{
    using NLS::Core::Assets::ArtifactType;
    return p_type == ArtifactType::Mesh ||
           p_type == ArtifactType::Material ||
           p_type == ArtifactType::Texture ||
           p_type == ArtifactType::Shader;
}

bool IsGridVisibleGeneratedSubAsset(const NLS::Core::Assets::ArtifactType p_type)
{
    using NLS::Core::Assets::ArtifactType;
    return p_type == ArtifactType::Prefab ||
           p_type == ArtifactType::Mesh ||
           p_type == ArtifactType::Material ||
           p_type == ArtifactType::Texture ||
           p_type == ArtifactType::Shader;
}

bool AssetTypeCanHaveGeneratedSubAssets(const AssetBrowserItemType type)
{
    return type == AssetBrowserItemType::Model ||
           type == AssetBrowserItemType::Prefab;
}

NLS::Core::Assets::ArtifactType DefaultDragArtifactTypeForSourceAssetType(
    const AssetBrowserItemType type)
{
    using NLS::Core::Assets::ArtifactType;
    switch (type)
    {
    case AssetBrowserItemType::Model:
    case AssetBrowserItemType::Prefab:
        return ArtifactType::Prefab;
    case AssetBrowserItemType::Material:
        return ArtifactType::Material;
    case AssetBrowserItemType::Texture:
        return ArtifactType::Texture;
    case AssetBrowserItemType::Shader:
        return ArtifactType::Shader;
    case AssetBrowserItemType::All:
    case AssetBrowserItemType::Folder:
    case AssetBrowserItemType::Mesh:
    case AssetBrowserItemType::Scene:
    case AssetBrowserItemType::Script:
    case AssetBrowserItemType::Other:
    case AssetBrowserItemType::Count:
        return ArtifactType::Unknown;
    }
    return ArtifactType::Unknown;
}

std::string DefaultDragSubAssetKeyForSourceAsset(
    const AssetBrowserItemType type,
    const std::string& resourcePath)
{
    const auto stem = std::filesystem::path(resourcePath).stem().generic_string();
    if (stem.empty())
        return {};

    switch (type)
    {
    case AssetBrowserItemType::Model:
    case AssetBrowserItemType::Prefab:
        return "prefab:" + stem;
    case AssetBrowserItemType::Material:
        return "material:" + stem;
    case AssetBrowserItemType::Texture:
        return "texture:" + stem;
    case AssetBrowserItemType::Shader:
        return "shader:" + stem;
    case AssetBrowserItemType::All:
    case AssetBrowserItemType::Folder:
    case AssetBrowserItemType::Mesh:
    case AssetBrowserItemType::Scene:
    case AssetBrowserItemType::Script:
    case AssetBrowserItemType::Other:
    case AssetBrowserItemType::Count:
        return {};
    }
    return {};
}

std::string MakeSubAssetDisplayName(const std::string& p_subAssetKey)
{
    const auto separator = p_subAssetKey.find(':');
    if (separator == std::string::npos || separator + 1u >= p_subAssetKey.size())
        return p_subAssetKey;

    auto name = p_subAssetKey.substr(separator + 1u);
    const auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos && slash + 1u < name.size())
        name = name.substr(slash + 1u);
    return name;
}

std::string PrettyTypeName(const NLS::Core::Assets::ArtifactType type)
{
    using NLS::Core::Assets::ArtifactType;
    switch (type)
    {
    case ArtifactType::Prefab: return "Prefab";
    case ArtifactType::Mesh: return "Mesh";
    case ArtifactType::Material: return "Material";
    case ArtifactType::Texture: return "Texture";
    case ArtifactType::Shader: return "Shader";
    case ArtifactType::Scene: return "Scene";
    default: return "Sub Asset";
    }
}

std::string MakeReadableGeneratedSubAssetFallback(
    const AssetDatabaseRecord& record,
    const std::string& rawName)
{
    if (rawName.empty())
        return PrettyTypeName(record.artifactType);

    const auto numeric = std::all_of(
        rawName.begin(),
        rawName.end(),
        [](const unsigned char ch)
        {
            return std::isdigit(ch) != 0;
        });
    if (!numeric)
        return rawName;

    return PrettyTypeName(record.artifactType) + " " + rawName;
}

std::optional<std::string> ExtractLineValue(
    const std::string& text,
    const std::string& key)
{
    size_t position = 0u;
    while ((position = text.find(key, position)) != std::string::npos)
    {
        if (position != 0u && text[position - 1u] != '\n' && text[position - 1u] != '\r')
        {
            position += key.size();
            continue;
        }

        const auto valueBegin = position + key.size();
        auto valueEnd = text.find('\n', valueBegin);
        if (valueEnd == std::string::npos)
            valueEnd = text.size();
        while (valueEnd > valueBegin &&
            (text[valueEnd - 1u] == '\r' || text[valueEnd - 1u] == '\n'))
        {
            --valueEnd;
        }
        if (valueEnd > valueBegin)
            return text.substr(valueBegin, valueEnd - valueBegin);
        position = valueBegin;
    }
    return std::nullopt;
}

std::optional<std::string> ExtractMaterialName(const std::string& text)
{
    const std::string beginTag = "<name>";
    const std::string endTag = "</name>";
    const auto begin = text.find(beginTag);
    if (begin == std::string::npos)
        return std::nullopt;
    const auto valueBegin = begin + beginTag.size();
    const auto end = text.find(endTag, valueBegin);
    if (end == std::string::npos || end <= valueBegin)
        return std::nullopt;
    return text.substr(valueBegin, end - valueBegin);
}

uint32_t PreviewMetadataSchemaVersionForArtifactType(const NLS::Core::Assets::ArtifactType artifactType)
{
    switch (artifactType)
    {
    case NLS::Core::Assets::ArtifactType::Mesh:
        return 3u;
    case NLS::Core::Assets::ArtifactType::Texture:
        return 4u;
    case NLS::Core::Assets::ArtifactType::Material:
    case NLS::Core::Assets::ArtifactType::Prefab:
    case NLS::Core::Assets::ArtifactType::Shader:
    case NLS::Core::Assets::ArtifactType::Scene:
    case NLS::Core::Assets::ArtifactType::Audio:
    case NLS::Core::Assets::ArtifactType::Skeleton:
    case NLS::Core::Assets::ArtifactType::Skin:
    case NLS::Core::Assets::ArtifactType::AnimationClip:
    case NLS::Core::Assets::ArtifactType::MorphTarget:
    case NLS::Core::Assets::ArtifactType::Model:
    case NLS::Core::Assets::ArtifactType::Unknown:
    case NLS::Core::Assets::ArtifactType::Count:
        return 1u;
    }
    return 1u;
}

std::optional<std::string> ReadArtifactDisplayName(const AssetDatabaseRecord& record)
{
    if (record.artifactPath.empty())
        return std::nullopt;
    if (record.artifactType != NLS::Core::Assets::ArtifactType::Mesh &&
        record.artifactType != NLS::Core::Assets::ArtifactType::Material &&
        record.artifactType != NLS::Core::Assets::ArtifactType::Prefab &&
        record.artifactType != NLS::Core::Assets::ArtifactType::Shader &&
        record.artifactType != NLS::Core::Assets::ArtifactType::Unknown)
    {
        return std::nullopt;
    }
    if (auto prefix = NLS::Core::Assets::ReadNativeArtifactPayloadPrefixFromFile(
            record.artifactPath,
            record.artifactType,
            PreviewMetadataSchemaVersionForArtifactType(record.artifactType),
            0u);
        prefix.has_value() && !prefix->metadata.displayName.empty())
    {
        return prefix->metadata.displayName;
    }

    std::error_code error;
    const auto fileSize = std::filesystem::file_size(record.artifactPath, error);
    if (error || fileSize > kMaxDisplayNameArtifactProbeBytes)
        return std::nullopt;

    std::ifstream input(std::filesystem::path(record.artifactPath), std::ios::binary);
    if (!input)
        return std::nullopt;

    std::string bytes {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
    if (bytes.empty())
        return std::nullopt;

    if (auto displayName = ExtractLineValue(bytes, "DISPLAY_NAME=");
        displayName.has_value() && !displayName->empty())
    {
        return displayName;
    }

    if (record.artifactType == NLS::Core::Assets::ArtifactType::Material)
        return ExtractMaterialName(bytes);

    return std::nullopt;
}

std::string ExtractTextureSourceKeyFromSubAssetKey(const std::string& subAssetKey)
{
    constexpr std::string_view kPrefix = "texture:";
    if (subAssetKey.rfind(kPrefix, 0u) != 0u)
        return {};
    return subAssetKey.substr(kPrefix.size());
}

std::optional<std::string> ExtractTextureSourcePathFromDependency(
    const NLS::Core::Assets::AssetDependencyRecord& dependency,
    const std::string& textureSourceKey)
{
    if (dependency.kind != NLS::Core::Assets::AssetDependencyKind::PostprocessorVersion ||
        textureSourceKey.empty())
    {
        return std::nullopt;
    }

    const std::string buildPrefix = "texture-build:texture:" + textureSourceKey;
    if (dependency.value != buildPrefix)
        return std::nullopt;

    const std::string sourceNeedle = "sourcePath=";
    const auto sourceBegin = dependency.hashOrVersion.find(sourceNeedle);
    if (sourceBegin == std::string::npos)
        return std::nullopt;

    const auto valueBegin = sourceBegin + sourceNeedle.size();
    auto valueEnd = dependency.hashOrVersion.find('|', valueBegin);
    if (valueEnd == std::string::npos)
        valueEnd = dependency.hashOrVersion.size();
    if (valueEnd <= valueBegin)
        return std::nullopt;

    auto sourcePath = dependency.hashOrVersion.substr(valueBegin, valueEnd - valueBegin);
    std::replace(sourcePath.begin(), sourcePath.end(), '\\', '/');
    return sourcePath;
}

std::unordered_map<std::string, std::string> BuildGeneratedSubAssetDisplayNameOverrides(
    const std::vector<AssetDatabaseRecord>& records,
    const AssetDatabaseFacade* database,
    const std::string& sourceAssetPath)
{
    std::unordered_map<std::string, std::string> overrides;
    if (database == nullptr || sourceAssetPath.empty())
        return overrides;

    const auto manifest = database->GetArtifactManifestForAssetPath(sourceAssetPath);
    if (!manifest.has_value())
        return overrides;

    for (const auto& record : records)
    {
        if (record.artifactType != NLS::Core::Assets::ArtifactType::Texture)
            continue;

        const auto textureSourceKey = ExtractTextureSourceKeyFromSubAssetKey(record.subAssetKey);
        if (textureSourceKey.empty())
            continue;

        for (const auto& dependency : manifest->dependencies)
        {
            const auto sourcePath = ExtractTextureSourcePathFromDependency(dependency, textureSourceKey);
            if (!sourcePath.has_value())
                continue;

            const auto filename = std::filesystem::path(*sourcePath).filename().generic_string();
            if (!filename.empty())
                overrides[record.subAssetKey] = filename;
            break;
        }
    }
    return overrides;
}

std::string PathToGenericUtf8String(const std::filesystem::path& path)
{
    const auto text = path.lexically_normal().generic_u8string();
    return { reinterpret_cast<const char*>(text.data()), text.size() };
}

std::string PathFilenameToGenericUtf8String(const std::filesystem::path& path)
{
    const auto text = path.filename().generic_u8string();
    return { reinterpret_cast<const char*>(text.data()), text.size() };
}

std::optional<std::filesystem::path> PathFromProjectRelativeUtf8(const std::string& path)
{
    std::u8string utf8;
    utf8.reserve(path.size());
    for (const auto character : path)
        utf8.push_back(static_cast<char8_t>(static_cast<unsigned char>(character)));

    try
    {
        return std::filesystem::path(utf8);
    }
    catch (const std::filesystem::filesystem_error&)
    {
        return std::nullopt;
    }
    catch (const std::system_error&)
    {
        return std::nullopt;
    }
}

std::filesystem::path ResolveAssetsRoot(const std::filesystem::path& projectRootOrAssetsRoot)
{
    auto root = projectRootOrAssetsRoot.lexically_normal();
    while (!root.empty() && !root.has_filename())
        root = root.parent_path();

    std::error_code error;
    if (root.filename() == "Assets" && std::filesystem::is_directory(root, error))
        return root;

    error.clear();
    const auto assets = root / "Assets";
    if (std::filesystem::is_directory(assets, error))
        return assets.lexically_normal();

    return root.filename() == "Assets" ? root : assets.lexically_normal();
}

std::string NormalizeProjectRelativePath(std::filesystem::path path)
{
    return NormalizeAssetBrowserProjectRelativePath(PathToGenericUtf8String(path));
}

bool IsContainedRelativePath(const std::filesystem::path& relative)
{
    if (relative.is_absolute())
        return false;

    for (const auto& part : relative)
    {
        if (part == "..")
            return false;
    }
    return true;
}

std::string ToProjectRelativePath(
    const std::filesystem::path& assetsRoot,
    const std::filesystem::path& absolutePath)
{
    const auto normalizedAbsolute = absolutePath.lexically_normal();
    const auto normalizedAssetsRoot = assetsRoot.lexically_normal();
    if (normalizedAbsolute == normalizedAssetsRoot)
        return "Assets";

    const auto relative = normalizedAbsolute.lexically_relative(normalizedAssetsRoot);
    if (relative.empty() || !IsContainedRelativePath(relative))
        return {};
    return NormalizeAssetBrowserProjectRelativePath("Assets/" + PathToGenericUtf8String(relative));
}

std::filesystem::path AbsolutePathForProjectRelative(
    const std::filesystem::path& assetsRoot,
    const std::string& projectRelativePath)
{
    const auto normalized = NormalizeAssetBrowserProjectRelativePath(projectRelativePath);
    if (normalized == "Assets")
        return assetsRoot.lexically_normal();

    if (normalized.compare(0u, 7u, "Assets/") != 0)
        return {};

    const auto relative = PathFromProjectRelativeUtf8(normalized.substr(7u));
    if (!relative.has_value() || relative->empty() || !IsContainedRelativePath(*relative))
        return {};
    return (assetsRoot / *relative).lexically_normal();
}

bool IsPhysicalDirectoryEntry(const std::filesystem::directory_entry& entry)
{
    std::error_code error;
    const auto status = entry.symlink_status(error);
    if (error)
        return false;

    return std::filesystem::is_directory(status) && !std::filesystem::is_symlink(status);
}

bool IsPhysicalRegularFileEntry(const std::filesystem::directory_entry& entry)
{
    std::error_code error;
    const auto status = entry.symlink_status(error);
    if (error)
        return false;

    return std::filesystem::is_regular_file(status) && !std::filesystem::is_symlink(status);
}

bool IsPhysicalRegularFilePath(const std::filesystem::path& path)
{
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error)
        return false;

    return std::filesystem::is_regular_file(status) && !std::filesystem::is_symlink(status);
}

bool IsPhysicalDirectoryPath(const std::filesystem::path& path)
{
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error)
        return false;

    return std::filesystem::is_directory(status) && !std::filesystem::is_symlink(status);
}

bool IsPhysicalPathInsideAssetsRoot(
    const std::filesystem::path& candidate,
    const std::filesystem::path& assetsRoot)
{
    const auto canonicalCandidate = TryWeaklyCanonicalEditorPath(candidate);
    const auto canonicalAssetsRoot = TryWeaklyCanonicalEditorPath(assetsRoot);
    return canonicalCandidate.has_value() &&
        canonicalAssetsRoot.has_value() &&
        IsPathInsideEditorAssetRoot(*canonicalCandidate, *canonicalAssetsRoot);
}

bool IsUsableAssetsRoot(const std::filesystem::path& assetsRoot)
{
    if (!IsPhysicalDirectoryPath(assetsRoot))
        return false;

    const auto projectRoot = assetsRoot.parent_path();
    const auto canonicalAssetsRoot = TryWeaklyCanonicalEditorPath(assetsRoot);
    const auto canonicalProjectRoot = TryWeaklyCanonicalEditorPath(projectRoot);
    return canonicalAssetsRoot.has_value() &&
        canonicalProjectRoot.has_value() &&
        IsPathInsideEditorAssetRoot(*canonicalAssetsRoot, *canonicalProjectRoot);
}

struct FolderDirectoryEntry
{
    std::filesystem::directory_entry entry;
    std::string displayName;
};

bool HasPhysicalChildDirectory(const std::filesystem::path& absolutePath)
{
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(absolutePath, error), end; iterator != end; iterator.increment(error))
    {
        if (error)
            break;

        if (IsPhysicalDirectoryEntry(*iterator))
            return true;
        error.clear();
    }
    return false;
}

bool ShouldEnumerateFolderChildren(
    const std::string& projectRelativePath,
    const AssetBrowserFolderTreeBuildOptions& options)
{
    if (projectRelativePath == "Assets")
        return true;

    const auto normalized = NormalizeAssetBrowserProjectRelativePath(projectRelativePath);
    const auto selected = NormalizeAssetBrowserProjectRelativePath(options.selectedFolder);
    if (selected == normalized ||
        (selected.size() > normalized.size() &&
         selected.compare(0u, normalized.size(), normalized) == 0 &&
         selected[normalized.size()] == '/'))
    {
        return true;
    }

    for (const auto& expanded : options.expandedFolders)
    {
        const auto normalizedExpanded = NormalizeAssetBrowserProjectRelativePath(expanded);
        if (normalizedExpanded == normalized ||
            (normalizedExpanded.size() > normalized.size() &&
             normalizedExpanded.compare(0u, normalized.size(), normalized) == 0 &&
             normalizedExpanded[normalized.size()] == '/'))
        {
            return true;
        }
    }
    return false;
}

AssetBrowserFolderNode BuildFolderNode(
    const std::filesystem::path& assetsRoot,
    const std::filesystem::path& absolutePath,
    const AssetBrowserFolderTreeBuildOptions& options)
{
    AssetBrowserFolderNode node;
    node.displayName = PathFilenameToGenericUtf8String(absolutePath);
    if (node.displayName.empty())
        node.displayName = "Assets";
    node.projectRelativePath = ToProjectRelativePath(assetsRoot, absolutePath);
    node.absolutePath = absolutePath.lexically_normal();

    if (!ShouldEnumerateFolderChildren(node.projectRelativePath, options))
    {
        node.hasChildren = HasPhysicalChildDirectory(absolutePath);
        return node;
    }
    node.childrenEnumerated = true;

    std::vector<FolderDirectoryEntry> directories;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(absolutePath, error), end; iterator != end; iterator.increment(error))
    {
        if (error)
            break;

        if (IsPhysicalDirectoryEntry(*iterator))
        {
            directories.push_back({
                *iterator,
                PathFilenameToGenericUtf8String(iterator->path())
            });
        }
        error.clear();
    }

    std::sort(
        directories.begin(),
        directories.end(),
        [](const FolderDirectoryEntry& left, const FolderDirectoryEntry& right)
        {
            return left.displayName < right.displayName;
        });

    node.hasChildren = !directories.empty();
    node.children.reserve(directories.size());
    for (const auto& directory : directories)
        node.children.push_back(BuildFolderNode(assetsRoot, directory.entry.path(), options));

    return node;
}

bool IsHiddenAssetBrowserFile(const std::filesystem::path& path)
{
    const auto filename = path.filename().generic_string();
    if (filename.empty())
        return true;
    if (filename[0] == '.')
        return true;
    if (path.extension() == ".meta")
        return true;
    return false;
}

std::string Trim(const std::string& value)
{
    const auto begin = std::find_if(
        value.begin(),
        value.end(),
        [](const unsigned char ch)
        {
            return !std::isspace(ch);
        });
    const auto end = std::find_if(
        value.rbegin(),
        value.rend(),
        [](const unsigned char ch)
        {
            return !std::isspace(ch);
        }).base();

    if (begin >= end)
        return {};
    return std::string(begin, end);
}

std::string Lower(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char ch)
        {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

bool MatchesAssetBrowserBuildOptions(
    const AssetBrowserItem& item,
    const AssetBrowserBuildOptions& options)
{
    if (options.typeFilter != AssetBrowserItemType::All &&
        item.type != options.typeFilter)
    {
        return false;
    }

    const auto query = Lower(Trim(options.searchQuery));
    if (query.empty())
        return true;

    return Lower(item.displayName).find(query) != std::string::npos;
}

AssetBrowserItemType TypeFromExtension(const std::filesystem::path& path)
{
    return AssetBrowserItemTypeFromPathParserFileType(
        NLS::Utils::PathParser::GetFileType(path.generic_string()));
}

AssetBrowserItemType TypeFromArtifactType(const NLS::Core::Assets::ArtifactType artifactType)
{
    using NLS::Core::Assets::ArtifactType;
    switch (artifactType)
    {
    case ArtifactType::Model:
        return AssetBrowserItemType::Model;
    case ArtifactType::Prefab:
        return AssetBrowserItemType::Prefab;
    case ArtifactType::Mesh:
        return AssetBrowserItemType::Mesh;
    case ArtifactType::Material:
        return AssetBrowserItemType::Material;
    case ArtifactType::Texture:
        return AssetBrowserItemType::Texture;
    case ArtifactType::Shader:
        return AssetBrowserItemType::Shader;
    case ArtifactType::Scene:
        return AssetBrowserItemType::Scene;
    case ArtifactType::Skeleton:
    case ArtifactType::Skin:
    case ArtifactType::AnimationClip:
    case ArtifactType::MorphTarget:
    case ArtifactType::Audio:
    case ArtifactType::Unknown:
    case ArtifactType::Count:
        return AssetBrowserItemType::Other;
    }
    return AssetBrowserItemType::Other;
}

AssetBrowserItem MakeFolderItem(
    const std::filesystem::path& assetsRoot,
    const std::filesystem::path& absolutePath)
{
    AssetBrowserItem item;
    item.displayName = absolutePath.filename().generic_string();
    item.projectRelativePath = ToProjectRelativePath(assetsRoot, absolutePath);
    item.absolutePath = absolutePath.lexically_normal();
    item.kind = AssetBrowserItemKind::Folder;
    item.type = AssetBrowserItemType::Folder;
    item.dragResourcePath = item.projectRelativePath;
    item.selectionResourcePath = item.projectRelativePath;
    return item;
}

AssetBrowserItem MakeSourceAssetItem(
    const std::filesystem::path& assetsRoot,
    const std::filesystem::path& absolutePath,
    const AssetDatabaseFacade* database,
    const bool loadMetadataWithoutDatabase)
{
    AssetBrowserItem item;
    item.displayName = absolutePath.filename().generic_string();
    item.projectRelativePath = ToProjectRelativePath(assetsRoot, absolutePath);
    item.sourceAssetPath = item.projectRelativePath;
    item.absolutePath = absolutePath.lexically_normal();
    item.kind = AssetBrowserItemKind::SourceAsset;
    item.type = TypeFromExtension(absolutePath);
    item.dragResourcePath = item.sourceAssetPath;
    item.selectionResourcePath = item.sourceAssetPath;
    item.previewableInAssetView =
        item.type == AssetBrowserItemType::Model ||
        item.type == AssetBrowserItemType::Material ||
        item.type == AssetBrowserItemType::Texture;
    item.hasGeneratedSubAssets = AssetTypeCanHaveGeneratedSubAssets(item.type);

    if (database != nullptr)
    {
        if (const auto record = database->LoadMainAssetAtPath(item.sourceAssetPath);
            record.has_value())
        {
            item.assetId = record->assetId;
            item.subAssetKey = record->subAssetKey;
            item.artifactPath = record->artifactPath;
            item.artifactType = record->artifactType;
        }
    }
    if (!item.assetId.IsValid() && loadMetadataWithoutDatabase)
    {
        if (const auto meta = NLS::Core::Assets::AssetMeta::Load(
                NLS::Core::Assets::GetAssetMetaPath(item.absolutePath));
            meta.has_value() && meta->id.IsValid())
        {
            item.assetId = meta->id;
        }
    }
    return item;
}

AssetBrowserItem MakeGeneratedSubAssetItem(
    const AssetDatabaseRecord& record,
    const std::unordered_map<std::string, std::string>& displayNameOverrides,
    const bool fastPath)
{
    AssetBrowserItem item;
    if (const auto overrideName = displayNameOverrides.find(record.subAssetKey);
        overrideName != displayNameOverrides.end() && !overrideName->second.empty())
    {
        item.displayName = overrideName->second;
    }
    else
    {
        std::optional<std::string> artifactName;
        if (!fastPath)
            artifactName = ReadArtifactDisplayName(record);
        item.displayName = artifactName.has_value() && !artifactName->empty()
            ? *artifactName
            : MakeReadableGeneratedSubAssetFallback(
                record,
                MakeSubAssetDisplayName(record.subAssetKey));
    }
    item.projectRelativePath = record.assetPath + "::" + record.subAssetKey;
    item.sourceAssetPath = record.assetPath;
    item.kind = AssetBrowserItemKind::GeneratedSubAsset;
    item.type = TypeFromArtifactType(record.artifactType);
    item.assetId = record.assetId;
    item.subAssetKey = record.subAssetKey;
    item.artifactPath = record.artifactPath;
    item.dragResourcePath = record.assetPath;
    item.selectionResourcePath = record.assetPath + "#" + record.subAssetKey;
    item.artifactType = record.artifactType;
    item.generatedReadOnly = true;
    item.previewableInAssetView = false;
    return item;
}

std::vector<AssetBrowserItem> BuildGeneratedSubAssetItems(
    const AssetBrowserItem& sourceItem,
    const AssetDatabaseFacade* database,
    const bool verifyManifestFreshness,
    const bool fastPath)
{
    std::vector<AssetBrowserItem> generatedItems;
    if (database == nullptr ||
        sourceItem.kind != AssetBrowserItemKind::SourceAsset ||
        sourceItem.sourceAssetPath.empty() ||
        !AssetBrowserSourceAssetCanHaveGeneratedSubAssets(sourceItem.sourceAssetPath))
    {
        return generatedItems;
    }

    const bool manifestCurrent = verifyManifestFreshness
        ? database->IsArtifactManifestCurrentForAssetPath(sourceItem.sourceAssetPath)
        : database->IsArtifactManifestKnownCurrentForAssetPath(sourceItem.sourceAssetPath);
    if (!manifestCurrent)
    {
        return generatedItems;
    }

    const auto records = database->LoadAllAssetsAtPath(sourceItem.sourceAssetPath);
    const auto displayNameOverrides = fastPath
        ? std::unordered_map<std::string, std::string> {}
        : BuildGeneratedSubAssetDisplayNameOverrides(
            records,
            database,
            sourceItem.sourceAssetPath);
    generatedItems.reserve(records.size());
    for (const auto& record : records)
    {
        if (record.subAssetKey.empty() ||
            !IsGridVisibleGeneratedSubAsset(record.artifactType))
        {
            continue;
        }

        generatedItems.push_back(MakeGeneratedSubAssetItem(record, displayNameOverrides, fastPath));
    }

    std::stable_sort(
        generatedItems.begin(),
        generatedItems.end(),
        [](const AssetBrowserItem& left, const AssetBrowserItem& right)
        {
            if (left.artifactType != right.artifactType)
                return left.artifactType < right.artifactType;
            return left.subAssetKey < right.subAssetKey;
        });
    return generatedItems;
}

struct CurrentFolderEntry
{
    std::filesystem::directory_entry entry;
    bool isDirectory = false;
    bool isRegularFile = false;
    std::string displayName;
};

struct AssetBrowserItemTypeDescriptor
{
    AssetBrowserItemType type = AssetBrowserItemType::Other;
    const char* label = "";
    AssetBrowserItemTypeColor color;
};

struct AssetBrowserRefreshReasonDescriptor
{
    AssetBrowserRefreshReason reason = AssetBrowserRefreshReason::InitialBuild;
    AssetBrowserRefreshPlan plan {};
};

constexpr std::array<AssetBrowserItemTypeDescriptor, kAssetBrowserItemTypeCount> kAssetBrowserItemTypeDescriptors {{
    { AssetBrowserItemType::All, "All", { 135u, 142u, 150u, 255u } },
    { AssetBrowserItemType::Folder, "Folder", { 224u, 170u, 72u, 255u } },
    { AssetBrowserItemType::Model, "Model", { 90u, 162u, 214u, 255u } },
    { AssetBrowserItemType::Prefab, "Prefab", { 114u, 175u, 112u, 255u } },
    { AssetBrowserItemType::Mesh, "Mesh", { 90u, 162u, 214u, 255u } },
    { AssetBrowserItemType::Material, "Material", { 196u, 132u, 92u, 255u } },
    { AssetBrowserItemType::Texture, "Texture", { 184u, 112u, 184u, 255u } },
    { AssetBrowserItemType::Shader, "Shader", { 125u, 158u, 190u, 255u } },
    { AssetBrowserItemType::Scene, "Scene", { 100u, 170u, 160u, 255u } },
    { AssetBrowserItemType::Script, "Script", { 170u, 170u, 170u, 255u } },
    { AssetBrowserItemType::Other, "Other", { 135u, 142u, 150u, 255u } }
}};

constexpr bool AssetBrowserItemTypeDescriptorsAreExhaustive()
{
    if (kAssetBrowserItemTypeDescriptors.size() != kAssetBrowserItemTypeCount)
        return false;

    std::array<bool, kAssetBrowserItemTypeCount> seen {};
    for (const auto& descriptor : kAssetBrowserItemTypeDescriptors)
    {
        const auto index = static_cast<size_t>(descriptor.type);
        if (index >= kAssetBrowserItemTypeCount || seen[index])
            return false;
        seen[index] = true;
    }

    for (const bool covered : seen)
    {
        if (!covered)
            return false;
    }
    return true;
}

static_assert(AssetBrowserItemTypeDescriptorsAreExhaustive());

constexpr std::array<AssetBrowserRefreshReasonDescriptor, kAssetBrowserRefreshReasonCount> kAssetBrowserRefreshReasonDescriptors {{
    { AssetBrowserRefreshReason::InitialBuild, { true, true, true } },
    { AssetBrowserRefreshReason::AssetDatabaseMutation, { true, true, true } },
    { AssetBrowserRefreshReason::AssetDatabaseReady, { false, false, true } },
    { AssetBrowserRefreshReason::FolderSelection, { false, true, true } },
    { AssetBrowserRefreshReason::FilterChange, { false, false, false } }
}};

constexpr bool AssetBrowserRefreshReasonDescriptorsAreExhaustive()
{
    if (kAssetBrowserRefreshReasonDescriptors.size() != kAssetBrowserRefreshReasonCount)
        return false;

    std::array<bool, kAssetBrowserRefreshReasonCount> seen {};
    for (const auto& descriptor : kAssetBrowserRefreshReasonDescriptors)
    {
        const auto index = static_cast<size_t>(descriptor.reason);
        if (index >= kAssetBrowserRefreshReasonCount || seen[index])
            return false;
        seen[index] = true;
    }

    for (const bool covered : seen)
    {
        if (!covered)
            return false;
    }
    return true;
}

static_assert(AssetBrowserRefreshReasonDescriptorsAreExhaustive());
}

std::string NormalizeAssetBrowserProjectRelativePath(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');

    std::vector<std::string_view> parts;
    const bool rooted = !path.empty() && path.front() == '/';
    bool sawRegularPart = false;
    bool sawParentPart = false;
    size_t offset = 0u;
    while (offset <= path.size())
    {
        const auto separator = path.find('/', offset);
        const auto end = separator == std::string::npos ? path.size() : separator;
        const auto length = end - offset;
        if (length > 0u)
        {
            const auto part = std::string_view(path).substr(offset, length);
            if (part == "..")
            {
                sawParentPart = true;
                if (!parts.empty() && parts.back() != "..")
                    parts.pop_back();
                else
                    parts.push_back(part);
            }
            else if (part != ".")
            {
                sawRegularPart = true;
                parts.push_back(part);
            }
        }

        if (separator == std::string::npos)
            break;
        offset = separator + 1u;
    }

    if (parts.empty() && rooted)
        return "/";
    if (parts.empty() && sawRegularPart && sawParentPart)
        return "..";
    if (parts.empty())
        return "Assets";

    std::string normalized = rooted ? "/" : "";
    for (const auto part : parts)
    {
        if (!normalized.empty() && normalized.back() != '/')
            normalized += '/';
        normalized.append(part.data(), part.size());
    }

    return normalized.empty() ? "Assets" : normalized;
}

bool ShouldStopDrawingAssetBrowserFolderNodeAfterSelection(
    const std::string& selectedFolder,
    const std::string& clickedFolder)
{
    return NormalizeAssetBrowserProjectRelativePath(selectedFolder) !=
        NormalizeAssetBrowserProjectRelativePath(clickedFolder);
}

bool ShouldStopDrawingAssetBrowserGridAfterOpeningItem(
    const std::string& selectedFolder,
    const AssetBrowserItem& openedItem)
{
    if (openedItem.kind != AssetBrowserItemKind::Folder)
        return false;

    return NormalizeAssetBrowserProjectRelativePath(selectedFolder) !=
        NormalizeAssetBrowserProjectRelativePath(openedItem.projectRelativePath);
}

const char* AssetBrowserItemTypeDisplayLabel(const AssetBrowserItemType type)
{
    const auto index = static_cast<size_t>(type);
    if (index >= kAssetBrowserItemTypeCount)
        return "";
    for (const auto& descriptor : kAssetBrowserItemTypeDescriptors)
    {
        if (descriptor.type == type)
            return descriptor.label;
    }
    return "";
}

AssetBrowserItemTypeColor AssetBrowserItemTypeDisplayColor(const AssetBrowserItemType type)
{
    const auto index = static_cast<size_t>(type);
    if (index >= kAssetBrowserItemTypeCount)
        return kAssetBrowserItemTypeDescriptors.back().color;
    for (const auto& descriptor : kAssetBrowserItemTypeDescriptors)
    {
        if (descriptor.type == type)
            return descriptor.color;
    }
    return kAssetBrowserItemTypeDescriptors.back().color;
}

const char* AssetBrowserFallbackIconId(const AssetBrowserItemType type)
{
    switch (type)
    {
    case AssetBrowserItemType::Folder:
        return "editor.icon.asset.folder";
    case AssetBrowserItemType::Model:
    case AssetBrowserItemType::Mesh:
        return "editor.icon.asset.mesh";
    case AssetBrowserItemType::Prefab:
        return "editor.icon.asset.prefab";
    case AssetBrowserItemType::Material:
        return "editor.icon.asset.material";
    case AssetBrowserItemType::Texture:
        return "editor.icon.asset.texture";
    case AssetBrowserItemType::Shader:
        return "editor.icon.asset.shader";
    case AssetBrowserItemType::Scene:
        return "editor.icon.asset.scene";
    case AssetBrowserItemType::Script:
        return "editor.icon.asset.script";
    case AssetBrowserItemType::All:
    case AssetBrowserItemType::Other:
    case AssetBrowserItemType::Count:
        break;
    }
    return "editor.icon.asset.default";
}

std::string_view ResolveAssetBrowserDisplayFallbackIconId(
    const AssetBrowserItemType type,
    const std::string_view thumbnailFallbackIcon)
{
    constexpr std::string_view defaultAssetIcon = "editor.icon.asset.default";
    if (thumbnailFallbackIcon.empty() || thumbnailFallbackIcon == defaultAssetIcon)
        return AssetBrowserFallbackIconId(type);
    return thumbnailFallbackIcon;
}

AssetBrowserContentViewMode ResolveAssetBrowserContentViewMode(const float thumbnailSize)
{
    return thumbnailSize <= 64.0f
        ? AssetBrowserContentViewMode::List
        : AssetBrowserContentViewMode::Grid;
}

AssetBrowserRect ComputeAssetBrowserThumbnailRect(
    AssetBrowserRect bounds,
    uint32_t imageWidth,
    uint32_t imageHeight)
{
    if (imageWidth == 0u || imageHeight == 0u)
        return bounds;

    const float boundsWidth = bounds.max.x - bounds.min.x;
    const float boundsHeight = bounds.max.y - bounds.min.y;
    if (boundsWidth <= 0.0f || boundsHeight <= 0.0f)
        return bounds;

    const float imageAspect = static_cast<float>(imageWidth) / static_cast<float>(imageHeight);
    const float boundsAspect = boundsWidth / boundsHeight;

    AssetBrowserRect result = bounds;
    if (imageAspect > boundsAspect)
    {
        const float scaledHeight = boundsWidth / imageAspect;
        const float offsetY = (boundsHeight - scaledHeight) * 0.5f;
        result.min.y = bounds.min.y + offsetY;
        result.max.y = result.min.y + scaledHeight;
    }
    else
    {
        const float scaledWidth = boundsHeight * imageAspect;
        const float offsetX = (boundsWidth - scaledWidth) * 0.5f;
        result.min.x = bounds.min.x + offsetX;
        result.max.x = result.min.x + scaledWidth;
    }

    return result;
}

bool ShouldDrawAssetBrowserThumbnailLetterboxBackground(const AssetBrowserItemType type)
{
    return type == AssetBrowserItemType::Texture;
}

std::vector<AssetBrowserDisplayItem> BuildAssetBrowserDisplayItems(
    const std::vector<AssetBrowserItem>& items,
    const std::unordered_set<std::string>& expandedSourceAssets,
    const std::unordered_map<std::string, size_t>& generatedSubAssetCountHints)
{
    NLS_PROFILE_NAMED_SCOPE("AssetBrowserPresentation::BuildDisplayItems");
    struct SubAssetRange
    {
        size_t begin = 0u;
        size_t count = 0u;
    };

    std::unordered_map<std::string, SubAssetRange> subAssetsBySource;
    subAssetsBySource.reserve(items.size());
    std::vector<size_t> subAssetIndices;
    subAssetIndices.reserve(items.size());

    for (size_t index = 0u; index < items.size(); ++index)
    {
        const auto& item = items[index];
        if (item.kind != AssetBrowserItemKind::GeneratedSubAsset)
            continue;

        auto& range = subAssetsBySource[item.sourceAssetPath];
        if (range.count == 0u)
            range.begin = subAssetIndices.size();
        ++range.count;
        subAssetIndices.push_back(index);
    }

    std::vector<AssetBrowserDisplayItem> displayItems;
    displayItems.reserve(items.size());

    for (size_t index = 0u; index < items.size(); ++index)
    {
        const auto& item = items[index];
        if (item.kind == AssetBrowserItemKind::GeneratedSubAsset)
            continue;

        AssetBrowserDisplayItem displayItem;
        displayItem.item = item;
        const auto sourcePath = item.sourceAssetPath.empty()
            ? item.projectRelativePath
            : item.sourceAssetPath;
        const bool expanded = expandedSourceAssets.find(sourcePath) != expandedSourceAssets.end();
        const auto subAssets = subAssetsBySource.find(sourcePath);
        const auto countHint = generatedSubAssetCountHints.find(sourcePath);
        displayItem.expanded = expanded;
        displayItem.childCount = subAssets == subAssetsBySource.end()
            ? (countHint == generatedSubAssetCountHints.end()
                ? (item.hasGeneratedSubAssets ? 1u : 0u)
                : countHint->second)
            : (std::max)(
                subAssets->second.count,
                countHint == generatedSubAssetCountHints.end() ? 0u : countHint->second);

        displayItems.push_back(std::move(displayItem));

        if (!expanded || subAssets == subAssetsBySource.end())
            continue;

        for (size_t childOffset = 0u; childOffset < subAssets->second.count; ++childOffset)
        {
            const auto& candidate = items[subAssetIndices[subAssets->second.begin + childOffset]];

            AssetBrowserDisplayItem child;
            child.item = candidate;
            child.subAsset = true;
            displayItems.push_back(std::move(child));
        }
    }

    return displayItems;
}

std::vector<AssetBrowserDisplayItem> BuildProgressiveAssetBrowserDisplayItems(
    const std::vector<AssetBrowserDisplayItem>& displayItems,
    const std::unordered_map<std::string, size_t>& generatedSubAssetRevealCounts,
    const size_t maxGeneratedSubAssetPlaceholdersPerSource)
{
    NLS_PROFILE_NAMED_SCOPE("AssetBrowserPresentation::BuildProgressiveDisplayItems");
    std::vector<AssetBrowserDisplayItem> progressiveItems;
    progressiveItems.reserve(displayItems.size());
    std::string currentExpandedSourcePath;
    size_t currentChildCount = 0u;
    size_t currentRevealCount = 0u;
    size_t emittedChildCount = 0u;

    auto appendMissingChildPlaceholders = [&]()
    {
        if (currentExpandedSourcePath.empty() || currentChildCount <= emittedChildCount)
            return;
        const auto placeholderCount = (std::min)(
            currentChildCount - emittedChildCount,
            maxGeneratedSubAssetPlaceholdersPerSource);
        for (size_t placeholderIndex = 0u; placeholderIndex < placeholderCount; ++placeholderIndex)
        {
            AssetBrowserDisplayItem placeholder;
            placeholder.subAsset = true;
            placeholder.loadingPlaceholder = true;
            progressiveItems.push_back(std::move(placeholder));
        }
    };

    for (const auto& displayItem : displayItems)
    {
        if (!displayItem.subAsset)
        {
            appendMissingChildPlaceholders();
            progressiveItems.push_back(displayItem);
            currentExpandedSourcePath.clear();
            currentChildCount = 0u;
            currentRevealCount = 0u;
            emittedChildCount = 0u;
            if (displayItem.expanded && displayItem.childCount > 0u)
            {
                currentExpandedSourcePath = displayItem.item.sourceAssetPath.empty()
                    ? displayItem.item.projectRelativePath
                    : displayItem.item.sourceAssetPath;
                currentChildCount = displayItem.childCount;
                if (const auto found = generatedSubAssetRevealCounts.find(currentExpandedSourcePath);
                    found != generatedSubAssetRevealCounts.end())
                {
                    currentRevealCount = (std::min)(found->second, currentChildCount);
                }
            }
            continue;
        }

        if (currentExpandedSourcePath.empty())
            continue;
        if (emittedChildCount >= currentRevealCount)
            continue;

        progressiveItems.push_back(displayItem);
        ++emittedChildCount;
    }

    appendMissingChildPlaceholders();

    return progressiveItems;
}

std::optional<AssetBrowserDisplayItemRange> ResolveAssetBrowserExpandedSubAssetRange(
    const std::vector<AssetBrowserDisplayItem>& displayItems,
    const size_t sourceDisplayIndex)
{
    if (sourceDisplayIndex >= displayItems.size())
        return std::nullopt;

    const auto& sourceItem = displayItems[sourceDisplayIndex];
    if (sourceItem.subAsset || !sourceItem.expanded || sourceItem.childCount == 0u)
        return std::nullopt;

    AssetBrowserDisplayItemRange range;
    range.begin = sourceDisplayIndex + 1u;
    while (range.begin + range.count < displayItems.size() &&
        displayItems[range.begin + range.count].subAsset)
    {
        ++range.count;
    }

    if (range.count == 0u)
        return std::nullopt;
    return range;
}

AssetBrowserRefreshPlan BuildAssetBrowserRefreshPlan(const AssetBrowserRefreshReason reason)
{
    const auto index = static_cast<size_t>(reason);
    if (index >= kAssetBrowserRefreshReasonCount)
        return {};

    for (const auto& descriptor : kAssetBrowserRefreshReasonDescriptors)
    {
        if (descriptor.reason == reason)
            return descriptor.plan;
    }
    return {};
}

void EnqueueAssetBrowserExternalDroppedFiles(
    AssetBrowserExternalDroppedFileQueue& queue,
    AssetBrowserExternalDroppedFileBatch paths)
{
    if (!paths.empty())
        queue.push_back(std::move(paths));
}

std::optional<AssetBrowserExternalDroppedFileBatch> ConsumeAssetBrowserExternalDroppedFiles(
    AssetBrowserExternalDroppedFileQueue& queue,
    const bool currentFolderHovered)
{
    if (!currentFolderHovered || queue.empty())
        return std::nullopt;

    auto paths = std::move(queue.front());
    queue.pop_front();
    return paths;
}

AssetDatabaseRefreshSchedulingDecision PlanAssetDatabaseRefreshScheduling(
    const bool projectRootEmpty,
    const bool refreshRequested,
    const bool databaseReady,
    const bool refreshInFlightForSameRoot)
{
    AssetDatabaseRefreshSchedulingDecision decision;
    if (projectRootEmpty || (!refreshRequested && databaseReady))
        return decision;

    if (refreshInFlightForSameRoot)
    {
        decision.queueRefreshAfterInFlight = refreshRequested;
        return decision;
    }

    decision.startRefresh = true;
    return decision;
}

AssetDatabaseRefreshDiscardAction PlanAssetDatabaseRefreshDiscardAction(
    const bool futureValid,
    const bool futureReady)
{
    return futureValid && !futureReady
        ? AssetDatabaseRefreshDiscardAction::Retire
        : AssetDatabaseRefreshDiscardAction::Drop;
}

AssetBrowserWorkflowCapabilities BuildAssetBrowserWorkflowCapabilities(
    const AssetBrowserItem& item)
{
    AssetBrowserWorkflowCapabilities capabilities;
    if (item.kind == AssetBrowserItemKind::Folder)
    {
        capabilities.canShowInExplorer = !item.absolutePath.empty();
        capabilities.canImportHere = !item.absolutePath.empty();
        capabilities.canCreateChildren = !item.absolutePath.empty();
        capabilities.canRename = !item.absolutePath.empty();
        capabilities.canDelete = !item.absolutePath.empty();
        capabilities.canAcceptAssetDrops = true;
        capabilities.canAcceptHierarchyDrops = true;
        return capabilities;
    }

    if (item.kind != AssetBrowserItemKind::SourceAsset)
    {
        capabilities.canOpenProperties = !item.selectionResourcePath.empty();
        capabilities.canPreview = item.previewableInAssetView;
        capabilities.canEdit = item.type == AssetBrowserItemType::Prefab ||
            item.type == AssetBrowserItemType::Scene;
        return capabilities;
    }

    const bool hasPhysicalFile = !item.absolutePath.empty();
    capabilities.canOpenExternal = hasPhysicalFile;
    capabilities.canRename = hasPhysicalFile;
    capabilities.canDelete = hasPhysicalFile;
    capabilities.canDuplicate = hasPhysicalFile;
    capabilities.canOpenProperties = !item.selectionResourcePath.empty();
    capabilities.canPreview = item.previewableInAssetView;
    capabilities.canReimport = hasPhysicalFile && item.type == AssetBrowserItemType::Model;
    capabilities.canReload = hasPhysicalFile &&
        (item.type == AssetBrowserItemType::Texture ||
         item.type == AssetBrowserItemType::Material);
    capabilities.canCompile = hasPhysicalFile && item.type == AssetBrowserItemType::Shader;
    capabilities.canEdit =
        item.type == AssetBrowserItemType::Prefab ||
        item.type == AssetBrowserItemType::Scene ||
        item.type == AssetBrowserItemType::Material;
    return capabilities;
}

AssetBrowserItemType AssetBrowserItemTypeFromPathParserFileType(
    const NLS::Utils::PathParser::EFileType fileType)
{
    switch (fileType)
    {
    case NLS::Utils::PathParser::EFileType::MODEL:
        return AssetBrowserItemType::Model;
    case NLS::Utils::PathParser::EFileType::TEXTURE:
        return AssetBrowserItemType::Texture;
    case NLS::Utils::PathParser::EFileType::SHADER:
        return AssetBrowserItemType::Shader;
    case NLS::Utils::PathParser::EFileType::MATERIAL:
        return AssetBrowserItemType::Material;
    case NLS::Utils::PathParser::EFileType::SCENE:
        return AssetBrowserItemType::Scene;
    case NLS::Utils::PathParser::EFileType::PREFAB:
        return AssetBrowserItemType::Prefab;
    case NLS::Utils::PathParser::EFileType::SCRIPT:
        return AssetBrowserItemType::Script;
    case NLS::Utils::PathParser::EFileType::SOUND:
    case NLS::Utils::PathParser::EFileType::FONT:
    case NLS::Utils::PathParser::EFileType::UNKNOWN:
    case NLS::Utils::PathParser::EFileType::COUNT:
        return AssetBrowserItemType::Other;
    }
    return AssetBrowserItemType::Other;
}

bool AssetBrowserSourceAssetCanHaveGeneratedSubAssets(const std::string& sourceAssetPath)
{
    return AssetTypeCanHaveGeneratedSubAssets(
        AssetBrowserItemTypeFromPathParserFileType(
            NLS::Utils::PathParser::GetFileType(sourceAssetPath)));
}

const std::array<AssetBrowserItemType, kAssetBrowserItemTypeCount>& AssetBrowserItemTypeFilterOptions()
{
    static const std::array<AssetBrowserItemType, kAssetBrowserItemTypeCount> options = []
    {
        std::array<AssetBrowserItemType, kAssetBrowserItemTypeCount> values {};
        for (size_t index = 0u; index < kAssetBrowserItemTypeDescriptors.size(); ++index)
            values[index] = kAssetBrowserItemTypeDescriptors[index].type;
        return values;
    }();
    return options;
}

std::string BuildAssetBrowserThumbnailGenerationScopeKey(
    const std::string& selectedFolder,
    const uint32_t requestedSize,
    const std::vector<AssetBrowserItem>& visibleItems)
{
    auto appendPart = [](std::ostringstream& stream, const std::string& value)
    {
        stream << value.size() << ':' << value << '|';
    };

    std::ostringstream stream;
    stream << "folder=";
    appendPart(stream, NormalizeAssetBrowserProjectRelativePath(selectedFolder));
    stream << "size=" << requestedSize << '|';
    stream << "count=" << visibleItems.size() << '|';
    for (const auto& item : visibleItems)
    {
        stream << "kind=" << static_cast<int>(item.kind) << '|';
        stream << "type=" << static_cast<int>(item.type) << '|';
        stream << "artifact=" << static_cast<int>(item.artifactType) << '|';
        appendPart(stream, item.projectRelativePath);
        appendPart(stream, item.sourceAssetPath);
        appendPart(stream, item.subAssetKey);
        appendPart(stream, item.assetId.ToString());
    }
    return stream.str();
}

std::string BuildAssetBrowserThumbnailItemKey(
    const AssetBrowserItem& item,
    const uint32_t requestedSize)
{
    auto appendIdentity = [&item, requestedSize](std::string& key)
    {
        key += "#";
        key += std::to_string(requestedSize);
        key += "|kind=";
        key += std::to_string(static_cast<int>(item.kind));
        key += "|type=";
        key += std::to_string(static_cast<int>(item.type));
        key += "|artifact=";
        key += std::to_string(static_cast<int>(item.artifactType));
    };

    if (item.kind == AssetBrowserItemKind::GeneratedSubAsset && !item.subAssetKey.empty())
    {
        std::string key = item.sourceAssetPath.empty()
            ? item.projectRelativePath
            : item.sourceAssetPath;
        key += "::";
        key += item.subAssetKey;
        appendIdentity(key);
        return key;
    }
    std::string key = item.projectRelativePath;
    appendIdentity(key);
    return key;
}

AssetBrowserThumbnailGenerationScopeDecision EvaluateAssetBrowserThumbnailGenerationScope(
    const std::string& previousScopeKey,
    const uint32_t previousRequestedSize,
    const bool scopeDirty,
    const std::string& nextScopeKey,
    const uint32_t nextRequestedSize)
{
    AssetBrowserThumbnailGenerationScopeDecision decision;
    decision.scopeChanged =
        previousScopeKey != nextScopeKey ||
        previousRequestedSize != nextRequestedSize;
    decision.canSkip =
        !scopeDirty &&
        previousRequestedSize == nextRequestedSize &&
        !decision.scopeChanged;
    decision.requerySameScope = scopeDirty && !decision.scopeChanged;
    return decision;
}

AssetBrowserHeavyGpuThumbnailPumpDecision PlanAssetBrowserHeavyGpuThumbnailPump(
    const AssetBrowserHeavyGpuThumbnailPumpInput& input)
{
    AssetBrowserHeavyGpuThumbnailPumpDecision decision;
    if (!input.allowHeavyGpuPreview ||
        !input.hasQueuedWork ||
        input.hasInFlightWork ||
        !input.hasPreviewRenderer ||
        input.interactive)
    {
        return decision;
    }

    decision.shouldPump =
        input.nowSeconds >= input.deferredUntilSeconds &&
        input.nowSeconds >= input.nextAllowedSeconds;
    return decision;
}

AssetBrowserLightGpuThumbnailPumpDecision PlanAssetBrowserLightGpuThumbnailPump(
    const AssetBrowserLightGpuThumbnailPumpInput& input)
{
    AssetBrowserLightGpuThumbnailPumpDecision decision;
    if (!input.allowGpuPreviewStart ||
        !input.hasQueuedWork ||
        !input.hasPreviewRenderer ||
        input.interactive)
    {
        return decision;
    }

    decision.shouldPump = input.nowSeconds >= input.nextAllowedSeconds;
    return decision;
}

AssetBrowserThumbnailPumpDecision PlanAssetBrowserThumbnailPump(
    const AssetBrowserThumbnailPumpInput& input)
{
    AssetBrowserThumbnailPumpDecision decision;
    if (!input.hasQueuedWork)
        return decision;

    if (!input.interactive)
    {
        decision.shouldStartBackgroundWork = true;
        return decision;
    }

    decision.shouldStartBackgroundWork =
        !input.hasInFlightWork &&
        input.interactiveStartsThisFrame < input.maxInteractiveStartsPerFrame;
    return decision;
}

AssetBrowserCachedThumbnailTexturePumpDecision PlanAssetBrowserCachedThumbnailTexturePump(
    const AssetBrowserCachedThumbnailTexturePumpInput& input)
{
    AssetBrowserCachedThumbnailTexturePumpDecision decision;
    if (input.queuedTextureLoads == 0u && input.inFlightDecodes == 0u)
        return decision;

    if (!input.interactive)
    {
        decision.shouldPump = true;
        return decision;
    }

    decision.shouldPump = input.interactiveStartsThisFrame < input.maxInteractiveStartsPerFrame;
    return decision;
}

AssetBrowserFolderNode BuildProjectAssetFolderTree(const std::filesystem::path& projectRootOrAssetsRoot)
{
    return BuildProjectAssetFolderTree(projectRootOrAssetsRoot, {});
}

AssetBrowserFolderNode BuildProjectAssetFolderTree(
    const std::filesystem::path& projectRootOrAssetsRoot,
    const AssetBrowserFolderTreeBuildOptions& options)
{
    NLS_PROFILE_NAMED_SCOPE("AssetBrowserPresentation::BuildProjectAssetFolderTree");
    const auto assetsRoot = ResolveAssetsRoot(projectRootOrAssetsRoot);
    std::error_code error;
    if (!std::filesystem::is_directory(assetsRoot, error) ||
        !IsUsableAssetsRoot(assetsRoot))
    {
        return {
            "Assets",
            "Assets",
            assetsRoot.lexically_normal(),
            {}
        };
    }

    auto root = BuildFolderNode(assetsRoot, assetsRoot, options);
    root.displayName = "Assets";
    root.projectRelativePath = "Assets";
    return root;
}

std::vector<AssetBrowserItem> BuildCurrentFolderAssetItems(
    const std::filesystem::path& projectRootOrAssetsRoot,
    const std::string& selectedFolder,
    const AssetDatabaseFacade* database)
{
    return BuildCurrentFolderAssetItems(
        projectRootOrAssetsRoot,
        selectedFolder,
        database,
        {});
}

std::vector<AssetBrowserItem> BuildCurrentFolderAssetItems(
    const std::filesystem::path& projectRootOrAssetsRoot,
    const std::string& selectedFolder,
    const AssetDatabaseFacade* database,
    const AssetBrowserBuildOptions options)
{
    NLS_PROFILE_NAMED_SCOPE("AssetBrowserPresentation::BuildCurrentFolderAssetItems");
    const auto assetsRoot = ResolveAssetsRoot(projectRootOrAssetsRoot);
    const auto selectedPath = AbsolutePathForProjectRelative(assetsRoot, selectedFolder);
    std::vector<AssetBrowserItem> items;
    if (selectedPath.empty())
        return items;

    if (!IsUsableAssetsRoot(assetsRoot) ||
        !IsPhysicalDirectoryPath(selectedPath) ||
        !IsPhysicalPathInsideAssetsRoot(selectedPath, assetsRoot))
    {
        return items;
    }

    std::vector<CurrentFolderEntry> entries;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(selectedPath, error), end; iterator != end; iterator.increment(error))
    {
        if (error)
            break;

        const bool isDirectory = IsPhysicalDirectoryEntry(*iterator);
        const bool isRegularFile = !isDirectory && IsPhysicalRegularFileEntry(*iterator);
        if (isDirectory || isRegularFile)
        {
            entries.push_back({
                *iterator,
                isDirectory,
                isRegularFile,
                iterator->path().filename().generic_string()
            });
        }
        error.clear();
    }

    std::sort(
        entries.begin(),
        entries.end(),
        [](const CurrentFolderEntry& left, const CurrentFolderEntry& right)
        {
            if (left.isDirectory != right.isDirectory)
                return left.isDirectory;
            return left.displayName < right.displayName;
        });

    for (const auto& entry : entries)
    {
        error.clear();
        if (entry.isDirectory)
        {
            auto folderItem = MakeFolderItem(assetsRoot, entry.entry.path());
            items.push_back(std::move(folderItem));
            continue;
        }

        error.clear();
        if (!entry.isRegularFile || IsHiddenAssetBrowserFile(entry.entry.path()))
            continue;

        auto sourceItem = MakeSourceAssetItem(
            assetsRoot,
            entry.entry.path(),
            database,
            options.loadSourceAssetMetadataWithoutDatabase);
        items.push_back(sourceItem);
        if (options.includeGeneratedSubAssets &&
            options.expandedSourceAssets.find(sourceItem.sourceAssetPath) != options.expandedSourceAssets.end())
        {
            auto generatedItems = BuildGeneratedSubAssetItems(
                sourceItem,
                database,
                options.verifyGeneratedSubAssetManifests,
                false);
            items.insert(
                items.end(),
                std::make_move_iterator(generatedItems.begin()),
                std::make_move_iterator(generatedItems.end()));
        }
    }

    return FilterAssetBrowserItems(items, options);
}

std::vector<AssetBrowserItem> FilterAssetBrowserItems(
    const std::vector<AssetBrowserItem>& items,
    const AssetBrowserBuildOptions& options)
{
    std::vector<AssetBrowserItem> filtered;
    filtered.reserve(items.size());
    for (const auto& item : items)
    {
        if (!options.includeGeneratedSubAssets &&
            item.kind == AssetBrowserItemKind::GeneratedSubAsset)
        {
            continue;
        }

        if (MatchesAssetBrowserBuildOptions(item, options))
            filtered.push_back(item);
    }
    return filtered;
}

std::vector<std::string> SelectAssetBrowserThumbnailTextureEvictionCandidates(
    const std::vector<std::string>& lruKeys,
    const std::unordered_set<std::string>& usedThisFrame,
    const size_t maxResidentTextures)
{
    std::vector<std::string> evictions;
    if (lruKeys.size() <= maxResidentTextures)
        return evictions;

    size_t residentAfterEviction = lruKeys.size();
    for (const auto& key : lruKeys)
    {
        if (residentAfterEviction <= maxResidentTextures)
            break;
        if (usedThisFrame.find(key) != usedThisFrame.end())
            continue;

        evictions.push_back(key);
        --residentAfterEviction;
    }
    return evictions;
}

std::vector<std::string> SelectAssetBrowserThumbnailTextureLoadCandidates(
    const std::vector<std::string>& queuedKeys,
    const std::unordered_set<std::string>& residentKeys,
    const size_t maxLoads)
{
    std::vector<std::string> candidates;
    if (maxLoads == 0u)
        return candidates;

    std::unordered_set<std::string> selected;
    for (const auto& key : queuedKeys)
    {
        if (key.empty() ||
            residentKeys.find(key) != residentKeys.end() ||
            selected.find(key) != selected.end())
        {
            continue;
        }

        candidates.push_back(key);
        selected.insert(key);
        if (candidates.size() >= maxLoads)
            break;
    }
    return candidates;
}

std::vector<std::string> SelectAssetBrowserThumbnailTextureDecodeCandidates(
    const std::vector<std::string>& queuedKeys,
    const std::unordered_set<std::string>& residentKeys,
    const std::unordered_set<std::string>& decodingKeys,
    const size_t maxDecodes)
{
    std::vector<std::string> candidates;
    if (maxDecodes == 0u)
        return candidates;

    std::unordered_set<std::string> selected;
    for (const auto& key : queuedKeys)
    {
        if (key.empty() ||
            residentKeys.find(key) != residentKeys.end() ||
            decodingKeys.find(key) != decodingKeys.end() ||
            selected.find(key) != selected.end())
        {
            continue;
        }

        candidates.push_back(key);
        selected.insert(key);
        if (candidates.size() >= maxDecodes)
            break;
    }
    return candidates;
}

size_t AssetBrowserThumbnailTextureDecodeStartBudget(
    const size_t inFlightDecodes,
    const size_t maxInFlightDecodes)
{
    return inFlightDecodes >= maxInFlightDecodes
        ? 0u
        : maxInFlightDecodes - inFlightDecodes;
}

bool IsAssetBrowserCachedThumbnailTextureSizeAllowed(
    const uint32_t width,
    const uint32_t height,
    const uint32_t maxDimension)
{
    return width > 0u &&
        height > 0u &&
        maxDimension > 0u &&
        width <= maxDimension &&
        height <= maxDimension;
}

AssetBrowserThumbnailTextureFrameReleasePlan BeginAssetBrowserThumbnailTextureFrame(
    std::unordered_set<std::string> usedThisFrame,
    std::unordered_set<std::string> pendingRelease)
{
    (void)usedThisFrame;
    AssetBrowserThumbnailTextureFrameReleasePlan plan;
    plan.releaseNow.assign(pendingRelease.begin(), pendingRelease.end());
    std::sort(plan.releaseNow.begin(), plan.releaseNow.end());
    return plan;
}

AssetBrowserThumbnailTextureFrameReleasePlan PlanAssetBrowserThumbnailTextureFullClear(
    const std::vector<std::string>& residentKeys,
    const std::unordered_set<std::string>& usedThisFrame,
    const std::unordered_set<std::string>& pendingRelease)
{
    AssetBrowserThumbnailTextureFrameReleasePlan plan;
    plan.usedThisFrame = usedThisFrame;
    plan.pendingRelease = pendingRelease;
    for (const auto& key : residentKeys)
    {
        if (key.empty())
            continue;
        if (usedThisFrame.find(key) != usedThisFrame.end())
        {
            plan.pendingRelease.insert(key);
            continue;
        }
        plan.releaseNow.push_back(key);
    }
    std::sort(plan.releaseNow.begin(), plan.releaseNow.end());
    return plan;
}

std::vector<AssetBrowserItem> SelectAssetBrowserThumbnailGenerationItems(
    const std::vector<AssetBrowserItem>& currentFolderItems,
    const std::vector<AssetBrowserItem>& visibleItems,
    const bool visibleItemsKnown)
{
    (void)currentFolderItems;
    if (!visibleItemsKnown)
        return {};

    std::vector<AssetBrowserItem> items;
    items.reserve(visibleItems.size());
    std::unordered_set<std::string> selectedKeys;
    auto addItem = [&items, &selectedKeys](const AssetBrowserItem& item)
    {
        if (item.kind == AssetBrowserItemKind::Folder || item.projectRelativePath.empty())
            return;
        if (selectedKeys.insert(item.projectRelativePath).second)
            items.push_back(item);
    };

    for (const auto& visible : visibleItems)
        addItem(visible);
    return items;
}

std::vector<AssetBrowserBreadcrumbSegment> BuildAssetBrowserBreadcrumb(const std::string& selectedFolder)
{
    std::vector<AssetBrowserBreadcrumbSegment> segments;
    const auto normalized = NormalizeAssetBrowserProjectRelativePath(selectedFolder);
    std::string current;
    size_t offset = 0u;
    while (offset <= normalized.size())
    {
        const auto separator = normalized.find('/', offset);
        const auto end = separator == std::string::npos ? normalized.size() : separator;
        const auto length = end - offset;
        if (length > 0u)
        {
            const auto part = std::string_view(normalized).substr(offset, length);
            if (!current.empty())
                current += '/';
            current.append(part.data(), part.size());
            segments.push_back({
                std::string(part),
                NormalizeAssetBrowserProjectRelativePath(current)
            });
        }

        if (separator == std::string::npos)
            break;
        offset = separator + 1u;
    }

    if (segments.empty())
        segments.push_back({"Assets", "Assets"});
    return segments;
}

AssetBrowserFolderSelection ResolveAssetBrowserFolderSelection(
    const std::filesystem::path& projectRootOrAssetsRoot,
    const std::string& requestedFolder)
{
    const auto assetsRoot = ResolveAssetsRoot(projectRootOrAssetsRoot);
    if (!IsUsableAssetsRoot(assetsRoot))
    {
        return {
            "Assets",
            assetsRoot.lexically_normal(),
            false
        };
    }

    auto requested = AbsolutePathForProjectRelative(assetsRoot, requestedFolder);
    if (requested.empty())
        requested = assetsRoot;

    while (true)
    {
        std::error_code error;
        if (IsPhysicalDirectoryPath(requested) &&
            IsPhysicalPathInsideAssetsRoot(requested, assetsRoot))
        {
            return {
                ToProjectRelativePath(assetsRoot, requested),
                requested.lexically_normal(),
                true
            };
        }

        if (requested == assetsRoot || requested.empty())
            break;

        const auto parent = requested.parent_path();
        if (parent == requested)
            break;
        requested = parent;
    }

    return {
        "Assets",
        assetsRoot.lexically_normal(),
        IsPhysicalDirectoryPath(assetsRoot)
    };
}

bool CanMovePhysicalProjectAssetFileIntoFolder(
    const std::filesystem::path& assetsRoot,
    const std::filesystem::path& sourceFile,
    const std::filesystem::path& targetFolder)
{
    const auto normalizedAssetsRoot = NLS::Core::Assets::NormalizeAssetPath(assetsRoot);
    const auto normalizedSource = NLS::Core::Assets::NormalizeAssetPath(sourceFile);
    const auto normalizedTarget = NLS::Core::Assets::NormalizeAssetPath(targetFolder);
    return !normalizedAssetsRoot.empty() &&
        IsUsableAssetsRoot(normalizedAssetsRoot) &&
        IsPhysicalRegularFilePath(normalizedSource) &&
        IsPhysicalDirectoryPath(normalizedTarget) &&
        IsPhysicalPathInsideAssetsRoot(normalizedSource, normalizedAssetsRoot) &&
        IsPhysicalPathInsideAssetsRoot(normalizedTarget, normalizedAssetsRoot);
}

bool CanMovePhysicalProjectAssetFolderIntoFolder(
    const std::filesystem::path& assetsRoot,
    const std::filesystem::path& sourceFolder,
    const std::filesystem::path& targetFolder)
{
    const auto normalizedAssetsRoot = NLS::Core::Assets::NormalizeAssetPath(assetsRoot);
    const auto normalizedSource = NLS::Core::Assets::NormalizeAssetPath(sourceFolder);
    const auto normalizedTarget = NLS::Core::Assets::NormalizeAssetPath(targetFolder);
    if (normalizedAssetsRoot.empty() ||
        !IsUsableAssetsRoot(normalizedAssetsRoot) ||
        !IsPhysicalDirectoryPath(normalizedSource) ||
        !IsPhysicalDirectoryPath(normalizedTarget) ||
        !IsPhysicalPathInsideAssetsRoot(normalizedSource, normalizedAssetsRoot) ||
        !IsPhysicalPathInsideAssetsRoot(normalizedTarget, normalizedAssetsRoot))
    {
        return false;
    }

    return normalizedSource != normalizedTarget &&
        !IsPathInsideEditorAssetRoot(normalizedTarget, normalizedSource);
}

bool CanMoveProjectBrowserResourcePathIntoFolder(
    const std::filesystem::path& assetsRoot,
    const std::string& sourceResourcePath,
    const std::filesystem::path& targetFolder,
    const bool sourceIsFolder)
{
    if (sourceResourcePath.empty() || sourceResourcePath.front() == ':')
        return false;

    const auto normalizedResourcePath = NormalizeAssetBrowserProjectRelativePath(sourceResourcePath);
    if (normalizedResourcePath != "Assets" &&
        normalizedResourcePath.compare(0u, 7u, "Assets/") != 0)
    {
        return false;
    }

    const auto source = AbsolutePathForProjectRelative(assetsRoot, normalizedResourcePath);
    if (source.empty())
        return false;

    return sourceIsFolder
        ? CanMovePhysicalProjectAssetFolderIntoFolder(assetsRoot, source, targetFolder)
        : CanMovePhysicalProjectAssetFileIntoFolder(assetsRoot, source, targetFolder);
}

std::optional<EditorAssetDragPayload> MakeAssetBrowserItemDragPayload(
    const AssetBrowserItem& item,
    const AssetDatabaseFacade* database)
{
    if (item.kind == AssetBrowserItemKind::Folder ||
        item.dragResourcePath.empty() ||
        !item.assetId.IsValid())
    {
        return std::nullopt;
    }

    if (database != nullptr &&
        item.kind == AssetBrowserItemKind::GeneratedSubAsset &&
        !database->IsArtifactManifestCurrentForAssetPath(item.dragResourcePath))
    {
        return std::nullopt;
    }

    auto subAssetKey = item.subAssetKey;
    auto artifactType = item.artifactType;
    if (item.kind == AssetBrowserItemKind::SourceAsset)
    {
        if (subAssetKey.empty())
            subAssetKey = DefaultDragSubAssetKeyForSourceAsset(item.type, item.dragResourcePath);
        if (artifactType == NLS::Core::Assets::ArtifactType::Unknown)
            artifactType = DefaultDragArtifactTypeForSourceAssetType(item.type);
    }
    if (subAssetKey.empty() || artifactType == NLS::Core::Assets::ArtifactType::Unknown)
        return std::nullopt;
    if (!CanStoreEditorAssetDragPayload(item.dragResourcePath, item.assetId, subAssetKey))
        return std::nullopt;

    const bool generatedModelPrefab =
        item.kind == AssetBrowserItemKind::GeneratedSubAsset &&
        artifactType == NLS::Core::Assets::ArtifactType::Prefab;
    const bool imported = item.kind == AssetBrowserItemKind::GeneratedSubAsset ||
        !item.subAssetKey.empty();
    const bool previewPrefabReady = generatedModelPrefab;
    return MakeEditorAssetDragPayload(
        item.dragResourcePath,
        item.assetId,
        subAssetKey,
        artifactType,
        generatedModelPrefab,
        imported,
        previewPrefabReady,
        item.kind == AssetBrowserItemKind::GeneratedSubAsset);
}

void SetObjectReferencePickerAssetRoots(std::vector<EditorAssetRoot> roots)
{
    g_objectReferencePickerAssetRoots = std::move(roots);
}

std::vector<EditorAssetRoot> GetObjectReferencePickerAssetRoots()
{
    return g_objectReferencePickerAssetRoots;
}

void SetObjectReferencePickerEntries(std::vector<ObjectReferencePickerEntry> entries)
{
    g_objectReferencePickerEntries = std::move(entries);
    g_objectReferencePickerEntriesDirty = false;
}

void SetObjectReferencePickerEntriesProvider(ObjectReferencePickerEntriesProvider provider)
{
    g_objectReferencePickerEntriesProvider = std::move(provider);
    g_objectReferencePickerEntriesDirty = static_cast<bool>(g_objectReferencePickerEntriesProvider);
}

void MarkObjectReferencePickerEntriesDirty()
{
    g_objectReferencePickerEntriesDirty = true;
    g_objectReferencePickerEntries.clear();
}

bool ShouldDeferObjectReferencePickerEntriesRefresh()
{
    return g_objectReferencePickerEntriesDirty &&
        static_cast<bool>(g_objectReferencePickerEntriesProvider);
}

bool RefreshObjectReferencePickerEntries()
{
    if (!g_objectReferencePickerEntriesDirty)
        return true;

    if (g_objectReferencePickerEntriesProvider)
    {
        g_objectReferencePickerEntries = g_objectReferencePickerEntriesProvider();
        g_objectReferencePickerEntriesDirty = false;
        return true;
    }
    return false;
}

std::vector<ObjectReferencePickerEntry> GetObjectReferencePickerEntries()
{
    return g_objectReferencePickerEntries;
}

std::vector<AssetBrowserSubAssetEntry> BuildAssetBrowserSubAssetEntries(
    const AssetDatabaseFacade& database,
    const std::string& sourceAssetPath)
{
    std::vector<AssetBrowserSubAssetEntry> entries;
    if (!database.IsArtifactManifestCurrentForAssetPath(sourceAssetPath))
        return entries;

    const auto records = database.LoadAllAssetsAtPath(sourceAssetPath);
    for (const auto& record : records)
    {
        if (!IsInspectorReferenceableSubAsset(record.artifactType) || record.subAssetKey.empty())
            continue;

        const auto displayName = ReadArtifactDisplayName(record);
        entries.push_back({
            displayName.has_value() && !displayName->empty()
                ? *displayName
                : MakeSubAssetDisplayName(record.subAssetKey),
            record.assetPath,
            record.subAssetKey,
            record.assetPath,
            record.assetId,
            record.artifactType,
            true
        });
    }

    std::stable_sort(
        entries.begin(),
        entries.end(),
        [](const AssetBrowserSubAssetEntry& p_left, const AssetBrowserSubAssetEntry& p_right)
        {
            if (p_left.artifactType != p_right.artifactType)
                return p_left.artifactType < p_right.artifactType;
            return p_left.subAssetKey < p_right.subAssetKey;
        });
    return entries;
}

std::vector<ObjectReferencePickerEntry> BuildObjectReferencePickerEntriesFromSnapshots(
    const std::vector<ObjectReferencePickerAssetSnapshot>& snapshots)
{
    std::vector<ObjectReferencePickerEntry> entries;
    auto appendSnapshotEntries = [&entries](const ObjectReferencePickerAssetSnapshot& snapshot)
    {
        for (const auto& subAsset : snapshot.subAssets)
        {
            if (!IsInspectorReferenceableSubAsset(subAsset.artifactType) ||
                subAsset.subAssetKey.empty())
            {
                continue;
            }

            if (!CanStoreEditorAssetDragPayload(
                    snapshot.sourceAssetPath,
                    snapshot.assetId,
                    subAsset.subAssetKey))
            {
                continue;
            }

            entries.push_back({
                snapshot.sourceAssetPath + " / " +
                (!subAsset.displayName.empty() ? subAsset.displayName : subAsset.subAssetKey),
                MakeEditorAssetDragPayload(
                    snapshot.sourceAssetPath,
                    snapshot.assetId,
                    subAsset.subAssetKey,
                    subAsset.artifactType,
                    false,
                    true,
                    false,
                    true)
            });
        }
    };

    for (const auto& snapshot : snapshots)
        appendSnapshotEntries(snapshot);

    std::stable_sort(
        entries.begin(),
        entries.end(),
        [](const ObjectReferencePickerEntry& p_left, const ObjectReferencePickerEntry& p_right)
        {
            return p_left.displayName < p_right.displayName;
        });
    entries.erase(
        std::unique(
            entries.begin(),
            entries.end(),
            [](const ObjectReferencePickerEntry& p_left, const ObjectReferencePickerEntry& p_right)
            {
                return p_left.displayName == p_right.displayName;
            }),
        entries.end());
    return entries;
}

std::vector<ObjectReferencePickerEntry> BuildObjectReferencePickerEntries(
    const AssetDatabaseFacade& database)
{
    std::vector<ObjectReferencePickerEntry> entries;
    database.ForEachFreshObjectReferencePickerAssetSnapshot(
        [&entries](const ObjectReferencePickerAssetSnapshot& snapshot)
        {
            for (const auto& subAsset : snapshot.subAssets)
            {
                if (!IsInspectorReferenceableSubAsset(subAsset.artifactType) ||
                    subAsset.subAssetKey.empty())
                {
                    continue;
                }

                if (!CanStoreEditorAssetDragPayload(
                        snapshot.sourceAssetPath,
                        snapshot.assetId,
                        subAsset.subAssetKey))
                {
                    continue;
                }

                entries.push_back({
                    snapshot.sourceAssetPath + " / " +
                    (!subAsset.displayName.empty() ? subAsset.displayName : subAsset.subAssetKey),
                    MakeEditorAssetDragPayload(
                        snapshot.sourceAssetPath,
                        snapshot.assetId,
                        subAsset.subAssetKey,
                        subAsset.artifactType,
                        false,
                        true,
                        false,
                        true)
                });
            }
        });

    std::stable_sort(
        entries.begin(),
        entries.end(),
        [](const ObjectReferencePickerEntry& p_left, const ObjectReferencePickerEntry& p_right)
        {
            return p_left.displayName < p_right.displayName;
        });
    entries.erase(
        std::unique(
            entries.begin(),
            entries.end(),
            [](const ObjectReferencePickerEntry& p_left, const ObjectReferencePickerEntry& p_right)
            {
                return p_left.displayName == p_right.displayName;
            }),
        entries.end());
    return entries;
}

AssetWatcherStartupReport BuildAssetWatcherStartupReport(
    const std::filesystem::path& engineAssetsPath,
    const bool engineWatcherStarted,
    const std::filesystem::path& projectAssetsPath,
    const bool projectWatcherStarted)
{
    AssetWatcherStartupReport report;
    if (!engineWatcherStarted)
    {
        report.diagnostics.push_back({
            NLS::Core::Assets::AssetDiagnosticSeverity::Error,
            "asset-watcher-start-failed",
            {},
            engineAssetsPath,
            "Engine asset watcher could not be started."
        });
    }
    if (!projectWatcherStarted)
    {
        report.diagnostics.push_back({
            NLS::Core::Assets::AssetDiagnosticSeverity::Error,
            "asset-watcher-start-failed",
            {},
            projectAssetsPath,
            "Project asset watcher could not be started."
        });
    }
    report.succeeded = report.diagnostics.empty();
    return report;
}
}
