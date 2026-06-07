#include "Assets/ExternalAssetImporter.h"

#include "Assets/AssetImporterSettings.h"
#include "Assets/ArtifactWriter.h"
#include "Assets/TextureEncoding/DirectXTexTextureEncoder.h"
#include "Debug/Logger.h"
#include "Engine/Assets/ModelPrefabBuilder.h"
#include "Profiling/Profiler.h"
#include "Assets/NativeArtifactContainer.h"
#include "Rendering/Assets/MaterialConversion.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/SceneImportPipeline.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/Assets/TextureEncoder.h"
#include "Rendering/Assets/TextureFormatResolver.h"
#include "Rendering/Assets/TextureMipGenerator.h"
#include "Rendering/Resources/Parsers/AssimpParser.h"
#include "Rendering/Resources/Parsers/FbxSdkParser.h"
#include "Serialize/ObjectGraphWriter.h"

#include <Json/json.hpp>
#include <assimp/Base64.hpp>

#ifndef NLS_HAS_ASSIMP_FBX_IMPORTER
#define NLS_HAS_ASSIMP_FBX_IMPORTER 0
#endif

#ifndef NLS_HAS_AUTODESK_FBX_SDK
#define NLS_HAS_AUTODESK_FBX_SDK 0
#endif

#ifndef NLS_HAS_DIRECTXTEX
#define NLS_HAS_DIRECTXTEX 0
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace NLS::Editor::Assets
{
namespace
{
constexpr uint32_t kExternalTextureSafetyMaxDimension = 16384u;
constexpr uint64_t kExternalTextureSafetyMaxPixels = 16384ull * 16384ull;

using ImportedSceneJson = nlohmann::json;

constexpr int64_t kExternalModelImportTimingLogThresholdMilliseconds = 100;

struct ExternalModelImportTimingStats
{
    size_t sourceMeshCount = 0u;
    size_t sceneMeshCount = 0u;
    size_t materialCount = 0u;
    size_t textureCount = 0u;
    size_t subAssetCount = 0u;
    size_t payloadCount = 0u;
    size_t dependencyCount = 0u;
    size_t diagnosticCount = 0u;
    const char* status = "failed";
};

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

bool IsGltfModelSourcePath(const std::filesystem::path& sourcePath)
{
    const auto extension = ToLower(sourcePath.extension().string());
    return extension == ".gltf" || extension == ".glb";
}

bool ShouldFlipExternalModelTextureVertically(const std::filesystem::path& sourcePath)
{
    // glTF UVs are already authored against the encoded image row order.
    return !IsGltfModelSourcePath(sourcePath);
}

char HexDigit(const unsigned char value)
{
    return value < 10u
        ? static_cast<char>('0' + value)
        : static_cast<char>('A' + (value - 10u));
}

bool IsSafeArtifactFileNameCharacter(const unsigned char character)
{
    return std::isalnum(character) || character == '_' || character == '-' || character == '.';
}

std::string EncodeArtifactFileName(const std::string& value)
{
    if (value.empty())
        return "asset";

    std::string encoded;
    encoded.reserve(value.size());
    for (const auto character : value)
    {
        const auto byte = static_cast<unsigned char>(character);
        if (IsSafeArtifactFileNameCharacter(byte))
        {
            encoded.push_back(character);
            continue;
        }

        encoded.push_back('%');
        encoded.push_back(HexDigit((byte >> 4u) & 0x0Fu));
        encoded.push_back(HexDigit(byte & 0x0Fu));
    }
    return encoded;
}

std::vector<uint8_t> ToBytes(const std::string& text)
{
    return {text.begin(), text.end()};
}

int64_t MillisecondsSince(const std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
}

void LogExternalModelImportTiming(
    const ExternalModelImportRequest& request,
    const char* stage,
    const int64_t elapsedMilliseconds,
    const ExternalModelImportTimingStats& stats)
{
    if (elapsedMilliseconds < kExternalModelImportTimingLogThresholdMilliseconds)
        return;

    NLS_LOG_INFO(
        "[AssetImport][ExternalModel] " +
        std::string(stage) +
        " " +
        std::to_string(elapsedMilliseconds) +
        "ms status=" +
        stats.status +
        " ext=" +
        request.sourcePath.extension().string() +
        " sourceMeshes=" +
        std::to_string(stats.sourceMeshCount) +
        " sceneMeshes=" +
        std::to_string(stats.sceneMeshCount) +
        " materials=" +
        std::to_string(stats.materialCount) +
        " textures=" +
        std::to_string(stats.textureCount) +
        " subAssets=" +
        std::to_string(stats.subAssetCount) +
        " payloads=" +
        std::to_string(stats.payloadCount) +
        " dependencies=" +
        std::to_string(stats.dependencyCount) +
        " diagnostics=" +
        std::to_string(stats.diagnosticCount) +
        " source=" +
        request.sourcePath.string());
}

class ScopedExternalModelImportTiming final
{
public:
    ScopedExternalModelImportTiming(
        const ExternalModelImportRequest& request,
        const ExternalModelImportTimingStats& stats)
        : m_request(request)
        , m_stats(stats)
        , m_start(std::chrono::steady_clock::now())
    {
    }

    ~ScopedExternalModelImportTiming()
    {
        LogExternalModelImportTiming(
            m_request,
            "Total",
            MillisecondsSince(m_start),
            m_stats);
    }

private:
    const ExternalModelImportRequest& m_request;
    const ExternalModelImportTimingStats& m_stats;
    std::chrono::steady_clock::time_point m_start;
};

std::optional<size_t> ParseIndexedSourceKey(const std::string& sourceKey, const std::string& prefix)
{
    const auto expectedPrefix = prefix + "/";
    if (sourceKey.rfind(expectedPrefix, 0u) != 0u)
        return std::nullopt;

    const auto indexText = sourceKey.substr(expectedPrefix.size());
    if (indexText.empty() ||
        !std::all_of(
            indexText.begin(),
            indexText.end(),
            [](const unsigned char character)
            {
                return std::isdigit(character) != 0;
            }))
    {
        return std::nullopt;
    }

    return static_cast<size_t>(std::stoull(indexText));
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
}

uint32_t ReadLittleEndianU32(const std::vector<uint8_t>& bytes, const size_t offset)
{
    if (offset + 4u > bytes.size())
        return 0u;

    return static_cast<uint32_t>(bytes[offset]) |
        (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
        (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
        (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
}

float ReadFloat32(const std::vector<uint8_t>& bytes, const size_t offset)
{
    float value = 0.0f;
    if (offset + sizeof(value) <= bytes.size())
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

uint32_t ReadUnsignedIndex(const std::vector<uint8_t>& bytes, const size_t offset, const uint32_t componentType)
{
    if (componentType == 5121u && offset < bytes.size())
        return bytes[offset];

    if (componentType == 5123u && offset + sizeof(uint16_t) <= bytes.size())
    {
        uint16_t value = 0u;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }

    if (componentType == 5125u && offset + sizeof(uint32_t) <= bytes.size())
    {
        uint32_t value = 0u;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }

    return 0u;
}

bool IsDataUri(const std::string& uri)
{
    return ToLower(uri).rfind("data:", 0u) == 0u;
}

std::vector<uint8_t> DecodeDataUri(const std::string& uri)
{
    if (!IsDataUri(uri))
        return {};

    const auto comma = uri.find(',');
    if (comma == std::string::npos)
        return {};

    const auto metadata = ToLower(uri.substr(0u, comma));
    if (metadata.find(";base64") == std::string::npos)
        return {};

    try
    {
        return Assimp::Base64::Decode(uri.substr(comma + 1u));
    }
    catch (...)
    {
        return {};
    }
}

bool ExtractGlbJsonAndBinary(
    const std::vector<uint8_t>& sourceBytes,
    std::string& jsonText,
    std::vector<uint8_t>& binaryChunk)
{
    constexpr uint32_t kGlbMagic = 0x46546C67u;
    constexpr uint32_t kGlbVersion = 2u;
    constexpr uint32_t kGlbJsonChunkType = 0x4E4F534Au;
    constexpr uint32_t kGlbBinChunkType = 0x004E4942u;

    if (sourceBytes.size() < 20u ||
        ReadLittleEndianU32(sourceBytes, 0u) != kGlbMagic ||
        ReadLittleEndianU32(sourceBytes, 4u) != kGlbVersion)
    {
        return false;
    }

    const auto declaredLength = ReadLittleEndianU32(sourceBytes, 8u);
    if (declaredLength > sourceBytes.size())
        return false;

    size_t offset = 12u;
    while (offset + 8u <= declaredLength)
    {
        const auto chunkLength = ReadLittleEndianU32(sourceBytes, offset);
        const auto chunkType = ReadLittleEndianU32(sourceBytes, offset + 4u);
        offset += 8u;
        if (offset + chunkLength > declaredLength)
            return false;

        if (chunkType == kGlbJsonChunkType)
        {
            jsonText.assign(
                reinterpret_cast<const char*>(sourceBytes.data() + offset),
                reinterpret_cast<const char*>(sourceBytes.data() + offset + chunkLength));
        }
        else if (chunkType == kGlbBinChunkType)
        {
            binaryChunk.assign(sourceBytes.begin() + offset, sourceBytes.begin() + offset + chunkLength);
        }

        offset += chunkLength;
    }

    return !jsonText.empty();
}

const ImportedSceneJson* FindArray(const ImportedSceneJson& object, const char* key)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->is_array())
        return nullptr;
    return &*found;
}

const ImportedSceneJson* FindObject(const ImportedSceneJson& object, const char* key)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->is_object())
        return nullptr;
    return &*found;
}

void AddError(
    NLS::Core::Assets::AssetDiagnostics& diagnostics,
    const NLS::Core::Assets::AssetId assetId,
    const std::filesystem::path& path,
    std::string code,
    std::string message)
{
    diagnostics.push_back({
        NLS::Core::Assets::AssetDiagnosticSeverity::Error,
        std::move(code),
        assetId,
        path,
        std::move(message)
    });
}

void AddWarning(
    NLS::Core::Assets::AssetDiagnostics& diagnostics,
    const NLS::Core::Assets::AssetId assetId,
    const std::filesystem::path& path,
    std::string code,
    std::string message)
{
    diagnostics.push_back({
        NLS::Core::Assets::AssetDiagnosticSeverity::Warning,
        std::move(code),
        assetId,
        path,
        std::move(message)
    });
}

bool HasErrors(const NLS::Core::Assets::AssetDiagnostics& diagnostics)
{
    return std::any_of(
        diagnostics.begin(),
        diagnostics.end(),
        [](const NLS::Core::Assets::AssetDiagnostic& diagnostic)
        {
            return diagnostic.severity == NLS::Core::Assets::AssetDiagnosticSeverity::Error;
        });
}

std::string FileStamp(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return {};

    error.clear();
    const auto writeTime = std::filesystem::last_write_time(path, error);
    if (error)
        return {};

    const auto writeTimeTicks = static_cast<std::intmax_t>(writeTime.time_since_epoch().count());
    return std::to_string(size) + ":" + std::to_string(writeTimeTicks);
}

bool IsEmbeddedUri(const std::string& uri)
{
    return ToLower(uri).rfind("data:", 0u) == 0u;
}

bool PathEscapesRoot(const std::filesystem::path& relativePath)
{
    if (relativePath.empty() || relativePath.is_absolute())
        return true;

    for (const auto& part : relativePath)
    {
        if (part == "..")
            return true;
    }
    return false;
}

std::optional<unsigned char> HexValue(const char value)
{
    if (value >= '0' && value <= '9')
        return static_cast<unsigned char>(value - '0');
    if (value >= 'a' && value <= 'f')
        return static_cast<unsigned char>(10 + value - 'a');
    if (value >= 'A' && value <= 'F')
        return static_cast<unsigned char>(10 + value - 'A');
    return std::nullopt;
}

std::string DecodeUriPercentEscapes(const std::string& uri)
{
    std::string decoded;
    decoded.reserve(uri.size());
    for (size_t index = 0u; index < uri.size(); ++index)
    {
        if (uri[index] == '%' && index + 2u < uri.size())
        {
            const auto high = HexValue(uri[index + 1u]);
            const auto low = HexValue(uri[index + 2u]);
            if (high.has_value() && low.has_value())
            {
                decoded.push_back(static_cast<char>((*high << 4u) | *low));
                index += 2u;
                continue;
            }
        }

        decoded.push_back(uri[index]);
    }
    return decoded;
}

void AddUniqueCandidate(
    std::vector<std::filesystem::path>& candidates,
    std::filesystem::path candidate)
{
    candidate = candidate.lexically_normal();
    if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
        candidates.push_back(std::move(candidate));
}

std::vector<std::filesystem::path> ExternalSceneResourceCandidates(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& sourcePath,
    const std::string& uri)
{
    std::vector<std::filesystem::path> candidates;
    auto decodedUri = DecodeUriPercentEscapes(uri);
    // DCC exporters often write Windows separators for external asset references.
    std::replace(decodedUri.begin(), decodedUri.end(), '\\', '/');
    const auto uriPath = std::filesystem::path(decodedUri);
    if (uriPath.is_absolute())
    {
        AddUniqueCandidate(candidates, uriPath);
        return candidates;
    }

    AddUniqueCandidate(candidates, sourcePath.parent_path() / uriPath);
    if (!projectRoot.empty())
    {
        AddUniqueCandidate(candidates, projectRoot / uriPath);
        AddUniqueCandidate(candidates, projectRoot / "Assets" / uriPath);
    }
    return candidates;
}

bool IsInsideProjectRoot(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& path)
{
    if (projectRoot.empty())
        return true;

    const auto relative = path.lexically_normal().lexically_relative(projectRoot.lexically_normal());
    return !PathEscapesRoot(relative);
}

std::optional<std::filesystem::path> ResolveExternalSceneResourceFilePath(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& sourcePath,
    const std::string& uri)
{
    if (uri.empty() || IsEmbeddedUri(uri))
        return std::nullopt;

    for (const auto& candidate : ExternalSceneResourceCandidates(projectRoot, sourcePath, uri))
    {
        if (!IsInsideProjectRoot(projectRoot, candidate))
            continue;

        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error))
            return candidate;
    }
    return std::nullopt;
}

std::optional<NLS::Core::Assets::AssetDependencyRecord> MakeExternalSourceFileDependency(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& sourcePath,
    const std::string& uri)
{
    if (projectRoot.empty() || uri.empty() || IsEmbeddedUri(uri))
        return std::nullopt;

    const auto dependencyPath = ResolveExternalSceneResourceFilePath(projectRoot, sourcePath, uri);
    if (!dependencyPath.has_value())
        return std::nullopt;

    const auto normalizedRoot = projectRoot.lexically_normal();
    const auto editorPath = dependencyPath->lexically_relative(normalizedRoot);
    if (PathEscapesRoot(editorPath))
        return std::nullopt;

    const auto stamp = FileStamp(*dependencyPath);
    if (stamp.empty())
        return std::nullopt;

    return NLS::Core::Assets::AssetDependencyRecord {
        NLS::Core::Assets::AssetDependencyKind::SourceFileHash,
        editorPath.generic_string(),
        stamp
    };
}

void AddUniqueDependency(
    std::vector<NLS::Core::Assets::AssetDependencyRecord>& dependencies,
    NLS::Core::Assets::AssetDependencyRecord dependency)
{
    const auto exists = std::any_of(
        dependencies.begin(),
        dependencies.end(),
        [&dependency](const NLS::Core::Assets::AssetDependencyRecord& existing)
        {
            return existing.kind == dependency.kind &&
                existing.value == dependency.value &&
                existing.hashOrVersion == dependency.hashOrVersion;
        });
    if (!exists)
        dependencies.push_back(std::move(dependency));
}

std::optional<std::string> ReadTextureBuildIdentity(const std::vector<uint8_t>& payload)
{
    const auto texture = NLS::Render::Assets::DeserializeTextureArtifact(payload);
    if (!texture.has_value() || texture->buildIdentity.empty())
        return std::nullopt;
    return texture->buildIdentity;
}

void AddTextureBuildIdentityDependencies(
    std::vector<NLS::Core::Assets::AssetDependencyRecord>& dependencies,
    const std::vector<NLS::Core::Assets::ArtifactPayload>& payloads)
{
    std::vector<std::pair<std::string, std::string>> identities;
    for (const auto& payload : payloads)
    {
        if (payload.artifactType != NLS::Core::Assets::ArtifactType::Texture)
            continue;

        auto buildIdentity = ReadTextureBuildIdentity(payload.payload);
        if (!buildIdentity.has_value())
            continue;

        identities.emplace_back(payload.subAssetKey, std::move(*buildIdentity));
    }

    std::sort(
        identities.begin(),
        identities.end(),
        [](const auto& lhs, const auto& rhs)
        {
            return lhs.first < rhs.first;
        });

    for (const auto& [subAssetKey, buildIdentity] : identities)
    {
        AddUniqueDependency(dependencies, {
            NLS::Core::Assets::AssetDependencyKind::PostprocessorVersion,
            "texture-build:" + subAssetKey,
            buildIdentity
        });
    }
}

std::vector<NLS::Core::Assets::AssetDependencyRecord> CollectExternalSourceFileDependencies(
    const ExternalModelImportRequest& request,
    const NLS::Render::Assets::ImportedScene& scene)
{
    std::vector<NLS::Core::Assets::AssetDependencyRecord> dependencies;
    for (const auto& buffer : scene.buffers)
    {
        if (buffer.embedded)
            continue;

        auto dependency = MakeExternalSourceFileDependency(
            request.projectRoot,
            request.sourcePath,
            buffer.uri);
        if (dependency.has_value())
            AddUniqueDependency(dependencies, std::move(*dependency));
    }

    for (const auto& texture : scene.textures)
    {
        if (texture.embedded)
            continue;

        auto dependency = MakeExternalSourceFileDependency(
            request.projectRoot,
            request.sourcePath,
            texture.uri);
        if (dependency.has_value())
            AddUniqueDependency(dependencies, std::move(*dependency));
    }
    return dependencies;
}

std::optional<std::filesystem::path> ResolveExternalSceneResourcePath(
    const ExternalModelImportRequest& request,
    const std::string& uri)
{
    return ResolveExternalSceneResourceFilePath(
        request.projectRoot,
        request.sourcePath,
        uri);
}

struct GltfBufferViewData
{
    size_t bufferIndex = 0u;
    size_t byteOffset = 0u;
    size_t byteLength = 0u;
    size_t byteStride = 0u;
};

struct GltfAccessorData
{
    size_t bufferViewIndex = 0u;
    size_t byteOffset = 0u;
    uint32_t componentType = 0u;
    size_t count = 0u;
    std::string type;
};


size_t GltfComponentSize(const uint32_t componentType)
{
    switch (componentType)
    {
    case 5120u:
    case 5121u:
        return 1u;
    case 5122u:
    case 5123u:
        return 2u;
    case 5125u:
    case 5126u:
        return 4u;
    default:
        return 0u;
    }
}

size_t GltfComponentCount(const std::string& type)
{
    if (type == "SCALAR")
        return 1u;
    if (type == "VEC2")
        return 2u;
    if (type == "VEC3")
        return 3u;
    if (type == "VEC4")
        return 4u;
    if (type == "MAT2")
        return 4u;
    if (type == "MAT3")
        return 9u;
    if (type == "MAT4")
        return 16u;
    return 0u;
}

void LoadGltfBuffers(
    const ExternalModelImportRequest& request,
    const ImportedSceneJson& root,
    const std::vector<uint8_t>& glbBinaryChunk,
    std::vector<std::vector<uint8_t>>& buffers)
{
    buffers.clear();
    const auto* bufferArray = FindArray(root, "buffers");
    if (!bufferArray)
        return;

    buffers.reserve(bufferArray->size());
    for (size_t index = 0u; index < bufferArray->size(); ++index)
    {
        const auto& buffer = (*bufferArray)[index];
        if (!buffer.is_object())
        {
            buffers.push_back({});
            continue;
        }

        const auto uri = buffer.value("uri", std::string {});
        if (uri.empty())
        {
            buffers.push_back(index == 0u ? glbBinaryChunk : std::vector<uint8_t> {});
        }
        else if (IsDataUri(uri))
        {
            buffers.push_back(DecodeDataUri(uri));
        }
        else if (auto filePath = ResolveExternalSceneResourceFilePath(
            request.projectRoot,
            request.sourcePath,
            uri))
        {
            buffers.push_back(ReadBinaryFile(*filePath));
        }
        else
        {
            buffers.push_back({});
        }
    }
}

std::vector<GltfBufferViewData> ReadGltfBufferViews(const ImportedSceneJson& root)
{
    std::vector<GltfBufferViewData> views;
    const auto* bufferViews = FindArray(root, "bufferViews");
    if (!bufferViews)
        return views;

    views.reserve(bufferViews->size());
    for (const auto& view : *bufferViews)
    {
        GltfBufferViewData data;
        if (view.is_object())
        {
            data.bufferIndex = static_cast<size_t>(view.value("buffer", 0));
            data.byteOffset = static_cast<size_t>(view.value("byteOffset", 0u));
            data.byteLength = static_cast<size_t>(view.value("byteLength", 0u));
            data.byteStride = static_cast<size_t>(view.value("byteStride", 0u));
        }
        views.push_back(std::move(data));
    }
    return views;
}

std::vector<GltfAccessorData> ReadGltfAccessors(const ImportedSceneJson& root)
{
    std::vector<GltfAccessorData> accessors;
    const auto* accessorArray = FindArray(root, "accessors");
    if (!accessorArray)
        return accessors;

    accessors.reserve(accessorArray->size());
    for (const auto& accessor : *accessorArray)
    {
        GltfAccessorData data;
        if (accessor.is_object())
        {
            data.bufferViewIndex = static_cast<size_t>(accessor.value("bufferView", 0));
            data.byteOffset = static_cast<size_t>(accessor.value("byteOffset", 0u));
            data.componentType = static_cast<uint32_t>(accessor.value("componentType", 0u));
            data.count = static_cast<size_t>(accessor.value("count", 0u));
            data.type = accessor.value("type", std::string {});
        }
        accessors.push_back(std::move(data));
    }
    return accessors;
}

std::unordered_map<std::string, std::vector<uint8_t>> LoadGltfBufferViewPayloads(
    const ExternalModelImportRequest& request,
    const std::string& extension)
{
    std::string jsonText;
    std::vector<uint8_t> glbBinaryChunk;
    if (extension == ".gltf")
    {
        jsonText = ReadTextFile(request.sourcePath);
    }
    else if (extension == ".glb")
    {
        if (!ExtractGlbJsonAndBinary(ReadBinaryFile(request.sourcePath), jsonText, glbBinaryChunk))
            return {};
    }
    else
    {
        return {};
    }

    const auto root = ImportedSceneJson::parse(jsonText, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return {};

    std::vector<std::vector<uint8_t>> buffers;
    LoadGltfBuffers(request, root, glbBinaryChunk, buffers);
    const auto bufferViews = ReadGltfBufferViews(root);

    std::unordered_map<std::string, std::vector<uint8_t>> payloads;
    for (size_t index = 0u; index < bufferViews.size(); ++index)
    {
        const auto& view = bufferViews[index];
        if (view.bufferIndex >= buffers.size())
            continue;

        const auto& buffer = buffers[view.bufferIndex];
        if (view.byteOffset > buffer.size() || view.byteLength > buffer.size() - view.byteOffset)
            continue;

        payloads.emplace(
            "bufferView/" + std::to_string(index),
            std::vector<uint8_t>(
                buffer.begin() + static_cast<std::ptrdiff_t>(view.byteOffset),
                buffer.begin() + static_cast<std::ptrdiff_t>(view.byteOffset + view.byteLength)));
    }
    return payloads;
}

bool ShouldImportGltfVertexSemantic(
    const NLS::Render::Assets::SceneImportSettings& settings,
    const char* semantic)
{
    const std::string_view value(semantic);
    if (!settings.importNormals && value == "NORMAL")
        return false;
    if (!settings.importTangents && value == "TANGENT")
        return false;
    if (!settings.importUvs && value.rfind("TEXCOORD", 0u) == 0u)
        return false;
    return true;
}

std::optional<NLS::Render::Assets::MeshArtifactData> BuildGltfPrimitiveMesh(
    const ImportedSceneJson& primitive,
    const std::vector<std::vector<uint8_t>>& buffers,
    const std::vector<GltfBufferViewData>& bufferViews,
    const std::vector<GltfAccessorData>& accessors)
{
    const auto* attributes = FindObject(primitive, "attributes");
    if (!attributes)
        return std::nullopt;

    const auto positionAccessorValue = attributes->find("POSITION");
    if (positionAccessorValue == attributes->end() || !positionAccessorValue->is_number_integer())
        return std::nullopt;

    const auto positionAccessorIndex = positionAccessorValue->get<size_t>();
    if (positionAccessorIndex >= accessors.size())
        return std::nullopt;

    const auto& positionAccessor = accessors[positionAccessorIndex];
    if (positionAccessor.componentType != 5126u ||
        positionAccessor.type != "VEC3" ||
        positionAccessor.bufferViewIndex >= bufferViews.size())
    {
        return std::nullopt;
    }

    const auto& positionView = bufferViews[positionAccessor.bufferViewIndex];
    if (positionView.bufferIndex >= buffers.size())
        return std::nullopt;
    const auto& positionBuffer = buffers[positionView.bufferIndex];
        const auto positionStride = positionView.byteStride != 0u
            ? positionView.byteStride
            : GltfComponentSize(positionAccessor.componentType) * GltfComponentCount(positionAccessor.type);
        const auto positionBase = positionView.byteOffset + positionAccessor.byteOffset;
    if (positionStride < 12u ||
        (positionAccessor.count > 0u &&
            positionBase + positionStride * (positionAccessor.count - 1u) + 12u > positionBuffer.size()))
    {
        return std::nullopt;
    }

    NLS::Render::Assets::MeshArtifactData mesh;
    mesh.vertices.reserve(positionAccessor.count);
    for (size_t vertexIndex = 0u; vertexIndex < positionAccessor.count; ++vertexIndex)
    {
        const auto offset = positionBase + vertexIndex * positionStride;
        if (offset + 12u > positionBuffer.size())
            return std::nullopt;

        NLS::Render::Geometry::Vertex vertex {};
        vertex.position[0] = ReadFloat32(positionBuffer, offset);
        vertex.position[1] = ReadFloat32(positionBuffer, offset + 4u);
        vertex.position[2] = ReadFloat32(positionBuffer, offset + 8u);
        mesh.vertices.push_back(vertex);
    }

    const auto indexAccessorValue = primitive.find("indices");
    if (indexAccessorValue != primitive.end() && indexAccessorValue->is_number_integer())
    {
        const auto indexAccessorIndex = indexAccessorValue->get<size_t>();
        if (indexAccessorIndex >= accessors.size())
            return std::nullopt;

        const auto& indexAccessor = accessors[indexAccessorIndex];
        if (indexAccessor.type != "SCALAR" || indexAccessor.bufferViewIndex >= bufferViews.size())
            return std::nullopt;

        const auto& indexView = bufferViews[indexAccessor.bufferViewIndex];
        if (indexView.bufferIndex >= buffers.size())
            return std::nullopt;
        const auto& indexBuffer = buffers[indexView.bufferIndex];
        const auto indexStride = indexView.byteStride != 0u
            ? indexView.byteStride
            : GltfComponentSize(indexAccessor.componentType);
        const auto indexBase = indexView.byteOffset + indexAccessor.byteOffset;
        const auto indexComponentSize = GltfComponentSize(indexAccessor.componentType);
        if (indexStride == 0u ||
            indexComponentSize == 0u ||
            (indexAccessor.count > 0u &&
                indexBase + indexStride * (indexAccessor.count - 1u) + indexComponentSize > indexBuffer.size()))
        {
            return std::nullopt;
        }

        mesh.indices.reserve(indexAccessor.count);
        for (size_t index = 0u; index < indexAccessor.count; ++index)
        {
            const auto offset = indexBase + index * indexStride;
            if (offset + indexComponentSize > indexBuffer.size())
                return std::nullopt;
            mesh.indices.push_back(ReadUnsignedIndex(indexBuffer, offset, indexAccessor.componentType));
        }
    }
    else
    {
        mesh.indices.reserve(mesh.vertices.size());
        for (uint32_t index = 0u; index < mesh.vertices.size(); ++index)
            mesh.indices.push_back(index);
    }

    const auto materialIndex = primitive.value("material", -1);
    if (materialIndex >= 0)
        mesh.materialIndex = static_cast<uint32_t>(materialIndex);
    return mesh;
}

void CopyGltfVec2VertexStream(
    const ImportedSceneJson& primitive,
    const char* semantic,
    std::vector<NLS::Render::Geometry::Vertex>& vertices,
    float (NLS::Render::Geometry::Vertex::* target)[2],
    const std::vector<std::vector<uint8_t>>& buffers,
    const std::vector<GltfBufferViewData>& bufferViews,
    const std::vector<GltfAccessorData>& accessors)
{
    const auto* attributes = FindObject(primitive, "attributes");
    if (!attributes)
        return;

    const auto accessorValue = attributes->find(semantic);
    if (accessorValue == attributes->end() || !accessorValue->is_number_integer())
        return;

    const auto accessorIndex = accessorValue->get<size_t>();
    if (accessorIndex >= accessors.size())
        return;

    const auto& accessor = accessors[accessorIndex];
    if (accessor.componentType != 5126u || accessor.type != "VEC2" || accessor.bufferViewIndex >= bufferViews.size())
        return;

    const auto& view = bufferViews[accessor.bufferViewIndex];
    if (view.bufferIndex >= buffers.size())
        return;

    const auto& buffer = buffers[view.bufferIndex];
    const auto stride = view.byteStride != 0u ? view.byteStride : 8u;
    const auto base = view.byteOffset + accessor.byteOffset;
    const auto count = std::min(accessor.count, vertices.size());
    for (size_t vertexIndex = 0u; vertexIndex < count; ++vertexIndex)
    {
        const auto offset = base + vertexIndex * stride;
        if (offset + 8u > buffer.size())
            return;
        auto& values = vertices[vertexIndex].*target;
        values[0] = ReadFloat32(buffer, offset);
        values[1] = ReadFloat32(buffer, offset + 4u);
    }
}

void CopyGltfVec3VertexStream(
    const ImportedSceneJson& primitive,
    const char* semantic,
    std::vector<NLS::Render::Geometry::Vertex>& vertices,
    float (NLS::Render::Geometry::Vertex::* target)[3],
    const std::vector<std::vector<uint8_t>>& buffers,
    const std::vector<GltfBufferViewData>& bufferViews,
    const std::vector<GltfAccessorData>& accessors)
{
    const auto* attributes = FindObject(primitive, "attributes");
    if (!attributes)
        return;

    const auto accessorValue = attributes->find(semantic);
    if (accessorValue == attributes->end() || !accessorValue->is_number_integer())
        return;

    const auto accessorIndex = accessorValue->get<size_t>();
    if (accessorIndex >= accessors.size())
        return;

    const auto& accessor = accessors[accessorIndex];
    if (accessor.componentType != 5126u || accessor.type != "VEC3" || accessor.bufferViewIndex >= bufferViews.size())
        return;

    const auto& view = bufferViews[accessor.bufferViewIndex];
    if (view.bufferIndex >= buffers.size())
        return;

    const auto& buffer = buffers[view.bufferIndex];
    const auto stride = view.byteStride != 0u ? view.byteStride : 12u;
    const auto base = view.byteOffset + accessor.byteOffset;
    const auto count = std::min(accessor.count, vertices.size());
    for (size_t vertexIndex = 0u; vertexIndex < count; ++vertexIndex)
    {
        const auto offset = base + vertexIndex * stride;
        if (offset + 12u > buffer.size())
            return;
        auto& values = vertices[vertexIndex].*target;
        values[0] = ReadFloat32(buffer, offset);
        values[1] = ReadFloat32(buffer, offset + 4u);
        values[2] = ReadFloat32(buffer, offset + 8u);
    }
}

void CopyGltfVec4TangentStream(
    const ImportedSceneJson& primitive,
    std::vector<NLS::Render::Geometry::Vertex>& vertices,
    const std::vector<std::vector<uint8_t>>& buffers,
    const std::vector<GltfBufferViewData>& bufferViews,
    const std::vector<GltfAccessorData>& accessors)
{
    const auto* attributes = FindObject(primitive, "attributes");
    if (!attributes)
        return;

    const auto accessorValue = attributes->find("TANGENT");
    if (accessorValue == attributes->end() || !accessorValue->is_number_integer())
        return;

    const auto accessorIndex = accessorValue->get<size_t>();
    if (accessorIndex >= accessors.size())
        return;

    const auto& accessor = accessors[accessorIndex];
    if (accessor.componentType != 5126u || accessor.type != "VEC4" || accessor.bufferViewIndex >= bufferViews.size())
        return;

    const auto& view = bufferViews[accessor.bufferViewIndex];
    if (view.bufferIndex >= buffers.size())
        return;

    const auto& buffer = buffers[view.bufferIndex];
    const auto stride = view.byteStride != 0u ? view.byteStride : 16u;
    const auto base = view.byteOffset + accessor.byteOffset;
    const auto count = std::min(accessor.count, vertices.size());
    for (size_t vertexIndex = 0u; vertexIndex < count; ++vertexIndex)
    {
        const auto offset = base + vertexIndex * stride;
        if (offset + 16u > buffer.size())
            return;

        auto& vertex = vertices[vertexIndex];
        vertex.tangent[0] = ReadFloat32(buffer, offset);
        vertex.tangent[1] = ReadFloat32(buffer, offset + 4u);
        vertex.tangent[2] = ReadFloat32(buffer, offset + 8u);
        const auto handedness = ReadFloat32(buffer, offset + 12u);
        vertex.bitangent[0] = vertex.normals[1] * vertex.tangent[2] - vertex.normals[2] * vertex.tangent[1];
        vertex.bitangent[1] = vertex.normals[2] * vertex.tangent[0] - vertex.normals[0] * vertex.tangent[2];
        vertex.bitangent[2] = vertex.normals[0] * vertex.tangent[1] - vertex.normals[1] * vertex.tangent[0];
        vertex.bitangent[0] *= handedness;
        vertex.bitangent[1] *= handedness;
        vertex.bitangent[2] *= handedness;
    }
}

void PopulateGltfOptionalVertexStreams(
    const ImportedSceneJson& primitive,
    NLS::Render::Assets::MeshArtifactData& mesh,
    const NLS::Render::Assets::SceneImportSettings& settings,
    const std::vector<std::vector<uint8_t>>& buffers,
    const std::vector<GltfBufferViewData>& bufferViews,
    const std::vector<GltfAccessorData>& accessors)
{
    if (ShouldImportGltfVertexSemantic(settings, "TEXCOORD_0"))
        CopyGltfVec2VertexStream(primitive, "TEXCOORD_0", mesh.vertices, &NLS::Render::Geometry::Vertex::texCoords, buffers, bufferViews, accessors);
    if (ShouldImportGltfVertexSemantic(settings, "NORMAL"))
        CopyGltfVec3VertexStream(primitive, "NORMAL", mesh.vertices, &NLS::Render::Geometry::Vertex::normals, buffers, bufferViews, accessors);
    if (ShouldImportGltfVertexSemantic(settings, "TANGENT"))
        CopyGltfVec4TangentStream(primitive, mesh.vertices, buffers, bufferViews, accessors);
}

std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> LoadGltfSourceMeshData(
    const ExternalModelImportRequest& request,
    const std::string& extension)
{
    std::string jsonText;
    std::vector<uint8_t> glbBinaryChunk;
    if (extension == ".gltf")
    {
        jsonText = ReadTextFile(request.sourcePath);
    }
    else if (extension == ".glb")
    {
        if (!ExtractGlbJsonAndBinary(ReadBinaryFile(request.sourcePath), jsonText, glbBinaryChunk))
            return {};
    }
    else
    {
        return {};
    }

    const auto root = ImportedSceneJson::parse(jsonText, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return {};

    std::vector<std::vector<uint8_t>> buffers;
    LoadGltfBuffers(request, root, glbBinaryChunk, buffers);
    const auto bufferViews = ReadGltfBufferViews(root);
    const auto accessors = ReadGltfAccessors(root);
    const auto importSettings = ToSceneImportSettings(request.meta.settings);

    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    const auto* meshArray = FindArray(root, "meshes");
    if (!meshArray)
        return meshes;

    for (size_t meshIndex = 0u; meshIndex < meshArray->size(); ++meshIndex)
    {
        const auto& mesh = (*meshArray)[meshIndex];
        if (!mesh.is_object())
            continue;

        const auto* primitives = FindArray(mesh, "primitives");
        if (!primitives)
            continue;

        const bool hasMultiplePrimitives = primitives->size() > 1u;
        size_t primitiveIndex = 0u;
        for (const auto& primitive : *primitives)
        {
            if (!primitive.is_object())
            {
                ++primitiveIndex;
                continue;
            }

            auto primitiveMesh = BuildGltfPrimitiveMesh(primitive, buffers, bufferViews, accessors);
            if (!primitiveMesh.has_value())
            {
                ++primitiveIndex;
                continue;
            }
            PopulateGltfOptionalVertexStreams(
                primitive,
                *primitiveMesh,
                importSettings,
                buffers,
                bufferViews,
                accessors);

            NLS::Render::Resources::Parsers::ParsedMeshData parsedMesh;
            parsedMesh.vertices = std::move(primitiveMesh->vertices);
            parsedMesh.indices = std::move(primitiveMesh->indices);
            parsedMesh.materialIndex = primitiveMesh->materialIndex;
            parsedMesh.sourceMeshIndex = static_cast<uint32_t>(meshIndex);
            parsedMesh.sourceKey = "mesh/" + std::to_string(meshIndex);
            if (hasMultiplePrimitives)
                parsedMesh.sourceKey = NLS::Render::Assets::BuildPrimitiveMeshSourceKey(
                    parsedMesh.sourceKey,
                    primitiveIndex);
            meshes.push_back(std::move(parsedMesh));
            ++primitiveIndex;
        }
    }

    return meshes;
}

std::string Trim(const std::string& value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1u);
}

std::vector<std::string> PathTokens(std::string value)
{
    value = Trim(std::move(value));
    std::vector<std::string> tokens;
    size_t offset = 0u;
    while (offset < value.size())
    {
        while (offset < value.size() && std::isspace(static_cast<unsigned char>(value[offset])))
            ++offset;
        if (offset >= value.size())
            break;

        if (value[offset] == '"' || value[offset] == '\'')
        {
            const auto quote = value[offset++];
            const auto closing = value.find(quote, offset);
            if (closing == std::string::npos)
            {
                tokens.push_back(value.substr(offset));
                break;
            }

            tokens.push_back(value.substr(offset, closing - offset));
            offset = closing + 1u;
            continue;
        }

        const auto end = value.find_first_of(" \t\r\n", offset);
        if (end == std::string::npos)
        {
            tokens.push_back(value.substr(offset));
            break;
        }

        tokens.push_back(value.substr(offset, end - offset));
        offset = end + 1u;
    }
    return tokens;
}

void AddObjDependencyUri(
    const ExternalModelImportRequest& request,
    std::vector<NLS::Core::Assets::AssetDependencyRecord>& dependencies,
    const std::string& uri)
{
    auto dependency = MakeExternalSourceFileDependency(request.projectRoot, request.sourcePath, uri);
    if (dependency.has_value())
        AddUniqueDependency(dependencies, std::move(*dependency));
}

void AddMtlTextureDependencyUri(
    const ExternalModelImportRequest& request,
    std::vector<NLS::Core::Assets::AssetDependencyRecord>& dependencies,
    const std::filesystem::path& mtlPath,
    const std::string& uri)
{
    if (uri.empty() || IsEmbeddedUri(uri) || request.projectRoot.empty())
        return;

    const auto uriPath = std::filesystem::path(uri);
    const auto dependencyPath = (uriPath.is_absolute()
        ? uriPath
        : mtlPath.parent_path() / uriPath).lexically_normal();
    const auto editorPath = dependencyPath.lexically_relative(request.projectRoot.lexically_normal());
    if (PathEscapesRoot(editorPath))
        return;

    const auto stamp = FileStamp(dependencyPath);
    if (stamp.empty())
        return;

    AddUniqueDependency(dependencies, {
        NLS::Core::Assets::AssetDependencyKind::SourceFileHash,
        editorPath.generic_string(),
        stamp
    });
}

void CollectObjMaterialFileDependencies(
    const ExternalModelImportRequest& request,
    std::vector<NLS::Core::Assets::AssetDependencyRecord>& dependencies)
{
    if (ToLower(request.sourcePath.extension().string()) != ".obj")
        return;

    std::ifstream objInput(request.sourcePath, std::ios::binary);
    if (!objInput)
        return;

    std::set<std::filesystem::path> mtlPaths;
    std::string line;
    while (std::getline(objInput, line))
    {
        const auto trimmed = Trim(line);
        if (trimmed.rfind("mtllib", 0u) != 0u ||
            (trimmed.size() > 6u && !std::isspace(static_cast<unsigned char>(trimmed[6u]))))
        {
            continue;
        }

        for (const auto& mtlUri : PathTokens(trimmed.substr(6u)))
        {
            if (mtlUri.empty())
                continue;

            AddObjDependencyUri(request, dependencies, mtlUri);
            const auto mtlPath = (std::filesystem::path(mtlUri).is_absolute()
                ? std::filesystem::path(mtlUri)
                : request.sourcePath.parent_path() / std::filesystem::path(mtlUri)).lexically_normal();
            mtlPaths.insert(mtlPath);
        }
    }

    for (const auto& mtlPath : mtlPaths)
    {
        std::ifstream mtlInput(mtlPath, std::ios::binary);
        if (!mtlInput)
            continue;

        while (std::getline(mtlInput, line))
        {
            const auto trimmed = Trim(line);
            if (trimmed.rfind("map_", 0u) != 0u)
                continue;

            const auto space = trimmed.find_first_of(" \t");
            if (space == std::string::npos)
                continue;

            const auto tokens = PathTokens(trimmed.substr(space + 1u));
            if (!tokens.empty())
            {
                AddMtlTextureDependencyUri(
                    request,
                    dependencies,
                    mtlPath,
                    tokens.back());
            }
        }
    }
}

void CollectParserExternalFileDependencies(
    const ExternalModelImportRequest& request,
    std::vector<NLS::Core::Assets::AssetDependencyRecord>& dependencies,
    const std::vector<std::string>& externalDependencies)
{
    for (const auto& uri : externalDependencies)
    {
        auto dependency = MakeExternalSourceFileDependency(
            request.projectRoot,
            request.sourcePath,
            uri);
        if (dependency.has_value())
            AddUniqueDependency(dependencies, std::move(*dependency));
    }
}

void ReportProgress(
    const ExternalModelImportRequest& request,
    const ImportPhase phase,
    const double progress,
    std::string message)
{
    if (!request.progressTracker || !request.progressJob.IsValid())
        return;

    request.progressTracker->ReportProgress(
        request.progressJob,
        phase,
        progress,
        std::move(message));
}

NLS::Core::Assets::ArtifactType ToArtifactType(const NLS::Render::Assets::ImportedSceneSubAssetType type)
{
    using NLS::Core::Assets::ArtifactType;
    using NLS::Render::Assets::ImportedSceneSubAssetType;

    switch (type)
    {
    case ImportedSceneSubAssetType::Mesh: return ArtifactType::Mesh;
    case ImportedSceneSubAssetType::Material: return ArtifactType::Material;
    case ImportedSceneSubAssetType::Texture: return ArtifactType::Texture;
    case ImportedSceneSubAssetType::Skeleton: return ArtifactType::Skeleton;
    case ImportedSceneSubAssetType::Skin: return ArtifactType::Skin;
    case ImportedSceneSubAssetType::AnimationClip: return ArtifactType::AnimationClip;
    case ImportedSceneSubAssetType::MorphTarget: return ArtifactType::MorphTarget;
    case ImportedSceneSubAssetType::Model: return ArtifactType::Model;
    case ImportedSceneSubAssetType::Prefab: return ArtifactType::Prefab;
    default: return ArtifactType::Unknown;
    }
}

std::string LoaderIdFor(const NLS::Render::Assets::ImportedSceneSubAssetType type)
{
    return NLS::Render::Assets::ToSubAssetPrefix(type);
}

std::filesystem::path RelativePathFor(const NLS::Render::Assets::GeneratedSceneSubAsset& subAsset)
{
    using NLS::Render::Assets::ImportedSceneSubAssetType;

    switch (subAsset.type)
    {
    case ImportedSceneSubAssetType::Mesh:
        return std::filesystem::path("meshes") / (EncodeArtifactFileName(subAsset.key) + ".nmesh");
    case ImportedSceneSubAssetType::Material:
        return std::filesystem::path("materials") / (EncodeArtifactFileName(subAsset.key) + ".nmat");
    case ImportedSceneSubAssetType::Texture:
        return std::filesystem::path("textures") / (EncodeArtifactFileName(subAsset.key) + ".ntex");
    case ImportedSceneSubAssetType::Skeleton:
        return std::filesystem::path("skeletons") / (EncodeArtifactFileName(subAsset.key) + ".nskel");
    case ImportedSceneSubAssetType::Skin:
        return std::filesystem::path("skins") / (EncodeArtifactFileName(subAsset.key) + ".nskin");
    case ImportedSceneSubAssetType::AnimationClip:
        return std::filesystem::path("animations") / (EncodeArtifactFileName(subAsset.key) + ".nanim");
    case ImportedSceneSubAssetType::MorphTarget:
        return std::filesystem::path("morphs") / (EncodeArtifactFileName(subAsset.key) + ".nmorph");
    case ImportedSceneSubAssetType::Prefab:
        return "prefab.nprefab";
    default:
        return std::filesystem::path("assets") / (EncodeArtifactFileName(subAsset.key) + ".nasset");
    }
}

std::unordered_map<std::string, std::filesystem::path> BuildTextureArtifactPathMap(
    const std::vector<NLS::Render::Assets::GeneratedSceneSubAsset>& subAssets,
    const std::filesystem::path& committedRoot,
    const std::filesystem::path& projectRoot)
{
    std::unordered_map<std::string, std::filesystem::path> paths;
    for (const auto& subAsset : subAssets)
    {
        if (subAsset.type != NLS::Render::Assets::ImportedSceneSubAssetType::Texture ||
            subAsset.sourceKey.empty())
        {
            continue;
        }

        auto artifactPath = (committedRoot / RelativePathFor(subAsset)).lexically_normal();
        if (!projectRoot.empty())
        {
            const auto projectRelative = artifactPath.lexically_relative(projectRoot.lexically_normal());
            if (!PathEscapesRoot(projectRelative))
                artifactPath = projectRelative;
        }

        paths.emplace(
            subAsset.sourceKey,
            artifactPath);
    }
    return paths;
}

std::string SerializeGenericSubAsset(
    const NLS::Render::Assets::ImportedScene& scene,
    const NLS::Render::Assets::GeneratedSceneSubAsset& subAsset)
{
    std::ostringstream stream;
    stream << "NULLUS_IMPORTED_SCENE_ARTIFACT=1\n";
    stream << "SCENE=" << scene.sceneKey << '\n';
    stream << "SOURCE_ASSET=" << scene.sourceAssetId.ToString() << '\n';
    stream << "SUB_ASSET_KEY=" << subAsset.key << '\n';
    stream << "SOURCE_KEY=" << subAsset.sourceKey << '\n';
    stream << "DISPLAY_NAME=" << subAsset.displayName << '\n';
    stream << "TYPE=" << LoaderIdFor(subAsset.type) << '\n';
    return stream.str();
}

const NLS::Render::Assets::ImportedSceneNamedRecord* FindTextureRecord(
    const NLS::Render::Assets::ImportedScene& scene,
    const std::string& sourceKey)
{
    const auto found = std::find_if(
        scene.textures.begin(),
        scene.textures.end(),
        [&sourceKey](const NLS::Render::Assets::ImportedSceneNamedRecord& texture)
        {
            return texture.sourceKey == sourceKey;
        });
    return found != scene.textures.end() ? &*found : nullptr;
}

std::unordered_map<std::string, std::vector<uint8_t>> LoadTexturePayloads(
    const ExternalModelImportRequest& request,
    const NLS::Render::Assets::ImportedScene& scene,
    const std::string& extension)
{
    const auto needsGltfBufferViews = std::any_of(
        scene.textures.begin(),
        scene.textures.end(),
        [](const NLS::Render::Assets::ImportedSceneNamedRecord& texture)
        {
            return !texture.bufferViewKey.empty();
        });
    const auto bufferViewPayloads = needsGltfBufferViews
        ? LoadGltfBufferViewPayloads(request, extension)
        : std::unordered_map<std::string, std::vector<uint8_t>> {};

    std::unordered_map<std::string, std::vector<uint8_t>> payloads;
    for (const auto& texture : scene.textures)
    {
        if (texture.sourceKey.empty())
            continue;

        std::vector<uint8_t> bytes;
        if (IsDataUri(texture.uri))
        {
            bytes = DecodeDataUri(texture.uri);
        }
        else if (!texture.bufferViewKey.empty())
        {
            const auto found = bufferViewPayloads.find(texture.bufferViewKey);
            if (found != bufferViewPayloads.end())
                bytes = found->second;
        }
        else if (auto filePath = ResolveExternalSceneResourcePath(request, texture.uri))
        {
            bytes = ReadBinaryFile(*filePath);
        }

        payloads.emplace(texture.sourceKey, std::move(bytes));
    }
    return payloads;
}

NLS::Render::Assets::TextureMipIntent ResolveSceneTextureMipIntent(
    const NLS::Render::Assets::ImportedScene& scene,
    const std::string& textureKey)
{
    if (textureKey.empty())
        return NLS::Render::Assets::TextureMipIntent::Color;

    for (const auto& material : scene.materials)
    {
        if (material.normalTextureKey == textureKey)
            return NLS::Render::Assets::TextureMipIntent::Normal;
        if (material.metallicRoughnessTextureKey == textureKey ||
            material.occlusionTextureKey == textureKey)
        {
            return NLS::Render::Assets::TextureMipIntent::Mask;
        }

        for (const auto& channel : material.materialChannels)
        {
            if (channel.textureKey != textureKey)
                continue;

            const auto channelName = ToLower(channel.name);
            if (channelName.find("normal") != std::string::npos ||
                channelName.find("bump") != std::string::npos)
            {
                return NLS::Render::Assets::TextureMipIntent::Normal;
            }
            if (channelName.find("metal") != std::string::npos ||
                channelName.find("rough") != std::string::npos ||
                channelName.find("occlusion") != std::string::npos ||
                channelName.find("mask") != std::string::npos)
            {
                return NLS::Render::Assets::TextureMipIntent::Mask;
            }
        }
    }

    return NLS::Render::Assets::TextureMipIntent::Color;
}

std::string TextureIntentName(const NLS::Render::Assets::TextureMipIntent intent)
{
    switch (intent)
    {
    case NLS::Render::Assets::TextureMipIntent::Normal: return "normal";
    case NLS::Render::Assets::TextureMipIntent::Mask: return "mask";
    case NLS::Render::Assets::TextureMipIntent::UI: return "ui";
    case NLS::Render::Assets::TextureMipIntent::HDR: return "hdr";
    case NLS::Render::Assets::TextureMipIntent::Color:
    default:
        return "default";
    }
}

const char* TextureFormatName(const NLS::Render::RHI::TextureFormat format)
{
    return NLS::Render::RHI::GetTextureFormatName(format);
}

NLS::Render::Assets::TextureImportSettingsSnapshot BuildTextureImportSettingsSnapshot(
    const NLS::Render::Assets::TextureMipGeneratorSettings& mipSettings)
{
    NLS::Render::Assets::TextureImportSettingsSnapshot settings;
    settings.textureType = TextureIntentName(mipSettings.intent);
    settings.srgbTexture = mipSettings.colorSpace == NLS::Render::Assets::TextureArtifactColorSpace::Srgb;
    settings.mipmapEnabled = mipSettings.mipmapEnabled;
    settings.maxTextureSize = 0u;
    settings.resizePolicy = "keep";
    settings.compressionIntent = "default";
    return settings;
}

std::string BuildTextureSettingsIdentity(const NLS::Render::Assets::TextureImportSettingsSnapshot& settings)
{
    std::ostringstream stream;
    stream
        << "type=" << settings.textureType
        << "|srgb=" << (settings.srgbTexture ? 1u : 0u)
        << "|alphaTransparency=" << (settings.alphaIsTransparency ? 1u : 0u)
        << "|mips=" << (settings.mipmapEnabled ? 1u : 0u)
        << "|maxSize=" << settings.maxTextureSize
        << "|resize=" << settings.resizePolicy
        << "|compression=" << settings.compressionIntent
        << "|explicit=" << settings.explicitFormat;
    return stream.str();
}

std::string BuildDirectXTexEncoderOptionsHash(const NLS::Render::Assets::TextureBuildSettings& settings)
{
    std::vector<std::string> options;
    options.push_back("parallel");
    options.push_back(settings.colorSpace == NLS::Render::RHI::TextureColorSpace::SRGB ? "srgb" : "linear");

    if (settings.resolvedFormat == NLS::Render::RHI::TextureFormat::BC5 ||
        settings.textureIntent == "normal" ||
        settings.textureIntent == "mask")
    {
        options.push_back("uniform");
    }

    std::ostringstream stream;
    stream << "directxtex:";
    for (size_t index = 0u; index < options.size(); ++index)
    {
        if (index > 0u)
            stream << ",";
        stream << options[index];
    }
    return stream.str();
}

std::string TextureSourceContentHash(
    const ExternalModelImportRequest& request,
    const NLS::Render::Assets::TextureSourceDescriptor& source,
    const std::vector<uint8_t>& encodedBytes)
{
    if (auto path = ResolveExternalSceneResourceFilePath(request.projectRoot, request.sourcePath, source.assetPath))
    {
        const auto stamp = FileStamp(*path);
        if (!stamp.empty())
            return stamp;
    }

    return NLS::Core::Assets::ComputeNativeArtifactPayloadHash(encodedBytes);
}

std::string TextureSourceIdentity(
    const ExternalModelImportRequest& request,
    const std::string& textureKey,
    const NLS::Render::Assets::TextureSourceDescriptor& source)
{
    std::ostringstream stream;
    stream
        << request.meta.id.ToString()
        << "|" << textureKey
        << "|" << source.assetPath;
    return stream.str();
}

bool ValidateDecodedTextureSafetyLimits(
    const NLS::Render::Assets::TextureArtifactData& artifact,
    NLS::Core::Assets::AssetDiagnostics& diagnostics,
    const NLS::Core::Assets::AssetId assetId,
    const std::filesystem::path& sourcePath,
    const std::string& textureKey,
    const std::string& textureUri)
{
    const uint64_t pixelCount = static_cast<uint64_t>(artifact.width) * static_cast<uint64_t>(artifact.height);
    if (artifact.width <= kExternalTextureSafetyMaxDimension &&
        artifact.height <= kExternalTextureSafetyMaxDimension &&
        pixelCount <= kExternalTextureSafetyMaxPixels)
    {
        return true;
    }

    AddError(
        diagnostics,
        assetId,
        sourcePath,
        "external-model-importer-texture-safety-limit",
        "Texture " +
            textureKey +
            " uri=" +
            textureUri +
            " dimensions " +
            std::to_string(artifact.width) +
            "x" +
            std::to_string(artifact.height) +
            " exceed external model import safety limits.");
    return false;
}

bool TextureArtifactHasAlpha(const NLS::Render::Assets::TextureArtifactData& artifact)
{
    if (artifact.format != NLS::Render::RHI::TextureFormat::RGBA8 || artifact.mips.empty())
        return true;

    const auto& pixels = artifact.mips.front().pixels;
    for (size_t alphaIndex = 3u; alphaIndex < pixels.size(); alphaIndex += 4u)
    {
        if (pixels[alphaIndex] != 255u)
            return true;
    }
    return false;
}

NLS::Render::Assets::TextureSourceDescriptor BuildTextureSourceDescriptor(
    const NLS::Render::Assets::ImportedScene& scene,
    const std::string& textureKey,
    const NLS::Render::Assets::TextureArtifactData& artifact)
{
    NLS::Render::Assets::TextureSourceDescriptor source;
    source.width = artifact.width;
    source.height = artifact.height;
    source.hasAlpha = TextureArtifactHasAlpha(artifact);
    source.isHDR = artifact.format == NLS::Render::RHI::TextureFormat::RGBA16F;

    const auto found = std::find_if(
        scene.textures.begin(),
        scene.textures.end(),
        [&textureKey](const NLS::Render::Assets::ImportedSceneNamedRecord& texture)
        {
            return texture.sourceKey == textureKey;
        });
    source.assetPath = found != scene.textures.end() ? found->uri : textureKey;
    return source;
}

NLS::Render::Assets::TextureBackendCapabilities BuildExternalTexturePassthroughCapabilities(
    std::string targetPlatform)
{
    NLS::Render::Assets::TextureBackendCapabilities capabilities;
    capabilities.targetPlatform = std::move(targetPlatform);
    NLS::Render::RHI::TextureFormatCapability rgba8;
    rgba8.format = NLS::Render::RHI::TextureFormat::RGBA8;
    rgba8.sampled = true;
    rgba8.upload = true;
    rgba8.supportsSrgbView = true;

    NLS::Render::RHI::TextureFormatCapability rgba16f;
    rgba16f.format = NLS::Render::RHI::TextureFormat::RGBA16F;
    rgba16f.sampled = true;
    rgba16f.upload = true;

    capabilities.supportedFormats = {
        {NLS::Render::RHI::TextureFormat::RGBA8, rgba8},
        {NLS::Render::RHI::TextureFormat::RGBA16F, rgba16f}
    };
    return capabilities;
}

NLS::Render::Assets::TextureBackendCapabilities BuildExternalTextureDirectXCapabilities()
{
    auto capabilities = BuildExternalTexturePassthroughCapabilities("win64-dx12");

    const auto addCompressed = [&capabilities](const NLS::Render::RHI::TextureFormat format, const bool supportsSrgbView)
    {
        NLS::Render::RHI::TextureFormatCapability capability;
        capability.format = format;
        capability.sampled = true;
        capability.upload = true;
        capability.supportsSrgbView = supportsSrgbView;
        capability.supportsUnalignedBlockTextures = true;
        capabilities.supportedFormats[format] = capability;
    };

    addCompressed(NLS::Render::RHI::TextureFormat::BC1, true);
    addCompressed(NLS::Render::RHI::TextureFormat::BC3, true);
    addCompressed(NLS::Render::RHI::TextureFormat::BC5, false);
    addCompressed(NLS::Render::RHI::TextureFormat::BC7, true);
    return capabilities;
}

std::string ResolveExternalTextureBuildTargetPlatform(const std::string& artifactTargetPlatform)
{
    if (artifactTargetPlatform == "editor")
    {
#if defined(_WIN32)
        return "win64-dx12";
#else
        return artifactTargetPlatform;
#endif
    }

    return artifactTargetPlatform;
}

void AppendTextureFormatResolverDiagnostics(
    NLS::Core::Assets::AssetDiagnostics& diagnostics,
    const NLS::Core::Assets::AssetId assetId,
    const std::filesystem::path& sourcePath,
    const std::string& textureKey,
    const std::vector<NLS::Render::Assets::TextureBuildDiagnostic>& resolverDiagnostics)
{
    for (const auto& diagnostic : resolverDiagnostics)
    {
        AddWarning(
            diagnostics,
            assetId,
            sourcePath,
            "external-model-importer-texture-format-resolution",
            "Texture " +
                textureKey +
                " format resolution for " +
                diagnostic.targetPlatform +
                " requested " +
                TextureFormatName(diagnostic.requestedFormat) +
                " resolved " +
                TextureFormatName(diagnostic.resolvedFormat) +
                ": " +
                diagnostic.reason +
                " source=" +
                diagnostic.assetPath);
    }
}

std::vector<uint8_t> SerializeTextureSubAsset(
    const NLS::Render::Assets::ImportedScene& scene,
    const NLS::Render::Assets::GeneratedSceneSubAsset& subAsset,
    const std::unordered_map<std::string, NLS::Render::Assets::TextureArtifactColorSpace>& textureColorSpaces,
    std::unordered_map<std::string, std::vector<uint8_t>>& texturePayloads,
    NLS::Core::Assets::AssetDiagnostics& diagnostics,
    const NLS::Core::Assets::AssetId assetId,
    const ExternalModelImportRequest& request,
    const std::string& textureBuildTargetPlatform,
    const NLS::Render::Assets::TextureEncoderRegistry& textureEncoders)
{
    const auto payload = texturePayloads.find(subAsset.sourceKey);
    const auto* bytes = payload != texturePayloads.end() ? &payload->second : nullptr;

    if (!bytes || bytes->empty())
    {
        AddError(
            diagnostics,
            assetId,
            request.sourcePath,
            "external-model-importer-texture-payload-missing",
            "Texture " + subAsset.key + " source=" + subAsset.sourceKey + " has no readable encoded payload.");
        return {};
    }

    const auto textureIntent = ResolveSceneTextureMipIntent(scene, subAsset.sourceKey);
    NLS::Render::Assets::TextureMipGeneratorSettings mipSettings;
    mipSettings.intent = textureIntent;
    mipSettings.colorSpace = [&]()
    {
        const auto colorSpace = textureColorSpaces.find(subAsset.sourceKey);
        return colorSpace != textureColorSpaces.end()
            ? colorSpace->second
            : NLS::Render::Assets::TextureArtifactColorSpace::Srgb;
    }();
    mipSettings.mipmapEnabled = textureIntent != NLS::Render::Assets::TextureMipIntent::UI;
    mipSettings.format = NLS::Render::RHI::TextureFormat::RGBA8;
    const auto importSettings = BuildTextureImportSettingsSnapshot(mipSettings);

    auto decodeSettings = mipSettings;
    decodeSettings.mipmapEnabled = false;

    auto artifact = NLS::Render::Assets::DecodeTextureArtifactFromEncodedImage(
        bytes->data(),
        bytes->size(),
        decodeSettings,
        ShouldFlipExternalModelTextureVertically(request.sourcePath));
    if (!artifact.has_value())
    {
        AddError(
            diagnostics,
            assetId,
            request.sourcePath,
            "external-model-importer-texture-decode-failed",
            "Texture " + subAsset.key + " source=" + subAsset.sourceKey + " could not be decoded.");
        return {};
    }

    const auto sourceDescriptor = BuildTextureSourceDescriptor(scene, subAsset.sourceKey, *artifact);
    if (!ValidateDecodedTextureSafetyLimits(
            *artifact,
            diagnostics,
            assetId,
            request.sourcePath,
            subAsset.key,
            sourceDescriptor.assetPath))
    {
        return {};
    }
    const auto* directXTexEncoder = textureEncoders.Find("directxtex-bc");
    const bool canEncodeBC = directXTexEncoder != nullptr && textureBuildTargetPlatform == "win64-dx12";
    const auto capabilities = canEncodeBC
        ? BuildExternalTextureDirectXCapabilities()
        : BuildExternalTexturePassthroughCapabilities(textureBuildTargetPlatform);
    const std::string requestedEncoderId = canEncodeBC
        ? std::string(directXTexEncoder->GetId())
        : std::string("rgba8-passthrough");
    const uint32_t requestedEncoderVersion = canEncodeBC ? directXTexEncoder->GetVersion() : 1u;

    auto resolved = NLS::Render::Assets::ResolveTextureBuildSettingsWithDiagnostics(
        importSettings,
        std::nullopt,
        sourceDescriptor,
        capabilities,
        requestedEncoderId,
        requestedEncoderVersion);
    AppendTextureFormatResolverDiagnostics(
        diagnostics,
        assetId,
        request.sourcePath,
        subAsset.key,
        resolved.diagnostics);
    if (!resolved.settings.has_value())
        return {};

    if (resolved.settings->maxTextureSize > 0u &&
        (artifact->width > resolved.settings->maxTextureSize ||
            artifact->height > resolved.settings->maxTextureSize))
    {
        AddWarning(
            diagnostics,
            assetId,
            request.sourcePath,
            "external-model-importer-texture-size-limit",
            "Texture " +
                subAsset.key +
                " exceeds maxTextureSize=" +
                std::to_string(resolved.settings->maxTextureSize) +
                " and resizePolicy=" +
                resolved.settings->resizePolicy +
                " is not implemented for external model textures yet");
        return {};
    }

    resolved.settings->sourceAssetIdentity = TextureSourceIdentity(request, subAsset.sourceKey, sourceDescriptor);
    resolved.settings->sourceContentHash = TextureSourceContentHash(request, sourceDescriptor, *bytes);
    resolved.settings->normalizedSettingsHash = BuildTextureSettingsIdentity(importSettings);
    resolved.settings->platformOverrideHash = "none";
    resolved.settings->importerVersion = request.meta.importerVersion;
    resolved.settings->postprocessorVersion = kExternalTexturePostprocessorVersion;
    resolved.settings->dependencyHash = NLS::Core::Assets::ComputeNativeArtifactDependencyHash(scene.dependencies);
    if (resolved.settings->encoderId == "directxtex-bc")
    {
        resolved.settings->encoderOptionsHash = BuildDirectXTexEncoderOptionsHash(*resolved.settings);
        resolved.settings->toolVersion = GetDirectXTexTextureEncoderToolVersion();
    }

    if (resolved.settings->encoderId == "directxtex-bc")
    {
        if (directXTexEncoder == nullptr)
            return {};

        mipSettings.mipmapEnabled = resolved.settings->mipmapEnabled;
        mipSettings.format = NLS::Render::RHI::TextureFormat::RGBA8;
        const bool hasRequestedMipChain = resolved.settings->mipmapEnabled
            ? artifact->mips.size() > 1u
            : artifact->mips.size() == 1u;
        if (!hasRequestedMipChain)
        {
            artifact = NLS::Render::Assets::GenerateTextureMipChain(
                artifact->width,
                artifact->height,
                artifact->mips.front().pixels,
                mipSettings);
            if (!artifact.has_value())
                return {};
        }

        auto encodeResult = directXTexEncoder->Encode({ &*resolved.settings, &*artifact });
        for (const auto& diagnostic : encodeResult.diagnostics)
        {
            AddWarning(
                diagnostics,
                assetId,
                request.sourcePath,
                "external-model-importer-texture-encoding",
                "Texture " + subAsset.key + " " + diagnostic.stage + ": " + diagnostic.message);
        }
        if (!encodeResult.succeeded)
            return {};

        artifact = std::move(encodeResult.artifact);
        const auto serialized = NLS::Render::Assets::SerializeTextureArtifact(*artifact);
        if (serialized.empty())
        {
            AddError(
                diagnostics,
                assetId,
                request.sourcePath,
                "external-model-importer-texture-serialization-failed",
                "Texture " + subAsset.key + " encoded artifact could not be serialized.");
            return {};
        }

        texturePayloads.erase(payload);
        return serialized;
    }
    else
    {
        mipSettings.mipmapEnabled = resolved.settings->mipmapEnabled;
        mipSettings.format = resolved.settings->resolvedFormat;
        if (mipSettings.format != artifact->format ||
            mipSettings.mipmapEnabled != (artifact->mips.size() > 1u))
        {
            artifact = NLS::Render::Assets::GenerateTextureMipChain(
                artifact->width,
                artifact->height,
                artifact->mips.front().pixels,
                mipSettings);
            if (!artifact.has_value())
                return {};
        }

        artifact->targetPlatform = resolved.settings->targetPlatform;
        artifact->encoderId = resolved.settings->encoderId;
        artifact->encoderVersion = resolved.settings->encoderVersion;
        artifact->buildIdentity = NLS::Render::Assets::BuildTextureBuildIdentity(*resolved.settings);
    }

    const auto serialized = NLS::Render::Assets::SerializeTextureArtifact(*artifact);
    if (serialized.empty())
    {
        AddError(
            diagnostics,
            assetId,
            request.sourcePath,
            "external-model-importer-texture-serialization-failed",
            "Texture " + subAsset.key + " artifact could not be serialized.");
        return {};
    }

    texturePayloads.erase(payload);
    return serialized;
}

void RecordTextureSlotColorSpace(
    std::unordered_map<std::string, NLS::Render::Assets::TextureArtifactColorSpace>& textureColorSpaces,
    const NLS::Render::Assets::ConvertedMaterialTextureSlot& slot)
{
    if (slot.textureKey.empty())
        return;

    const auto artifactColorSpace = slot.colorSpace == NLS::Render::Assets::MaterialTextureColorSpace::Linear
        ? NLS::Render::Assets::TextureArtifactColorSpace::Linear
        : NLS::Render::Assets::TextureArtifactColorSpace::Srgb;

    auto [entry, inserted] = textureColorSpaces.emplace(slot.textureKey, artifactColorSpace);
    if (!inserted && artifactColorSpace == NLS::Render::Assets::TextureArtifactColorSpace::Linear)
        entry->second = artifactColorSpace;
}

NLS::Core::Assets::ArtifactManifest MakeProvisionalManifest(
    const ExternalModelImportRequest& request,
    const std::vector<NLS::Core::Assets::ArtifactPayload>& payloads)
{
    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = request.meta.id;
    manifest.importerId = request.meta.importerId;
    manifest.importerVersion = request.meta.importerVersion;
    manifest.targetPlatform = request.targetPlatform;
    manifest.primarySubAssetKey = "prefab:" + request.sceneKey;

    for (const auto& payload : payloads)
    {
        manifest.subAssets.push_back({
            request.meta.id,
            payload.subAssetKey,
            payload.artifactType,
            payload.loaderId,
            request.targetPlatform,
            (request.committedRoot / payload.relativePath).string(),
            "provisional"
        });
    }
    return manifest;
}

NLS::Render::Assets::MaterialSourceModel MaterialSourceForExtension(const std::string& extension)
{
    if (extension == ".obj")
        return NLS::Render::Assets::MaterialSourceModel::ObjMtl;
    if (extension == ".fbx")
        return NLS::Render::Assets::MaterialSourceModel::FbxParserMaterial;
    return NLS::Render::Assets::MaterialSourceModel::GltfPbrMetallicRoughness;
}

NLS::Render::Assets::SceneModelSourceFormat SourceFormatForExtension(const std::string& extension)
{
    if (extension == ".obj")
        return NLS::Render::Assets::SceneModelSourceFormat::Obj;
    if (extension == ".fbx")
        return NLS::Render::Assets::SceneModelSourceFormat::Fbx;
    return NLS::Render::Assets::SceneModelSourceFormat::Gltf;
}

FbxReaderSelection ResolveFbxReaderSelection(const ExternalModelImportRequest& request)
{
    const auto selection = ModelImporterSettingsFromSerialized(request.meta.settings).fbxReaderSelection;
#if !NLS_HAS_AUTODESK_FBX_SDK && NLS_HAS_ASSIMP_FBX_IMPORTER
    if (selection == FbxReaderSelection::Autodesk ||
        selection == FbxReaderSelection::AutodeskWithAssimpFallback)
    {
        return FbxReaderSelection::Assimp;
    }
#endif
    return selection;
}

bool IsAssimpFbxImporterAvailable()
{
    return NLS_HAS_ASSIMP_FBX_IMPORTER != 0;
}

bool IsAutodeskFbxSdkAvailable()
{
    return NLS_HAS_AUTODESK_FBX_SDK != 0;
}

bool LoadWithAssimpParser(
    const ExternalModelImportRequest& request,
    const std::string& extension,
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData>& meshes,
    std::vector<std::string>& materialNames,
    const NLS::Render::Resources::Parsers::EModelParserFlags parserFlags,
    std::vector<std::string>* parserExternalDependencies,
    NLS::Render::Assets::ImportedScene& detailedScene,
    bool& hasDetailedScene)
{
    NLS::Render::Resources::Parsers::AssimpParser parser;
    const bool parsed = parser.LoadModelData(
        request.sourcePath.string(),
        meshes,
        materialNames,
        parserFlags,
        parserExternalDependencies);
    hasDetailedScene = parsed && parser.PopulateImportedSceneData(
        request.sourcePath,
        SourceFormatForExtension(extension),
        detailedScene);
    return parsed;
}

bool LoadFbxWithAssimp(
    const ExternalModelImportRequest& request,
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData>& meshes,
    std::vector<std::string>& materialNames,
    const NLS::Render::Resources::Parsers::EModelParserFlags parserFlags,
    std::vector<std::string>* parserExternalDependencies,
    NLS::Render::Assets::ImportedScene& detailedScene,
    bool& hasDetailedScene,
    NLS::Core::Assets::AssetDiagnostics& diagnostics)
{
    if (!IsAssimpFbxImporterAvailable())
    {
        AddError(
            diagnostics,
            request.meta.id,
            request.sourcePath,
            "external-model-importer-assimp-fbx-unavailable",
            "Assimp FBX import is not enabled in this build. Reconfigure with NLS_ENABLE_ASSIMP_FBX_IMPORTER=ON or choose the Autodesk FBX reader.");
        return false;
    }

    return LoadWithAssimpParser(
        request,
        ".fbx",
        meshes,
        materialNames,
        parserFlags,
        parserExternalDependencies,
        detailedScene,
        hasDetailedScene);
}

bool LoadFbxWithAutodesk(
    const ExternalModelImportRequest& request,
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData>& meshes,
    std::vector<std::string>& materialNames,
    const NLS::Render::Resources::Parsers::EModelParserFlags parserFlags,
    std::vector<std::string>* parserExternalDependencies,
    NLS::Render::Assets::ImportedScene& detailedScene,
    bool& hasDetailedScene)
{
    if (!IsAutodeskFbxSdkAvailable())
        return false;

    NLS::Render::Resources::Parsers::FbxSdkParser parser;
    const bool parsed = parser.LoadModelData(
        request.sourcePath.string(),
        meshes,
        materialNames,
        parserFlags,
        parserExternalDependencies);
    hasDetailedScene = parsed && parser.PopulateImportedSceneData(
        request.sourcePath,
        NLS::Render::Assets::SceneModelSourceFormat::Fbx,
        detailedScene);
    return parsed;
}

void AddFbxReaderFallbackWarning(
    NLS::Core::Assets::AssetDiagnostics& diagnostics,
    const ExternalModelImportRequest& request,
    std::string reason)
{
    AddWarning(
        diagnostics,
        request.meta.id,
        request.sourcePath,
        "external-model-importer-fbx-reader-fallback",
        "Autodesk FBX reader failed; attempting Assimp FBX reader fallback. Reason: " + std::move(reason));
}

NLS::Render::Assets::ImportedScene ImportSceneForRequest(
    const ExternalModelImportRequest& request,
    NLS::Core::Assets::AssetDiagnostics& diagnostics,
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData>* sourceMeshes = nullptr,
    std::vector<std::string>* parserExternalDependencies = nullptr)
{
    const auto extension = ToLower(request.sourcePath.extension().string());
    if (extension == ".gltf")
    {
        if (sourceMeshes != nullptr)
            *sourceMeshes = LoadGltfSourceMeshData(request, extension);
        return NLS::Render::Assets::ImportGltfSceneJson(
            ReadTextFile(request.sourcePath),
            request.meta.id,
            request.sceneKey,
            ToSceneImportSettings(request.meta.settings));
    }

    if (extension == ".glb")
    {
        if (sourceMeshes != nullptr)
            *sourceMeshes = LoadGltfSourceMeshData(request, extension);
        return NLS::Render::Assets::ImportGltfSceneBytes(
            ReadBinaryFile(request.sourcePath),
            request.meta.id,
            request.sceneKey,
            ToSceneImportSettings(request.meta.settings));
    }

    if (extension == ".obj" || extension == ".fbx")
    {
        std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
        std::vector<std::string> materialNames;
        const auto parserFlags = extension == ".fbx"
            ? NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE |
                NLS::Render::Resources::Parsers::EModelParserFlags::GLOBAL_SCALE
            : NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE;
        NLS::Render::Assets::ImportedScene detailedScene;
        detailedScene.sourceAssetId = request.meta.id;
        detailedScene.sceneKey = request.sceneKey;
        bool hasDetailedScene = false;
        const bool loaded = [&]()
        {
            if (extension == ".fbx")
            {
                const auto fbxReaderSelection = ResolveFbxReaderSelection(request);
                if (fbxReaderSelection == FbxReaderSelection::Assimp)
                {
                    return LoadFbxWithAssimp(
                        request,
                        meshes,
                        materialNames,
                        parserFlags,
                        parserExternalDependencies,
                        detailedScene,
                        hasDetailedScene,
                        diagnostics);
                }

                const bool allowAssimpFallback =
                    fbxReaderSelection == FbxReaderSelection::AutodeskWithAssimpFallback;

                const bool parsed = LoadFbxWithAutodesk(
                    request,
                    meshes,
                    materialNames,
                    parserFlags,
                    parserExternalDependencies,
                    detailedScene,
                    hasDetailedScene);
                if (parsed || !allowAssimpFallback)
                    return parsed;

                AddFbxReaderFallbackWarning(diagnostics, request, "Autodesk parser failed to load the source file.");
                meshes.clear();
                materialNames.clear();
                hasDetailedScene = false;
                detailedScene = {};
                detailedScene.sourceAssetId = request.meta.id;
                detailedScene.sceneKey = request.sceneKey;
                if (parserExternalDependencies)
                    parserExternalDependencies->clear();
                return LoadFbxWithAssimp(
                    request,
                    meshes,
                    materialNames,
                    parserFlags,
                    parserExternalDependencies,
                    detailedScene,
                    hasDetailedScene,
                    diagnostics);
            }

            return LoadWithAssimpParser(
                request,
                extension,
                meshes,
                materialNames,
                parserFlags,
                parserExternalDependencies,
                detailedScene,
                hasDetailedScene);
        }();
        if (!loaded)
        {
            AddError(
                diagnostics,
                request.meta.id,
                request.sourcePath,
                "external-model-importer-source-parse-failed",
                "Model parser failed to load the source file.");
            return {};
        }

        if (meshes.empty())
        {
            AddError(
                diagnostics,
                request.meta.id,
                request.sourcePath,
                "external-model-importer-source-empty",
                "Model parser did not produce any mesh data from the source file.");
            return {};
        }

        auto scene = hasDetailedScene
            ? std::move(detailedScene)
            : NLS::Render::Assets::ImportParsedModelScene(
                meshes,
                materialNames,
                request.sourcePath,
                SourceFormatForExtension(extension),
                request.meta.id,
                request.sceneKey);
        scene.importSettings = ToSceneImportSettings(request.meta.settings);
        NLS::Render::Assets::ApplySceneImportSettings(scene);
        if (sourceMeshes != nullptr)
            *sourceMeshes = std::move(meshes);

        return scene;
    }

    AddError(
        diagnostics,
        request.meta.id,
        request.sourcePath,
        "external-model-importer-unsupported-extension",
        "External model importer only accepts glTF, GLB, FBX, and OBJ source files.");
    return {};
}

std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> LoadSourceMeshData(
    const ExternalModelImportRequest& request,
    NLS::Core::Assets::AssetDiagnostics& diagnostics,
    std::vector<std::string>* externalDependencies)
{
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;
    const auto extension = ToLower(request.sourcePath.extension().string());
    if (extension == ".gltf" || extension == ".glb")
    {
        meshes = LoadGltfSourceMeshData(request, extension);
        if (meshes.empty())
        {
            AddWarning(
                diagnostics,
                request.meta.id,
                request.sourcePath,
                "external-model-importer-mesh-cache-build-failed",
                "Native mesh artifacts could not be generated from the source model; mesh artifacts will be empty placeholders.");
        }
        return meshes;
    }

    const bool loaded = [&]()
    {
        if (extension == ".fbx")
        {
            NLS::Render::Assets::ImportedScene unusedScene;
            bool hasDetailedScene = false;
            const auto fbxParserFlags =
                NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE |
                NLS::Render::Resources::Parsers::EModelParserFlags::GLOBAL_SCALE;
            const auto fbxReaderSelection = ResolveFbxReaderSelection(request);
            if (fbxReaderSelection == FbxReaderSelection::Assimp)
            {
                return LoadFbxWithAssimp(
                    request,
                    meshes,
                    materials,
                    fbxParserFlags,
                    externalDependencies,
                    unusedScene,
                    hasDetailedScene,
                    diagnostics);
            }

            const bool loadedWithAutodesk = LoadFbxWithAutodesk(
                request,
                meshes,
                materials,
                fbxParserFlags,
                externalDependencies,
                unusedScene,
                hasDetailedScene);
            if (loadedWithAutodesk || fbxReaderSelection != FbxReaderSelection::AutodeskWithAssimpFallback)
                return loadedWithAutodesk;

            AddFbxReaderFallbackWarning(diagnostics, request, "Autodesk parser failed while building native mesh cache.");
            meshes.clear();
            materials.clear();
            if (externalDependencies)
                externalDependencies->clear();
            hasDetailedScene = false;
            unusedScene = {};
            return LoadFbxWithAssimp(
                request,
                meshes,
                materials,
                fbxParserFlags,
                externalDependencies,
                unusedScene,
                hasDetailedScene,
                diagnostics);
        }

        NLS::Render::Resources::Parsers::AssimpParser parser;
        return parser.LoadModelData(
            request.sourcePath.string(),
            meshes,
            materials,
            NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE,
            externalDependencies);
    }();
    if (!loaded)
    {
        AddWarning(
            diagnostics,
            request.meta.id,
            request.sourcePath,
            "external-model-importer-mesh-cache-build-failed",
            "Native mesh artifacts could not be generated from the source model; mesh artifacts will be empty placeholders.");
    }
    return meshes;
}

std::vector<uint8_t> SerializeMeshSubAsset(
    const NLS::Render::Assets::ImportedScene& scene,
    const NLS::Render::Assets::GeneratedSceneSubAsset& subAsset,
    const std::vector<NLS::Render::Resources::Parsers::ParsedMeshData>& sourceMeshes)
{
    std::unordered_map<std::string, const NLS::Render::Resources::Parsers::ParsedMeshData*> meshesByExactSourceKey;
    meshesByExactSourceKey.reserve(sourceMeshes.size());
    std::unordered_map<std::string, size_t> meshStartIndexBySourceKey;
    meshStartIndexBySourceKey.reserve(sourceMeshes.size());
    for (size_t index = 0u; index < sourceMeshes.size(); ++index)
    {
        const auto& mesh = sourceMeshes[index];
        if (!mesh.sourceKey.empty())
            meshesByExactSourceKey.emplace(mesh.sourceKey, &mesh);

        if (mesh.sourceMeshIndex != std::numeric_limits<uint32_t>::max())
            meshStartIndexBySourceKey.emplace("parser/mesh/" + std::to_string(mesh.sourceMeshIndex), index);

        if (mesh.sourceMeshIndex == std::numeric_limits<uint32_t>::max())
            meshStartIndexBySourceKey.emplace("mesh/" + std::to_string(index), index);
        else
            meshStartIndexBySourceKey.emplace("mesh/" + std::to_string(mesh.sourceMeshIndex), index);
    }

    const auto mappedMesh = meshesByExactSourceKey.find(subAsset.sourceKey);
    const auto meshRecord = std::find_if(
        scene.meshes.begin(),
        scene.meshes.end(),
        [&subAsset](const NLS::Render::Assets::ImportedSceneNamedRecord& mesh)
        {
            return mesh.sourceKey == subAsset.sourceKey;
        });
    const auto primitiveCount = meshRecord != scene.meshes.end() && meshRecord->primitiveCount > 0u
        ? static_cast<size_t>(meshRecord->primitiveCount)
        : 1u;

    if (mappedMesh != meshesByExactSourceKey.end())
    {
        NLS::Render::Assets::MeshArtifactData mesh;
        mesh.vertices = mappedMesh->second->vertices;
        mesh.indices = mappedMesh->second->indices;
        mesh.materialIndex = mappedMesh->second->materialIndex;
        auto bytes = NLS::Render::Assets::SerializeMeshArtifact(mesh);
        return bytes.empty() ? ToBytes(SerializeGenericSubAsset(scene, subAsset)) : std::move(bytes);
    }

    std::optional<size_t> sourceIndex;
    if (const auto foundStartIndex = meshStartIndexBySourceKey.find(subAsset.sourceKey);
        foundStartIndex != meshStartIndexBySourceKey.end())
    {
        sourceIndex = foundStartIndex->second;
    }
    else
    {
        sourceIndex = ParseIndexedSourceKey(subAsset.sourceKey, "mesh");
        if (!sourceIndex.has_value())
            sourceIndex = ParseIndexedSourceKey(subAsset.sourceKey, "parser/mesh");
    }

    if (!sourceIndex.has_value() || *sourceIndex >= sourceMeshes.size())
        return ToBytes(SerializeGenericSubAsset(scene, subAsset));

    NLS::Render::Assets::MeshArtifactData mergedMesh;
    for (size_t primitiveIndex = 0u;
        primitiveIndex < primitiveCount && *sourceIndex + primitiveIndex < sourceMeshes.size();
        ++primitiveIndex)
    {
        const auto& sourceMesh = sourceMeshes[*sourceIndex + primitiveIndex];
        if (primitiveIndex == 0u)
            mergedMesh.materialIndex = sourceMesh.materialIndex;

        const auto vertexOffset = static_cast<uint32_t>(mergedMesh.vertices.size());
        mergedMesh.vertices.insert(mergedMesh.vertices.end(), sourceMesh.vertices.begin(), sourceMesh.vertices.end());
        mergedMesh.indices.reserve(mergedMesh.indices.size() + sourceMesh.indices.size());
        for (const auto index : sourceMesh.indices)
            mergedMesh.indices.push_back(vertexOffset + index);
    }

    auto bytes = NLS::Render::Assets::SerializeMeshArtifact(mergedMesh);
    return bytes.empty() ? ToBytes(SerializeGenericSubAsset(scene, subAsset)) : std::move(bytes);
}

void AppendConvertedMaterialPayloads(
    const NLS::Render::Assets::ImportedScene& scene,
    const std::string& extension,
    const std::filesystem::path& textureResourcePathPrefix,
    std::string materialShaderResourcePath,
    std::unordered_map<std::string, std::filesystem::path> importedTextureArtifactPaths,
    std::vector<NLS::Core::Assets::ArtifactPayload>& payloads,
    std::unordered_map<std::string, NLS::Render::Assets::TextureArtifactColorSpace>& textureColorSpaces)
{
    std::vector<NLS::Render::Assets::ConvertedMaterialArtifact> materials;
    materials.reserve(scene.materials.size());
    const auto sourceModel = MaterialSourceForExtension(extension);
    const NLS::Render::Assets::MaterialConversionContext context {
        textureResourcePathPrefix,
        std::move(importedTextureArtifactPaths),
        std::move(materialShaderResourcePath),
        scene.importSettings.ignoreFbxTexturedNeutralDiffuseTint
    };
    for (const auto& material : scene.materials)
    {
        auto converted = NLS::Render::Assets::ConvertImportedSceneMaterial(scene, material, sourceModel, context);
        for (const auto& slot : converted.textureSlots)
            RecordTextureSlotColorSpace(textureColorSpaces, slot);
        materials.push_back(std::move(converted));
    }

    auto materialPayloads = NLS::Render::Assets::BuildMaterialArtifactPayloads(materials);
    payloads.insert(
        payloads.end(),
        std::make_move_iterator(materialPayloads.begin()),
        std::make_move_iterator(materialPayloads.end()));
}

const NLS::Core::Assets::IArtifactWriteCancellation* GetImportCancellation(
    const ExternalModelImportRequest& request,
    std::optional<ImportCancellationTokenHandle>& token)
{
    if (!request.progressTracker || !request.progressJob.IsValid())
        return nullptr;

    token = request.progressTracker->GetCancellationToken(request.progressJob);
    return token.has_value() ? &token->get() : nullptr;
}
}

ExternalModelImportResult ImportExternalModelAsset(const ExternalModelImportRequest& request)
{
    NLS_PROFILE_NAMED_SCOPE("AssetImport::ExternalModel::Total");
    ExternalModelImportResult result;
    ExternalModelImportTimingStats timingStats;
    ScopedExternalModelImportTiming timing(request, timingStats);
    if (!request.meta.id.IsValid())
    {
        AddError(
            result.diagnostics,
            request.meta.id,
            request.sourcePath,
            "external-model-importer-invalid-source-id",
            "External model import requires a valid source asset id.");
        timingStats.diagnosticCount = result.diagnostics.size();
        return result;
    }

    ReportProgress(request, ImportPhase::SourceParse, 0.05, "Reading model source");
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> sourceMeshes;
    std::vector<std::string> parserExternalDependencies;
    NLS::Render::Assets::ImportedScene scene;
    {
        NLS_PROFILE_NAMED_SCOPE("AssetImport::ExternalModel::SourceParse");
        scene = ImportSceneForRequest(
            request,
            result.diagnostics,
            &sourceMeshes,
            &parserExternalDependencies);
    }
    timingStats.sourceMeshCount = sourceMeshes.size();
    timingStats.sceneMeshCount = scene.meshes.size();
    timingStats.materialCount = scene.materials.size();
    timingStats.textureCount = scene.textures.size();
    timingStats.diagnosticCount = result.diagnostics.size();
    if (HasErrors(result.diagnostics))
        return result;

    for (const auto& diagnostic : scene.diagnostics)
    {
        AddWarning(
            result.diagnostics,
            request.meta.id,
            request.sourcePath,
            diagnostic.code,
            diagnostic.message);
    }
    timingStats.diagnosticCount = result.diagnostics.size();

    ReportProgress(request, ImportPhase::IntermediateConversion, 0.25, "Converting scene hierarchy and materials");
    std::vector<NLS::Render::Assets::GeneratedSceneSubAsset> subAssets;
    {
        NLS_PROFILE_NAMED_SCOPE("AssetImport::ExternalModel::GenerateSubAssets");
        subAssets = NLS::Render::Assets::GenerateSceneSubAssets(scene);
    }
    timingStats.subAssetCount = subAssets.size();
    std::vector<NLS::Core::Assets::ArtifactPayload> payloads;
    payloads.reserve(subAssets.size());

    const auto extension = ToLower(request.sourcePath.extension().string());
    std::unordered_map<std::string, NLS::Render::Assets::TextureArtifactColorSpace> textureColorSpaces;
    {
        NLS_PROFILE_NAMED_SCOPE("AssetImport::ExternalModel::ConvertMaterials");
        AppendConvertedMaterialPayloads(
            scene,
            extension,
            request.textureResourcePathPrefix,
            request.materialShaderResourcePath,
            BuildTextureArtifactPathMap(subAssets, request.committedRoot, request.projectRoot),
            payloads,
            textureColorSpaces);
    }
    timingStats.payloadCount = payloads.size();
    if (extension != ".obj" && extension != ".fbx")
    {
        ReportProgress(request, ImportPhase::IntermediateConversion, 0.4, "Building native mesh cache");
        {
            NLS_PROFILE_NAMED_SCOPE("AssetImport::ExternalModel::LoadSourceMeshes");
            sourceMeshes = LoadSourceMeshData(
                request,
                result.diagnostics,
                &parserExternalDependencies);
        }
        timingStats.sourceMeshCount = sourceMeshes.size();
        timingStats.diagnosticCount = result.diagnostics.size();
    }
    std::unordered_map<std::string, std::vector<uint8_t>> texturePayloads;
    ReportProgress(request, ImportPhase::IntermediateConversion, 0.42, "Loading texture payloads");
    {
        NLS_PROFILE_NAMED_SCOPE("AssetImport::ExternalModel::LoadTextures");
        texturePayloads = LoadTexturePayloads(request, scene, extension);
    }
    ReportProgress(request, ImportPhase::IntermediateConversion, 0.44, "Loaded texture payloads");
    NLS::Render::Assets::TextureEncoderRegistry textureEncoders;
#if NLS_HAS_DIRECTXTEX
    textureEncoders.Register(CreateDirectXTexTextureEncoder());
#endif

    size_t processedSubAssets = 0u;
    const size_t convertibleSubAssetCount = static_cast<size_t>(std::count_if(
        subAssets.begin(),
        subAssets.end(),
        [](const NLS::Render::Assets::GeneratedSceneSubAsset& subAsset)
        {
            return subAsset.type != NLS::Render::Assets::ImportedSceneSubAssetType::Material &&
                subAsset.type != NLS::Render::Assets::ImportedSceneSubAssetType::Prefab;
        }));
    {
        NLS_PROFILE_NAMED_SCOPE("AssetImport::ExternalModel::SerializeSubAssets");
        for (const auto& subAsset : subAssets)
        {
            if (subAsset.type == NLS::Render::Assets::ImportedSceneSubAssetType::Material ||
                subAsset.type == NLS::Render::Assets::ImportedSceneSubAssetType::Prefab)
                continue;

            const auto subAssetProgress = subAssets.empty()
                ? 0.55
                : 0.45 + (0.25 * static_cast<double>(processedSubAssets) / static_cast<double>(std::max<size_t>(convertibleSubAssetCount, 1u)));
            ReportProgress(
                request,
                ImportPhase::IntermediateConversion,
                subAssetProgress,
                "Converting " + subAsset.key);
            auto artifactPayload = subAsset.type == NLS::Render::Assets::ImportedSceneSubAssetType::Mesh
                ? SerializeMeshSubAsset(scene, subAsset, sourceMeshes)
                : subAsset.type == NLS::Render::Assets::ImportedSceneSubAssetType::Texture
                    ? SerializeTextureSubAsset(
                        scene,
                        subAsset,
                        textureColorSpaces,
                        texturePayloads,
                        result.diagnostics,
                        request.meta.id,
                        request,
                        ResolveExternalTextureBuildTargetPlatform(request.targetPlatform),
                        textureEncoders)
                    : ToBytes(SerializeGenericSubAsset(scene, subAsset));
            if (subAsset.type == NLS::Render::Assets::ImportedSceneSubAssetType::Texture &&
                artifactPayload.empty())
            {
                if (!HasErrors(result.diagnostics))
                {
                    AddError(
                        result.diagnostics,
                        request.meta.id,
                        request.sourcePath,
                        "external-model-importer-texture-serialization-failed",
                        "Texture " + subAsset.key + " could not be converted into a native texture artifact.");
                }
                timingStats.diagnosticCount = result.diagnostics.size();
                timingStats.status = "failed";
                return result;
            }

            payloads.push_back({
                subAsset.key,
                ToArtifactType(subAsset.type),
                LoaderIdFor(subAsset.type),
                RelativePathFor(subAsset),
                std::move(artifactPayload)
            });
            ++processedSubAssets;
            const auto completedSubAssetProgress = 0.45 +
                (0.25 * static_cast<double>(processedSubAssets) / static_cast<double>(std::max<size_t>(convertibleSubAssetCount, 1u)));
            ReportProgress(
                request,
                ImportPhase::IntermediateConversion,
                completedSubAssetProgress,
                "Converted " + subAsset.key);
        }
    }
    timingStats.payloadCount = payloads.size();

    ReportProgress(request, ImportPhase::Postprocess, 0.72, "Building generated prefab graph");
    auto provisionalManifest = MakeProvisionalManifest(request, payloads);
    NLS::Engine::Assets::PrefabImportResult prefab;
    {
        NLS_PROFILE_NAMED_SCOPE("AssetImport::ExternalModel::BuildPrefab");
        prefab = NLS::Engine::Assets::BuildGeneratedModelPrefab(
            scene,
            subAssets,
            provisionalManifest);
    }

    if (prefab.diagnostics.HasErrors())
    {
        AddError(
            result.diagnostics,
            request.meta.id,
            request.sourcePath,
            "external-model-importer-prefab-build-failed",
            "Generated model prefab could not be built from the imported scene.");
        timingStats.diagnosticCount = result.diagnostics.size();
        return result;
    }

    for (const auto& subAsset : subAssets)
    {
        if (subAsset.type != NLS::Render::Assets::ImportedSceneSubAssetType::Prefab)
            continue;

        std::vector<uint8_t> prefabBytes;
        {
            NLS_PROFILE_NAMED_SCOPE("AssetImport::ExternalModel::SerializePrefab");
            prefabBytes = ToBytes(NLS::Engine::Serialize::ObjectGraphWriter::Write(prefab.artifact.graph));
        }
        payloads.push_back({
            subAsset.key,
            ToArtifactType(subAsset.type),
            LoaderIdFor(subAsset.type),
            RelativePathFor(subAsset),
            std::move(prefabBytes)
        });
    }
    timingStats.payloadCount = payloads.size();

    ReportProgress(request, ImportPhase::ArtifactWrite, 0.85, "Writing imported artifacts");
    NLS::Core::Assets::ArtifactWriteRequest writeRequest;
    writeRequest.sourceAssetId = request.meta.id;
    writeRequest.importerId = request.meta.importerId;
    writeRequest.importerVersion = request.meta.importerVersion;
    writeRequest.targetPlatform = request.targetPlatform;
    writeRequest.primarySubAssetKey = provisionalManifest.primarySubAssetKey;
    writeRequest.artifacts = std::move(payloads);
    writeRequest.dependencies = scene.dependencies;
    auto externalSourceDependencies = CollectExternalSourceFileDependencies(request, scene);
    CollectParserExternalFileDependencies(request, externalSourceDependencies, parserExternalDependencies);
    CollectObjMaterialFileDependencies(request, externalSourceDependencies);
    for (auto& dependency : externalSourceDependencies)
        AddUniqueDependency(writeRequest.dependencies, std::move(dependency));
    AddTextureBuildIdentityDependencies(writeRequest.dependencies, writeRequest.artifacts);
    writeRequest.dependencies.push_back({
        NLS::Core::Assets::AssetDependencyKind::ImporterVersion,
        request.meta.importerId,
        std::to_string(request.meta.importerVersion)
    });
    writeRequest.dependencies.push_back({
        NLS::Core::Assets::AssetDependencyKind::PostprocessorVersion,
        kExternalTextureBuildPipelineDependencyName,
        std::to_string(kExternalTexturePostprocessorVersion)
    });
    writeRequest.dependencies.push_back({
        NLS::Core::Assets::AssetDependencyKind::BuildTarget,
        request.targetPlatform,
        request.targetPlatform
    });
    timingStats.dependencyCount = writeRequest.dependencies.size();

    NLS::Core::Assets::ArtifactWriter writer(request.stagingRoot, request.committedRoot);
    NLS::Core::Assets::ArtifactWriteResult writeResult;
    std::optional<ImportCancellationTokenHandle> cancellationToken;
    {
        NLS_PROFILE_NAMED_SCOPE("AssetImport::ExternalModel::WriteAndCommit");
        writeResult = writer.WriteAndCommit(
            writeRequest,
            request.previousManifest,
            GetImportCancellation(request, cancellationToken));
    }
    ReportProgress(request, ImportPhase::Commit, 0.95, "Committing imported artifacts");

    result.diagnostics.insert(
        result.diagnostics.end(),
        writeResult.diagnostics.begin(),
        writeResult.diagnostics.end());

    result.imported = writeResult.committed && !HasErrors(result.diagnostics);
    result.manifest = writeResult.manifest;
    timingStats.diagnosticCount = result.diagnostics.size();
    timingStats.status = result.imported ? "imported" : "failed";
    return result;
}
}
