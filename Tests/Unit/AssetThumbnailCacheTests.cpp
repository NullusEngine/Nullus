#include <gtest/gtest.h>

#include "Assets/AssetThumbnailCache.h"
#include "Assets/AssetThumbnailPreviewCamera.h"
#include "Assets/AssetThumbnailService.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/AssetId.h"
#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/EditorThumbnailPreviewRenderer.h"
#include "Assets/NativeArtifactContainer.h"
#include "Assets/PreviewRenderableSnapshot.h"
#include "Assets/ResidentPrefabPreviewRegistry.h"
#include "Assets/ThumbnailPreviewProxyPool.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Core/ServiceLocator.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Jobs/JobSystem.h"
#include "Profiling/PerformanceStageStats.h"
#include "Serialize/ObjectGraphWriter.h"
#include "Guid.h"
#include "Image.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/RHITypes.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/Resources/Texture2D.h"
#include <Json/json.hpp>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define NLS_UNREGISTERED_TEST(suite, name) static void suite##_##name##_Unregistered()
#if defined(NLS_REGISTER_LONG_RUNNING_ASSET_THUMBNAIL_TESTS)
#undef TEST
#define TEST(suite, name) NLS_UNREGISTERED_TEST(suite, name)
#define NLS_LONG_RUNNING_TEST(performanceSuite, name) GTEST_TEST(performanceSuite, name)
#else
#define NLS_LONG_RUNNING_TEST(performanceSuite, name) NLS_UNREGISTERED_TEST(performanceSuite, name)
#endif

namespace
{
template<typename T>
class ScopedThumbnailServiceOverride final
{
public:
    explicit ScopedThumbnailServiceOverride(T& service)
    {
        m_hadPrevious = NLS::Core::ServiceLocator::Contains<T>();
        if (m_hadPrevious)
            m_previous = &NLS::Core::ServiceLocator::Get<T>();
        NLS::Core::ServiceLocator::Provide<T>(service);
    }

    ~ScopedThumbnailServiceOverride()
    {
        if (m_hadPrevious && m_previous != nullptr)
            NLS::Core::ServiceLocator::Provide<T>(*m_previous);
        else
            NLS::Core::ServiceLocator::Remove<T>();
    }

    ScopedThumbnailServiceOverride(const ScopedThumbnailServiceOverride&) = delete;
    ScopedThumbnailServiceOverride& operator=(const ScopedThumbnailServiceOverride&) = delete;

private:
    bool m_hadPrevious = false;
    T* m_previous = nullptr;
};

class ThumbnailReadyTexture final : public NLS::Render::RHI::RHITexture
{
public:
    ThumbnailReadyTexture()
    {
        m_desc.extent = {1u, 1u, 1u};
        m_desc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
        m_desc.format = NLS::Render::RHI::TextureFormat::RGBA8;
        m_desc.usage = NLS::Render::RHI::TextureUsageFlags::Sampled;
    }

    std::string_view GetDebugName() const override { return "ThumbnailReadyTexture"; }
    const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
    NLS::Render::RHI::ResourceState GetState() const override
    {
        return static_cast<NLS::Render::RHI::ResourceState>(1u << 5);
    }

private:
    NLS::Render::RHI::RHITextureDesc m_desc;
};

std::filesystem::path MakeAssetThumbnailCacheRoot()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_asset_thumbnail_cache_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root / "Assets");
    return root;
}

std::filesystem::path RepoPath(std::string_view relativePath)
{
    return std::filesystem::path(NLS_ROOT_DIR) / std::filesystem::path(relativePath);
}

std::string ReadSourceText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    EXPECT_TRUE(input.is_open()) << "Failed to open source file: " << path.string();
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

std::string ExtractFunctionBody(const std::string& source, std::string_view functionNeedle)
{
    const auto begin = source.find(functionNeedle);
    EXPECT_NE(begin, std::string::npos) << "Missing function: " << functionNeedle;
    if (begin == std::string::npos)
        return {};

    const auto bodyBegin = source.find('{', begin);
    EXPECT_NE(bodyBegin, std::string::npos) << "Missing function body: " << functionNeedle;
    if (bodyBegin == std::string::npos)
        return {};

    size_t depth = 0u;
    for (size_t offset = bodyBegin; offset < source.size(); ++offset)
    {
        if (source[offset] == '{')
            ++depth;
        else if (source[offset] == '}')
        {
            --depth;
            if (depth == 0u)
                return source.substr(bodyBegin, offset - bodyBegin + 1u);
        }
    }

    ADD_FAILURE() << "Unterminated function body: " << functionNeedle;
    return {};
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
    const auto oldRelativePath = NormalizePortablePath(path.lexically_relative(*projectRoot));
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

    const auto relative = path.lexically_normal().lexically_relative(*projectRoot).lexically_normal();
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

NLS::Render::Assets::MeshArtifactData ThinTriangleStripMeshArtifact()
{
    NLS::Render::Assets::MeshArtifactData mesh;
    constexpr uint32_t segmentCount = 72u;
    mesh.vertices.reserve((segmentCount + 1u) * 2u);
    mesh.indices.reserve(segmentCount * 6u);
    for (uint32_t index = 0u; index <= segmentCount; ++index)
    {
        const float t = static_cast<float>(index) / static_cast<float>(segmentCount);
        const float x = -1.0f + t * 2.0f;
        const float y = std::sin(t * 6.28318530718f) * 0.18f;
        const float halfWidth = 0.006f;
        NLS::Render::Geometry::Vertex lower {};
        lower.position[0] = x;
        lower.position[1] = y - halfWidth;
        lower.position[2] = 0.0f;
        lower.normals[2] = 1.0f;
        NLS::Render::Geometry::Vertex upper {};
        upper.position[0] = x;
        upper.position[1] = y + halfWidth;
        upper.position[2] = 0.0f;
        upper.normals[2] = 1.0f;
        mesh.vertices.push_back(lower);
        mesh.vertices.push_back(upper);
    }
    for (uint32_t index = 0u; index < segmentCount; ++index)
    {
        const uint32_t base = index * 2u;
        mesh.indices.insert(mesh.indices.end(), {
            base + 0u, base + 1u, base + 2u,
            base + 1u, base + 3u, base + 2u
        });
    }
    mesh.hasBoundingSphere = true;
    mesh.boundingSphere.position = NLS::Maths::Vector3(0.0f, 0.0f, 0.0f);
    mesh.boundingSphere.radius = 1.1f;
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

size_t CountOpaquePixelsMatchingColor(
    const NLS::Image& image,
    const uint8_t red,
    const uint8_t green,
    const uint8_t blue)
{
    if (image.GetData() == nullptr || image.GetWidth() <= 0 || image.GetHeight() <= 0)
        return 0u;

    const auto channels = image.GetChannels();
    if (channels < 4)
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
            if (pixels[pixelIndex + 3u] != 0u &&
                pixels[pixelIndex + 0u] == red &&
                pixels[pixelIndex + 1u] == green &&
                pixels[pixelIndex + 2u] == blue)
            {
                ++count;
            }
        }
    }
    return count;
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

class EmptyDiagnosticThumbnailPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest&) const override
    {
        return true;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.diagnostic = "thumbnail-gpu-preview-resources-pending";
        return result;
    }
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

template <typename Renderer>
std::optional<NLS::Editor::Assets::AssetThumbnailServiceResult> PumpUntilDeferredPreviewResolves(
    NLS::Editor::Assets::AssetThumbnailService& service,
    Renderer& renderer,
    const bool includeHeavyGpuPreviews = true)
{
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        auto generated = service.GenerateNextThumbnail(renderer, includeHeavyGpuPreviews);
        if (generated.has_value() &&
            generated->diagnostic != "thumbnail-preview-request-resolution-pending")
        {
            return generated;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::nullopt;
}

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

class CompletingAsyncThumbnailPreviewRenderer final :
    public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest&) const override
    {
        return true;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        return {};
    }

    NLS::Editor::Assets::EditorThumbnailPreviewSubmitResult SubmitPreparedPreview(
        const NLS::Editor::Assets::AssetThumbnailRequest& request) override
    {
        NLS::Editor::Assets::EditorThumbnailPreviewResult preview;
        preview.width = 2u;
        preview.height = 2u;
        preview.diagnostic = "thumbnail-gpu-preview-readback-pending";
        NLS::Editor::Assets::EditorThumbnailPreviewReadbackTicket ticket {
            NLS::Editor::Assets::BuildThumbnailPreviewReadbackRequestKey(request),
            request.requestRevision
        };
        pendingTickets.push_back(ticket);
        return {std::move(preview), std::move(ticket)};
    }

    std::vector<NLS::Editor::Assets::EditorThumbnailPreviewCompletedReadback>
    PollCompletedReadbacks(const size_t maxCount) override
    {
        if (!completeNextReadback || maxCount == 0u || pendingTickets.empty())
            return {};

        completeNextReadback = false;
        auto ticket = std::move(pendingTickets.front());
        pendingTickets.erase(pendingTickets.begin());
        NLS::Editor::Assets::EditorThumbnailPreviewResult preview;
        preview.width = 2u;
        preview.height = 2u;
        preview.rgbaPixels = {
            255u, 0u, 0u, 255u,
            0u, 255u, 0u, 255u,
            0u, 0u, 255u, 255u,
            255u, 255u, 255u, 255u
        };
        return {{std::move(ticket), std::move(preview)}};
    }

    bool SupportsAsynchronousReadbackPolling() const override
    {
        return true;
    }

    bool completeNextReadback = false;
    std::vector<NLS::Editor::Assets::EditorThumbnailPreviewReadbackTicket> pendingTickets;
};

class NeverReadyThumbnailPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest&) const override
    {
        return true;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++renderCount;
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.width = 2u;
        result.height = 2u;
        result.diagnostic = "thumbnail-gpu-preview-readback-pending";
        return result;
    }

    bool OrphanReadback(
        const NLS::Editor::Assets::EditorThumbnailPreviewReadbackTicket&) override
    {
        ++orphanCount;
        return true;
    }

    size_t renderCount = 0u;
    size_t orphanCount = 0u;
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

class BlackFrameThumbnailPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest&) const override
    {
        return true;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++renderCount;
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.width = 2u;
        result.height = 2u;
        result.rgbaPixels = {
            0u, 0u, 0u, 255u,
            0u, 0u, 0u, 255u,
            0u, 0u, 0u, 255u,
            0u, 0u, 0u, 255u
        };
        return result;
    }

    size_t renderCount = 0u;
};

class SubmittedBlackFrameThumbnailPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest&) const override
    {
        return true;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.width = 2u;
        result.height = 2u;
        result.submittedSceneDrawCount = 1u;
        result.rgbaPixels = {
            0u, 0u, 0u, 255u,
            0u, 0u, 0u, 255u,
            0u, 0u, 0u, 255u,
            0u, 0u, 0u, 255u
        };
        return result;
    }
};

class PendingThenBlackFrameThumbnailPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest&) const override
    {
        return true;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++renderCount;
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.width = 2u;
        result.height = 2u;
        if (renderCount == 1u)
        {
            result.diagnostic = "thumbnail-gpu-preview-readback-pending";
            return result;
        }

        result.rgbaPixels = {
            0u, 0u, 0u, 255u,
            0u, 0u, 0u, 255u,
            0u, 0u, 0u, 255u,
            0u, 0u, 0u, 255u
        };
        return result;
    }

    size_t renderCount = 0u;
};

class PendingThenTransparentLitFrameThumbnailPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest&) const override
    {
        return true;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++renderCount;
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.width = 2u;
        result.height = 2u;
        if (renderCount == 1u)
        {
            result.diagnostic = "thumbnail-gpu-preview-readback-pending";
            return result;
        }

        result.completedPendingReadback = true;
        result.rawVisibleDrawCount = 4u;
        result.submittedSceneDrawCount = 3u;
        result.rgbaPixels = {
            255u, 255u, 255u, 0u,
            64u, 32u, 16u, 0u,
            0u, 0u, 0u, 0u,
            0u, 0u, 0u, 0u
        };
        return result;
    }

    size_t renderCount = 0u;
};

class ResourcesPendingThumbnailPreviewRenderer final : public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest& request) const override
    {
        return request.kind == NLS::Editor::Assets::AssetThumbnailKind::PrefabPreview;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResourcePumpResult PumpResources(
        const NLS::Editor::Assets::AssetThumbnailRequest& request) override
    {
        ++pumpCount;
        pumpKeys.push_back(request.subAssetKey);
        NLS::Editor::Assets::EditorThumbnailPreviewResourcePumpResult result;
        result.supported = true;
        result.resourcesPending = true;
        result.diagnostic = diagnostic;
        if (reportProgress)
            result.resourceProgressToken = pumpCount;
        result.resourceWorkActive = reportActiveResourceWork;
        return result;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++renderCount;
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.diagnostic = diagnostic;
        return result;
    }

    std::string diagnostic = "thumbnail-gpu-preview-resources-pending";
    bool reportProgress = false;
    bool reportActiveResourceWork = false;
    size_t pumpCount = 0u;
    size_t renderCount = 0u;
    std::vector<std::string> pumpKeys;
};

class TerminalResourceFailureThumbnailPreviewRenderer final :
    public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest& request) const override
    {
        return request.kind == NLS::Editor::Assets::AssetThumbnailKind::PrefabPreview;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResourcePumpResult PumpResources(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++pumpCount;
        NLS::Editor::Assets::EditorThumbnailPreviewResourcePumpResult result;
        result.supported = true;
        result.diagnostic = "thumbnail-gpu-preview-mesh-load-failed|meshFailed=1";
        return result;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++renderCount;
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.width = 1u;
        result.height = 1u;
        result.rgbaPixels = {255u, 255u, 255u, 255u};
        return result;
    }

    size_t pumpCount = 0u;
    size_t renderCount = 0u;
};

class MixedPendingThenReadyThumbnailPreviewRenderer final :
    public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest& request) const override
    {
        return request.kind == NLS::Editor::Assets::AssetThumbnailKind::PrefabPreview;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResourcePumpResult PumpResources(
        const NLS::Editor::Assets::AssetThumbnailRequest& request) override
    {
        ++pumpCount;
        pumpKeys.push_back(request.subAssetKey);
        NLS::Editor::Assets::EditorThumbnailPreviewResourcePumpResult result;
        result.supported = true;
        result.resourcesPending = pumpCount == 1u;
        if (result.resourcesPending)
        {
            result.diagnostic =
                "thumbnail-gpu-preview-resources-pending|mesh=1|material=0|texture=0|truncated=0";
        }
        return result;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++renderCount;
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.width = 2u;
        result.height = 2u;
        result.rawVisibleDrawCount = 2u;
        result.submittedSceneDrawCount = 2u;
        result.rgbaPixels = {
            255u, 0u, 0u, 255u,
            0u, 255u, 0u, 255u,
            0u, 0u, 255u, 255u,
            255u, 255u, 255u, 255u
        };
        return result;
    }

    void ReleaseCompletedPreviewResources(
        const NLS::Editor::Assets::AssetThumbnailRequest& request) override
    {
        releasedKeys.push_back(request.subAssetKey);
    }

    size_t pumpCount = 0u;
    size_t renderCount = 0u;
    std::vector<std::string> pumpKeys;
    std::vector<std::string> releasedKeys;
};

class RenderDetailedResourcesPendingThumbnailPreviewRenderer final :
    public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest& request) const override
    {
        return request.kind == NLS::Editor::Assets::AssetThumbnailKind::PrefabPreview;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResourcePumpResult PumpResources(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++pumpCount;
        NLS::Editor::Assets::EditorThumbnailPreviewResourcePumpResult result;
        result.supported = true;
        return result;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++renderCount;
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.diagnostic = "thumbnail-gpu-preview-resources-pending|mesh=2|material=1|texture=4";
        return result;
    }

    size_t pumpCount = 0u;
    size_t renderCount = 0u;
};

class PreparedSubmissionThumbnailPreviewRenderer final :
    public NLS::Editor::Assets::IEditorThumbnailPreviewRenderer
{
public:
    bool Supports(const NLS::Editor::Assets::AssetThumbnailRequest& request) const override
    {
        return request.kind == NLS::Editor::Assets::AssetThumbnailKind::PrefabPreview;
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResourcePumpResult PumpResources(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++pumpCount;
        return {true, false, {}};
    }

    NLS::Editor::Assets::EditorThumbnailPreviewResult Render(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++renderCount;
        return MakeReadyPreview();
    }

    NLS::Editor::Assets::EditorThumbnailPreviewSubmitResult SubmitPreview(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++ordinarySubmitCount;
        return {MakeReadyPreview(), std::nullopt};
    }

    NLS::Editor::Assets::EditorThumbnailPreviewSubmitResult SubmitPreparedPreview(
        const NLS::Editor::Assets::AssetThumbnailRequest&) override
    {
        ++preparedSubmitCount;
        return {MakeReadyPreview(), std::nullopt};
    }

    size_t pumpCount = 0u;
    size_t renderCount = 0u;
    size_t ordinarySubmitCount = 0u;
    size_t preparedSubmitCount = 0u;

private:
    static NLS::Editor::Assets::EditorThumbnailPreviewResult MakeReadyPreview()
    {
        NLS::Editor::Assets::EditorThumbnailPreviewResult result;
        result.width = 2u;
        result.height = 2u;
        result.expectedSceneDrawCount = 1u;
        result.rawVisibleDrawCount = 1u;
        result.submittedSceneDrawCount = 1u;
        result.rgbaPixels = {
            255u, 0u, 0u, 255u,
            0u, 255u, 0u, 255u,
            0u, 0u, 255u, 255u,
            255u, 255u, 255u, 255u
        };
        return result;
    }
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

#if defined(NLS_ENABLE_TEST_HOOKS)
TEST(AssetThumbnailCacheTests, ServiceEvaluatesThumbnailCacheOncePerGeneration)
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
    ResetAssetThumbnailCacheEvaluationCountForTesting();

    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    ASSERT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(GetAssetThumbnailCacheEvaluationCountForTesting(), 1u)
        << "A single thumbnail generation should reuse its initial cache evaluation.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceChecksFreshnessOnlyAroundThumbnailCacheWrite)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto sourcePath = root / "Assets" / "Textures" / "Hero.png";
    WriteBinaryFile(sourcePath, TinyPng());

    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/Hero.png";
    request.kind = AssetThumbnailKind::Texture;
    request.freshnessInputs = {{"source-file", FileStampForTest(sourcePath)}};

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    ResetAssetThumbnailFreshnessInputCheckCountForTesting();

    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    ASSERT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(GetAssetThumbnailFreshnessInputCheckCountForTesting(), 2u)
        << "Freshness should be checked before generation and after the cache image write.";

    std::filesystem::remove_all(root);
}
#endif

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

TEST(AssetThumbnailCacheTests, DiscardsUnreferencedCandidateAfterCancelledWrite)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, "prefab:Hero", "source:cancelled");
    request.requestRevision = 7u;
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());
    ASSERT_TRUE(WriteAssetThumbnailCacheFile(request, entry->imagePath, TinyPng()));
    ASSERT_TRUE(std::filesystem::exists(entry->imagePath));

    EXPECT_TRUE(DiscardUnreferencedAssetThumbnailCacheCandidate(request, *entry));
    EXPECT_FALSE(std::filesystem::exists(entry->imagePath));
    EXPECT_FALSE(std::filesystem::exists(entry->metadataPath));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, KeepsCandidateReferencedByPresentationGenerations)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, "prefab:Hero", "source:committed");
    request.requestRevision = 8u;
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());
    ASSERT_TRUE(WriteAssetThumbnailCacheFile(request, entry->imagePath, TinyPng()));
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Fresh, {}));

    EXPECT_FALSE(DiscardUnreferencedAssetThumbnailCacheCandidate(request, *entry));
    EXPECT_TRUE(std::filesystem::exists(entry->imagePath));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, RejectsEqualRevisionForDifferentCanonicalCandidate)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto first = MakeThumbnailRequest(root, "prefab:Hero", "source:revision-a");
    first.requestRevision = 9u;
    const auto firstEntry = ResolveAssetThumbnailCacheEntry(first);
    ASSERT_TRUE(firstEntry.has_value());
    ASSERT_TRUE(WriteAssetThumbnailCacheFile(first, firstEntry->imagePath, TinyPng()));
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(first, AssetThumbnailCacheStatus::Fresh, {}));

    auto conflicting = first;
    conflicting.freshnessInputs.front().stamp = "source:revision-b";
    const auto conflictingEntry = ResolveAssetThumbnailCacheEntry(conflicting);
    ASSERT_TRUE(conflictingEntry.has_value());
    ASSERT_TRUE(WriteAssetThumbnailCacheFile(
        conflicting,
        conflictingEntry->imagePath,
        TinyPng()));

    EXPECT_FALSE(WriteAssetThumbnailCacheMetadata(
        conflicting,
        AssetThumbnailCacheStatus::Fresh,
        {}));
    const auto index = ReadAssetThumbnailPresentationIndex(conflicting);
    ASSERT_TRUE(index.has_value());
    ASSERT_TRUE(index->current.has_value());
    EXPECT_EQ(index->current->cacheKey, firstEntry->cacheKey);

    EXPECT_TRUE(DiscardUnreferencedAssetThumbnailCacheCandidate(
        conflicting,
        *conflictingEntry));
    EXPECT_FALSE(std::filesystem::exists(conflictingEntry->imagePath));
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

#if defined(NLS_ENABLE_TEST_HOOKS)
TEST(AssetThumbnailCacheTests, ResolveCacheEntryAvoidsRepeatedCanonicalContainmentChecks)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    ResetAssetThumbnailCacheCanonicalPathAttemptCountForTesting();

    const auto entry = ResolveAssetThumbnailCacheEntry(MakeThumbnailRequest(root, "prefab:Hero"));

    ASSERT_TRUE(entry.has_value());
    EXPECT_LE(GetAssetThumbnailCacheCanonicalPathAttemptCountForTesting(), 5u);

    std::filesystem::remove_all(root);
}
#endif
TEST(AssetThumbnailCacheTests, MetadataLoadTelemetryUsesMetadataFileSize)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto request = MakeThumbnailRequest(root, "prefab:Hero");
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(
        request,
        *entry,
        AssetThumbnailCacheStatus::Failed,
        "intentional-test-failure"));
    const auto failureMetadataPath =
        std::filesystem::path(entry->metadataPath.generic_string() + ".failure");
    const auto metadataFileSize = std::filesystem::file_size(failureMetadataPath);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto evaluation = EvaluateAssetThumbnailCache(request);

    EXPECT_EQ(evaluation.status, AssetThumbnailCacheStatus::Failed);
    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    const auto record = std::find_if(
        telemetry.begin(),
        telemetry.end(),
        [](const NLS::Core::Assets::ArtifactLoadTelemetryRecord& candidate)
        {
            return candidate.stage ==
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateMetadataLoad;
        });
    ASSERT_NE(record, telemetry.end());
    EXPECT_EQ(record->byteCount, metadataFileSize);

    std::filesystem::remove_all(root);
}

#if defined(NLS_ENABLE_TEST_HOOKS)
TEST(AssetThumbnailCacheTests, EvaluateCacheReusesUnchangedMetadataLoad)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto request = MakeThumbnailRequest(root, "prefab:Hero");
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(
        request,
        *entry,
        AssetThumbnailCacheStatus::Failed,
        "intentional-test-failure"));

    ResetAssetThumbnailCacheMetadataFileLoadCountForTesting();
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Failed);
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Failed);

    EXPECT_EQ(GetAssetThumbnailCacheMetadataFileLoadCountForTesting(), 1u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, FastEvaluateFreshImageSkipsMetadataJsonLoad)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, "texture:Hero");
    request.sourceAssetPath = "Assets/Textures/Hero.png";
    request.kind = AssetThumbnailKind::Texture;
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());
    ASSERT_TRUE(WriteAssetThumbnailCacheFile(request, entry->imagePath, TinyPng()));
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Fresh, {}));

    ResetAssetThumbnailCacheMetadataFileLoadCountForTesting();
    EXPECT_EQ(
        EvaluateAssetThumbnailCache(request, AssetThumbnailCacheIntegrityMode::Fast).status,
        AssetThumbnailCacheStatus::Fresh);
    EXPECT_EQ(GetAssetThumbnailCacheMetadataFileLoadCountForTesting(), 0u)
        << "Fast UI thumbnail lookups should not parse per-thumbnail JSON; the cache key already carries freshness.";

    EXPECT_EQ(
        EvaluateAssetThumbnailCache(request, AssetThumbnailCacheIntegrityMode::Full).status,
        AssetThumbnailCacheStatus::Fresh);
    EXPECT_EQ(GetAssetThumbnailCacheMetadataFileLoadCountForTesting(), 1u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, FastEvaluateFreshImageDoesNotRequireMetadata)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, "texture:Hero");
    request.sourceAssetPath = "Assets/Textures/Hero.png";
    request.kind = AssetThumbnailKind::Texture;
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());
    ASSERT_TRUE(WriteAssetThumbnailCacheFile(request, entry->imagePath, TinyPng()));
    ASSERT_FALSE(std::filesystem::exists(entry->metadataPath));

    ResetAssetThumbnailCacheMetadataFileLoadCountForTesting();
    EXPECT_EQ(
        EvaluateAssetThumbnailCache(request, AssetThumbnailCacheIntegrityMode::Fast).status,
        AssetThumbnailCacheStatus::Fresh)
        << "UI cache lookups should be able to show a content-addressed image without blocking on metadata.";
    EXPECT_EQ(GetAssetThumbnailCacheMetadataFileLoadCountForTesting(), 0u);

    EXPECT_EQ(
        EvaluateAssetThumbnailCache(request, AssetThumbnailCacheIntegrityMode::Full).status,
        AssetThumbnailCacheStatus::Missing);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, FailedSidecarRetainsStaleImageForFastEvaluation)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto request = MakeThumbnailRequest(root, "texture:Missing");
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());
    ASSERT_TRUE(WriteAssetThumbnailCacheFile(request, entry->imagePath, TinyPng()));
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(
        request,
        AssetThumbnailCacheStatus::Fresh,
        {}));
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(
        request,
        AssetThumbnailCacheStatus::Failed,
        "thumbnail-texture-extension-unsupported"));
    const auto failureMetadataPath =
        std::filesystem::path(entry->metadataPath.generic_string() + ".failure");
    EXPECT_TRUE(std::filesystem::exists(entry->imagePath));
    EXPECT_TRUE(std::filesystem::exists(failureMetadataPath));

    ResetAssetThumbnailCacheMetadataFileLoadCountForTesting();
    const auto evaluated = EvaluateAssetThumbnailCache(
        request,
        AssetThumbnailCacheIntegrityMode::Fast);

    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Failed);
    EXPECT_EQ(evaluated.diagnostic, "thumbnail-texture-extension-unsupported");
    EXPECT_EQ(GetAssetThumbnailCacheMetadataFileLoadCountForTesting(), 1u)
        << "Fast cache evaluation can skip metadata only when the cached image exists.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, EvaluateCacheReusesStableContainmentValidation)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto request = MakeThumbnailRequest(root, "prefab:Hero");
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(
        request,
        *entry,
        AssetThumbnailCacheStatus::Failed,
        "intentional-test-failure"));

    ResetAssetThumbnailCacheCanonicalPathAttemptCountForTesting();
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Failed);
    const auto firstAttemptCount = GetAssetThumbnailCacheCanonicalPathAttemptCountForTesting();
    EXPECT_GT(firstAttemptCount, 0u);

    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Failed);
    EXPECT_EQ(GetAssetThumbnailCacheCanonicalPathAttemptCountForTesting(), firstAttemptCount)
        << "Repeated cache reads for the same stable request should not redo canonical containment checks.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, EvaluateCacheAvoidsColdPerFileCanonicalContainmentValidation)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto request = MakeThumbnailRequest(root, "prefab:Hero");

    ResetAssetThumbnailCacheCanonicalPathAttemptCountForTesting();
    (void)EvaluateAssetThumbnailCache(request);

    EXPECT_LE(GetAssetThumbnailCacheCanonicalPathAttemptCountForTesting(), 2u)
        << "Read-side cache containment for internally hashed thumbnail paths should only need "
           "to canonicalize the project/cache root; per-image and per-metadata weakly_canonical "
           "calls dominate thumbnail grid display time.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, EvaluateCacheReusesProjectRootContainmentAcrossColdEntries)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto first = MakeThumbnailRequest(root, "prefab:Hero");
    const auto second = MakeThumbnailRequest(root, "prefab:Hero", "source:v2");

    ResetAssetThumbnailCacheCanonicalPathAttemptCountForTesting();
    (void)EvaluateAssetThumbnailCache(first);
    (void)EvaluateAssetThumbnailCache(second);

    EXPECT_LE(GetAssetThumbnailCacheCanonicalPathAttemptCountForTesting(), 2u)
        << "Cold reads for distinct thumbnail entries in the same project should reuse the "
           "project/cache root physical containment result instead of canonicalizing the same "
           "roots once per visible asset.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, EvaluateCacheTelemetrySplitsResolveEntryCost)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto request = MakeThumbnailRequest(root, "prefab:Hero");
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());
    ASSERT_TRUE(WriteAssetThumbnailCacheFile(request, entry->imagePath, TinyPng()));
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Fresh, {}));

    ClearArtifactLoadTelemetry();
    (void)EvaluateAssetThumbnailCache(request);

    const auto records = SnapshotArtifactLoadTelemetry();
    const auto hasStage = [&records](const ArtifactLoadTelemetryStage stage)
    {
        return std::any_of(
            records.begin(),
            records.end(),
            [stage](const ArtifactLoadTelemetryRecord& record)
            {
                return record.stage == stage;
            });
    };

    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateResolveEntryBuild));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateResolveEntryContainmentKey));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateResolveEntryContainmentValidate));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateResolveEntryContainmentStamp));
    EXPECT_TRUE(hasStage(ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateResolveEntry));

    ClearArtifactLoadTelemetry();
    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, EvaluateCacheReusesDuplicateParentContainmentStampWithinEntry)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto request = MakeThumbnailRequest(root, "prefab:Hero");
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());

    ResetAssetThumbnailCacheContainmentStampAttemptCountForTesting();
    (void)EvaluateAssetThumbnailCache(request);

    ASSERT_EQ(entry->imagePath.parent_path(), entry->metadataPath.parent_path());
    EXPECT_LE(GetAssetThumbnailCacheContainmentStampAttemptCountForTesting(), 5u)
        << "The first read-side cache lookup has no containment memo to compare against, so it should only stamp paths once after the authoritative containment validation completes.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, MetadataWriteInvalidatesCachedMetadataLoad)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto request = MakeThumbnailRequest(root, "prefab:Hero");
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(
        request,
        *entry,
        AssetThumbnailCacheStatus::Failed,
        "intentional-test-failure"));

    ResetAssetThumbnailCacheMetadataFileLoadCountForTesting();
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Failed);
    ASSERT_EQ(GetAssetThumbnailCacheMetadataCacheEntryCountForTesting(), 1u);

    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(
        request,
        *entry,
        AssetThumbnailCacheStatus::Failed,
        "intentional-test-failure"));
    EXPECT_EQ(GetAssetThumbnailCacheMetadataCacheEntryCountForTesting(), 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceReusesStableFreshCacheEvaluation)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto request = MakeThumbnailRequest(root, "prefab:Hero");
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());
    ASSERT_TRUE(WriteAssetThumbnailCacheFile(request, entry->imagePath, TinyPng()));
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Fresh, {}));

    AssetThumbnailService service;
    ResetAssetThumbnailCacheEvaluationCountForTesting();

    EXPECT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(GetAssetThumbnailCacheEvaluationCountForTesting(), 1u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, MissingCacheEvaluationSkipsPhysicalContainmentValidation)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto request = MakeThumbnailRequest(root, "prefab:Hero");

    ResetAssetThumbnailCacheCanonicalPathAttemptCountForTesting();
    const auto evaluated = EvaluateAssetThumbnailCache(request, AssetThumbnailCacheIntegrityMode::Fast);

    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Missing);
    EXPECT_EQ(GetAssetThumbnailCacheCanonicalPathAttemptCountForTesting(), 0u)
        << "Cache misses without metadata should not perform UI-thread canonical path checks.";

    std::filesystem::remove_all(root);
}
#endif
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

TEST(AssetThumbnailCacheTests, FastEvaluationSkipsPngHeaderValidationButFullEvaluationDetectsInvalidHeader)
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

    ASSERT_GT(bytes.size(), 24u);
    bytes[16u] = 0u;
    bytes[17u] = 0u;
    bytes[18u] = 1u;
    bytes[19u] = 0u;
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
        auto request = MakeThumbnailRequest(root, "texture:Hero");
        request.sourceAssetPath = "Assets/Textures/Hero" + std::to_string(index) + ".png";
        request.kind = AssetThumbnailKind::Texture;
        request.requestRevision = index + 1u;
        request.freshnessInputs = {{"source", "tiny-png:v" + std::to_string(index)}};
        const auto entry = ResolveAssetThumbnailCacheEntry(request);
        ASSERT_TRUE(entry.has_value());
        ASSERT_TRUE(WriteAssetThumbnailCacheFile(request, entry->imagePath, TinyPng()));
        ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Fresh, {}))
            << "thumbnail cache metadata commit failed at iteration " << index;
        requests.push_back(request);
    }

    const auto newestEntry = ResolveAssetThumbnailCacheEntry(requests.back());
    ASSERT_TRUE(newestEntry.has_value());

    AssetThumbnailDiskCachePruneOptions options;
    options.maxEntries = 1u;
    options.maxBytes = UINT64_MAX;
    const auto pruned = PruneAssetThumbnailDiskCache(root, options);

    EXPECT_EQ(pruned.scannedEntries, 2u);
    EXPECT_EQ(pruned.removedEntries, 0u)
        << "Only current/previous generations remain after presentation rotation.";
    EXPECT_EQ(pruned.remainingEntries, 2u)
        << "Presentation current and previous generations must survive pruning.";
    EXPECT_EQ(EvaluateAssetThumbnailCache(requests.back()).status, AssetThumbnailCacheStatus::Fresh);
    EXPECT_TRUE(std::filesystem::exists(newestEntry->imagePath));
    EXPECT_TRUE(std::filesystem::exists(newestEntry->metadataPath));

    const auto droppedEntry = ResolveAssetThumbnailCacheEntry(requests.front());
    ASSERT_TRUE(droppedEntry.has_value());
    EXPECT_FALSE(std::filesystem::exists(droppedEntry->imagePath));
    EXPECT_FALSE(std::filesystem::exists(droppedEntry->metadataPath));

    for (size_t index = 1u; index < requests.size(); ++index)
    {
        const auto entry = ResolveAssetThumbnailCacheEntry(requests[index]);
        ASSERT_TRUE(entry.has_value());
        EXPECT_TRUE(std::filesystem::exists(entry->imagePath));
        EXPECT_TRUE(std::filesystem::exists(entry->metadataPath));
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
        auto request = MakeThumbnailRequest(root, "texture:Budget");
        request.sourceAssetPath = "Assets/Textures/Budget" + std::to_string(index) + ".png";
        request.kind = AssetThumbnailKind::Texture;
        request.requestRevision = index + 1u;
        request.freshnessInputs = {{"source", "tiny-png:v" + std::to_string(index)}};
        const auto entry = ResolveAssetThumbnailCacheEntry(request);
        ASSERT_TRUE(entry.has_value());
        ASSERT_TRUE(WriteAssetThumbnailCacheFile(request, entry->imagePath, TinyPng()));
        ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Fresh, {}))
            << "thumbnail cache metadata commit failed at iteration " << index;
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
    EXPECT_EQ(pruned.removedEntries, 0u)
        << "The two canonical presentation generations are protected from byte pruning.";
    EXPECT_EQ(pruned.remainingEntries, 2u);
    EXPECT_GT(pruned.remainingBytes, options.maxBytes)
        << "Presentation protection is allowed to exceed the ordinary cache budget.";
    EXPECT_EQ(EvaluateAssetThumbnailCache(requests.back()).status, AssetThumbnailCacheStatus::Fresh);
    EXPECT_EQ(EvaluateAssetThumbnailCache(requests.front()).status, AssetThumbnailCacheStatus::Fresh);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, GpuPreviewReadbackKeyIncludesProjectAndFreshnessIdentity)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect preview readback identity.";
#else
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, "prefab:ReadbackIdentity");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.freshnessInputs = {{"artifact", "prefab:v1"}};

    auto changedFreshness = request;
    changedFreshness.freshnessInputs = {{"artifact", "prefab:v2"}};
    EXPECT_NE(
        BuildThumbnailPreviewReadbackRequestKeyForTesting(request),
        BuildThumbnailPreviewReadbackRequestKeyForTesting(changedFreshness));

    auto changedProject = request;
    changedProject.projectRoot = root / "OtherProject";
    EXPECT_NE(
        BuildThumbnailPreviewReadbackRequestKeyForTesting(request),
        BuildThumbnailPreviewReadbackRequestKeyForTesting(changedProject));

    auto changedRevision = request;
    changedRevision.requestRevision = 2u;
    EXPECT_EQ(
        BuildThumbnailPreviewReadbackRequestKeyForTesting(request),
        BuildThumbnailPreviewReadbackRequestKeyForTesting(changedRevision));

    std::filesystem::remove_all(root);
#endif
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
    EXPECT_EQ(textureRequest->previewRendererVersion, "asset-browser-thumbnail-renderer:v8");
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
    EXPECT_EQ(materialRequest->previewRendererVersion, "asset-browser-thumbnail-renderer:v12");
    EXPECT_EQ(materialRequest->settingsFingerprint, "asset-browser-thumbnail:v20-gpu-black-frame-fallback");
    EXPECT_FALSE(materialRequest->dependencyStamp.empty());
    EXPECT_EQ(materialRequest->colorSpaceMode, "srgb");
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
    EXPECT_EQ(sourceMaterialRequest->settingsFingerprint, "asset-browser-thumbnail:v20-gpu-black-frame-fallback");

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
    EXPECT_EQ(modelRequest->previewRendererVersion, "asset-browser-thumbnail-renderer:v35");
    EXPECT_EQ(modelRequest->settingsFingerprint, "asset-browser-thumbnail:v37-prefab-complete-material-fallback");
    EXPECT_FALSE(modelRequest->dependencyStamp.empty());
    EXPECT_EQ(modelRequest->colorSpaceMode, "srgb");
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
    EXPECT_EQ(meshRequest->previewRendererVersion, "asset-browser-thumbnail-renderer:v9");
    EXPECT_EQ(meshRequest->settingsFingerprint, "asset-browser-thumbnail:v20-gpu-black-frame-fallback");
    EXPECT_FALSE(meshRequest->dependencyStamp.empty());
    EXPECT_EQ(meshRequest->colorSpaceMode, "srgb");
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
    EXPECT_EQ(prefabRequest->previewRendererVersion, "asset-browser-thumbnail-renderer:v35");
    EXPECT_EQ(prefabRequest->settingsFingerprint, "asset-browser-thumbnail:v37-prefab-complete-material-fallback");
    EXPECT_FALSE(prefabRequest->dependencyStamp.empty());
    EXPECT_EQ(prefabRequest->colorSpaceMode, "srgb");
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
    EXPECT_EQ(modelRequest->dependencyStamp.find("artifact-db="), std::string::npos)
        << "A resolved content-addressed artifact must not inherit the global ArtifactDB stamp.";
    EXPECT_NE(modelRequest->dependencyStamp.find("artifact-record="), std::string::npos);
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
    EXPECT_EQ(modelRequest->dependencyStamp.find("artifact-db="), std::string::npos)
        << "Deferred lookup must not make this asset's cache identity depend on unrelated imports.";
    EXPECT_EQ(modelRequest->dependencyStamp.find("artifact-record="), std::string::npos);
    EXPECT_EQ(modelRequest->dependencyStamp.find("artifact-file="), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ResidentRequestReusesSnapshotManifestForCanonicalFreshness)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = AssetId(NLS::Guid::Parse("c6060606-0606-4606-8606-060606060606"));
    const auto sourcePath = std::string("Assets/Models/Resident.gltf");
    const auto artifactPath =
        (std::filesystem::path("Library") /
            "Artifacts" /
            BuildArtifactStorageRelativePath(
                BuildArtifactStorageFileName(assetId.ToString() + ":prefab:Resident")))
            .generic_string();
    WriteTextFile(root / "Assets" / "Models" / "Resident.gltf", "model");
    WriteTextFile(
        root / "Assets" / "Models" / "Resident.gltf.meta",
        "GUID=" + assetId.ToString() + "\nIMPORTER_ID=scene-model\nASSET_TYPE=model-scene\n");

    auto database = std::make_shared<AssetDatabaseFacade>(MakeProjectEditorAssetRoots(root));
    ASSERT_TRUE(database->Refresh());
    ArtifactManifest manifest;
    manifest.sourceAssetId = assetId;
    manifest.primarySubAssetKey = "prefab:Resident";
    ImportedArtifact artifact;
    artifact.sourceAssetId = assetId;
    artifact.subAssetKey = "prefab:Resident";
    artifact.artifactType = ArtifactType::Prefab;
    artifact.artifactPath = artifactPath;
    manifest.subAssets.push_back(artifact);
    database->AddArtifactManifest(std::move(manifest));

    AssetBrowserItem item;
    item.kind = AssetBrowserItemKind::SourceAsset;
    item.type = AssetBrowserItemType::Model;
    item.assetId = assetId;
    item.sourceAssetPath = sourcePath;

    AssetThumbnailRequestBuildContext context;
    context.deferManifestLookups = true;
    context.assetDatabaseSnapshot = AssetDatabaseFacade::CreateReadOnlySnapshot(*database);
    context.residentPrefabPreviewRegistry = ResidentPrefabPreviewRegistry::Create();

    const auto request = BuildAssetThumbnailRequestForItem(root, item, 96u, context);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->kind, AssetThumbnailKind::PrefabPreview);
    EXPECT_EQ(request->subAssetKey, "prefab:Resident");
    EXPECT_EQ(request->artifactPath, artifactPath);
    EXPECT_NE(request->dependencyStamp.find("artifact-file="), std::string::npos);
    EXPECT_EQ(
        request->dependencyStamp,
        BuildPrefabThumbnailDependencyStamp(
            root,
            assetId,
            sourcePath,
            "prefab:Resident",
            artifactPath));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceBuildRequestRetainsAssetDatabaseSnapshotForBackgroundPrefabPreparation)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.gltf", {0x67u, 0x6cu, 0x54u, 0x46u});

    auto database = std::make_shared<AssetDatabaseFacade>(MakeProjectEditorAssetRoots(root));
    AssetThumbnailRequestBuildContext context;
    context.deferManifestLookups = true;
    context.assetDatabaseSnapshot = AssetDatabaseFacade::CreateReadOnlySnapshot(*database);

    AssetBrowserItem item;
    item.kind = AssetBrowserItemKind::SourceAsset;
    item.type = AssetBrowserItemType::Model;
    item.assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("b4040404-0404-4404-8404-040404040404"));
    item.sourceAssetPath = "Assets/Models/Hero.gltf";

    const auto request = BuildAssetThumbnailRequestForItem(root, item, 96u, context);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->assetDatabaseSnapshot.get(), context.assetDatabaseSnapshot.get());

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, FreshCommitClearsFailureSidecarWithoutLosingCanonicalImage)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto request = MakeThumbnailRequest(root, "texture:Retry");
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());
    ASSERT_TRUE(WriteAssetThumbnailCacheFile(request, entry->imagePath, TinyPng()));
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(
        request,
        AssetThumbnailCacheStatus::Fresh,
        {}));
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(
        request,
        AssetThumbnailCacheStatus::Failed,
        "transient-preview-failure"));

    const auto failureMetadataPath =
        std::filesystem::path(entry->metadataPath.generic_string() + ".failure");
    ASSERT_TRUE(std::filesystem::exists(failureMetadataPath));
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(
        request,
        AssetThumbnailCacheStatus::Fresh,
        {}));

    EXPECT_TRUE(std::filesystem::is_regular_file(entry->imagePath));
    EXPECT_TRUE(std::filesystem::is_regular_file(entry->metadataPath));
    EXPECT_FALSE(std::filesystem::exists(failureMetadataPath));
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Fresh);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceResolvesDeferredModelManifestRequestWhenGeneratingThumbnail)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

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
    context.residentPrefabPreviewRegistry = ResidentPrefabPreviewRegistry::Create();
    const auto deferredRequest = BuildAssetThumbnailRequestForItem(root, modelSource, 48u, context);
    ASSERT_TRUE(deferredRequest.has_value());
    ASSERT_TRUE(deferredRequest->artifactPath.empty());
    ASSERT_TRUE(deferredRequest->residentPrefabPreviewSource.has_value());
    EXPECT_TRUE(deferredRequest->residentPrefabPreviewSource->snapshot.expired());

    AssetThumbnailService service;
    ResetAssetThumbnailManifestLookupStatsForTesting();
    ASSERT_EQ(service.GetThumbnail(*deferredRequest).status, AssetThumbnailServiceStatus::Pending);
    CapturingThumbnailPreviewRenderer renderer;
    std::optional<AssetThumbnailServiceResult> generated;
    for (int attempt = 0; attempt < 100 && !generated.has_value(); ++attempt)
    {
        generated = service.GenerateNextThumbnail(renderer, true);
        if (generated.has_value() &&
            generated->diagnostic == "thumbnail-preview-request-resolution-pending")
        {
            generated.reset();
        }
        if (!generated.has_value())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Pending) << generated->diagnostic;
    const auto manifestLookupStats = GetAssetThumbnailManifestLookupStatsForTesting();
    EXPECT_EQ(manifestLookupStats.mainThreadLookupCount, 0u)
        << "Deferred GPU manifest resolution must not run on the thumbnail pump thread.";
    EXPECT_GE(manifestLookupStats.backgroundThreadLookupCount, 1u);
    auto completed = service.ConsumeCompletedThumbnail();
    for (int attempt = 0; attempt < 100 && !completed.has_value(); ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        completed = service.ConsumeCompletedThumbnail();
    }
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->status, AssetThumbnailServiceStatus::Fresh) << completed->diagnostic;
    EXPECT_EQ(service.GetThumbnailState(*deferredRequest), ThumbnailState::Ready);
    EXPECT_EQ(service.GetThumbnailState(*deferredRequest), ThumbnailState::Ready);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, SingleGpuPumpStartsMultipleDeferredManifestResolutions)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem(8u);
    ASSERT_TRUE(jobSystem.IsInitialized());

    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto makeDeferredModelRequest = [&root](
        const NLS::Core::Assets::AssetId& assetId,
        const std::string& name)
        -> std::optional<AssetThumbnailRequest>
    {
        const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
        std::filesystem::create_directories(artifactRoot);
        WriteTextFile(root / "Assets" / "Models" / (name + ".fbx"), "large model source");
        WriteNativeArtifactTextFile(
            artifactRoot / (name + ".nprefab"),
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
            "\"primarySubAssetKey\":\"prefab:" + name + "\","
            "\"subAssets\":[{"
            "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
            "\"subAssetKey\":\"prefab:" + name + "\","
            "\"artifactType\":\"Prefab\","
            "\"loaderId\":\"native-prefab\","
            "\"targetPlatform\":\"editor\","
            "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/" + name + ".nprefab\","
            "\"contentHash\":\"prefab-hash-" + name + "\"}]}" );

        AssetBrowserItem item;
        item.kind = AssetBrowserItemKind::SourceAsset;
        item.type = AssetBrowserItemType::Model;
        item.assetId = assetId;
        item.sourceAssetPath = "Assets/Models/" + name + ".fbx";

        AssetThumbnailRequestBuildContext context;
        context.deferManifestLookups = true;
        return BuildAssetThumbnailRequestForItem(root, item, 48u, context);
    };

    const auto firstRequest = makeDeferredModelRequest(
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("b8080808-0808-4808-8808-080808080808")),
        "First");
    const auto secondRequest = makeDeferredModelRequest(
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("b9090909-0909-4909-8909-090909090909")),
        "Second");
    ASSERT_TRUE(firstRequest.has_value());
    ASSERT_TRUE(secondRequest.has_value());
    ASSERT_TRUE(firstRequest->artifactPath.empty());
    ASSERT_TRUE(secondRequest->artifactPath.empty());

    AssetThumbnailManifestLookupStatsForTesting lookupStats;
    CapturingThumbnailPreviewRenderer renderer;
    {
        AssetThumbnailService service;
        ASSERT_EQ(service.GetThumbnail(*firstRequest).status, AssetThumbnailServiceStatus::Pending);
        ASSERT_EQ(service.GetThumbnail(*secondRequest).status, AssetThumbnailServiceStatus::Pending);
        ASSERT_EQ(service.GetQueuedRequestCount(), 2u);

        ResetAssetThumbnailManifestLookupStatsForTesting();
        (void)service.GenerateNextThumbnail(renderer, true);
    }

    lookupStats = GetAssetThumbnailManifestLookupStatsForTesting();
    ::testing::Test::RecordProperty(
        "manifestMainThreadLookupCount",
        static_cast<int>(lookupStats.mainThreadLookupCount));
    ::testing::Test::RecordProperty(
        "manifestBackgroundThreadLookupCount",
        static_cast<int>(lookupStats.backgroundThreadLookupCount));
    ::testing::Test::RecordProperty("rendererSupportsCount", static_cast<int>(renderer.supportsCount));
    ::testing::Test::RecordProperty("rendererRenderCount", static_cast<int>(renderer.renderCount));
    EXPECT_EQ(lookupStats.mainThreadLookupCount, 0u);
    EXPECT_GE(lookupStats.backgroundThreadLookupCount, 2u)
        << "A single GPU pump should fan out deferred manifest resolution for visible requests.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, DeferredSourceModelPrefabPreviewUsesManifestPrimaryArtifactNotSourceFile)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

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
    CapturingThumbnailPreviewRenderer renderer;
    const auto generated = PumpUntilDeferredPreviewResolves(service, renderer);
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Pending) << generated->diagnostic;
    auto completed = service.ConsumeCompletedThumbnail();
    for (int attempt = 0; attempt < 100 && !completed.has_value(); ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        completed = service.ConsumeCompletedThumbnail();
    }
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->status, AssetThumbnailServiceStatus::Fresh) << completed->diagnostic;
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

    {
        CapturingThumbnailPreviewRenderer renderer;
        AssetThumbnailService service;
        ASSERT_EQ(service.GetThumbnail(*deferredRequest).status, AssetThumbnailServiceStatus::Pending);
        const auto pending = PumpUntilDeferredPreviewResolves(service, renderer);
        ASSERT_TRUE(pending.has_value());
        EXPECT_EQ(pending->status, AssetThumbnailServiceStatus::Pending);
        ASSERT_TRUE(renderer.lastSupportsRequest.has_value());
        EXPECT_EQ(renderer.lastSupportsRequest->subAssetKey, "prefab:Heavy");
        EXPECT_TRUE(NLS::Core::Assets::IsContentStorageArtifactPath(renderer.lastSupportsRequest->artifactPath));
        ASSERT_TRUE(renderer.lastRenderRequest.has_value());
        EXPECT_EQ(renderer.lastRenderRequest->subAssetKey, "prefab:Heavy");
        EXPECT_TRUE(NLS::Core::Assets::IsContentStorageArtifactPath(renderer.lastRenderRequest->artifactPath));
    }

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
        context.residentPrefabPreviewRegistry = ResidentPrefabPreviewRegistry::Create();
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
        const auto pending = PumpUntilDeferredPreviewResolves(service, renderer);
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
    EXPECT_EQ(service.GetThumbnailState(meshRequest), ThumbnailState::Encoding);
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
        EXPECT_EQ(service.GetThumbnailState(materialRequest), ThumbnailState::Encoding);
    }

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, LightGpuPumpReachesMaterialPreviewBehindCpuOnlyMeshBurst)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();

    {
        AssetThumbnailService service;
        CapturingThumbnailPreviewRenderer renderer;

        for (size_t index = 0u; index < 128u; ++index)
        {
            auto meshRequest = MakeThumbnailRequest(root, "mesh:Queued" + std::to_string(index));
            meshRequest.sourceAssetPath = "Assets/Models/Queued.fbx";
            meshRequest.artifactPath = "Library/Artifacts/queued/meshes/Queued" + std::to_string(index) + ".nmesh";
            meshRequest.kind = AssetThumbnailKind::ModelPreview;
            meshRequest.generatedSubAsset = true;
            meshRequest.priority = ThumbnailRequestPriority::Visible;
            meshRequest.freshnessInputs = {{"source", "mesh:" + std::to_string(index)}};
            ASSERT_EQ(service.GetThumbnail(meshRequest).status, AssetThumbnailServiceStatus::Pending);
        }

        auto materialRequest = MakeThumbnailRequest(root, "material:Body");
        materialRequest.sourceAssetPath = "Assets/Models/Queued.fbx";
        materialRequest.artifactPath.clear();
        materialRequest.kind = AssetThumbnailKind::MaterialSphere;
        materialRequest.priority = ThumbnailRequestPriority::Visible;
        materialRequest.freshnessInputs = {{"source", "material:v1"}};
        ASSERT_EQ(service.GetThumbnail(materialRequest).status, AssetThumbnailServiceStatus::Pending);

        const auto generated = service.GenerateNextThumbnail(renderer, false);
        ASSERT_TRUE(generated.has_value())
            << "GPU preview selection must skip CPU-only mesh thumbnails instead of repeatedly "
               "deferring the same visible burst back to the queue front.";
        EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Pending);
        ASSERT_TRUE(renderer.lastRenderRequest.has_value());
        EXPECT_EQ(renderer.lastRenderRequest->kind, AssetThumbnailKind::MaterialSphere);
        EXPECT_EQ(renderer.lastRenderRequest->subAssetKey, "material:Body");
        EXPECT_EQ(renderer.renderCount, 1u);
        EXPECT_EQ(service.GetThumbnailState(materialRequest), ThumbnailState::Encoding);
    }

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, HeavyGpuPumpBoundsUnsupportedPrefabScanPerCall)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();

    AssetThumbnailService service;
    constexpr size_t prefabCount = 24u;
    for (size_t index = 0u; index < prefabCount; ++index)
    {
        auto prefabRequest = MakeThumbnailRequest(root, "prefab:Queued" + std::to_string(index));
        prefabRequest.sourceAssetPath = "Assets/Prefabs/Queued" + std::to_string(index) + ".prefab";
        prefabRequest.kind = AssetThumbnailKind::PrefabPreview;
        prefabRequest.priority = ThumbnailRequestPriority::Visible;
        prefabRequest.freshnessInputs = {{"source", "prefab:" + std::to_string(index)}};
        ASSERT_EQ(service.GetThumbnail(prefabRequest).status, AssetThumbnailServiceStatus::Pending);
    }

    RejectingThumbnailPreviewRenderer renderer;
    EXPECT_FALSE(service.GenerateNextThumbnail(renderer, true).has_value());
    EXPECT_EQ(renderer.supportsCount, 8u)
        << "Heavy GPU preview pumps should scan a bounded slice so large prefab folders cannot monopolize a frame.";
    EXPECT_EQ(renderer.renderCount, 0u);
    EXPECT_EQ(service.GetQueuedRequestCount(), prefabCount);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, PrefabSubAssetThumbnailWaitsForGpuRendererWithoutCpuFallback)
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
    EXPECT_FALSE(generated.has_value());
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Missing);

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
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

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
        const auto generated = PumpUntilDeferredPreviewResolves(service, renderer);
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
    request.generatedSubAsset = true;
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

    EXPECT_EQ(renderer.supportsCount, 0u);
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

    {
        PendingThenReadyThumbnailPreviewRenderer renderer;
        AssetThumbnailService service;
        ASSERT_EQ(service.GetThumbnail(*request).status, AssetThumbnailServiceStatus::Pending);
        const auto pending = PumpUntilDeferredPreviewResolves(service, renderer);
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
    }

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

    {
        PendingThenReadyThumbnailPreviewRenderer renderer;
        AssetThumbnailService service;
        ASSERT_EQ(service.GetThumbnail(*request).status, AssetThumbnailServiceStatus::Pending);
        const auto pending = PumpUntilDeferredPreviewResolves(service, renderer);
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
    }

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ScopeSupersedePreservesGpuReadbackPollingState)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();

    auto request = MakeThumbnailRequest(root, "material:ScopeSupersede");
    request.sourceAssetPath = "Assets/Materials/ScopeSupersede.mat";
    request.kind = AssetThumbnailKind::MaterialSphere;
    request.requestedSize = 48u;
    request.priority = ThumbnailRequestPriority::Visible;
    request.freshnessInputs = {{"source", "scope-supersede-material:v1"}};

    PendingThenReadyThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);

    const auto pending = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->diagnostic, "thumbnail-gpu-preview-readback-pending");
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::WaitingForGpu);

    service.SupersedeQueuedRequestsForGeneration("new-visible-scope");
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::WaitingForGpu)
        << "Changing the UI thumbnail scope must not cancel an already submitted GPU readback.";
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

    EXPECT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::WaitingForGpu);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

    const auto polled = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(polled.has_value());
    EXPECT_EQ(polled->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Encoding);
    EXPECT_EQ(renderer.renderCount, 2u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, QueueBackpressureDoesNotEvictActiveGpuReadback)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();

    auto active = MakeThumbnailRequest(root, "material:ActiveReadback");
    active.sourceAssetPath = "Assets/Materials/ActiveReadback.mat";
    active.kind = AssetThumbnailKind::MaterialSphere;
    active.requestedSize = 48u;
    active.priority = ThumbnailRequestPriority::Background;
    active.freshnessInputs = {{"source", "active-readback:v1"}};

    PendingThenReadyThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(active).status, AssetThumbnailServiceStatus::Pending);

    const auto pending = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->diagnostic, "thumbnail-gpu-preview-readback-pending");
    EXPECT_EQ(service.GetThumbnailState(active), ThumbnailState::WaitingForGpu);

    for (size_t index = 0u; index < 511u; ++index)
    {
        auto visible = MakeThumbnailRequest(root, "texture:Visible" + std::to_string(index));
        visible.sourceAssetPath = "Assets/Textures/Visible" + std::to_string(index) + ".png";
        visible.kind = AssetThumbnailKind::Texture;
        visible.priority = ThumbnailRequestPriority::Visible;
        visible.freshnessInputs = {{"source", "visible-fill:" + std::to_string(index)}};
        ASSERT_EQ(service.RequestAssetPreview(visible).status, AssetThumbnailServiceStatus::Pending);
    }
    ASSERT_EQ(service.GetQueuedRequestCount(), 512u);

    auto newcomer = MakeThumbnailRequest(root, "texture:NewVisible");
    newcomer.sourceAssetPath = "Assets/Textures/NewVisible.png";
    newcomer.kind = AssetThumbnailKind::Texture;
    newcomer.priority = ThumbnailRequestPriority::Visible;
    newcomer.freshnessInputs = {{"source", "new-visible:v1"}};

    const auto rejected = service.RequestAssetPreview(newcomer);
    EXPECT_EQ(rejected.status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_EQ(rejected.diagnostic, "thumbnail-generation-queue-full");
    EXPECT_EQ(service.GetThumbnailState(active), ThumbnailState::WaitingForGpu)
        << "Queue backpressure must not cancel a GPU readback that has already been submitted.";
    EXPECT_EQ(service.GetQueuedRequestCount(), 512u);

    const auto polled = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(polled.has_value());
    EXPECT_EQ(polled->status, AssetThumbnailServiceStatus::Pending)
        << "diagnostic=" << polled->diagnostic;
    EXPECT_EQ(service.GetThumbnailState(active), ThumbnailState::Encoding);

    const auto retried = service.RequestAssetPreview(newcomer);
    EXPECT_EQ(retried.status, AssetThumbnailServiceStatus::Pending)
        << "A generation-scope item retained during backpressure must be accepted after queue capacity is released.";
    EXPECT_EQ(service.GetThumbnailState(newcomer), ThumbnailState::Queued);

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
    EXPECT_EQ(service.GetThumbnailState(first), ThumbnailState::Encoding);
    EXPECT_EQ(service.GetThumbnailState(second), ThumbnailState::Queued);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, GpuPumpImmediatelyContinuesResourcePendingPreviewWithLightweightPump)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();

    auto request = MakeThumbnailRequest(root, "prefab:Hero");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 48u;
    request.priority = ThumbnailRequestPriority::Visible;
    request.artifactPath = "Library/Artifacts/67/670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772";
    request.freshnessInputs = {{"source", "prefab:v1"}, {"artifact", "prefab-artifact:v1"}};
    WriteBinaryFile(root / request.artifactPath, std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});

    ResourcesPendingThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);

    const auto pending = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(pending->diagnostic, "thumbnail-gpu-preview-resources-pending");
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::WaitingForResources);
    EXPECT_EQ(renderer.pumpCount, 1u);
    EXPECT_EQ(renderer.renderCount, 0u);

    const auto retried = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(retried.has_value());
    EXPECT_EQ(retried->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(retried->diagnostic, "thumbnail-gpu-preview-resources-pending");
    EXPECT_EQ(renderer.pumpCount, 2u)
        << "Resource-pending prefab previews should continue with the lightweight resource pump.";
    EXPECT_EQ(renderer.renderCount, 0u)
        << "Resource-pending prefab previews should not re-enter full GPU render while resources are still pending.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, VisibleResourcePendingPrefabKeepsPriorityUntilReady)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto first = MakeThumbnailRequest(root, "prefab:First");
    first.kind = AssetThumbnailKind::PrefabPreview;
    first.priority = ThumbnailRequestPriority::Visible;
    first.artifactPath = "Library/Artifacts/71/first-prefab";
    first.freshnessInputs = {{ "artifact", "first:v1" }};
    WriteBinaryFile(root / first.artifactPath, std::vector<uint8_t>{ 'p' });

    auto second = MakeThumbnailRequest(root, "prefab:Second");
    second.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("72727272-7272-4272-8272-727272727272"));
    second.kind = AssetThumbnailKind::PrefabPreview;
    second.priority = ThumbnailRequestPriority::Visible;
    second.artifactPath = "Library/Artifacts/72/second-prefab";
    second.freshnessInputs = {{ "artifact", "second:v1" }};
    WriteBinaryFile(root / second.artifactPath, std::vector<uint8_t>{ 'p' });

    ResourcesPendingThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(first).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.RequestAssetPreview(second).status, AssetThumbnailServiceStatus::Pending);

    ASSERT_TRUE(service.GenerateNextThumbnail(renderer, true).has_value());
    ASSERT_TRUE(service.GenerateNextThumbnail(renderer, true).has_value());

    ASSERT_EQ(renderer.pumpKeys.size(), 2u);
    EXPECT_EQ(renderer.pumpKeys[0], "prefab:First");
    EXPECT_EQ(renderer.pumpKeys[1], "prefab:First")
        << "The first visible prefab must keep advancing instead of rotating hundreds of mesh uploads across every visible prefab.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, GpuPumpTerminalMeshFailureSkipsRenderAndSuccessfulCachePublication)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, "prefab:TerminalMeshFailure");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 48u;
    request.priority = ThumbnailRequestPriority::Visible;
    request.artifactPath = "Library/Artifacts/68/680d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490773";
    request.freshnessInputs = {{"source", "prefab:v1"}, {"artifact", "prefab-artifact:v1"}};
    WriteBinaryFile(root / request.artifactPath, std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});

    TerminalResourceFailureThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);

    const auto failed = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(failed.has_value());
    EXPECT_EQ(failed->status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(failed->diagnostic, "thumbnail-gpu-preview-mesh-load-failed|meshFailed=1");
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Failed);
    EXPECT_EQ(renderer.pumpCount, 1u);
    EXPECT_EQ(renderer.renderCount, 0u);
    EXPECT_NE(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Fresh);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, GpuPumpPublishesExactlyOnceAfterMixedMeshesBecomeReady)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, "prefab:MixedThenReady");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 48u;
    request.priority = ThumbnailRequestPriority::Visible;
    request.artifactPath = "Library/Artifacts/69/690d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490774";
    request.freshnessInputs = {{"source", "prefab:v1"}, {"artifact", "prefab-artifact:v1"}};
    WriteBinaryFile(root / request.artifactPath, std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});

    MixedPendingThenReadyThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);

    const auto mixed = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(mixed.has_value());
    EXPECT_EQ(mixed->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::WaitingForResources);
    EXPECT_EQ(renderer.pumpCount, 1u);
    EXPECT_EQ(renderer.renderCount, 0u);
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Missing);

    std::optional<AssetThumbnailServiceResult> submitted;
    for (size_t attempt = 0u; attempt < 4u && !submitted.has_value(); ++attempt)
        submitted = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(submitted.has_value());
    EXPECT_EQ(submitted->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(submitted->diagnostic, "thumbnail-gpu-preview-cache-write-pending");
    EXPECT_EQ(renderer.pumpCount, 2u);
    EXPECT_EQ(renderer.renderCount, 1u);

    auto written = service.ConsumeCompletedThumbnail();
    for (size_t attempt = 0u; attempt < 500u && !written.has_value(); ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        written = service.ConsumeCompletedThumbnail();
    }
    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(written->status, AssetThumbnailServiceStatus::Fresh);
    ASSERT_TRUE(written->cacheEntry.has_value());
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Fresh);
    EXPECT_EQ(renderer.renderCount, 1u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ResourcePendingContinuationSurvivesRepeatedThumbnailLookups)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, "prefab:ResourcePendingRepeatedLookup");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 48u;
    request.priority = ThumbnailRequestPriority::Visible;
    request.artifactPath =
        "Library/Artifacts/69/690d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490774";
    request.freshnessInputs = { {"source", "prefab-repeated-lookup:v1"}, {"artifact", "prefab-repeated-lookup-artifact:v1"} };
    WriteBinaryFile(root / request.artifactPath, std::vector<uint8_t> {'p', 'r', 'e', 'f', 'a', 'b'});

    MixedPendingThenReadyThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    const auto resourcePending = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(resourcePending.has_value());
    EXPECT_EQ(resourcePending->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::WaitingForResources);
    ASSERT_NE(resourcePending->requestRevision, 0u);

    // AssetBrowser polls the same item while its resource continuation is
    // waiting. This must not create a newer revision and strand the original
    // continuation as an obsolete request.
    const auto repeatedLookup = service.GetThumbnail(request);
    EXPECT_EQ(repeatedLookup.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(repeatedLookup.requestRevision, resourcePending->requestRevision);
    EXPECT_GT(service.GetQueuedRequestCount(), 0u);

    std::optional<AssetThumbnailServiceResult> submitted;
    for (size_t attempt = 0u; attempt < 8u && !submitted.has_value(); ++attempt)
        submitted = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(submitted.has_value());
    EXPECT_EQ(submitted->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(submitted->diagnostic, "thumbnail-gpu-preview-cache-write-pending");
    EXPECT_EQ(renderer.renderCount, 1u);

    std::optional<AssetThumbnailServiceResult> completed;
    for (size_t attempt = 0u; attempt < 500u && !completed.has_value(); ++attempt)
    {
        completed = service.ConsumeCompletedThumbnail();
        if (!completed.has_value())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Ready);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ResourcePendingContinuationSurvivesGenerationScopeRebuild)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, "prefab:ImportedResident");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 48u;
    request.priority = ThumbnailRequestPriority::Visible;
    request.artifactPath = "Library/Artifacts/69/imported-resident-prefab";
    request.freshnessInputs = {
        {"source", "imported-resident:v1"},
        {"artifact", "imported-resident-artifact:v1"}};
    WriteBinaryFile(root / request.artifactPath, std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});

    MixedPendingThenReadyThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    const auto resourcePending = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(resourcePending.has_value());
    ASSERT_EQ(resourcePending->status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetThumbnailState(request), ThumbnailState::WaitingForResources);
    ASSERT_EQ(renderer.pumpCount, 1u);

    service.SupersedeQueuedRequestsForGeneration("Assets/Imported#rebuilt");
    EXPECT_TRUE(service.HasQueuedGpuPreviewResourceContinuation());
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

    const auto submitted = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(submitted.has_value());
    EXPECT_EQ(submitted->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(submitted->diagnostic, "thumbnail-gpu-preview-cache-write-pending");
    EXPECT_EQ(renderer.pumpCount, 2u);
    EXPECT_EQ(renderer.renderCount, 1u);

    std::optional<AssetThumbnailServiceResult> completed;
    for (size_t attempt = 0u; attempt < 500u && !completed.has_value(); ++attempt)
    {
        completed = service.ConsumeCompletedThumbnail();
        if (!completed.has_value())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->status, AssetThumbnailServiceStatus::Fresh);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ResourcePendingRequestDoesNotAdoptDifferentPrefab)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto first = MakeThumbnailRequest(root, "prefab:FirstPending");
    first.kind = AssetThumbnailKind::PrefabPreview;
    first.requestedSize = 48u;
    first.priority = ThumbnailRequestPriority::Visible;
    first.artifactPath = "Library/Artifacts/69/first-pending-prefab";
    first.freshnessInputs = {{"artifact", "first-pending:v1"}};
    WriteBinaryFile(root / first.artifactPath, std::vector<uint8_t>{'p'});

    auto second = first;
    second.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("73737373-7373-4373-8373-737373737373"));
    second.subAssetKey = "prefab:SecondQueued";
    second.artifactPath = "Library/Artifacts/73/second-queued-prefab";
    second.freshnessInputs = {{"artifact", "second-queued:v1"}};
    WriteBinaryFile(root / second.artifactPath, std::vector<uint8_t>{'p'});

    ResourcesPendingThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(first).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_TRUE(service.GenerateNextThumbnail(renderer, true).has_value());
    ASSERT_TRUE(service.HasQueuedGpuPreviewResourceContinuation());
    ASSERT_EQ(service.GetQueuedRequestCount(), 1u);

    const auto queuedSecond = service.GetThumbnail(second);
    EXPECT_EQ(queuedSecond.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetThumbnailState(second), ThumbnailState::Queued);
    EXPECT_EQ(service.GetQueuedRequestCount(), 2u)
        << "A resource continuation for one cache key must not impersonate another prefab's owner.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ResourcePendingContinuationExpiresAfterItsDeadline)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, "prefab:ResourcePendingDeadline");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 48u;
    request.priority = ThumbnailRequestPriority::Visible;
    request.artifactPath = "Library/Artifacts/69/resource-pending-deadline";
    request.freshnessInputs = {{"source", "prefab-deadline:v1"}, {"artifact", "prefab-deadline-artifact:v1"}};
    WriteBinaryFile(root / request.artifactPath, std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});

    ResourcesPendingThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);

    const auto pending = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(pending.has_value());
    ASSERT_EQ(pending->status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetThumbnailState(request), ThumbnailState::WaitingForResources);

#if defined(NLS_ENABLE_TEST_HOOKS)
    service.SetVisibleThumbnailRequestAgeForTesting(
        request,
        std::chrono::seconds(21));
    const auto stillWaitingForResources = service.GetThumbnail(request);
    EXPECT_EQ(stillWaitingForResources.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_NE(service.GetThumbnailState(request), ThumbnailState::Failed);
    EXPECT_NE(service.GetThumbnailState(request), ThumbnailState::Cancelled);
    EXPECT_TRUE(service.HasQueuedGpuPreviewResourceContinuation());
    EXPECT_FALSE(service.ConsumeCompletedThumbnail(false).has_value())
        << "The ordinary visible deadline must not preempt the longer resource deadline.";

    // Move the request past the production deadline without changing the
    // production timeout or adding a 120-second unit-test delay.
    service.SetGpuPreviewResourcePendingAgeForTesting(
        request,
        std::chrono::seconds(121));

    EXPECT_FALSE(service.GenerateNextThumbnail(renderer, true).has_value())
        << "An expired resource continuation must be retired before another renderer pump.";

    const auto timedOut = service.GetThumbnail(request);
    EXPECT_EQ(timedOut.status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Failed);
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);
    EXPECT_EQ(renderer.pumpCount, 1u)
        << "An expired resource continuation must not be pumped again after it becomes terminal.";
#else
    GTEST_SKIP() << "Resource deadline timing is only shortened in test-hook builds.";
#endif

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ResourcePendingProgressExtendsTheStallDeadline)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "Resource deadline timing requires test hooks.";
#else
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, "prefab:ResourcePendingProgress");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 48u;
    request.priority = ThumbnailRequestPriority::Visible;
    request.artifactPath = "Library/Artifacts/69/resource-pending-progress";
    request.freshnessInputs = {
        {"source", "prefab-progress:v1"},
        {"artifact", "prefab-progress-artifact:v1"}};
    WriteBinaryFile(root / request.artifactPath, std::vector<uint8_t>{'p', 'r', 'o', 'g'});

    ResourcesPendingThumbnailPreviewRenderer renderer;
    renderer.reportProgress = true;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);

    const auto firstPending = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(firstPending.has_value());
    ASSERT_EQ(firstPending->status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(renderer.pumpCount, 1u);

    service.SetGpuPreviewResourceRequestStartAgeForTesting(
        request,
        std::chrono::seconds(121));
    const auto progressed = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(progressed.has_value())
        << "Recent renderer progress must supersede the older admission timestamp.";
    EXPECT_EQ(progressed->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::WaitingForResources);
    EXPECT_EQ(renderer.pumpCount, 2u);

    service.SetGpuPreviewResourcePendingAgeForTesting(
        request,
        std::chrono::seconds(121));
    EXPECT_FALSE(service.GenerateNextThumbnail(renderer, true).has_value());
    EXPECT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(renderer.pumpCount, 2u)
        << "A genuinely stalled continuation must still expire without another renderer pump.";

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailCacheTests, ActiveResourceWorkExtendsTheStallDeadlineUntilItStops)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "Resource deadline timing requires test hooks.";
#else
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, "prefab:ActiveResourceWork");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 48u;
    request.priority = ThumbnailRequestPriority::Visible;
    request.artifactPath = "Library/Artifacts/69/active-resource-work";
    request.freshnessInputs = {
        {"source", "active-resource-work:v1"},
        {"artifact", "active-resource-work-artifact:v1"}};
    WriteBinaryFile(root / request.artifactPath, std::vector<uint8_t>{'w', 'o', 'r', 'k'});

    ResourcesPendingThumbnailPreviewRenderer renderer;
    renderer.reportActiveResourceWork = true;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);

    const auto firstPending = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(firstPending.has_value());
    ASSERT_EQ(firstPending->status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(renderer.pumpCount, 1u);

    service.SetGpuPreviewResourcePendingAgeForTesting(
        request,
        std::chrono::seconds(121));
    const auto stillActive = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(stillActive.has_value())
        << "Active dependency work must receive another renderer pump before expiring.";
    EXPECT_EQ(stillActive->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(renderer.pumpCount, 2u);

    renderer.reportActiveResourceWork = false;
    const auto becameInactive = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(becameInactive.has_value());
    ASSERT_EQ(becameInactive->status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(renderer.pumpCount, 3u);

    service.SetGpuPreviewResourcePendingAgeForTesting(
        request,
        std::chrono::seconds(121));
    EXPECT_FALSE(service.GenerateNextThumbnail(renderer, true).has_value());
    EXPECT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(renderer.pumpCount, 3u)
        << "Inactive resource work must still expire without an extra renderer pump.";

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailCacheTests, VisibleQueuedRequestExpiresWithRetainedCanonicalAndNewRevisionRetry)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "Visible request deadline timing is only shortened in test-hook builds.";
#else
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto previous = MakeThumbnailRequest(root, "texture:VisibleTimeout", "source:v1");
    previous.kind = AssetThumbnailKind::Texture;
    previous.sourceAssetPath = "Assets/Textures/VisibleTimeout.png";
    previous.requestRevision = 1u;
    const auto previousEntry = ResolveAssetThumbnailCacheEntry(previous);
    ASSERT_TRUE(previousEntry.has_value());
    ASSERT_TRUE(WriteAssetThumbnailCacheFile(previous, previousEntry->imagePath, TinyPng()));
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(previous, AssetThumbnailCacheStatus::Fresh, {}));
    ASSERT_TRUE(CommitAssetThumbnailPresentation(previous, *previousEntry, previous.requestRevision));

    auto request = previous;
    request.freshnessInputs.front().stamp = "source:v2";
    request.requestRevision = 2u;

    AssetThumbnailService service;
    const auto queued = service.GetThumbnail(request);
    ASSERT_EQ(queued.status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(queued.presentationState, ThumbnailPresentationState::StaleRefreshing);
    ASSERT_TRUE(queued.retainedImage.has_value());
    EXPECT_EQ(queued.retainedImage->cacheKey, previousEntry->cacheKey);
    ASSERT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);

    service.SetVisibleThumbnailRequestAgeForTesting(request, std::chrono::seconds(21));
    const auto timedOut = service.GetThumbnail(request);
    EXPECT_EQ(timedOut.status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(timedOut.diagnostic, "thumbnail-visible-request-timeout");
    EXPECT_EQ(timedOut.presentationState, ThumbnailPresentationState::FailedRetained);
    EXPECT_TRUE(timedOut.failureRetained);
    ASSERT_TRUE(timedOut.retainedImage.has_value());
    EXPECT_EQ(timedOut.retainedImage->cacheKey, previousEntry->cacheKey);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Failed);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u)
        << "The terminal failure remains consumable until the UI observes it.";

    const auto completed = service.ConsumeCompletedThumbnail();
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(completed->diagnostic, "thumbnail-visible-request-timeout");

    auto newer = request;
    newer.freshnessInputs.front().stamp = "source:v3";
    newer.requestRevision = 3u;
    const auto retry = service.GetThumbnail(newer);
    EXPECT_EQ(retry.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_NE(service.GetThumbnailState(newer), ThumbnailState::Failed)
        << "A newer freshness revision must not inherit the old timeout barrier.";

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailCacheTests, VisibleDeadlineExpiresAfterQueueOwnershipIsLost)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "Visible request deadline timing is only shortened in test-hook builds.";
#else
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, "texture:LostQueueOwnership");
    request.kind = AssetThumbnailKind::Texture;
    request.sourceAssetPath = "Assets/Textures/LostQueueOwnership.png";
    request.priority = ThumbnailRequestPriority::Visible;
    request.requestRevision = 1u;

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    // Simulate a lane-transfer bookkeeping gap: the visible deadline remains,
    // but the request is temporarily absent from the queue and continuation
    // maps. It must still become a terminal result instead of being assigned a
    // fresh revision on every UI lookup.
    service.DropGpuPreviewResourceQueueOwnershipForTesting(request);
    service.SetVisibleThumbnailRequestAgeForTesting(request, std::chrono::seconds(21));

    const auto timedOut = service.GetThumbnail(request);
    EXPECT_EQ(timedOut.status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(timedOut.diagnostic, "thumbnail-visible-request-timeout");
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Failed);

    const auto completed = service.ConsumeCompletedThumbnail();
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->diagnostic, "thumbnail-visible-request-timeout");

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailCacheTests, VisibleResidentLaneAdvancesBeforeResourcePendingVisible)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto resourcePending = MakeThumbnailRequest(root, "prefab:ResourcePending");
    resourcePending.kind = AssetThumbnailKind::PrefabPreview;
    resourcePending.priority = ThumbnailRequestPriority::Visible;
    resourcePending.artifactPath = "Library/Artifacts/70/resource-pending";
    resourcePending.freshnessInputs = {{"source", "resource-pending:v1"}};
    WriteBinaryFile(root / resourcePending.artifactPath, std::vector<uint8_t>{'p'});

    auto resident = MakeThumbnailRequest(root, "prefab:Resident");
    resident.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("71717171-7171-4171-8171-717171717171"));
    resident.kind = AssetThumbnailKind::PrefabPreview;
    resident.priority = ThumbnailRequestPriority::Visible;
    resident.artifactPath = "Library/Artifacts/71/resident";
    resident.freshnessInputs = {{"source", "resident:v1"}};
    WriteBinaryFile(root / resident.artifactPath, std::vector<uint8_t>{'p'});

    const auto registry = ResidentPrefabPreviewRegistry::Create();
    const auto snapshot = std::make_shared<PreviewRenderableSnapshot>();
    registry->RegisterSnapshot("runtime:resident", "resident:v1", snapshot, 1u);
    resident.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        "runtime:resident",
        "resident:v1",
        snapshot,
        registry
    };

    MixedPendingThenReadyThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(resourcePending).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.RequestAssetPreview(resident).status, AssetThumbnailServiceStatus::Pending);

    const auto first = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(
        first->diagnostic,
        "thumbnail-gpu-preview-resources-pending|mesh=1|material=0|texture=0|truncated=0");
    ASSERT_EQ(renderer.pumpKeys.size(), 1u);
    EXPECT_EQ(renderer.pumpKeys[0], "prefab:Resident");

    const auto second = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(second.has_value());
    ASSERT_EQ(renderer.pumpKeys.size(), 2u);
    EXPECT_EQ(renderer.pumpKeys[1], "prefab:ResourcePending")
        << "After the resident candidate advances, the resource-pending visible continuation must make progress.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, VisibleResidentGateRequiresLiveSnapshot)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, "prefab:ResidentGate");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.priority = ThumbnailRequestPriority::Visible;
    request.artifactPath = "Library/Artifacts/resident-gate";
    request.freshnessInputs = {{"source", "resident-gate:v1"}};
    WriteBinaryFile(root / request.artifactPath, std::vector<uint8_t>{'p'});

    const auto registry = ResidentPrefabPreviewRegistry::Create();
    request.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        "runtime:resident-gate",
        "resident-gate:v1",
        {},
        registry
    };

    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_FALSE(service.HasQueuedVisibleResidentThumbnail())
        << "An identity-only request must remain behind scene-load resource work.";

    registry->RegisterSnapshot(
        "runtime:resident-gate",
        "resident-gate:v1",
        std::make_shared<PreviewRenderableSnapshot>(),
        1u);
    EXPECT_TRUE(service.HasQueuedVisibleResidentThumbnail())
        << "A late scene registration must make the exact queued request resident-eligible.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, CompleteResidentSnapshotRecoversVisibleTimeout)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "Visible request deadline timing is only shortened in test-hook builds.";
#else
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, "prefab:ResidentTimeoutRecovery");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.priority = ThumbnailRequestPriority::Visible;
    request.artifactPath = "Library/Artifacts/resident-timeout-recovery";
    request.freshnessInputs = {{"source", "resident-timeout-recovery:v1"}};
    WriteBinaryFile(root / request.artifactPath, std::vector<uint8_t>{'p'});

    const auto registry = ResidentPrefabPreviewRegistry::Create();
    auto partialSnapshot = std::make_shared<PreviewRenderableSnapshot>();
    partialSnapshot->drawItems.resize(1u);
    partialSnapshot->expectedDrawItemCount = 2u;
    registry->RegisterSnapshot(
        "runtime:resident-timeout-recovery",
        "resident-timeout-recovery:v1",
        partialSnapshot,
        1u);
    request.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        "runtime:resident-timeout-recovery",
        "resident-timeout-recovery:v1",
        partialSnapshot,
        registry
    };

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    service.SetVisibleThumbnailRequestAgeForTesting(request, std::chrono::seconds(21));
    const auto timedOut = service.GetThumbnail(request);
    ASSERT_EQ(timedOut.status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(timedOut.diagnostic, "thumbnail-visible-request-timeout");
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Failed);

    auto completeSnapshot = std::make_shared<PreviewRenderableSnapshot>();
    completeSnapshot->drawItems.resize(2u);
    completeSnapshot->expectedDrawItemCount = 2u;
    registry->RegisterSnapshot(
        "runtime:resident-timeout-recovery",
        "resident-timeout-recovery:v1",
        completeSnapshot,
        2u);

    const auto recovered = service.GetThumbnail(request);
    EXPECT_EQ(recovered.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_NE(service.GetThumbnailState(request), ThumbnailState::Failed);
    EXPECT_TRUE(service.HasQueuedReadyResidentThumbnail());
    service.MaintainPendingThumbnailRequests();
    EXPECT_NE(service.GetThumbnailState(request), ThumbnailState::Failed);

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailCacheTests, CompleteResidentSnapshotReactivatesDeferredLargePrefab)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, "prefab:DeferredLargeResidentRecovery");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.priority = ThumbnailRequestPriority::Visible;
    request.artifactPath = "Library/Artifacts/deferred-large-resident-recovery";
    request.freshnessInputs = {{"source", "deferred-large-resident-recovery:v1"}};
    WriteBinaryFile(root / request.artifactPath, std::vector<uint8_t>{'p'});
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(
        request,
        AssetThumbnailCacheStatus::Failed,
        "thumbnail-prefab-preview-awaiting-resident-load"));

    AssetThumbnailService service;
    const auto cold = service.GetThumbnail(request);
    ASSERT_EQ(cold.status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_EQ(cold.diagnostic, "thumbnail-prefab-preview-awaiting-resident-load");
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);

    const auto registry = ResidentPrefabPreviewRegistry::Create();
    auto completeSnapshot = std::make_shared<PreviewRenderableSnapshot>();
    completeSnapshot->drawItems.resize(405u);
    completeSnapshot->expectedDrawItemCount = completeSnapshot->drawItems.size();
    registry->RegisterSnapshot(
        "runtime:deferred-large-resident-recovery",
        "deferred-large-resident-recovery:v1",
        completeSnapshot,
        completeSnapshot->drawItems.size());
    request.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        "runtime:deferred-large-resident-recovery",
        "deferred-large-resident-recovery:v1",
        completeSnapshot,
        registry
    };

    const auto recovered = service.GetThumbnail(request);
    EXPECT_EQ(recovered.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_NE(service.GetThumbnailState(request), ThumbnailState::Failed);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_TRUE(service.HasQueuedReadyResidentThumbnail())
        << "A complete scene package must reactivate the deferred thumbnail without artifact reload.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ResidentThumbnailWakeRevisionTracksUsableStateTransitions)
{
    using namespace NLS::Editor::Assets;

    const auto registry = ResidentPrefabPreviewRegistry::Create();
    constexpr const char* identity = "runtime:thumbnail-wake";
    constexpr const char* freshness = "thumbnail-wake:v1";
    const auto initialRevision = registry->GetThumbnailWakeRevision();

    auto partial = std::make_shared<PreviewRenderableSnapshot>();
    partial->drawItems.resize(1u);
    partial->expectedDrawItemCount = 3u;
    registry->RegisterSnapshot(identity, freshness, partial, 1u);
    const auto registeredRevision = registry->GetThumbnailWakeRevision();
    EXPECT_GT(registeredRevision, initialRevision);

    auto richerPartial = std::make_shared<PreviewRenderableSnapshot>();
    richerPartial->drawItems.resize(2u);
    richerPartial->expectedDrawItemCount = 3u;
    registry->RegisterSnapshot(identity, freshness, richerPartial, 2u);
    EXPECT_EQ(registry->GetThumbnailWakeRevision(), registeredRevision)
        << "Progressive partial snapshots must not restart the Asset Browser scope every frame.";

    auto complete = std::make_shared<PreviewRenderableSnapshot>();
    complete->drawItems.resize(3u);
    complete->expectedDrawItemCount = 3u;
    registry->RegisterSnapshot(identity, freshness, complete, 3u);
    const auto topologyRevision = registry->GetThumbnailWakeRevision();
    EXPECT_GT(topologyRevision, registeredRevision);

    auto incompleteResources = std::make_shared<ResidentPrefabPreviewResources>();
    incompleteResources->drawItems.resize(3u);
    incompleteResources->sourceExpectedDrawItemCount = 3u;
    incompleteResources->hasUnresolvedTextureBindings = true;
    registry->RegisterSnapshot(
        identity,
        freshness,
        complete,
        3u,
        false,
        {},
        {},
        incompleteResources);
    EXPECT_EQ(registry->GetThumbnailWakeRevision(), topologyRevision);

    auto completeResources = std::make_shared<ResidentPrefabPreviewResources>();
    completeResources->drawItems.resize(3u);
    completeResources->sourceExpectedDrawItemCount = 3u;
    registry->RegisterSnapshot(
        identity,
        freshness,
        complete,
        3u,
        false,
        {},
        {},
        completeResources);
    const auto resourcesRevision = registry->GetThumbnailWakeRevision();
    EXPECT_GT(resourcesRevision, topologyRevision);

    registry->Remove(identity, freshness);
    EXPECT_GT(registry->GetThumbnailWakeRevision(), resourcesRevision);
}

TEST(AssetThumbnailCacheTests, ImportedPrefabSnapshotPublishesCompleteModelAliases)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("18181818-1818-4818-8818-181818181818"));
    const auto sourcePath = std::string("Assets/Models/ImportedResident.gltf");
    const auto artifactPath = std::string("Library/Artifacts/18/imported-resident");
    WriteBinaryFile(root / sourcePath, std::vector<uint8_t>{'g'});
    WriteBinaryFile(root / artifactPath, std::vector<uint8_t>{'p'});

    auto snapshot = std::make_shared<PreviewRenderableSnapshot>();
    PreviewDrawItem drawItem;
    drawItem.meshPath = "Library/Artifacts/18/imported-mesh";
    snapshot->drawItems.push_back(std::move(drawItem));
    snapshot->expectedDrawItemCount = snapshot->drawItems.size();

    auto preparedMeshPayload = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{1u, 2u, 3u, 4u});
    std::weak_ptr<const std::vector<uint8_t>> preparedMeshPayloadLifetime =
        preparedMeshPayload;

    const auto registry = ResidentPrefabPreviewRegistry::Create();
    ASSERT_TRUE(registry->RegisterImportedPrefabSnapshot(
        root,
        assetId,
        sourcePath,
        "prefab:ImportedResident",
        artifactPath,
        snapshot,
        {{snapshot->drawItems.front().meshPath, preparedMeshPayload}}));
    preparedMeshPayload.reset();
    EXPECT_FALSE(preparedMeshPayloadLifetime.expired());

    const auto runtimeIdentity = BuildResidentPrefabRuntimeCacheIdentity(
        assetId.ToString(),
        "prefab:ImportedResident");
    const auto canonicalFreshness = BuildPrefabThumbnailDependencyStamp(
        root,
        assetId,
        sourcePath,
        "prefab:ImportedResident",
        artifactPath);
    const auto state = registry->GetSnapshotState(runtimeIdentity, canonicalFreshness);
    ASSERT_TRUE(state.has_value());
    EXPECT_TRUE(state->complete);
    EXPECT_TRUE(state->allowArtifactResourceLoading);
    EXPECT_EQ(state->readyDrawItemCount, 1u);
    EXPECT_EQ(state->expectedDrawItemCount, 1u);

    const auto modelFreshness = BuildPrefabThumbnailDependencyStamp(
        root,
        assetId,
        sourcePath,
        "model:ImportedResident",
        artifactPath);
    auto modelLease = registry->Acquire(runtimeIdentity, modelFreshness, true);
    ASSERT_TRUE(modelLease.has_value());
    ASSERT_NE(modelLease->Snapshot(), nullptr);
    EXPECT_EQ(modelLease->Snapshot()->drawItems.size(), 1u);
    const auto residentBytesBeforePayloadTake = registry->GetStats().residentBytes;
    auto takenPayload = modelLease->TakePreparedMeshPayload(
        (root / snapshot->drawItems.front().meshPath).generic_string());
    ASSERT_NE(takenPayload, nullptr);
    EXPECT_EQ(*takenPayload, (std::vector<uint8_t>{1u, 2u, 3u, 4u}));
    EXPECT_EQ(
        registry->GetStats().residentBytes + takenPayload->size(),
        residentBytesBeforePayloadTake);
    EXPECT_EQ(
        modelLease->TakePreparedMeshPayload(snapshot->drawItems.front().meshPath),
        nullptr) << "Import mesh bytes are a one-shot handoff, not a second cache.";
    takenPayload.reset();
    EXPECT_TRUE(preparedMeshPayloadLifetime.expired());

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ImportedPrefabThumbnailContinuationIsProcessLocal)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("28282828-2828-4828-8828-282828282828"));
    const auto sourcePath = std::string("Assets/Models/ImportedContinuation.fbx");
    const auto artifactPath = std::string(
        "Library/Artifacts/28/imported-continuation.nprefab");
    WriteBinaryFile(root / sourcePath, std::vector<uint8_t>{'f', 'b', 'x'});
    WriteBinaryFile(root / artifactPath, std::vector<uint8_t>{'p'});

    auto snapshot = std::make_shared<PreviewRenderableSnapshot>();
    snapshot->drawItems.resize(kMaxColdGpuPreviewPrefabDrawItems + 1u);
    snapshot->expectedDrawItemCount = snapshot->drawItems.size();
    const auto importingRegistry = ResidentPrefabPreviewRegistry::Create();
    ASSERT_TRUE(importingRegistry->RegisterImportedPrefabSnapshot(
        root,
        assetId,
        sourcePath,
        "prefab:ImportedContinuation",
        artifactPath,
        snapshot));
    const auto imported = importingRegistry->GetImportedPrefabThumbnailContinuations(root);
    ASSERT_EQ(imported.size(), 1u);
    EXPECT_NE(imported.front().registrationRevision, 0u);

    AssetBrowserItem item;
    item.sourceAssetPath = sourcePath;
    item.projectRelativePath = sourcePath;
    item.absolutePath = root / sourcePath;
    item.kind = AssetBrowserItemKind::SourceAsset;
    item.type = AssetBrowserItemType::Model;
    item.assetId = assetId;
    item.subAssetKey = "prefab:ImportedContinuation";
    item.artifactPath = artifactPath;
    item.artifactType = NLS::Core::Assets::ArtifactType::Prefab;
    AssetThumbnailRequestBuildContext context;
    context.residentPrefabPreviewRegistry = importingRegistry;
    const auto importedRequest = BuildAssetThumbnailRequestForItem(root, item, 96u, context);
    ASSERT_TRUE(importedRequest.has_value());
    EXPECT_TRUE(importedRequest->importedPrefabThumbnailContinuation);
    EXPECT_EQ(
        importedRequest->importedPrefabThumbnailContinuationRevision,
        imported.front().registrationRevision);
    ASSERT_TRUE(importedRequest->residentPrefabPreviewSource.has_value());
    EXPECT_TRUE(importedRequest->residentPrefabPreviewSource->allowArtifactResourceLoading);
    EXPECT_FALSE(importedRequest->residentPrefabPreviewSource->snapshot.expired());

    const auto restartedRegistry = ResidentPrefabPreviewRegistry::Create();
    EXPECT_TRUE(restartedRegistry->GetImportedPrefabThumbnailContinuations(root).empty());
    context.residentPrefabPreviewRegistry = restartedRegistry;
    const auto restartedRequest = BuildAssetThumbnailRequestForItem(root, item, 96u, context);
    ASSERT_TRUE(restartedRequest.has_value());
    EXPECT_FALSE(restartedRequest->importedPrefabThumbnailContinuation);
    EXPECT_EQ(restartedRequest->importedPrefabThumbnailContinuationRevision, 0u);
    ASSERT_TRUE(restartedRequest->residentPrefabPreviewSource.has_value());
    EXPECT_FALSE(restartedRequest->residentPrefabPreviewSource->allowArtifactResourceLoading);
    EXPECT_TRUE(restartedRequest->residentPrefabPreviewSource->snapshot.expired())
        << "A restarted editor must wait for a scene load or a new import instead of cold-loading a large Prefab.";

    importingRegistry->CompleteImportedPrefabThumbnailContinuation(root, assetId);
    EXPECT_TRUE(importingRegistry->GetImportedPrefabThumbnailContinuations(root).empty());

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, PendingImportedThumbnailsRetainTopologyUnderInactiveBudget)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto firstAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("38383838-3838-4838-8838-383838383838"));
    const auto secondAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("48484848-4848-4848-8848-484848484848"));
    const std::string firstSource = "Assets/Models/FirstPending.fbx";
    const std::string secondSource = "Assets/Models/SecondPending.fbx";
    const std::string firstArtifact = "Library/Artifacts/38/first-pending";
    const std::string secondArtifact = "Library/Artifacts/48/second-pending";
    WriteBinaryFile(root / firstSource, std::vector<uint8_t>{'1'});
    WriteBinaryFile(root / secondSource, std::vector<uint8_t>{'2'});
    WriteBinaryFile(root / firstArtifact, std::vector<uint8_t>{'a'});
    WriteBinaryFile(root / secondArtifact, std::vector<uint8_t>{'b'});

    const auto makeSnapshot = [](const std::string& meshPath)
    {
        auto snapshot = std::make_shared<PreviewRenderableSnapshot>();
        PreviewDrawItem drawItem;
        drawItem.meshPath = meshPath;
        snapshot->drawItems.push_back(std::move(drawItem));
        snapshot->expectedDrawItemCount = 1u;
        return snapshot;
    };
    const auto firstSnapshot = makeSnapshot("Library/Artifacts/38/first-mesh");
    const auto secondSnapshot = makeSnapshot("Library/Artifacts/48/second-mesh");
    auto firstPayload = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>(32u, 1u));
    auto secondPayload = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>(32u, 2u));
    std::weak_ptr<const std::vector<uint8_t>> firstPayloadLifetime = firstPayload;
    std::weak_ptr<const std::vector<uint8_t>> secondPayloadLifetime = secondPayload;

    const auto registry = ResidentPrefabPreviewRegistry::Create(1u);
    ASSERT_TRUE(registry->RegisterImportedPrefabSnapshot(
        root,
        firstAssetId,
        firstSource,
        "prefab:FirstPending",
        firstArtifact,
        firstSnapshot,
        {{firstSnapshot->drawItems.front().meshPath, firstPayload}}));
    firstPayload.reset();
    EXPECT_FALSE(firstPayloadLifetime.expired());

    ASSERT_TRUE(registry->RegisterImportedPrefabSnapshot(
        root,
        secondAssetId,
        secondSource,
        "prefab:SecondPending",
        secondArtifact,
        secondSnapshot,
        {{secondSnapshot->drawItems.front().meshPath, secondPayload}}));
    secondPayload.reset();

    const auto firstIdentity = BuildResidentPrefabRuntimeCacheIdentity(
        firstAssetId.ToString(),
        "prefab:FirstPending");
    const auto secondIdentity = BuildResidentPrefabRuntimeCacheIdentity(
        secondAssetId.ToString(),
        "prefab:SecondPending");
    const auto firstFreshness = BuildPrefabThumbnailDependencyStamp(
        root, firstAssetId, firstSource, "prefab:FirstPending", firstArtifact);
    const auto secondFreshness = BuildPrefabThumbnailDependencyStamp(
        root, secondAssetId, secondSource, "prefab:SecondPending", secondArtifact);

    EXPECT_TRUE(registry->GetSnapshotState(firstIdentity, firstFreshness).has_value())
        << "The older import must retain its complete graph until thumbnail completion.";
    EXPECT_TRUE(registry->GetSnapshotState(secondIdentity, secondFreshness).has_value());
    EXPECT_TRUE(firstPayloadLifetime.expired())
        << "Older prepared bytes should be trimmed before its complete graph is evicted.";
    EXPECT_FALSE(secondPayloadLifetime.expired())
        << "The newest protected import may still hand its prepared bytes to the renderer.";

    registry->CompleteImportedPrefabThumbnailContinuation(root, firstAssetId);
    EXPECT_FALSE(registry->GetSnapshotState(firstIdentity, firstFreshness).has_value());
    EXPECT_TRUE(registry->GetSnapshotState(secondIdentity, secondFreshness).has_value());

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, LargePrefabRequiresResidentSnapshotAfterRestart)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect the large Prefab admission policy.";
#else
    using namespace NLS::Editor::Assets;

    const auto largeDrawItemCount = kMaxColdGpuPreviewPrefabDrawItems + 1u;
    EXPECT_TRUE(ShouldDeferLargePrefabPreviewUntilResidentForTesting(
        largeDrawItemCount,
        false));
    EXPECT_FALSE(ShouldDeferLargePrefabPreviewUntilResidentForTesting(
        largeDrawItemCount,
        true))
        << "Only a real scene/import resident snapshot may bypass the large Prefab gate.";
#endif
}

TEST(AssetThumbnailCacheTests, NewImportContinuationRetriesOlderResourceTimeoutOnce)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, "prefab:ImportedTimeoutRecovery");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.priority = ThumbnailRequestPriority::Background;
    request.artifactPath = "Library/Artifacts/28/imported-timeout-recovery";
    WriteBinaryFile(root / request.sourceAssetPath, std::vector<uint8_t>{'f'});
    WriteBinaryFile(root / request.artifactPath, std::vector<uint8_t>{'p'});
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(
        request,
        AssetThumbnailCacheStatus::Failed,
        "thumbnail-gpu-preview-resources-timeout:thumbnail-resource-continuation-deadline"));

    AssetThumbnailService service;
    const auto oldFailure = service.GetThumbnail(request);
    ASSERT_EQ(oldFailure.status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);

    request.importedPrefabThumbnailContinuation = true;
    request.importedPrefabThumbnailContinuationRevision = 7u;
    const auto reimported = service.GetThumbnail(request);
    EXPECT_EQ(reimported.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

    service.ClearQueuedRequests();
    const auto sameImportFailure = service.GetThumbnail(request);
    EXPECT_EQ(sameImportFailure.status, AssetThumbnailServiceStatus::Failed)
        << "The same import registration must not turn a new terminal failure into a retry loop.";
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);

    request.importedPrefabThumbnailContinuationRevision = 8u;
    const auto nextReimport = service.GetThumbnail(request);
    EXPECT_EQ(nextReimport.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u)
        << "A later explicit reimport must receive one new recovery admission.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ImportedPrefabThumbnailContinuationsLoadOneAssetAtATime)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto first = MakeThumbnailRequest(root, "prefab:ImportedFirst");
    first.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("29292929-2929-4929-8929-292929292929"));
    first.sourceAssetPath = "Assets/Models/ImportedFirst.fbx";
    first.artifactPath =
        "Library/Artifacts/29/2929292929292929292929292929292929292929292929292929292929292929";
    first.kind = AssetThumbnailKind::PrefabPreview;
    first.priority = ThumbnailRequestPriority::Visible;
    first.importedPrefabThumbnailContinuation = true;

    auto second = first;
    second.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("30303030-3030-4030-8030-303030303030"));
    second.sourceAssetPath = "Assets/Models/ImportedSecond.fbx";
    second.subAssetKey = "prefab:ImportedSecond";
    second.artifactPath =
        "Library/Artifacts/30/3030303030303030303030303030303030303030303030303030303030303030";
    second.freshnessInputs = {
        {"source", "imported-second:v1"},
        {"artifact", "imported-second-artifact:v1"}
    };
    WriteBinaryFile(root / first.sourceAssetPath, std::vector<uint8_t>{'f'});
    WriteBinaryFile(root / first.artifactPath, std::vector<uint8_t>{'p'});
    WriteBinaryFile(root / second.sourceAssetPath, std::vector<uint8_t>{'f'});
    WriteBinaryFile(root / second.artifactPath, std::vector<uint8_t>{'p'});

    ResourcesPendingThumbnailPreviewRenderer renderer;
    renderer.diagnostic =
        "thumbnail-gpu-preview-resources-pending|mesh=1|material=0|texture=0|truncated=1";
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(first).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetThumbnail(second).status, AssetThumbnailServiceStatus::Pending);

    std::vector<std::string> diagnostics;
    for (size_t attempt = 0u; attempt < 8u; ++attempt)
    {
        const auto generated = service.GenerateNextThumbnail(renderer, true);
        if (generated.has_value())
            diagnostics.push_back(generated->diagnostic);
    }

    ASSERT_GE(renderer.pumpKeys.size(), 2u)
        << "diagnostics=" << testing::PrintToString(diagnostics)
        << " firstState=" << static_cast<int>(service.GetThumbnailState(first))
        << " secondState=" << static_cast<int>(service.GetThumbnailState(second));
    EXPECT_TRUE(std::all_of(
        renderer.pumpKeys.begin(),
        renderer.pumpKeys.end(),
        [&](const std::string& key)
        {
            return key == renderer.pumpKeys.front();
        })) << "A second imported Prefab must not enter resource preparation while the first is pending.";
    EXPECT_EQ(service.GetQueuedRequestCount(), 2u);

    service.ClearQueuedRequests();
    ASSERT_EQ(service.GetThumbnail(second).status, AssetThumbnailServiceStatus::Pending);
    const auto previousPumpCount = renderer.pumpKeys.size();
    ASSERT_TRUE(service.GenerateNextThumbnail(renderer, true).has_value());
    ASSERT_GT(renderer.pumpKeys.size(), previousPumpCount);
    EXPECT_EQ(renderer.pumpKeys.back(), second.subAssetKey)
        << "Clearing the generation must release the imported-continuation owner.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ImportedPrefabContinuationWaitsForRendererAdmissionBeforeDeadline)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "Resource deadline timing requires test hooks.";
#else
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto first = MakeThumbnailRequest(root, "prefab:ImportedAdmissionFirst");
    first.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("33333333-3333-4333-8333-333333333333"));
    first.sourceAssetPath = "Assets/Models/ImportedAdmissionFirst.fbx";
    first.artifactPath =
        "Library/Artifacts/33/3333333333333333333333333333333333333333333333333333333333333333";
    first.kind = AssetThumbnailKind::PrefabPreview;
    first.priority = ThumbnailRequestPriority::Visible;
    first.importedPrefabThumbnailContinuation = true;

    auto second = first;
    second.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("34343434-3434-4434-8434-343434343434"));
    second.sourceAssetPath = "Assets/Models/ImportedAdmissionSecond.fbx";
    second.subAssetKey = "prefab:ImportedAdmissionSecond";
    second.artifactPath =
        "Library/Artifacts/34/3434343434343434343434343434343434343434343434343434343434343434";
    second.freshnessInputs = {
        {"source", "imported-admission-second:v1"},
        {"artifact", "imported-admission-second-artifact:v1"}};
    WriteBinaryFile(root / first.sourceAssetPath, std::vector<uint8_t>{'f'});
    WriteBinaryFile(root / first.artifactPath, std::vector<uint8_t>{'p'});
    WriteBinaryFile(root / second.sourceAssetPath, std::vector<uint8_t>{'f'});
    WriteBinaryFile(root / second.artifactPath, std::vector<uint8_t>{'p'});

    ResourcesPendingThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(first).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetThumbnail(second).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_TRUE(service.GenerateNextThumbnail(renderer, true).has_value());
    ASSERT_EQ(renderer.pumpCount, 1u);

    service.SetGpuPreviewResourceRequestStartAgeForTesting(
        second,
        std::chrono::seconds(121));
    const auto continuedFirst = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(continuedFirst.has_value());
    EXPECT_EQ(continuedFirst->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetThumbnailState(second), ThumbnailState::Queued);
    EXPECT_EQ(service.GetThumbnail(second).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(renderer.pumpKeys.size(), 2u);
    EXPECT_EQ(renderer.pumpKeys[0], renderer.pumpKeys[1])
        << "An imported Prefab waiting for the renderer owner must not expire before its first pump.";

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailCacheTests, CompletedImportedPrefabWaitsForFreshPersistenceBeforeNextAsset)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto first = MakeThumbnailRequest(root, "prefab:ImportedOwnerFirst");
    first.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("31313131-3131-4131-8131-313131313131"));
    first.sourceAssetPath = "Assets/Models/ImportedOwnerFirst.fbx";
    first.artifactPath =
        "Library/Artifacts/31/3131313131313131313131313131313131313131313131313131313131313131";
    first.kind = AssetThumbnailKind::PrefabPreview;
    first.priority = ThumbnailRequestPriority::Visible;
    first.importedPrefabThumbnailContinuation = true;

    auto second = first;
    second.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("32323232-3232-4232-8232-323232323232"));
    second.sourceAssetPath = "Assets/Models/ImportedOwnerSecond.fbx";
    second.subAssetKey = "prefab:ImportedOwnerSecond";
    second.artifactPath =
        "Library/Artifacts/32/3232323232323232323232323232323232323232323232323232323232323232";
    second.freshnessInputs = {
        {"source", "imported-owner-second:v1"},
        {"artifact", "imported-owner-second-artifact:v1"}
    };
    WriteBinaryFile(root / first.sourceAssetPath, std::vector<uint8_t>{'f'});
    WriteBinaryFile(root / first.artifactPath, std::vector<uint8_t>{'p'});
    WriteBinaryFile(root / second.sourceAssetPath, std::vector<uint8_t>{'f'});
    WriteBinaryFile(root / second.artifactPath, std::vector<uint8_t>{'p'});

    MixedPendingThenReadyThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(first).status, AssetThumbnailServiceStatus::Pending);
    auto firstAlias = first;
    firstAlias.requestedSize = 128u;
    ASSERT_EQ(service.GetThumbnail(firstAlias).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetThumbnail(second).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetQueuedRequestCount(), 3u);

    ASSERT_TRUE(service.GenerateNextThumbnail(renderer, true).has_value());
    ASSERT_TRUE(service.GenerateNextThumbnail(renderer, true).has_value());
    ASSERT_TRUE(service.GenerateNextThumbnail(renderer, true).has_value());

    ASSERT_GE(renderer.pumpKeys.size(), 3u);
    EXPECT_TRUE(std::all_of(
        renderer.pumpKeys.begin(),
        renderer.pumpKeys.end(),
        [&](const std::string& key)
        {
            return key == renderer.pumpKeys.front();
        })) << "The next imported asset must wait while the first asset's PNG is still being persisted.";
    ASSERT_FALSE(renderer.releasedKeys.empty());
    EXPECT_TRUE(std::all_of(
        renderer.releasedKeys.begin(),
        renderer.releasedKeys.end(),
        [&](const std::string& key)
        {
            return key == renderer.pumpKeys.front();
        })) << "Only completed readback inputs for the active asset may be released.";

    std::optional<AssetThumbnailServiceResult> persisted;
    for (size_t attempt = 0u; attempt < 200u; ++attempt)
    {
        persisted = service.ConsumeCompletedThumbnail(false);
        if (persisted.has_value() &&
            persisted->status == AssetThumbnailServiceStatus::Fresh)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(persisted.has_value());
    ASSERT_EQ(persisted->status, AssetThumbnailServiceStatus::Fresh);

    const auto previousPumpCount = renderer.pumpKeys.size();
    for (size_t attempt = 0u; attempt < 8u &&
        renderer.pumpKeys.size() == previousPumpCount; ++attempt)
    {
        (void)service.GenerateNextThumbnail(renderer, true);
    }
    ASSERT_GT(renderer.pumpKeys.size(), previousPumpCount);
    EXPECT_EQ(renderer.pumpKeys.back(), second.subAssetKey)
        << "A durably completed asset's aliases must yield to the next imported asset.";

    service.Shutdown();
    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, CompleteResidentSnapshotRecoversAfterOwnerTablesAreCleared)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to simulate an owner-table reset.";
#else
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, "prefab:ResidentOwnerRecovery");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.priority = ThumbnailRequestPriority::Visible;
    request.artifactPath = "Library/Artifacts/resident-owner-recovery";
    request.freshnessInputs = {{"source", "resident-owner-recovery:v1"}};
    WriteBinaryFile(root / request.artifactPath, std::vector<uint8_t>{'p'});

    const auto registry = ResidentPrefabPreviewRegistry::Create();
    auto partialSnapshot = std::make_shared<PreviewRenderableSnapshot>();
    partialSnapshot->drawItems.resize(1u);
    partialSnapshot->expectedDrawItemCount = 2u;
    registry->RegisterSnapshot(
        "runtime:resident-owner-recovery",
        "resident-owner-recovery:v1",
        partialSnapshot,
        1u);
    request.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        "runtime:resident-owner-recovery",
        "resident-owner-recovery:v1",
        partialSnapshot,
        registry
    };

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    service.DropGpuPreviewResourcePendingOwnershipForTesting(request);
    service.DropGpuPreviewResourceQueueOwnershipForTesting(request);
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);

    auto completeSnapshot = std::make_shared<PreviewRenderableSnapshot>();
    completeSnapshot->drawItems.resize(2u);
    completeSnapshot->expectedDrawItemCount = 2u;
    registry->RegisterSnapshot(
        "runtime:resident-owner-recovery",
        "resident-owner-recovery:v1",
        completeSnapshot,
        2u);

    EXPECT_TRUE(service.HasQueuedReadyResidentThumbnail());
    EXPECT_GT(service.GetQueuedRequestCount(), 0u);

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailCacheTests, GpuPumpTreatsDetailedResourcePendingDiagnosticAsWaitingForResources)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();

    auto request = MakeThumbnailRequest(root, "prefab:Hero");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 48u;
    request.priority = ThumbnailRequestPriority::Visible;
    request.artifactPath = "Library/Artifacts/67/670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772";
    request.freshnessInputs = {{"source", "prefab:v1"}, {"artifact", "prefab-artifact:v1"}};
    WriteBinaryFile(root / request.artifactPath, std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});

    ResourcesPendingThumbnailPreviewRenderer renderer;
    renderer.diagnostic = "thumbnail-gpu-preview-resources-pending|mesh=2|material=1|texture=4|truncated=1";
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);

    const auto pending = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(pending->diagnostic, renderer.diagnostic);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::WaitingForResources);
    EXPECT_TRUE(service.HasQueuedGpuPreviewResourceContinuation())
        << "A pending-resource request must keep the heavy scheduler on the continuation lane.";
    EXPECT_NE(
        service.GetQueuedGpuPreviewResourceContinuationSummary().find("count=1"),
        std::string::npos);
    EXPECT_EQ(renderer.pumpCount, 1u);
    EXPECT_EQ(renderer.renderCount, 0u);

    const auto deferred = service.GenerateNextThumbnail(renderer, true);
    EXPECT_FALSE(deferred.has_value());
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::WaitingForResources);
    EXPECT_EQ(renderer.renderCount, 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, GpuRenderTreatsDetailedResourcePendingDiagnosticAsWaitingForResources)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();

    auto request = MakeThumbnailRequest(root, "prefab:Hero");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 48u;
    request.priority = ThumbnailRequestPriority::Visible;
    request.artifactPath = "Library/Artifacts/67/670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772";
    request.freshnessInputs = {{"source", "prefab:v1"}, {"artifact", "prefab-artifact:v1"}};
    WriteBinaryFile(root / request.artifactPath, std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});

    RenderDetailedResourcesPendingThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);

    const auto pending = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(
        pending->diagnostic,
        "thumbnail-gpu-preview-resources-pending|mesh=2|material=1|texture=4");
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::WaitingForResources)
        << "Detailed resources-pending diagnostics must keep prefab previews on the lightweight resource pump path.";
    EXPECT_EQ(renderer.pumpCount, 1u);
    EXPECT_EQ(renderer.renderCount, 1u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, SuccessfulPrefabResourcePumpUsesPreparedSubmission)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();

    auto request = MakeThumbnailRequest(root, "prefab:PreparedSubmission");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 48u;
    request.priority = ThumbnailRequestPriority::Visible;
    request.artifactPath =
        "Library/Artifacts/69/690d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772";
    request.freshnessInputs = {{"source", "prefab:v1"}, {"artifact", "prefab-artifact:v1"}};
    WriteBinaryFile(root / request.artifactPath, std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});

    PreparedSubmissionThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);

    const auto generated = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(renderer.pumpCount, 1u);
    EXPECT_EQ(renderer.preparedSubmitCount, 1u);
    EXPECT_EQ(renderer.ordinarySubmitCount, 0u);
    EXPECT_EQ(renderer.renderCount, 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, GpuRenderResourcePendingEventuallyFailsInsteadOfRemainingPending)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();

    auto request = MakeThumbnailRequest(root, "prefab:Stalled");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 48u;
    request.priority = ThumbnailRequestPriority::Visible;
    request.artifactPath = "Library/Artifacts/68/680d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772";
    request.freshnessInputs = {{"source", "prefab:v1"}, {"artifact", "prefab-artifact:v1"}};
    WriteBinaryFile(root / request.artifactPath, std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});

    RenderDetailedResourcesPendingThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);

    const auto pending = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(pending.has_value());
    ASSERT_EQ(pending->status, AssetThumbnailServiceStatus::Pending);

    service.SetGpuPreviewResourcePendingAgeForTesting(
        request,
        std::chrono::seconds(121));
    service.MaintainPendingThumbnailRequests();
    const auto terminal = service.ConsumeCompletedThumbnail(false);

    ASSERT_TRUE(terminal.has_value())
        << "A Render-stage resource-pending diagnostic must be bounded by the same wall-clock policy as PumpResources. "
        << "renderCount=" << renderer.renderCount
        << ", queuedCount=" << service.GetQueuedRequestCount();
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Failed);
    EXPECT_EQ(terminal->status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(
        terminal->diagnostic.rfind("thumbnail-gpu-preview-resources-timeout:", 0u),
        0u);
    EXPECT_EQ(renderer.renderCount, 1u);

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
    EXPECT_EQ(service.GetThumbnailState(material), ThumbnailState::Encoding);
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
    EXPECT_EQ(service.GetThumbnailState(prefab), ThumbnailState::Encoding);
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

TEST(AssetThumbnailCacheTests, GpuPumpRejectsOpaqueBlackPreviewFramesWithoutCpuFallback)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteTextFile(
        root / "Assets" / "Materials" / "BlackFrame.mat",
        "shaderLabMaterialVersion=1\n"
        "shader=Assets/Engine/Shaders/ShaderLab/StandardPBR.shader\n"
        "property _BaseColor Color 1 1 1 1\n"
        "property _Metallic Float 0\n"
        "property _Roughness Float 0.5\n");

    auto material = MakeThumbnailRequest(root, "material:BlackFrame");
    material.sourceAssetPath = "Assets/Materials/BlackFrame.mat";
    material.kind = AssetThumbnailKind::MaterialSphere;
    material.requestedSize = 48u;
    material.priority = ThumbnailRequestPriority::Visible;
    material.freshnessInputs = {{"source", "black-frame:v1"}};

    BlackFrameThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(material).status, AssetThumbnailServiceStatus::Pending);

    const auto generated = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(generated->diagnostic, "thumbnail-gpu-preview-empty-frame");
    EXPECT_EQ(service.GetThumbnailState(material), ThumbnailState::Failed);
    EXPECT_EQ(EvaluateAssetThumbnailCache(material).status, AssetThumbnailCacheStatus::Failed);
    ASSERT_TRUE(generated->cacheEntry.has_value());
    EXPECT_FALSE(std::filesystem::exists(generated->cacheEntry->imagePath));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, GpuModelPreviewDefersOpaqueBlackFrameWithoutCpuRasterFallback)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Models" / "BlackFrame.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    const auto artifactPath =
        root / LibraryArtifactPath("31bb5a71075a04ac35b0f324a6ebaeb38d80fe1f76a45048c1f03633c4314423");
    WriteBinaryFile(
        artifactPath,
        NLS::Render::Assets::SerializeMeshArtifact(TriangleMeshArtifact()));

    auto model = MakeThumbnailRequest(root, "mesh:BlackFrame");
    model.sourceAssetPath = "Assets/Models/BlackFrame.fbx";
    model.artifactPath = "Library/Artifacts/31/31bb5a71075a04ac35b0f324a6ebaeb38d80fe1f76a45048c1f03633c4314423";
    model.kind = AssetThumbnailKind::ModelPreview;
    model.requestedSize = 48u;
    model.priority = ThumbnailRequestPriority::Visible;
    model.freshnessInputs = {{"artifact", "model-black-frame:v1"}};

    BlackFrameThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(model).status, AssetThumbnailServiceStatus::Pending);

    const auto generated = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Pending)
        << "An invalid GPU frame must remain retryable instead of switching to CPU rasterization.";
    EXPECT_EQ(service.GetThumbnailState(model), ThumbnailState::Queued);
    EXPECT_NE(EvaluateAssetThumbnailCache(model).status, AssetThumbnailCacheStatus::Fresh);
    ASSERT_TRUE(generated->cacheEntry.has_value());
    EXPECT_FALSE(std::filesystem::exists(generated->cacheEntry->imagePath));
    EXPECT_FALSE(service.StartNextThumbnailGeneration())
        << "GPU-capable model previews must not enter the CPU raster worker path.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, GpuPrefabPreviewRetriesOpaqueBlackFramesWithoutCpuFallback)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("bf141414-1414-4414-8414-141414141414"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot);
    WriteBinaryFile(root / "Assets" / "Prefabs" / "BlackFrame.prefab", std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});
    WriteNativeArtifactTextFile(
        artifactRoot / "BlackFrame.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        MinimalPrefabPayload());

    auto prefab = MakeThumbnailRequest(root, "prefab:BlackFrame");
    prefab.assetId = assetId;
    prefab.sourceAssetPath = "Assets/Prefabs/BlackFrame.prefab";
    prefab.artifactPath = RedirectedArtifactPathOrFallback(
        "Library/Artifacts/" + assetId.ToString() + "/BlackFrame.nprefab");
    prefab.kind = AssetThumbnailKind::PrefabPreview;
    prefab.requestedSize = 48u;
    prefab.priority = ThumbnailRequestPriority::Visible;
    prefab.freshnessInputs = {{"artifact", "prefab-black-frame:v1"}};

    BlackFrameThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(prefab).status, AssetThumbnailServiceStatus::Pending);

    const auto generated = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Pending)
        << "Prefab thumbnails must be produced by the GPU preview renderer, but an early empty frame should "
           "not be persisted as a permanent cache failure because startup/resource ordering can make it transient.";
    EXPECT_EQ(generated->diagnostic, "thumbnail-gpu-preview-empty-frame");
    EXPECT_EQ(service.GetThumbnailState(prefab), ThumbnailState::Queued);
    EXPECT_EQ(EvaluateAssetThumbnailCache(prefab).status, AssetThumbnailCacheStatus::Missing);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, GpuPrefabPreviewEmptyFrameDoesNotHotRetryInSameGeneration)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("bf171717-1717-4717-8717-171717171717"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot);
    WriteBinaryFile(root / "Assets" / "Prefabs" / "EmptyFrame.prefab", std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});
    WriteNativeArtifactTextFile(
        artifactRoot / "EmptyFrame.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        MinimalPrefabPayload());

    auto prefab = MakeThumbnailRequest(root, "prefab:EmptyFrame");
    prefab.assetId = assetId;
    prefab.sourceAssetPath = "Assets/Prefabs/EmptyFrame.prefab";
    prefab.artifactPath = RedirectedArtifactPathOrFallback(
        "Library/Artifacts/" + assetId.ToString() + "/EmptyFrame.nprefab");
    prefab.kind = AssetThumbnailKind::PrefabPreview;
    prefab.requestedSize = 48u;
    prefab.priority = ThumbnailRequestPriority::Visible;
    prefab.freshnessInputs = {{"artifact", "prefab-empty-frame-hot-loop:v1"}};

    BlackFrameThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(prefab).status, AssetThumbnailServiceStatus::Pending);

    const auto first = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(first->diagnostic, "thumbnail-gpu-preview-empty-frame");
    EXPECT_EQ(renderer.renderCount, 1u);

    const auto duplicate = service.RequestAssetPreview(prefab);
    EXPECT_EQ(duplicate.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(duplicate.diagnostic, "thumbnail-gpu-preview-empty-frame");
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);

    const auto second = service.GenerateNextThumbnail(renderer, true);
    EXPECT_FALSE(second.has_value())
        << "A transient empty GPU prefab frame must not immediately requeue the same visible preview and monopolize the UI thumbnail pump.";
    EXPECT_EQ(renderer.renderCount, 1u);

    std::this_thread::sleep_for(std::chrono::milliseconds(5100));
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u)
        << "A deferred empty GPU frame should become eligible for retry after the cooldown.";
    const auto delayedRetry = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(delayedRetry.has_value());
    EXPECT_EQ(delayedRetry->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(delayedRetry->diagnostic, "thumbnail-gpu-preview-empty-frame");
    EXPECT_EQ(renderer.renderCount, 2u);

    service.SupersedeQueuedRequestsForGeneration("prefab-empty-frame-next-scope");
    EXPECT_EQ(service.GetThumbnailState(prefab), ThumbnailState::Cancelled);
    ASSERT_EQ(service.RequestAssetPreview(prefab).status, AssetThumbnailServiceStatus::Pending);
    const auto retried = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(retried.has_value());
    EXPECT_EQ(retried->diagnostic, "thumbnail-gpu-preview-empty-frame");
    EXPECT_EQ(renderer.renderCount, 3u)
        << "A new generation should still be allowed to retry GPU-only prefab preview generation.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, GpuPrefabPreviewKeepsSubmittedDarkFramesWithoutCpuFallback)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("bf151515-1515-4515-8515-151515151515"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot);
    WriteBinaryFile(root / "Assets" / "Prefabs" / "Dark.prefab", std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});
    WriteNativeArtifactTextFile(
        artifactRoot / "Dark.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        MinimalPrefabPayload());

    auto prefab = MakeThumbnailRequest(root, "prefab:Dark");
    prefab.assetId = assetId;
    prefab.sourceAssetPath = "Assets/Prefabs/Dark.prefab";
    prefab.artifactPath = RedirectedArtifactPathOrFallback(
        "Library/Artifacts/" + assetId.ToString() + "/Dark.nprefab");
    prefab.kind = AssetThumbnailKind::PrefabPreview;
    prefab.requestedSize = 48u;
    prefab.priority = ThumbnailRequestPriority::Visible;
    prefab.freshnessInputs = {{"artifact", "prefab-submitted-dark-frame:v1"}};

    SubmittedBlackFrameThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(prefab).status, AssetThumbnailServiceStatus::Pending);

    const auto generated = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Pending)
        << "A prefab frame with submitted scene draws can be a legitimate dark asset and must not be "
           "failed solely because its luma is low.";
    EXPECT_EQ(service.GetThumbnailState(prefab), ThumbnailState::Encoding);

    std::optional<AssetThumbnailServiceResult> written;
    for (size_t attempt = 0u; attempt < 100u && !written.has_value(); ++attempt)
    {
        written = service.ConsumeCompletedThumbnail();
        if (!written.has_value())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(written->status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(EvaluateAssetThumbnailCache(prefab).status, AssetThumbnailCacheStatus::Fresh);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, MainThreadGpuPreviewPathUsesSharedClearFrameDisposition)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Assets/AssetThumbnailService.cpp"));
    const std::string gpuPreviewNeedle =
        "std::optional<AssetThumbnailServiceResult> AssetThumbnailService::GenerateNextThumbnail(";
    std::string gpuPreviewBody;
    for (size_t searchBegin = 0u;; searchBegin += gpuPreviewNeedle.size())
    {
        const auto candidateBegin = source.find(gpuPreviewNeedle, searchBegin);
        if (candidateBegin == std::string::npos)
            break;

        const auto candidateBodyBegin = source.find('{', candidateBegin);
        ASSERT_NE(candidateBodyBegin, std::string::npos);
        const auto candidateSignature = source.substr(candidateBegin, candidateBodyBegin - candidateBegin);
        if (candidateSignature.find("IEditorThumbnailPreviewRenderer& previewRenderer") != std::string::npos)
        {
            gpuPreviewBody = ExtractFunctionBody(source.substr(candidateBegin), gpuPreviewNeedle);
            break;
        }
        searchBegin = candidateBegin;
    }
    ASSERT_FALSE(gpuPreviewBody.empty())
        << "Missing main-thread GPU preview GenerateNextThumbnail overload.";
    ASSERT_NE(gpuPreviewBody.find("EvaluateGpuPreviewClearFrameDisposition"), std::string::npos)
        << "The main-thread GPU preview pump must share the clear-frame policy covered by the prefab black-frame test.";
    ASSERT_NE(gpuPreviewBody.find("GpuPreviewClearFrameDisposition::FailEmptyFrame"), std::string::npos);
    ASSERT_NE(gpuPreviewBody.find("BuildGpuPreviewEmptyFrameResult"), std::string::npos);
    EXPECT_EQ(source.find("GenerateGpuPreviewThumbnailResult"), std::string::npos)
        << "GPU preview rendering must not be scheduled through a background worker.";
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
    EXPECT_EQ(service.GetThumbnailState(material), ThumbnailState::Encoding)
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

TEST(AssetThumbnailCacheTests, GpuReadbackPendingEventuallyFailsInsteadOfRemainingPending)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto material = MakeThumbnailRequest(root, "material:NeverReadyReadback");
    material.sourceAssetPath = "Assets/Materials/NeverReadyReadback.mat";
    material.kind = AssetThumbnailKind::MaterialSphere;
    material.requestedSize = 48u;
    material.priority = ThumbnailRequestPriority::Visible;
    material.freshnessInputs = {{"source", "never-ready-readback:v1"}};

    NeverReadyThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(material).status, AssetThumbnailServiceStatus::Pending);

    const auto pending = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(pending->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(pending->diagnostic, "thumbnail-gpu-preview-readback-pending");
    EXPECT_EQ(service.GetThumbnailState(material), ThumbnailState::WaitingForGpu);

    std::this_thread::sleep_for(std::chrono::milliseconds(5100));
    const auto timedOut = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(timedOut.has_value());
    EXPECT_EQ(timedOut->status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(timedOut->diagnostic, "thumbnail-gpu-preview-readback-timeout");
    EXPECT_TRUE(timedOut->revokeGpuTexture);
    EXPECT_EQ(service.GetThumbnailState(material), ThumbnailState::Failed);
    EXPECT_EQ(renderer.orphanCount, 1u);

    std::error_code removeError;
    std::filesystem::remove_all(root, removeError);
    EXPECT_FALSE(removeError) << removeError.message();
}

TEST(AssetThumbnailCacheTests, GpuPumpPollsExistingPendingReadbackWhenCacheWriteBudgetIsExhausted)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();

    auto material = MakeThumbnailRequest(root, "material:BudgetedCacheWrite");
    material.sourceAssetPath = "Assets/Materials/BudgetedCacheWrite.mat";
    material.kind = AssetThumbnailKind::MaterialSphere;
    material.requestedSize = 48u;
    material.priority = ThumbnailRequestPriority::Visible;
    material.freshnessInputs = {{"source", "budgeted-cache-write-material:v1"}};

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
    exhaustedBudget.cacheWriteCountBudget = 0u;
    service.SetThumbnailGenerationBudget(exhaustedBudget);

    const auto polled = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(polled.has_value());
    EXPECT_EQ(polled->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetThumbnailState(material), ThumbnailState::Persisting)
        << "Cache-write budget throttles persistence, but must not strand an already submitted GPU readback.";
    EXPECT_EQ(renderer.renderCount, 2u);

    ThumbnailGenerationBudget restoredBudget;
    restoredBudget.previewRenderCountBudget = 0u;
    restoredBudget.readbackCountBudget = 0u;
    restoredBudget.cacheWriteCountBudget = 1u;
    service.SetThumbnailGenerationBudget(restoredBudget);

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

TEST(AssetThumbnailCacheTests, DeferredGpuPersistenceWaitsForCacheWriteBudget)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto material = MakeThumbnailRequest(root, "material:DeferredPersistence");
    material.sourceAssetPath = "Assets/Materials/DeferredPersistence.mat";
    material.kind = AssetThumbnailKind::MaterialSphere;
    material.requestedSize = 48u;
    material.priority = ThumbnailRequestPriority::Visible;
    material.freshnessInputs = {{"source", "deferred-persistence-material:v1"}};

    PendingThenReadyThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ThumbnailGenerationBudget initialBudget;
    initialBudget.previewRenderCountBudget = 1u;
    initialBudget.readbackCountBudget = 1u;
    initialBudget.cacheWriteCountBudget = 1u;
    service.SetThumbnailGenerationBudget(initialBudget);
    ASSERT_EQ(service.RequestAssetPreview(material).status, AssetThumbnailServiceStatus::Pending);

    const auto submitted = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(submitted.has_value());
    EXPECT_EQ(submitted->diagnostic, "thumbnail-gpu-preview-readback-pending");

    ThumbnailGenerationBudget throttledBudget;
    throttledBudget.previewRenderCountBudget = 0u;
    throttledBudget.readbackCountBudget = 0u;
    throttledBudget.cacheWriteCountBudget = 0u;
    service.SetThumbnailGenerationBudget(throttledBudget);

    const auto completed = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(completed->diagnostic, "thumbnail-gpu-preview-cache-write-pending");
    EXPECT_EQ(service.GetThumbnailState(material), ThumbnailState::Persisting);
    EXPECT_EQ(EvaluateAssetThumbnailCache(material).status, AssetThumbnailCacheStatus::Missing);

    EXPECT_FALSE(service.ConsumeCompletedThumbnail().has_value());
    EXPECT_EQ(service.GetThumbnailState(material), ThumbnailState::Persisting);
    EXPECT_EQ(EvaluateAssetThumbnailCache(material).status, AssetThumbnailCacheStatus::Missing);

    ThumbnailGenerationBudget restoredBudget;
    restoredBudget.previewRenderCountBudget = 0u;
    restoredBudget.readbackCountBudget = 0u;
    restoredBudget.cacheWriteCountBudget = 1u;
    service.SetThumbnailGenerationBudget(restoredBudget);

    std::optional<AssetThumbnailServiceResult> written;
    for (size_t attempt = 0u; attempt < 100u && !written.has_value(); ++attempt)
    {
        written = service.ConsumeCompletedThumbnail();
        if (!written.has_value())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(written.has_value());
    ASSERT_EQ(written->status, AssetThumbnailServiceStatus::Fresh);
    ASSERT_TRUE(written->cacheEntry.has_value());
    EXPECT_TRUE(std::filesystem::is_regular_file(written->cacheEntry->imagePath));

    std::error_code removeError;
    std::filesystem::remove_all(root, removeError);
    EXPECT_FALSE(removeError) << removeError.message();
}

TEST(AssetThumbnailCacheTests, SupersededDeferredGpuPersistenceDoesNotWriteOldGeneration)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto material = MakeThumbnailRequest(root, "material:SupersededDeferredPersistence");
    material.sourceAssetPath = "Assets/Materials/SupersededDeferredPersistence.mat";
    material.kind = AssetThumbnailKind::MaterialSphere;
    material.requestedSize = 48u;
    material.priority = ThumbnailRequestPriority::Visible;
    material.freshnessInputs = {{"source", "superseded-deferred-persistence:v1"}};

    PendingThenReadyThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ThumbnailGenerationBudget initialBudget;
    initialBudget.previewRenderCountBudget = 1u;
    initialBudget.readbackCountBudget = 1u;
    initialBudget.cacheWriteCountBudget = 1u;
    service.SetThumbnailGenerationBudget(initialBudget);
    ASSERT_EQ(service.RequestAssetPreview(material).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_TRUE(service.GenerateNextThumbnail(renderer, true).has_value());

    ThumbnailGenerationBudget throttledBudget;
    throttledBudget.previewRenderCountBudget = 0u;
    throttledBudget.readbackCountBudget = 0u;
    throttledBudget.cacheWriteCountBudget = 0u;
    service.SetThumbnailGenerationBudget(throttledBudget);
    ASSERT_TRUE(service.GenerateNextThumbnail(renderer, true).has_value());
    EXPECT_EQ(service.GetThumbnailState(material), ThumbnailState::Persisting);

    service.SupersedeQueuedRequestsForGeneration("new-visible-scope");
    EXPECT_EQ(service.GetThumbnailState(material), ThumbnailState::Cancelled);

    ThumbnailGenerationBudget restoredBudget;
    restoredBudget.previewRenderCountBudget = 0u;
    restoredBudget.readbackCountBudget = 0u;
    restoredBudget.cacheWriteCountBudget = 1u;
    service.SetThumbnailGenerationBudget(restoredBudget);
    for (size_t attempt = 0u; attempt < 20u; ++attempt)
    {
        EXPECT_FALSE(service.ConsumeCompletedThumbnail().has_value());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(EvaluateAssetThumbnailCache(material).status, AssetThumbnailCacheStatus::Missing);

    std::error_code removeError;
    std::filesystem::remove_all(root, removeError);
    EXPECT_FALSE(removeError) << removeError.message();
}

TEST(AssetThumbnailCacheTests, GpuPreviewRenderTelemetryIncludesEmptyResultDiagnostic)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();

    auto request = MakeThumbnailRequest(root, "prefab:Hero");
    request.sourceAssetPath = "Assets/Models/Hero.fbx";
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.priority = ThumbnailRequestPriority::Visible;
    request.freshnessInputs = {{"source", "prefab-empty-diagnostic:v1"}};

    EmptyDiagnosticThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto generated = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(generated->diagnostic, "thumbnail-gpu-preview-resources-pending");

    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    const auto found = std::find_if(
        telemetry.begin(),
        telemetry.end(),
        [](const NLS::Core::Assets::ArtifactLoadTelemetryRecord& record)
        {
            return record.stage == NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewRender &&
                record.path.find("Assets/Models/Hero.fbx|prefab:Hero") != std::string::npos &&
                record.path.find("|diag=thumbnail-gpu-preview-resources-pending") != std::string::npos;
        });
    EXPECT_NE(found, telemetry.end())
        << "GPU preview telemetry must preserve empty-result diagnostics so real editor runs can prove why a prefab thumbnail did not publish.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, GpuPreviewPollTelemetryIncludesCompletedEmptyFrameDiagnostic)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();

    auto request = MakeThumbnailRequest(root, "prefab:HeroPendingBlack");
    request.sourceAssetPath = "Assets/Models/HeroPendingBlack.fbx";
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.priority = ThumbnailRequestPriority::Visible;
    request.freshnessInputs = {{"source", "prefab-pending-black-frame-diagnostic:v1"}};

    PendingThenBlackFrameThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();

    const auto pending = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(pending.has_value());
    ASSERT_EQ(pending->diagnostic, "thumbnail-gpu-preview-readback-pending");

    const auto completed = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(completed->diagnostic, "thumbnail-gpu-preview-empty-frame");

    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    const auto found = std::find_if(
        telemetry.begin(),
        telemetry.end(),
        [](const NLS::Core::Assets::ArtifactLoadTelemetryRecord& record)
        {
            return record.stage == NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewRender &&
                record.path.find("Assets/Models/HeroPendingBlack.fbx|prefab:HeroPendingBlack") != std::string::npos &&
                record.path.find("|diag=thumbnail-gpu-preview-empty-frame") != std::string::npos;
        });
    EXPECT_NE(found, telemetry.end())
        << "When a pending GPU readback completes into an empty frame, the telemetry must preserve that terminal diagnostic.";

    const auto queueDecision = std::find_if(
        telemetry.begin(),
        telemetry.end(),
        [](const NLS::Core::Assets::ArtifactLoadTelemetryRecord& record)
        {
            return record.stage == NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailServiceGpuPreviewQueueDecision &&
                record.path.find("completed-readback-disposition=empty-frame|Assets/Models/HeroPendingBlack.fbx|prefab:HeroPendingBlack") != std::string::npos;
        });
    EXPECT_NE(queueDecision, telemetry.end())
        << "Queue decision telemetry should say explicitly when a completed pending readback was rejected as an empty frame.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, GpuPreviewEmptyFrameTelemetryIncludesReadbackPixelAndDrawStats)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();

    auto request = MakeThumbnailRequest(root, "prefab:HeroTransparentLit");
    request.sourceAssetPath = "Assets/Models/HeroTransparentLit.fbx";
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.priority = ThumbnailRequestPriority::Visible;
    request.freshnessInputs = {{"source", "prefab-transparent-lit-diagnostic:v1"}};

    PendingThenTransparentLitFrameThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    ASSERT_TRUE(service.GenerateNextThumbnail(renderer, true).has_value());

    const auto completed = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(completed.has_value());
    ASSERT_EQ(
        completed->diagnostic,
        "thumbnail-gpu-preview-empty-frame-cpu-fallback-pending");

    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    const auto found = std::find_if(
        telemetry.begin(),
        telemetry.end(),
        [](const NLS::Core::Assets::ArtifactLoadTelemetryRecord& record)
        {
            return record.stage == NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewRender &&
                record.path.find("Assets/Models/HeroTransparentLit.fbx|prefab:HeroTransparentLit") != std::string::npos &&
                record.path.find("|pixels=4|visibleAlpha=0|litRgb=2|maxAlpha=0|maxLuma=255") != std::string::npos &&
                record.path.find("|rawVisibleDraws=4|submittedDraws=3") != std::string::npos;
        });
    EXPECT_NE(found, telemetry.end())
        << "Rejected GPU readbacks need pixel and draw statistics so editor proof can distinguish missing geometry from lost alpha.";

    std::filesystem::remove_all(root);
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
    request.settingsFingerprint = "asset-browser-thumbnail:v26-prefab-background-prepare";

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
    const ScopedAssetThumbnailCacheJobSystem jobSystem(2u);

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
    CapturingThumbnailPreviewRenderer renderer;
    const auto generated = PumpUntilDeferredPreviewResolves(service, renderer);
    ASSERT_TRUE(generated.has_value());
    ASSERT_EQ(generated->status, AssetThumbnailServiceStatus::Pending) << generated->diagnostic;
    auto completed = service.ConsumeCompletedThumbnail();
    for (int attempt = 0; attempt < 100 && !completed.has_value(); ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        completed = service.ConsumeCompletedThumbnail();
    }
    ASSERT_TRUE(completed.has_value());
    ASSERT_EQ(completed->status, AssetThumbnailServiceStatus::Fresh) << completed->diagnostic;
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
    const ScopedAssetThumbnailCacheJobSystem jobSystem(2u);

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
    CountingThumbnailPreviewRenderer renderer;
    const auto generated = PumpUntilDeferredPreviewResolves(service, renderer);
    ASSERT_TRUE(generated.has_value());
    ASSERT_EQ(generated->status, AssetThumbnailServiceStatus::Failed);
    ASSERT_EQ(generated->diagnostic, "test-renderer-called");
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

TEST(AssetThumbnailCacheTests, PrefabTransformsRemainPendingWithoutGpuRenderer)
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
    EXPECT_FALSE(generated.has_value());
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Missing);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ResidentPrefabSceneRegistrationReusesExactSnapshot)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto prefabAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("bf1a1a1a-1a1a-4a1a-8a1a-1a1a1a1a1a1a"));
    const auto meshAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("bf1b1b1b-1b1b-4b1b-8b1b-1b1b1b1b1b1b"));
    const auto artifactRoot = root / "Library" / "Artifacts" / prefabAssetId.ToString();
    const auto prefabArtifactPath = std::filesystem::path("Library") /
        "Artifacts" /
        prefabAssetId.ToString() /
        "Hero.nprefab";
    const auto prefabPayload = PrefabPayloadWithSingleRendererDependency(meshAssetId, "mesh:Body");

    std::filesystem::create_directories(artifactRoot);
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.fbx", std::vector<uint8_t> {'f', 'b', 'x'});
    WriteNativeArtifactTextFile(
        root / prefabArtifactPath,
        ArtifactType::Prefab,
        "prefab",
        1u,
        prefabPayload);

    const auto meshArtifactPath = std::filesystem::path("Library") /
        "Artifacts" /
        meshAssetId.ToString() /
        "Body.nmesh";
    NLS::Engine::Assets::PrefabResolvedAsset resolvedMesh;
    resolvedMesh.assetId = meshAssetId;
    resolvedMesh.expectedType = "Mesh";
    resolvedMesh.subAssetKey = "mesh:Body";
    resolvedMesh.artifactPath = meshArtifactPath.generic_string();
    const auto imported = NLS::Engine::Assets::ImportPrefabArtifact(
        prefabPayload,
        prefabAssetId,
        {resolvedMesh});
    ASSERT_FALSE(imported.diagnostics.HasErrors());

    auto registry = ResidentPrefabPreviewRegistry::Create();
    const auto runtimeIdentity = BuildResidentPrefabRuntimeCacheIdentity(
        prefabAssetId.ToString(),
        "prefab:Hero");
    const auto first = registry->RegisterPrefabSnapshotForScene(
        root,
        prefabAssetId,
        "Assets/Models/Hero.fbx",
        "prefab:Hero",
        prefabArtifactPath.generic_string(),
        runtimeIdentity,
        imported.artifact);
    ASSERT_TRUE(first.has_value());

    const auto second = registry->RegisterPrefabSnapshotForScene(
        root,
        prefabAssetId,
        "Assets/Models/Hero.fbx",
        "prefab:Hero",
        prefabArtifactPath.generic_string(),
        runtimeIdentity,
        imported.artifact);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first->Snapshot(), second->Snapshot())
        << "Repeated scene instantiation must lease the canonical snapshot instead of rebuilding it.";
    EXPECT_EQ(registry->GetStats().entryCount, 1u);
    EXPECT_EQ(registry->GetStats().activeLeaseCount, 2u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, PartialPrefabSnapshotRemainsPendingWithoutGpuRenderer)
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
    EXPECT_FALSE(generated.has_value());
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Missing);

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

TEST(AssetThumbnailCacheTests, GpuPrefabPreviewDefersMixedResourceReadinessUntilComplete)
{
    using namespace NLS::Editor::Assets;

#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect prefab preview readiness.";
#else
    EXPECT_TRUE(ShouldDeferPrefabPreviewForResourceReadinessForTesting(1u, 0u, 0u, false))
        << "Any pending mesh must prevent the thumbnail from caching incomplete prefab geometry.";
    EXPECT_TRUE(ShouldDeferPrefabPreviewForResourceReadinessForTesting(0u, 1u, 0u, false))
        << "Any pending material must prevent the thumbnail from caching a white prefab preview.";
    EXPECT_TRUE(ShouldDeferPrefabPreviewForResourceReadinessForTesting(0u, 0u, 1u, false))
        << "Any pending material texture must prevent the thumbnail from caching a white prefab preview.";
    EXPECT_TRUE(ShouldDeferPrefabPreviewForResourceReadinessForTesting(0u, 0u, 0u, true))
        << "Truncated resource discovery cannot prove that the complete prefab is ready.";
    EXPECT_FALSE(ShouldDeferPrefabPreviewForResourceReadinessForTesting(0u, 0u, 0u, false));
#endif
}

TEST(AssetThumbnailCacheTests, GpuPrefabPreviewContinuesRenderingAfterDrawPrewarmCompletes)
{
    using namespace NLS::Editor::Assets;

#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect GPU thumbnail draw prewarming.";
#else
    EXPECT_TRUE(ShouldDeferPrefabPreviewAfterDrawPrewarmForTesting(true, false))
        << "An incomplete supported prewarm must yield so the remaining draws stay within the frame budget.";
    EXPECT_FALSE(ShouldDeferPrefabPreviewAfterDrawPrewarmForTesting(true, true))
        << "A completed prewarm must fall through to the real render instead of retrying forever.";
    EXPECT_FALSE(ShouldDeferPrefabPreviewAfterDrawPrewarmForTesting(false, false))
        << "An unsupported prewarm path must fall through to the renderer's normal preparation path.";
#endif
}

TEST(AssetThumbnailCacheTests, GpuPrefabPreviewUsesCompleteResidentSceneWithoutDrawPrewarm)
{
    using namespace NLS::Editor::Assets;

#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect resident GPU thumbnail preparation.";
#else
    EXPECT_TRUE(ShouldSkipPrefabPreviewDrawPrewarmForResidentForTesting(true, true))
        << "A complete resident resource package is already draw-ready and must render immediately.";
    EXPECT_FALSE(ShouldSkipPrefabPreviewDrawPrewarmForResidentForTesting(true, false))
        << "A partial resident package must keep its provisional path and resource continuation.";
    EXPECT_FALSE(ShouldSkipPrefabPreviewDrawPrewarmForResidentForTesting(false, true))
        << "Without a resident snapshot the canonical cold preview still needs draw prewarming.";
#endif
}

TEST(AssetThumbnailCacheTests, ResidentResourceCompletionReusesUnchangedPrefabSceneAssembly)
{
    using namespace NLS::Editor::Assets;

    PreviewRenderableSnapshot previous;
    previous.expectedDrawItemCount = 2u;
    previous.drawItems.resize(2u);
    previous.drawItems[0].meshPath = "Library/Artifacts/aa/mesh-a";
    previous.drawItems[0].materialPaths = {"Library/Artifacts/bb/material-a"};
    previous.drawItems[0].localPosition = {1.0f, 2.0f, 3.0f};
    previous.drawItems[1].meshPath = "Library/Artifacts/cc/mesh-b";
    previous.drawItems[1].localScale = {2.0f, 2.0f, 2.0f};

    auto completedResources = previous;
    completedResources.drawItems[0].sourceObject = NLS::Engine::Serialize::ObjectId(
        NLS::Guid::Parse("11111111-1111-4111-8111-111111111111"));
    completedResources.drawItems[0].meshAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("22222222-2222-4222-8222-222222222222"));
    completedResources.drawItems[0].materialAssetIds = {
        NLS::Core::Assets::AssetId(
            NLS::Guid::Parse("33333333-3333-4333-8333-333333333333"))
    };
    completedResources.drawItems[0].materialPaths[0] =
        "Library/Artifacts/bb/material-a-resolved";
    completedResources.drawItems[0].localRotation = {
        -previous.drawItems[0].localRotation.x,
        -previous.drawItems[0].localRotation.y,
        -previous.drawItems[0].localRotation.z,
        -previous.drawItems[0].localRotation.w
    };
    EXPECT_TRUE(CanReusePrefabPreviewSceneAssemblyForTesting(
        previous,
        completedResources))
        << "Resource readiness and registry identity changes must not rebuild an already assembled prefab scene.";

    auto moved = previous;
    moved.drawItems[0].localPosition.x += 1.0f;
    EXPECT_FALSE(CanReusePrefabPreviewSceneAssemblyForTesting(previous, moved));

    auto rotated = previous;
    rotated.drawItems[0].localRotation = NLS::Maths::Quaternion(
        0.0f,
        0.0f,
        0.7071067f,
        0.7071067f);
    EXPECT_FALSE(CanReusePrefabPreviewSceneAssemblyForTesting(previous, rotated));

    auto replacedMesh = previous;
    replacedMesh.drawItems[1].meshPath = "Library/Artifacts/dd/mesh-c";
    EXPECT_FALSE(CanReusePrefabPreviewSceneAssemblyForTesting(previous, replacedMesh));

    auto addedMaterialSlot = previous;
    addedMaterialSlot.drawItems[0].materialPaths.push_back(
        "Library/Artifacts/ee/material-b");
    EXPECT_FALSE(CanReusePrefabPreviewSceneAssemblyForTesting(previous, addedMaterialSlot));

    auto addedDrawItem = previous;
    addedDrawItem.expectedDrawItemCount = 3u;
    addedDrawItem.drawItems.resize(3u);
    EXPECT_FALSE(CanReusePrefabPreviewSceneAssemblyForTesting(previous, addedDrawItem));
}

TEST(AssetThumbnailCacheTests, GpuPrefabPreviewPreservesSceneAcrossDrawPrewarmPending)
{
    using namespace NLS::Editor::Assets;

#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect GPU thumbnail scene cleanup.";
#else
    EXPECT_TRUE(ShouldPreservePrefabPreviewSceneAfterRenderAttemptForTesting(
        "thumbnail-gpu-preview-resources-pending:prefab-draw-prewarm=2/43"))
        << "A transient draw-prewarm yield must preserve the scene assembly and cursor.";
    EXPECT_TRUE(ShouldPreservePrefabPreviewSceneAfterRenderAttemptForTesting(
        "thumbnail-gpu-preview-readback-pending"))
        << "An in-flight readback must retain its preview scene inputs.";
    EXPECT_TRUE(ShouldPreservePrefabPreviewSceneAfterRenderAttemptForTesting(
        "thumbnail-gpu-preview-resources-pending:prefab-scene-assembly=18/43"))
        << "A time-sliced scene assembly must retain its proxies and cursor so the next pump can continue.";
    EXPECT_FALSE(ShouldPreservePrefabPreviewSceneAfterRenderAttemptForTesting(
        "thumbnail-gpu-preview-prefab-mesh-load-failed"))
        << "Terminal preview failures must release transient scene objects.";
#endif
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

TEST(AssetThumbnailCacheTests, LargeGpuPrefabPreviewKeepsCompleteSourcePlan)
{
    using namespace NLS::Editor::Assets;

#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect the GPU thumbnail proxy plan.";
#else
    PreviewRenderableSnapshot snapshot;
    snapshot.expectedDrawItemCount = 405u;
    snapshot.drawItems.reserve(snapshot.expectedDrawItemCount);
    for (size_t z = 0u; z < 9u; ++z)
    {
        for (size_t y = 0u; y < 5u; ++y)
        {
            for (size_t x = 0u; x < 9u; ++x)
            {
                PreviewDrawItem item;
                item.meshPath = ":Models/Cube";
                item.localPosition = {
                    static_cast<float>(x) - 4.0f,
                    static_cast<float>(y) - 2.0f,
                    static_cast<float>(z) - 4.0f
                };
                snapshot.drawItems.push_back(std::move(item));
            }
        }
    }
    ASSERT_EQ(snapshot.drawItems.size(), snapshot.expectedDrawItemCount);

    AssetThumbnailRequest request;
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 96u;
    const auto plan = BuildThumbnailPreviewPrefabResourcePlanForTesting(request, snapshot);

    EXPECT_EQ(plan.drawItemCount, snapshot.drawItems.size())
        << "The canonical GPU preview plan must retain every source draw item.";
    ASSERT_EQ(plan.selectedDrawItemIndices.size(), plan.drawItemCount);

    bool coversNegativeX = false;
    bool coversPositiveX = false;
    bool coversNegativeZ = false;
    bool coversPositiveZ = false;
    for (const auto index : plan.selectedDrawItemIndices)
    {
        ASSERT_LT(index, snapshot.drawItems.size());
        const auto& position = snapshot.drawItems[index].localPosition;
        coversNegativeX = coversNegativeX || position.x <= -3.0f;
        coversPositiveX = coversPositiveX || position.x >= 3.0f;
        coversNegativeZ = coversNegativeZ || position.z <= -3.0f;
        coversPositiveZ = coversPositiveZ || position.z >= 3.0f;
    }
    EXPECT_TRUE(coversNegativeX);
    EXPECT_TRUE(coversPositiveX);
    EXPECT_TRUE(coversNegativeZ);
    EXPECT_TRUE(coversPositiveZ);
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

TEST(AssetThumbnailCacheTests, ThumbnailPreviewDefaultShaderSelectionDoesNotSynchronouslyLoadArtifactDb)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp"));
    const auto body = ExtractFunctionBody(
        source,
        "NLS::Render::Resources::Shader* ResolveThumbnailPreviewDefaultShader(");

    EXPECT_EQ(body.find("ArtifactDatabase"), std::string::npos)
        << "Default material selection runs on the GPU preview path and must not synchronously scan ArtifactDB.";
    EXPECT_EQ(body.find("GetResource(candidate, true)"), std::string::npos)
        << "Default material selection must only use already-loaded shader resources.";
}

TEST(AssetThumbnailCacheTests, ThumbnailPreviewDefaultShaderLabMaterialKeepsAmbientOcclusionVisible)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp"));
    const auto body = ExtractFunctionBody(
        source,
        "NLS::Render::Resources::Material& DefaultMaterial()");

    EXPECT_NE(body.find("SetRawParameter(\"_AmbientOcclusion\", 1.0f)"), std::string::npos)
        << "ShaderLab StandardPBR multiplies RGB by _AmbientOcclusion, so the fallback material must not leave it at zero.";
}

TEST(AssetThumbnailCacheTests, ThumbnailPreviewPendingRenderInputsPollRetirementWithoutDraining)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Assets/EditorThumbnailPreviewRenderer.cpp"));
    const auto body = ExtractFunctionBody(source, "void RenderCurrentPreviewScene(");

    ASSERT_NE(body.find("threadedReadback.renderInputsKeepAlive"), std::string::npos);
    EXPECT_NE(body.find("SetNextFramePostSubmitTextureReadback"), std::string::npos)
        << "The readback must be queued on the same threaded frame as the thumbnail draw.";
    EXPECT_NE(body.find("WasLastThreadedFramePublished"), std::string::npos)
        << "Lifecycle backpressure must be reported without using a global frame-id maximum.";
    EXPECT_EQ(source.find("latestRetiredFrameId"), std::string::npos)
        << "Renderer-local frame ids are not a valid cross-renderer completion token.";
    EXPECT_EQ(body.find("TryDrainThreadedRendering"), std::string::npos)
        << "Thumbnail retries run from the editor UI pump and must never synchronously drain rendering.";
}

TEST(AssetThumbnailCacheTests, ExternalMeshPrefabRemainsPendingWithoutGpuRenderer)
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
    EXPECT_FALSE(generated.has_value());
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Missing);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, BuiltinPrimitivePrefabRemainsPendingWithoutGpuRenderer)
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
    EXPECT_FALSE(generated.has_value());
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Missing);

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

TEST(AssetThumbnailCacheTests, GpuPreviewCamerasAndLightingUseUpperObliqueSetup)
{
    using namespace NLS::Editor::Assets;

#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect GPU thumbnail preview setup.";
#else
    const auto prefabCamera = BuildPrefabPreviewCameraDebugInfoForTesting(
        {-1.0f, -1.0f, -1.0f},
        {1.0f, 1.0f, 1.0f},
        96u,
        96u);
    const auto meshCamera = BuildMeshPreviewCameraDebugInfoForTesting(
        {-1.0f, -1.0f, -1.0f},
        {1.0f, 1.0f, 1.0f},
        96u,
        96u);
    const NLS::Maths::Vector3 center{0.0f, 0.0f, 0.0f};
    const auto expectUpperObliqueCamera = [&center](
        const EditorThumbnailPreviewCameraDebugInfo& camera,
        const char* previewKind)
    {
        const auto toCamera = camera.cameraPosition - center;
        EXPECT_GT(camera.cameraPosition.y, center.y)
            << previewKind << " GPU previews should place the camera above the asset, not below it.";
        EXPECT_LT(camera.lookDirection.y, 0.0f)
            << previewKind << " preview camera should look downward from an upper oblique angle.";
        EXPECT_GT(std::abs(toCamera.x), 0.1f) << previewKind;
        EXPECT_GT(std::abs(toCamera.z), 0.1f) << previewKind;
        EXPECT_GT(camera.distance, 0.0f) << previewKind;
    };
    expectUpperObliqueCamera(prefabCamera, "Prefab");
    expectUpperObliqueCamera(meshCamera, "Mesh");
    EXPECT_LT(ThumbnailPreviewCamera::MeshLookPitchDegrees, 0.0f)
        << "CPU-generated Mesh sub-assets and GPU Mesh previews share the same upper-oblique pitch.";

    const auto keyLight = GetThumbnailPreviewKeyLightDirectionForTesting();
    EXPECT_LT(keyLight.y, -0.25f)
        << "The key light should illuminate from above instead of grazing the material sphere.";
    EXPECT_GT(GetThumbnailPreviewKeyLightIntensityForTesting(), 0.6f);
    EXPECT_LT(GetThumbnailPreviewKeyLightIntensityForTesting(), 0.9f);
    EXPECT_EQ(GetThumbnailPreviewKeyLightSampleCountForTesting(), 2u)
        << "The thumbnail rig should pair its key with one fill light, softening the terminator without excessive per-pixel light work.";
    EXPECT_GT(GetThumbnailPreviewKeyLightAngularRadiusDegreesForTesting(), 0.0f)
        << "Coincident directional samples would retain the same hard terminator as a single point-like key light.";
    EXPECT_NEAR(
        GetThumbnailPreviewKeyLightSampleIntensitySumForTesting(),
        GetThumbnailPreviewKeyLightIntensityForTesting(),
        0.001f)
        << "Softening the preview key must redistribute its energy instead of increasing peak brightness.";
    EXPECT_NEAR(GetThumbnailPreviewAmbientIntensityForTesting(), 0.10f, 0.001f)
        << "Thumbnail previews should use the same ambient-light intensity as Asset View. The PBR shader already "
           "adds a 0.18 visible ambient floor, so a stronger preview-only ambient light washes out bright materials.";
    EXPECT_LE(
        GetThumbnailPreviewKeyLightIntensityForTesting() +
            GetThumbnailPreviewAmbientIntensityForTesting() + 0.18f,
        1.0f)
        << "A directly lit white diffuse surface must retain LDR highlight headroom in thumbnail previews.";
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

TEST(AssetThumbnailCacheTests, OversizedImportedModelPrefabWaitsForGpuRendererWithoutCpuRasterFallback)
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
    EXPECT_FALSE(generated.has_value());
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);
    EXPECT_FALSE(service.StartNextThumbnailGeneration());
    EXPECT_NE(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Fresh);

    std::filesystem::remove_all(root);
}

NLS_LONG_RUNNING_TEST(AssetThumbnailStressPerformanceTests, MeshSetFallbackDoesNotCachePartialPreviewWhenMeshBudgetSkipsManifestMeshes)
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

TEST(AssetThumbnailCacheTests, DegeneratePrefabRemainsPendingWithoutGpuRenderer)
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
    EXPECT_FALSE(generated.has_value());
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Missing);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ThinTrianglePrefabRemainsPendingWithoutGpuRenderer)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("bf151515-1515-4515-8515-151515151515"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot / "meshes");

    WriteBinaryFile(root / "Assets" / "Models" / "ThinStrip.fbx", std::vector<uint8_t>{'f', 'b', 'x'});
    WriteBinaryFile(
        artifactRoot / "meshes" / "ThinStrip.nmesh",
        NLS::Render::Assets::SerializeMeshArtifact(ThinTriangleStripMeshArtifact()));
    const auto prefabPayload = PrefabPayloadWithSingleRendererDependency(assetId, "mesh:ThinStrip");
    WriteNativeArtifactTextFile(
        artifactRoot / "ThinStrip.nprefab",
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
        "\"primarySubAssetKey\":\"prefab:ThinStrip\","
        "\"subAssets\":["
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"prefab:ThinStrip\","
        "\"artifactType\":\"Prefab\","
        "\"loaderId\":\"native-prefab\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/ThinStrip.nprefab\","
        "\"contentHash\":\"prefab-hash\""
        "},"
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"mesh:ThinStrip\","
        "\"artifactType\":\"Mesh\","
        "\"loaderId\":\"mesh\","
        "\"targetPlatform\":\"editor\","
        "\"artifactPath\":\"Library/Artifacts/" + assetId.ToString() + "/meshes/ThinStrip.nmesh\","
        "\"contentHash\":\"mesh-hash\""
        "}"
        "]"
        "}");

    auto request = MakeThumbnailRequest(root, "prefab:ThinStrip");
    request.assetId = assetId;
    request.sourceAssetPath = "Assets/Models/ThinStrip.fbx";
    request.artifactPath = RedirectedArtifactPathOrFallback(
        "Library/Artifacts/" + assetId.ToString() + "/ThinStrip.nprefab");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 96u;
    request.freshnessInputs = {{"artifact", "thin-strip-prefab:v1"}};

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto generated = service.GenerateNextThumbnail();
    EXPECT_FALSE(generated.has_value());
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Missing);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, OversizedPrefabRemainsPendingWithoutGpuRenderer)
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
    EXPECT_FALSE(generated.has_value());
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Missing);
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

TEST(AssetThumbnailCacheTests, DeferredGeneratedRequestIgnoresUnrelatedArtifactDatabaseWrites)
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

    EXPECT_EQ(BuildAssetThumbnailCacheKey(*first), BuildAssetThumbnailCacheKey(*second));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceRequestFreshnessIgnoresUnrelatedDatabaseWritesForResolvedModel)
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

    EXPECT_EQ(BuildAssetThumbnailCacheKey(*first), BuildAssetThumbnailCacheKey(*second));

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
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);
    const auto fresh = service.GetThumbnail(request);
    EXPECT_EQ(fresh.status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(fresh.imagePath, entry->imagePath);
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);

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

    const auto firstPending = service.GetThumbnail(request);
    ASSERT_EQ(firstPending.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(firstPending.presentationState, ThumbnailPresentationState::Loading);
    const auto secondPending = service.GetThumbnail(request);
    ASSERT_EQ(secondPending.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(secondPending.presentationState, ThumbnailPresentationState::Loading);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);

    service.SupersedeQueuedRequestsForGeneration("Assets/Other#96");
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Cancelled);

    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    ASSERT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(generated->presentationState, ThumbnailPresentationState::Ready);
    EXPECT_EQ(generated->previewQuality, ThumbnailPreviewQuality::Canonical);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Ready);

    auto unsupported = MakeThumbnailRequest(root, "generic:Unsupported");
    unsupported.kind = AssetThumbnailKind::GenericPreview;
    const auto unsupportedResult = service.GetThumbnail(unsupported);
    EXPECT_EQ(unsupportedResult.status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_EQ(unsupportedResult.presentationState, ThumbnailPresentationState::Fallback);
    EXPECT_EQ(service.GetThumbnailState(unsupported), ThumbnailState::Failed);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, SameScopeRequeryKeepsQueuedThumbnailWorkAndScopeChangeSupersedes)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.png", TinyPng());

    auto request = MakeThumbnailRequest(root, {});
    request.sourceAssetPath = "Assets/Textures/Hero.png";
    request.kind = AssetThumbnailKind::Texture;
    request.freshnessInputs = {{"source", "tiny-png:v1"}};
    request.priority = ThumbnailRequestPriority::Visible;

    AssetThumbnailService service;
    service.SupersedeQueuedRequestsForGeneration("Assets/Textures#96#visible");
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetQueuedRequestCount(), 1u);

    service.SupersedeQueuedRequestsForGeneration("Assets/Textures#96#visible");
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u)
        << "Dirty same-scope thumbnail requeries must keep visible work queued without duplicating it.";
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Queued);

    service.SupersedeQueuedRequestsForGeneration("Assets/Other#96#visible");
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Cancelled);

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

TEST(AssetThumbnailCacheTests, ServiceKeepsValidComplexPrefabPreviewPendingWithoutFailureMetadata)
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
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(generated->diagnostic, "thumbnail-gpu-preview-complexity-pending");
    EXPECT_EQ(renderer.renderCount, 1u);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::WaitingForResources);

    const auto evaluated = EvaluateAssetThumbnailCache(request);
    EXPECT_NE(evaluated.status, AssetThumbnailCacheStatus::Failed);

    const auto repeated = service.GetThumbnail(request);
    EXPECT_EQ(repeated.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ImportedModelPrefabPreviewBudgetExceededKeepsGpuProxyPending)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("bf161616-1616-4616-8616-161616161616"));
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    std::filesystem::create_directories(artifactRoot);
    WriteBinaryFile(root / "Assets" / "Models" / "Huge.gltf", std::vector<uint8_t>{'g', 'l', 't', 'f'});
    WriteNativeArtifactTextFile(
        artifactRoot / "Huge.nprefab",
        ArtifactType::Prefab,
        "prefab",
        1u,
        MinimalPrefabPayload());

    auto request = MakeThumbnailRequest(root, "prefab:Huge");
    request.assetId = assetId;
    request.sourceAssetPath = "Assets/Models/Huge.gltf";
    request.artifactPath = RedirectedArtifactPathOrFallback(
        "Library/Artifacts/" + assetId.ToString() + "/Huge.nprefab");
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.requestedSize = 96u;
    request.priority = ThumbnailRequestPriority::Visible;
    request.freshnessInputs = {{"artifact", "imported-prefab-budget-gpu-only:v1"}};

    AssetThumbnailService service;
    PrefabBudgetExceededThumbnailPreviewRenderer renderer;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    const auto generated = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(generated->diagnostic, "thumbnail-gpu-preview-complexity-pending");
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::WaitingForResources);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_NE(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Failed);

    EXPECT_FALSE(service.StartNextThumbnailGeneration())
        << "Complex imported models must not be rasterized by a background CPU fallback.";
    EXPECT_EQ(renderer.renderCount, 1u);

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

TEST(AssetThumbnailCacheTests, PendingDuplicateMaterialPreviewKeepsCacheEntryForVisibleBackfill)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();

    auto material = MakeThumbnailRequest(root, "material:Body");
    material.sourceAssetPath = "Assets/Materials/Body.mat";
    material.kind = AssetThumbnailKind::MaterialSphere;
    material.priority = ThumbnailRequestPriority::Visible;
    material.freshnessInputs = {{"source", "material:v1"}};

    const auto expectedEntry = ResolveAssetThumbnailCacheEntry(material);
    ASSERT_TRUE(expectedEntry.has_value());

    AssetThumbnailService service;
    const auto first = service.GetThumbnail(material);
    ASSERT_EQ(first.status, AssetThumbnailServiceStatus::Pending);
    ASSERT_TRUE(first.cacheEntry.has_value());
    EXPECT_EQ(first.cacheEntry->cacheKey, expectedEntry->cacheKey);

    const auto duplicate = service.GetThumbnail(material);
    ASSERT_EQ(duplicate.status, AssetThumbnailServiceStatus::Pending);
    ASSERT_TRUE(duplicate.cacheEntry.has_value())
        << "Pending fast-path results still need the cache key so the Asset Browser can bind the visible item to the completed GPU preview.";
    EXPECT_EQ(duplicate.cacheEntry->cacheKey, expectedEntry->cacheKey);
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
    EXPECT_FALSE(std::filesystem::exists(
        std::filesystem::path(entry->metadataPath.generic_string() + ".failure")));
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

TEST(AssetThumbnailCacheTests, ServicePrefersReadableSourceTextureOverGeneratedArtifact)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Source.bmp", Bmp2x1());
    WriteBinaryFile(root / "Assets" / "Models" / "Hero.gltf", std::vector<uint8_t>{'g', 'l', 't', 'f'});

    const auto artifactPath =
        root / LibraryArtifactPath("c6d69f90e0b33e0d299d12cd1ae95ab4c06d34fd67c24a544d85efad52f70942");
    WriteBinaryFile(
        artifactPath,
        NLS::Render::Assets::SerializeTextureArtifact(RgbaTextureArtifact4x2()));

    auto request = MakeThumbnailRequest(root, "texture:source");
    request.sourceAssetPath = "Assets/Textures/Source.bmp";
    request.artifactPath = "Library/Artifacts/c6/c6d69f90e0b33e0d299d12cd1ae95ab4c06d34fd67c24a544d85efad52f70942";
    request.kind = AssetThumbnailKind::Texture;
    request.requestedSize = 2u;
    request.freshnessInputs = {{"source", "source-with-artifact:v1"}};

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);

    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(
        CountArtifactTelemetryStage(
            telemetry,
            NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead),
        0u);
    EXPECT_EQ(
        CountArtifactTelemetryStage(
            telemetry,
            NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy),
        0u);

    const NLS::Image decoded(generated->imagePath.string(), false);
    EXPECT_EQ(decoded.GetWidth(), 2);
    EXPECT_EQ(decoded.GetHeight(), 1);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, GeneratedSubAssetPrefersReadableSourceTextureOverArtifact)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "GeneratedSource.bmp", Bmp2x1());
    WriteBinaryFile(root / "Assets" / "Models" / "Container.gltf", std::vector<uint8_t>{'g', 'l', 't', 'f'});

    const auto artifactPath =
        root / LibraryArtifactPath("c6d69f90e0b33e0d299d12cd1ae95ab4c06d34fd67c24a544d85efad52f70942");
    WriteBinaryFile(
        artifactPath,
        NLS::Render::Assets::SerializeTextureArtifact(RgbaTextureArtifact4x2()));

    auto request = MakeThumbnailRequest(root, "texture:embedded");
    request.sourceAssetPath = "Assets/Textures/GeneratedSource.bmp";
    request.artifactPath = "Library/Artifacts/c6/c6d69f90e0b33e0d299d12cd1ae95ab4c06d34fd67c24a544d85efad52f70942";
    request.kind = AssetThumbnailKind::Texture;
    request.generatedSubAsset = true;
    request.requestedSize = 2u;
    request.freshnessInputs = {{"source", "generated-source:v1"}};

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);

    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(
        CountArtifactTelemetryStage(
            telemetry,
            NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead),
        0u);
    EXPECT_EQ(
        CountArtifactTelemetryStage(
            telemetry,
            NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy),
        0u);

    const NLS::Image decoded(generated->imagePath.string(), false);
    EXPECT_EQ(decoded.GetWidth(), 2);
    EXPECT_EQ(decoded.GetHeight(), 1);

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
    EXPECT_TRUE(std::filesystem::is_regular_file(
        std::filesystem::path(entry->metadataPath.generic_string() + ".failure")));

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

TEST(AssetThumbnailCacheTests, ServiceRejectsOversizedLegacyMeshPreviewWithoutFullPayloadRead)
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
    request.generatedSubAsset = true;
    request.requestedSize = 96u;
    request.freshnessInputs = {{"artifact", "oversized-mesh:v1"}};

    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fallback) << generated->diagnostic;
    EXPECT_EQ(generated->diagnostic, "thumbnail-model-preview-budget-exceeded");
    EXPECT_TRUE(std::filesystem::exists(
        std::filesystem::path(entry->metadataPath.generic_string() + ".failure")));
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
    EXPECT_EQ(evaluated.diagnostic, "thumbnail-model-preview-budget-exceeded");

    const auto repeated = service.GetThumbnail(request);
    EXPECT_EQ(repeated.status, AssetThumbnailServiceStatus::Failed);
    EXPECT_EQ(repeated.diagnostic, "thumbnail-model-preview-budget-exceeded");
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
    request.generatedSubAsset = true;
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

    uint64_t requestRevision = 0u;
    for (const std::string diagnostic : {
        "thumbnail-generation-out-of-memory",
        "thumbnail-generation-exception"
    })
    {
        auto retryRequest = request;
        retryRequest.freshnessInputs.front().stamp += ":" + diagnostic;
        retryRequest.requestRevision = ++requestRevision;
        ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(
            retryRequest,
            AssetThumbnailCacheStatus::Failed,
            diagnostic));

        const auto evaluated = EvaluateAssetThumbnailCache(retryRequest);
        ASSERT_EQ(evaluated.status, AssetThumbnailCacheStatus::Failed);
        ASSERT_EQ(evaluated.diagnostic, diagnostic);

        AssetThumbnailService service;
        const auto repeated = service.GetThumbnail(retryRequest);
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
    EXPECT_FALSE(std::filesystem::exists(
        std::filesystem::path(staleEntry->metadataPath.generic_string() + ".failure")));

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

TEST(AssetThumbnailCacheTests, ServiceStartsSmallCurrentGenerationThumbnailBatchAsynchronously)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem(2u);

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "First.png", TinyPng());
    WriteBinaryFile(root / "Assets" / "Textures" / "Second.png", TinyPng());
    WriteBinaryFile(root / "Assets" / "Textures" / "Third.png", TinyPng());

    auto first = MakeThumbnailRequest(root, "texture:First");
    first.sourceAssetPath = "Assets/Textures/First.png";
    first.kind = AssetThumbnailKind::Texture;
    first.priority = ThumbnailRequestPriority::Visible;
    first.freshnessInputs = {{"source", "first:v1"}};

    auto second = MakeThumbnailRequest(root, "texture:Second");
    second.sourceAssetPath = "Assets/Textures/Second.png";
    second.kind = AssetThumbnailKind::Texture;
    second.priority = ThumbnailRequestPriority::Visible;
    second.freshnessInputs = {{"source", "second:v1"}};

    auto third = MakeThumbnailRequest(root, "texture:Third");
    third.sourceAssetPath = "Assets/Textures/Third.png";
    third.kind = AssetThumbnailKind::Texture;
    third.priority = ThumbnailRequestPriority::Visible;
    third.freshnessInputs = {{"source", "third:v1"}};

    {
        AssetThumbnailService service;
        ASSERT_EQ(service.GetThumbnail(first).status, AssetThumbnailServiceStatus::Pending);
        ASSERT_EQ(service.GetThumbnail(second).status, AssetThumbnailServiceStatus::Pending);
        ASSERT_EQ(service.GetThumbnail(third).status, AssetThumbnailServiceStatus::Pending);
        ASSERT_EQ(service.GetQueuedRequestCount(), 3u);

        EXPECT_TRUE(service.StartNextThumbnailGeneration());
        EXPECT_TRUE(service.StartNextThumbnailGeneration())
            << "Visible thumbnail display should not serialize the current generation through one in-flight worker.";
        EXPECT_TRUE(service.HasInFlightRequest());
        EXPECT_LE(service.GetQueuedRequestCount(), 1u);
        // The visible Texture lane has four bounded slots. A very small source
        // image may complete between calls and immediately recycle one of them,
        // so this assertion must not depend on the exact worker scheduling point.
        (void)service.StartNextThumbnailGeneration();
        EXPECT_LE(service.GetQueuedRequestCount(), 1u);

        size_t completedCount = 0u;
        for (int attempt = 0; attempt < 100 && completedCount < 2u; ++attempt)
        {
            if (const auto completed = service.ConsumeCompletedThumbnail();
                completed.has_value())
            {
                EXPECT_EQ(completed->status, AssetThumbnailServiceStatus::Fresh);
                ++completedCount;
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        EXPECT_EQ(completedCount, 2u);
    }

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ExplicitCacheWriteBudgetDoesNotIncreaseCurrentGenerationConcurrency)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem(4u);

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    for (int index = 0; index < 5; ++index)
        WriteBinaryFile(root / "Assets" / "Textures" / ("Visible" + std::to_string(index) + ".png"), TinyPng());

    {
        AssetThumbnailService service;
        ThumbnailGenerationBudget budget;
        budget.cacheWriteCountBudget = 4u;
        service.SetThumbnailGenerationBudget(budget);

        for (int index = 0; index < 5; ++index)
        {
            auto request = MakeThumbnailRequest(root, "texture:Visible" + std::to_string(index));
            request.sourceAssetPath = "Assets/Textures/Visible" + std::to_string(index) + ".png";
            request.kind = AssetThumbnailKind::Texture;
            request.priority = ThumbnailRequestPriority::Visible;
            request.freshnessInputs = {{"source", "visible:" + std::to_string(index) + ":v1"}};
            ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
        }

        EXPECT_TRUE(service.StartNextThumbnailGeneration());
        EXPECT_TRUE(service.StartNextThumbnailGeneration());
        // Cache-write budget controls publication work, while the visible
        // Texture lane has its own bounded capacity. Completed tiny images may
        // recycle a slot before the next call, so do not assert a fixed return
        // value at this scheduling boundary.
        (void)service.StartNextThumbnailGeneration();
        EXPECT_LE(service.GetQueuedRequestCount(), 3u);
    }

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ObsoleteGenerationCompletionDoesNotConsumeCurrentCacheWriteBudget)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Old.png", TinyPng());
    WriteBinaryFile(root / "Assets" / "Textures" / "Current.png", TinyPng());

    auto oldRequest = MakeThumbnailRequest(root, "texture:Old");
    oldRequest.sourceAssetPath = "Assets/Textures/Old.png";
    oldRequest.kind = AssetThumbnailKind::Texture;
    oldRequest.freshnessInputs = {{"source", "old:v1"}};

    auto currentRequest = MakeThumbnailRequest(root, "texture:Current");
    currentRequest.sourceAssetPath = "Assets/Textures/Current.png";
    currentRequest.kind = AssetThumbnailKind::Texture;
    currentRequest.freshnessInputs = {{"source", "current:v1"}};

    {
        AssetThumbnailService service;
        ThumbnailGenerationBudget budget;
        budget.cacheWriteCountBudget = 1u;
        service.SetThumbnailGenerationBudget(budget);

        ASSERT_EQ(service.GetThumbnail(oldRequest).status, AssetThumbnailServiceStatus::Pending);
        ASSERT_TRUE(service.StartNextThumbnailGeneration());
        service.SupersedeQueuedRequestsForGeneration("Assets/Textures#current");

        for (int attempt = 0; attempt < 100 && service.HasInFlightRequest(); ++attempt)
        {
            (void)service.ConsumeCompletedThumbnail();
            if (service.HasInFlightRequest())
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_FALSE(service.HasInFlightRequest());

        ASSERT_EQ(service.GetThumbnail(currentRequest).status, AssetThumbnailServiceStatus::Pending);
        EXPECT_TRUE(service.StartNextThumbnailGeneration())
            << "Obsolete thumbnail completions must not steal the explicit cache-write budget from the current visible scope.";
    }

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ObsoleteGenerationFreshCompletionRemainsPublishable)
{
    const ScopedAssetThumbnailCacheJobSystem jobSystem;

    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Hero.bmp", BmpRgb(1024u, 1024u));

    auto request = MakeThumbnailRequest(root, "texture:Hero");
    request.sourceAssetPath = "Assets/Textures/Hero.bmp";
    request.kind = AssetThumbnailKind::Texture;
    request.priority = ThumbnailRequestPriority::Visible;
    request.freshnessInputs = {{"source", "hero:v1"}};
    const auto expectedEntry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(expectedEntry.has_value());

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_TRUE(service.StartNextThumbnailGeneration());
    service.SupersedeQueuedRequestsForGeneration("Assets/Textures#rebuilt");

    std::optional<AssetThumbnailServiceResult> completed;
    for (int attempt = 0; attempt < 100 && !completed.has_value(); ++attempt)
    {
        completed = service.ConsumeCompletedThumbnail();
        if (!completed.has_value())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(completed.has_value());
    EXPECT_EQ(completed->status, AssetThumbnailServiceStatus::Fresh);
    ASSERT_TRUE(completed->cacheEntry.has_value());
    EXPECT_EQ(completed->cacheEntry->cacheKey, expectedEntry->cacheKey);
    EXPECT_EQ(completed->presentationKey, BuildAssetThumbnailPresentationKey(request));
    EXPECT_EQ(completed->imagePath, expectedEntry->imagePath);

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
    second.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("a2020202-0202-4202-8202-020202020202"));
    second.sourceAssetPath = "Assets/Textures/Second.png";
    second.freshnessInputs = {{"source", "second:v1"}};

    {
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

        const auto secondEntry = ResolveAssetThumbnailCacheEntry(second);
        ASSERT_TRUE(secondEntry.has_value());
        std::optional<AssetThumbnailServiceResult> generated;
        for (int attempt = 0; attempt < 100 && !generated.has_value(); ++attempt)
        {
            auto completed = service.ConsumeCompletedThumbnail();
            if (completed.has_value() && completed->cacheEntry.has_value() &&
                completed->cacheEntry->cacheKey == secondEntry->cacheKey)
            {
                generated = std::move(completed);
            }
            else if (!completed.has_value())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        ASSERT_TRUE(generated.has_value());
        EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);

        EXPECT_EQ(generated->imagePath, secondEntry->imagePath);
        EXPECT_TRUE(std::filesystem::exists(secondEntry->metadataPath));
    }

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

        const auto thirdEntry = ResolveAssetThumbnailCacheEntry(third);
        ASSERT_TRUE(thirdEntry.has_value());
        std::optional<AssetThumbnailServiceResult> generated;
        for (int attempt = 0; attempt < 100 && !generated.has_value(); ++attempt)
        {
            auto completed = service.ConsumeCompletedThumbnail();
            if (completed.has_value() && completed->cacheEntry.has_value() &&
                completed->cacheEntry->cacheKey == thirdEntry->cacheKey)
            {
                generated = std::move(completed);
            }
            else if (!completed.has_value())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        ASSERT_TRUE(generated.has_value());
        EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);
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
    second.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("a3030303-0303-4303-8303-030303030303"));
    second.sourceAssetPath = "Assets/Textures/Second.bmp";
    second.freshnessInputs = {{"source", "second:v1"}};

    auto third = first;
    third.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("a4040404-0404-4404-8404-040404040404"));
    third.sourceAssetPath = "Assets/Textures/Third.bmp";
    third.freshnessInputs = {{"source", "third:v1"}};

    auto fourth = first;
    fourth.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("a5050505-0505-4505-8505-050505050505"));
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

        const auto fourthEntry = ResolveAssetThumbnailCacheEntry(fourth);
        ASSERT_TRUE(fourthEntry.has_value());
        std::optional<AssetThumbnailServiceResult> generated;
        for (int attempt = 0; attempt < 100 && !generated.has_value(); ++attempt)
        {
            auto completed = service.ConsumeCompletedThumbnail();
            if (completed.has_value() && completed->cacheEntry.has_value() &&
                completed->cacheEntry->cacheKey == fourthEntry->cacheKey)
            {
                generated = std::move(completed);
            }
            else if (!completed.has_value())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        ASSERT_TRUE(generated.has_value());
        EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);
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

TEST(AssetThumbnailCacheTests, UnresolvedThumbnailRequestDoesNotUseGlobalArtifactDatabaseFreshness)
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
    ASSERT_FALSE(std::any_of(
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
    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Fresh);
    EXPECT_TRUE(evaluated.diagnostic.empty());

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

#if defined(NLS_ENABLE_TEST_HOOKS)
    ResetAssetThumbnailCacheEvaluationCountForTesting();
#endif

    EXPECT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.RequestAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetAssetPreview(request).status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_TRUE(service.IsLoadingAssetPreview(request));
#if defined(NLS_ENABLE_TEST_HOOKS)
    EXPECT_EQ(GetAssetThumbnailCacheEvaluationCountForTesting(), 1u)
        << "Duplicate queued preview requests should not re-evaluate the thumbnail cache on the UI thread.";
#endif

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

TEST(AssetThumbnailCacheTests, ServicePopsVisibleRequestBeforeBackgroundBurst)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "Visible.png", TinyPng());
    WriteBinaryFile(root / "Assets" / "Textures" / "Background0.png", TinyPng());
    WriteBinaryFile(root / "Assets" / "Textures" / "Background1.png", TinyPng());
    WriteBinaryFile(root / "Assets" / "Textures" / "Background2.png", TinyPng());
    WriteBinaryFile(root / "Assets" / "Textures" / "Background3.png", TinyPng());
    WriteBinaryFile(root / "Assets" / "Textures" / "Background4.png", TinyPng());

    auto visible = MakeThumbnailRequest(root, "texture:Visible");
    visible.sourceAssetPath = "Assets/Textures/Visible.png";
    visible.kind = AssetThumbnailKind::Texture;
    visible.priority = ThumbnailRequestPriority::Visible;
    visible.freshnessInputs = {{ "source", "visible:v1" }};

    AssetThumbnailService service;
    for (int index = 0; index < 5; ++index)
    {
        auto background = MakeThumbnailRequest(root, "texture:Background" + std::to_string(index));
        background.sourceAssetPath = "Assets/Textures/Background" + std::to_string(index) + ".png";
        background.kind = AssetThumbnailKind::Texture;
        background.priority = ThumbnailRequestPriority::Background;
        background.freshnessInputs = {{ "source", "background:" + std::to_string(index) + ":v1" }};
        ASSERT_EQ(service.GetThumbnail(background).status, AssetThumbnailServiceStatus::Pending);
    }
    ASSERT_EQ(service.GetThumbnail(visible).status, AssetThumbnailServiceStatus::Pending);

    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    ASSERT_TRUE(generated->cacheEntry.has_value());
    const auto visibleEntry = ResolveAssetThumbnailCacheEntry(visible);
    ASSERT_TRUE(visibleEntry.has_value());
    EXPECT_EQ(generated->cacheEntry->cacheKey, visibleEntry->cacheKey);
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);

    std::filesystem::remove_all(root);
}

#if defined(NLS_ENABLE_TEST_HOOKS)
TEST(AssetThumbnailCacheTests, PresentationKeyExcludesFreshnessAndSourcePath)
{
    using namespace NLS::Editor::Assets;
    const auto root = MakeAssetThumbnailCacheRoot();
    auto first = MakeThumbnailRequest(root, "prefab:Hero", "source:v1");
    first.sourceAssetPath = "Assets/Prefabs/Hero.prefab";
    first.artifactPath = "Library/Artifacts/aa/hero-v1";
    auto second = first;
    second.sourceAssetPath = "Assets/Renamed/Hero.prefab";
    second.artifactPath = "Library/Artifacts/bb/hero-v2";
    second.freshnessInputs = {{"source", "source:v2"}};

    auto resized = second;
    resized.requestedSize += 32u;

    EXPECT_EQ(BuildAssetThumbnailPresentationKey(first), BuildAssetThumbnailPresentationKey(second));
    EXPECT_NE(BuildAssetThumbnailCacheKey(first), BuildAssetThumbnailCacheKey(second));
    EXPECT_NE(BuildAssetThumbnailPresentationKey(second), BuildAssetThumbnailPresentationKey(resized));
    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ResolvedModelThumbnailCacheSurvivesUnrelatedArtifactDatabaseWrites)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = AssetId(NLS::Guid::Parse("a7070707-0707-4707-8707-070707070707"));
    WriteBinaryFile(
        root / "Assets" / "Models" / "Resolved.gltf",
        std::vector<uint8_t>{'g', 'l', 't', 'f'});
    WriteBinaryFile(
        root / "Assets" / "Models" / "Resolved.gltf.meta",
        std::vector<uint8_t>{'m', 'e', 't', 'a'});
    const auto artifactPath = LibraryArtifactPath(
        "a7070707070747078707070707070707a7070707070747078707070707070707");
    WriteBinaryFile(root / artifactPath, std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});
    const auto artifactDatabaseDataPath = root / "Library" / "ArtifactDB" / "data.mdb";
    WriteBinaryFile(artifactDatabaseDataPath, std::vector<uint8_t>{'v', '1'});

    AssetBrowserItem model;
    model.kind = AssetBrowserItemKind::SourceAsset;
    model.type = AssetBrowserItemType::Model;
    model.assetId = assetId;
    model.sourceAssetPath = "Assets/Models/Resolved.gltf";
    model.subAssetKey = "prefab:Resolved";
    model.artifactPath = artifactPath;
    model.artifactType = ArtifactType::Prefab;

    const auto request = BuildAssetThumbnailRequestForItem(root, model, 96u);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->dependencyStamp.find("artifact-db="), std::string::npos);
    EXPECT_NE(request->dependencyStamp.find("artifact-record="), std::string::npos);
    EXPECT_NE(request->dependencyStamp.find("artifact-file="), std::string::npos);

    const auto entry = ResolveAssetThumbnailCacheEntry(*request);
    ASSERT_TRUE(entry.has_value());
    ASSERT_TRUE(WriteAssetThumbnailCacheFile(*request, entry->imagePath, TinyPng()));
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(*request, AssetThumbnailCacheStatus::Fresh, {}));
    ASSERT_EQ(EvaluateAssetThumbnailCache(*request).status, AssetThumbnailCacheStatus::Fresh);

    std::filesystem::resize_file(artifactDatabaseDataPath, 3u);

    const auto rebuilt = BuildAssetThumbnailRequestForItem(root, model, 96u);
    ASSERT_TRUE(rebuilt.has_value());
    EXPECT_EQ(BuildAssetThumbnailCacheKey(*rebuilt), entry->cacheKey);
    EXPECT_EQ(EvaluateAssetThumbnailCache(*rebuilt).status, AssetThumbnailCacheStatus::Fresh)
        << "Committing an unrelated asset must not hide a resolved model thumbnail after restart.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, PrefabResidentFreshnessUsesArtifactInsteadOfGlobalDatabaseOnceResolved)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = AssetId(NLS::Guid::Parse("a8080808-0808-4808-8808-080808080808"));
    const auto sourcePath = std::string("Assets/Models/Resident.gltf");
    const auto artifactPath = LibraryArtifactPath(
        "a8080808080848088808080808080808a8080808080848088808080808080808");
    WriteBinaryFile(root / sourcePath, std::vector<uint8_t>{'g', 'l', 't', 'f'});
    WriteBinaryFile(root / (sourcePath + ".meta"), std::vector<uint8_t>{'m', 'e', 't', 'a'});
    WriteBinaryFile(root / artifactPath, std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'});
    const auto artifactDatabaseDataPath = root / "Library" / "ArtifactDB" / "data.mdb";
    WriteBinaryFile(artifactDatabaseDataPath, std::vector<uint8_t>{'v', '1'});

    const auto resolvedBefore = BuildPrefabThumbnailDependencyStamp(
        root,
        assetId,
        sourcePath,
        "prefab:Resident",
        artifactPath);
    const auto resolvedAbsolute = BuildPrefabThumbnailDependencyStamp(
        root,
        assetId,
        sourcePath,
        "prefab:Resident",
        (root / artifactPath).generic_string());
    const auto deferredBefore = BuildPrefabThumbnailDependencyStamp(
        root,
        assetId,
        sourcePath,
        "prefab:Resident",
        {});

    std::filesystem::resize_file(artifactDatabaseDataPath, 3u);

    const auto resolvedAfter = BuildPrefabThumbnailDependencyStamp(
        root,
        assetId,
        sourcePath,
        "prefab:Resident",
        artifactPath);
    const auto deferredAfter = BuildPrefabThumbnailDependencyStamp(
        root,
        assetId,
        sourcePath,
        "prefab:Resident",
        {});

    EXPECT_EQ(resolvedAfter, resolvedBefore)
        << "An imported resident snapshot must remain reusable after unrelated database commits.";
    EXPECT_EQ(resolvedAbsolute, resolvedBefore)
        << "Scene registration and Asset Browser requests must canonicalize absolute and portable artifact records identically.";
    EXPECT_EQ(deferredAfter, deferredBefore)
        << "An unresolved request must remain stable across unrelated database commits; "
           "resolved metadata validates the exact sub-asset record.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, PrefabSceneAssemblyKeyExcludesFreshnessAndSourcePath)
{
    using namespace NLS::Editor::Assets;
    const auto root = MakeAssetThumbnailCacheRoot();
    auto first = MakeThumbnailRequest(root, "prefab:Hero", "source:v1");
    first.kind = AssetThumbnailKind::PrefabPreview;
    first.sourceAssetPath = "Assets/Prefabs/Hero.prefab";
    first.artifactPath = "Library/Artifacts/aa/hero-v1";
    auto second = first;
    second.sourceAssetPath = "Assets/Renamed/Hero.prefab";
    second.artifactPath = "Library/Artifacts/bb/hero-v2";
    second.dependencyStamp = "dependency:v2";
    second.freshnessInputs = {{"source", "source:v2"}};

    EXPECT_EQ(
        BuildThumbnailPreviewSceneAssemblyKeyForTesting(first),
        BuildThumbnailPreviewSceneAssemblyKeyForTesting(second));
    EXPECT_NE(
        BuildThumbnailPreviewReadbackRequestKeyForTesting(first),
        BuildThumbnailPreviewReadbackRequestKeyForTesting(second));
    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, PendingRefreshRetainsPreviousCanonicalPresentation)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto previous = MakeThumbnailRequest(root, "texture:Hero", "source:v1");
    previous.kind = AssetThumbnailKind::Texture;
    previous.sourceAssetPath = "Assets/Textures/Hero.png";
    previous.requestRevision = 1u;
    const auto previousEntry = ResolveAssetThumbnailCacheEntry(previous);
    ASSERT_TRUE(previousEntry.has_value());
    ASSERT_TRUE(WriteAssetThumbnailCacheFile(previous, previousEntry->imagePath, TinyPng()));
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(previous, AssetThumbnailCacheStatus::Fresh, {}));
    ASSERT_TRUE(CommitAssetThumbnailPresentation(previous, *previousEntry, previous.requestRevision));

    auto refreshing = previous;
    refreshing.freshnessInputs.front().stamp = "source:v2";
    refreshing.requestRevision = 2u;

    AssetThumbnailService service;
    const auto result = service.GetThumbnail(refreshing);
    EXPECT_EQ(result.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(result.presentationState, ThumbnailPresentationState::StaleRefreshing);
    ASSERT_TRUE(result.retainedImage.has_value());
    EXPECT_EQ(result.retainedImage->cacheKey, previousEntry->cacheKey);
    EXPECT_TRUE(result.refreshPending);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, PresentationIndexRotatesCurrentToPreviousAtomically)
{
    using namespace NLS::Editor::Assets;
    const auto root = MakeAssetThumbnailCacheRoot();
    auto first = MakeThumbnailRequest(root, "prefab:Hero", "source:v1");
    first.requestRevision = 1u;
    auto firstEntry = ResolveAssetThumbnailCacheEntry(first);
    ASSERT_TRUE(firstEntry.has_value());
    WriteBinaryFile(firstEntry->imagePath, TinyPng());
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(first, AssetThumbnailCacheStatus::Fresh, {}));

    auto second = first;
    second.freshnessInputs = {{"source", "source:v2"}};
    second.requestRevision = 2u;
    auto secondEntry = ResolveAssetThumbnailCacheEntry(second);
    ASSERT_TRUE(secondEntry.has_value());
    WriteBinaryFile(secondEntry->imagePath, TinyPng());
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(second, AssetThumbnailCacheStatus::Fresh, {}));

    const auto index = ReadAssetThumbnailPresentationIndex(second);
    ASSERT_TRUE(index.has_value());
    ASSERT_TRUE(index->current.has_value());
    ASSERT_TRUE(index->previous.has_value());
    EXPECT_EQ(index->current->cacheKey, secondEntry->cacheKey);
    EXPECT_EQ(index->previous->cacheKey, firstEntry->cacheKey);
    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, PresentationIndexAllowsNewEditorSessionToRestartRevisionOrdering)
{
    using namespace NLS::Editor::Assets;
    const auto root = MakeAssetThumbnailCacheRoot();

    auto previousSession = MakeThumbnailRequest(root, "prefab:SessionRestart", "source:v1");
    previousSession.requestSessionId = 101u;
    previousSession.requestRevision = 50u;
    const auto previousEntry = ResolveAssetThumbnailCacheEntry(previousSession);
    ASSERT_TRUE(previousEntry.has_value());
    WriteBinaryFile(previousEntry->imagePath, TinyPng());
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(
        previousSession,
        AssetThumbnailCacheStatus::Fresh,
        {}));

    auto restartedSession = previousSession;
    restartedSession.freshnessInputs = {{"source", "source:v2"}};
    restartedSession.requestSessionId = 202u;
    restartedSession.requestRevision = 1u;
    const auto restartedEntry = ResolveAssetThumbnailCacheEntry(restartedSession);
    ASSERT_TRUE(restartedEntry.has_value());
    WriteBinaryFile(restartedEntry->imagePath, TinyPng());
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(
        restartedSession,
        AssetThumbnailCacheStatus::Fresh,
        {})) << "A process-local revision from the previous editor must not cancel the new session.";

    const auto restartedIndex = ReadAssetThumbnailPresentationIndex(restartedSession);
    ASSERT_TRUE(restartedIndex.has_value());
    ASSERT_TRUE(restartedIndex->current.has_value());
    EXPECT_EQ(restartedIndex->committedSessionId, restartedSession.requestSessionId);
    EXPECT_EQ(restartedIndex->committedRevision, restartedSession.requestRevision);
    EXPECT_EQ(restartedIndex->current->cacheKey, restartedEntry->cacheKey);

    auto olderSameSession = restartedSession;
    olderSameSession.freshnessInputs = {{"source", "source:v3"}};
    olderSameSession.requestRevision = 0u;
    const auto olderEntry = ResolveAssetThumbnailCacheEntry(olderSameSession);
    ASSERT_TRUE(olderEntry.has_value());
    WriteBinaryFile(olderEntry->imagePath, TinyPng());
    EXPECT_FALSE(WriteAssetThumbnailCacheMetadata(
        olderSameSession,
        AssetThumbnailCacheStatus::Fresh,
        {})) << "Revision ordering must still reject an older writer in the current session.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, PresentationIndexReportsCanonicalFreshnessForBindingRecovery)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const std::string artifactHash(64u, 'a');
    const auto absoluteArtifactPath = root / LibraryArtifactPath(artifactHash);
    WriteBinaryFile(absoluteArtifactPath, {'p', 'r', 'e', 'f', 'a', 'b'});

    auto canonical = MakeThumbnailRequest(root, "prefab:BindingRecovery", "source:v2");
    canonical.kind = AssetThumbnailKind::PrefabPreview;
    canonical.sourceAssetPath = "Assets/Models/BindingRecovery.fbx";
    canonical.artifactPath = absoluteArtifactPath.generic_string();
    canonical.freshnessInputs = {{"artifact-file", FileStampForTest(absoluteArtifactPath)}};
    canonical.requestRevision = 26u;
    const auto canonicalEntry = ResolveAssetThumbnailCacheEntry(canonical);
    ASSERT_TRUE(canonicalEntry.has_value());
    ASSERT_TRUE(WriteAssetThumbnailCacheFile(canonical, canonicalEntry->imagePath, TinyPng()));
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(canonical, AssetThumbnailCacheStatus::Fresh, {}));

    auto unresolved = canonical;
    unresolved.artifactPath.clear();
    unresolved.dependencyStamp = "unresolved";
    unresolved.freshnessInputs.clear();
    unresolved.requestRevision = 13u;
    ASSERT_EQ(
        BuildAssetThumbnailPresentationKey(unresolved),
        BuildAssetThumbnailPresentationKey(canonical));
    ASSERT_NE(BuildAssetThumbnailCacheKey(unresolved), canonicalEntry->cacheKey);

    const auto presentation = ReadAssetThumbnailPresentationIndex(unresolved);
    ASSERT_TRUE(presentation.has_value());
    ASSERT_TRUE(presentation->current.has_value());
    EXPECT_TRUE(presentation->current->freshnessCurrent);

    AssetThumbnailServiceResult pending;
    pending.status = AssetThumbnailServiceStatus::Pending;
    pending.presentationState = ThumbnailPresentationState::Loading;
    pending.presentationKey = BuildAssetThumbnailPresentationKey(unresolved);
    pending.requestRevision = unresolved.requestRevision;
    ASSERT_TRUE(PromoteAssetThumbnailResultFromPresentationIndex(unresolved, pending));
    EXPECT_EQ(pending.status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(pending.presentationState, ThumbnailPresentationState::Ready);
    EXPECT_EQ(pending.requestRevision, canonical.requestRevision);
    ASSERT_TRUE(pending.cacheEntry.has_value());
    EXPECT_EQ(pending.cacheEntry->cacheKey, canonicalEntry->cacheKey);
    EXPECT_EQ(pending.imagePath, canonicalEntry->imagePath);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, PresentationPromotionRehydratesVerifiedCanonicalAcrossEditorSessions)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const std::string artifactHash(64u, 'c');
    const auto absoluteArtifactPath = root / LibraryArtifactPath(artifactHash);
    WriteBinaryFile(absoluteArtifactPath, {'p', 'r', 'e', 'f', 'a', 'b'});

    auto canonical = MakeThumbnailRequest(root, "prefab:RestartRecovery", "source:v2");
    canonical.kind = AssetThumbnailKind::PrefabPreview;
    canonical.sourceAssetPath = "Assets/Models/RestartRecovery.fbx";
    canonical.artifactPath = absoluteArtifactPath.generic_string();
    canonical.freshnessInputs = {
        {"artifact-file", FileStampForTest(absoluteArtifactPath)},
        {"item", "resolved-generation:v2"}};
    canonical.requestSessionId = 303u;
    canonical.requestRevision = 5u;
    const auto canonicalEntry = ResolveAssetThumbnailCacheEntry(canonical);
    ASSERT_TRUE(canonicalEntry.has_value());
    ASSERT_TRUE(WriteAssetThumbnailCacheFile(canonical, canonicalEntry->imagePath, TinyPng()));
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(canonical, AssetThumbnailCacheStatus::Fresh, {}));

    auto restarted = canonical;
    restarted.artifactPath.clear();
    restarted.dependencyStamp = "unresolved-after-restart";
    restarted.freshnessInputs = {{"item", "unresolved-after-restart"}};
    restarted.requestSessionId = 404u;
    restarted.requestRevision = 75u;
    ASSERT_NE(BuildAssetThumbnailCacheKey(restarted), canonicalEntry->cacheKey);

    const auto presentation = ReadAssetThumbnailPresentationIndex(restarted);
    ASSERT_TRUE(presentation.has_value());
    ASSERT_TRUE(presentation->current.has_value());
    EXPECT_FALSE(presentation->current->freshnessCurrent);
    EXPECT_TRUE(presentation->current->verifiableFreshnessCurrent);
    EXPECT_TRUE(presentation->current->hasVerifiableFreshnessInputs);

    AssetThumbnailServiceResult pending;
    pending.status = AssetThumbnailServiceStatus::Pending;
    pending.presentationState = ThumbnailPresentationState::Loading;
    pending.presentationKey = BuildAssetThumbnailPresentationKey(restarted);
    pending.requestSessionId = restarted.requestSessionId;
    pending.requestRevision = restarted.requestRevision;
    ASSERT_TRUE(PromoteAssetThumbnailResultFromPresentationIndex(restarted, pending))
        << "A validated canonical PNG must survive process-local revision reset/reordering.";
    ASSERT_TRUE(pending.cacheEntry.has_value());
    EXPECT_EQ(pending.cacheEntry->cacheKey, canonicalEntry->cacheKey);
    EXPECT_EQ(pending.requestRevision, restarted.requestRevision);
    EXPECT_EQ(pending.requestSessionId, restarted.requestSessionId);

    WriteBinaryFile(absoluteArtifactPath, {'c', 'h', 'a', 'n', 'g', 'e', 'd'});
    AssetThumbnailServiceResult stalePending;
    stalePending.status = AssetThumbnailServiceStatus::Pending;
    stalePending.presentationState = ThumbnailPresentationState::Loading;
    stalePending.presentationKey = BuildAssetThumbnailPresentationKey(restarted);
    stalePending.requestSessionId = restarted.requestSessionId;
    stalePending.requestRevision = restarted.requestRevision;
    EXPECT_FALSE(PromoteAssetThumbnailResultFromPresentationIndex(restarted, stalePending))
        << "Cross-session recovery must stop when the persisted artifact stamp is stale.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, PresentationPromotionAcceptsSameOrNewerVerifiableOpaqueItemCanonicalOnly)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto canonical = MakeThumbnailRequest(root, "prefab:OpaqueItem", "source:v2");
    canonical.kind = AssetThumbnailKind::PrefabPreview;
    canonical.sourceAssetPath = "Assets/Models/OpaqueItem.fbx";
    canonical.freshnessInputs = {{"item", "artifact-generation:v2"}};
    canonical.requestRevision = 26u;
    const auto canonicalEntry = ResolveAssetThumbnailCacheEntry(canonical);
    ASSERT_TRUE(canonicalEntry.has_value());
    ASSERT_TRUE(WriteAssetThumbnailCacheFile(canonical, canonicalEntry->imagePath, TinyPng()));
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(canonical, AssetThumbnailCacheStatus::Fresh, {}));

    const auto presentation = ReadAssetThumbnailPresentationIndex(canonical);
    ASSERT_TRUE(presentation.has_value());
    ASSERT_TRUE(presentation->current.has_value());
    EXPECT_FALSE(presentation->current->freshnessCurrent)
        << "The presentation reader cannot independently validate opaque item identities.";
    EXPECT_TRUE(presentation->current->verifiableFreshnessCurrent)
        << "Opaque identity must not hide that all filesystem-backed freshness stamps remain current.";

    AssetThumbnailServiceResult matchingPending;
    matchingPending.status = AssetThumbnailServiceStatus::Pending;
    matchingPending.presentationState = ThumbnailPresentationState::Loading;
    matchingPending.presentationKey = BuildAssetThumbnailPresentationKey(canonical);
    matchingPending.requestRevision = canonical.requestRevision - 1u;
    ASSERT_TRUE(PromoteAssetThumbnailResultFromPresentationIndex(canonical, matchingPending));
    ASSERT_TRUE(matchingPending.cacheEntry.has_value());
    EXPECT_EQ(matchingPending.cacheEntry->cacheKey, canonicalEntry->cacheKey);

    auto changedItem = canonical;
    changedItem.freshnessInputs = {{"item", "artifact-generation:v3"}};
    ASSERT_EQ(
        BuildAssetThumbnailPresentationKey(changedItem),
        BuildAssetThumbnailPresentationKey(canonical));
    ASSERT_NE(BuildAssetThumbnailCacheKey(changedItem), canonicalEntry->cacheKey);

    AssetThumbnailServiceResult lateResolvedPending;
    lateResolvedPending.status = AssetThumbnailServiceStatus::Pending;
    lateResolvedPending.presentationState = ThumbnailPresentationState::Loading;
    lateResolvedPending.presentationKey = BuildAssetThumbnailPresentationKey(changedItem);
    lateResolvedPending.requestRevision = canonical.requestRevision - 1u;
    ASSERT_TRUE(PromoteAssetThumbnailResultFromPresentationIndex(changedItem, lateResolvedPending))
        << "A newer canonical artifact with current filesystem stamps must complete the unresolved UI request.";
    ASSERT_TRUE(lateResolvedPending.cacheEntry.has_value());
    EXPECT_EQ(lateResolvedPending.cacheEntry->cacheKey, canonicalEntry->cacheKey);

    AssetThumbnailServiceResult sameRevisionPending;
    sameRevisionPending.status = AssetThumbnailServiceStatus::Pending;
    sameRevisionPending.presentationState = ThumbnailPresentationState::Loading;
    sameRevisionPending.presentationKey = BuildAssetThumbnailPresentationKey(changedItem);
    sameRevisionPending.requestRevision = canonical.requestRevision;
    ASSERT_TRUE(PromoteAssetThumbnailResultFromPresentationIndex(changedItem, sameRevisionPending))
        << "The canonical worker completes the same revision that the UI keeps Pending.";
    ASSERT_TRUE(sameRevisionPending.cacheEntry.has_value());
    EXPECT_EQ(sameRevisionPending.cacheEntry->cacheKey, canonicalEntry->cacheKey);

    AssetThumbnailServiceResult changedPending;
    changedPending.status = AssetThumbnailServiceStatus::Pending;
    changedPending.presentationState = ThumbnailPresentationState::Loading;
    changedPending.presentationKey = BuildAssetThumbnailPresentationKey(changedItem);
    changedPending.requestRevision = canonical.requestRevision + 1u;
    EXPECT_FALSE(PromoteAssetThumbnailResultFromPresentationIndex(changedItem, changedPending))
        << "A newer opaque item request must not revive an older canonical artifact.";

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, PresentationIndexRejectsIncompleteCanonicalEntry)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, "texture:Incomplete", "source:v1");
    request.kind = AssetThumbnailKind::Texture;
    request.requestRevision = 1u;
    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());
    ASSERT_TRUE(WriteAssetThumbnailCacheFile(request, entry->imagePath, TinyPng()));
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Fresh, {}));

    const auto complete = ReadAssetThumbnailPresentationIndex(request);
    ASSERT_TRUE(complete.has_value());
    ASSERT_TRUE(complete->current.has_value());

    std::filesystem::remove(entry->metadataPath);
    const auto missingMetadata = ReadAssetThumbnailPresentationIndex(request);
    ASSERT_TRUE(missingMetadata.has_value());
    EXPECT_FALSE(missingMetadata->current.has_value());

    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Fresh, {}));
    std::ifstream metadataInput(entry->metadataPath, std::ios::binary);
    ASSERT_TRUE(metadataInput.is_open());
    nlohmann::json metadata;
    metadataInput >> metadata;
    metadataInput.close();
    metadata["presentationKey"] = "wrong-presentation-key";
    const auto text = metadata.dump(2);
    WriteBinaryFile(entry->metadataPath, std::vector<uint8_t>(text.begin(), text.end()));

    const auto mismatchedMetadata = ReadAssetThumbnailPresentationIndex(request);
    ASSERT_TRUE(mismatchedMetadata.has_value());
    EXPECT_FALSE(mismatchedMetadata->current.has_value());

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, DeletedPresentationRemovesBothCanonicalGenerations)
{
    using namespace NLS::Editor::Assets;
    const auto root = MakeAssetThumbnailCacheRoot();
    auto first = MakeThumbnailRequest(root, "prefab:Deleted", "source:v1");
    first.sourceAssetPath = "Assets/Prefabs/Deleted.prefab";
    first.requestRevision = 1u;
    const auto firstEntry = ResolveAssetThumbnailCacheEntry(first);
    ASSERT_TRUE(firstEntry.has_value());
    ASSERT_TRUE(WriteAssetThumbnailCacheFile(first, firstEntry->imagePath, TinyPng()));
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(first, AssetThumbnailCacheStatus::Fresh, {}));

    auto second = first;
    second.freshnessInputs = {{"source", "source:v2"}};
    second.requestRevision = 2u;
    const auto secondEntry = ResolveAssetThumbnailCacheEntry(second);
    ASSERT_TRUE(secondEntry.has_value());
    ASSERT_TRUE(WriteAssetThumbnailCacheFile(second, secondEntry->imagePath, TinyPng()));
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(second, AssetThumbnailCacheStatus::Fresh, {}));
    ASSERT_TRUE(ReadAssetThumbnailPresentationIndex(second).has_value());

    EXPECT_TRUE(RemoveAssetThumbnailPresentation(second));
    EXPECT_FALSE(std::filesystem::exists(firstEntry->imagePath));
    EXPECT_FALSE(std::filesystem::exists(firstEntry->metadataPath));
    EXPECT_FALSE(std::filesystem::exists(secondEntry->imagePath));
    EXPECT_FALSE(std::filesystem::exists(secondEntry->metadataPath));
    EXPECT_FALSE(ReadAssetThumbnailPresentationIndex(second).has_value());

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, DeletedFolderRemovesAllSourcePathThumbnailRecords)
{
    using namespace NLS::Editor::Assets;
    const auto root = MakeAssetThumbnailCacheRoot();
    auto makeRequest = [&](const std::string& id, const std::string& path)
    {
        auto request = MakeThumbnailRequest(root, id, "source:v1");
        request.sourceAssetPath = path;
        request.requestRevision = 1u;
        const auto entry = ResolveAssetThumbnailCacheEntry(request);
        EXPECT_TRUE(entry.has_value());
        EXPECT_TRUE(WriteAssetThumbnailCacheFile(request, entry->imagePath, TinyPng()));
        EXPECT_TRUE(WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Fresh, {}));
        return entry;
    };
    const auto firstEntry = makeRequest(
        "prefab:FolderA",
        "Assets/DeletedFolder/A.prefab");
    const auto secondEntry = makeRequest(
        "prefab:FolderB",
        "Assets/DeletedFolder/Nested/B.prefab");
    ASSERT_TRUE(firstEntry.has_value());
    ASSERT_TRUE(secondEntry.has_value());

    const auto removed = RemoveAssetThumbnailCachesForSourcePath(
        root,
        {},
        "Assets/DeletedFolder",
        true);
    EXPECT_GE(removed, 6u);
    EXPECT_FALSE(std::filesystem::exists(firstEntry->imagePath));
    EXPECT_FALSE(std::filesystem::exists(firstEntry->metadataPath));
    EXPECT_FALSE(std::filesystem::exists(secondEntry->imagePath));
    EXPECT_FALSE(std::filesystem::exists(secondEntry->metadataPath));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ResidentRequestRequiresExactFreshnessFingerprint)
{
    using namespace NLS::Editor::Assets;
    const auto root = MakeAssetThumbnailCacheRoot();
    AssetBrowserItem item;
    item.kind = AssetBrowserItemKind::SourceAsset;
    item.type = AssetBrowserItemType::Prefab;
    item.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("d4040404-0404-4404-8404-040404040404"));
    item.sourceAssetPath = "Assets/Prefabs/Resident.prefab";
    item.subAssetKey = "prefab:Resident";
    item.artifactPath = "Library/Artifacts/aa/resident";

    AssetThumbnailRequestBuildContext coldContext;
    coldContext.deferManifestLookups = true;
    const auto coldRequest = BuildAssetThumbnailRequestForItem(root, item, 96u, coldContext);
    ASSERT_TRUE(coldRequest.has_value());

    auto registry = ResidentPrefabPreviewRegistry::Create();
    registry->RegisterSnapshot(
        BuildResidentPrefabRuntimeCacheIdentity(
            coldRequest->assetId.ToString(),
            coldRequest->subAssetKey),
        coldRequest->dependencyStamp,
        std::make_shared<PreviewRenderableSnapshot>(),
        1u);

    AssetThumbnailRequestBuildContext residentContext;
    residentContext.deferManifestLookups = true;
    residentContext.residentPrefabPreviewRegistry = registry;
    const auto residentRequest = BuildAssetThumbnailRequestForItem(root, item, 96u, residentContext);
    ASSERT_TRUE(residentRequest.has_value());
    ASSERT_TRUE(residentRequest->residentPrefabPreviewSource.has_value());
    EXPECT_EQ(
        residentRequest->residentPrefabPreviewSource->freshnessFingerprint,
        residentRequest->dependencyStamp);
    auto residentLease = registry->Acquire(
        residentRequest->residentPrefabPreviewSource->runtimeCacheIdentity,
        residentRequest->residentPrefabPreviewSource->freshnessFingerprint);
    ASSERT_TRUE(residentLease.has_value());
    EXPECT_EQ(registry->GetStats().zeroArtifactReadHitCount, 1u);

    auto staleRegistry = ResidentPrefabPreviewRegistry::Create();
    staleRegistry->RegisterSnapshot(
        BuildResidentPrefabRuntimeCacheIdentity(
            coldRequest->assetId.ToString(),
            coldRequest->subAssetKey),
        "different-freshness",
        std::make_shared<PreviewRenderableSnapshot>(),
        1u);
    AssetThumbnailRequestBuildContext staleContext;
    staleContext.deferManifestLookups = true;
    staleContext.residentPrefabPreviewRegistry = staleRegistry;
    const auto staleRequest = BuildAssetThumbnailRequestForItem(root, item, 96u, staleContext);
    ASSERT_TRUE(staleRequest.has_value());
    ASSERT_TRUE(staleRequest->residentPrefabPreviewSource.has_value());
    EXPECT_TRUE(staleRequest->residentPrefabPreviewSource->snapshot.expired());
    EXPECT_EQ(
        staleRequest->residentPrefabPreviewSource->freshnessFingerprint,
        staleRequest->dependencyStamp);
    EXPECT_EQ(staleRegistry->GetStats().staleCount, 0u);

    EXPECT_FALSE(staleRegistry->Acquire(
        BuildResidentPrefabRuntimeCacheIdentity(
            coldRequest->assetId.ToString(),
            coldRequest->subAssetKey),
        coldRequest->dependencyStamp));
    EXPECT_EQ(staleRegistry->GetStats().staleCount, 1u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ResidentSceneRegistrationNormalizesMissingPrefabSubAssetKey)
{
    using namespace NLS::Editor::Assets;
    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("e5050505-0505-4505-8505-050505050505"));
    const auto sourcePath = std::string("Assets/Prefabs/SceneResident.prefab");
    const auto canonicalSubAssetKey = BuildCanonicalPrefabPreviewSubAssetKey(sourcePath, {});

    EXPECT_EQ(canonicalSubAssetKey, "prefab:SceneResident");
    EXPECT_EQ(
        BuildResidentPrefabRuntimeCacheIdentity(assetId.ToString(), canonicalSubAssetKey),
        BuildResidentPrefabRuntimeCacheIdentity(assetId.ToString(), "prefab:SceneResident"));

    AssetBrowserItem item;
    item.kind = AssetBrowserItemKind::SourceAsset;
    item.type = AssetBrowserItemType::Prefab;
    item.assetId = assetId;
    item.sourceAssetPath = sourcePath;
    item.artifactPath = "Library/Artifacts/ee/resident";

    AssetThumbnailRequestBuildContext context;
    context.deferManifestLookups = true;
    // Resident-aware request construction may derive the canonical prefab key
    // before the deferred manifest lookup so it can attach a resident source.
    context.residentPrefabPreviewRegistry = ResidentPrefabPreviewRegistry::Create();
    const auto request = BuildAssetThumbnailRequestForItem(root, item, 96u, context);
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->subAssetKey, canonicalSubAssetKey);
    EXPECT_EQ(
        BuildResidentPrefabRuntimeCacheIdentity(request->assetId.ToString(), request->subAssetKey),
        BuildResidentPrefabRuntimeCacheIdentity(assetId.ToString(), canonicalSubAssetKey));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ResidentRequestNormalizesModelAliasWithoutChangingArtifactLookupKey)
{
    using namespace NLS::Editor::Assets;
    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("f6060606-0606-4606-8606-060606060606"));
    const auto sourcePath = std::string("Assets/Models/SceneResident.gltf");

    AssetBrowserItem item;
    item.kind = AssetBrowserItemKind::SourceAsset;
    item.type = AssetBrowserItemType::Model;
    item.assetId = assetId;
    item.sourceAssetPath = sourcePath;
    item.subAssetKey = "model:SceneResident";
    item.artifactPath = "Library/Artifacts/ff/resident";

    AssetThumbnailRequestBuildContext context;
    context.deferManifestLookups = true;
    const auto registry = ResidentPrefabPreviewRegistry::Create();
    context.residentPrefabPreviewRegistry = registry;
    const auto request = BuildAssetThumbnailRequestForItem(root, item, 96u, context);
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(request->residentPrefabPreviewSource.has_value());
    EXPECT_EQ(request->kind, AssetThumbnailKind::PrefabPreview);
    EXPECT_EQ(request->subAssetKey, item.subAssetKey);
    EXPECT_EQ(
        request->residentPrefabPreviewSource->runtimeCacheIdentity,
        BuildResidentPrefabRuntimeCacheIdentity(
            assetId.ToString(),
            "prefab:SceneResident"));
    EXPECT_EQ(request->artifactPath, item.artifactPath);
    EXPECT_EQ(
        BuildCanonicalPrefabPreviewSubAssetKey(sourcePath, item.subAssetKey),
        "prefab:SceneResident");

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ResidentRequestMatchesSceneAliasWithOriginalFreshnessKey)
{
    using namespace NLS::Editor::Assets;
    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("07070707-0707-4707-8707-070707070707"));
    const auto sourcePath = std::string("Assets/Models/SceneResident.gltf");
    const auto importerSubAssetKey = std::string("model:SceneResident");
    const auto artifactPath = std::string("Library/Artifacts/07/resident");

    AssetBrowserItem item;
    item.kind = AssetBrowserItemKind::SourceAsset;
    item.type = AssetBrowserItemType::Model;
    item.assetId = assetId;
    item.sourceAssetPath = sourcePath;
    item.subAssetKey = importerSubAssetKey;
    item.artifactPath = artifactPath;

    AssetThumbnailRequestBuildContext coldContext;
    coldContext.deferManifestLookups = true;
    const auto coldRequest = BuildAssetThumbnailRequestForItem(root, item, 96u, coldContext);
    ASSERT_TRUE(coldRequest.has_value());

    const auto registry = ResidentPrefabPreviewRegistry::Create();
    const auto canonicalSubAssetKey = BuildCanonicalPrefabPreviewSubAssetKey(
        sourcePath,
        importerSubAssetKey);
    registry->RegisterSnapshot(
        "prepared-runtime-identity",
        coldRequest->dependencyStamp,
        std::make_shared<PreviewRenderableSnapshot>(),
        1u,
        false,
        BuildResidentPrefabRuntimeCacheIdentity(assetId.ToString(), canonicalSubAssetKey));

    AssetThumbnailRequestBuildContext residentContext;
    residentContext.deferManifestLookups = true;
    residentContext.residentPrefabPreviewRegistry = registry;
    const auto residentRequest = BuildAssetThumbnailRequestForItem(
        root,
        item,
        96u,
        residentContext);
    ASSERT_TRUE(residentRequest.has_value());
    ASSERT_TRUE(residentRequest->residentPrefabPreviewSource.has_value());
    EXPECT_EQ(
        residentRequest->residentPrefabPreviewSource->freshnessFingerprint,
        coldRequest->dependencyStamp);

    const auto lease = registry->Acquire(
        residentRequest->residentPrefabPreviewSource->runtimeCacheIdentity,
        residentRequest->residentPrefabPreviewSource->freshnessFingerprint,
        true);
    ASSERT_TRUE(lease.has_value());
    EXPECT_EQ(registry->GetStats().thumbnailZeroArtifactReadHitCount, 1u);
    EXPECT_EQ(registry->GetStats().thumbnailFreshnessMismatchCount, 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, GpuPrefabPreparationWaitsForResidentResources)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect prefab preparation.";
#else
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto snapshot = std::make_shared<PreviewRenderableSnapshot>();
    snapshot->expectedDrawItemCount = 1u;
    PreviewDrawItem drawItem;
    drawItem.meshPath = "Library/Artifacts/resident-mesh";
    snapshot->drawItems.push_back(std::move(drawItem));

    const auto registry = ResidentPrefabPreviewRegistry::Create();
    const auto runtimeIdentity = BuildResidentPrefabRuntimeCacheIdentity(
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
        "prefab:Resident");
    constexpr const char* freshness = "resident-freshness:v1";
    registry->RegisterSnapshot(runtimeIdentity, freshness, snapshot, 1u);

    AssetThumbnailRequest request;
    request.projectRoot = root;
    request.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"));
    request.sourceAssetPath = "Assets/Prefabs/Resident.prefab";
    request.subAssetKey = "prefab:Resident";
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        runtimeIdentity,
        freshness,
        snapshot,
        registry
    };

    ResetAssetDatabasePrefabArtifactLoadCountForTesting();
    EXPECT_FALSE(ThumbnailPrefabPreparationUsesResidentSnapshotForTesting(request));
    EXPECT_EQ(GetAssetDatabasePrefabArtifactLoadCountForTesting(), 0u);
    EXPECT_EQ(registry->GetStats().zeroArtifactReadHitCount, 1u);

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailCacheTests, GpuPrefabPreparationWaitsForLateResidentSnapshotResources)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect prefab preparation.";
#else
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto snapshot = std::make_shared<PreviewRenderableSnapshot>();
    snapshot->expectedDrawItemCount = 1u;
    PreviewDrawItem drawItem;
    drawItem.meshPath = "Library/Artifacts/late-resident-mesh";
    snapshot->drawItems.push_back(std::move(drawItem));

    const auto registry = ResidentPrefabPreviewRegistry::Create();
    const auto runtimeIdentity = BuildResidentPrefabRuntimeCacheIdentity(
        "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
        "prefab:LateResident");
    constexpr const char* freshness = "late-resident-freshness:v1";

    AssetThumbnailRequest request;
    request.projectRoot = root;
    request.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"));
    request.sourceAssetPath = "Assets/Prefabs/LateResident.prefab";
    request.subAssetKey = "prefab:LateResident";
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        runtimeIdentity,
        freshness,
        {},
        registry
    };

    registry->SetSceneRestoreInProgress(true);
    ResetAssetDatabasePrefabArtifactLoadCountForTesting();
    EXPECT_FALSE(ThumbnailPrefabPreparationUsesResidentSnapshotForTesting(request));
    EXPECT_EQ(GetAssetDatabasePrefabArtifactLoadCountForTesting(), 0u);

    registry->RegisterSnapshot(runtimeIdentity, freshness, snapshot, 1u);
    registry->SetSceneRestoreInProgress(false);
    ResetAssetDatabasePrefabArtifactLoadCountForTesting();
    EXPECT_FALSE(ThumbnailPrefabPreparationUsesResidentSnapshotForTesting(request));
    EXPECT_EQ(GetAssetDatabasePrefabArtifactLoadCountForTesting(), 0u);
    EXPECT_EQ(registry->GetStats().thumbnailZeroArtifactReadHitCount, 1u);

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailCacheTests, DeferredPrefabResolutionRebindsResidentFreshnessWithoutArtifactRead)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect deferred resident preparation.";
#else
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetThumbnailCacheRoot();
    const auto assetId = AssetId(
        NLS::Guid::Parse("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"));
    const auto sourcePath = std::string("Assets/Models/DeferredResident.gltf");
    const auto subAssetKey = std::string("prefab:DeferredResident");
    const auto artifactPath =
        "Library/Artifacts/" + assetId.ToString() + "/DeferredResident.nprefab";

    WriteBinaryFile(root / sourcePath, std::vector<uint8_t>{'g', 'l', 't', 'f'});
    const auto artifactRoot = root / "Library" / "Artifacts" / assetId.ToString();
    WriteTextFile(
        artifactRoot / "manifest.json",
        "{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"primarySubAssetKey\":\"" + subAssetKey + "\","
        "\"subAssets\":[{"
        "\"sourceAssetId\":\"" + assetId.GetGuid().ToString() + "\","
        "\"subAssetKey\":\"" + subAssetKey + "\","
        "\"artifactType\":\"Prefab\","
        "\"artifactPath\":\"" + artifactPath + "\""
        "}]}" );

    const auto snapshot = std::make_shared<PreviewRenderableSnapshot>();
    snapshot->expectedDrawItemCount = 1u;
    PreviewDrawItem drawItem;
    drawItem.meshPath = "Library/Artifacts/resident-mesh";
    snapshot->drawItems.push_back(std::move(drawItem));

    const auto registry = ResidentPrefabPreviewRegistry::Create();
    const auto runtimeIdentity = BuildResidentPrefabRuntimeCacheIdentity(
        assetId.ToString(),
        subAssetKey);
    const auto initialFreshness = BuildPrefabThumbnailDependencyStamp(
        root,
        assetId,
        sourcePath,
        subAssetKey,
        {});

    AssetThumbnailRequest request;
    request.projectRoot = root;
    request.assetId = assetId;
    request.sourceAssetPath = sourcePath;
    request.subAssetKey = subAssetKey;
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        runtimeIdentity,
        initialFreshness,
        {},
        registry
    };

    const auto resolved = ResolveDeferredThumbnailPreviewRequestForTesting(request);
    ASSERT_FALSE(resolved.artifactPath.empty());
    ASSERT_TRUE(resolved.residentPrefabPreviewSource.has_value());
    EXPECT_EQ(
        resolved.residentPrefabPreviewSource->runtimeCacheIdentity,
        runtimeIdentity);
    const auto exactFreshness = BuildPrefabThumbnailDependencyStamp(
        root,
        assetId,
        sourcePath,
        resolved.subAssetKey.empty() ? subAssetKey : resolved.subAssetKey,
        resolved.artifactPath);
    EXPECT_EQ(
        resolved.residentPrefabPreviewSource->freshnessFingerprint,
        exactFreshness);

    registry->RegisterSnapshot(runtimeIdentity, exactFreshness, snapshot, 1u);

    ResetAssetDatabasePrefabArtifactLoadCountForTesting();
    EXPECT_FALSE(ThumbnailPrefabPreparationUsesResidentSnapshotForTesting(resolved));
    EXPECT_EQ(GetAssetDatabasePrefabArtifactLoadCountForTesting(), 0u);
    EXPECT_EQ(registry->GetStats().thumbnailZeroArtifactReadHitCount, 1u);

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetThumbnailCacheTests, ServiceInvalidationCancelsQueuedPresentation)
{
    using namespace NLS::Editor::Assets;
    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, "model:Deleted");
    request.requestRevision = 1u;
    AssetThumbnailService service;
    EXPECT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetQueuedRequestCount(), 1u);

    service.InvalidateThumbnail(request);

    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Cancelled);
    EXPECT_FALSE(ReadAssetThumbnailPresentationIndex(request).has_value());
    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceKeepsOnlyLatestPresentationRevisionQueued)
{
    using namespace NLS::Editor::Assets;
    const auto root = MakeAssetThumbnailCacheRoot();

    auto older = MakeThumbnailRequest(root, "prefab:Revisioned", "source:v1");
    older.requestRevision = 1u;
    auto newer = older;
    newer.freshnessInputs[0].stamp = "source:v2";
    newer.requestRevision = 2u;

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(older).status, AssetThumbnailServiceStatus::Pending);
    ASSERT_EQ(service.GetQueuedRequestCount(), 1u);
    ASSERT_EQ(service.GetThumbnailState(older), ThumbnailState::Queued);

    const auto newerResult = service.GetThumbnail(newer);
    EXPECT_EQ(newerResult.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);
    EXPECT_EQ(service.GetThumbnailState(older), ThumbnailState::Cancelled);
    EXPECT_EQ(service.GetThumbnailState(newer), ThumbnailState::Queued);

    const auto supersededResult = service.GetThumbnail(older);
    EXPECT_EQ(supersededResult.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(supersededResult.diagnostic, "thumbnail-request-superseded");
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, CompletedSupersededReadbackKeepsCacheKeyAliveDuringCleanup)
{
    using namespace NLS::Editor::Assets;
    const auto root = MakeAssetThumbnailCacheRoot();

    auto older = MakeThumbnailRequest(root, "material:RevisionedReadback", "source:v1");
    older.sourceAssetPath = "Assets/Materials/RevisionedReadback.mat";
    older.kind = AssetThumbnailKind::MaterialSphere;
    older.requestedSize = 48u;
    older.requestRevision = 1u;
    auto newer = older;
    newer.freshnessInputs[0].stamp = "source:v2";
    newer.requestRevision = 2u;

    CompletingAsyncThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(older).status, AssetThumbnailServiceStatus::Pending);
    const auto submitted = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(submitted.has_value());
    EXPECT_EQ(submitted->diagnostic, "thumbnail-gpu-preview-readback-pending");
    ASSERT_EQ(renderer.pendingTickets.size(), 1u);

    ASSERT_EQ(service.GetThumbnail(newer).status, AssetThumbnailServiceStatus::Pending);
    renderer.completeNextReadback = true;
    EXPECT_NO_THROW((void)service.GenerateNextThumbnail(renderer, true));
    EXPECT_EQ(service.GetThumbnailState(older), ThumbnailState::Cancelled);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ServiceInvalidationKeepsSubmittedReadbackPollable)
{
    using namespace NLS::Editor::Assets;
    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, "material:DeletedReadback");
    request.sourceAssetPath = "Assets/Materials/DeletedReadback.mat";
    request.kind = AssetThumbnailKind::MaterialSphere;
    request.requestedSize = 48u;
    request.requestRevision = 1u;

    PendingThenReadyThumbnailPreviewRenderer renderer;
    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);

    const auto submitted = service.GenerateNextThumbnail(renderer, true);
    ASSERT_TRUE(submitted.has_value());
    EXPECT_EQ(submitted->diagnostic, "thumbnail-gpu-preview-readback-pending");
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::WaitingForGpu);

    service.InvalidateThumbnail(request);

    // The renderer must be called once more so its submitted readback can be
    // retired, but the completed pixels must never be returned to the UI.
    const auto retired = service.GenerateNextThumbnail(renderer, true);
    EXPECT_FALSE(retired.has_value());
    EXPECT_EQ(renderer.renderCount, 2u);
    EXPECT_EQ(service.GetQueuedRequestCount(), 0u);
    EXPECT_EQ(service.GetThumbnailState(request), ThumbnailState::Cancelled);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, PresentationCommitRemovesDroppedThirdGeneration)
{
    using namespace NLS::Editor::Assets;
    const auto root = MakeAssetThumbnailCacheRoot();
    auto first = MakeThumbnailRequest(root, "prefab:Rotation", "source:v1");
    first.requestRevision = 1u;
    const auto firstEntry = ResolveAssetThumbnailCacheEntry(first);
    ASSERT_TRUE(firstEntry.has_value());
    WriteBinaryFile(firstEntry->imagePath, TinyPng());
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(first, AssetThumbnailCacheStatus::Fresh, {}));

    auto second = first;
    second.freshnessInputs = {{"source", "source:v2"}};
    second.requestRevision = 2u;
    const auto secondEntry = ResolveAssetThumbnailCacheEntry(second);
    ASSERT_TRUE(secondEntry.has_value());
    WriteBinaryFile(secondEntry->imagePath, TinyPng());
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(second, AssetThumbnailCacheStatus::Fresh, {}));

    auto third = second;
    third.freshnessInputs = {{"source", "source:v3"}};
    third.requestRevision = 3u;
    const auto thirdEntry = ResolveAssetThumbnailCacheEntry(third);
    ASSERT_TRUE(thirdEntry.has_value());
    WriteBinaryFile(thirdEntry->imagePath, TinyPng());
    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(third, AssetThumbnailCacheStatus::Fresh, {}));

    EXPECT_FALSE(std::filesystem::exists(firstEntry->imagePath));
    EXPECT_FALSE(std::filesystem::exists(firstEntry->metadataPath));
    EXPECT_TRUE(std::filesystem::exists(secondEntry->imagePath));
    EXPECT_TRUE(std::filesystem::exists(thirdEntry->imagePath));

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, CustomCacheRootIsUsedForCanonicalAndPresentationWrites)
{
    using namespace NLS::Editor::Assets;
    const auto root = MakeAssetThumbnailCacheRoot();
    auto request = MakeThumbnailRequest(root, "prefab:Hero", "source:v1");
    request.cacheRoot = root / "Library" / "CustomThumbnailCache";
    request.requestRevision = 1u;

    const auto entry = ResolveAssetThumbnailCacheEntry(request);
    ASSERT_TRUE(entry.has_value());
    EXPECT_TRUE(entry->imagePath.lexically_normal().string().find("CustomThumbnailCache") != std::string::npos);
    EXPECT_TRUE(WriteAssetThumbnailCacheFile(request, entry->imagePath, TinyPng()));
    EXPECT_TRUE(std::filesystem::is_regular_file(entry->imagePath));
    EXPECT_FALSE(std::filesystem::exists(root / "Library" / "AssetThumbnails"));

    ASSERT_TRUE(WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Fresh, {}));
    const auto presentation = ReadAssetThumbnailPresentationIndex(request);
    ASSERT_TRUE(presentation.has_value());
    ASSERT_TRUE(presentation->current.has_value());
    EXPECT_EQ(presentation->current->cacheKey, entry->cacheKey);
    EXPECT_TRUE(presentation->current->imagePath.lexically_normal().string().find("CustomThumbnailCache") != std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailCacheTests, ResidentPrefabRegistryLeasePreventsInactiveEviction)
{
    using namespace NLS::Editor::Assets;
    auto registry = ResidentPrefabPreviewRegistry::Create(1u);
    auto first = std::make_shared<PreviewRenderableSnapshot>();
    registry->RegisterSnapshot("prefab:hero", "deps:v1", first, 2u);
    auto lease = registry->Acquire("prefab:hero", "deps:v1");
    ASSERT_TRUE(lease.has_value());
    registry->SetInactiveBudgetBytes(0u);
    EXPECT_EQ(registry->GetStats().entryCount, 1u);
    lease.reset();
    EXPECT_EQ(registry->GetStats().entryCount, 0u);
}

TEST(AssetThumbnailCacheTests, ResidentPrefabRegistryTrimsOlderInactiveEntriesOnRegistration)
{
    using namespace NLS::Editor::Assets;
    auto registry = ResidentPrefabPreviewRegistry::Create(2u);
    registry->RegisterSnapshot(
        "prefab:first",
        "deps:v1",
        std::make_shared<PreviewRenderableSnapshot>(),
        2u);
    registry->RegisterSnapshot(
        "prefab:second",
        "deps:v1",
        std::make_shared<PreviewRenderableSnapshot>(),
        2u);

    EXPECT_EQ(registry->GetStats().entryCount, 1u);
    EXPECT_EQ(registry->GetStats().residentBytes, 2u);
    EXPECT_FALSE(registry->Acquire("prefab:first", "deps:v1").has_value());
    EXPECT_TRUE(registry->Acquire("prefab:second", "deps:v1").has_value());
}

TEST(AssetThumbnailCacheTests, ResidentRegistrySeparatesThumbnailAcquireTelemetry)
{
    using namespace NLS::Editor::Assets;
    auto registry = ResidentPrefabPreviewRegistry::Create();
    auto snapshot = std::make_shared<PreviewRenderableSnapshot>();
    registry->RegisterSnapshot("prefab:hero", "deps:v1", snapshot, 2u);

    auto sceneLease = registry->Acquire("prefab:hero", "deps:v1");
    ASSERT_TRUE(sceneLease.has_value());
    EXPECT_EQ(registry->GetStats().hitCount, 1u);
    EXPECT_EQ(registry->GetStats().thumbnailHitCount, 0u);

    auto thumbnailLease = registry->Acquire("prefab:hero", "deps:v1", true);
    ASSERT_TRUE(thumbnailLease.has_value());
    EXPECT_EQ(registry->GetStats().thumbnailHitCount, 1u);
    EXPECT_EQ(registry->GetStats().thumbnailZeroArtifactReadHitCount, 1u);

    EXPECT_FALSE(registry->Acquire("prefab:hero", "deps:v2", true).has_value());
    EXPECT_EQ(registry->GetStats().thumbnailMissCount, 1u);
    EXPECT_EQ(registry->GetStats().thumbnailStaleCount, 1u);
}

TEST(AssetThumbnailCacheTests, ResidentRegistrySupportsDeferredArtifactFreshnessAlias)
{
    using namespace NLS::Editor::Assets;
    auto registry = ResidentPrefabPreviewRegistry::Create();
    auto snapshot = std::make_shared<PreviewRenderableSnapshot>();

    registry->RegisterSnapshot(
        "runtime:prefab:hero",
        "full:source:v1;artifact-file:v1;",
        snapshot,
        2u,
        false,
        "prefab:hero:prefab:Hero",
        "deferred:source:v1;artifact-db:v1;");

    auto deferredLease = registry->Acquire(
        "prefab:hero:prefab:Hero",
        "deferred:source:v1;artifact-db:v1;",
        true);
    ASSERT_TRUE(deferredLease.has_value());
    EXPECT_EQ(registry->GetStats().thumbnailHitCount, 1u);
    EXPECT_EQ(registry->GetStats().thumbnailZeroArtifactReadHitCount, 1u);

    auto completeLease = registry->Acquire(
        "prefab:hero:prefab:Hero",
        "full:source:v1;artifact-file:v1;",
        true);
    ASSERT_TRUE(completeLease.has_value());
    EXPECT_EQ(registry->GetStats().thumbnailHitCount, 2u);

    EXPECT_FALSE(registry->Acquire(
        "prefab:hero:prefab:Hero",
        "deferred:source:v2;artifact-db:v1;",
        true).has_value());
    EXPECT_EQ(registry->GetStats().thumbnailFreshnessMismatchCount, 1u);
}

TEST(AssetThumbnailCacheTests, ResidentRegistryAggregatesThumbnailRequestIdentities)
{
    using namespace NLS::Editor::Assets;
    auto registry = ResidentPrefabPreviewRegistry::Create();

    registry->RecordThumbnailRequest("prefab:hero:prefab:Hero", "deps:v1");
    registry->RecordThumbnailRequest("prefab:hero:prefab:Hero", "deps:v1");
    registry->RecordThumbnailRequest("prefab:other:prefab:Other", "deps:v1");

    const auto stats = registry->GetStats();
    EXPECT_EQ(stats.thumbnailRequestCount, 3u);
    ASSERT_EQ(stats.thumbnailRequestIdentityCounts.size(), 2u);
    EXPECT_EQ(stats.thumbnailRequestIdentityCounts[0].identity, "prefab:hero:prefab:Hero");
    EXPECT_EQ(stats.thumbnailRequestIdentityCounts[0].count, 2u);
    EXPECT_EQ(stats.thumbnailRequestIdentityCounts[1].identity, "prefab:other:prefab:Other");
    EXPECT_EQ(stats.thumbnailRequestIdentityCounts[1].count, 1u);
    EXPECT_EQ(stats.thumbnailRequestOtherIdentityCount, 0u);
}

TEST(AssetThumbnailCacheTests, ResidentRegistryTracksSceneRestoreState)
{
    using namespace NLS::Editor::Assets;
    const auto registry = ResidentPrefabPreviewRegistry::Create();

    EXPECT_FALSE(registry->IsSceneRestoreInProgress());
    registry->SetSceneRestoreInProgress(true);
    EXPECT_TRUE(registry->IsSceneRestoreInProgress());
    registry->SetSceneRestoreInProgress(false);
    EXPECT_FALSE(registry->IsSceneRestoreInProgress());
}

TEST(AssetThumbnailCacheTests, ResidentRegistryReportsPartialSnapshotRevisionAndCompletion)
{
    using namespace NLS::Editor::Assets;

    const auto registry = ResidentPrefabPreviewRegistry::Create();
    auto partial = std::make_shared<PreviewRenderableSnapshot>();
    partial->drawItems.resize(1u);
    partial->expectedDrawItemCount = 2u;
    registry->RegisterSnapshot("runtime:partial", "deps:v1", partial, 1u);

    const auto first = registry->GetSnapshotState("runtime:partial", "deps:v1");
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->revision, 1u);
    EXPECT_EQ(first->readyDrawItemCount, 1u);
    EXPECT_EQ(first->expectedDrawItemCount, 2u);
    EXPECT_FALSE(first->complete);

    auto complete = std::make_shared<PreviewRenderableSnapshot>();
    complete->drawItems.resize(2u);
    complete->expectedDrawItemCount = 2u;
    registry->RegisterSnapshot("runtime:partial", "deps:v1", complete, 2u);

    const auto second = registry->GetSnapshotState("runtime:partial", "deps:v1");
    ASSERT_TRUE(second.has_value());
    EXPECT_GT(second->revision, first->revision);
    EXPECT_EQ(second->readyDrawItemCount, 2u);
    EXPECT_EQ(second->expectedDrawItemCount, 2u);
    EXPECT_TRUE(second->complete);
}

TEST(AssetThumbnailCacheTests, LegacyThumbnailServiceDoesNotRecordResidentRequests)
{
    using namespace NLS::Editor::Assets;
    const auto registry = ResidentPrefabPreviewRegistry::Create();

    AssetThumbnailRequest request;
    request.projectRoot = std::filesystem::temp_directory_path();
    request.sourceAssetPath = "Assets/hero.prefab";
    request.kind = AssetThumbnailKind::PrefabPreview;
    request.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
        "prefab:hero:prefab:Hero",
        "deps:v1",
        {},
        registry
    };

    AssetThumbnailService legacyService(AssetThumbnailFeatureConfig::Legacy());
    (void)legacyService.GetThumbnail(request);
    EXPECT_EQ(registry->GetStats().thumbnailRequestCount, 0u);
}

TEST(AssetThumbnailCacheTests, ResidentPrefabRegistryAliasAcquiresExactRuntimeEntry)
{
    using namespace NLS::Editor::Assets;
    auto registry = ResidentPrefabPreviewRegistry::Create(1u);
    auto snapshot = std::make_shared<PreviewRenderableSnapshot>();
    registry->RegisterSnapshot(
        "project|hero|manifest@v2|prefab@stamp",
        "deps:v2",
        snapshot,
        2u,
        false,
        "prefab:hero:prefab:Hero");

    auto lease = registry->Acquire("prefab:hero:prefab:Hero", "deps:v2");
    ASSERT_TRUE(lease.has_value());
    EXPECT_EQ(lease->Snapshot(), snapshot);

    registry->SetInactiveBudgetBytes(0u);
    EXPECT_EQ(registry->GetStats().entryCount, 1u);
    lease.reset();
    EXPECT_EQ(registry->GetStats().entryCount, 0u);
    EXPECT_FALSE(registry->Acquire("prefab:hero:prefab:Hero", "deps:v2").has_value());
}

TEST(AssetThumbnailCacheTests, ResidentSceneLeaseAllowsThumbnailFreshnessFallback)
{
    using namespace NLS::Editor::Assets;
    auto registry = ResidentPrefabPreviewRegistry::Create();
    auto snapshot = std::make_shared<PreviewRenderableSnapshot>();
    snapshot->drawItems.resize(1u);
    snapshot->expectedDrawItemCount = 1u;

    registry->RegisterSnapshot(
        "runtime:hero",
        "scene-freshness:v1",
        snapshot,
        2u,
        true);

    auto thumbnailLease = registry->Acquire(
        "runtime:hero",
        "request-freshness:v2",
        true);
    ASSERT_TRUE(thumbnailLease.has_value());
    EXPECT_EQ(thumbnailLease->Snapshot(), snapshot);
    const auto state = registry->GetSnapshotState("runtime:hero", "request-freshness:v2");
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->revision, 1u);
}

TEST(AssetThumbnailCacheTests, ResidentStateFallbackUsesMostRecentSceneEntry)
{
    using namespace NLS::Editor::Assets;
    const auto registry = ResidentPrefabPreviewRegistry::Create();

    auto partial = std::make_shared<PreviewRenderableSnapshot>();
    partial->drawItems.resize(1u);
    partial->expectedDrawItemCount = 2u;
    registry->RegisterSnapshot(
        "runtime:hero",
        "scene-freshness:v1",
        partial,
        1u,
        true);

    auto complete = std::make_shared<PreviewRenderableSnapshot>();
    complete->drawItems.resize(2u);
    complete->expectedDrawItemCount = 2u;
    registry->RegisterSnapshot(
        "runtime:hero",
        "scene-freshness:v2",
        complete,
        2u,
        true);

    const auto lease = registry->Acquire(
        "runtime:hero",
        "request-freshness:v3",
        true);
    ASSERT_TRUE(lease.has_value());
    EXPECT_EQ(lease->Snapshot(), complete);

    const auto state = registry->GetSnapshotState(
        "runtime:hero",
        "request-freshness:v3");
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->readyDrawItemCount, 2u);
    EXPECT_EQ(state->expectedDrawItemCount, 2u);
    EXPECT_TRUE(state->complete);
}

TEST(AssetThumbnailCacheTests, InactiveEntryStillRequiresExactThumbnailFreshness)
{
    using namespace NLS::Editor::Assets;
    auto registry = ResidentPrefabPreviewRegistry::Create();
    auto snapshot = std::make_shared<PreviewRenderableSnapshot>();
    snapshot->drawItems.resize(1u);
    snapshot->expectedDrawItemCount = 1u;
    registry->RegisterSnapshot("runtime:hero", "scene-freshness:v1", snapshot, 2u);

    EXPECT_FALSE(registry->Acquire("runtime:hero", "request-freshness:v2", true).has_value());
    EXPECT_FALSE(registry->GetSnapshotState("runtime:hero", "request-freshness:v2").has_value());
}

TEST(AssetThumbnailCacheTests, ThumbnailPreviewProxyPoolReusesObjectsAndKeepsLeasesExclusive)
{
    using namespace NLS::Editor::Assets;

    NLS::Engine::SceneSystem::Scene scene;
    ThumbnailPreviewProxyPool pool(scene, 1u);

    auto first = pool.Acquire("thumbnail-proxy-first");
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(static_cast<bool>(*first));
    auto* firstObject = first->Get();
    const auto firstSerial = first->Serial();
    EXPECT_EQ(pool.GetObjectCount(), 1u);
    EXPECT_EQ(pool.GetActiveLeaseCount(), 1u);
    EXPECT_EQ(pool.GetAllocationCount(), 1u);
    EXPECT_FALSE(pool.Acquire("thumbnail-proxy-over-capacity").has_value());

    first.reset();
    EXPECT_EQ(pool.GetActiveLeaseCount(), 0u);

    auto second = pool.Acquire("thumbnail-proxy-second");
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->Get(), firstObject);
    EXPECT_NE(second->Serial(), firstSerial);
    EXPECT_EQ(pool.GetReuseHitCount(), 1u);
    EXPECT_EQ(pool.GetAllocationCount(), 1u);
}

TEST(AssetThumbnailCacheTests, MaterialPreviewBindsReadyAsyncTextureDependencies)
{
    NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::TextureManager textureManager;
    ScopedThumbnailServiceOverride<NLS::Core::ResourceManagement::TextureManager> textureService(textureManager);

    NLS::Render::Resources::Material material;
    const std::string texturePath =
        "Library/Artifacts/aa/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    auto* texture = NLS::Render::Resources::Texture2D::WrapExternal(
        std::make_shared<ThumbnailReadyTexture>(),
        1u,
        1u).release();
    textureManager.RegisterResource(texturePath, texture);

    material.SetTextureResourcePath("_BaseMap", texturePath);
    material.SetRawParameter("_BaseMap", static_cast<NLS::Render::Resources::Texture2D*>(nullptr));

    const auto requestCountBefore =
        NLS::Core::ResourceManagement::TextureManager::GetTotalAsyncArtifactRequestCountForTesting();
    ASSERT_TRUE(NLS::Editor::Assets::BindReadyMaterialPreviewTexturesForTesting(material));
    EXPECT_EQ(
        NLS::Core::ResourceManagement::TextureManager::GetTotalAsyncArtifactRequestCountForTesting(),
        requestCountBefore)
        << "Ready preview textures must be reused instead of re-requested through the async loader.";

    const auto& uniforms = material.GetUniformsData();
    const auto uniform = uniforms.find("_BaseMap");
    ASSERT_NE(uniform, uniforms.end());
    const auto* value = std::any_cast<NLS::Render::Resources::Texture2D*>(&uniform->second);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, texture);
}

TEST(AssetThumbnailCacheTests, MaterialPreviewKeepsReadyTextureBindingOnRepeatCall)
{
    NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::TextureManager textureManager;
    ScopedThumbnailServiceOverride<NLS::Core::ResourceManagement::TextureManager> textureService(textureManager);

    NLS::Render::Resources::Material material;
    const std::string texturePath =
        "Library/Artifacts/bb/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    auto* texture = NLS::Render::Resources::Texture2D::WrapExternal(
        std::make_shared<ThumbnailReadyTexture>(),
        1u,
        1u).release();
    textureManager.RegisterResource(texturePath, texture);

    material.SetTextureResourcePath("_BaseMap", texturePath);
    material.SetRawParameter("_BaseMap", static_cast<NLS::Render::Resources::Texture2D*>(nullptr));

    const auto requestCountBefore =
        NLS::Core::ResourceManagement::TextureManager::GetTotalAsyncArtifactRequestCountForTesting();
    ASSERT_TRUE(NLS::Editor::Assets::BindReadyMaterialPreviewTexturesForTesting(material));
    ASSERT_TRUE(NLS::Editor::Assets::BindReadyMaterialPreviewTexturesForTesting(material));
    EXPECT_EQ(
        NLS::Core::ResourceManagement::TextureManager::GetTotalAsyncArtifactRequestCountForTesting(),
        requestCountBefore)
        << "Repeated material preview checks should keep the ready texture binding and avoid new async requests.";

    const auto& uniforms = material.GetUniformsData();
    const auto uniform = uniforms.find("_BaseMap");
    ASSERT_NE(uniform, uniforms.end());
    const auto* value = std::any_cast<NLS::Render::Resources::Texture2D*>(&uniform->second);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, texture);
}
#endif
