#include "Assets/EditorAssetDragPayload.h"

#include "Guid.h"

#include <algorithm>
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
    const bool previewPrefabReady)
{
    EditorAssetDragPayload payload;
    CopyPayloadString(payload.assetPath, assetPath);
    CopyPayloadString(payload.assetGuid, assetId.ToString());
    CopyPayloadString(payload.subAssetKey, subAssetKey);
    payload.artifactType = static_cast<uint32_t>(artifactType);
    payload.generatedModelPrefab = generatedModelPrefab ? 1u : 0u;
    payload.imported = imported ? 1u : 0u;
    payload.previewPrefabReady = previewPrefabReady ? 1u : 0u;
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
    const bool previewPrefabReady)
{
    return MakeEditorAssetDragPayload(
        assetPath,
        assetId,
        subAssetKey,
        artifactType,
        generatedModelPrefab,
        imported,
        previewPrefabReady);
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
    switch (static_cast<ArtifactType>(payload.artifactType))
    {
    case ArtifactType::Model:
    case ArtifactType::Mesh:
    case ArtifactType::Material:
    case ArtifactType::Texture:
    case ArtifactType::Skeleton:
    case ArtifactType::Skin:
    case ArtifactType::AnimationClip:
    case ArtifactType::MorphTarget:
    case ArtifactType::Prefab:
    case ArtifactType::Scene:
    case ArtifactType::Shader:
    case ArtifactType::Audio:
        return static_cast<ArtifactType>(payload.artifactType);
    case ArtifactType::Unknown:
    default:
        return ArtifactType::Unknown;
    }
}

bool IsEditorAssetDragPayloadPreviewPrefabReady(const EditorAssetDragPayload& payload)
{
    return payload.previewPrefabReady != 0u;
}
}
