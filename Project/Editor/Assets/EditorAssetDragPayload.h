#pragma once

#include "Assets/ArtifactManifest.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/AssetId.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace NLS::Editor::Assets
{
constexpr const char* kEditorAssetDragPayloadType = "EditorAsset";
constexpr size_t kEditorAssetDragPayloadPathCapacity = 260u;
constexpr size_t kEditorAssetDragPayloadSubAssetCapacity = 160u;
constexpr size_t kEditorAssetDragPayloadGuidCapacity = 37u;

struct EditorAssetDragPayload
{
    char assetPath[kEditorAssetDragPayloadPathCapacity] {};
    char assetGuid[kEditorAssetDragPayloadGuidCapacity] {};
    char subAssetKey[kEditorAssetDragPayloadSubAssetCapacity] {};
    uint32_t artifactType = static_cast<uint32_t>(NLS::Core::Assets::ArtifactType::Unknown);
    uint8_t generatedModelPrefab = 0u;
    uint8_t imported = 0u;
    uint8_t previewPrefabReady = 0u;
    uint8_t generatedBrowserSubAsset = 0u;
};

static_assert(std::is_trivially_copyable_v<EditorAssetDragPayload>);

EditorAssetDragPayload MakeEditorAssetDragPayload(
    const AssetDatabaseRecord& record,
    bool generatedModelPrefab,
    bool imported);

EditorAssetDragPayload MakeEditorAssetDragPayload(
    const std::string& assetPath,
    NLS::Core::Assets::AssetId assetId,
    const std::string& subAssetKey,
    NLS::Core::Assets::ArtifactType artifactType,
    bool generatedModelPrefab,
    bool imported,
    bool previewPrefabReady = false,
    bool generatedBrowserSubAsset = false);

#if defined(NLS_ENABLE_TEST_HOOKS)
EditorAssetDragPayload MakeEditorAssetDragPayloadForTesting(
    const std::string& assetPath,
    NLS::Core::Assets::AssetId assetId,
    const std::string& subAssetKey,
    NLS::Core::Assets::ArtifactType artifactType,
    bool generatedModelPrefab,
    bool imported,
    bool previewPrefabReady = false,
    bool generatedBrowserSubAsset = false);
#endif

bool CanStoreEditorAssetDragPayload(
    const std::string& assetPath,
    NLS::Core::Assets::AssetId assetId,
    const std::string& subAssetKey);

std::string GetEditorAssetDragPayloadPath(const EditorAssetDragPayload& payload);
std::string GetEditorAssetDragPayloadGuid(const EditorAssetDragPayload& payload);
std::string GetEditorAssetDragPayloadSubAssetKey(const EditorAssetDragPayload& payload);
NLS::Core::Assets::AssetId GetEditorAssetDragPayloadAssetId(const EditorAssetDragPayload& payload);
NLS::Core::Assets::ArtifactType GetEditorAssetDragPayloadArtifactType(const EditorAssetDragPayload& payload);
bool IsEditorAssetDragPayloadPreviewPrefabReady(const EditorAssetDragPayload& payload);
bool IsEditorAssetDragPayloadGeneratedBrowserSubAsset(const EditorAssetDragPayload& payload);
bool CanMoveEditorAssetDragPayloadAsPhysicalProjectFile(const EditorAssetDragPayload& payload);
}
