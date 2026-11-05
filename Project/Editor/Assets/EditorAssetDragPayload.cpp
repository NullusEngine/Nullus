#include "Assets/EditorAssetDragPayload.h"

#include "Assets/ScriptAssetUtility.h"
#include "Guid.h"
#include <Assets/AssetMeta.h>
#include <Core/ServiceLocator.h>
#include <Engine/Assets/RuntimeAssetDatabase.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace NLS::Editor::Assets
{
namespace
{
template <size_t Capacity>
void CopyPayloadString(char (&destination)[Capacity], const std::string& source)
{
    static_assert(Capacity > 0u);
    const auto length = std::min(source.size(), Capacity - 1u);
    std::memcpy(destination, source.data(), length);
    destination[length] = '\0';
}
}

EditorAssetDragPayload MakeEditorAssetDragPayload(
    const AssetDatabaseRecord& record,
    const bool generatedModelPrefab,
    const bool imported)
{
    return MakeEditorAssetDragPayload(
        record.assetPath,
        record.assetId,
        record.subAssetKey,
        record.artifactType,
        generatedModelPrefab,
        imported);
}

EditorAssetDragPayload MakeEditorAssetDragPayload(
    const std::string& assetPath,
    const NLS::Core::Assets::AssetId assetId,
    const std::string& subAssetKey,
    const NLS::Core::Assets::ArtifactType artifactType,
    const bool generatedModelPrefab,
    const bool imported,
    const bool generatedBrowserSubAsset)
{
    EditorAssetDragPayload payload;
    CopyPayloadString(payload.assetPath, assetPath);
    CopyPayloadString(payload.assetGuid, assetId.ToString());
    CopyPayloadString(payload.subAssetKey, subAssetKey);
    payload.artifactType = static_cast<uint32_t>(artifactType);
    payload.generatedModelPrefab = generatedModelPrefab ? 1u : 0u;
    payload.imported = imported ? 1u : 0u;
    payload.scriptAsset = IsScriptAssetPath(assetPath) ? 1u : 0u;
    payload.reserved = 0u;
    payload.generatedBrowserSubAsset = generatedBrowserSubAsset ? 1u : 0u;
    return payload;
}

#if defined(NLS_ENABLE_TEST_HOOKS)
EditorAssetDragPayload MakeEditorAssetDragPayloadForTesting(
    const std::string& assetPath,
    const NLS::Core::Assets::AssetId assetId,
    const std::string& subAssetKey,
    const NLS::Core::Assets::ArtifactType artifactType,
    const bool generatedModelPrefab,
    const bool imported,
    const bool generatedBrowserSubAsset)
{
    return MakeEditorAssetDragPayload(
        assetPath,
        assetId,
        subAssetKey,
        artifactType,
        generatedModelPrefab,
        imported,
        generatedBrowserSubAsset);
}
#endif

bool CanStoreEditorAssetDragPayload(
    const std::string& assetPath,
    const NLS::Core::Assets::AssetId assetId,
    const std::string& subAssetKey)
{
    return assetPath.size() < kEditorAssetDragPayloadPathCapacity &&
        assetId.ToString().size() < kEditorAssetDragPayloadGuidCapacity &&
        subAssetKey.size() < kEditorAssetDragPayloadSubAssetCapacity;
}

std::string GetEditorAssetDragPayloadPath(const EditorAssetDragPayload& payload)
{
    return payload.assetPath;
}

std::string GetEditorAssetDragPayloadGuid(const EditorAssetDragPayload& payload)
{
    return payload.assetGuid;
}

std::string GetEditorAssetDragPayloadSubAssetKey(const EditorAssetDragPayload& payload)
{
    return payload.subAssetKey;
}

NLS::Core::Assets::AssetId GetEditorAssetDragPayloadAssetId(const EditorAssetDragPayload& payload)
{
    const auto guid = NLS::Guid::TryParse(GetEditorAssetDragPayloadGuid(payload));
    return guid.has_value()
        ? NLS::Core::Assets::AssetId(*guid)
        : NLS::Core::Assets::AssetId {};
}

NLS::Core::Assets::ArtifactType GetEditorAssetDragPayloadArtifactType(const EditorAssetDragPayload& payload)
{
    using NLS::Core::Assets::ArtifactType;
    if (payload.artifactType >= static_cast<uint32_t>(ArtifactType::Count))
        return ArtifactType::Unknown;

    const auto artifactType = static_cast<ArtifactType>(payload.artifactType);
    switch (artifactType)
    {
    case ArtifactType::Model:
    case ArtifactType::Mesh:
    case ArtifactType::Material:
    case ArtifactType::Texture:
    case ArtifactType::Sprite:
    case ArtifactType::Skeleton:
    case ArtifactType::Skin:
    case ArtifactType::AnimationClip:
    case ArtifactType::MorphTarget:
    case ArtifactType::Prefab:
    case ArtifactType::Scene:
    case ArtifactType::Shader:
    case ArtifactType::Audio:
    case ArtifactType::UIDocument:
    case ArtifactType::UIStyleSheet:
    case ArtifactType::Font:
    case ArtifactType::LocalizationTable:
        return artifactType;
    case ArtifactType::Unknown:
    case ArtifactType::Count:
        return ArtifactType::Unknown;
    }
    return ArtifactType::Unknown;
}

bool IsEditorAssetDragPayloadGeneratedBrowserSubAsset(const EditorAssetDragPayload& payload)
{
    return payload.generatedBrowserSubAsset != 0u;
}

bool IsEditorAssetDragPayloadScript(const EditorAssetDragPayload& payload)
{
    if (payload.scriptAsset != 0u)
        return true;
    return IsScriptAssetPath(GetEditorAssetDragPayloadPath(payload));
}

bool CanMoveEditorAssetDragPayloadAsPhysicalProjectFile(const EditorAssetDragPayload& payload)
{
    return !IsEditorAssetDragPayloadGeneratedBrowserSubAsset(payload);
}
std::optional<EditorAssetDragPayload> ResolveSpriteDragPayload(const EditorAssetDragPayload& payload)
{
    using Type=NLS::Core::Assets::ArtifactType;
    const auto type=GetEditorAssetDragPayloadArtifactType(payload);
    if(type==Type::Sprite)return payload;
    if(type!=Type::Texture||!NLS::Core::ServiceLocator::Contains<Engine::Assets::RuntimeAssetDatabase>())return {};
    const auto id=GetEditorAssetDragPayloadAssetId(payload);
    const Engine::Assets::RuntimeAssetManifestEntry* single=nullptr;
    for(const auto& entry:NLS_SERVICE(Engine::Assets::RuntimeAssetDatabase).GetManifest().entries)
        if(entry.assetId==id&&entry.artifactType==Type::Sprite){if(single)return {};single=&entry;}
    if(!single)return {};
    return MakeEditorAssetDragPayload(GetEditorAssetDragPayloadPath(payload),id,single->subAssetKey,Type::Sprite,false,true,true);
}

std::optional<EditorAssetDragPayload> ResolveSpriteDragPayload(
    const std::string& resourcePath, const std::string& projectAssetsPath)
{
    if (resourcePath.empty() || resourcePath.front() == ':') return {};
    const auto fragment = resourcePath.find('#');
    const auto key = fragment == std::string::npos ? std::string{} : resourcePath.substr(fragment + 1);
    if (!key.empty() && !key.starts_with("sprite:")) return {};
    auto assetsRoot = std::filesystem::path(projectAssetsPath).lexically_normal();
    while (!assetsRoot.empty() && !assetsRoot.has_filename()) assetsRoot = assetsRoot.parent_path();
    if (assetsRoot.empty()) return {};
    auto source = std::filesystem::path(resourcePath.substr(0, fragment)).lexically_normal();
    if (source.empty()) return {};
    if (source.is_relative())
    {
        auto first = source.begin()->string();
        std::transform(first.begin(), first.end(), first.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        source = ((first == "assets" ? assetsRoot.parent_path() : assetsRoot) / source).lexically_normal();
    }
    const auto withinAssets = source.lexically_relative(assetsRoot);
    if (withinAssets.empty() || withinAssets.is_absolute()) return {};
    for (const auto& part : withinAssets) if (part == "..") return {};
    const auto meta = NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(source));
    if (!meta || meta->assetType != NLS::Core::Assets::AssetType::Texture) return {};
    const auto editorPath = (std::filesystem::path("Assets") / withinAssets).generic_string();
    if (!CanStoreEditorAssetDragPayload(editorPath, meta->id, key)) return {};
    return ResolveSpriteDragPayload(MakeEditorAssetDragPayload(editorPath, meta->id, key,
        key.empty() ? NLS::Core::Assets::ArtifactType::Texture : NLS::Core::Assets::ArtifactType::Sprite,
        false, true, !key.empty()));
}
}
