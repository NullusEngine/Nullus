#include "Assets/ModelTextureReferenceResolver.h"

#include "Assets/ModelTextureTextCodec.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <utility>

namespace NLS::Editor::Assets
{
namespace
{
void AppendComponent(std::string& key, const char* name, const std::string& value)
{
    if (value.empty())
        return;
    key.push_back(';');
    key += name;
    key.push_back('=');
    key += EncodeModelTextureTextField(value);
}

bool HasStrongIdentity(const ModelTextureSourceReference& source)
{
    return !source.sourceKey.empty() ||
        !source.normalizedUri.empty() ||
        !source.bufferViewKey.empty() ||
        !source.embeddedIndex.empty() ||
        !source.stableDiscriminator.empty();
}

ModelTextureDiagnostic MakeDiagnostic(
    std::string code,
    std::string message,
    std::string severity = "Warning")
{
    return {std::move(severity), std::move(code), std::move(message)};
}

std::string ToLower(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

bool CaseInsensitiveMatch(const std::string& lhs, const std::string& rhs)
{
    return ToLower(lhs) == ToLower(rhs);
}

bool IsTextureCandidate(const ModelTextureAssetCandidate& candidate)
{
    return candidate.assetType == NLS::Core::Assets::AssetType::Texture;
}

bool HasUsableArtifact(const ModelTextureAssetCandidate& candidate)
{
    return candidate.assetId.IsValid() && !candidate.artifactPath.empty();
}

bool IsViableTextureCandidate(
    const ModelTextureAssetCandidate& candidate,
    const ModelTextureResolutionSettings& settings)
{
    if (!IsTextureCandidate(candidate))
        return false;
    if (!candidate.imported && !settings.autoImportMissingTextureFiles)
        return false;
    return HasUsableArtifact(candidate);
}

std::string CandidateAssetTypeToken(const ModelTextureAssetCandidate& candidate)
{
    return candidate.assetType == NLS::Core::Assets::AssetType::Texture ? "texture" : "unknown";
}

std::string BuildMappingFingerprintRow(const ModelTextureAssetCandidate& candidate)
{
    const auto normalizedPath = candidate.editorPath.lexically_normal();
    std::ostringstream row;
    row << candidate.rootIndex << '|'
        << EncodeModelTextureTextField(normalizedPath.generic_string()) << '|'
        << EncodeModelTextureTextField(candidate.assetId.ToString()) << '|'
        << EncodeModelTextureTextField(CandidateAssetTypeToken(candidate)) << '|'
        << (candidate.imported ? "imported" : "unimported") << '|'
        << "case-sensitive" << '|'
        << EncodeModelTextureTextField(ToLower(normalizedPath.filename().generic_string())) << '|'
        << EncodeModelTextureTextField(candidate.subAssetKey) << '|'
        << EncodeModelTextureTextField(candidate.artifactHashOrVersion);
    return row.str();
}

void ApplyProjectTextureResolution(
    ResolvedModelTextureReference& result,
    const ModelTextureAssetCandidate& candidate,
    const ModelTextureResolutionKind kind)
{
    result.kind = kind;
    result.targetAssetId = candidate.assetId;
    result.targetSubAssetKey = candidate.subAssetKey;
    result.targetEditorPath = candidate.editorPath;
    result.targetArtifactHashOrVersion = candidate.artifactHashOrVersion;
    result.resourcePath = candidate.artifactPath;
}

}

void ApplyModelTextureFallback(ResolvedModelTextureReference& result)
{
    result.kind = ModelTextureResolutionKind::ModelEmbeddedFallback;
    result.targetAssetId = {};
    result.targetSubAssetKey.clear();
    result.targetEditorPath.clear();
    result.targetArtifactHashOrVersion.clear();
    result.resourcePath.clear();
    if (!result.source.sourceKey.empty())
        result.modelSubAssetKey = "texture:" + result.source.sourceKey;
    else if (!result.source.materialTextureKey.empty())
        result.modelSubAssetKey = "texture:" + result.source.materialTextureKey;
    else
        result.modelSubAssetKey = "texture:" + result.source.stableKey;
}

std::string MakeModelTextureStableKey(const ModelTextureSourceReference& source)
{
    std::string key = "mtxsrc:v1:kind=";
    key += ToString(source.kind);

    AppendComponent(key, "source", source.sourceKey);
    AppendComponent(key, "uri", source.normalizedUri);
    AppendComponent(key, "bufferView", source.bufferViewKey);
    AppendComponent(key, "embedded", source.embeddedIndex);
    AppendComponent(key, "discriminator", source.stableDiscriminator);

    if (!HasStrongIdentity(source))
        AppendComponent(key, "name", source.displayName);

    return key;
}

std::vector<ModelTextureSourceReference> AssignModelTextureStableKeys(
    std::vector<ModelTextureSourceReference> sources)
{
    for (auto& source : sources)
    {
        source.stableKey = MakeModelTextureStableKey(source);
        source.stableKeyStatus = HasStrongIdentity(source) ?
            ModelTextureStableKeyStatus::Stable :
            ModelTextureStableKeyStatus::Insufficient;
    }

    std::map<std::string, std::vector<size_t>> groups;
    for (size_t index = 0u; index < sources.size(); ++index)
        groups[sources[index].stableKey].push_back(index);

    for (const auto& [stableKey, indexes] : groups)
    {
        if (indexes.size() <= 1u)
        {
            if (sources[indexes.front()].stableKeyStatus == ModelTextureStableKeyStatus::Insufficient)
                sources[indexes.front()].stableKeyStatus = ModelTextureStableKeyStatus::Stable;
            continue;
        }

        bool allHaveMaterialKeys = true;
        for (const size_t index : indexes)
            allHaveMaterialKeys = allHaveMaterialKeys && !sources[index].materialTextureKey.empty();

        if (allHaveMaterialKeys)
        {
            for (const size_t index : indexes)
            {
                sources[index].stableDiscriminator = sources[index].materialTextureKey;
                sources[index].stableKey = MakeModelTextureStableKey(sources[index]);
                sources[index].stableKeyStatus = ModelTextureStableKeyStatus::Stable;
            }
            continue;
        }

        for (size_t occurrence = 0u; occurrence < indexes.size(); ++occurrence)
        {
            auto& source = sources[indexes[occurrence]];
            source.stableKey = stableKey + ";dup=" + std::to_string(occurrence);
            source.stableKeyStatus = ModelTextureStableKeyStatus::OrderDerived;
        }
    }

    return sources;
}

std::string MakeModelTextureMappingDependencyValue(
    const std::string& query,
    const std::string& mode)
{
    return "project|" + query + "|" + mode;
}

std::string BuildModelTextureMappingFingerprint(
    const std::vector<ModelTextureAssetCandidate>& candidates)
{
    std::vector<ModelTextureAssetCandidate> sorted = candidates;
    std::sort(
        sorted.begin(),
        sorted.end(),
        [](const ModelTextureAssetCandidate& lhs, const ModelTextureAssetCandidate& rhs)
        {
            if (lhs.rootIndex != rhs.rootIndex)
                return lhs.rootIndex < rhs.rootIndex;
            const auto lhsPath = lhs.editorPath.lexically_normal().generic_string();
            const auto rhsPath = rhs.editorPath.lexically_normal().generic_string();
            if (lhsPath != rhsPath)
                return lhsPath < rhsPath;
            return lhs.assetId.ToString() < rhs.assetId.ToString();
        });

    std::string fingerprint;
    for (const auto& candidate : sorted)
    {
        if (!fingerprint.empty())
            fingerprint.push_back(';');
        fingerprint += BuildMappingFingerprintRow(candidate);
    }
    return fingerprint;
}

ResolvedModelTextureReference ResolveModelTextureReference(
    const ModelTextureSourceReference& source,
    const ModelTextureResolveRequest& request)
{
    ResolvedModelTextureReference result;
    result.source = source;
    result.materialTextureKey = source.materialTextureKey;

    if (!request.settings.useExternalTextures)
    {
        result.diagnostics.push_back(MakeDiagnostic(
            "model-texture-external-resolution-disabled",
            "External texture resolution is disabled for this model."));
        if (source.hasModelLocalPayload)
            ApplyModelTextureFallback(result);
        return result;
    }

    for (const auto& remap : request.remaps)
    {
        if (remap.sourceStableKey != source.stableKey)
            continue;

        if (!remap.target.assetId.IsValid())
        {
            result.diagnostics.push_back(MakeDiagnostic(
                "model-texture-remap-invalid-target",
                "Texture remap target is missing or invalid."));
            break;
        }

        if (!IsTextureCandidate(remap.target))
        {
            result.diagnostics.push_back(MakeDiagnostic(
                "model-texture-remap-non-texture-target",
                "Texture remap target is not a texture asset."));
            break;
        }

        if (remap.target.artifactPath.empty())
        {
            result.diagnostics.push_back(MakeDiagnostic(
                "model-texture-artifact-missing",
                "Texture remap target has no imported artifact."));
            break;
        }

        ApplyProjectTextureResolution(result, remap.target, ModelTextureResolutionKind::ExplicitRemap);
        return result;
    }

    if (!source.normalizedUri.empty())
    {
        result.mappingQuery = source.normalizedUri;
        result.mappingMode = "source-path";
    }

    for (const auto& candidate : request.pathCandidates)
    {
        if (!CaseInsensitiveMatch(candidate.editorPath.lexically_normal().generic_string(), source.normalizedUri))
            continue;

        if (IsTextureCandidate(candidate) && candidate.assetId.IsValid() && candidate.artifactPath.empty())
        {
            result.diagnostics.push_back(MakeDiagnostic(
                "model-texture-artifact-missing",
                "Texture asset has no imported artifact available yet."));
            if (result.mappingQuery.empty())
            {
                result.mappingQuery = source.normalizedUri;
                result.mappingMode = "source-path";
            }
            result.mappingCandidates.push_back(candidate);
            continue;
        }

        if (!IsViableTextureCandidate(candidate, request.settings))
            continue;

        ApplyProjectTextureResolution(result, candidate, ModelTextureResolutionKind::SourcePath);
        result.mappingQuery = source.normalizedUri;
        result.mappingMode = "source-path";
        result.mappingCandidates.push_back(candidate);
        return result;
    }

    if (request.settings.searchByName)
    {
        const auto queryName = source.displayName.empty()
            ? std::filesystem::path(source.normalizedUri).filename().generic_string()
            : source.displayName;
        if (result.mappingQuery.empty())
        {
            result.mappingQuery = queryName;
            result.mappingMode = "name-search";
        }
        std::vector<ModelTextureAssetCandidate> viableNameCandidates;
        for (const auto& candidate : request.nameCandidates)
        {
            if (!queryName.empty() && !candidate.nameQuery.empty() && !CaseInsensitiveMatch(candidate.nameQuery, queryName))
                continue;

            if (IsTextureCandidate(candidate) && candidate.assetId.IsValid() && candidate.artifactPath.empty())
            {
                result.diagnostics.push_back(MakeDiagnostic(
                    "model-texture-artifact-missing",
                    "Texture asset has no imported artifact available yet."));
                result.mappingCandidates.push_back(candidate);
                continue;
            }

            if (IsViableTextureCandidate(candidate, request.settings))
            {
                viableNameCandidates.push_back(candidate);
                result.mappingCandidates.push_back(candidate);
            }
        }

        std::sort(
            viableNameCandidates.begin(),
            viableNameCandidates.end(),
            [](const ModelTextureAssetCandidate& lhs, const ModelTextureAssetCandidate& rhs)
            {
                if (lhs.rootIndex != rhs.rootIndex)
                    return lhs.rootIndex < rhs.rootIndex;
                const auto lhsPath = lhs.editorPath.generic_string();
                const auto rhsPath = rhs.editorPath.generic_string();
                if (lhsPath != rhsPath)
                    return lhsPath < rhsPath;
                return lhs.assetId.ToString() < rhs.assetId.ToString();
            });

        if (viableNameCandidates.size() == 1u)
        {
            ApplyProjectTextureResolution(result, viableNameCandidates.front(), ModelTextureResolutionKind::NameSearch);
            result.mappingQuery = queryName;
            result.mappingMode = "name-search";
            return result;
        }

        if (viableNameCandidates.size() > 1u)
        {
            result.mappingQuery = queryName;
            result.mappingMode = "name-search";
            result.diagnostics.push_back(MakeDiagnostic(
                "model-texture-name-ambiguous",
                "Texture name matched multiple project texture assets."));
        }

    }

    if (source.hasModelLocalPayload)
        ApplyModelTextureFallback(result);

    return result;
}

const char* ToString(const ModelTextureResolutionKind kind)
{
    switch (kind)
    {
    case ModelTextureResolutionKind::ExplicitRemap: return "ExplicitRemap";
    case ModelTextureResolutionKind::SourcePath: return "SourcePath";
    case ModelTextureResolutionKind::NameSearch: return "NameSearch";
    case ModelTextureResolutionKind::ModelEmbeddedFallback: return "ModelEmbeddedFallback";
    case ModelTextureResolutionKind::Invalid: return "Invalid";
    case ModelTextureResolutionKind::Missing:
        return "Missing";
    }

    return "Missing";
}

const char* ToString(const TextureSourceKind kind)
{
    switch (kind)
    {
    case TextureSourceKind::ExternalFile: return "ExternalFile";
    case TextureSourceKind::EmbeddedData: return "EmbeddedData";
    case TextureSourceKind::BufferView: return "BufferView";
    case TextureSourceKind::Missing:
        return "Missing";
    }

    return "Missing";
}

const char* ToString(const ModelTextureStableKeyStatus status)
{
    switch (status)
    {
    case ModelTextureStableKeyStatus::Stable: return "Stable";
    case ModelTextureStableKeyStatus::OrderDerived: return "OrderDerived";
    case ModelTextureStableKeyStatus::Insufficient:
        return "Insufficient";
    }

    return "Insufficient";
}
}
