#pragma once

#include "Assets/AssetImporterSettings.h"
#include "Assets/AssetMeta.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace NLS::Editor::Assets
{
enum class ModelTextureResolutionKind
{
    ExplicitRemap,
    SourcePath,
    NameSearch,
    ModelEmbeddedFallback,
    Missing,
    Invalid
};

enum class TextureSourceKind
{
    ExternalFile,
    EmbeddedData,
    BufferView,
    Missing
};

enum class ModelTextureStableKeyStatus
{
    Stable,
    OrderDerived,
    Insufficient
};

struct ModelTextureDiagnostic
{
    std::string severity;
    std::string code;
    std::string message;
};

struct ModelTextureSourceReference
{
    std::string sourceKey;
    std::string materialTextureKey;
    std::string stableKey;
    std::string displayName;
    std::string uri;
    std::string normalizedUri;
    std::string bufferViewKey;
    std::string embeddedIndex;
    std::string stableDiscriminator;
    TextureSourceKind kind = TextureSourceKind::Missing;
    ModelTextureStableKeyStatus stableKeyStatus = ModelTextureStableKeyStatus::Stable;
    bool hasModelLocalPayload = false;
};

struct ModelTextureAssetCandidate
{
    NLS::Core::Assets::AssetId assetId;
    std::string subAssetKey;
    std::filesystem::path editorPath;
    std::filesystem::path artifactPath;
    std::string displayName;
    NLS::Core::Assets::AssetType assetType = NLS::Core::Assets::AssetType::Unknown;
    bool imported = true;
    uint32_t rootIndex = 0u;
    std::string artifactHashOrVersion;
    std::string nameQuery;
};

struct ModelTextureExplicitRemap
{
    std::string sourceStableKey;
    ModelTextureAssetCandidate target;
};

struct ModelTextureResolveRequest
{
    ModelTextureResolutionSettings settings;
    std::vector<ModelTextureExplicitRemap> remaps;
    std::vector<ModelTextureAssetCandidate> pathCandidates;
    std::vector<ModelTextureAssetCandidate> nameCandidates;
};

struct ResolvedModelTextureReference
{
    ModelTextureSourceReference source;
    ModelTextureResolutionKind kind = ModelTextureResolutionKind::Missing;
    std::string materialTextureKey;
    NLS::Core::Assets::AssetId targetAssetId;
    std::string targetSubAssetKey;
    std::filesystem::path resourcePath;
    std::string modelSubAssetKey;
    std::vector<ModelTextureDiagnostic> diagnostics;
    std::filesystem::path targetEditorPath;
    std::string targetArtifactHashOrVersion;
    std::string mappingQuery;
    std::string mappingMode;
    std::vector<ModelTextureAssetCandidate> mappingCandidates;
};

void ApplyModelTextureFallback(ResolvedModelTextureReference& result);
std::string MakeModelTextureStableKey(const ModelTextureSourceReference& source);
std::vector<ModelTextureSourceReference> AssignModelTextureStableKeys(
    std::vector<ModelTextureSourceReference> sources);
std::string MakeModelTextureMappingDependencyValue(
    const std::string& query,
    const std::string& mode);
std::string BuildModelTextureMappingFingerprint(
    const std::vector<ModelTextureAssetCandidate>& candidates);
ResolvedModelTextureReference ResolveModelTextureReference(
    const ModelTextureSourceReference& source,
    const ModelTextureResolveRequest& request);
const char* ToString(ModelTextureResolutionKind kind);
const char* ToString(TextureSourceKind kind);
const char* ToString(ModelTextureStableKeyStatus status);
}
