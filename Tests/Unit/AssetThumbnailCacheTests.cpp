#include <gtest/gtest.h>

#include "Assets/AssetThumbnailCache.h"
#include "Assets/AssetThumbnailService.h"
#include "Assets/AssetId.h"
#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/EditorThumbnailPreviewRenderer.h"
#include "Assets/NativeArtifactContainer.h"
#include "Assets/PreviewRenderableSnapshot.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Jobs/JobSystem.h"
#include "Profiling/PerformanceStageStats.h"
#include "Serialize/ObjectGraphWriter.h"
#include "Guid.h"
#include "Image.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/RHI/RHITypes.h"
#include "Rendering/Resources/Shader.h"
#include <Json/json.hpp>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
std::filesystem::path MakeAssetThumbnailCacheRoot()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_asset_thumbnail_cache_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root / "Assets");
    return root;
}

class ScopedThumbnailMeshManagerAssetPaths final
{
public:
    ScopedThumbnailMeshManagerAssetPaths(
        const std::filesystem::path& projectAssetsRoot,
        const std::filesystem::path& engineAssetsRoot)
    {
        NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths(
            projectAssetsRoot.generic_string() + "/",
            engineAssetsRoot.generic_string() + "/");
    }

    ~ScopedThumbnailMeshManagerAssetPaths()
    {
        NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths({}, {});
    }

    ScopedThumbnailMeshManagerAssetPaths(const ScopedThumbnailMeshManagerAssetPaths&) = delete;
    ScopedThumbnailMeshManagerAssetPaths& operator=(const ScopedThumbnailMeshManagerAssetPaths&) = delete;
};

class ScopedAssetThumbnailCacheJobSystem final
{
public:
    explicit ScopedAssetThumbnailCacheJobSystem(const uint32_t backgroundWorkerCount = 1u)
    {
        NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
#if defined(NLS_ENABLE_TEST_HOOKS)
        NLS::Base::Jobs::ResetJobSystemForTesting();
#endif

        NLS::Base::Jobs::JobSystemConfig config;
        config.workerCount = 0u;
        config.backgroundWorkerCount = backgroundWorkerCount;
        m_initialized = NLS::Base::Jobs::InitializeJobSystem(config);
    }

    ~ScopedAssetThumbnailCacheJobSystem()
    {
        NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
#if defined(NLS_ENABLE_TEST_HOOKS)
        NLS::Base::Jobs::ResetJobSystemForTesting();
#endif
    }

    [[nodiscard]] bool IsInitialized() const
    {
        return m_initialized;
    }

    ScopedAssetThumbnailCacheJobSystem(const ScopedAssetThumbnailCacheJobSystem&) = delete;
    ScopedAssetThumbnailCacheJobSystem& operator=(const ScopedAssetThumbnailCacheJobSystem&) = delete;

private:
    bool m_initialized = false;
};

NLS::Editor::Assets::AssetThumbnailRequest MakeThumbnailRequest(
    const std::filesystem::path& root,
    std::string subAssetKey = {},
    std::string sourceStamp = "source:v1")
{
    NLS::Editor::Assets::AssetThumbnailRequest request;
    request.projectRoot = root;
    request.assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("a1010101-0101-4101-8101-010101010101"));
    request.sourceAssetPath = "Assets/Models/Hero.fbx";
    request.subAssetKey = std::move(subAssetKey);
    request.kind = NLS::Editor::Assets::AssetThumbnailKind::ModelPreview;
    request.requestedSize = 96u;
    request.settingsFingerprint = "lighting:v1";
    request.freshnessInputs.push_back({"source", std::move(sourceStamp)});
    request.freshnessInputs.push_back({"artifact", "artifact:v1"});
    return request;
}

void WriteBytesToDisk(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

std::optional<std::filesystem::path> ProjectRootForLibraryArtifactPath(const std::filesystem::path& path)
{
    std::vector<std::filesystem::path> parts;
    for (const auto& part : path.lexically_normal())
        parts.push_back(part);

    for (size_t index = 0u; index + 1u < parts.size(); ++index)
    {
        if (parts[index].generic_string() != "Library" ||
            parts[index + 1u].generic_string() != "Artifacts")
        {
            continue;
        }

        std::filesystem::path root;
        for (size_t rootIndex = 0u; rootIndex < index; ++rootIndex)
            root /= parts[rootIndex];
        return root;
    }

    return std::nullopt;
}

std::string NormalizePortablePath(const std::filesystem::path& path)
{
    return path.lexically_normal().generic_string();
}

std::unordered_map<std::string, std::string>& LegacyArtifactPathRedirects()
{
    static std::unordered_map<std::string, std::string> redirects;
    return redirects;
}

std::string RedirectedArtifactPathOrFallback(const std::string& artifactPath)
{
    const auto normalized = NormalizePortablePath(artifactPath);
    const auto& redirects = LegacyArtifactPathRedirects();
    const auto found = redirects.find(normalized);
    if (found != redirects.end())
        return found->second;

    const auto storageName = NLS::Core::Assets::BuildArtifactStorageFileName(normalized);
    return (std::filesystem::path("Library") /
        "Artifacts" /
        NLS::Core::Assets::BuildArtifactStorageRelativePath(storageName)).generic_string();
}

std::filesystem::path BuiltinMeshArtifactPath(
    const std::filesystem::path& engineAssetsRoot,
    const std::string& virtualSourcePath)
{
    const auto storageName = NLS::Core::Assets::BuildArtifactStorageFileName(
        "BuiltinMeshArtifact:" + virtualSourcePath);
    return engineAssetsRoot /
        "Library" /
        "BuiltinArtifacts" /
        std::filesystem::path(virtualSourcePath).parent_path() /
        storageName;
}

bool IsLegacySemanticArtifactPayloadPath(const std::filesystem::path& path)
{
    const auto extension = path.extension().generic_string();
    return extension == ".nmesh" ||
        extension == ".nmat" ||
        extension == ".ntex" ||
        extension == ".nprefab";
}

bool WriteContentAddressedArtifactIfNeeded(
    const std::filesystem::path& path,
    const std::vector<uint8_t>& bytes)
{
    if (!IsLegacySemanticArtifactPayloadPath(path))
        return false;

    const auto projectRoot = ProjectRootForLibraryArtifactPath(path);
    if (!projectRoot.has_value())
        return false;

    const auto storageName = NLS::Core::Assets::BuildArtifactStorageFileName(bytes.data(), bytes.size());
    const auto redirectedPath = (std::filesystem::path("Library") /
        "Artifacts" /
        NLS::Core::Assets::BuildArtifactStorageRelativePath(storageName)).generic_string();
    const auto oldRelativePath = NormalizePortablePath(std::filesystem::relative(path, *projectRoot));
    LegacyArtifactPathRedirects()[oldRelativePath] = redirectedPath;
    WriteBytesToDisk(*projectRoot / redirectedPath, bytes);
    return true;
}

NLS::Core::Assets::ArtifactType ArtifactTypeFromManifestString(const std::string& value)
{
    using NLS::Core::Assets::ArtifactType;
    if (value == "Model") return ArtifactType::Model;
    if (value == "Mesh") return ArtifactType::Mesh;
    if (value == "Material") return ArtifactType::Material;
    if (value == "Texture") return ArtifactType::Texture;
    if (value == "Skeleton") return ArtifactType::Skeleton;
    if (value == "Skin") return ArtifactType::Skin;
    if (value == "AnimationClip") return ArtifactType::AnimationClip;
    if (value == "MorphTarget") return ArtifactType::MorphTarget;
    if (value == "Prefab") return ArtifactType::Prefab;
    if (value == "Scene") return ArtifactType::Scene;
    if (value == "Shader") return ArtifactType::Shader;
    if (value == "Audio") return ArtifactType::Audio;
    return ArtifactType::Unknown;
}

std::optional<NLS::Core::Assets::AssetId> AssetIdFromArtifactManifestPath(const std::filesystem::path& path)
{
    const auto projectRoot = ProjectRootForLibraryArtifactPath(path);
    if (!projectRoot.has_value())
        return std::nullopt;

    const auto relative = std::filesystem::relative(path.lexically_normal(), *projectRoot).lexically_normal();
    std::vector<std::string> parts;
    for (const auto& part : relative)
        parts.push_back(part.generic_string());
    if (parts.size() < 4u)
        return std::nullopt;

    return NLS::Core::Assets::AssetId(NLS::Guid::Parse(parts[2u]));
}

void WriteArtifactDatabaseFromLegacyManifestJson(
    const std::filesystem::path& path,
    const std::string& text)
{
    const auto projectRoot = ProjectRootForLibraryArtifactPath(path);
    if (!projectRoot.has_value())
        return;

    const auto document = nlohmann::json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object())
        return;

    const auto pathAssetId = AssetIdFromArtifactManifestPath(path);
    const auto readAssetId = [&pathAssetId](const nlohmann::json& object)
    {
        if (object.contains("sourceAssetId") && object["sourceAssetId"].is_string())
            return NLS::Core::Assets::AssetId(NLS::Guid::Parse(object["sourceAssetId"].get<std::string>()));
        return pathAssetId.value_or(NLS::Core::Assets::AssetId {});
    };

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = readAssetId(document);
    if (!manifest.sourceAssetId.IsValid())
        return;
    manifest.importerId = document.value("importerId", "test-importer");
    manifest.importerVersion = document.value("importerVersion", 1u);
    manifest.targetPlatform = document.value("targetPlatform", "editor");
    manifest.primarySubAssetKey = document.value("primarySubAssetKey", std::string {});

    if (document.contains("subAssets") && document["subAssets"].is_array())
    {
        for (const auto& artifactJson : document["subAssets"])
        {
            if (!artifactJson.is_object())
                continue;

            NLS::Core::Assets::ImportedArtifact artifact;
            artifact.sourceAssetId = readAssetId(artifactJson);
            artifact.subAssetKey = artifactJson.value("subAssetKey", std::string {});
            artifact.artifactType = ArtifactTypeFromManifestString(artifactJson.value("artifactType", std::string {}));
            artifact.loaderId = artifactJson.value("loaderId", std::string {});
            artifact.targetPlatform = artifactJson.value("targetPlatform", manifest.targetPlatform);
            artifact.artifactPath = RedirectedArtifactPathOrFallback(artifactJson.value("artifactPath", std::string {}));
            artifact.contentHash = artifactJson.value("contentHash", std::string {});
            artifact.displayName = artifactJson.value("displayName", std::string {});
            if (!artifact.subAssetKey.empty() &&
                artifact.artifactType != NLS::Core::Assets::ArtifactType::Unknown)
            {
                manifest.subAssets.push_back(std::move(artifact));
            }
        }
    }

    NLS::Core::Assets::ArtifactDatabase database;
    const auto databasePath = *projectRoot / "Library" / "ArtifactDB";
    if (std::filesystem::exists(databasePath))
        (void)database.Load(databasePath);
    database.UpsertManifest(
        manifest,
        (std::filesystem::path("Assets") / manifest.sourceAssetId.ToString()).generic_string(),
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(database.Save(databasePath));
}

void WriteBinaryFile(const std::filesystem::path& path)
{
    const std::vector<uint8_t> bytes {'p', 'n', 'g'};
    if (WriteContentAddressedArtifactIfNeeded(path, bytes))
        return;
    WriteBytesToDisk(path, bytes);
}

void WriteBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    if (WriteContentAddressedArtifactIfNeeded(path, bytes))
        return;
    WriteBytesToDisk(path, bytes);
}

std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
}

void WriteTextFile(const std::filesystem::path& path, const std::string& text)
{
    if (path.filename() == "manifest.json")
    {
        WriteArtifactDatabaseFromLegacyManifestJson(path, text);
        return;
    }

    const auto bytes = std::vector<uint8_t>(text.begin(), text.end());
    if (WriteContentAddressedArtifactIfNeeded(path, bytes))
        return;
    WriteBytesToDisk(path, bytes);
}

std::string LibraryArtifactPath(const std::string& hash)
{
    return (std::filesystem::path("Library") /
        "Artifacts" /
        NLS::Core::Assets::BuildArtifactStorageRelativePath(hash)).generic_string();
}

std::vector<uint8_t> TinyPng()
{
    return {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x04, 0x00, 0x00, 0x00, 0xB5, 0x1C, 0x0C,
        0x02, 0x00, 0x00, 0x00, 0x0B, 0x49, 0x44, 0x41,
        0x54, 0x78, 0xDA, 0x63, 0xFC, 0xFF, 0x1F, 0x00,
        0x03, 0x03, 0x02, 0x00, 0xEF, 0xBF, 0x4A, 0x3B,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
        0xAE, 0x42, 0x60, 0x82
    };
}

std::vector<uint8_t> PngHeaderOnly(const uint32_t width, const uint32_t height)
{
    auto bytes = std::vector<uint8_t> {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R',
        static_cast<uint8_t>((width >> 24u) & 0xffu),
        static_cast<uint8_t>((width >> 16u) & 0xffu),
        static_cast<uint8_t>((width >> 8u) & 0xffu),
        static_cast<uint8_t>(width & 0xffu),
        static_cast<uint8_t>((height >> 24u) & 0xffu),
        static_cast<uint8_t>((height >> 16u) & 0xffu),
        static_cast<uint8_t>((height >> 8u) & 0xffu),
        static_cast<uint8_t>(height & 0xffu),
        0x08, 0x06, 0x00, 0x00, 0x00
    };
    bytes.resize(64u, 0u);
    return bytes;
}

std::vector<uint8_t> JpegWithLargeAppSegmentBeforeSof(
    const uint16_t width,
    const uint16_t height)
{
    std::vector<uint8_t> bytes;
    bytes.reserve(65564u);
    bytes.push_back(0xffu);
    bytes.push_back(0xd8u);
    bytes.push_back(0xffu);
    bytes.push_back(0xe1u);
    bytes.push_back(0xffu);
    bytes.push_back(0xffu);
    bytes.resize(bytes.size() + 65533u, 0u);
    bytes.push_back(0xffu);
    bytes.push_back(0xc0u);
    bytes.push_back(0x00u);
    bytes.push_back(0x11u);
    bytes.push_back(0x08u);
    bytes.push_back(static_cast<uint8_t>((height >> 8u) & 0xffu));
    bytes.push_back(static_cast<uint8_t>(height & 0xffu));
    bytes.push_back(static_cast<uint8_t>((width >> 8u) & 0xffu));
    bytes.push_back(static_cast<uint8_t>(width & 0xffu));
    bytes.push_back(0x03u);
    for (uint8_t component = 1u; component <= 3u; ++component)
    {
        bytes.push_back(component);
        bytes.push_back(0x11u);
        bytes.push_back(0x00u);
    }
    return bytes;
}

std::vector<uint8_t> Bmp2x1()
{
    return {
        0x42, 0x4D, 0x3E, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x36, 0x00, 0x00, 0x00, 0x28, 0x00,
        0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x01, 0x00, 0x18, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x13, 0x0B,
        0x00, 0x00, 0x13, 0x0B, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00
    };
}

void AppendLittleEndian16(std::vector<uint8_t>& bytes, const uint16_t value)
{
    bytes.push_back(static_cast<uint8_t>(value & 0xffu));
    bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
}

void AppendLittleEndian32(std::vector<uint8_t>& bytes, const uint32_t value)
{
    bytes.push_back(static_cast<uint8_t>(value & 0xffu));
    bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
    bytes.push_back(static_cast<uint8_t>((value >> 16u) & 0xffu));
    bytes.push_back(static_cast<uint8_t>((value >> 24u) & 0xffu));
}

void AppendLittleEndian64(std::vector<uint8_t>& bytes, const uint64_t value)
{
    for (uint32_t byteIndex = 0u; byteIndex < 8u; ++byteIndex)
        bytes.push_back(static_cast<uint8_t>((value >> (byteIndex * 8u)) & 0xffu));
}

std::vector<uint8_t> NativeArtifactHeaderOnly(
    const NLS::Core::Assets::ArtifactType artifactType,
    const uint32_t schemaVersion,
    const uint64_t metadataSize,
    const uint64_t payloadSize)
{
    std::vector<uint8_t> bytes;
    AppendLittleEndian32(bytes, 0x41534C4Eu);
    AppendLittleEndian32(bytes, 1u);
    AppendLittleEndian32(bytes, 64u);
    AppendLittleEndian32(bytes, 1u);
    AppendLittleEndian32(bytes, static_cast<uint32_t>(artifactType));
    AppendLittleEndian32(bytes, schemaVersion);
    AppendLittleEndian64(bytes, metadataSize);
    AppendLittleEndian64(bytes, payloadSize);
    AppendLittleEndian64(bytes, 64ull + metadataSize);
    AppendLittleEndian64(bytes, 0u);
    AppendLittleEndian64(bytes, 0u);
    return bytes;
}

std::vector<uint8_t> NativeTextureArtifactHeaderOnly(const uint32_t width, const uint32_t height)
{
    std::vector<uint8_t> payload;
    payload.reserve(64u);
    AppendLittleEndian32(payload, 0x5845544Eu);
    AppendLittleEndian32(payload, 3u);
    AppendLittleEndian32(payload, width);
    AppendLittleEndian32(payload, height);
    AppendLittleEndian32(payload, 1u);
    AppendLittleEndian32(payload, static_cast<uint32_t>(NLS::Render::RHI::TextureDimension::Texture2D));
    AppendLittleEndian32(payload, 1u);
    AppendLittleEndian32(payload, static_cast<uint32_t>(NLS::Render::RHI::TextureFormat::RGBA8));
    AppendLittleEndian32(payload, static_cast<uint32_t>(NLS::Render::Assets::TextureArtifactColorSpace::Linear));
    AppendLittleEndian32(payload, 1u);
    AppendLittleEndian32(payload, 1u);
    for (uint32_t index = 0u; index < 20u; ++index)
        payload.push_back(0u);

    NLS::Core::Assets::NativeArtifactMetadata metadata;
    metadata.artifactType = NLS::Core::Assets::ArtifactType::Texture;
    metadata.schemaName = "texture";
    metadata.schemaVersion = 4u;
    return NLS::Core::Assets::WriteNativeArtifactContainer(std::move(metadata), payload);
}

std::vector<uint8_t> BmpRgb(const uint32_t width, const uint32_t height)
{
    if (width == 0u || height == 0u)
        throw std::invalid_argument("BMP dimensions must be non-zero.");

    const uint32_t rowStride = ((width * 3u + 3u) / 4u) * 4u;
    const uint32_t pixelDataSize = rowStride * height;
    const uint32_t fileSize = 54u + pixelDataSize;

    std::vector<uint8_t> bytes;
    bytes.reserve(fileSize);
    bytes.push_back('B');
    bytes.push_back('M');
    AppendLittleEndian32(bytes, fileSize);
    AppendLittleEndian16(bytes, 0u);
    AppendLittleEndian16(bytes, 0u);
    AppendLittleEndian32(bytes, 54u);
    AppendLittleEndian32(bytes, 40u);
    AppendLittleEndian32(bytes, width);
    AppendLittleEndian32(bytes, height);
    AppendLittleEndian16(bytes, 1u);
    AppendLittleEndian16(bytes, 24u);
    AppendLittleEndian32(bytes, 0u);
    AppendLittleEndian32(bytes, pixelDataSize);
    AppendLittleEndian32(bytes, 2835u);
    AppendLittleEndian32(bytes, 2835u);
    AppendLittleEndian32(bytes, 0u);
    AppendLittleEndian32(bytes, 0u);

    bytes.resize(fileSize, 0u);
    for (uint32_t y = 0u; y < height; ++y)
    {
        auto* row = bytes.data() + 54u + static_cast<size_t>(y) * rowStride;
        for (uint32_t x = 0u; x < width; ++x)
        {
            row[x * 3u + 0u] = static_cast<uint8_t>((x + y) & 0xffu);
            row[x * 3u + 1u] = static_cast<uint8_t>((x * 3u) & 0xffu);
            row[x * 3u + 2u] = static_cast<uint8_t>((y * 5u) & 0xffu);
        }
    }
    return bytes;
}

std::vector<uint8_t> BmpWithRawHeight(const uint32_t width, const uint32_t rawHeight)
{
    std::vector<uint8_t> bytes;
    bytes.reserve(54u);
    bytes.push_back('B');
    bytes.push_back('M');
    AppendLittleEndian32(bytes, 54u);
    AppendLittleEndian16(bytes, 0u);
    AppendLittleEndian16(bytes, 0u);
    AppendLittleEndian32(bytes, 54u);
    AppendLittleEndian32(bytes, 40u);
    AppendLittleEndian32(bytes, width);
    AppendLittleEndian32(bytes, rawHeight);
    AppendLittleEndian16(bytes, 1u);
    AppendLittleEndian16(bytes, 24u);
    AppendLittleEndian32(bytes, 0u);
    AppendLittleEndian32(bytes, 0u);
    AppendLittleEndian32(bytes, 2835u);
    AppendLittleEndian32(bytes, 2835u);
    AppendLittleEndian32(bytes, 0u);
    AppendLittleEndian32(bytes, 0u);
    return bytes;
}

NLS::Render::Assets::TextureArtifactData RgbaTextureArtifact2x1()
{
    NLS::Render::Assets::TextureArtifactData artifact;
    artifact.width = 2u;
    artifact.height = 1u;
    artifact.format = NLS::Render::RHI::TextureFormat::RGBA8;
    artifact.mips.push_back({
        0u,
        2u,
        1u,
        8u,
        8u,
        {
            255u, 0u, 0u, 255u,
            0u, 255u, 0u, 255u
        }
    });
    return artifact;
}

NLS::Render::Assets::TextureArtifactData RgbaTextureArtifact4x2()
{
    NLS::Render::Assets::TextureArtifactData artifact;
    artifact.width = 4u;
    artifact.height = 2u;
    artifact.format = NLS::Render::RHI::TextureFormat::RGBA8;

    NLS::Render::Assets::TextureArtifactMip mip;
    mip.level = 0u;
    mip.width = 4u;
    mip.height = 2u;
    mip.rowPitch = 16u;
    mip.slicePitch = 32u;
    mip.pixels.resize(32u);
    for (uint32_t y = 0u; y < mip.height; ++y)
    {
        for (uint32_t x = 0u; x < mip.width; ++x)
        {
            const auto index = (static_cast<size_t>(y) * mip.width + x) * 4u;
            mip.pixels[index + 0u] = static_cast<uint8_t>(x * 40u);
            mip.pixels[index + 1u] = static_cast<uint8_t>(y * 90u);
            mip.pixels[index + 2u] = 180u;
            mip.pixels[index + 3u] = 255u;
        }
    }
    artifact.mips.push_back(std::move(mip));
    return artifact;
}

void WriteNativeArtifactTextFile(
    const std::filesystem::path& path,
    const NLS::Core::Assets::ArtifactType artifactType,
    const std::string& schemaName,
    const uint32_t schemaVersion,
    const std::string& contents)
{
    NLS::Core::Assets::NativeArtifactMetadata metadata;
    metadata.artifactType = artifactType;
    metadata.schemaName = schemaName;
    metadata.schemaVersion = schemaVersion;

    const auto payload = std::vector<uint8_t>(contents.begin(), contents.end());
    WriteBinaryFile(path, NLS::Core::Assets::WriteNativeArtifactContainer(std::move(metadata), payload));
}

void WriteNativeArtifactTextFileWithTrailingBytes(
    const std::filesystem::path& path,
    const NLS::Core::Assets::ArtifactType artifactType,
    const std::string& schemaName,
    const uint32_t schemaVersion,
    const std::string& contents,
    const size_t trailingByteCount)
{
    NLS::Core::Assets::NativeArtifactMetadata metadata;
    metadata.artifactType = artifactType;
    metadata.schemaName = schemaName;
    metadata.schemaVersion = schemaVersion;

    const auto payload = std::vector<uint8_t>(contents.begin(), contents.end());
    auto bytes = NLS::Core::Assets::WriteNativeArtifactContainer(std::move(metadata), payload);
    bytes.resize(bytes.size() + trailingByteCount, 0u);
    WriteBinaryFile(path, bytes);
}

NLS::Render::Assets::MeshArtifactData TriangleMeshArtifact()
{
    NLS::Render::Assets::MeshArtifactData mesh;
    mesh.vertices.resize(3u);
    mesh.vertices[0].position[0] = -1.0f;
    mesh.vertices[0].position[1] = -0.75f;
    mesh.vertices[0].position[2] = 0.0f;
    mesh.vertices[1].position[0] = 1.0f;
    mesh.vertices[1].position[1] = -0.75f;
    mesh.vertices[1].position[2] = 0.0f;
    mesh.vertices[2].position[0] = 0.0f;
    mesh.vertices[2].position[1] = 0.75f;
    mesh.vertices[2].position[2] = 0.0f;
    mesh.indices = {0u, 1u, 2u};
    mesh.hasBoundingSphere = true;
    mesh.boundingSphere.position = NLS::Maths::Vector3(0.0f, 0.0f, 0.0f);
    mesh.boundingSphere.radius = 1.25f;
    return mesh;
}

NLS::Render::Assets::MeshArtifactData DegenerateTriangleMeshArtifact()
{
    NLS::Render::Assets::MeshArtifactData mesh;
    mesh.vertices.resize(3u);
    for (auto& vertex : mesh.vertices)
    {
        vertex.position[0] = 0.0f;
        vertex.position[1] = 0.0f;
        vertex.position[2] = 0.0f;
    }
    mesh.indices = {0u, 1u, 2u};
    mesh.hasBoundingSphere = true;
    mesh.boundingSphere.position = NLS::Maths::Vector3(0.0f, 0.0f, 0.0f);
    mesh.boundingSphere.radius = 0.1f;
    return mesh;
}

NLS::Render::Geometry::Vertex CubeVertex(
    const float x,
    const float y,
    const float z,
    const float nx = 0.0f,
    const float ny = 0.0f,
    const float nz = 0.0f)
{
    NLS::Render::Geometry::Vertex vertex {};
    vertex.position[0] = x;
    vertex.position[1] = y;
    vertex.position[2] = z;
    vertex.normals[0] = nx;
    vertex.normals[1] = ny;
    vertex.normals[2] = nz;
    return vertex;
}

NLS::Render::Assets::MeshArtifactData CubeMeshArtifactWithMissingNormals()
{
    NLS::Render::Assets::MeshArtifactData mesh;
    auto addFace = [&mesh](
        const NLS::Render::Geometry::Vertex& a,
        const NLS::Render::Geometry::Vertex& b,
        const NLS::Render::Geometry::Vertex& c,
        const NLS::Render::Geometry::Vertex& d)
    {
        const auto base = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back(a);
        mesh.vertices.push_back(b);
        mesh.vertices.push_back(c);
        mesh.vertices.push_back(d);
        mesh.indices.insert(mesh.indices.end(), {
            base + 0u, base + 1u, base + 2u,
            base + 0u, base + 2u, base + 3u
        });
    };

    addFace(CubeVertex(-0.5f, -0.5f, 0.5f), CubeVertex(0.5f, -0.5f, 0.5f), CubeVertex(0.5f, 0.5f, 0.5f), CubeVertex(-0.5f, 0.5f, 0.5f));
    addFace(CubeVertex(0.5f, -0.5f, -0.5f), CubeVertex(-0.5f, -0.5f, -0.5f), CubeVertex(-0.5f, 0.5f, -0.5f), CubeVertex(0.5f, 0.5f, -0.5f));
    addFace(CubeVertex(-0.5f, 0.5f, 0.5f), CubeVertex(0.5f, 0.5f, 0.5f), CubeVertex(0.5f, 0.5f, -0.5f), CubeVertex(-0.5f, 0.5f, -0.5f));
    addFace(CubeVertex(-0.5f, -0.5f, -0.5f), CubeVertex(0.5f, -0.5f, -0.5f), CubeVertex(0.5f, -0.5f, 0.5f), CubeVertex(-0.5f, -0.5f, 0.5f));
    addFace(CubeVertex(0.5f, -0.5f, 0.5f), CubeVertex(0.5f, -0.5f, -0.5f), CubeVertex(0.5f, 0.5f, -0.5f), CubeVertex(0.5f, 0.5f, 0.5f));
    addFace(CubeVertex(-0.5f, -0.5f, -0.5f), CubeVertex(-0.5f, -0.5f, 0.5f), CubeVertex(-0.5f, 0.5f, 0.5f), CubeVertex(-0.5f, 0.5f, -0.5f));

    mesh.hasBoundingSphere = true;
    mesh.boundingSphere.position = NLS::Maths::Vector3(0.0f, 0.0f, 0.0f);
    mesh.boundingSphere.radius = 0.9f;
    return mesh;
}

NLS::Engine::Serialize::PropertyValue PreviewVector3Value(
    const double x,
    const double y,
    const double z)
{
    using namespace NLS::Engine::Serialize;
    return PropertyValue::Object({
        {"x", PropertyValue::Number(x)},
        {"y", PropertyValue::Number(y)},
        {"z", PropertyValue::Number(z)}
    });
}

NLS::Engine::Serialize::PropertyRecord PreviewTransformProperty(
    const double x,
    const double y,
    const double z)
{
    using namespace NLS::Engine::Serialize;
    return {
        "m_transform",
        PropertyValue::Object({
            {"m_localPosition", PreviewVector3Value(x, y, z)},
            {"m_localScale", PreviewVector3Value(1.0, 1.0, 1.0)}
        })
    };
}

NLS::Render::Assets::MeshArtifactData OversizedMeshArtifact()
{
    NLS::Render::Assets::MeshArtifactData mesh;
    constexpr size_t triangleCount = 85000u;
    mesh.vertices.resize(triangleCount * 3u);
    mesh.indices.reserve(triangleCount * 3u);
    for (size_t triangle = 0u; triangle < triangleCount; ++triangle)
    {
        const auto base = static_cast<uint32_t>(triangle * 3u);
        const auto x = static_cast<float>(triangle % 300u) * 0.01f;
        const auto y = static_cast<float>(triangle / 300u) * 0.01f;
        mesh.vertices[base + 0u].position[0] = x;
        mesh.vertices[base + 0u].position[1] = y;
        mesh.vertices[base + 1u].position[0] = x + 0.005f;
        mesh.vertices[base + 1u].position[1] = y;
        mesh.vertices[base + 2u].position[0] = x;
        mesh.vertices[base + 2u].position[1] = y + 0.005f;
        mesh.indices.push_back(base + 0u);
        mesh.indices.push_back(base + 1u);
        mesh.indices.push_back(base + 2u);
    }
    mesh.hasBoundingSphere = true;
    mesh.boundingSphere.position = NLS::Maths::Vector3(1.5f, 0.15f, 0.0f);
    mesh.boundingSphere.radius = 2.0f;
    return mesh;
}

NLS::Render::Assets::MeshArtifactData BudgetSizedMeshArtifact(
    const uint32_t vertexCount,
    const uint32_t indexCount,
    const float xOffset)
{
    NLS::Render::Assets::MeshArtifactData mesh;
    mesh.vertices.resize(vertexCount);
    mesh.indices.reserve(indexCount);
    for (uint32_t vertexIndex = 0u; vertexIndex < vertexCount; ++vertexIndex)
    {
        auto& vertex = mesh.vertices[vertexIndex];
        vertex.position[0] = xOffset + static_cast<float>(vertexIndex % 400u) * 0.01f;
        vertex.position[1] = static_cast<float>(vertexIndex / 400u) * 0.01f;
        vertex.position[2] = 0.0f;
    }
    for (uint32_t index = 0u; index < indexCount; ++index)
        mesh.indices.push_back(index % vertexCount);
    mesh.hasBoundingSphere = true;
    mesh.boundingSphere.position = NLS::Maths::Vector3(xOffset + 2.0f, 2.0f, 0.0f);
    mesh.boundingSphere.radius = 3.0f;
    return mesh;
}

NLS::Engine::Serialize::ObjectId MakeTestObjectId(const char* guid)
{
    return NLS::Engine::Serialize::ObjectId(NLS::Guid::Parse(guid));
}

std::string MinimalPrefabPayload()
{
    NLS::Engine::Serialize::ObjectGraphDocument document;
    document.format = "Nullus.ObjectGraph.Prefab";
    document.version = 1;
    document.documentId = NLS::Guid::Parse("b1010101-0101-4101-8101-010101010101");
    document.root = NLS::Engine::Serialize::ObjectId(
        NLS::Guid::Parse("b2020202-0202-4202-8202-020202020202"));

    NLS::Engine::Serialize::ObjectRecord root;
    root.id = document.root;
    root.localIdentifierInFile = NLS::Engine::Serialize::MakeLocalIdentifierInFile(root.id);
    root.typeName = "NLS::Engine::GameObject";
    root.debugName = "PreviewLamp";
    root.debugPath = "PreviewLamp";
    document.objects.push_back(std::move(root));

    return NLS::Engine::Serialize::ObjectGraphWriter::Write(document);
}

std::string PrefabPayloadWithTwoTransformedRendererDependencies(
    const NLS::Core::Assets::AssetId& meshAssetId,
    const std::string& leftMeshSubAssetKey,
    const std::string& rightMeshSubAssetKey)
{
    using namespace NLS::Engine::Serialize;

    const auto leftGameObjectId = MakeTestObjectId("bd010101-0101-4101-8101-010101010101");
    const auto leftMeshFilterId = MakeTestObjectId("bd020202-0202-4202-8202-020202020202");
    const auto leftMeshRendererId = MakeTestObjectId("bd030303-0303-4303-8303-030303030303");
    const auto rightGameObjectId = MakeTestObjectId("bd040404-0404-4404-8404-040404040404");
    const auto rightMeshFilterId = MakeTestObjectId("bd050505-0505-4505-8505-050505050505");
    const auto rightMeshRendererId = MakeTestObjectId("bd060606-0606-4606-8606-060606060606");
    const auto rootGameObjectId = MakeTestObjectId("bd080808-0808-4808-8808-080808080808");

    ObjectGraphDocument document;
    document.format = "Nullus.ObjectGraph.Prefab";
    document.version = 1;
    document.documentId = NLS::Guid::Parse("bd070707-0707-4707-8707-070707070707");
    document.root = rootGameObjectId;
    document.objects.push_back(ObjectRecord{
        rootGameObjectId,
        NLS_TYPEOF(NLS::Engine::GameObject).GetName(),
        "Root",
        "Root",
        ObjectRecordState::Alive,
        {
            {
                "children",
                PropertyValue::Array({
                    PropertyValue::OwnedReference(leftGameObjectId),
                    PropertyValue::OwnedReference(rightGameObjectId)
                })
            },
            {
                "components",
                PropertyValue::Array({})
            }
        },
        MakeLocalIdentifierInFile(rootGameObjectId)});

    auto addRendererObject = [&](const NLS::Engine::Serialize::ObjectId& gameObjectId,
                                 const NLS::Engine::Serialize::ObjectId& meshFilterId,
                                 const NLS::Engine::Serialize::ObjectId& meshRendererId,
                                 const char* name,
                                 const double x,
                                 const std::string& meshSubAssetKey)
    {
        document.objects.push_back(ObjectRecord{
            gameObjectId,
            NLS_TYPEOF(NLS::Engine::GameObject).GetName(),
            name,
            name,
            ObjectRecordState::Alive,
            {
                {
                    "components",
                    PropertyValue::Array({
                        PropertyValue::OwnedReference(meshFilterId),
                        PropertyValue::OwnedReference(meshRendererId)
                    })
                },
                {
                    "parent",
                    PropertyValue::ObjectReference(ObjectIdentifier::LocalObject(
                        MakeLocalIdentifierInFile(rootGameObjectId)))
                },
                PreviewTransformProperty(x, 0.0, 0.0)
            },
            MakeLocalIdentifierInFile(gameObjectId)});
        document.objects.push_back(ObjectRecord{
            meshFilterId,
            NLS_TYPEOF(NLS::Engine::Components::MeshFilter).GetName(),
            std::string(name) + " MeshFilter",
            std::string(name) + "/MeshFilter",
            ObjectRecordState::Alive,
            {
                {
                    "mesh",
                    PropertyValue::ObjectReference(ObjectIdentifier::Asset(
                        NLS::Engine::Serialize::AssetId(meshAssetId.GetGuid()),
                        1,
                        meshSubAssetKey))
                }
            },
            MakeLocalIdentifierInFile(meshFilterId)});
        document.objects.push_back(ObjectRecord{
            meshRendererId,
            NLS_TYPEOF(NLS::Engine::Components::MeshRenderer).GetName(),
            std::string(name) + " MeshRenderer",
            std::string(name) + "/MeshRenderer",
            ObjectRecordState::Alive,
            {},
            MakeLocalIdentifierInFile(meshRendererId)});
    };

    addRendererObject(leftGameObjectId, leftMeshFilterId, leftMeshRendererId, "Left", -3.0, leftMeshSubAssetKey);
    addRendererObject(rightGameObjectId, rightMeshFilterId, rightMeshRendererId, "Right", 3.0, rightMeshSubAssetKey);

    return NLS::Engine::Serialize::ObjectGraphWriter::Write(document);
}

std::string PrefabPayloadWithTwoTransformedRendererDependencies(
    const NLS::Core::Assets::AssetId& meshAssetId,
    const std::string& meshSubAssetKey)
{
    return PrefabPayloadWithTwoTransformedRendererDependencies(
        meshAssetId,
        meshSubAssetKey,
        meshSubAssetKey);
}

std::string PrefabPayloadWithSingleRendererDependency(
    const NLS::Core::Assets::AssetId& meshAssetId,
    const std::string& meshSubAssetKey)
{
    using namespace NLS::Engine::Serialize;

    const auto gameObjectId = MakeTestObjectId("bc010101-0101-4101-8101-010101010101");
    const auto meshFilterId = MakeTestObjectId("bc020202-0202-4202-8202-020202020202");
    const auto meshRendererId = MakeTestObjectId("bc030303-0303-4303-8303-030303030303");

    ObjectGraphDocument document;
    document.format = "Nullus.ObjectGraph.Prefab";
    document.version = 1;
    document.documentId = NLS::Guid::Parse("bc040404-0404-4404-8404-040404040404");
    document.root = gameObjectId;
    document.objects.push_back(ObjectRecord{
        gameObjectId,
        NLS_TYPEOF(NLS::Engine::GameObject).GetName(),
        "PreviewRoot",
        "PreviewRoot",
        ObjectRecordState::Alive,
        {
            {
                "components",
                PropertyValue::Array({
                    PropertyValue::OwnedReference(meshFilterId),
                    PropertyValue::OwnedReference(meshRendererId)
                })
            }
        },
        MakeLocalIdentifierInFile(gameObjectId)});
    document.objects.push_back(ObjectRecord{
        meshFilterId,
        NLS_TYPEOF(NLS::Engine::Components::MeshFilter).GetName(),
        "MeshFilter",
        "PreviewRoot/MeshFilter",
        ObjectRecordState::Alive,
        {
            {
                "mesh",
                PropertyValue::ObjectReference(ObjectIdentifier::Asset(
                    NLS::Engine::Serialize::AssetId(meshAssetId.GetGuid()),
                    1,
                    meshSubAssetKey))
            }
        },
        MakeLocalIdentifierInFile(meshFilterId)});
    document.objects.push_back(ObjectRecord{
        meshRendererId,
        NLS_TYPEOF(NLS::Engine::Components::MeshRenderer).GetName(),
        "MeshRenderer",
        "PreviewRoot/MeshRenderer",
        ObjectRecordState::Alive,
        {},
        MakeLocalIdentifierInFile(meshRendererId)});

    return NLS::Engine::Serialize::ObjectGraphWriter::Write(document);
}

std::string PrefabPayloadWithBuiltinPrimitiveMesh(
    const std::string& primitiveMeshPath)
{
    using namespace NLS::Engine::Serialize;

    const auto gameObjectId = MakeTestObjectId("bc121212-1212-4212-8212-121212121212");
    const auto meshFilterId = MakeTestObjectId("bc131313-1313-4313-8313-131313131313");
    const auto meshRendererId = MakeTestObjectId("bc141414-1414-4414-8414-141414141414");
    const auto primitiveGuid = NLS::Guid::NewDeterministic("NLS.MeshReference:" + primitiveMeshPath);

    ObjectGraphDocument document;
    document.format = "Nullus.ObjectGraph.Prefab";
    document.version = 1;
    document.documentId = NLS::Guid::Parse("bc151515-1515-4515-8515-151515151515");
    document.root = gameObjectId;
    document.objects.push_back(ObjectRecord{
        gameObjectId,
        NLS_TYPEOF(NLS::Engine::GameObject).GetName(),
        "Cube",
        "Cube",
        ObjectRecordState::Alive,
        {
            {
                "components",
                PropertyValue::Array({
                    PropertyValue::OwnedReference(meshFilterId),
                    PropertyValue::OwnedReference(meshRendererId)
                })
            }
        },
        MakeLocalIdentifierInFile(gameObjectId)});
    document.objects.push_back(ObjectRecord{
        meshFilterId,
        NLS_TYPEOF(NLS::Engine::Components::MeshFilter).GetName(),
        "Cube MeshFilter",
        "Cube/MeshFilter",
        ObjectRecordState::Alive,
        {
            {
                "mesh",
                PropertyValue::ObjectReference(ObjectIdentifier::Asset(
                    NLS::Engine::Serialize::AssetId(primitiveGuid),
                    MakeLocalIdentifierInFile(primitiveGuid, "mesh:Cube"),
                    primitiveMeshPath))
            }
        },
        MakeLocalIdentifierInFile(meshFilterId)});
    document.objects.push_back(ObjectRecord{
        meshRendererId,
        NLS_TYPEOF(NLS::Engine::Components::MeshRenderer).GetName(),
        "Cube MeshRenderer",
        "Cube/MeshRenderer",
        ObjectRecordState::Alive,
        {},
        MakeLocalIdentifierInFile(meshRendererId)});

    return NLS::Engine::Serialize::ObjectGraphWriter::Write(document);
}

const NLS::Base::Profiling::PerformanceStageEntry* FindThumbnailPerformanceStage(
    const NLS::Base::Profiling::PerformanceStageStatsSnapshot& snapshot,
    const std::string& stageName)
{
    for (const auto& stage : snapshot.stages)
    {
        if (stage.domain == NLS::Base::Profiling::PerformanceStageDomain::Thumbnail &&
            stage.stageName == stageName)
        {
            return &stage;
        }
    }
    return nullptr;
}

size_t CountOpaqueColumnClusters(const NLS::Image& image)
{
    if (image.GetData() == nullptr || image.GetWidth() <= 0 || image.GetHeight() <= 0)
        return 0u;

    const auto channels = image.GetChannels();
    if (channels <= 0)
        return 0u;

    size_t clusters = 0u;
    bool previousColumnOpaque = false;
    const auto* pixels = image.GetData();
    for (int x = 0; x < image.GetWidth(); ++x)
    {
        bool columnOpaque = false;
        for (int y = 0; y < image.GetHeight(); ++y)
        {
            const auto pixelIndex =
                (static_cast<size_t>(y) * static_cast<size_t>(image.GetWidth()) + static_cast<size_t>(x)) *
                static_cast<size_t>(channels);
            const auto alpha = channels >= 4 ? pixels[pixelIndex + 3u] : 255u;
            if (alpha != 0u)
            {
                columnOpaque = true;
                break;
            }
        }

        if (columnOpaque && !previousColumnOpaque)
            ++clusters;
        previousColumnOpaque = columnOpaque;
    }
    return clusters;
}

size_t CountOpaquePixels(const NLS::Image& image)
{
    if (image.GetData() == nullptr || image.GetWidth() <= 0 || image.GetHeight() <= 0)
        return 0u;

    const auto channels = image.GetChannels();
    if (channels <= 0)
        return 0u;

    size_t count = 0u;
    const auto* pixels = image.GetData();
    for (int y = 0; y < image.GetHeight(); ++y)
    {
        for (int x = 0; x < image.GetWidth(); ++x)
        {
            const auto pixelIndex =
                (static_cast<size_t>(y) * static_cast<size_t>(image.GetWidth()) + static_cast<size_t>(x)) *
                static_cast<size_t>(channels);
            const auto alpha = channels >= 4 ? pixels[pixelIndex + 3u] : 255u;
            if (alpha != 0u)
                ++count;
        }
    }
    return count;
}

std::string FormatSerializationDiagnostics(
    const NLS::Engine::Serialize::SerializationDiagnosticList& diagnostics)
{
    std::string formatted;
    for (const auto& diagnostic : diagnostics.GetItems())
    {
        formatted += std::to_string(static_cast<int>(diagnostic.GetCode()));
        formatted += ":";
        formatted += std::to_string(static_cast<int>(diagnostic.GetSeverity()));
        formatted += ":";
        formatted += diagnostic.GetMessage();
        formatted += "\n";
    }
    return formatted;
}

double AverageOpaqueLuminance(const NLS::Image& image)
{
    if (image.GetData() == nullptr || image.GetWidth() <= 0 || image.GetHeight() <= 0)
        return 0.0;

    const auto channels = image.GetChannels();
    if (channels <= 0)
        return 0.0;

    const auto* pixels = image.GetData();
    double total = 0.0;
    size_t count = 0u;
    for (int y = 0; y < image.GetHeight(); ++y)
    {
        for (int x = 0; x < image.GetWidth(); ++x)
        {
            const auto pixelIndex =
                (static_cast<size_t>(y) * static_cast<size_t>(image.GetWidth()) + static_cast<size_t>(x)) *
                static_cast<size_t>(channels);
            const auto alpha = channels >= 4 ? pixels[pixelIndex + 3u] : 255u;
            if (alpha == 0u)
                continue;

            total +=
                0.2126 * static_cast<double>(pixels[pixelIndex + 0u]) +
                0.7152 * static_cast<double>(pixels[pixelIndex + 1u]) +
                0.0722 * static_cast<double>(pixels[pixelIndex + 2u]);
            ++count;
        }
    }

    return count > 0u ? total / static_cast<double>(count) : 0.0;
}

double OpaquePixelCoverage(const NLS::Image& image)
{
    if (image.GetWidth() <= 0 || image.GetHeight() <= 0)
        return 0.0;

    const auto totalPixels =
        static_cast<size_t>(image.GetWidth()) * static_cast<size_t>(image.GetHeight());
    return totalPixels == 0u
        ? 0.0
        : static_cast<double>(CountOpaquePixels(image)) / static_cast<double>(totalPixels);
}

void ExpectGeneratedFreshPng(
    const std::filesystem::path& root,
    NLS::Editor::Assets::AssetThumbnailRequest request,
    const int expectedWidth,
    const int expectedHeight)
{
    using namespace NLS::Editor::Assets;

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);
    ASSERT_TRUE(generated->cacheEntry.has_value());
    EXPECT_TRUE(std::filesystem::exists(generated->imagePath));

    const NLS::Image decoded(generated->imagePath.string(), false);
    EXPECT_EQ(decoded.GetWidth(), expectedWidth);
    EXPECT_EQ(decoded.GetHeight(), expectedHeight);
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Fresh);
    EXPECT_TRUE(IsAssetThumbnailCachePathContained(root, generated->imagePath));
}

void ExpectBackgroundPreviewGeneratesWithoutRenderer(
    const std::filesystem::path& root,
    const NLS::Editor::Assets::AssetThumbnailRequest& request)
{
    using namespace NLS::Editor::Assets;

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh)
        << generated->diagnostic << " for " << request.sourceAssetPath
        << " subAsset=" << request.subAssetKey
        << " artifact=" << request.artifactPath;
    ASSERT_TRUE(generated->cacheEntry.has_value());
    EXPECT_TRUE(std::filesystem::exists(generated->cacheEntry->imagePath))
        << generated->diagnostic << " for " << request.sourceAssetPath
        << " subAsset=" << request.subAssetKey
        << " artifact=" << request.artifactPath;
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Fresh)
        << generated->diagnostic << " for " << request.sourceAssetPath
        << " subAsset=" << request.subAssetKey
        << " artifact=" << request.artifactPath;
    EXPECT_TRUE(IsAssetThumbnailCachePathContained(root, generated->cacheEntry->metadataPath));
}

void ExpectGpuPreviewDefersWithoutRenderer(
    const NLS::Editor::Assets::AssetThumbnailRequest& request)
{
    using namespace NLS::Editor::Assets;

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

    const auto generated = service.GenerateNextThumbnail();
    EXPECT_FALSE(generated.has_value());
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Missing);
}

class CountingThumbnailPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest&) const override
    {
        ++supportsCount;
        return true;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++renderCount;
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.diagnostic = "test-renderer-called";
        return result;
    }

    mutable size_t supportsCount = 0u;
    size_t renderCount = 0u;
};

class HeavyOnlyThumbnailPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest& request) const override
    {
        ++supportsCount;
        return request.kind == NLS::Editor::Assets::AssetThumbnailKind::MaterialSphere ||
            request.kind == NLS::Editor::Assets::AssetThumbnailKind::PrefabPreview;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++renderCount;
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.diagnostic = "test-renderer-called";
        return result;
    }

    mutable size_t supportsCount = 0u;
    size_t renderCount = 0u;
};

class CapturingThumbnailPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest& request) const override
    {
        ++supportsCount;
        lastSupportsRequest = request;
        return true;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest& request) override
    {
        ++renderCount;
        lastRenderRequest = request;
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.width = 2u;
        result.height = 2u;
        result.rgbaPixels = {
            255u, 0u, 0u, 255u,
            0u, 255u, 0u, 255u,
            0u, 0u, 255u, 255u,
            255u, 255u, 255u, 255u
        };
        return result;
    }

    mutable size_t supportsCount = 0u;
    size_t renderCount = 0u;
    mutable std::optional<NLS::Editor::Assets::AssetThumbnailRequest> lastSupportsRequest;
    std::optional<NLS::Editor::Assets::AssetThumbnailRequest> lastRenderRequest;
};

class PendingThenReadyThumbnailPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest& request) const override
    {
        ++supportsCount;
        lastSupportsRequest = request;
        return true;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest& request) override
    {
        ++renderCount;
        lastRenderRequest = request;
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.width = 2u;
        result.height = 2u;
        if (renderCount == 1u)
        {
            result.diagnostic = "thumbnail-gpu-preview-readback-pending";
            return result;
        }

        result.rgbaPixels = {
            255u, 0u, 0u, 255u,
            0u, 255u, 0u, 255u,
            0u, 0u, 255u, 255u,
            255u, 255u, 255u, 255u
        };
        return result;
    }

    mutable size_t supportsCount = 0u;
    size_t renderCount = 0u;
    mutable std::optional<NLS::Editor::Assets::AssetThumbnailRequest> lastSupportsRequest;
    std::optional<NLS::Editor::Assets::AssetThumbnailRequest> lastRenderRequest;
};

class PrefabBudgetExceededThumbnailPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest& request) const override
    {
        return request.kind == NLS::Editor::Assets::AssetThumbnailKind::PrefabPreview;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest& request) override
    {
        ++renderCount;
        lastRenderRequest = request;
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.diagnostic = "thumbnail-prefab-preview-budget-exceeded";
        return result;
    }

    size_t renderCount = 0u;
    std::optional<NLS::Editor::Assets::AssetThumbnailRequest> lastRenderRequest;
};

class PendingMaterialThenKindColoredPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest& request) const override
    {
        return request.kind == NLS::Editor::Assets::AssetThumbnailKind::MaterialSphere ||
            request.kind == NLS::Editor::Assets::AssetThumbnailKind::PrefabPreview;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest& request) override
    {
        ++renderCount;
        lastRenderRequest = request;
        renderKinds.push_back(request.kind);
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.width = 2u;
        result.height = 2u;
        if (request.kind == NLS::Editor::Assets::AssetThumbnailKind::MaterialSphere &&
            !materialReturnedPending)
        {
            materialReturnedPending = true;
            result.diagnostic = "thumbnail-gpu-preview-readback-pending";
            return result;
        }

        const std::array<uint8_t, 4> color =
            request.kind == NLS::Editor::Assets::AssetThumbnailKind::MaterialSphere
                ? std::array<uint8_t, 4> {255u, 16u, 32u, 255u}
                : std::array<uint8_t, 4> {32u, 128u, 255u, 255u};
        result.rgbaPixels.reserve(16u);
        for (size_t pixel = 0u; pixel < 4u; ++pixel)
        {
            result.rgbaPixels.push_back(color[0]);
            result.rgbaPixels.push_back(color[1]);
            result.rgbaPixels.push_back(color[2]);
            result.rgbaPixels.push_back(color[3]);
        }
        return result;
    }

    bool materialReturnedPending = false;
    size_t renderCount = 0u;
    std::optional<NLS::Editor::Assets::AssetThumbnailRequest> lastRenderRequest;
    std::vector<NLS::Editor::Assets::AssetThumbnailKind> renderKinds;
};

class RejectingThumbnailPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest& request) const override
    {
        ++supportsCount;
        lastSupportsRequest = request;
        return false;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest& request) override
    {
        ++renderCount;
        lastRenderRequest = request;
        return {};
    }

    mutable size_t supportsCount = 0u;
    size_t renderCount = 0u;
    mutable std::optional<NLS::Editor::Assets::AssetThumbnailRequest> lastSupportsRequest;
    std::optional<NLS::Editor::Assets::AssetThumbnailRequest> lastRenderRequest;
};

void ExpectGpuPreviewDefersWithoutRenderer(
    const std::filesystem::path& root,
    NLS::Editor::Assets::AssetThumbnailRequest request)
{
    using namespace NLS::Editor::Assets;

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto generated = service.GenerateNextThumbnail();
    if (!generated.has_value())
    {
        EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);
        return;
    }
    ASSERT_TRUE(generated->cacheEntry.has_value());
    if (generated->status == AssetThumbnailServiceStatus::Fallback)
    {
        EXPECT_EQ(generated->diagnostic, "thumbnail-gpu-preview-renderer-unavailable");
        EXPECT_FALSE(std::filesystem::exists(generated->cacheEntry->imagePath));
        EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Missing);
    }
    else
    {
        EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh) << generated->diagnostic;
        EXPECT_TRUE(std::filesystem::exists(generated->cacheEntry->imagePath));
        EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Fresh);
    }
    EXPECT_TRUE(IsAssetThumbnailCachePathContained(root, generated->cacheEntry->metadataPath));
}

void ExpectGpuPreviewRejectsInvalidArtifactPath(
    NLS::Editor::Assets::AssetThumbnailRequest request,
    const std::string& expectedDiagnostic)
{
    using namespace NLS::Editor::Assets;

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto generated = service.GenerateNextThumbnail();
    if (!generated.has_value())
    {
        EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);
        return;
    }
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(generated->diagnostic, expectedDiagnostic);
}

std::string FileStampForTest(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return "missing";

    error.clear();
    const auto writeTime = std::filesystem::last_write_time(path, error);
    if (error)
        return "missing";

    return std::to_string(size) + ":" +
        std::to_string(static_cast<std::intmax_t>(writeTime.time_since_epoch().count()));
}

size_t CountArtifactTelemetryStage(
    const std::vector<NLS::Core::Assets::ArtifactLoadTelemetryRecord>& records,
    const NLS::Core::Assets::ArtifactLoadTelemetryStage stage)
{
    return static_cast<size_t>(std::count_if(
        records.begin(),
        records.end(),
        [stage](const NLS::Core::Assets::ArtifactLoadTelemetryRecord& record)
        {
            return record.stage == stage;
        }));
}

size_t CountArtifactTelemetryStageForPathSuffix(
    const std::vector<NLS::Core::Assets::ArtifactLoadTelemetryRecord>& records,
    const NLS::Core::Assets::ArtifactLoadTelemetryStage stage,
    const std::string& pathSuffix)
{
    return static_cast<size_t>(std::count_if(
        records.begin(),
        records.end(),
        [stage, &pathSuffix](const NLS::Core::Assets::ArtifactLoadTelemetryRecord& record)
        {
            return record.stage == stage &&
                record.path.size() >= pathSuffix.size() &&
                record.path.compare(record.path.size() - pathSuffix.size(), pathSuffix.size(), pathSuffix) == 0;
        }));
}
}

TEST(AssetThumbnailCacheTests, CacheKeyIncludesAssetIdentitySubAssetFreshnessAndSettings)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();

    const auto base = MakeThumbnailRequest(root, "prefab:Hero");
    auto differentSubAsset = base;
    differentSubAsset.subAssetKey = "mesh:Body";
    auto differentFreshness = base;
    differentFreshness.freshnessInputs[0].stamp = "source:v2";
    auto differentSize = base;
    differentSize.requestedSize = 128u;
    auto differentSettings = base;
    differentSettings.settingsFingerprint = "lighting:v2";

    EXPECT_NE(BuildAssetThumbnailCacheKey(base), BuildAssetThumbnailCacheKey(differentSubAsset));
    EXPECT_NE(BuildAssetThumbnailCacheKey(base), BuildAssetThumbnailCacheKey(differentFreshness));
    EXPECT_NE(BuildAssetThumbnailCacheKey(base), BuildAssetThumbnailCacheKey(differentSize));
    EXPECT_NE(BuildAssetThumbnailCacheKey(base), BuildAssetThumbnailCacheKey(differentSettings));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, CacheKeyInvalidatesWhenPreviewRendererVersionChanges)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();

    auto base = MakeThumbnailRequest(root, "prefab:Hero");
    base.previewRendererVersion = "preview-renderer:v1";
    base.settingsFingerprint = "lighting:v1";

    auto changedRenderer = base;
    changedRenderer.previewRendererVersion = "preview-renderer:v2";
    auto changedSettings = base;
    changedSettings.settingsFingerprint = "lighting:v2";

    EXPECT_NE(BuildAssetThumbnailCacheKey(base), BuildAssetThumbnailCacheKey(changedRenderer));
    EXPECT_NE(BuildAssetThumbnailCacheKey(base), BuildAssetThumbnailCacheKey(changedSettings));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, CacheKeyInvalidatesWhenDependencyColorSpaceOrHdrModeChanges)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();

    auto base = MakeThumbnailRequest(root, "prefab:Hero");
    base.previewRendererVersion = "preview-renderer:v1";
    base.settingsFingerprint = "lighting:v1";
    base.dependencyStamp = "deps:v1";
    base.colorSpaceMode = "linear";
    base.hdrMode = "ldr";

    auto changedDependency = base;
    changedDependency.dependencyStamp = "deps:v2";
    auto changedColorSpace = base;
    changedColorSpace.colorSpaceMode = "srgb";
    auto changedHdr = base;
    changedHdr.hdrMode = "hdr10";

    EXPECT_NE(BuildAssetThumbnailCacheKey(base), BuildAssetThumbnailCacheKey(changedDependency));
    EXPECT_NE(BuildAssetThumbnailCacheKey(base), BuildAssetThumbnailCacheKey(changedColorSpace));
    EXPECT_NE(BuildAssetThumbnailCacheKey(base), BuildAssetThumbnailCacheKey(changedHdr));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, CacheKeyIgnoresRequestSchedulingPriority)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto background = MakeThumbnailRequest(root, "prefab:Hero");
    background.priority = ThumbnailRequestPriority::Background;

    auto visible = background;
    visible.priority = ThumbnailRequestPriority::Visible;

    EXPECT_EQ(BuildAssetThumbnailCacheKey(background), BuildAssetThumbnailCacheKey(visible));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, CacheKeyLabelsIdentityAndFreshnessFieldsBeforeHashing)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();

    auto left = MakeThumbnailRequest(root, "field-a");
    left.sourceAssetPath = "Assets/field-b";
    left.artifactPath = "Library/Artifacts/field-c";
    left.freshnessInputs = {
        {"freshness-name", "freshness-stamp"},
        {"ambiguous", "value"}
    };

    auto swappedIdentity = left;
    swappedIdentity.sourceAssetPath = left.subAssetKey;
    swappedIdentity.subAssetKey = left.sourceAssetPath;

    auto swappedFreshness = left;
    swappedFreshness.freshnessInputs = {
        {"freshness-stamp", "freshness-name"},
        {"ambiguous", "value"}
    };

    EXPECT_NE(BuildAssetThumbnailCacheKey(left), BuildAssetThumbnailCacheKey(swappedIdentity));
    EXPECT_NE(BuildAssetThumbnailCacheKey(left), BuildAssetThumbnailCacheKey(swappedFreshness));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, CacheStatusStorageTokensAreExhaustiveAndRoundTrip)
{
    using namespace NLS::Editor::Assets;

    const auto& statuses = AssetThumbnailCacheStatusValues();
    ASSERT_EQ(statuses.size(), kAssetThumbnailCacheStatusCount);

    std::unordered_set<std::string> seenTokens;
    for (const auto status : statuses)
    {
        const auto token = AssetThumbnailCacheStatusStorageToken(status);
        ASSERT_NE(token, nullptr);
        if (status != AssetThumbnailCacheStatus::Missing)
            ASSERT_NE(std::string(token), "missing") << "Only Missing may use the missing token.";
        EXPECT_TRUE(seenTokens.insert(token).second) << token;

        const auto parsed = AssetThumbnailCacheStatusFromStorageToken(token);
        ASSERT_TRUE(parsed.has_value()) << token;
        EXPECT_EQ(*parsed, status) << token;
    }

    EXPECT_EQ(
        AssetThumbnailCacheStatusStorageToken(AssetThumbnailCacheStatus::Missing),
        std::string("missing"));
    EXPECT_FALSE(AssetThumbnailCacheStatusFromStorageToken("future-status").has_value());
}

TEST(AssetThumbnailCacheTests, CachePathsChangeWhenFreshnessChanges)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();

    const auto original = MakeThumbnailRequest(root, "prefab:Hero", "source:v1");
    const auto updated = MakeThumbnailRequest(root, "prefab:Hero", "source:v2");

    const auto originalEntry = ResolveAssetThumbnailCacheEntry(original);
    const auto updatedEntry = ResolveAssetThumbnailCacheEntry(updated);

    ASSERT_TRUE(originalEntry.has_value());
    ASSERT_TRUE(updatedEntry.has_value());
    EXPECT_NE(originalEntry->cacheKey, updatedEntry->cacheKey);
    EXPECT_NE(originalEntry->imagePath, updatedEntry->imagePath);
    EXPECT_NE(originalEntry->metadataPath, updatedEntry->metadataPath);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ResolvesContainedProjectLibraryPaths)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto entry = ResolveAssetThumbnailCacheEntry(MakeThumbnailRequest(root, "prefab:Hero"));

    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->imagePath.extension(), ".png");
    EXPECT_EQ(entry->metadataPath.extension(), ".json");
    EXPECT_TRUE(IsAssetThumbnailCachePathContained(root, entry->imagePath));
    EXPECT_TRUE(IsAssetThumbnailCachePathContained(root, entry->metadataPath));
    EXPECT_EQ(
        entry->imagePath.lexically_relative(root).begin()->generic_string(),
        std::string("Library"));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, RejectsCacheRootsThatResolveOutsideProject)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto outside = root.parent_path() / ("nullus_asset_thumbnail_outside_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(outside);
    std::filesystem::create_directories(root / "Library");

    std::error_code error;
    std::filesystem::create_directory_symlink(outside, root / "Library" / "AssetThumbnails", error);
    if (error)
    {
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(outside);
        GTEST_SKIP() << "Directory symlink creation is not available in this environment.";
    }

    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());
    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/Hero.png";
    request.kind = AssetThumbnailKind::Texture;

    EXPECT_FALSE(ResolveAssetThumbnailCacheEntry(request).has_value());
    EXPECT_FALSE(WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Fresh, {}));

    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside);
}

TEST(AssetThumbnailCacheTests, CacheFileWritesRejectDirectorySymlinkReplacement)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/Hero.png";
    request.kind = AssetThumbnailKind::Texture;
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());

    const auto outside = root.parent_path() / ("nullus_asset_thumbnail_write_outside_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(outside);
    std::filesystem::create_directories(entry->imagePath.parent_path().parent_path());

    std::error_code error;
    std::filesystem::create_directory_symlink(outside, entry->imagePath.parent_path(), error);
    if (error)
    {
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(outside);
        GTEST_SKIP() << "Directory symlink creation is not available in this environment.";
    }

    EXPECT_FALSE(WriteAssetThumbnailCacheFile(request, entry->imagePath, TinyPng()));
    EXPECT_FALSE(std::filesystem::exists(outside / entry->imagePath.filename()));

    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside);
}

TEST(AssetThumbnailCacheTests, CacheFileWritesAvoidPredictableSharedTempPath)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, "texture:body");
    request.kind = AssetThumbnailKind::Texture;
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());

    const auto legacyTempPath =
        entry->imagePath.parent_path() /
        ("." + entry->imagePath.filename().generic_string() + "." + entry->cacheKey + ".tmp");
    std::filesystem::create_directories(legacyTempPath);

    EXPECT_TRUE(WriteAssetThumbnailCacheFile(request, entry->imagePath, TinyPng()));
    EXPECT_TRUE(std::filesystem::is_regular_file(entry->imagePath));
    EXPECT_TRUE(std::filesystem::is_directory(legacyTempPath));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, CacheFileWritesRejectPreexistingTempSymlink)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, "texture:body");
    request.kind = AssetThumbnailKind::Texture;
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());

    const auto outside = root.parent_path() / ("nullus_asset_thumbnail_temp_symlink_outside_" + NLS::Guid::New().ToString());
    const auto outsideTarget = outside / "body.png";
    std::filesystem::create_directories(outside);
    WriteBinaryFile(outsideTarget, std::vector<uint8_t>{'o', 'l', 'd'});

    std::filesystem::create_directories(entry->imagePath.parent_path());
    const auto tempPrefix =
        "." + entry->imagePath.filename().generic_string() + "." + entry->cacheKey + ".";
    const auto tempPath =
        entry->imagePath.parent_path() /
        (tempPrefix + "thumbnail-temp-symlink-test.tmp");

    std::error_code error;
    std::filesystem::create_symlink(outsideTarget, tempPath, error);
    if (error)
    {
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(outside);
        GTEST_SKIP() << "File symlink creation is not available in this environment.";
    }

    EXPECT_FALSE(WriteAssetThumbnailCacheFile(request, tempPath, TinyPng()));
    EXPECT_EQ(std::filesystem::file_size(outsideTarget), 3u);
    EXPECT_TRUE(std::filesystem::is_symlink(tempPath));

    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside);
}

TEST(AssetThumbnailCacheTests, ReportsMissingFreshAndStaleEntries)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());
    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/Hero.png";
    request.kind = AssetThumbnailKind::Texture;
    auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());

    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Missing);

    WriteBinaryFile(entry->imagePath, TinyPng());
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Fresh, {}));
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Fresh);

    auto changed = request;
    changed.freshnessInputs[0].stamp = "source:v2";
    EXPECT_EQ(EvaluateAssetThumbnailCache(changed).status, AssetThumbnailCacheStatus::Missing);

    {
        std::ofstream output(entry->metadataPath, std::ios::binary | std::ios::trunc);
        output << nlohmann::json {
            {"cacheKey", "different-cache-key"},
            {"status", "fresh"}
        }.dump(2);
    }
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Stale);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ReportsFreshMetadataAsStaleWhenSourceMetaChanges)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto texturePath = root / "Assets" / "Textures" / "Hero.png";
    const auto metaPath = texturePath.string() + ".meta";
    WriteBinaryFile(texturePath, TinyPng());
    WriteTextFile(metaPath, "meta:v1");

    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/Hero.png";
    request.kind = AssetThumbnailKind::Texture;
    request.requestedSize = 64u;
    request.freshnessInputs = {{"source-meta", FileStampForTest(metaPath)}};
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());

    WriteBinaryFile(entry->imagePath, TinyPng());
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Fresh, {}));
    ASSERT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Fresh);

    const auto oldStamp = FileStampForTest(metaPath);
    for (int attempt = 0; attempt < 20 && FileStampForTest(metaPath) == oldStamp; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        WriteTextFile(metaPath, "meta:v2:" + std::to_string(attempt));
    }
    ASSERT_NE(FileStampForTest(metaPath), oldStamp);

    const auto evaluated = EvaluateAssetThumbnailCache(request);
    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Stale);
    EXPECT_EQ(evaluated.diagnostic, "thumbnail-cache-freshness-stale");

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, RejectsFreshMetadataPublishForCorruptPng)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());
    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/Hero.png";
    request.kind = AssetThumbnailKind::Texture;
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());

    WriteBinaryFile(entry->imagePath, std::vector<uint8_t>{'n', 'o', 't', '-', 'p', 'n', 'g'});
    EXPECT_FALSE(WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Fresh, {}));
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Missing);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ReportsLegacyFreshMetadataWithoutImageHashAsStale)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());
    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/Hero.png";
    request.kind = AssetThumbnailKind::Texture;
    request.requestedSize = 64u;
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());

    WriteBinaryFile(entry->imagePath, TinyPng());
    {
        std::filesystem::create_directories(entry->metadataPath.parent_path());
        std::ofstream output(entry->metadataPath, std::ios::binary | std::ios::trunc);
        output << nlohmann::json {
            {"cacheKey", entry->cacheKey},
            {"status", "fresh"}
        }.dump(2);
    }

    const auto evaluated = EvaluateAssetThumbnailCache(request);
    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Stale);
    EXPECT_EQ(evaluated.diagnostic, "thumbnail-cache-image-invalid");

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ReportsFreshMetadataWithMutatedCachedPngAsStale)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());
    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/Hero.png";
    request.kind = AssetThumbnailKind::Texture;
    request.requestedSize = 64u;
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());

    auto bytes = TinyPng();
    WriteBinaryFile(entry->imagePath, bytes);
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Fresh, {}));
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Fresh);

    ASSERT_GT(bytes.size(), 41u);
    bytes[41u] ^= 0xffu;
    WriteBinaryFile(entry->imagePath, bytes);

    const auto evaluated = EvaluateAssetThumbnailCache(request);
    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Stale);
    EXPECT_EQ(evaluated.diagnostic, "thumbnail-cache-image-invalid");

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, FastEvaluationSkipsFullImageHashButFullEvaluationDetectsMutation)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());
    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/Hero.png";
    request.kind = AssetThumbnailKind::Texture;
    request.requestedSize = 64u;
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());

    auto bytes = TinyPng();
    WriteBinaryFile(entry->imagePath, bytes);
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Fresh, {}));

    ASSERT_GT(bytes.size(), 41u);
    bytes[41u] ^= 0xffu;
    WriteBinaryFile(entry->imagePath, bytes);

    EXPECT_EQ(
        EvaluateAssetThumbnailCache(request, AssetThumbnailCacheIntegrityMode::Fast).status,
        AssetThumbnailCacheStatus::Fresh);
    const auto fullEvaluation =
        EvaluateAssetThumbnailCache(request, AssetThumbnailCacheIntegrityMode::Full);
    EXPECT_EQ(fullEvaluation.status, AssetThumbnailCacheStatus::Stale);
    EXPECT_EQ(fullEvaluation.diagnostic, "thumbnail-cache-image-invalid");

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, DiskCachePruneEnforcesEntryCapacityAndReportsEvictionStats)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    std::vector<AssetThumbnailRequest> requests;
    requests.reserve(3u);
    for (size_t index = 0u; index < 3u; ++index)
    {
        auto request = MakeThumbnailRequest(root, "texture:Hero" + std::to_string(index));
        request.sourceAssetPath = "Assets/Textures/Hero" + std::to_string(index) + ".png";
        request.kind = AssetThumbnailKind::Texture;
        request.freshnessInputs = {{"source", "tiny-png:v" + std::to_string(index)}};
        const auto entry = ResolveAssetThumbnailCacheEntry(request);
        ASSERT_TRUE(entry.has_value());
        ASSERT_TRUE(WriteAssetThumbnailCacheFile(request, entry->imagePath, TinyPng()));
        ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Fresh, {}));
        requests.push_back(request);
    }

    const auto newestEntry = ResolveAssetThumbnailCacheEntry(requests.back());
    ASSERT_TRUE(newestEntry.has_value());

    AssetThumbnailDiskCachePruneOptions options;
    options.maxEntries = 1u;
    options.maxBytes = UINT64_MAX;
    const auto pruned = PruneAssetThumbnailDiskCache(root, options);

    EXPECT_EQ(pruned.scannedEntries, 3u);
    EXPECT_EQ(pruned.removedEntries, 2u);
    EXPECT_EQ(pruned.remainingEntries, 1u);
    EXPECT_EQ(EvaluateAssetThumbnailCache(requests.back()).status, AssetThumbnailCacheStatus::Fresh);
    EXPECT_TRUE(std::filesystem::exists(newestEntry->imagePath));
    EXPECT_TRUE(std::filesystem::exists(newestEntry->metadataPath));

    for (size_t index = 0u; index + 1u < requests.size(); ++index)
    {
        const auto entry = ResolveAssetThumbnailCacheEntry(requests[index]);
        ASSERT_TRUE(entry.has_value());
        EXPECT_FALSE(std::filesystem::exists(entry->imagePath));
        EXPECT_FALSE(std::filesystem::exists(entry->metadataPath));
    }

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, DiskCachePruneEnforcesByteCapacityAndReportsRemainingBytes)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    std::vector<AssetThumbnailRequest> requests;
    requests.reserve(2u);
    uint64_t newestEntryBytes = 0u;
    for (size_t index = 0u; index < 2u; ++index)
    {
        auto request = MakeThumbnailRequest(root, "texture:Budget" + std::to_string(index));
        request.sourceAssetPath = "Assets/Textures/Budget" + std::to_string(index) + ".png";
        request.kind = AssetThumbnailKind::Texture;
        request.freshnessInputs = {{"source", "tiny-png:v" + std::to_string(index)}};
        const auto entry = ResolveAssetThumbnailCacheEntry(request);
        ASSERT_TRUE(entry.has_value());
        ASSERT_TRUE(WriteAssetThumbnailCacheFile(request, entry->imagePath, TinyPng()));
        ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Fresh, {}));
        if (index == 1u)
        {
            newestEntryBytes =
                static_cast<uint64_t>(std::filesystem::file_size(entry->imagePath)) +
                static_cast<uint64_t>(std::filesystem::file_size(entry->metadataPath));
        }
        requests.push_back(request);
    }

    AssetThumbnailDiskCachePruneOptions options;
    options.maxEntries = 10u;
    options.maxBytes = newestEntryBytes;
    const auto pruned = PruneAssetThumbnailDiskCache(root, options);

    EXPECT_EQ(pruned.scannedEntries, 2u);
    EXPECT_EQ(pruned.removedEntries, 1u);
    EXPECT_EQ(pruned.remainingEntries, 1u);
    EXPECT_LE(pruned.remainingBytes, options.maxBytes);
    EXPECT_EQ(EvaluateAssetThumbnailCache(requests.back()).status, AssetThumbnailCacheStatus::Fresh);
    EXPECT_EQ(EvaluateAssetThumbnailCache(requests.front()).status, AssetThumbnailCacheStatus::Missing);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceBuildsRequestsFromSourceAndGeneratedItems)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("a2020202-0202-4202-8202-020202020202"));

    AssetBrowserItem texture;
    texture.kind = AssetBrowserItemKind::SourceAsset;
    texture.type = AssetBrowserItemType::Texture;
    texture.assetId = assetId;
    texture.sourceAssetPath = "Assets/Textures/Hero.png";
    texture.subAssetKey = "texture:Hero";
    texture.artifactPath = "Library/Artifacts/83/830502920ce24978347054d7448f70f1490df1b667706700189bd1708ea89e22";

    const auto textureRequest = BuildAssetThumbnailRequestForItem(root, texture, 128u);
    ASSERT_TRUE(textureRequest.has_value());
    EXPECT_EQ(textureRequest->kind, AssetThumbnailKind::Texture);
    EXPECT_EQ(textureRequest->requestedSize, 96u);
    EXPECT_EQ(textureRequest->priority, ThumbnailRequestPriority::Background);
    EXPECT_EQ(textureRequest->previewRendererVersion, "asset-browser-thumbnail-renderer:v3");
    EXPECT_EQ(textureRequest->settingsFingerprint, "asset-browser-thumbnail:v15-lowres-image-thumbnails");
    EXPECT_FALSE(textureRequest->dependencyStamp.empty());
    EXPECT_EQ(textureRequest->colorSpaceMode, "linear");
    EXPECT_EQ(textureRequest->hdrMode, "ldr");
    EXPECT_EQ(textureRequest->sourceAssetPath, "Assets/Textures/Hero.png");
    EXPECT_EQ(textureRequest->subAssetKey, "texture:Hero");
    EXPECT_EQ(textureRequest->artifactPath, "Library/Artifacts/83/830502920ce24978347054d7448f70f1490df1b667706700189bd1708ea89e22");
    EXPECT_FALSE(textureRequest->freshnessInputs.empty());

    AssetBrowserItem material;
    material.kind = AssetBrowserItemKind::GeneratedSubAsset;
    material.type = AssetBrowserItemType::Material;
    material.assetId = assetId;
    material.sourceAssetPath = "Assets/Models/Hero.gltf";
    material.subAssetKey = "material:Body";
    material.artifactPath = "Library/Artifacts/47/47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae";
    material.artifactType = ArtifactType::Material;

    const auto materialRequest = BuildAssetThumbnailRequestForItem(root, material, 96u);
    ASSERT_TRUE(materialRequest.has_value());
    EXPECT_EQ(materialRequest->kind, AssetThumbnailKind::MaterialSphere);
    EXPECT_EQ(materialRequest->previewRendererVersion, "asset-browser-thumbnail-renderer:v3");
    EXPECT_EQ(materialRequest->settingsFingerprint, "asset-browser-thumbnail:v19-gpu-preview-textured-materials");
    EXPECT_FALSE(materialRequest->dependencyStamp.empty());
    EXPECT_EQ(materialRequest->colorSpaceMode, "linear");
    EXPECT_EQ(materialRequest->hdrMode, "ldr");
    EXPECT_EQ(materialRequest->sourceAssetPath, "Assets/Models/Hero.gltf");
    EXPECT_EQ(materialRequest->subAssetKey, "material:Body");
    EXPECT_EQ(materialRequest->artifactPath, "Library/Artifacts/47/47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae");

    AssetBrowserItem sourceMaterial;
    sourceMaterial.kind = AssetBrowserItemKind::SourceAsset;
    sourceMaterial.type = AssetBrowserItemType::Material;
    sourceMaterial.assetId = assetId;
    sourceMaterial.sourceAssetPath = "Assets/Materials/New.mat";

    const auto sourceMaterialRequest = BuildAssetThumbnailRequestForItem(root, sourceMaterial, 96u);
    ASSERT_TRUE(sourceMaterialRequest.has_value());
    EXPECT_EQ(sourceMaterialRequest->kind, AssetThumbnailKind::MaterialSphere);
    EXPECT_EQ(sourceMaterialRequest->sourceAssetPath, "Assets/Materials/New.mat");
    EXPECT_EQ(sourceMaterialRequest->subAssetKey, "material:New");
    EXPECT_TRUE(sourceMaterialRequest->artifactPath.empty());
    EXPECT_EQ(sourceMaterialRequest->settingsFingerprint, "asset-browser-thumbnail:v19-gpu-preview-textured-materials");

    AssetBrowserItem modelSource;
    modelSource.kind = AssetBrowserItemKind::SourceAsset;
    modelSource.type = AssetBrowserItemType::Model;
    modelSource.assetId = assetId;
    modelSource.sourceAssetPath = "Assets/Models/Hero.gltf";
    modelSource.subAssetKey = "prefab:Hero";
    modelSource.artifactPath = "Library/Artifacts/67/670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772";
    modelSource.artifactType = ArtifactType::Prefab;

    const auto modelRequest = BuildAssetThumbnailRequestForItem(root, modelSource, 96u);
    ASSERT_TRUE(modelRequest.has_value());
    EXPECT_EQ(modelRequest->kind, AssetThumbnailKind::PrefabPreview);
    EXPECT_EQ(modelRequest->previewRendererVersion, "asset-browser-thumbnail-renderer:v3");
    EXPECT_EQ(modelRequest->settingsFingerprint, "asset-browser-thumbnail:v22-prefab-mesh-set-preview");
    EXPECT_FALSE(modelRequest->dependencyStamp.empty());
    EXPECT_EQ(modelRequest->colorSpaceMode, "linear");
    EXPECT_EQ(modelRequest->hdrMode, "ldr");
    EXPECT_EQ(modelRequest->subAssetKey, "prefab:Hero");
    EXPECT_EQ(modelRequest->artifactPath, "Library/Artifacts/67/670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772");

    AssetBrowserItem mesh;
    mesh.kind = AssetBrowserItemKind::GeneratedSubAsset;
    mesh.type = AssetBrowserItemType::Mesh;
    mesh.assetId = assetId;
    mesh.sourceAssetPath = "Assets/Models/Hero.gltf";
    mesh.subAssetKey = "mesh:Body";
    mesh.artifactPath = "Library/Artifacts/36/36eee85124b95361c55a48634e6956a87607d0b6a69bfd04ffcd04f145ffa8d7";
    mesh.artifactType = ArtifactType::Mesh;
    const auto meshRequest = BuildAssetThumbnailRequestForItem(root, mesh, 96u);
    ASSERT_TRUE(meshRequest.has_value());
    EXPECT_EQ(meshRequest->kind, AssetThumbnailKind::ModelPreview);
    EXPECT_EQ(meshRequest->previewRendererVersion, "asset-browser-thumbnail-renderer:v3");
    EXPECT_EQ(meshRequest->settingsFingerprint, "asset-browser-thumbnail:v19-gpu-preview-textured-materials");
    EXPECT_FALSE(meshRequest->dependencyStamp.empty());
    EXPECT_EQ(meshRequest->colorSpaceMode, "linear");
    EXPECT_EQ(meshRequest->hdrMode, "ldr");
    EXPECT_EQ(meshRequest->artifactPath, "Library/Artifacts/36/36eee85124b95361c55a48634e6956a87607d0b6a69bfd04ffcd04f145ffa8d7");

    AssetBrowserItem prefab;
    prefab.kind = AssetBrowserItemKind::SourceAsset;
    prefab.type = AssetBrowserItemType::Prefab;
    prefab.assetId = assetId;
    prefab.sourceAssetPath = "Assets/Prefabs/Lamp.prefab";
    prefab.subAssetKey = "prefab:Lamp";
    prefab.artifactPath = "Library/Artifacts/67/670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772";
    prefab.artifactType = ArtifactType::Prefab;
    const auto prefabRequest = BuildAssetThumbnailRequestForItem(root, prefab, 96u);
    ASSERT_TRUE(prefabRequest.has_value());
    EXPECT_EQ(prefabRequest->kind, AssetThumbnailKind::PrefabPreview);
    EXPECT_EQ(prefabRequest->previewRendererVersion, "asset-browser-thumbnail-renderer:v3");
    EXPECT_EQ(prefabRequest->settingsFingerprint, "asset-browser-thumbnail:v22-prefab-mesh-set-preview");
    EXPECT_FALSE(prefabRequest->dependencyStamp.empty());
    EXPECT_EQ(prefabRequest->colorSpaceMode, "linear");
    EXPECT_EQ(prefabRequest->hdrMode, "ldr");
    EXPECT_EQ(
        prefabRequest->artifactPath,
        "Library/Artifacts/67/670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772");

    AssetBrowserItem folder;
    folder.kind = AssetBrowserItemKind::Folder;
    folder.type = AssetBrowserItemType::Folder;
    EXPECT_FALSE(BuildAssetThumbnailRequestForItem(root, folder, 96u).has_value());

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceBuildsSourceModelPrefabPreviewRequestFromManifest)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("b3030303-0303-4303-8303-030303030303"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot);
    const auto manifest =
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"scene-model\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"prefab:Hero\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"prefab:Hero\","
        "\"artifactType\":\"Prefab\","
        "\"loaderId\":\"native-prefab\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/Hero.nprefab\","
        "\"contentHash\":\"prefab-hash\","
        "\"displayName\":\"Hero\""
        "}"
        "]"
        "}";
    WriteTextFile(artifactRoot / "manifest.json", manifest);
    WriteTextFile(artifactRoot / "Hero.nprefab", "prefab artifact v1");

    AssetBrowserItem modelSource;
    modelSource.kind = AssetBrowserItemKind::SourceAsset;
    modelSource.type = AssetBrowserItemType::Model;
    modelSource.assetId = assetId;
    modelSource.sourceAssetPath = "Assets/Models/Hero.fbx";

    const auto modelRequest = BuildAssetThumbnailRequestForItem(root, modelSource, 96u);
    ASSERT_TRUE(modelRequest.has_value());
    EXPECT_EQ(modelRequest->kind, AssetThumbnailKind::PrefabPreview);
    EXPECT_EQ(modelRequest->subAssetKey, "prefab:Hero");
    EXPECT_TRUE(NLS::Core::Assets::IsContentStorageArtifactPath(modelRequest->artifactPath));
    const auto hasArtifactFileFreshness = std::any_of(
        modelRequest->freshnessInputs.begin(),
        modelRequest->freshnessInputs.end(),
        [](const AssetThumbnailFreshnessInput& input)
        {
            return input.name == "artifact-file";
        });
    EXPECT_TRUE(hasArtifactFileFreshness);
    EXPECT_NE(modelRequest->dependencyStamp.find("artifact-file="), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceBuildsKnownModelThumbnailRequestWithoutLoadingManifest)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("b4040404-0404-4404-8404-040404040404"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot);
    WriteBinaryFile(
        root / "Assets" / "Models" / "Heavy.fbx",
        std::vector<uint8_t>((1024u * 1024u) + 1u, 'F'));
    WriteTextFile(artifactRoot / "Heavy.nprefab", "prefab artifact v1");
    WriteTextFile(artifactRoot / "manifest.json", R"({"subAssets":[]})");

    AssetBrowserItem modelSource;
    modelSource.kind = AssetBrowserItemKind::SourceAsset;
    modelSource.type = AssetBrowserItemType::Model;
    modelSource.assetId = assetId;
    modelSource.sourceAssetPath = "Assets/Models/Heavy.fbx";
    modelSource.subAssetKey = "prefab:Heavy";
    modelSource.artifactPath = RedirectedArtifactPathOrFallback(
        "Library/Artifacts/" + assetId.ToString() + "/Heavy.nprefab");
    modelSource.artifactType = ArtifactType::Prefab;

    AssetThumbnailRequestBuildContext context;
    const auto modelRequest = BuildAssetThumbnailRequestForItem(root, modelSource, 96u, context);
    ASSERT_TRUE(modelRequest.has_value());
    EXPECT_EQ(modelRequest->kind, AssetThumbnailKind::PrefabPreview);
    EXPECT_EQ(modelRequest->subAssetKey, "prefab:Heavy");
    EXPECT_TRUE(NLS::Core::Assets::IsContentStorageArtifactPath(modelRequest->artifactPath));
    EXPECT_TRUE(context.artifactManifestsByAssetId.empty())
        << "Entering a folder with a large model must not parse its artifact manifest when "
           "the asset database item already carries the resolved prefab artifact identity.";
    EXPECT_NE(modelRequest->dependencyStamp.find("artifact-db="), std::string::npos);
    EXPECT_NE(modelRequest->dependencyStamp.find("artifact-file="), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceCanDeferModelManifestLookupDuringThumbnailScopeBuild)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("b5050505-0505-4505-8505-050505050505"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot);
    WriteTextFile(root / "Assets" / "Models" / "Heavy.fbx", "large model source");
    WriteTextFile(artifactRoot / "manifest.json", R"({"subAssets":[{"subAssetKey":"prefab:Heavy","artifactType":"Prefab","artifactPath":"Library/Artifacts/ignored/Heavy.nprefab"}]})");

    AssetBrowserItem modelSource;
    modelSource.kind = AssetBrowserItemKind::SourceAsset;
    modelSource.type = AssetBrowserItemType::Model;
    modelSource.assetId = assetId;
    modelSource.sourceAssetPath = "Assets/Models/Heavy.fbx";

    AssetThumbnailRequestBuildContext context;
    context.deferManifestLookups = true;
    const auto modelRequest = BuildAssetThumbnailRequestForItem(root, modelSource, 96u, context);
    ASSERT_TRUE(modelRequest.has_value());
    EXPECT_EQ(modelRequest->kind, AssetThumbnailKind::PrefabPreview);
    EXPECT_TRUE(modelRequest->subAssetKey.empty());
    EXPECT_TRUE(modelRequest->artifactPath.empty());
    EXPECT_TRUE(context.artifactManifestsByAssetId.empty());
    EXPECT_NE(modelRequest->dependencyStamp.find("artifact-db="), std::string::npos);
    EXPECT_EQ(modelRequest->dependencyStamp.find("artifact-file="), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceResolvesDeferredModelManifestRequestWhenGeneratingThumbnail)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("b6060606-0606-4606-8606-060606060606"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot);
    WriteTextFile(root / "Assets" / "Models" / "Heavy.fbx", "large model source");
    WriteNativeArtifactTextFile(
        artifactRoot / "Heavy.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        MinimalPrefabPayload());
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"scene-model\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"prefab:Heavy\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"prefab:Heavy\","
        "\"artifactType\":\"Prefab\","
        "\"loaderId\":\"native-prefab\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/Heavy.nprefab\","
        "\"contentHash\":\"prefab-hash\","
        "\"displayName\":\"Heavy\""
        "}"
        "]"
        "}");

    AssetBrowserItem modelSource;
    modelSource.kind = AssetBrowserItemKind::SourceAsset;
    modelSource.type = AssetBrowserItemType::Model;
    modelSource.assetId = assetId;
    modelSource.sourceAssetPath = "Assets/Models/Heavy.fbx";

    AssetThumbnailRequestBuildContext context;
    context.deferManifestLookups = true;
    const auto deferredRequest = BuildAssetThumbnailRequestForItem(root, modelSource, 48u, context);
    ASSERT_TRUE(deferredRequest.has_value());
    ASSERT_TRUE(deferredRequest->subAssetKey.empty());
    ASSERT_TRUE(deferredRequest->artifactPath.empty());

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(*deferredRequest).status, AssetThumbnailServiceStatus::Pending);
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh) << generated->diagnostic;
    EXPECT_EQ(service.GetThumbnailState(*deferredRequest), ThumbnailState::Ready);
    EXPECT_EQ(service.GetThumbnailState(*deferredRequest), ThumbnailState::Ready);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, DeferredSourceModelPrefabPreviewUsesManifestPrimaryArtifactNotSourceFile)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("b7070707-0707-4707-8707-070707070707"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot);
    WriteBinaryFile(
        root / "Assets" / "Models" / "Heavy.fbx",
        std::vector<uint8_t>((1024u * 1024u) + 1u, 'F'));
    WriteNativeArtifactTextFile(
        artifactRoot / "Heavy.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        MinimalPrefabPayload());
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"scene-model\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"prefab:Heavy\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"prefab:Heavy\","
        "\"artifactType\":\"Prefab\","
        "\"loaderId\":\"native-prefab\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/Heavy.nprefab\","
        "\"contentHash\":\"prefab-hash\","
        "\"displayName\":\"Heavy\""
        "}"
        "]"
        "}");

    AssetBrowserItem modelSource;
    modelSource.kind = AssetBrowserItemKind::SourceAsset;
    modelSource.type = AssetBrowserItemType::Model;
    modelSource.assetId = assetId;
    modelSource.sourceAssetPath = "Assets/Models/Heavy.fbx";

    AssetThumbnailRequestBuildContext context;
    context.deferManifestLookups = true;
    const auto deferredRequest = BuildAssetThumbnailRequestForItem(root, modelSource, 48u, context);
    ASSERT_TRUE(deferredRequest.has_value());
    ASSERT_TRUE(deferredRequest->subAssetKey.empty());
    ASSERT_TRUE(deferredRequest->artifactPath.empty());

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(*deferredRequest).status, AssetThumbnailServiceStatus::Pending);
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh) << generated->diagnostic;
    EXPECT_EQ(service.GetThumbnailState(*deferredRequest), ThumbnailState::Ready);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, DeferredSourceModelGpuPreviewUsesManifestPrimaryPrefabArtifact)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("b8080808-0808-4808-8808-080808080808"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot);
    WriteBinaryFile(
        root / "Assets" / "Models" / "Heavy.fbx",
        std::vector<uint8_t>((1024u * 1024u) + 1u, 'F'));
    WriteNativeArtifactTextFile(
        artifactRoot / "Heavy.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        MinimalPrefabPayload());
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"scene-model\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"prefab:Heavy\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"prefab:Heavy\","
        "\"artifactType\":\"Prefab\","
        "\"loaderId\":\"native-prefab\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/Heavy.nprefab\","
        "\"contentHash\":\"prefab-hash\","
        "\"displayName\":\"Heavy\""
        "}"
        "]"
        "}");

    AssetBrowserItem modelSource;
    modelSource.kind = AssetBrowserItemKind::SourceAsset;
    modelSource.type = AssetBrowserItemType::Model;
    modelSource.assetId = assetId;
    modelSource.sourceAssetPath = "Assets/Models/Heavy.fbx";

    AssetThumbnailRequestBuildContext context;
    context.deferManifestLookups = true;
    const auto deferredRequest = BuildAssetThumbnailRequestForItem(root, modelSource, 48u, context);
    ASSERT_TRUE(deferredRequest.has_value());
    ASSERT_TRUE(deferredRequest->subAssetKey.empty());
    ASSERT_TRUE(deferredRequest->artifactPath.empty());

    CapturingThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(*deferredRequest).status, AssetThumbnailServiceStatus::Pending);
    const auto pending = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->status, AssetThumbnailServiceStatus::Pending);
    ASSERT_TRUE(renderer.lastSupportsRequest.has_value());
    EXPECT_EQ(renderer.lastSupportsRequest->subAssetKey, "prefab:Heavy");
    EXPECT_TRUE(NLS::Core::Assets::IsContentStorageArtifactPath(renderer.lastSupportsRequest->artifactPath));
    ASSERT_TRUE(renderer.lastRenderRequest.has_value());
    EXPECT_EQ(renderer.lastRenderRequest->subAssetKey, "prefab:Heavy");
    EXPECT_TRUE(NLS::Core::Assets::IsContentStorageArtifactPath(renderer.lastRenderRequest->artifactPath));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, DeferredGeneratedSubAssetRequestsResolveTheirOwnArtifactsWhenGeneratingThumbnail)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("b9090909-0909-4909-8909-090909090909"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot);
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteBinaryFile(
        artifactRoot / "meshes" / "Body.nmesh",
        NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));
    WriteNativeArtifactTextFile(
        artifactRoot / "materials" / "Body.nmat",
        ArtifactType::Material,
        "material",
        1u,
        "<root><name>Body</name><uniform name=\"u_Albedo\" type=\"vec4\" value=\"0.8 0.2 0.1 1\"/></root>");
    WriteBinaryFile(
        artifactRoot / "textures" / "Albedo.ntex",
        NLS::Render::Assets::SerializeTextureArtifact(RgbaTextureArtifact2x1()));
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"scene-model\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"prefab:Hero\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"mesh:Body\","
        "\"artifactType\":\"Mesh\","
        "\"loaderId\":\"mesh\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/meshes/Body.nmesh\","
        "\"contentHash\":\"mesh-hash\""
        "},"
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"material:Body\","
        "\"artifactType\":\"Material\","
        "\"loaderId\":\"material\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/materials/Body.nmat\","
        "\"contentHash\":\"material-hash\""
        "},"
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"texture:Albedo\","
        "\"artifactType\":\"Texture\","
        "\"loaderId\":\"texture\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/textures/Albedo.ntex\","
        "\"contentHash\":\"texture-hash\""
        "}"
        "]"
        "}");

    const auto makeDeferredItemRequest = [&](const std::string& subAssetKey, const AssetBrowserItemType type)
    {
        AssetBrowserItem item;
        item.kind = AssetBrowserItemKind::GeneratedSubAsset;
        item.type = type;
        item.assetId = assetId;
        item.sourceAssetPath = "Assets/Models/Hero.fbx";
        item.subAssetKey = subAssetKey;

        AssetThumbnailRequestBuildContext context;
        context.deferManifestLookups = true;
        auto request = BuildAssetThumbnailRequestForItem(root, item, 48u, context);
        EXPECT_TRUE(context.artifactManifestsByAssetId.empty());
        return request;
    };

    const auto meshRequest = makeDeferredItemRequest("mesh:Body", AssetBrowserItemType::Mesh);
    ASSERT_TRUE(meshRequest.has_value());
    ASSERT_TRUE(meshRequest->artifactPath.empty());
    ExpectBackgroundPreviewGeneratesWithoutRenderer(root, *meshRequest);

    const auto materialRequest = makeDeferredItemRequest("material:Body", AssetBrowserItemType::Material);
    ASSERT_TRUE(materialRequest.has_value());
    ASSERT_TRUE(materialRequest->artifactPath.empty());
    ExpectGpuPreviewDefersWithoutRenderer(*materialRequest);

    const auto textureRequest = makeDeferredItemRequest("texture:Albedo", AssetBrowserItemType::Texture);
    ASSERT_TRUE(textureRequest.has_value());
    ASSERT_TRUE(textureRequest->artifactPath.empty());
    ExpectBackgroundPreviewGeneratesWithoutRenderer(root, *textureRequest);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, DeferredGeneratedSubAssetRoutesMeshToCpuAndMaterialToGpu)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("ba0a0a0a-0a0a-4a0a-8a0a-0a0a0a0a0a0a"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot);
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteBinaryFile(
        artifactRoot / "meshes" / "Body.nmesh",
        NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));
    WriteNativeArtifactTextFile(
        artifactRoot / "materials" / "Body.nmat",
        ArtifactType::Material,
        "material",
        1u,
        "<root><name>Body</name><uniform name=\"u_Albedo\" type=\"vec4\" value=\"0.8 0.2 0.1 1\"/></root>");
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"scene-model\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"prefab:Hero\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"mesh:Body\","
        "\"artifactType\":\"Mesh\","
        "\"loaderId\":\"mesh\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/meshes/Body.nmesh\","
        "\"contentHash\":\"mesh-hash\""
        "},"
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"material:Body\","
        "\"artifactType\":\"Material\","
        "\"loaderId\":\"material\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/materials/Body.nmat\","
        "\"contentHash\":\"material-hash\""
        "}"
        "]"
        "}");

    const auto buildDeferredRequest = [&](const std::string& subAssetKey, const AssetBrowserItemType type)
    {
        AssetBrowserItem item;
        item.kind = AssetBrowserItemKind::GeneratedSubAsset;
        item.type = type;
        item.assetId = assetId;
        item.sourceAssetPath = "Assets/Models/Hero.fbx";
        item.subAssetKey = subAssetKey;

        AssetThumbnailRequestBuildContext context;
        context.deferManifestLookups = true;
        auto request = BuildAssetThumbnailRequestForItem(root, item, 48u, context);
        EXPECT_TRUE(context.artifactManifestsByAssetId.empty());
        return request;
    };

    {
        const auto meshRequest = buildDeferredRequest("mesh:Body", AssetBrowserItemType::Mesh);
        ASSERT_TRUE(meshRequest.has_value());
        ASSERT_TRUE(meshRequest->artifactPath.empty());

        CapturingThumbnailPreviewRenderer renderer;
        AssetThumbnailService service;
        ASSERT_EQ(service.GetThumbnail(*meshRequest).status, AssetThumbnailServiceStatus::Pending);
        const auto skippedGpu = service.GenerateNextThumbnail(renderer, true);
        EXPECT_FALSE(skippedGpu.has_value());
        EXPECT_EQ(renderer.supportsCount, 0u);
        EXPECT_EQ(renderer.renderCount, 0u);
        EXPECT_EQ(service.GetThumbnailState(*meshRequest), ThumbnailState::Queued);

        const auto generated = service.GenerateNextThumbnail();
        ASSERT_TRUE(generated.has_value());
        EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh) << generated->diagnostic;
        EXPECT_EQ(service.GetThumbnailState(*meshRequest), ThumbnailState::Ready);
    }

    {
        const auto materialRequest = buildDeferredRequest("material:Body", AssetBrowserItemType::Material);
        ASSERT_TRUE(materialRequest.has_value());
        ASSERT_TRUE(materialRequest->artifactPath.empty());

        CapturingThumbnailPreviewRenderer renderer;
        AssetThumbnailService service;
        ASSERT_EQ(service.GetThumbnail(*materialRequest).status, AssetThumbnailServiceStatus::Pending);
        const auto pending = service.GenerateNextThumbnail(renderer, true);
        ASSERT_TRUE(pending.has_value());
        EXPECT_EQ(pending->status, AssetThumbnailServiceStatus::Pending);
        ASSERT_TRUE(renderer.lastSupportsRequest.has_value());
        EXPECT_EQ(renderer.lastSupportsRequest->subAssetKey, "material:Body");
        EXPECT_TRUE(NLS::Core::Assets::IsContentStorageArtifactPath(renderer.lastSupportsRequest->artifactPath));
        ASSERT_TRUE(renderer.lastRenderRequest.has_value());
        EXPECT_EQ(renderer.lastRenderRequest->subAssetKey, "material:Body");
        EXPECT_TRUE(NLS::Core::Assets::IsContentStorageArtifactPath(renderer.lastRenderRequest->artifactPath));
    }

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, LightGpuPumpSkipsPrefabHeavyPreviewAndLetsCpuSubMeshProgress)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("c0ffee00-1000-4000-8000-000000000001"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteBinaryFile(
        artifactRoot / "meshes" / "Body.nmesh",
        NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));
    WriteNativeArtifactTextFile(
        artifactRoot / "Hero.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        PrefabPayloadWithSingleRendererDependency(assetId, "mesh:Body"));
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"scene-model\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"prefab:Hero\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"prefab:Hero\","
        "\"artifactType\":\"Prefab\","
        "\"loaderId\":\"native-prefab\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/Hero.nprefab\","
        "\"contentHash\":\"prefab-hash\""
        "},"
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"mesh:Body\","
        "\"artifactType\":\"Mesh\","
        "\"loaderId\":\"mesh\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/meshes/Body.nmesh\","
        "\"contentHash\":\"mesh-hash\""
        "}"
        "]"
        "}");

    auto prefabRequest = MakeThumbnailRequest(root, "prefab:Hero");
    prefabRequest.assetId = assetId;
    prefabRequest.sourceAssetPath = "Assets/Models/Hero.fbx";
    prefabRequest.artifactPath = RedirectedArtifactPathOrFallback(
        "Library/Artifacts/" + assetId.ToString() + "/Hero.nprefab");
    prefabRequest.kind = AssetThumbnailKind::PrefabPreview;
    prefabRequest.requestedSize = 48u;
    prefabRequest.priority = ThumbnailRequestPriority::Visible;
    prefabRequest.freshnessInputs = {{"artifact", "prefab:v1"}};

    auto meshRequest = MakeThumbnailRequest(root, "mesh:Body");
    meshRequest.assetId = assetId;
    meshRequest.sourceAssetPath = "Assets/Models/Hero.fbx";
    meshRequest.artifactPath = RedirectedArtifactPathOrFallback(
        "Library/Artifacts/" + assetId.ToString() + "/meshes/Body.nmesh");
    meshRequest.kind = AssetThumbnailKind::ModelPreview;
    meshRequest.requestedSize = 48u;
    meshRequest.priority = ThumbnailRequestPriority::Visible;
    meshRequest.freshnessInputs = {{"artifact", "mesh:v1"}};

    AssetThumbnailService service;
    CapturingThumbnailPreviewRenderer renderer;
    ASSERT_EQ(service.GetThumbnail(prefabRequest).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetThumbnail(meshRequest).status, AssetThumbnailServiceStatus::Pending);

    const auto lightGpu = service.GenerateNextThumbnail(renderer, false);
    ASSERT_TRUE(lightGpu.has_value());
    EXPECT_EQ(lightGpu->status, AssetThumbnailServiceStatus::Pending) << lightGpu->diagnostic;
    EXPECT_EQ(lightGpu->diagnostic, "thumbnail-gpu-preview-cache-write-pending");
    EXPECT_EQ(renderer.supportsCount, 1u);
    EXPECT_EQ(renderer.renderCount, 1u);
    EXPECT_EQ(service.GetThumbnailState(prefabRequest), ThumbnailState::Queued);
    EXPECT_EQ(service.GetThumbnailState(meshRequest), ThumbnailState::Readback);

    const auto meshGenerated = service.GenerateNextThumbnail();
    EXPECT_FALSE(meshGenerated.has_value());
    EXPECT_EQ(service.GetThumbnailState(meshRequest), ThumbnailState::Readback);
    EXPECT_EQ(service.GetThumbnailState(prefabRequest), ThumbnailState::Queued);
}

TEST(AssetThumbnailCacheTests, LightGpuPumpReachesMaterialPreviewBehindQueuedPrefabPreviews)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteTextFile(root / "Assets" / "Materials" / "New.mat", "<root><name>New</name></root>");

    {
        AssetThumbnailService service;
        CapturingThumbnailPreviewRenderer renderer;

        for (size_t index = 0u; index < 12u; ++index)
        {
            auto prefabRequest = MakeThumbnailRequest(root, "prefab:Queued" + std::to_string(index));
            prefabRequest.sourceAssetPath = "Assets/Prefabs/Queued" + std::to_string(index) + ".prefab";
            prefabRequest.kind = AssetThumbnailKind::PrefabPreview;
            prefabRequest.priority = ThumbnailRequestPriority::Visible;
            prefabRequest.freshnessInputs = {{"source", "prefab:" + std::to_string(index)}};
            ASSERT_EQ(service.GetThumbnail(prefabRequest).status, AssetThumbnailServiceStatus::Pending);
        }

        auto materialRequest = MakeThumbnailRequest(root, "material:New");
        materialRequest.sourceAssetPath = "Assets/Materials/New.mat";
        materialRequest.artifactPath.clear();
        materialRequest.kind = AssetThumbnailKind::MaterialSphere;
        materialRequest.priority = ThumbnailRequestPriority::Visible;
        materialRequest.freshnessInputs = {{"source", "material-source:v1"}};
        ASSERT_EQ(service.GetThumbnail(materialRequest).status, AssetThumbnailServiceStatus::Pending);

        const auto generated = service.GenerateNextThumbnail(renderer, false);
        ASSERT_TRUE(generated.has_value());
        EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Pending);
        ASSERT_TRUE(renderer.lastRenderRequest.has_value());
        EXPECT_EQ(renderer.lastRenderRequest->kind, AssetThumbnailKind::MaterialSphere);
        EXPECT_EQ(renderer.lastRenderRequest->sourceAssetPath, "Assets/Materials/New.mat");
        EXPECT_EQ(renderer.renderCount, 1u);
        EXPECT_EQ(service.GetThumbnailState(materialRequest), ThumbnailState::Readback);
    }

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, PrefabSubAssetThumbnailIgnoresUnreferencedManifestMeshes)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("bb0b0b0b-0b0b-4b0b-8b0b-0b0b0b0b0b0b"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot);
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteBinaryFile(
        artifactRoot / "meshes" / "Small.nmesh",
        NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));
    WriteBinaryFile(
        artifactRoot / "meshes" / "UnrelatedHuge.nmesh",
        NLS::Render::Assets::SerializeMeshArtifact(OversizedMeshArtifact()));
    WriteNativeArtifactTextFile(
        artifactRoot / "Small.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        PrefabPayloadWithSingleRendererDependency(assetId, "mesh:Small"));
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"scene-model\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"prefab:Small\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"prefab:Small\","
        "\"artifactType\":\"Prefab\","
        "\"loaderId\":\"native-prefab\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/Small.nprefab\","
        "\"contentHash\":\"prefab-hash\""
        "},"
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"mesh:Small\","
        "\"artifactType\":\"Mesh\","
        "\"loaderId\":\"mesh\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/meshes/Small.nmesh\","
        "\"contentHash\":\"small-mesh-hash\""
        "},"
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"mesh:UnrelatedHuge\","
        "\"artifactType\":\"Mesh\","
        "\"loaderId\":\"mesh\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/meshes/UnrelatedHuge.nmesh\","
        "\"contentHash\":\"huge-mesh-hash\""
        "}"
        "]"
        "}");

    auto request = MakeThumbnailRequest(root, "prefab:Small");
    request.assetId = assetId;
    request.sourceAssetPath = "Assets/Models/Hero.fbx";
    request.artifactPath = RedirectedArtifactPathOrFallback(
        "Library/Artifacts/" + assetId.ToString() + "/Small.nprefab");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 48u;
    request.freshnessInputs = {{"artifact", "prefab-small:v1"}};

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh)
        << generated->diagnostic;
    EXPECT_TRUE(generated->diagnostic.empty());
    ASSERT_TRUE(generated->cacheEntry.has_value());
    EXPECT_TRUE(std::filesystem::exists(generated->cacheEntry->imagePath));
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Fresh);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, RendererPumpCapsDeferredRequestsWhenRendererRejectsHeavyPreviews)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    AssetThumbnailService service;

    constexpr size_t kRequestCount = 12u;
    for (size_t index = 0u; index < kRequestCount; ++index)
    {
        const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::NewDeterministic(
            "rejecting-thumbnail-renderer-deferred-" + std::to_string(index)));
        const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
        std::filesystem::create_directories(artifactRoot / "meshes");
        WriteBinaryFile(
            root / "Assets" / "Models" / ("Hero" + std::to_string(index) + ".fbx"),
            std::vector<uint8_t>{'f', 'b', 'x'});
        WriteBinaryFile(
            artifactRoot / "meshes" / "Body.nmesh",
            NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));
        WriteTextFile(
            artifactRoot / "manifest.json",
            "{"
            "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
            "\"importerId\":\"scene-model\","
            "\"importerVersion\":1,"
            "\"targetPlatform\":\"editor\","
            "\"primarySubAssetKey\":\"mesh:Body\","
            "\"subAssets\":["
            "{"
            "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
            "\"subAssetKey\":\"mesh:Body\","
            "\"artifactType\":\"Mesh\","
            "\"loaderId\":\"mesh\","
            "\"targetPlatform\":\"editor\","
            "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/meshes/Body.nmesh\","
            "\"contentHash\":\"mesh-hash-" + std::to_string(index) + "\""
            "}"
            "]"
            "}");

        AssetBrowserItem item;
        item.kind = AssetBrowserItemKind::GeneratedSubAsset;
        item.type = AssetBrowserItemType::Mesh;
        item.assetId = assetId;
        item.sourceAssetPath = "Assets/Models/Hero" + std::to_string(index) + ".fbx";
        item.subAssetKey = "mesh:Body";

        AssetThumbnailRequestBuildContext context;
        context.deferManifestLookups = true;
        const auto request = BuildAssetThumbnailRequestForItem(root, item, 48u, context);
        ASSERT_TRUE(request.has_value());
        ASSERT_TRUE(request->artifactPath.empty());
        ASSERT_EQ(service.GetThumbnail(*request).status, AssetThumbnailServiceStatus::Pending);
    }

    RejectingThumbnailPreviewRenderer renderer;
    const auto generated = service.GenerateNextThumbnail(renderer, true);
    EXPECT_FALSE(generated.has_value());
    EXPECT_LE(renderer.supportsCount, 8u);
    EXPECT_EQ(renderer.renderCount, 0u);
    EXPECT_EQ(service.GetQueuedRequestCount(), kRequestCount);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, RendererPumpDefersHeavyRequestsWithoutResolvingDeferredManifest)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;
    using namespace NLS::Base::Profiling;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("be0e0e0e-0e0e-4e0e-8e0e-0e0e0e0e0e0e"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot / "meshes");
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteBinaryFile(
        artifactRoot / "meshes" / "Body.nmesh",
        NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"scene-model\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"mesh:Body\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"mesh:Body\","
        "\"artifactType\":\"Mesh\","
        "\"loaderId\":\"mesh\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/meshes/Body.nmesh\","
        "\"contentHash\":\"mesh-hash\""
        "}"
        "]"
        "}");

    AssetBrowserItem item;
    item.kind = AssetBrowserItemKind::GeneratedSubAsset;
    item.type = AssetBrowserItemType::Mesh;
    item.assetId = assetId;
    item.sourceAssetPath = "Assets/Models/Hero.fbx";
    item.subAssetKey = "mesh:Body";

    AssetThumbnailRequestBuildContext context;
    context.deferManifestLookups = true;
    const auto request = BuildAssetThumbnailRequestForItem(root, item, 48u, context);
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(request->artifactPath.empty());

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(*request).status, AssetThumbnailServiceStatus::Pending);

    RejectingThumbnailPreviewRenderer renderer;
    PerformanceStageStats stats;
    {
        PerformanceStageStatsCapture capture(stats);
        const auto generated = service.GenerateNextThumbnail(renderer, false);
        ASSERT_FALSE(generated.has_value());
    }

    EXPECT_EQ(renderer.supportsCount, 0u);
    EXPECT_EQ(renderer.renderCount, 0u);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_EQ(FindThumbnailPerformanceStage(stats.Snapshot(), "ThumbnailManifestLookup"), nullptr)
        << "Skipping a CPU mesh preview from the GPU pump must not synchronously parse its manifest on the editor thread.";

    PerformanceStageStats heavyStats;
    {
        PerformanceStageStatsCapture capture(heavyStats);
        const auto generated = service.GenerateNextThumbnail(renderer, true);
        EXPECT_FALSE(generated.has_value());
    }

    EXPECT_EQ(renderer.supportsCount, 0u);
    EXPECT_EQ(renderer.renderCount, 0u);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_EQ(FindThumbnailPerformanceStage(heavyStats.Snapshot(), "ThumbnailManifestLookup"), nullptr)
        << "The GPU preview pump must leave CPU mesh previews queued without resolving deferred "
           "manifests on the editor thread when the request does not already carry an artifact path.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, RendererPumpRejectsOversizedDeferredManifestWithoutJsonParse)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;
    using namespace NLS::Base::Profiling;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("be7e7e7e-7e7e-4e7e-8e7e-7e7e7e7e7e7e"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    WriteBinaryFile(root / "Assets" / "Models" / "Huge.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteTextFile(artifactRoot / "manifest.json", std::string(2u * 1024u * 1024u, '{'));

    AssetBrowserItem item;
    item.kind = AssetBrowserItemKind::SourceAsset;
    item.type = AssetBrowserItemType::Model;
    item.assetId = assetId;
    item.sourceAssetPath = "Assets/Models/Huge.fbx";

    AssetThumbnailRequestBuildContext context;
    context.deferManifestLookups = true;
    const auto request = BuildAssetThumbnailRequestForItem(root, item, 48u, context);
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(request->artifactPath.empty());

    CapturingThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(*request).status, AssetThumbnailServiceStatus::Pending);

    PerformanceStageStats stats;
    {
        PerformanceStageStatsCapture capture(stats);
        const auto generated = service.GenerateNextThumbnail(renderer, true);
        EXPECT_TRUE(generated.has_value());
    }

    const auto snapshot = stats.Snapshot();
    const auto* manifestStage = FindThumbnailPerformanceStage(snapshot, "ThumbnailManifestLookup");
    EXPECT_EQ(manifestStage, nullptr)
        << "ArtifactDB-backed deferred manifests no longer parse per-source manifest.json files.";
    EXPECT_EQ(renderer.renderCount, 1u);
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, RendererPumpUsesResolvedArtifactPathWithoutManifestLookup)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;
    using namespace NLS::Base::Profiling;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("be1e1e1e-1e1e-4e1e-8e1e-1e1e1e1e1e1e"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot / "meshes");
    WriteBinaryFile(
        artifactRoot / "meshes" / "Body.nmesh",
        NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"scene-model\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"mesh:Body\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"mesh:Body\","
        "\"artifactType\":\"Mesh\","
        "\"loaderId\":\"mesh\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/meshes/Body.nmesh\","
        "\"contentHash\":\"mesh-hash\""
        "}"
        "]"
        "}");

    auto request = MakeThumbnailRequest(root, "mesh:Body");
    request.assetId = assetId;
    request.sourceAssetPath = "Assets/Models/Hero.fbx";
    request.artifactPath = "Library/Artifacts/" + assetId.ToString() + "/meshes/Body.nmesh";
    request.kind = AssetThumbnailKind::ModelPreview;
    request.requestedSize = 48u;

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    HeavyOnlyThumbnailPreviewRenderer renderer;
    PerformanceStageStats stats;
    {
        PerformanceStageStatsCapture capture(stats);
        const auto generated = service.GenerateNextThumbnail(renderer, true);
        EXPECT_FALSE(generated.has_value());
    }

    EXPECT_EQ(renderer.supportsCount, 1u);
    EXPECT_EQ(renderer.renderCount, 0u);
    EXPECT_EQ(FindThumbnailPerformanceStage(stats.Snapshot(), "ThumbnailManifestLookup"), nullptr)
        << "Resolved CPU mesh thumbnail requests must be skipped by the GPU pump without "
           "synchronously parsing the source manifest on the editor thread.";

    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh) << generated->diagnostic;

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, RendererReadbackPollingReusesResolvedRequestWithoutManifestLookup)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;
    using namespace NLS::Base::Profiling;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("be3e3e3e-3e3e-4e3e-8e3e-3e3e3e3e3e3e"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot / "meshes");
    std::filesystem::create_directories(artifactRoot / "materials");
    WriteBinaryFile(
        artifactRoot / "meshes" / "Body.nmesh",
        NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));
    WriteTextFile(
        artifactRoot / "materials" / "Body.nmat",
        "<root><name>Body</name></root>");
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"scene-model\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"mesh:Body\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"mesh:Body\","
        "\"artifactType\":\"Mesh\","
        "\"loaderId\":\"mesh\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/meshes/Body.nmesh\","
        "\"contentHash\":\"mesh-hash\""
        "},"
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"material:Body\","
        "\"artifactType\":\"Material\","
        "\"loaderId\":\"material\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/materials/Body.nmat\","
        "\"contentHash\":\"material-hash\""
        "}"
        "]"
        "}");

    AssetBrowserItem item;
    item.kind = AssetBrowserItemKind::GeneratedSubAsset;
    item.type = AssetBrowserItemType::Material;
    item.assetId = assetId;
    item.sourceAssetPath = "Assets/Models/Hero.fbx";
    item.subAssetKey = "material:Body";

    AssetThumbnailRequestBuildContext context;
    context.deferManifestLookups = true;
    const auto request = BuildAssetThumbnailRequestForItem(root, item, 48u, context);
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(request->artifactPath.empty());

    PendingThenReadyThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(*request).status, AssetThumbnailServiceStatus::Pending);
    const auto pending = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->diagnostic, "thumbnail-gpu-preview-readback-pending");
    EXPECT_EQ(service.GetThumbnailState(*request), ThumbnailState::WaitingForGpu);
    ASSERT_TRUE(renderer.lastRenderRequest.has_value());
    EXPECT_TRUE(NLS::Core::Assets::IsContentStorageArtifactPath(renderer.lastRenderRequest->artifactPath));

    PerformanceStageStats stats;
    {
        PerformanceStageStatsCapture capture(stats);
        const auto repolled = service.GenerateNextThumbnail(renderer, false);
        ASSERT_TRUE(repolled.has_value());
        EXPECT_EQ(repolled->status, AssetThumbnailServiceStatus::Pending);
    }

    EXPECT_EQ(renderer.renderCount, 2u);
    EXPECT_EQ(FindThumbnailPerformanceStage(stats.Snapshot(), "ThumbnailManifestLookup"), nullptr)
        << "Polling an already submitted GPU readback must reuse the resolved preview request "
           "instead of parsing the manifest again on the editor thread.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, DuplicateRequestDoesNotOverwriteGpuReadbackPollingState)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;
    using namespace NLS::Base::Profiling;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("be4e4e4e-4e4e-4e4e-8e4e-4e4e4e4e4e4e"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot / "materials");
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteBinaryFile(
        artifactRoot / "materials" / "Body.nmat",
        std::vector<uint8_t>{'<', 'm', 'a', 't', '/', '>'});
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"scene-model\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"material:Body\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"material:Body\","
        "\"artifactType\":\"Material\","
        "\"loaderId\":\"material\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/materials/Body.nmat\","
        "\"contentHash\":\"material-hash\""
        "}"
        "]"
        "}");

    AssetBrowserItem item;
    item.kind = AssetBrowserItemKind::GeneratedSubAsset;
    item.type = AssetBrowserItemType::Material;
    item.assetId = assetId;
    item.sourceAssetPath = "Assets/Models/Hero.fbx";
    item.subAssetKey = "material:Body";

    AssetThumbnailRequestBuildContext context;
    context.deferManifestLookups = true;
    const auto request = BuildAssetThumbnailRequestForItem(root, item, 48u, context);
    ASSERT_TRUE(request.has_value());

    PendingThenReadyThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(*request).status, AssetThumbnailServiceStatus::Pending);
    const auto pending = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(service.GetThumbnailState(*request), ThumbnailState::WaitingForGpu);

    EXPECT_EQ(service.GetThumbnail(*request).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetThumbnailState(*request), ThumbnailState::WaitingForGpu)
        << "Duplicate UI thumbnail requests must not demote an in-flight GPU readback to Queued.";

    PerformanceStageStats stats;
    {
        PerformanceStageStatsCapture capture(stats);
        const auto repolled = service.GenerateNextThumbnail(renderer, false);
        ASSERT_TRUE(repolled.has_value());
        EXPECT_EQ(repolled->status, AssetThumbnailServiceStatus::Pending);
    }

    EXPECT_EQ(renderer.renderCount, 2u);
    EXPECT_EQ(FindThumbnailPerformanceStage(stats.Snapshot(), "ThumbnailManifestLookup"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, GpuPumpPollsPendingReadbackBeforeStartingAnotherPreview)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();

    auto first = MakeThumbnailRequest(root, "material:First");
    first.sourceAssetPath = "Assets/Materials/First.mat";
    first.kind = AssetThumbnailKind::MaterialSphere;
    first.requestedSize = 48u;
    first.priority = ThumbnailRequestPriority::Visible;
    first.freshnessInputs = {{"source", "first:v1"}};

    auto second = MakeThumbnailRequest(root, "material:Second");
    second.sourceAssetPath = "Assets/Materials/Second.mat";
    second.kind = AssetThumbnailKind::MaterialSphere;
    second.requestedSize = 48u;
    second.priority = ThumbnailRequestPriority::Visible;
    second.freshnessInputs = {{"source", "second:v1"}};

    PendingThenReadyThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(first).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.RequestAssetPreview(second).status, AssetThumbnailServiceStatus::Pending);

    const auto pending = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->diagnostic, "thumbnail-gpu-preview-readback-pending");
    ASSERT_TRUE(renderer.lastRenderRequest.has_value());
    EXPECT_EQ(renderer.lastRenderRequest->subAssetKey, "material:First");
    EXPECT_EQ(service.GetThumbnailState(first), ThumbnailState::WaitingForGpu);

    const auto polled = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(polled.has_value());
    EXPECT_EQ(polled->status, AssetThumbnailServiceStatus::Pending);
    ASSERT_TRUE(renderer.lastRenderRequest.has_value());
    EXPECT_EQ(renderer.lastRenderRequest->subAssetKey, "material:First")
        << "A pending GPU readback must be polled before starting another preview; "
           "switching requests retires the renderer readback and repeats GPU work.";
    EXPECT_EQ(renderer.renderCount, 2u);
    EXPECT_EQ(service.GetThumbnailState(first), ThumbnailState::Readback);
    EXPECT_EQ(service.GetThumbnailState(second), ThumbnailState::Queued);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, GpuPumpDoesNotWritePendingMaterialPixelsIntoPrefabCache)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();

    auto material = MakeThumbnailRequest(root, "material:Body");
    material.sourceAssetPath = "Assets/Models/Hero.gltf";
    material.kind = AssetThumbnailKind::MaterialSphere;
    material.requestedSize = 48u;
    material.priority = ThumbnailRequestPriority::Visible;
    material.freshnessInputs = {{"source", "material:v1"}};

    auto prefab = MakeThumbnailRequest(root, "prefab:Hero");
    prefab.sourceAssetPath = "Assets/Models/Hero.prefab";
    prefab.kind = AssetThumbnailKind::PrefabPreview;
    prefab.requestedSize = 48u;
    prefab.priority = ThumbnailRequestPriority::Visible;
    prefab.freshnessInputs = {{"source", "prefab:v1"}};

    PendingMaterialThenKindColoredPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(material).status, AssetThumbnailServiceStatus::Pending);

    const auto materialPending = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(materialPending.has_value());
    EXPECT_EQ(materialPending->diagnostic, "thumbnail-gpu-preview-readback-pending");
    EXPECT_EQ(service.GetThumbnailState(material), ThumbnailState::WaitingForGpu);

    ASSERT_EQ(service.RequestAssetPreview(prefab).status, AssetThumbnailServiceStatus::Pending);

    const auto materialReady = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(materialReady.has_value());
    EXPECT_EQ(service.GetThumbnailState(material), ThumbnailState::Readback);
    EXPECT_EQ(service.GetThumbnailState(prefab), ThumbnailState::Queued);
    ASSERT_GE(renderer.renderKinds.size(), 2u);
    EXPECT_EQ(renderer.renderKinds[1], AssetThumbnailKind::MaterialSphere)
        << "A pending material readback must be completed before rendering the queued prefab.";

    auto materialWritten = service.ConsumeCompletedThumbnail();
    for (size_t attempt = 0u; attempt < 100u && !materialWritten.has_value(); ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        materialWritten = service.ConsumeCompletedThumbnail();
    }
    ASSERT_TRUE(materialWritten.has_value());
    ASSERT_EQ(materialWritten->status, AssetThumbnailServiceStatus::Fresh);
    ASSERT_TRUE(materialWritten->cacheEntry.has_value());

    const auto prefabGenerated = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(prefabGenerated.has_value());
    EXPECT_EQ(service.GetThumbnailState(prefab), ThumbnailState::Readback);
    ASSERT_GE(renderer.renderKinds.size(), 3u);
    EXPECT_EQ(renderer.renderKinds[2], AssetThumbnailKind::PrefabPreview);

    auto prefabWritten = service.ConsumeCompletedThumbnail();
    for (size_t attempt = 0u; attempt < 100u && !prefabWritten.has_value(); ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        prefabWritten = service.ConsumeCompletedThumbnail();
    }
    ASSERT_TRUE(prefabWritten.has_value());
    ASSERT_EQ(prefabWritten->status, AssetThumbnailServiceStatus::Fresh);
    ASSERT_TRUE(prefabWritten->cacheEntry.has_value());

    const auto materialBytes = ReadBinaryFile(materialWritten->cacheEntry->imagePath);
    const auto prefabBytes = ReadBinaryFile(prefabWritten->cacheEntry->imagePath);
    ASSERT_FALSE(materialBytes.empty());
    ASSERT_FALSE(prefabBytes.empty());
    EXPECT_NE(materialBytes, prefabBytes)
        << "Prefab GPU thumbnail cache must not receive the material preview image.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, GpuPumpPollsExistingPendingReadbackWhenReadbackBudgetIsExhausted)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();

    auto material = MakeThumbnailRequest(root, "material:Budgeted");
    material.sourceAssetPath = "Assets/Materials/Budgeted.mat";
    material.kind = AssetThumbnailKind::MaterialSphere;
    material.requestedSize = 48u;
    material.priority = ThumbnailRequestPriority::Visible;
    material.freshnessInputs = {{"source", "budgeted-material:v1"}};

    PendingThenReadyThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ThumbnailGenerationBudget initialBudget;
    initialBudget.previewRenderCountBudget = 1u;
    initialBudget.readbackCountBudget = 1u;
    initialBudget.cacheWriteCountBudget = 1u;
    service.SetThumbnailGenerationBudget(initialBudget);
    ASSERT_EQ(service.RequestAssetPreview(material).status, AssetThumbnailServiceStatus::Pending);

    const auto pending = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->diagnostic, "thumbnail-gpu-preview-readback-pending");
    EXPECT_EQ(service.GetThumbnailState(material), ThumbnailState::WaitingForGpu);
    EXPECT_EQ(renderer.renderCount, 1u);

    ThumbnailGenerationBudget exhaustedBudget;
    exhaustedBudget.previewRenderCountBudget = 0u;
    exhaustedBudget.readbackCountBudget = 0u;
    exhaustedBudget.cacheWriteCountBudget = 1u;
    service.SetThumbnailGenerationBudget(exhaustedBudget);

    const auto polled = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(polled.has_value());
    EXPECT_EQ(polled->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetThumbnailState(material), ThumbnailState::Readback)
        << "Readback budget throttles new GPU submissions, but must not block polling an existing fence.";
    EXPECT_EQ(renderer.renderCount, 2u);

    auto written = service.ConsumeCompletedThumbnail();
    for (size_t attempt = 0u; attempt < 100u && !written.has_value(); ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        written = service.ConsumeCompletedThumbnail();
    }
    ASSERT_TRUE(written.has_value());
    ASSERT_EQ(written->status, AssetThumbnailServiceStatus::Fresh);
    ASSERT_TRUE(written->cacheEntry.has_value());

    std::error_code removeError;
    std::filesystem::remove_all(root, removeError);
    EXPECT_FALSE(removeError) << removeError.message();
}

TEST(AssetThumbnailCacheTests, SourcePrefabAndGeneratedPrimaryPrefabSharePreviewCacheIdentity)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = AssetId(NLS::Guid::Parse("7677e767-e26d-4f6e-88fd-e389a5b1224b"));
    WriteBinaryFile(root / "Assets" / "Model" / "Cube 1.prefab", std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});
    WriteBinaryFile(
        root / "Library" / "Artifacts" / assetId.ToString() / "prefab.nprefab",
        std::vector<uint8_t>{'a', 'r', 't', 'i', 'f', 'a', 'c', 't'});
    WriteTextFile(
        root / "Library" / "Artifacts" / assetId.ToString() / "manifest.json",
        "{\"assetId\":\"" + assetId.ToString() + "\","
        "\"sourcePath\":\"Assets/Model/Cube 1.prefab\","
        "\"artifacts\":["
        "{"
        "\"subAssetKey\":\"prefab:Cube 1\","
        "\"artifactType\":\"Prefab\","
        "\"loaderId\":\"prefab\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/prefab.nprefab\","
        "\"contentHash\":\"prefab-hash\""
        "}"
        "]"
        "}");

    AssetBrowserItem sourceItem;
    sourceItem.kind = AssetBrowserItemKind::SourceAsset;
    sourceItem.type = AssetBrowserItemType::Prefab;
    sourceItem.assetId = assetId;
    sourceItem.sourceAssetPath = "Assets/Model/Cube 1.prefab";
    sourceItem.subAssetKey = "prefab:Cube 1";
    sourceItem.artifactPath = RedirectedArtifactPathOrFallback(
        "Library/Artifacts/" + assetId.ToString() + "/prefab.nprefab");
    sourceItem.artifactType = ArtifactType::Prefab;

    AssetBrowserItem generatedItem = sourceItem;
    generatedItem.kind = AssetBrowserItemKind::GeneratedSubAsset;
    generatedItem.projectRelativePath = "Assets/Model/Cube 1.prefab::prefab:Cube 1";

    const auto sourceRequest = BuildAssetThumbnailRequestForItem(root, sourceItem, 160u);
    const auto generatedRequest = BuildAssetThumbnailRequestForItem(root, generatedItem, 160u);
    ASSERT_TRUE(sourceRequest.has_value());
    ASSERT_TRUE(generatedRequest.has_value());
    ASSERT_EQ(sourceRequest->kind, AssetThumbnailKind::PrefabPreview);
    ASSERT_EQ(generatedRequest->kind, AssetThumbnailKind::PrefabPreview);
    ASSERT_EQ(sourceRequest->subAssetKey, generatedRequest->subAssetKey);
    ASSERT_EQ(
        NLS::Core::Assets::NormalizeAssetPath(root / sourceRequest->artifactPath),
        NLS::Core::Assets::NormalizeAssetPath(root / generatedRequest->artifactPath));

    EXPECT_EQ(BuildAssetThumbnailCacheKey(*sourceRequest), BuildAssetThumbnailCacheKey(*generatedRequest))
        << "The source .prefab tile and its primary generated prefab artifact must share one preview cache; "
           "otherwise one entry can go stale or receive wrong pixels while the other is correct.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, BackgroundPumpDefersPrefabPreviewWhenRendererUnavailable)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("be2e2e2e-2e2e-4e2e-8e2e-2e2e2e2e2e2e"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    WriteNativeArtifactTextFile(
        artifactRoot / "Hero.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        MinimalPrefabPayload());

    auto request = MakeThumbnailRequest(root, "prefab:Hero");
    request.assetId = assetId;
    request.sourceAssetPath = "Assets/Models/Hero.fbx";
    request.artifactPath = RedirectedArtifactPathOrFallback(
        "Library/Artifacts/" + assetId.ToString() + "/Hero.nprefab");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 48u;
    request.settingsFingerprint = "asset-browser-thumbnail:v22-prefab-mesh-set-preview";

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    EXPECT_FALSE(service.StartNextThumbnailGeneration())
        << "Without a preview renderer, the background CPU pump must leave prefab previews "
           "queued for the GPU preview path.";
    EXPECT_FALSE(service.HasInFlightRequest());
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Missing);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, BackgroundPumpDefersCpuDeferredPrefabPreviewWhenRendererUnavailable)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("be6e6e6e-6e6e-4e6e-8e6e-6e6e6e6e6e6e"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteNativeArtifactTextFile(
        artifactRoot / "Hero.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        MinimalPrefabPayload());
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"scene-model\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"prefab:Hero\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"prefab:Hero\","
        "\"artifactType\":\"Prefab\","
        "\"loaderId\":\"native-prefab\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/Hero.nprefab\","
        "\"contentHash\":\"prefab-hash\""
        "}"
        "]"
        "}");

    AssetBrowserItem item;
    item.kind = AssetBrowserItemKind::SourceAsset;
    item.type = AssetBrowserItemType::Model;
    item.assetId = assetId;
    item.sourceAssetPath = "Assets/Models/Hero.fbx";

    AssetThumbnailRequestBuildContext context;
    context.deferManifestLookups = true;
    const auto request = BuildAssetThumbnailRequestForItem(root, item, 48u, context);
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(request->artifactPath.empty());
    ASSERT_EQ(request->kind, AssetThumbnailKind::PrefabPreview);

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(*request).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_FALSE(service.StartNextThumbnailGeneration())
        << "Without a GPU preview renderer, deferred prefab requests must remain queued "
           "instead of starting the background CPU preview path.";
    EXPECT_FALSE(service.HasInFlightRequest());
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_EQ(service.GetThumbnailState(*request), ThumbnailState::Queued);
    EXPECT_EQ(EvaluateAssetThumbnailCache(*request).status, AssetThumbnailCacheStatus::Missing);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, DeferredGeneratedThumbnailInvalidatesWhenResolvedArtifactChanges)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("be4e4e4e-4e4e-4e4e-8e4e-4e4e4e4e4e4e"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteNativeArtifactTextFile(
        artifactRoot / "Hero.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        MinimalPrefabPayload());
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"scene-model\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"prefab:Hero\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"prefab:Hero\","
        "\"artifactType\":\"Prefab\","
        "\"loaderId\":\"native-prefab\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/Hero.nprefab\","
        "\"contentHash\":\"prefab-hash\""
        "}"
        "]"
        "}");

    AssetBrowserItem item;
    item.kind = AssetBrowserItemKind::SourceAsset;
    item.type = AssetBrowserItemType::Model;
    item.assetId = assetId;
    item.sourceAssetPath = "Assets/Models/Hero.fbx";

    AssetThumbnailRequestBuildContext context;
    context.deferManifestLookups = true;
    const auto request = BuildAssetThumbnailRequestForItem(root, item, 48u, context);
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(request->artifactPath.empty());
    ASSERT_EQ(request->kind, AssetThumbnailKind::PrefabPreview);

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(*request).status, AssetThumbnailServiceStatus::Pending);
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    ASSERT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh) << generated->diagnostic;
    ASSERT_EQ(service.GetThumbnailState(*request), ThumbnailState::Ready);

    const auto oldPrefabPath = root / RedirectedArtifactPathOrFallback(
        "Library/Artifacts/" + assetId.ToString() + "/Hero.nprefab");
    const auto oldStamp = FileStampForTest(oldPrefabPath);
    std::string changedPayload;
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        changedPayload = MinimalPrefabPayload() + "\n// changed " + std::to_string(attempt);
        WriteNativeArtifactTextFile(
            artifactRoot / "Hero.nprefab",
            ArtifactType::Prefab,
            "prefab",
            1u,
            changedPayload);
        if (root / RedirectedArtifactPathOrFallback(
                "Library/Artifacts/" + assetId.ToString() + "/Hero.nprefab") != oldPrefabPath)
        {
            break;
        }
    }
    ASSERT_EQ(FileStampForTest(oldPrefabPath), oldStamp);
    const auto newPrefabPath = root / RedirectedArtifactPathOrFallback(
        "Library/Artifacts/" + assetId.ToString() + "/Hero.nprefab");
    ASSERT_NE(newPrefabPath, oldPrefabPath);
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"scene-model\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"prefab:Hero\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"prefab:Hero\","
        "\"artifactType\":\"Prefab\","
        "\"loaderId\":\"native-prefab\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/Hero.nprefab\","
        "\"contentHash\":\"prefab-hash-changed\""
        "}"
        "]"
        "}");

    const auto evaluated = EvaluateAssetThumbnailCache(*request);
    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Stale);
    EXPECT_EQ(evaluated.diagnostic, "thumbnail-cache-freshness-stale");

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, DeferredFailedThumbnailInvalidatesWhenResolvedArtifactChanges)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("be5e5e5e-5e5e-4e5e-8e5e-5e5e5e5e5e5e"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteNativeArtifactTextFile(
        artifactRoot / "Hero.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        std::string(2u * 1024u * 1024u, 'p'));
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"scene-model\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"prefab:Hero\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"prefab:Hero\","
        "\"artifactType\":\"Prefab\","
        "\"loaderId\":\"native-prefab\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/Hero.nprefab\","
        "\"contentHash\":\"prefab-hash\""
        "}"
        "]"
        "}");

    AssetBrowserItem item;
    item.kind = AssetBrowserItemKind::SourceAsset;
    item.type = AssetBrowserItemType::Model;
    item.assetId = assetId;
    item.sourceAssetPath = "Assets/Models/Hero.fbx";

    AssetThumbnailRequestBuildContext context;
    context.deferManifestLookups = true;
    const auto request = BuildAssetThumbnailRequestForItem(root, item, 48u, context);
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(request->artifactPath.empty());
    ASSERT_EQ(request->kind, AssetThumbnailKind::PrefabPreview);

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(*request).status, AssetThumbnailServiceStatus::Pending);
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    ASSERT_EQ(generated->status, AssetThumbnailServiceStatus::Fallback);
    ASSERT_EQ(generated->diagnostic, "thumbnail-prefab-preview-budget-exceeded");
    auto evaluated = EvaluateAssetThumbnailCache(*request);
    ASSERT_EQ(evaluated.status, AssetThumbnailCacheStatus::Failed);

    const auto oldPrefabPath = root / RedirectedArtifactPathOrFallback(
        "Library/Artifacts/" + assetId.ToString() + "/Hero.nprefab");
    const auto oldStamp = FileStampForTest(oldPrefabPath);
    std::string fixedPayload;
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        fixedPayload = MinimalPrefabPayload() + "\n// fixed " + std::to_string(attempt);
        WriteNativeArtifactTextFile(
            artifactRoot / "Hero.nprefab",
            ArtifactType::Prefab,
            "prefab",
            1u,
            fixedPayload);
        if (root / RedirectedArtifactPathOrFallback(
                "Library/Artifacts/" + assetId.ToString() + "/Hero.nprefab") != oldPrefabPath)
        {
            break;
        }
    }
    ASSERT_EQ(FileStampForTest(oldPrefabPath), oldStamp);
    const auto newPrefabPath = root / RedirectedArtifactPathOrFallback(
        "Library/Artifacts/" + assetId.ToString() + "/Hero.nprefab");
    ASSERT_NE(newPrefabPath, oldPrefabPath);
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"scene-model\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"prefab:Hero\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"prefab:Hero\","
        "\"artifactType\":\"Prefab\","
        "\"loaderId\":\"native-prefab\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/Hero.nprefab\","
        "\"contentHash\":\"prefab-hash-fixed\""
        "}"
        "]"
        "}");

    evaluated = EvaluateAssetThumbnailCache(*request);
    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Stale);
    EXPECT_EQ(evaluated.diagnostic, "thumbnail-cache-freshness-stale");
    EXPECT_EQ(service.GetThumbnail(*request).status, AssetThumbnailServiceStatus::Pending);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, PrefabCpuThumbnailPreservesSnapshotDrawItemTransforms)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("bf0f0f0f-0f0f-4f0f-8f0f-0f0f0f0f0f0f"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot);
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteBinaryFile(
        artifactRoot / "meshes" / "Body.nmesh",
        NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));
    const auto prefabPayload = PrefabPayloadWithTwoTransformedRendererDependencies(
        assetId,
        "mesh:Body",
        "mesh:Body");
    WriteNativeArtifactTextFile(
        artifactRoot / "Hero.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        prefabPayload);
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"scene-model\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"prefab:Hero\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"prefab:Hero\","
        "\"artifactType\":\"Prefab\","
        "\"loaderId\":\"native-prefab\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/Hero.nprefab\","
        "\"contentHash\":\"prefab-hash\""
        "},"
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"mesh:Body\","
        "\"artifactType\":\"Mesh\","
        "\"loaderId\":\"mesh\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/meshes/Body.nmesh\","
        "\"contentHash\":\"mesh-hash\""
        "}"
        "]"
        "}");

    auto imported = NLS::Engine::Assets::ImportPrefabArtifact(
        prefabPayload,
        assetId,
        {
            {assetId, "Mesh", "mesh:Body", RedirectedArtifactPathOrFallback(
                "Library/Artifacts/" + assetId.ToString() + "/meshes/Body.nmesh")}
        });
    ASSERT_FALSE(imported.diagnostics.HasErrors());
    const auto snapshot = BuildPreviewRenderableSnapshot(imported.artifact);
    ASSERT_EQ(snapshot.drawItems.size(), 2u);
    EXPECT_LT(snapshot.drawItems[0].localPosition.x, 0.0f);
    EXPECT_GT(snapshot.drawItems[1].localPosition.x, 0.0f);

    auto request = MakeThumbnailRequest(root, "prefab:Hero");
    request.assetId = assetId;
    request.sourceAssetPath = "Assets/Models/Hero.fbx";
    request.artifactPath = RedirectedArtifactPathOrFallback(
        "Library/Artifacts/" + assetId.ToString() + "/Hero.nprefab");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 96u;
    request.freshnessInputs = {{"artifact", "two-instance-prefab:v1"}};

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    ASSERT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh) << generated->diagnostic;
    ASSERT_TRUE(generated->cacheEntry.has_value());

    const NLS::Image decoded(generated->cacheEntry->imagePath.string(), false);
    ASSERT_NE(decoded.GetData(), nullptr);
    EXPECT_GE(CountOpaqueColumnClusters(decoded), 2u)
        << "The CPU prefab thumbnail path must render repeated snapshot draw items at their prefab transforms.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, PrefabCpuThumbnailDoesNotCachePartialSnapshotWhenOneMeshIsMissing)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("bf0e0e0e-0e0e-4e0e-8e0e-0e0e0e0e0e0e"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot / "meshes");
    WriteBinaryFile(root / "Assets" / "Models" / "BrokenHero.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteBinaryFile(
        artifactRoot / "meshes" / "Body.nmesh",
        NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));
    const auto prefabPayload = PrefabPayloadWithTwoTransformedRendererDependencies(
        assetId,
        "mesh:Body",
        "mesh:Missing");
    WriteNativeArtifactTextFile(
        artifactRoot / "BrokenHero.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        prefabPayload);
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"scene-model\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"prefab:BrokenHero\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"prefab:BrokenHero\","
        "\"artifactType\":\"Prefab\","
        "\"loaderId\":\"native-prefab\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/BrokenHero.nprefab\","
        "\"contentHash\":\"prefab-hash\""
        "},"
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"mesh:Body\","
        "\"artifactType\":\"Mesh\","
        "\"loaderId\":\"mesh\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/meshes/Body.nmesh\","
        "\"contentHash\":\"mesh-hash\""
        "}"
        "]"
        "}");

    auto imported = NLS::Engine::Assets::ImportPrefabArtifact(
        prefabPayload,
        assetId,
        {
            {assetId, "Mesh", "mesh:Body", RedirectedArtifactPathOrFallback(
                "Library/Artifacts/" + assetId.ToString() + "/meshes/Body.nmesh")}
        });
    ASSERT_FALSE(imported.diagnostics.HasErrors());
    const auto snapshot = BuildPreviewRenderableSnapshot(imported.artifact);
    ASSERT_EQ(snapshot.drawItems.size(), 1u)
        << "This regression requires one prefab draw item to be dropped because its mesh is missing.";

    auto request = MakeThumbnailRequest(root, "prefab:BrokenHero");
    request.assetId = assetId;
    request.sourceAssetPath = "Assets/Models/BrokenHero.fbx";
    request.artifactPath = RedirectedArtifactPathOrFallback(
        "Library/Artifacts/" + assetId.ToString() + "/BrokenHero.nprefab");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 96u;
    request.freshnessInputs = {{"artifact", "partial-prefab:v1"}};

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_NE(generated->status, AssetThumbnailServiceStatus::Fresh)
        << "A prefab thumbnail that drops one draw item must not be cached as if the full prefab rendered.";
    EXPECT_NE(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Fresh);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, GpuPrefabPreviewRejectsPartialRenderableSnapshot)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("bf0d0d0d-0d0d-4d0d-8d0d-0d0d0d0d0d0d"));
    const auto prefabPayload = PrefabPayloadWithTwoTransformedRendererDependencies(
        assetId,
        "mesh:Body",
        "mesh:Missing");
    auto imported = NLS::Engine::Assets::ImportPrefabArtifact(
        prefabPayload,
        assetId,
        {
            {assetId, "Mesh", "mesh:Body", "Library/Artifacts/bodymesh"}
        });
    ASSERT_FALSE(imported.diagnostics.HasErrors());

    const auto snapshot = BuildPreviewRenderableSnapshot(imported.artifact);
    ASSERT_EQ(snapshot.expectedDrawItemCount, 2u);
    ASSERT_EQ(snapshot.drawItems.size(), 1u)
        << "This regression requires the prefab snapshot builder to drop one unresolved renderer.";

    EXPECT_FALSE(ThumbnailPreviewSnapshotIsCompleteForGpuPrefabPreviewForTesting(snapshot))
        << "GPU thumbnails must not render and cache a misleading partial prefab preview.";
}

TEST(AssetThumbnailCacheTests, GpuPrefabPreviewDrawItemCapacityCoversLargeImportedModelPrefabs)
{
    using namespace NLS::Editor::Assets;

#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect GPU thumbnail preview capacity.";
#else
    constexpr size_t kSponzaMainGltfPrimitiveCount = 405u;

    EXPECT_GE(GetThumbnailPreviewPrefabDrawItemCapacityForTesting(), kSponzaMainGltfPrimitiveCount)
        << "Large imported model thumbnails such as Sponza must stay on the complete GPU prefab "
           "preview path instead of falling back to a partial CPU mesh-set thumbnail.";
#endif
}

TEST(AssetThumbnailCacheTests, ThumbnailPreviewDefaultShaderDoesNotUseLegacyStandardHlslFallbackWhenUnavailable)
{
    using NLS::Core::ResourceManagement::ShaderManager;

    ShaderManager shaderManager;

    const auto selection = NLS::Editor::Assets::SelectThumbnailPreviewDefaultShaderForTesting(shaderManager);

    EXPECT_FALSE(selection.usesLegacyBuiltInStandardHlsl)
        << "After ShaderLab migration, thumbnail default material must not silently fall back to legacy Standard.hlsl.";
    EXPECT_TRUE(selection.resourcePath.empty());
    EXPECT_FALSE(selection.usesShaderLabStandardPbrForward);
}

TEST(AssetThumbnailCacheTests, ThumbnailPreviewDefaultShaderUsesLoadedStandardPbrForwardShaderLabPass)
{
    using NLS::Core::ResourceManagement::ShaderManager;
    using NLS::Render::Resources::Shader;

#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to create a ShaderLab shader for thumbnail fallback selection.";
#else
    ShaderManager shaderManager;
    auto* forward = Shader::CreateForTesting("Library/Artifacts/12/standardpbrforward");
    ASSERT_NE(forward, nullptr);
    forward->SetImportedShaderLabPassForTesting(
        "App/Assets/Engine/Shaders/ShaderLab/StandardPBR.shader",
        "shader:StandardPBR/Forward#0",
        "Forward",
        {});
    shaderManager.RegisterResource("Library/Artifacts/12/standardpbrforward", forward);

    const auto selection = NLS::Editor::Assets::SelectThumbnailPreviewDefaultShaderForTesting(shaderManager);

    EXPECT_EQ(selection.resourcePath, "Library/Artifacts/12/standardpbrforward");
    EXPECT_TRUE(selection.usesShaderLabStandardPbrForward);
    EXPECT_FALSE(selection.usesLegacyBuiltInStandardHlsl);

    shaderManager.UnloadResources();
#endif
}

TEST(AssetThumbnailCacheTests, PrefabCpuThumbnailResolvesSnapshotMeshFromReferencedAssetManifest)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto prefabAssetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("bf101010-1010-4010-8010-101010101010"));
    const auto meshAssetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("bf111111-1111-4111-8111-111111111111"));
    const auto prefabArtifactRoot = root / "Library" / "Artifacts" / prefabAssetId.ToString();
    const auto meshArtifactRoot = root / "Library" / "Artifacts" / meshAssetId.ToString();
    std::filesystem::create_directories(prefabArtifactRoot);
    std::filesystem::create_directories(meshArtifactRoot / "meshes");

    WriteBinaryFile(root / "Assets" / "Prefabs" / "Hero.prefab", std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});
    WriteBinaryFile(
        meshArtifactRoot / "meshes" / "Body.nmesh",
        NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));
    const auto prefabPayload = PrefabPayloadWithSingleRendererDependency(meshAssetId, "mesh:Body");
    WriteNativeArtifactTextFile(
        prefabArtifactRoot / "Hero.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        prefabPayload);
    WriteTextFile(
        prefabArtifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + prefabAssetId.GetGuid().ToString() + "\","
        "\"importerId\":\"prefab\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"prefab:Hero\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + prefabAssetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"prefab:Hero\","
        "\"artifactType\":\"Prefab\","
        "\"loaderId\":\"native-prefab\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + prefabAssetId.ToString() + "/Hero.nprefab\","
        "\"contentHash\":\"prefab-hash\""
        "}"
        "]"
        "}");
    WriteTextFile(
        meshArtifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + meshAssetId.GetGuid().ToString() + "\","
        "\"importerId\":\"scene-model\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"mesh:Body\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + meshAssetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"mesh:Body\","
        "\"artifactType\":\"Mesh\","
        "\"loaderId\":\"mesh\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + meshAssetId.ToString() + "/meshes/Body.nmesh\","
        "\"contentHash\":\"mesh-hash\""
        "}"
        "]"
        "}");

    auto request = MakeThumbnailRequest(root, "prefab:Hero");
    request.assetId = prefabAssetId;
    request.sourceAssetPath = "Assets/Models/Hero.fbx";
    request.artifactPath = RedirectedArtifactPathOrFallback(
        "Library/Artifacts/" + prefabAssetId.ToString() + "/Hero.nprefab");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 96u;
    request.freshnessInputs = {{"artifact", "external-mesh-prefab:v1"}};

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    ASSERT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh) << generated->diagnostic;
    ASSERT_TRUE(generated->cacheEntry.has_value());

    const NLS::Image decoded(generated->cacheEntry->imagePath.string(), false);
    ASSERT_NE(decoded.GetData(), nullptr);
    EXPECT_EQ(CountOpaqueColumnClusters(decoded), 1u)
        << "The CPU prefab snapshot path should render the external mesh dependency instead "
           "of falling back to the multi-block prefab structure placeholder.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, PrefabCpuThumbnailRendersSceneCubeBuiltinPrimitiveMesh)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("bf121212-1212-4212-8212-121212121212"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    const auto engineAssetsRoot = root / "EngineAssets";
    const auto builtinCubeArtifact = BuiltinMeshArtifactPath(engineAssetsRoot, "Models/Cube.fbx");
    std::filesystem::create_directories(artifactRoot);
    std::filesystem::create_directories(builtinCubeArtifact.parent_path());

    WriteBinaryFile(root / "Assets" / "Prefabs" / "Cube.prefab", std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});
    WriteBinaryFile(
        builtinCubeArtifact,
        NLS::Render::Assets::SerializeMeshArtifact(CubeMeshArtifactWithMissingNormals()));
    const auto prefabPayload = PrefabPayloadWithBuiltinPrimitiveMesh("builtin:Primitive/Cube");
    WriteNativeArtifactTextFile(
        artifactRoot / "Cube.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        prefabPayload);
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"prefab\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"prefab:Cube\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"prefab:Cube\","
        "\"artifactType\":\"Prefab\","
        "\"loaderId\":\"native-prefab\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/Cube.nprefab\","
        "\"contentHash\":\"prefab-hash\""
        "}"
        "]"
        "}");

    auto imported = NLS::Engine::Assets::ImportPrefabArtifact(prefabPayload, assetId, {});
    ASSERT_FALSE(imported.diagnostics.HasErrors());
    const auto snapshot = BuildPreviewRenderableSnapshot(imported.artifact);
    ASSERT_EQ(snapshot.drawItems.size(), 1u)
        << "A scene-created Cube prefab stores its mesh as a builtin primitive asset reference; "
           "the preview snapshot must preserve that renderable instead of falling back to structure art.";
    EXPECT_EQ(snapshot.drawItems.front().meshPath, "builtin:Primitive/Cube");

    const ScopedThumbnailMeshManagerAssetPaths meshManagerPaths(root / "Assets", engineAssetsRoot);

    auto request = MakeThumbnailRequest(root, "prefab:Cube");
    request.assetId = assetId;
    request.sourceAssetPath = "Assets/Models/Cube.fbx";
    request.artifactPath = RedirectedArtifactPathOrFallback(
        "Library/Artifacts/" + assetId.ToString() + "/Cube.nprefab");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 96u;
    request.freshnessInputs = {{"artifact", "builtin-cube-prefab:v1"}};

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    ASSERT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh) << generated->diagnostic;
    ASSERT_TRUE(generated->cacheEntry.has_value());

    const NLS::Image decoded(generated->cacheEntry->imagePath.string(), false);
    ASSERT_NE(decoded.GetData(), nullptr);
    EXPECT_EQ(CountOpaqueColumnClusters(decoded), 1u)
        << "A scene-created Cube prefab should render the builtin primitive mesh thumbnail "
           "instead of the prefab structure placeholder.";
    EXPECT_GT(AverageOpaqueLuminance(decoded), 75.0)
        << "The cube preview should have usable preview lighting even when the mesh artifact "
           "does not carry vertex normals.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, PrefabPreviewRequiresGpuRendererForBrightObliqueUnityStylePreview)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("bf131313-1313-4313-8313-131313131313"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    const auto engineAssetsRoot = root / "EngineAssets";
    const auto builtinCubeArtifact = BuiltinMeshArtifactPath(engineAssetsRoot, "Models/Cube.fbx");
    std::filesystem::create_directories(artifactRoot);
    std::filesystem::create_directories(builtinCubeArtifact.parent_path());

    WriteBinaryFile(root / "Assets" / "Prefabs" / "Cube.prefab", std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});
    WriteBinaryFile(
        builtinCubeArtifact,
        NLS::Render::Assets::SerializeMeshArtifact(CubeMeshArtifactWithMissingNormals()));
    const auto prefabPayload = PrefabPayloadWithBuiltinPrimitiveMesh("builtin:Primitive/Cube");
    WriteNativeArtifactTextFile(
        artifactRoot / "Cube.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        prefabPayload);
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"prefab\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"prefab:Cube\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"prefab:Cube\","
        "\"artifactType\":\"Prefab\","
        "\"loaderId\":\"native-prefab\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/Cube.nprefab\","
        "\"contentHash\":\"prefab-hash\""
        "}"
        "]"
        "}");

    const ScopedThumbnailMeshManagerAssetPaths meshManagerPaths(root / "Assets", engineAssetsRoot);

    auto request = MakeThumbnailRequest(root, "prefab:Cube");
    request.assetId = assetId;
    request.sourceAssetPath = "Assets/Prefabs/Cube.prefab";
    request.artifactPath = RedirectedArtifactPathOrFallback(
        "Library/Artifacts/" + assetId.ToString() + "/Cube.nprefab");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 96u;
    request.freshnessInputs = {{"artifact", "builtin-cube-prefab:v2-bright-oblique"}} ;

    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);
    const auto generated = service.GenerateNextThumbnail();
    EXPECT_FALSE(generated.has_value())
        << "Prefab thumbnails are GPU previews; without a renderer the service must keep the request queued "
           "instead of generating an obsolete CPU cube thumbnail.";
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Missing);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, GpuPreviewCameraAndLightingUseUpperObliqueUnityStyleSetup)
{
    using namespace NLS::Editor::Assets;

#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect GPU thumbnail preview setup.";
#else
    const auto camera = BuildPrefabPreviewCameraDebugInfoForTesting(
        {-1.0f, -1.0f, -1.0f},
        {1.0f, 1.0f, 1.0f},
        96u,
        96u);
    const NLS::Maths::Vector3 center{0.0f, 0.0f, 0.0f};
    const auto toCamera = camera.cameraPosition - center;

    EXPECT_GT(camera.cameraPosition.y, 0.0f)
        << "Prefab GPU previews should place the camera above the asset, not below it.";
    EXPECT_LT(camera.lookDirection.y, 0.0f)
        << "The preview camera should look downward toward the asset from an upper oblique angle.";
    EXPECT_GT(std::abs(toCamera.x), 0.1f);
    EXPECT_GT(std::abs(toCamera.z), 0.1f);
    EXPECT_GT(camera.distance, 0.0f);

    const auto keyLight = GetThumbnailPreviewKeyLightDirectionForTesting();
    EXPECT_LT(keyLight.y, -0.25f)
        << "The key light should illuminate from above instead of grazing the material sphere.";
    EXPECT_GT(GetThumbnailPreviewKeyLightIntensityForTesting(), 0.6f);
    EXPECT_LT(GetThumbnailPreviewKeyLightIntensityForTesting(), 0.9f);
    EXPECT_GT(GetThumbnailPreviewAmbientIntensityForTesting(), 0.18f)
        << "Material and prefab previews need enough fill light to avoid dark grey thumbnails.";
    EXPECT_LT(GetThumbnailPreviewAmbientIntensityForTesting(), 0.35f)
        << "Cube and material thumbnails should not be flattened by overbright ambient light.";
#endif
}

TEST(AssetThumbnailCacheTests, GpuPreviewMeshLoaderUsesArtifactPathForContentAddressedStorage)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect GPU thumbnail preview path routing.";
#else
    const auto storageName = BuildArtifactStorageFileName("thumbnail-preview:mesh:Body");
    const auto contentArtifactPath = (std::filesystem::path("Library") /
        "Artifacts" /
        BuildArtifactStorageRelativePath(storageName)).generic_string();
    ASSERT_TRUE(IsContentStorageArtifactPath(contentArtifactPath));

    EXPECT_TRUE(ThumbnailPreviewMeshPathUsesArtifactLoaderForTesting(contentArtifactPath))
        << "Prefab GPU previews must load extensionless Asset Database v2 mesh artifacts through "
           "MeshManager::RequestAsyncArtifact; normal resource loading treats them as source paths.";
    EXPECT_FALSE(ThumbnailPreviewMeshPathUsesArtifactLoaderForTesting(
        "Library/Artifacts/legacy-guid/meshes/Body.nmesh"))
        << "Asset Database v2 preview loading should treat extensionless content artifacts as authoritative.";
    EXPECT_FALSE(ThumbnailPreviewMeshPathUsesArtifactLoaderForTesting("Assets/Models/Body.fbx"));
#endif
}

TEST(AssetThumbnailCacheTests, GpuPreviewMeshLoadPathUsesResolvedContentArtifactFile)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect GPU thumbnail preview path routing.";
#else
    const auto root = MakeAssetThumbnailCacheRoot();
    const auto prefabAssetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("bf171717-1717-4717-8717-171717171717"));
    const auto meshAssetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("bf181818-1818-4818-8818-181818181818"));
    const auto meshPayload = NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact());
    const auto meshStorageName = BuildArtifactStorageFileName(meshPayload.data(), meshPayload.size());
    const auto meshArtifactPath = (std::filesystem::path("Library") /
        "Artifacts" /
        BuildArtifactStorageRelativePath(meshStorageName)).generic_string();
    WriteBinaryFile(root / meshArtifactPath, meshPayload);

    NLS::Engine::Assets::PrefabResolvedAsset resolvedMesh;
    resolvedMesh.assetId = meshAssetId;
    resolvedMesh.expectedType = "Mesh";
    resolvedMesh.subAssetKey = "mesh:Body";
    resolvedMesh.artifactPath = meshArtifactPath;

    auto imported = NLS::Engine::Assets::ImportPrefabArtifact(
        PrefabPayloadWithSingleRendererDependency(meshAssetId, "mesh:Body"),
        prefabAssetId,
        {resolvedMesh});
    ASSERT_FALSE(imported.diagnostics.HasErrors());
    const auto snapshot = BuildPreviewRenderableSnapshot(imported.artifact);
    ASSERT_EQ(snapshot.drawItems.size(), 1u);
    ASSERT_EQ(snapshot.drawItems.front().meshPath, meshArtifactPath);
    ASSERT_EQ(snapshot.drawItems.front().meshAssetId, meshAssetId);

    auto request = MakeThumbnailRequest(root, "prefab:Body");
    request.assetId = prefabAssetId;
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.artifactPath = (std::filesystem::path("Library") /
        "Artifacts" /
        "aa" /
        "aa00000000000000000000000000000000000000000000000000000000000000").generic_string();

    const auto loadPath = ResolveThumbnailPreviewMeshLoadPathForTesting(
        request,
        snapshot.drawItems.front().meshPath,
        snapshot.drawItems.front().meshAssetId);

    EXPECT_NE(loadPath, snapshot.drawItems.front().meshPath)
        << "GPU prefab previews should load the physical resolved content artifact file, not the "
           "raw prefab snapshot reference.";
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(loadPath)))
        << loadPath;
    EXPECT_TRUE(ThumbnailPreviewMeshPathUsesArtifactLoaderForTesting(loadPath))
        << "Resolved physical content artifact files are extensionless but still need the mesh artifact loader.";

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailCacheTests, OversizedImportedModelPrefabFallsBackToManifestMeshPreview)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("bf151515-1515-4515-8515-151515151515"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot / "meshes");

    WriteBinaryFile(root / "Assets" / "Models" / "Heavy.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteBinaryFile(
        artifactRoot / "meshes" / "Body.nmesh",
        NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));

    const std::string largePadding((1024u * 1024u) + 64u, ' ');
    WriteNativeArtifactTextFile(
        artifactRoot / "Heavy.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        MinimalPrefabPayload() + largePadding);
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"scene-model\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"prefab:Heavy\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"prefab:Heavy\","
        "\"artifactType\":\"Prefab\","
        "\"loaderId\":\"native-prefab\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/Heavy.nprefab\","
        "\"contentHash\":\"prefab-hash\""
        "},"
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"mesh:Body\","
        "\"artifactType\":\"Mesh\","
        "\"loaderId\":\"mesh\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/meshes/Body.nmesh\","
        "\"contentHash\":\"mesh-hash\""
        "}"
        "]"
        "}");

    auto request = MakeThumbnailRequest(root, "prefab:Heavy");
    request.assetId = assetId;
    request.sourceAssetPath = "Assets/Models/Heavy.fbx";
    request.artifactPath = RedirectedArtifactPathOrFallback(
        "Library/Artifacts/" + assetId.ToString() + "/Heavy.nprefab");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 96u;
    request.freshnessInputs = {{"artifact", "oversized-prefab-manifest-mesh:v1"}};

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    ASSERT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh) << generated->diagnostic;
    ASSERT_TRUE(generated->cacheEntry.has_value());

    const NLS::Image decoded(generated->cacheEntry->imagePath.string(), false);
    ASSERT_NE(decoded.GetData(), nullptr);
    EXPECT_GT(CountOpaquePixels(decoded), 0u);
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Fresh);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, MeshSetFallbackDoesNotCachePartialPreviewWhenMeshBudgetSkipsManifestMeshes)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("bf191919-1919-4919-8919-191919191919"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot / "meshes");

    WriteBinaryFile(root / "Assets" / "Models" / "LargeSet.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteBinaryFile(
        artifactRoot / "meshes" / "BlockA.nmesh",
        NLS::Render::Assets::SerializeMeshArtifact(BudgetSizedMeshArtifact(180000u, 540000u, 0.0f)));
    WriteBinaryFile(
        artifactRoot / "meshes" / "BlockB.nmesh",
        NLS::Render::Assets::SerializeMeshArtifact(BudgetSizedMeshArtifact(120000u, 360000u, 8.0f)));

    const std::string largePadding((1024u * 1024u) + 64u, ' ');
    WriteNativeArtifactTextFile(
        artifactRoot / "LargeSet.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        MinimalPrefabPayload() + largePadding);
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"scene-model\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"prefab:LargeSet\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"prefab:LargeSet\","
        "\"artifactType\":\"Prefab\","
        "\"loaderId\":\"native-prefab\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/LargeSet.nprefab\","
        "\"contentHash\":\"prefab-hash\""
        "},"
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"mesh:BlockA\","
        "\"artifactType\":\"Mesh\","
        "\"loaderId\":\"mesh\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/meshes/BlockA.nmesh\","
        "\"contentHash\":\"mesh-a-hash\""
        "},"
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"mesh:BlockB\","
        "\"artifactType\":\"Mesh\","
        "\"loaderId\":\"mesh\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/meshes/BlockB.nmesh\","
        "\"contentHash\":\"mesh-b-hash\""
        "}"
        "]"
        "}");

    auto request = MakeThumbnailRequest(root, "prefab:LargeSet");
    request.assetId = assetId;
    request.sourceAssetPath = "Assets/Models/LargeSet.fbx";
    request.artifactPath = RedirectedArtifactPathOrFallback(
        "Library/Artifacts/" + assetId.ToString() + "/LargeSet.nprefab");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 96u;
    request.freshnessInputs = {{"artifact", "budgeted-manifest-mesh-set:v1"}};

    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());

    EXPECT_NE(generated->status, AssetThumbnailServiceStatus::Fresh)
        << "A source-model prefab fallback must not cache a partial mesh-set thumbnail when "
           "CPU preview budgets skip manifest meshes.";
    EXPECT_EQ(generated->diagnostic, "thumbnail-model-preview-budget-exceeded");
    EXPECT_FALSE(std::filesystem::exists(entry->imagePath));
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Failed);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, PrefabCpuThumbnailFallsBackToVisiblePointsWhenTrianglesRasterizeNothing)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("bf141414-1414-4414-8414-141414141414"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot / "meshes");

    WriteBinaryFile(root / "Assets" / "Models" / "Degenerate.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteBinaryFile(
        artifactRoot / "meshes" / "Degenerate.nmesh",
        NLS::Render::Assets::SerializeMeshArtifact(DegenerateTriangleMeshArtifact()));
    const auto prefabPayload = PrefabPayloadWithSingleRendererDependency(assetId, "mesh:Degenerate");
    WriteNativeArtifactTextFile(
        artifactRoot / "Degenerate.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        prefabPayload);
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"scene-model\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"prefab:Degenerate\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"prefab:Degenerate\","
        "\"artifactType\":\"Prefab\","
        "\"loaderId\":\"native-prefab\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/Degenerate.nprefab\","
        "\"contentHash\":\"prefab-hash\""
        "},"
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"mesh:Degenerate\","
        "\"artifactType\":\"Mesh\","
        "\"loaderId\":\"mesh\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/meshes/Degenerate.nmesh\","
        "\"contentHash\":\"mesh-hash\""
        "}"
        "]"
        "}");

    auto request = MakeThumbnailRequest(root, "prefab:Degenerate");
    request.assetId = assetId;
    request.sourceAssetPath = "Assets/Models/Degenerate.fbx";
    request.artifactPath = RedirectedArtifactPathOrFallback(
        "Library/Artifacts/" + assetId.ToString() + "/Degenerate.nprefab");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 96u;
    request.freshnessInputs = {{"artifact", "degenerate-prefab:v1"}};

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    ASSERT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh) << generated->diagnostic;
    ASSERT_TRUE(generated->cacheEntry.has_value());

    const NLS::Image decoded(generated->cacheEntry->imagePath.string(), false);
    ASSERT_NE(decoded.GetData(), nullptr);
    EXPECT_GT(CountOpaquePixels(decoded), 0u)
        << "A renderable prefab must not cache a fully transparent thumbnail as Fresh when all "
           "sampled triangles are degenerate or otherwise rasterize no pixels.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, PrefabCpuThumbnailSamplesOversizedMeshWithoutStableFailure)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("bf131313-1313-4313-8313-131313131313"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot / "meshes");

    WriteBinaryFile(root / "Assets" / "Models" / "Large.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteBinaryFile(
        artifactRoot / "meshes" / "Large.nmesh",
        NLS::Render::Assets::SerializeMeshArtifact(OversizedMeshArtifact()));
    const auto prefabPayload = PrefabPayloadWithSingleRendererDependency(assetId, "mesh:Large");
    WriteNativeArtifactTextFile(
        artifactRoot / "Large.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        prefabPayload);
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"importerId\":\"scene-model\","
        "\"importerVersion\":1,"
        "\"targetPlatform\":\"editor\","
        "\"primarySubAssetKey\":\"prefab:Large\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"prefab:Large\","
        "\"artifactType\":\"Prefab\","
        "\"loaderId\":\"native-prefab\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/Large.nprefab\","
        "\"contentHash\":\"prefab-hash\""
        "},"
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"mesh:Large\","
        "\"artifactType\":\"Mesh\","
        "\"loaderId\":\"mesh\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/meshes/Large.nmesh\","
        "\"contentHash\":\"mesh-hash\""
        "}"
        "]"
        "}");

    auto request = MakeThumbnailRequest(root, "prefab:Large");
    request.assetId = assetId;
    request.sourceAssetPath = "Assets/Models/Large.fbx";
    request.artifactPath = RedirectedArtifactPathOrFallback(
        "Library/Artifacts/" + assetId.ToString() + "/Large.nprefab");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 96u;
    request.freshnessInputs = {{"artifact", "large-prefab:v1"}};

    const auto imported = NLS::Engine::Assets::ImportPrefabArtifact(
        prefabPayload,
        assetId,
        {
            {
                assetId,
                "Prefab",
                "prefab:Large",
                "Library/Artifacts/" + assetId.ToString() + "/Large.nprefab"
            },
            {
                assetId,
                "Mesh",
                "mesh:Large",
                "Library/Artifacts/" + assetId.ToString() + "/meshes/Large.nmesh"
            }
        });
    ASSERT_FALSE(imported.diagnostics.HasErrors())
        << FormatSerializationDiagnostics(imported.diagnostics);
    const auto snapshot = BuildPreviewRenderableSnapshot(imported.artifact);
    ASSERT_EQ(snapshot.drawItems.size(), 1u)
        << "Imported model prefabs must preserve mesh sub-asset references for preview generation; "
           "otherwise large prefab thumbnails fall back to the structure placeholder and never retry "
           "the GPU preview path.";
    EXPECT_EQ(
        snapshot.drawItems.front().meshPath,
        "Library/Artifacts/" + assetId.ToString() + "/meshes/Large.nmesh");

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh) << generated->diagnostic;
    EXPECT_TRUE(generated->diagnostic.empty());
    ASSERT_TRUE(generated->cacheEntry.has_value());
    EXPECT_TRUE(std::filesystem::exists(generated->cacheEntry->imagePath));

    const auto evaluated = EvaluateAssetThumbnailCache(request);
    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Fresh)
        << "Large renderable prefabs should get a bounded sampled preview instead of a "
           "structure placeholder or stable failed cache entry.";
    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(
        CountArtifactTelemetryStageForPathSuffix(
            telemetry,
            NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy,
            "meshes/Large.nmesh"),
        0u);
    EXPECT_EQ(
        CountArtifactTelemetryStageForPathSuffix(
            telemetry,
            NLS::Core::Assets::ArtifactLoadTelemetryStage::CpuDeserialize,
            "meshes/Large.nmesh"),
        0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceBuildsSourceTextureRequestFromMetaWhenArtifactRecordIsMissing)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto sourcePath = root / "Assets" / "Textures" / "Loose.png";
    WriteBinaryFile(sourcePath, TinyPng());
    const auto meta = AssetMeta::CreateForAsset(sourcePath);
    ASSERT_TRUE(meta.Save(GetAssetMetaPath(sourcePath)));

    AssetBrowserItem texture;
    texture.kind = AssetBrowserItemKind::SourceAsset;
    texture.type = AssetBrowserItemType::Texture;
    texture.sourceAssetPath = "Assets/Textures/Loose.png";

    const auto request = BuildAssetThumbnailRequestForItem(root, texture, 96u);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->assetId, meta.id);
    EXPECT_EQ(request->kind, AssetThumbnailKind::Texture);
    EXPECT_EQ(request->sourceAssetPath, "Assets/Textures/Loose.png");

    texture.assetId = meta.id;
    const auto requestAfterDatabaseRefresh = BuildAssetThumbnailRequestForItem(root, texture, 96u);
    ASSERT_TRUE(requestAfterDatabaseRefresh.has_value());
    EXPECT_EQ(BuildAssetThumbnailCacheKey(*request), BuildAssetThumbnailCacheKey(*requestAfterDatabaseRefresh));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceKeepsMaterialPreviewQueuedWithoutRenderer)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Materials" / "Body.mat", std::vector<uint8_t>{'<', 'm', 'a', 't', '/', '>'});
    WriteNativeArtifactTextFile(
        root / LibraryArtifactPath("47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae"),
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        "shaderLabMaterialVersion=1\n"
        "shader=?\n"
        "property _BaseColor Color 0.1 0.8 0.4 1.0\n");

    auto request = MakeThumbnailRequest(root, "material:Body");
    request.sourceAssetPath = "Assets/Materials/Body.mat";
    request.artifactPath =
        "Library/Artifacts/47/47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae";
    request.kind = AssetThumbnailKind::MaterialSphere;
    request.requestedSize = 48u;
    request.freshnessInputs = {{ "artifact", "material:v1" }};

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto firstGenerated = service.GenerateNextThumbnail();
    EXPECT_FALSE(firstGenerated.has_value());
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Missing);

    const auto retried = service.GetThumbnail(request);
    EXPECT_EQ(retried.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceRequestFreshnessTracksSourceFileChanges)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto sourcePath = root / "Assets" / "Textures" / "Hero.png";
    WriteBinaryFile(sourcePath, TinyPng());

    AssetBrowserItem texture;
    texture.kind = AssetBrowserItemKind::SourceAsset;
    texture.type = AssetBrowserItemType::Texture;
    texture.assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("a3030303-0303-4303-8303-030303030303"));
    texture.sourceAssetPath = "Assets/Textures/Hero.png";

    const auto first = BuildAssetThumbnailRequestForItem(root, texture, 96u);
    ASSERT_TRUE(first.has_value());

    std::filesystem::resize_file(sourcePath, TinyPng().size() + 1u);
    const auto second = BuildAssetThumbnailRequestForItem(root, texture, 96u);
    ASSERT_TRUE(second.has_value());

    EXPECT_NE(BuildAssetThumbnailCacheKey(*first), BuildAssetThumbnailCacheKey(*second));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceRequestFreshnessUsesFileMetadataWithoutHashingFullSource)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto sourcePath = root / "Assets" / "Textures" / "Hero.png";
    auto firstBytes = TinyPng();
    WriteBinaryFile(sourcePath, firstBytes);

    AssetBrowserItem texture;
    texture.kind = AssetBrowserItemKind::SourceAsset;
    texture.type = AssetBrowserItemType::Texture;
    texture.assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("a3030303-0303-4303-8303-030303030304"));
    texture.sourceAssetPath = "Assets/Textures/Hero.png";

    const auto first = BuildAssetThumbnailRequestForItem(root, texture, 96u);
    ASSERT_TRUE(first.has_value());

    std::error_code error;
    const auto originalWriteTime = std::filesystem::last_write_time(sourcePath, error);
    ASSERT_FALSE(error);

    auto secondBytes = firstBytes;
    ASSERT_FALSE(secondBytes.empty());
    secondBytes.back() ^= 0xffu;
    WriteBinaryFile(sourcePath, secondBytes);
    std::filesystem::last_write_time(sourcePath, originalWriteTime, error);
    ASSERT_FALSE(error);

    const auto second = BuildAssetThumbnailRequestForItem(root, texture, 96u);
    ASSERT_TRUE(second.has_value());

    EXPECT_EQ(std::filesystem::file_size(sourcePath), firstBytes.size());
    EXPECT_EQ(std::filesystem::last_write_time(sourcePath), originalWriteTime);
    EXPECT_EQ(BuildAssetThumbnailCacheKey(*first), BuildAssetThumbnailCacheKey(*second));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceRequestFreshnessTracksGeneratedArtifactManifestChanges)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.gltf", std::vector<uint8_t>{'g', 'l', 't', 'f'});

    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("a4040404-0404-4404-8404-040404040404"));
    const auto artifactDatabasePath = root / "Library" / "ArtifactDB" / "data.mdb";
    WriteBinaryFile(artifactDatabasePath, std::vector<uint8_t>{'v', '1'});

    AssetBrowserItem material;
    material.kind = AssetBrowserItemKind::GeneratedSubAsset;
    material.type = AssetBrowserItemType::Material;
    material.assetId = assetId;
    material.sourceAssetPath = "Assets/Models/Hero.gltf";
    material.subAssetKey = "material:Body";
    material.artifactType = ArtifactType::Material;

    const auto first = BuildAssetThumbnailRequestForItem(root, material, 96u);
    ASSERT_TRUE(first.has_value());

    std::filesystem::resize_file(artifactDatabasePath, 3u);
    const auto second = BuildAssetThumbnailRequestForItem(root, material, 96u);
    ASSERT_TRUE(second.has_value());

    EXPECT_NE(BuildAssetThumbnailCacheKey(*first), BuildAssetThumbnailCacheKey(*second));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceRequestFreshnessTracksSourceModelArtifactManifestChanges)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.gltf", std::vector<uint8_t>{'g', 'l', 't', 'f'});

    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("a5050505-0505-4505-8505-050505050505"));
    const auto artifactDatabasePath = root / "Library" / "ArtifactDB" / "data.mdb";
    const auto prefabArtifactPath = LibraryArtifactPath("670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772");
    const auto prefabPath = root / prefabArtifactPath;
    WriteBinaryFile(artifactDatabasePath, std::vector<uint8_t>{'v', '1'});
    WriteBinaryFile(prefabPath, std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});

    AssetBrowserItem model;
    model.kind = AssetBrowserItemKind::SourceAsset;
    model.type = AssetBrowserItemType::Model;
    model.assetId = assetId;
    model.sourceAssetPath = "Assets/Models/Hero.gltf";
    model.subAssetKey = "prefab:Hero";
    model.artifactPath = prefabArtifactPath;
    model.artifactType = ArtifactType::Prefab;

    const auto first = BuildAssetThumbnailRequestForItem(root, model, 96u);
    ASSERT_TRUE(first.has_value());

    std::filesystem::resize_file(artifactDatabasePath, 3u);
    const auto second = BuildAssetThumbnailRequestForItem(root, model, 96u);
    ASSERT_TRUE(second.has_value());

    EXPECT_NE(BuildAssetThumbnailCacheKey(*first), BuildAssetThumbnailCacheKey(*second));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceRequeuesTextureThumbnailWhenSourceFreshnessChanges)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/Hero.png";
    request.kind = AssetThumbnailKind::Texture;
    request.freshnessInputs = {{"source", "tiny-png:v1"}};

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_TRUE(service.GenerateNextThumbnail().has_value());
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Fresh);

    auto changed = request;
    changed.freshnessInputs = {{"source", "tiny-png:v2"}};
    EXPECT_EQ(service.GetThumbnail(changed).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceReportsFreshPendingAndFallbackStates)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/Hero.png";
    request.kind = AssetThumbnailKind::Texture;
    auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());

    AssetThumbnailService service;
    const auto missing = service.GetThumbnail(request);
    EXPECT_EQ(missing.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_TRUE(missing.cacheEntry.has_value());
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

    WriteBinaryFile(entry->imagePath, TinyPng());
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Fresh, {}));
    const auto fresh = service.GetThumbnail(request);
    EXPECT_EQ(fresh.status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(fresh.imagePath, entry->imagePath);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

    auto unsupported = MakeThumbnailRequest(root, "generic:Hero");
    unsupported.kind = AssetThumbnailKind::GenericPreview;
    const auto fallback = service.GetThumbnail(unsupported);
    EXPECT_EQ(fallback.status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_FALSE(fallback.fallbackIcon.empty());

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceReportsQueuedReadyFailedAndCancelledThumbnailStates)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/Hero.png";
    request.kind = AssetThumbnailKind::Texture;
    request.freshnessInputs = {{"source", "tiny-png:v1"}};

    AssetThumbnailService service;
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Missing);

    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);

    service.SupersedeQueuedRequestsForGeneration("Assets/Other#96");
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Cancelled);

    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    ASSERT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Ready);

    auto unsupported = MakeThumbnailRequest(root, "generic:Unsupported");
    unsupported.kind = AssetThumbnailKind::GenericPreview;
    EXPECT_EQ(service.GetThumbnail(unsupported).status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_EQ(service.GetThumbnailState(unsupported), ThumbnailState::Failed);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, RendererlessPumpKeepsPrefabPreviewQueuedAndGeneratesCpuTexture)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    auto texture = MakeThumbnailRequest(root, "texture:Hero");
    texture.sourceAssetPath = "Assets/Textures/Hero.png";
    texture.kind = AssetThumbnailKind::Texture;
    texture.freshnessInputs = {{"source", "tiny-png:v1"}};

    auto prefab = MakeThumbnailRequest(root, "prefab:Hero");
    prefab.sourceAssetPath = "Assets/Prefabs/Hero.prefab";
    prefab.kind = AssetThumbnailKind::PrefabPreview;
    prefab.freshnessInputs = {{"source", "prefab:v1"}};

    const auto textureEntry = ResolveAssetThumbnailCacheEntry(texture);
    ASSERT_TRUE(textureEntry.has_value());

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(texture).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetThumbnail(prefab).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetQueuedRequestCount(), 2u);

    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    ASSERT_TRUE(generated->cacheEntry.has_value());
    EXPECT_EQ(generated->cacheEntry->cacheKey, textureEntry->cacheKey);
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_TRUE(generated->diagnostic.empty());
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_EQ(service.GetThumbnailState(prefab), ThumbnailState::Queued);
    EXPECT_EQ(service.GetThumbnailState(texture), ThumbnailState::Ready);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceRetriesLegacyPrefabPreviewBudgetFailureCache)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Models" / "Huge.gltf", std::vector<uint8_t>{'g', 'l', 't', 'f'});

    auto request = MakeThumbnailRequest(root, "prefab:Huge");
    request.sourceAssetPath = "Assets/Models/Huge.gltf";
    request.artifactPath = RedirectedArtifactPathOrFallback(
        "Library/Artifacts/a1010101-0101-4101-8101-010101010101/prefab.nprefab");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 96u;
    request.freshnessInputs = {{"artifact", "legacy-budget-failure:v1"}};

    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(
        request,
        AssetThumbnailCacheStatus::Failed,
        "thumbnail-prefab-preview-budget-exceeded"));
    ASSERT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Failed);

    AssetThumbnailService service;
    const auto result = service.GetThumbnail(request);
    EXPECT_EQ(result.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServicePersistsRegularPrefabPreviewBudgetFailureWithoutRequeueLoop)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Prefabs" / "Huge.prefab", std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});
    WriteNativeArtifactTextFile(
        root / "Library" / "Artifacts" / "a1010101-0101-4101-8101-010101010101" / "prefab.nprefab",
        NLS::Core::Assets::ArtifactType::Prefab,
        "prefab",
        1u,
        MinimalPrefabPayload());

    auto request = MakeThumbnailRequest(root, "prefab:Huge");
    request.sourceAssetPath = "Assets/Prefabs/Huge.prefab";
    request.artifactPath = RedirectedArtifactPathOrFallback(
        "Library/Artifacts/a1010101-0101-4101-8101-010101010101/prefab.nprefab");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 96u;
    request.freshnessInputs = {{"artifact", "regular-prefab-budget-failure:v1"}};

    AssetThumbnailService service;
    PrefabBudgetExceededThumbnailPreviewRenderer renderer;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    const auto generated = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(generated->diagnostic, "thumbnail-prefab-preview-budget-exceeded");
    EXPECT_EQ(renderer.renderCount, 1u);
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Failed);

    const auto evaluated = EvaluateAssetThumbnailCache(request);
    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Failed);
    EXPECT_EQ(evaluated.diagnostic, "thumbnail-prefab-preview-budget-exceeded");

    const auto repeated = service.GetThumbnail(request);
    EXPECT_EQ(repeated.status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServicePrioritizesVisibleRequestsBeforeBackgroundRequests)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Background.png", TinyPng());
    WriteBinaryFile(root / "Assets" / "Textures" / "Visible.png", TinyPng());

    auto background = MakeThumbnailRequest(root, "texture:Background");
    background.sourceAssetPath = "Assets/Textures/Background.png";
    background.kind = AssetThumbnailKind::Texture;
    background.priority = ThumbnailRequestPriority::Background;
    background.freshnessInputs = {{"source", "background:v1"}};

    auto visible = MakeThumbnailRequest(root, "texture:Visible");
    visible.sourceAssetPath = "Assets/Textures/Visible.png";
    visible.kind = AssetThumbnailKind::Texture;
    visible.priority = ThumbnailRequestPriority::Visible;
    visible.freshnessInputs = {{"source", "visible:v1"}};

    const auto visibleEntry = ResolveAssetThumbnailCacheEntry(visible);
    ASSERT_TRUE(visibleEntry.has_value());

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(background).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetThumbnail(visible).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetQueuedRequestCount(), 2u);

    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    ASSERT_TRUE(generated->cacheEntry.has_value());
    EXPECT_EQ(generated->cacheEntry->cacheKey, visibleEntry->cacheKey);
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServicePromotesQueuedDuplicateRequestToVisiblePriority)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Duplicate.png", TinyPng());
    WriteBinaryFile(root / "Assets" / "Textures" / "Other.png", TinyPng());

    auto duplicateBackground = MakeThumbnailRequest(root, "texture:Duplicate");
    duplicateBackground.sourceAssetPath = "Assets/Textures/Duplicate.png";
    duplicateBackground.kind = AssetThumbnailKind::Texture;
    duplicateBackground.priority = ThumbnailRequestPriority::Background;
    duplicateBackground.freshnessInputs = {{"source", "duplicate:v1"}};

    auto duplicateVisible = duplicateBackground;
    duplicateVisible.priority = ThumbnailRequestPriority::Visible;

    auto otherBackground = MakeThumbnailRequest(root, "texture:Other");
    otherBackground.sourceAssetPath = "Assets/Textures/Other.png";
    otherBackground.kind = AssetThumbnailKind::Texture;
    otherBackground.priority = ThumbnailRequestPriority::Background;
    otherBackground.freshnessInputs = {{"source", "other:v1"}};

    const auto duplicateEntry = ResolveAssetThumbnailCacheEntry(duplicateVisible);
    ASSERT_TRUE(duplicateEntry.has_value());

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(otherBackground).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetThumbnail(duplicateBackground).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetThumbnail(duplicateVisible).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetQueuedRequestCount(), 2u);

    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    ASSERT_TRUE(generated->cacheEntry.has_value());
    EXPECT_EQ(generated->cacheEntry->cacheKey, duplicateEntry->cacheKey);
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceConsumesSuccessfulThumbnailCacheWriteBudget)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "First.png", TinyPng());
    WriteBinaryFile(root / "Assets" / "Textures" / "Second.png", TinyPng());

    auto first = MakeThumbnailRequest(root, "texture:First");
    first.sourceAssetPath = "Assets/Textures/First.png";
    first.kind = AssetThumbnailKind::Texture;
    first.freshnessInputs = {{"source", "first:v1"}};

    auto second = MakeThumbnailRequest(root, "texture:Second");
    second.sourceAssetPath = "Assets/Textures/Second.png";
    second.kind = AssetThumbnailKind::Texture;
    second.freshnessInputs = {{"source", "second:v1"}};

    AssetThumbnailService service;
    ThumbnailGenerationBudget budget;
    budget.cacheWriteCountBudget = 1u;
    service.SetThumbnailGenerationBudget(budget);

    ASSERT_EQ(service.GetThumbnail(first).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetThumbnail(second).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetQueuedRequestCount(), 2u);

    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

    const auto budgetExhausted = service.GenerateNextThumbnail();
    EXPECT_FALSE(budgetExhausted.has_value());
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_EQ(service.GetThumbnailState(second), ThumbnailState::Queued);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceProcessesPrefabPriorityRequestsInFifoOrder)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();

    auto firstPrefab = MakeThumbnailRequest(root, "prefab:First");
    firstPrefab.sourceAssetPath = "Assets/Prefabs/First.prefab";
    firstPrefab.kind = AssetThumbnailKind::PrefabPreview;
    firstPrefab.freshnessInputs = {{"source", "prefab:first"}};

    auto secondPrefab = MakeThumbnailRequest(root, "prefab:Second");
    secondPrefab.sourceAssetPath = "Assets/Prefabs/Second.prefab";
    secondPrefab.kind = AssetThumbnailKind::PrefabPreview;
    secondPrefab.freshnessInputs = {{"source", "prefab:second"}};

    const auto firstEntry = ResolveAssetThumbnailCacheEntry(firstPrefab);
    const auto secondEntry = ResolveAssetThumbnailCacheEntry(secondPrefab);
    ASSERT_TRUE(firstEntry.has_value());
    ASSERT_TRUE(secondEntry.has_value());

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(firstPrefab).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetThumbnail(secondPrefab).status, AssetThumbnailServiceStatus::Pending);

    CapturingThumbnailPreviewRenderer renderer;
    const auto first = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->status, AssetThumbnailServiceStatus::Pending);
    ASSERT_TRUE(renderer.lastRenderRequest.has_value());
    EXPECT_EQ(BuildAssetThumbnailCacheKey(*renderer.lastRenderRequest), firstEntry->cacheKey);
    auto firstCompleted = service.ConsumeCompletedThumbnail();
    for (int attempt = 0; attempt < 100 && !firstCompleted.has_value(); ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        firstCompleted = service.ConsumeCompletedThumbnail();
    }
    ASSERT_TRUE(firstCompleted.has_value());
    ASSERT_TRUE(firstCompleted->cacheEntry.has_value());
    EXPECT_EQ(firstCompleted->cacheEntry->cacheKey, firstEntry->cacheKey);

    renderer.lastRenderRequest.reset();
    const auto second = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->status, AssetThumbnailServiceStatus::Pending);
    ASSERT_TRUE(renderer.lastRenderRequest.has_value());
    EXPECT_EQ(BuildAssetThumbnailCacheKey(*renderer.lastRenderRequest), secondEntry->cacheKey);
    auto secondCompleted = service.ConsumeCompletedThumbnail();
    for (int attempt = 0; attempt < 100 && !secondCompleted.has_value(); ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        secondCompleted = service.ConsumeCompletedThumbnail();
    }
    ASSERT_TRUE(secondCompleted.has_value());
    ASSERT_TRUE(secondCompleted->cacheEntry.has_value());
    EXPECT_EQ(secondCompleted->cacheEntry->cacheKey, secondEntry->cacheKey);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceLetsRegularRequestsProgressDuringPrefabPriorityBurst)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    std::vector<AssetThumbnailRequest> prefabs;
    prefabs.reserve(5u);
    for (size_t index = 0u; index < 5u; ++index)
    {
        auto prefab = MakeThumbnailRequest(root, "prefab:Item" + std::to_string(index));
        prefab.sourceAssetPath = "Assets/Prefabs/Item" + std::to_string(index) + ".prefab";
        prefab.kind = AssetThumbnailKind::PrefabPreview;
        prefab.freshnessInputs = {{"source", "prefab:" + std::to_string(index)}};
        prefabs.push_back(prefab);
    }

    auto texture = MakeThumbnailRequest(root, "texture:Hero");
    texture.sourceAssetPath = "Assets/Textures/Hero.png";
    texture.kind = AssetThumbnailKind::Texture;
    texture.freshnessInputs = {{"source", "tiny-png:v1"}};

    const auto textureEntry = ResolveAssetThumbnailCacheEntry(texture);
    ASSERT_TRUE(textureEntry.has_value());

    AssetThumbnailService service;
    for (const auto& prefab : prefabs)
        ASSERT_EQ(service.GetThumbnail(prefab).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetThumbnail(texture).status, AssetThumbnailServiceStatus::Pending);

    const auto regular = service.GenerateNextThumbnail();
    ASSERT_TRUE(regular.has_value());
    ASSERT_TRUE(regular->cacheEntry.has_value());
    EXPECT_EQ(regular->cacheEntry->cacheKey, textureEntry->cacheKey);
    EXPECT_EQ(regular->status, AssetThumbnailServiceStatus::Fresh);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, BackgroundPumpDefersHeavyModelAndPrefabPreviewRequests)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();

    AssetThumbnailService service;
    for (size_t index = 0u; index < 9u; ++index)
    {
        auto prefab = MakeThumbnailRequest(root, "prefab:LargeFolderItem" + std::to_string(index));
        prefab.sourceAssetPath = "Assets/Prefabs/LargeFolderItem" + std::to_string(index) + ".prefab";
        prefab.kind = AssetThumbnailKind::PrefabPreview;
        prefab.freshnessInputs = {{"source", "prefab:" + std::to_string(index)}};
        ASSERT_EQ(service.GetThumbnail(prefab).status, AssetThumbnailServiceStatus::Pending);
    }

    HeavyOnlyThumbnailPreviewRenderer renderer;
    EXPECT_FALSE(service.StartNextThumbnailGeneration(renderer))
        << "When a GPU preview renderer is available for heavy prefab/model previews, the "
           "background CPU thumbnail pump must leave those requests for the renderer path.";
    EXPECT_FALSE(service.HasInFlightRequest());
    EXPECT_EQ(service.GetQueuedRequestCount(), 9u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceCpuPreviewQueueSkipsHeavyRequestsAndKeepsThemQueued)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    auto firstModel = MakeThumbnailRequest(root, "mesh:First");
    firstModel.sourceAssetPath = "Assets/Models/First.fbx";
    firstModel.kind = AssetThumbnailKind::ModelPreview;
    firstModel.freshnessInputs = {{"source", "model:first"}};

    auto texture = MakeThumbnailRequest(root, "texture:Hero");
    texture.sourceAssetPath = "Assets/Textures/Hero.png";
    texture.kind = AssetThumbnailKind::Texture;
    texture.freshnessInputs = {{"source", "tiny-png:v1"}};

    auto secondModel = MakeThumbnailRequest(root, "mesh:Second");
    secondModel.sourceAssetPath = "Assets/Models/Second.fbx";
    secondModel.kind = AssetThumbnailKind::ModelPreview;
    secondModel.freshnessInputs = {{"source", "model:second"}};

    const auto firstModelEntry = ResolveAssetThumbnailCacheEntry(firstModel);
    const auto textureEntry = ResolveAssetThumbnailCacheEntry(texture);
    const auto secondModelEntry = ResolveAssetThumbnailCacheEntry(secondModel);
    ASSERT_TRUE(firstModelEntry.has_value());
    ASSERT_TRUE(textureEntry.has_value());
    ASSERT_TRUE(secondModelEntry.has_value());

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(firstModel).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetThumbnail(texture).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetThumbnail(secondModel).status, AssetThumbnailServiceStatus::Pending);

    HeavyOnlyThumbnailPreviewRenderer renderer;
    ASSERT_TRUE(service.StartNextThumbnailGeneration(renderer));
    std::optional<AssetThumbnailServiceResult> completedTexture;
    for (int attempt = 0; attempt < 100 && !completedTexture.has_value(); ++attempt)
    {
        completedTexture = service.ConsumeCompletedThumbnail();
        if (!completedTexture.has_value())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(completedTexture.has_value());
    ASSERT_TRUE(completedTexture->cacheEntry.has_value());
    EXPECT_EQ(completedTexture->cacheEntry->cacheKey, textureEntry->cacheKey);
    EXPECT_EQ(completedTexture->status, AssetThumbnailServiceStatus::Fresh);

    EXPECT_EQ(service.GetThumbnailState(firstModel), ThumbnailState::Queued);
    EXPECT_EQ(service.GetThumbnailState(secondModel), ThumbnailState::Queued);
    EXPECT_EQ(service.GetQueuedRequestCount(), 2u);
    EXPECT_FALSE(service.StartNextThumbnailGeneration(renderer))
        << "Remaining heavy model previews should wait for the renderer pump when one is "
           "available instead of falling back to the CPU preview path.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceGeneratesAndReusesPersistentTextureThumbnails)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/Hero.png";
    request.kind = AssetThumbnailKind::Texture;
    request.freshnessInputs = {{"source", "tiny-png:v1"}};

    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());

    AssetThumbnailService service;
    const auto pending = service.GetThumbnail(request);
    ASSERT_EQ(pending.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);
    ASSERT_TRUE(generated->cacheEntry.has_value());
    EXPECT_EQ(generated->imagePath, entry->imagePath);
    EXPECT_TRUE(std::filesystem::is_regular_file(entry->imagePath));
    EXPECT_TRUE(std::filesystem::is_regular_file(entry->metadataPath));
    EXPECT_TRUE(IsAssetThumbnailCachePathContained(root, entry->imagePath));
    EXPECT_EQ(entry->imagePath.lexically_relative(root).begin()->generic_string(), std::string("Library"));
    EXPECT_FALSE(std::filesystem::exists(root / "Assets" / "AssetThumbnails"));
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);

    AssetThumbnailService restartedService;
    const auto reused = restartedService.GetThumbnail(request);
    EXPECT_EQ(reused.status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(reused.imagePath, entry->imagePath);
    EXPECT_EQ(restartedService.GetQueuedRequestCount(), 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceGeneratesTextureThumbnailFromGeneratedTextureArtifact)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.gltf", std::vector<uint8_t>{'g', 'l', 't', 'f'});
    const auto artifactPath =
        root / LibraryArtifactPath("c6d69f90e0b33e0d299d12cd1ae95ab4c06d34fd67c24a544d85efad52f70942");
    WriteBinaryFile(
        artifactPath,
        NLS::Render::Assets::SerializeTextureArtifact(RgbaTextureArtifact2x1()));

    auto request = MakeThumbnailRequest(root, "texture:body");
    request.sourceAssetPath = "Assets/Models/Hero.gltf";
    request.artifactPath = "Library/Artifacts/c6/c6d69f90e0b33e0d299d12cd1ae95ab4c06d34fd67c24a544d85efad52f70942";
    request.kind = AssetThumbnailKind::Texture;
    request.freshnessInputs = {{"artifact", "texture:v1"}};

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_TRUE(std::filesystem::exists(generated->imagePath));
    const NLS::Image decoded(generated->imagePath.string(), false);
    ASSERT_NE(decoded.GetData(), nullptr);
    EXPECT_GT(decoded.GetWidth(), 0);
    EXPECT_GT(decoded.GetHeight(), 0);
    EXPECT_GT(CountOpaquePixels(decoded), 0u)
        << "Native texture artifacts loaded through backed pixel views must remain usable by thumbnail generation.";

    const auto evaluated = EvaluateAssetThumbnailCache(request);
    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Fresh);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceRejectsGeneratedTextureArtifactSymlinkInsideArtifactRoot)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto outside =
        std::filesystem::temp_directory_path() /
        ("nullus_thumbnail_artifact_symlink_outside_" + NLS::Guid::New().ToString());
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.gltf", std::vector<uint8_t>{'g', 'l', 't', 'f'});
    WriteBinaryFile(
        outside / "c6d69f90e0b33e0d299d12cd1ae95ab4c06d34fd67c24a544d85efad52f70942",
        NLS::Render::Assets::SerializeTextureArtifact(RgbaTextureArtifact2x1()));

    const auto linkPath =
        root / LibraryArtifactPath("c6d69f90e0b33e0d299d12cd1ae95ab4c06d34fd67c24a544d85efad52f70942");
    std::filesystem::create_directories(linkPath.parent_path());
    std::error_code error;
    std::filesystem::create_symlink(outside / "c6d69f90e0b33e0d299d12cd1ae95ab4c06d34fd67c24a544d85efad52f70942", linkPath, error);
    if (error)
    {
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(outside);
        GTEST_SKIP() << "File symlink creation is not available in this environment.";
    }

    auto request = MakeThumbnailRequest(root, "texture:body");
    request.sourceAssetPath = "Assets/Models/Hero.gltf";
    request.artifactPath = "Library/Artifacts/c6/c6d69f90e0b33e0d299d12cd1ae95ab4c06d34fd67c24a544d85efad52f70942";
    request.kind = AssetThumbnailKind::Texture;
    request.freshnessInputs = {{"artifact", "texture-symlink:v1"}};

    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(generated->diagnostic, "thumbnail-texture-artifact-path-invalid");
    EXPECT_FALSE(std::filesystem::exists(entry->imagePath));

    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead),
        0u);

    const auto evaluated = EvaluateAssetThumbnailCache(request);
    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Failed);
    EXPECT_EQ(evaluated.diagnostic, "thumbnail-texture-artifact-path-invalid");

    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside);
}

TEST(AssetThumbnailCacheTests, ServiceRejectsGeneratedMaterialArtifactSymlinkWithoutSourceFallback)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto outside =
        std::filesystem::temp_directory_path() /
        ("nullus_thumbnail_material_symlink_outside_" + NLS::Guid::New().ToString());
    WriteBinaryFile(
        root / "Assets" / "Materials" / "Body.mat",
        std::vector<uint8_t>{'<', 'm', 'a', 't', 'e', 'r', 'i', 'a', 'l', '/', '>'});
    WriteNativeArtifactTextFile(
        outside / "Body.mat",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        "shaderLabMaterialVersion=1\n"
        "shader=?\n"
        "property _BaseColor Color 0.9 0.2 0.1 1.0\n");

    const auto linkPath =
        root / LibraryArtifactPath("47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae");
    std::filesystem::create_directories(linkPath.parent_path());
    std::error_code error;
    std::filesystem::create_symlink(outside / "Body.mat", linkPath, error);
    if (error)
    {
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(outside);
        GTEST_SKIP() << "File symlink creation is not available in this environment.";
    }

    auto request = MakeThumbnailRequest(root, "material:Body");
    request.sourceAssetPath = "Assets/Materials/Body.mat";
    request.artifactPath =
        "Library/Artifacts/47/47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae";
    request.kind = AssetThumbnailKind::MaterialSphere;
    request.freshnessInputs = {{"artifact", "material-symlink:v1"}};

    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    CapturingThumbnailPreviewRenderer renderer;
    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto generated = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(generated->diagnostic, "thumbnail-material-artifact-path-invalid");
    EXPECT_FALSE(std::filesystem::exists(entry->imagePath));

    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead),
        0u);

    const auto evaluated = EvaluateAssetThumbnailCache(request);
    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Failed);
    EXPECT_EQ(evaluated.diagnostic, "thumbnail-material-artifact-path-invalid");

    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside);
}

TEST(AssetThumbnailCacheTests, ServiceRejectsGeneratedPrefabArtifactSymlinkWithoutSourceFallback)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto outside =
        std::filesystem::temp_directory_path() /
        ("nullus_thumbnail_prefab_symlink_outside_" + NLS::Guid::New().ToString());
    WriteBinaryFile(
        root / "Assets" / "Prefabs" / "Lamp.prefab",
        std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});
    WriteNativeArtifactTextFile(
        outside / "Lamp.prefab",
        NLS::Core::Assets::ArtifactType::Prefab,
        "prefab",
        1u,
        MinimalPrefabPayload());

    const auto linkPath =
        root / LibraryArtifactPath("670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772");
    std::filesystem::create_directories(linkPath.parent_path());
    std::error_code error;
    std::filesystem::create_symlink(outside / "Lamp.prefab", linkPath, error);
    if (error)
    {
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(outside);
        GTEST_SKIP() << "File symlink creation is not available in this environment.";
    }

    auto request = MakeThumbnailRequest(root, "prefab:Lamp");
    request.sourceAssetPath = "Assets/Prefabs/Lamp.prefab";
    request.artifactPath = "Library/Artifacts/67/670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772";
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.freshnessInputs = {{"artifact", "prefab-symlink:v1"}};

    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    CapturingThumbnailPreviewRenderer renderer;
    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto generated = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(generated->diagnostic, "thumbnail-prefab-artifact-path-invalid");
    EXPECT_FALSE(std::filesystem::exists(entry->imagePath));

    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead),
        0u);

    const auto evaluated = EvaluateAssetThumbnailCache(request);
    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Failed);
    EXPECT_EQ(evaluated.diagnostic, "thumbnail-prefab-artifact-path-invalid");

    std::filesystem::remove_all(root);
    std::filesystem::remove_all(outside);
}

TEST(AssetThumbnailCacheTests, ServiceRecordsOversizedSourceImageAsStableFailureWithoutDecode)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "TooLarge.png", PngHeaderOnly(131072u, 131072u));

    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/TooLarge.png";
    request.kind = AssetThumbnailKind::Texture;
    request.freshnessInputs = {{"source", "oversized-png:v1"}};

    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_EQ(generated->diagnostic, "thumbnail-source-preview-budget-exceeded");
    EXPECT_FALSE(std::filesystem::exists(entry->imagePath));

    const auto evaluated = EvaluateAssetThumbnailCache(request);
    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Failed);
    EXPECT_EQ(evaluated.diagnostic, "thumbnail-source-preview-budget-exceeded");

    const auto repeated = service.GetThumbnail(request);
    EXPECT_EQ(repeated.status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(repeated.diagnostic, "thumbnail-source-preview-budget-exceeded");
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceRecordsOversizedJpegWithLateSofAsStableFailureWithoutDecode)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(
        root / "Assets" / "Textures" / "LateSof.jpg",
        JpegWithLargeAppSegmentBeforeSof(8192u, 8192u));

    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/LateSof.jpg";
    request.kind = AssetThumbnailKind::Texture;
    request.freshnessInputs = {{"source", "late-sof-jpeg:v1"}};

    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_EQ(generated->diagnostic, "thumbnail-source-preview-budget-exceeded");
    EXPECT_FALSE(std::filesystem::exists(entry->imagePath));

    const auto evaluated = EvaluateAssetThumbnailCache(request);
    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Failed);
    EXPECT_EQ(evaluated.diagnostic, "thumbnail-source-preview-budget-exceeded");

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceRecordsBmpInt32MinHeightAsBudgetFailureWithoutOverflow)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(
        root / "Assets" / "Textures" / "Impossible.bmp",
        BmpWithRawHeight(16u, 0x80000000u));

    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/Impossible.bmp";
    request.kind = AssetThumbnailKind::Texture;
    request.freshnessInputs = {{"source", "bmp-int32-min-height:v1"}};

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_EQ(generated->diagnostic, "thumbnail-source-preview-budget-exceeded");

    const auto evaluated = EvaluateAssetThumbnailCache(request);
    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Failed);
    EXPECT_EQ(evaluated.diagnostic, "thumbnail-source-preview-budget-exceeded");

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServicePersistsFailedMetadataForUnsupportedGeneratedTextureArtifact)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.gltf", std::vector<uint8_t>{'g', 'l', 't', 'f'});
    const auto artifactPath =
        root / LibraryArtifactPath("c6d69f90e0b33e0d299d12cd1ae95ab4c06d34fd67c24a544d85efad52f70942");
    WriteBinaryFile(artifactPath, std::vector<uint8_t>{'n', 'o', 't', '-', 'a', '-', 't', 'e', 'x'});

    auto request = MakeThumbnailRequest(root, "texture:body");
    request.sourceAssetPath = "Assets/Models/Hero.gltf";
    request.artifactPath = "Library/Artifacts/c6/c6d69f90e0b33e0d299d12cd1ae95ab4c06d34fd67c24a544d85efad52f70942";
    request.kind = AssetThumbnailKind::Texture;
    request.freshnessInputs = {{"artifact", "unsupported-texture:v1"}};

    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_EQ(generated->diagnostic, "thumbnail-texture-artifact-unsupported");
    EXPECT_TRUE(std::filesystem::is_regular_file(entry->metadataPath));

    const auto evaluated = EvaluateAssetThumbnailCache(request);
    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Failed);
    EXPECT_EQ(evaluated.diagnostic, "thumbnail-texture-artifact-unsupported");
    EXPECT_FALSE(std::filesystem::exists(entry->imagePath));

    EXPECT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceRecordsOversizedGeneratedTextureArtifactBeforePayloadLoad)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.gltf", std::vector<uint8_t>{'g', 'l', 't', 'f'});
    const auto artifactPath =
        root / LibraryArtifactPath("1e0eefe13afbe66136a8d56a2e6bc8848a815d0ee3d39839659329e186cb2d8c");
    WriteBinaryFile(artifactPath, NativeTextureArtifactHeaderOnly(131072u, 131072u));

    auto request = MakeThumbnailRequest(root, "texture:huge");
    request.sourceAssetPath = "Assets/Models/Hero.gltf";
    request.artifactPath = "Library/Artifacts/1e/1e0eefe13afbe66136a8d56a2e6bc8848a815d0ee3d39839659329e186cb2d8c";
    request.kind = AssetThumbnailKind::Texture;
    request.freshnessInputs = {{"artifact", "oversized-texture:v1"}};

    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_EQ(generated->diagnostic, "thumbnail-source-preview-budget-exceeded");
    EXPECT_FALSE(std::filesystem::exists(entry->imagePath));

    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_GE(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead),
        1u);
    EXPECT_EQ(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy),
        0u);
    EXPECT_EQ(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::CpuDeserialize),
        0u);

    const auto evaluated = EvaluateAssetThumbnailCache(request);
    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Failed);
    EXPECT_EQ(evaluated.diagnostic, "thumbnail-source-preview-budget-exceeded");

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceRejectsMalformedTextureArtifactBeforeFullPayloadRead)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.gltf", std::vector<uint8_t>{'g', 'l', 't', 'f'});
    const auto artifactPath =
        root / LibraryArtifactPath("470886b56c3fdc232ab0b6fbb442fdab0b12b75fd0ec32c1eddbf98e79859c61");
    WriteBinaryFile(
        artifactPath,
        NativeArtifactHeaderOnly(NLS::Core::Assets::ArtifactType::Texture, 4u, 2ull * 1024ull * 1024ull, 64u));
    std::filesystem::resize_file(artifactPath, 2ull * 1024ull * 1024ull + 128ull);

    auto request = MakeThumbnailRequest(root, "texture:broken");
    request.sourceAssetPath = "Assets/Models/Hero.gltf";
    request.artifactPath = "Library/Artifacts/47/470886b56c3fdc232ab0b6fbb442fdab0b12b75fd0ec32c1eddbf98e79859c61";
    request.kind = AssetThumbnailKind::Texture;
    request.freshnessInputs = {{"artifact", "broken-texture:v1"}};

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_EQ(generated->diagnostic, "thumbnail-texture-artifact-unsupported");

    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy),
        0u);
    EXPECT_EQ(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::CpuDeserialize),
        0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceSamplesOversizedMeshPreviewWithoutFullPayloadRead)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Models" / "City.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    const auto artifactPath =
        root / LibraryArtifactPath("21bb5a71075a04ac35b0f324a6ebaeb38d80fe1f76a45048c1f03633c4314423");
    WriteBinaryFile(
        artifactPath,
        NLS::Render::Assets::SerializeMeshArtifact(OversizedMeshArtifact()));

    auto request = MakeThumbnailRequest(root, "mesh:City");
    request.sourceAssetPath = "Assets/Models/City.fbx";
    request.artifactPath = "Library/Artifacts/21/21bb5a71075a04ac35b0f324a6ebaeb38d80fe1f76a45048c1f03633c4314423";
    request.kind = AssetThumbnailKind::ModelPreview;
    request.requestedSize = 96u;
    request.freshnessInputs = {{"artifact", "oversized-mesh:v1"}};

    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh) << generated->diagnostic;
    EXPECT_TRUE(generated->diagnostic.empty());
    EXPECT_TRUE(std::filesystem::exists(entry->metadataPath));
    EXPECT_TRUE(std::filesystem::exists(entry->imagePath));
    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_GE(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead),
        1u);
    EXPECT_EQ(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy),
        0u);
    EXPECT_EQ(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::CpuDeserialize),
        0u);

    const auto evaluated = EvaluateAssetThumbnailCache(request);
    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Fresh);
    EXPECT_TRUE(evaluated.diagnostic.empty());

    const auto repeated = service.GetThumbnail(request);
    EXPECT_EQ(repeated.status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceRejectsMalformedMeshArtifactBeforeFullPayloadRead)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Models" / "City.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    const auto artifactPath =
        root / LibraryArtifactPath("21bb5a71075a04ac35b0f324a6ebaeb38d80fe1f76a45048c1f03633c4314423");
    WriteBinaryFile(
        artifactPath,
        NativeArtifactHeaderOnly(NLS::Core::Assets::ArtifactType::Mesh, 3u, 2ull * 1024ull * 1024ull, 64u));
    std::filesystem::resize_file(artifactPath, 2ull * 1024ull * 1024ull + 128ull);

    auto request = MakeThumbnailRequest(root, "mesh:City");
    request.sourceAssetPath = "Assets/Models/City.fbx";
    request.artifactPath = "Library/Artifacts/21/21bb5a71075a04ac35b0f324a6ebaeb38d80fe1f76a45048c1f03633c4314423";
    request.kind = AssetThumbnailKind::ModelPreview;
    request.freshnessInputs = {{"artifact", "broken-mesh:v1"}};

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Failed);
    EXPECT_TRUE(generated->diagnostic == "thumbnail-model-mesh-artifact-read-failed" ||
        generated->diagnostic == "thumbnail-model-mesh-artifact-missing");

    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_GE(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead),
        1u);
    EXPECT_EQ(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy),
        0u);
    EXPECT_EQ(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::CpuDeserialize),
        0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceRecordsOversizedMaterialPreviewBeforePayloadCopy)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.gltf", std::vector<uint8_t>{'g', 'l', 't', 'f'});

    const auto artifactPath =
        root / LibraryArtifactPath("a72c61cca07dd301e2a0719e6c945d0534a9936316ebd7527e3f4a738f1b93a0");
    WriteNativeArtifactTextFile(
        artifactPath,
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        std::string(2u * 1024u * 1024u, 'm'));

    auto request = MakeThumbnailRequest(root, "material:Huge");
    request.sourceAssetPath = "Assets/Models/Hero.gltf";
    request.artifactPath = "Library/Artifacts/a7/a72c61cca07dd301e2a0719e6c945d0534a9936316ebd7527e3f4a738f1b93a0";
    request.kind = AssetThumbnailKind::MaterialSphere;
    request.requestedSize = 96u;
    request.freshnessInputs = {{"artifact", "oversized-material:v1"}};

    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto generated = service.GenerateNextThumbnail();
    EXPECT_FALSE(generated.has_value());
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);
    EXPECT_FALSE(std::filesystem::exists(entry->imagePath));

    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead),
        0u);
    EXPECT_EQ(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy),
        0u);

    const auto evaluated = EvaluateAssetThumbnailCache(request);
    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Missing);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceDefersOversizedSourcePrefabPreviewWithoutGpuRenderer)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Prefabs" / "Huge.prefab", std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});

    const auto artifactPath =
        root / LibraryArtifactPath("670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772");
    WriteNativeArtifactTextFile(
        artifactPath,
        NLS::Core::Assets::ArtifactType::Prefab,
        "prefab",
        1u,
        std::string(2u * 1024u * 1024u, 'p'));

    auto request = MakeThumbnailRequest(root, "prefab:Huge");
    request.sourceAssetPath = "Assets/Prefabs/Huge.prefab";
    request.artifactPath = "Library/Artifacts/67/670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772";
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 96u;
    request.freshnessInputs = {{"artifact", "oversized-prefab:v1"}};

    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto generated = service.GenerateNextThumbnail();
    EXPECT_FALSE(generated.has_value());
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);
    EXPECT_FALSE(std::filesystem::exists(entry->imagePath));

    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead),
        0u);
    EXPECT_EQ(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy),
        0u);

    const auto evaluated = EvaluateAssetThumbnailCache(request);
    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Missing);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceRejectsTrailingNativeMaterialPreviewBeforeFullRead)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.gltf", std::vector<uint8_t>{'g', 'l', 't', 'f'});

    const auto artifactPath =
        root / LibraryArtifactPath("ce779663dbe192580e74969a717775df76de05fe97ee3e6277d979b9aad290d2");
    WriteNativeArtifactTextFileWithTrailingBytes(
        artifactPath,
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        "material",
        2u * 1024u * 1024u);

    auto request = MakeThumbnailRequest(root, "material:Trailing");
    request.sourceAssetPath = "Assets/Models/Hero.gltf";
    request.artifactPath = "Library/Artifacts/ce/ce779663dbe192580e74969a717775df76de05fe97ee3e6277d979b9aad290d2";
    request.kind = AssetThumbnailKind::MaterialSphere;
    request.freshnessInputs = {{"artifact", "trailing-material:v1"}};

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto generated = service.GenerateNextThumbnail();
    EXPECT_FALSE(generated.has_value());
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);

    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead),
        0u);
    EXPECT_EQ(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy),
        0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceRejectsTrailingNativePrefabPreviewBeforeFullRead)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Prefabs" / "Trailing.prefab", std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});

    const auto artifactPath =
        root / LibraryArtifactPath("670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772");
    WriteNativeArtifactTextFileWithTrailingBytes(
        artifactPath,
        NLS::Core::Assets::ArtifactType::Prefab,
        "prefab",
        1u,
        R"({"format":"Nullus.ObjectGraph.Prefab","version":1,"objects":[]})",
        2u * 1024u * 1024u);

    auto request = MakeThumbnailRequest(root, "prefab:Trailing");
    request.sourceAssetPath = "Assets/Prefabs/Trailing.prefab";
    request.artifactPath = "Library/Artifacts/67/670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772";
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.freshnessInputs = {{"artifact", "trailing-prefab:v1"}};

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto generated = service.GenerateNextThumbnail();
    EXPECT_FALSE(generated.has_value());
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);

    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead),
        0u);
    EXPECT_EQ(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy),
        0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceDownsamplesGeneratedTextureArtifactThumbnailToRequestedSize)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.gltf", std::vector<uint8_t>{'g', 'l', 't', 'f'});
    const auto artifactPath =
        root / LibraryArtifactPath("c6d69f90e0b33e0d299d12cd1ae95ab4c06d34fd67c24a544d85efad52f70942");
    WriteBinaryFile(
        artifactPath,
        NLS::Render::Assets::SerializeTextureArtifact(RgbaTextureArtifact4x2()));

    auto request = MakeThumbnailRequest(root, "texture:body");
    request.sourceAssetPath = "Assets/Models/Hero.gltf";
    request.artifactPath = "Library/Artifacts/c6/c6d69f90e0b33e0d299d12cd1ae95ab4c06d34fd67c24a544d85efad52f70942";
    request.kind = AssetThumbnailKind::Texture;
    request.requestedSize = 2u;
    request.freshnessInputs = {{"artifact", "texture-4x2:v1"}};

    ExpectGeneratedFreshPng(root, request, 2, 1);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceDownsamplesPersistentTextureThumbnailToRequestedSize)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Wide.bmp", Bmp2x1());

    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/Wide.bmp";
    request.kind = AssetThumbnailKind::Texture;
    request.requestedSize = 1u;
    request.freshnessInputs = {{"source", "wide-png:v1"}};

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);

    const NLS::Image decoded(generated->imagePath.string(), false);
    EXPECT_EQ(decoded.GetWidth(), 1);
    EXPECT_EQ(decoded.GetHeight(), 1);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceRecordsUnsupportedTextureThumbnailExtensionsAsStableFailure)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Compressed.dds", std::vector<uint8_t>{'D', 'D', 'S', ' '});

    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/Compressed.dds";
    request.kind = AssetThumbnailKind::Texture;
    request.freshnessInputs = {{"source", "dds:v1"}};

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_EQ(generated->diagnostic, "thumbnail-texture-extension-unsupported");

    const auto evaluated = EvaluateAssetThumbnailCache(request);
    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Failed);
    EXPECT_EQ(evaluated.diagnostic, "thumbnail-texture-extension-unsupported");

    const auto repeated = service.GetThumbnail(request);
    EXPECT_EQ(repeated.status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(repeated.diagnostic, "thumbnail-texture-extension-unsupported");
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceRetriesCachedWorkerStartFailures)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/Hero.png";
    request.kind = AssetThumbnailKind::Texture;
    request.freshnessInputs = {{"source", "tiny-png:v1"}};

    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(
        request,
        AssetThumbnailCacheStatus::Failed,
        "thumbnail-generation-worker-start-failed"));

    const auto evaluated = EvaluateAssetThumbnailCache(request);
    ASSERT_EQ(evaluated.status, AssetThumbnailCacheStatus::Failed);
    ASSERT_EQ(evaluated.diagnostic, "thumbnail-generation-worker-start-failed");

    AssetThumbnailService service;
    const auto repeated = service.GetThumbnail(request);
    EXPECT_EQ(repeated.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(repeated.diagnostic, "thumbnail-generation-worker-start-failed");
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceKeepsQueuedRequestWhenAsyncWorkerSchedulingFails)
{
    using namespace NLS::Editor::Assets;

    NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
#if defined(NLS_ENABLE_TEST_HOOKS)
    NLS::Base::Jobs::ResetJobSystemForTesting();
#endif

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/Hero.png";
    request.kind = AssetThumbnailKind::Texture;
    request.freshnessInputs = {{"source", "tiny-png:v1"}};

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetQueuedRequestCount(), 1u);

    EXPECT_FALSE(service.StartNextThumbnailGeneration());
    EXPECT_FALSE(service.HasInFlightRequest());
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u)
        << "Rejected JobSystem scheduling must restore the thumbnail queue instead of leaving a loading tombstone.";
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceRetriesCachedTransientExceptionFailures)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/Hero.png";
    request.kind = AssetThumbnailKind::Texture;
    request.freshnessInputs = {{"source", "tiny-png:v1"}};

    for (const std::string diagnostic : {
        "thumbnail-generation-out-of-memory",
        "thumbnail-generation-exception"
    })
    {
        ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(
            request,
            AssetThumbnailCacheStatus::Failed,
            diagnostic));

        const auto evaluated = EvaluateAssetThumbnailCache(request);
        ASSERT_EQ(evaluated.status, AssetThumbnailCacheStatus::Failed);
        ASSERT_EQ(evaluated.diagnostic, diagnostic);

        AssetThumbnailService service;
        const auto repeated = service.GetThumbnail(request);
        EXPECT_EQ(repeated.status, AssetThumbnailServiceStatus::Pending);
        EXPECT_EQ(repeated.diagnostic, diagnostic);
        EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

        const auto generated = service.GenerateNextThumbnail();
        ASSERT_TRUE(generated.has_value());
        EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);
        EXPECT_TRUE(generated->diagnostic.empty());
    }

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceSupersedesQueuedRequestsWhenGenerationChanges)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    auto small = MakeThumbnailRequest(root, {});
    small.sourceAssetPath = "Assets/Textures/Hero.png";
    small.kind = AssetThumbnailKind::Texture;
    small.requestedSize = 64u;
    small.freshnessInputs = {{"source", "tiny-png:v1"}};

    auto large = small;
    large.requestedSize = 128u;

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(small).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

    service.SupersedeQueuedRequestsForGeneration("Assets");
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);

    ASSERT_EQ(service.GetThumbnail(large).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);

    const auto smallEntry = ResolveAssetThumbnailCacheEntry(small);
    ASSERT_TRUE(smallEntry.has_value());
    EXPECT_FALSE(std::filesystem::exists(smallEntry->metadataPath));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceSkipsCachePublishWhenFreshnessInputsChanged)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto texturePath = root / "Assets" / "Textures" / "Hero.png";
    WriteBinaryFile(texturePath, TinyPng());

    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/Hero.png";
    request.kind = AssetThumbnailKind::Texture;
    request.freshnessInputs = {{"source-file", FileStampForTest(texturePath)}};
    const auto staleEntry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(staleEntry.has_value());

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    for (int attempt = 0; attempt < 20 && FileStampForTest(texturePath) == request.freshnessInputs.front().stamp; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        WriteBinaryFile(texturePath, Bmp2x1());
    }
    ASSERT_NE(FileStampForTest(texturePath), request.freshnessInputs.front().stamp);

    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(generated->diagnostic, "thumbnail-request-stale");
    EXPECT_FALSE(std::filesystem::exists(staleEntry->imagePath));
    EXPECT_FALSE(std::filesystem::exists(staleEntry->metadataPath));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceGeneratesTextureThumbnailAsynchronously)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/Hero.png";
    request.kind = AssetThumbnailKind::Texture;
    request.freshnessInputs = {{"source", "tiny-png:v1"}};

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_TRUE(service.StartNextThumbnailGeneration());
    EXPECT_TRUE(service.HasInFlightRequest());

    std::optional<AssetThumbnailServiceResult> generated;
    for (int attempt = 0; attempt < 100 && !generated.has_value(); ++attempt)
    {
        generated = service.ConsumeCompletedThumbnail();
        if (!generated.has_value())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_FALSE(service.HasInFlightRequest());
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceSupersedeStartsNewGenerationWhileOldInFlightDrains)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "First.png", TinyPng());
    WriteBinaryFile(root / "Assets" / "Textures" / "Second.png", TinyPng());

    auto first = MakeThumbnailRequest(root, {});
    first.sourceAssetPath = "Assets/Textures/First.png";
    first.kind = AssetThumbnailKind::Texture;
    first.freshnessInputs = {{"source", "first:v1"}};

    auto second = first;
    second.sourceAssetPath = "Assets/Textures/Second.png";
    second.freshnessInputs = {{"source", "second:v1"}};

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(first).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetThumbnail(second).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetQueuedRequestCount(), 2u);
    ASSERT_TRUE(service.StartNextThumbnailGeneration());
    EXPECT_TRUE(service.HasInFlightRequest());
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

    service.SupersedeQueuedRequestsForGeneration("Assets/Other#96");
    EXPECT_TRUE(service.HasInFlightRequest());
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);

    ASSERT_EQ(service.GetThumbnail(second).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_TRUE(service.StartNextThumbnailGeneration());
    EXPECT_TRUE(service.HasInFlightRequest());

    std::optional<AssetThumbnailServiceResult> generated;
    for (int attempt = 0; attempt < 100 && !generated.has_value(); ++attempt)
    {
        generated = service.ConsumeCompletedThumbnail();
        if (!generated.has_value())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);

    const auto secondEntry = ResolveAssetThumbnailCacheEntry(second);
    ASSERT_TRUE(secondEntry.has_value());
    EXPECT_EQ(generated->imagePath, secondEntry->imagePath);
    EXPECT_TRUE(std::filesystem::exists(secondEntry->metadataPath));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceStartsCurrentGenerationWhenTwoOldGenerationsDrain)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "First.bmp", BmpRgb(1024u, 1024u));
    WriteBinaryFile(root / "Assets" / "Textures" / "Second.bmp", BmpRgb(1024u, 1024u));
    WriteBinaryFile(root / "Assets" / "Textures" / "Third.png", TinyPng());

    auto first = MakeThumbnailRequest(root, {});
    first.sourceAssetPath = "Assets/Textures/First.bmp";
    first.kind = AssetThumbnailKind::Texture;
    first.freshnessInputs = {{"source", "first:v1"}};

    auto second = first;
    second.sourceAssetPath = "Assets/Textures/Second.bmp";
    second.freshnessInputs = {{"source", "second:v1"}};

    auto third = first;
    third.sourceAssetPath = "Assets/Textures/Third.png";
    third.freshnessInputs = {{"source", "third:v1"}};

    {
        AssetThumbnailService service;
        ASSERT_EQ(service.GetThumbnail(first).status, AssetThumbnailServiceStatus::Pending);
        ASSERT_TRUE(service.StartNextThumbnailGeneration());

        service.SupersedeQueuedRequestsForGeneration("Assets/Textures#scope-2");
        ASSERT_EQ(service.GetThumbnail(second).status, AssetThumbnailServiceStatus::Pending);
        ASSERT_TRUE(service.StartNextThumbnailGeneration());

        service.SupersedeQueuedRequestsForGeneration("Assets/Textures#scope-3");
        ASSERT_EQ(service.GetThumbnail(third).status, AssetThumbnailServiceStatus::Pending);
        EXPECT_TRUE(service.StartNextThumbnailGeneration());

        std::optional<AssetThumbnailServiceResult> generated;
        for (int attempt = 0; attempt < 100 && !generated.has_value(); ++attempt)
        {
            generated = service.ConsumeCompletedThumbnail();
            if (!generated.has_value())
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        ASSERT_TRUE(generated.has_value());
        EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);
        const auto thirdEntry = ResolveAssetThumbnailCacheEntry(third);
        ASSERT_TRUE(thirdEntry.has_value());
        ASSERT_TRUE(generated->cacheEntry.has_value());
        EXPECT_EQ(generated->cacheEntry->cacheKey, thirdEntry->cacheKey);
        EXPECT_EQ(generated->imagePath, thirdEntry->imagePath);
    }

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceStartsCurrentGenerationWhenThreeOldGenerationsDrain)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "First.bmp", BmpRgb(1024u, 1024u));
    WriteBinaryFile(root / "Assets" / "Textures" / "Second.bmp", BmpRgb(1024u, 1024u));
    WriteBinaryFile(root / "Assets" / "Textures" / "Third.bmp", BmpRgb(1024u, 1024u));
    WriteBinaryFile(root / "Assets" / "Textures" / "Fourth.png", TinyPng());

    auto first = MakeThumbnailRequest(root, {});
    first.sourceAssetPath = "Assets/Textures/First.bmp";
    first.kind = AssetThumbnailKind::Texture;
    first.freshnessInputs = {{"source", "first:v1"}};

    auto second = first;
    second.sourceAssetPath = "Assets/Textures/Second.bmp";
    second.freshnessInputs = {{"source", "second:v1"}};

    auto third = first;
    third.sourceAssetPath = "Assets/Textures/Third.bmp";
    third.freshnessInputs = {{"source", "third:v1"}};

    auto fourth = first;
    fourth.sourceAssetPath = "Assets/Textures/Fourth.png";
    fourth.freshnessInputs = {{"source", "fourth:v1"}};

    {
        AssetThumbnailService service;
        ASSERT_EQ(service.GetThumbnail(first).status, AssetThumbnailServiceStatus::Pending);
        ASSERT_TRUE(service.StartNextThumbnailGeneration());

        service.SupersedeQueuedRequestsForGeneration("Assets/Textures#scope-2");
        ASSERT_EQ(service.GetThumbnail(second).status, AssetThumbnailServiceStatus::Pending);
        ASSERT_TRUE(service.StartNextThumbnailGeneration());

        service.SupersedeQueuedRequestsForGeneration("Assets/Textures#scope-3");
        ASSERT_EQ(service.GetThumbnail(third).status, AssetThumbnailServiceStatus::Pending);
        ASSERT_TRUE(service.StartNextThumbnailGeneration());

        service.SupersedeQueuedRequestsForGeneration("Assets/Textures#scope-4");
        ASSERT_EQ(service.GetThumbnail(fourth).status, AssetThumbnailServiceStatus::Pending);
        EXPECT_FALSE(service.StartNextThumbnailGeneration());

        bool startedCurrentGeneration = false;
        for (int attempt = 0; attempt < 100 && !startedCurrentGeneration; ++attempt)
        {
            (void)service.ConsumeCompletedThumbnail();
            startedCurrentGeneration = service.StartNextThumbnailGeneration();
            if (!startedCurrentGeneration)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(startedCurrentGeneration);

        std::optional<AssetThumbnailServiceResult> generated;
        for (int attempt = 0; attempt < 100 && !generated.has_value(); ++attempt)
        {
            generated = service.ConsumeCompletedThumbnail();
            if (!generated.has_value())
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        ASSERT_TRUE(generated.has_value());
        EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);
        const auto fourthEntry = ResolveAssetThumbnailCacheEntry(fourth);
        ASSERT_TRUE(fourthEntry.has_value());
        ASSERT_TRUE(generated->cacheEntry.has_value());
        EXPECT_EQ(generated->cacheEntry->cacheKey, fourthEntry->cacheKey);
        EXPECT_EQ(generated->imagePath, fourthEntry->imagePath);
    }

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceAdoptsMatchingInFlightRequestAfterGenerationChange)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.bmp", BmpRgb(1024u, 1024u));

    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/Hero.bmp";
    request.kind = AssetThumbnailKind::Texture;
    request.freshnessInputs = {{"source", "bmp-1024:v1"}};
    const auto expectedEntry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(expectedEntry.has_value());

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_TRUE(service.StartNextThumbnailGeneration());
    EXPECT_TRUE(service.HasInFlightRequest());

    service.SupersedeQueuedRequestsForGeneration("Assets/Textures#96");

    const auto adopted = service.GetThumbnail(request);
    EXPECT_EQ(adopted.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);
    EXPECT_FALSE(service.StartNextThumbnailGeneration());

    std::optional<AssetThumbnailServiceResult> generated;
    for (int attempt = 0; attempt < 100 && !generated.has_value(); ++attempt)
    {
        generated = service.ConsumeCompletedThumbnail();
        if (!generated.has_value())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);
    ASSERT_TRUE(generated->cacheEntry.has_value());
    EXPECT_EQ(generated->cacheEntry->cacheKey, expectedEntry->cacheKey);
    EXPECT_EQ(generated->imagePath, expectedEntry->imagePath);
    EXPECT_FALSE(service.HasInFlightRequest());

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceKeepsGpuOnlyPreviewThumbnailsPendingWithoutRenderer)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.gltf", std::vector<uint8_t>{'g', 'l', 't', 'f'});
    WriteBinaryFile(root / "Assets" / "Prefabs" / "Lamp.prefab", std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});

    WriteNativeArtifactTextFile(
        root / LibraryArtifactPath("47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae"),
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        1u,
        "shaderLabMaterialVersion=1\n"
        "shader=?\n"
        "property _BaseColor Color 0.2 0.6 0.9 1.0\n"
        "property _Metallic Float 0.1\n"
        "property _Roughness Float 0.45\n");
    WriteBinaryFile(
        root / LibraryArtifactPath("eab993d3e507e9b6427246cc0f936120bbb30a9cbc60e1c782e8eda361f75f3b"),
        NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));
    WriteNativeArtifactTextFile(
        root / LibraryArtifactPath("670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772"),
        NLS::Core::Assets::ArtifactType::Prefab,
        "prefab",
        1u,
        MinimalPrefabPayload());

    auto materialRequest = MakeThumbnailRequest(root, "material:Body");
    materialRequest.sourceAssetPath = "Assets/Models/Hero.gltf";
    materialRequest.artifactPath =
        "Library/Artifacts/47/47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae";
    materialRequest.kind = AssetThumbnailKind::MaterialSphere;
    materialRequest.requestedSize = 48u;
    materialRequest.freshnessInputs = {{"artifact", "material:v1"}};
    ExpectGpuPreviewDefersWithoutRenderer(materialRequest);

    auto sourceMaterialRequest = MakeThumbnailRequest(root, "material:New");
    sourceMaterialRequest.sourceAssetPath = "Assets/Materials/New.mat";
    sourceMaterialRequest.artifactPath = "Assets/Materials/New.mat";
    sourceMaterialRequest.kind = AssetThumbnailKind::MaterialSphere;
    sourceMaterialRequest.requestedSize = 48u;
    sourceMaterialRequest.freshnessInputs = {{"source", "material-source:v1"}};
    ExpectGpuPreviewRejectsInvalidArtifactPath(
        sourceMaterialRequest,
        "thumbnail-material-artifact-path-invalid");

    auto sourceMaterialRequestWithoutArtifactPath = sourceMaterialRequest;
    sourceMaterialRequestWithoutArtifactPath.artifactPath.clear();
    sourceMaterialRequestWithoutArtifactPath.freshnessInputs = {{"source", "material-source-no-artifact:v1"}};
    ExpectGpuPreviewRejectsInvalidArtifactPath(
        sourceMaterialRequestWithoutArtifactPath,
        "thumbnail-material-artifact-missing");

    auto modelRequest = MakeThumbnailRequest(root, "mesh:Body");
    modelRequest.sourceAssetPath = "Assets/Models/Hero.gltf";
    modelRequest.artifactPath =
        "Library/Artifacts/ea/eab993d3e507e9b6427246cc0f936120bbb30a9cbc60e1c782e8eda361f75f3b";
    modelRequest.kind = AssetThumbnailKind::ModelPreview;
    modelRequest.requestedSize = 48u;
    modelRequest.freshnessInputs = {{"artifact", "mesh:v1"}};
    ExpectGpuPreviewDefersWithoutRenderer(root, modelRequest);

    auto prefabRequest = MakeThumbnailRequest(root, "prefab:Hero");
    prefabRequest.sourceAssetPath = "Assets/Prefabs/Lamp.prefab";
    prefabRequest.artifactPath =
        "Library/Artifacts/67/670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772";
    prefabRequest.kind = AssetThumbnailKind::PrefabPreview;
    prefabRequest.requestedSize = 48u;
    prefabRequest.freshnessInputs = {{"artifact", "prefab:v1"}};
    ExpectGpuPreviewDefersWithoutRenderer(root, prefabRequest);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, StoredThumbnailMetadataInvalidatesWhenArtifactDatabaseDataFileChanges)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.gltf", std::vector<uint8_t>{'g', 'l', 't', 'f'});
    const auto artifactDatabaseDataPath = root / "Library" / "ArtifactDB" / "data.mdb";
    WriteBinaryFile(artifactDatabaseDataPath, std::vector<uint8_t>{'v', '1'});

    AssetBrowserItem material;
    material.kind = AssetBrowserItemKind::GeneratedSubAsset;
    material.type = AssetBrowserItemType::Material;
    material.assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("a6060606-0606-4606-8606-060606060606"));
    material.sourceAssetPath = "Assets/Models/Hero.gltf";
    material.subAssetKey = "material:Body";
    material.artifactType = ArtifactType::Material;

    const auto request = BuildAssetThumbnailRequestForItem(root, material, 96u);
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(std::any_of(
        request->freshnessInputs.begin(),
        request->freshnessInputs.end(),
        [](const AssetThumbnailFreshnessInput& input)
        {
            return input.name == "artifact-db";
        }));

    const auto entry = ResolveAssetThumbnailCacheEntry(*request);
    ASSERT_TRUE(entry.has_value());
    ASSERT_TRUE(WriteAssetThumbnailCacheFile(*request, entry->imagePath, TinyPng()));
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(*request, AssetThumbnailCacheStatus::Fresh, {}));
    ASSERT_EQ(EvaluateAssetThumbnailCache(*request).status, AssetThumbnailCacheStatus::Fresh);

    std::filesystem::resize_file(artifactDatabaseDataPath, 3u);

    const auto evaluated = EvaluateAssetThumbnailCache(*request);
    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Stale);
    EXPECT_EQ(evaluated.diagnostic, "thumbnail-cache-freshness-stale");

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, UnityStylePreviewRequestQueuesAndReportsLoadingWithoutRendering)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Prefabs" / "Lamp.prefab", std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});

    auto request = MakeThumbnailRequest(root, "prefab:Hero");
    request.sourceAssetPath = "Assets/Prefabs/Lamp.prefab";
    request.artifactPath =
        "Library/Artifacts/67/670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772";
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 48u;
    request.freshnessInputs = {{"artifact", "prefab:v1"}};

    CountingThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;

    const auto miniThumbnail = service.GetMiniThumbnail(request);
    EXPECT_EQ(miniThumbnail.status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_EQ(miniThumbnail.fallbackIcon, "editor.icon.asset.prefab");
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Missing);
    EXPECT_FALSE(service.IsLoadingAssetPreview(request));

    const auto requested = service.RequestAssetPreview(request);
    EXPECT_EQ(requested.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(requested.fallbackIcon, "editor.icon.asset.prefab");
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);
    EXPECT_TRUE(service.IsLoadingAssetPreview(request));
    EXPECT_EQ(renderer.supportsCount, 0u);
    EXPECT_EQ(renderer.renderCount, 0u);

    const auto queried = service.GetAssetPreview(request);
    EXPECT_EQ(queried.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_TRUE(service.IsLoadingAssetPreview(request));
    EXPECT_EQ(renderer.supportsCount, 0u);
    EXPECT_EQ(renderer.renderCount, 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, UnityStylePreviewRequestCoalescesDuplicatePrefabRequests)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Prefabs" / "Lamp.prefab", std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});

    auto request = MakeThumbnailRequest(root, "prefab:Hero");
    request.sourceAssetPath = "Assets/Prefabs/Lamp.prefab";
    request.artifactPath =
        "Library/Artifacts/67/670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772";
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 48u;
    request.freshnessInputs = {{"artifact", "prefab:v1"}};

    AssetThumbnailService service;

    EXPECT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_TRUE(service.IsLoadingAssetPreview(request));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceDoesNotProcessGpuPreviewWhenReadbackBudgetIsExhausted)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Prefabs" / "Lamp.prefab", std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});

    auto request = MakeThumbnailRequest(root, "prefab:Hero");
    request.sourceAssetPath = "Assets/Prefabs/Lamp.prefab";
    request.artifactPath =
        "Library/Artifacts/67/670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772";
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 48u;
    request.freshnessInputs = {{"artifact", "prefab:v1"}};

    AssetThumbnailService service;
    ThumbnailGenerationBudget budget;
    budget.readbackCountBudget = 0u;
    service.SetThumbnailGenerationBudget(budget);

    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);
    ASSERT_EQ(service.GetQueuedRequestCount(), 1u);

    const auto generated = service.GenerateNextThumbnail();
    EXPECT_FALSE(generated.has_value());
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Missing);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, CpuThumbnailProgressesWhenGpuReadbackBudgetIsExhausted)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Prefabs" / "Lamp.prefab", std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    auto prefab = MakeThumbnailRequest(root, "prefab:Hero");
    prefab.sourceAssetPath = "Assets/Prefabs/Lamp.prefab";
    prefab.artifactPath =
        "Library/Artifacts/67/670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772";
    prefab.kind = AssetThumbnailKind::PrefabPreview;
    prefab.requestedSize = 48u;
    prefab.freshnessInputs = {{"artifact", "prefab:v1"}};

    auto texture = MakeThumbnailRequest(root, "texture:Hero");
    texture.sourceAssetPath = "Assets/Textures/Hero.png";
    texture.kind = AssetThumbnailKind::Texture;
    texture.requestedSize = 48u;
    texture.freshnessInputs = {{"source", "tiny-png:v1"}};

    const auto textureEntry = ResolveAssetThumbnailCacheEntry(texture);
    ASSERT_TRUE(textureEntry.has_value());

    AssetThumbnailService service;
    ThumbnailGenerationBudget budget;
    budget.readbackCountBudget = 0u;
    service.SetThumbnailGenerationBudget(budget);

    ASSERT_EQ(service.GetThumbnail(prefab).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetThumbnail(texture).status, AssetThumbnailServiceStatus::Pending);

    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    ASSERT_TRUE(generated->cacheEntry.has_value());
    EXPECT_EQ(generated->cacheEntry->cacheKey, textureEntry->cacheKey);
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(service.GetThumbnailState(prefab), ThumbnailState::Queued);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_EQ(EvaluateAssetThumbnailCache(prefab).status, AssetThumbnailCacheStatus::Missing);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceBoundsQueuedBackgroundRequestsAndPreservesVisibleRequests)
{
    using namespace NLS::Editor::Assets;

    constexpr size_t expectedQueueCap = 512u;
    const auto root = MakeAssetThumbnailCacheRoot();

    AssetThumbnailService service;
    for (size_t index = 0u; index < expectedQueueCap + 64u; ++index)
    {
        auto request = MakeThumbnailRequest(root, "prefab:Background" + std::to_string(index));
        request.sourceAssetPath = "Assets/Prefabs/Background" + std::to_string(index) + ".prefab";
        request.kind = AssetThumbnailKind::PrefabPreview;
        request.priority = ThumbnailRequestPriority::Background;
        request.freshnessInputs = {{"artifact", "background:" + std::to_string(index)}};

        EXPECT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
        EXPECT_LE(service.GetQueuedRequestCount(), expectedQueueCap);
    }

    EXPECT_EQ(service.GetQueuedRequestCount(), expectedQueueCap);

    auto visible = MakeThumbnailRequest(root, "prefab:Visible");
    visible.sourceAssetPath = "Assets/Prefabs/Visible.prefab";
    visible.kind = AssetThumbnailKind::PrefabPreview;
    visible.priority = ThumbnailRequestPriority::Visible;
    visible.freshnessInputs = {{"artifact", "visible:v1"}};

    EXPECT_EQ(service.GetThumbnail(visible).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetQueuedRequestCount(), expectedQueueCap);
    EXPECT_EQ(service.GetThumbnailState(visible), ThumbnailState::Queued);
    EXPECT_TRUE(service.IsLoadingAssetPreview(visible));
    std::filesystem::remove_all(root);
}
