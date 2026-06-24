#include <gtest/gtest.h>

#include "Assets/AssetThumbnailCache.h"
#include "Assets/AssetThumbnailService.h"
#include "Assets/AssetId.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/NativeArtifactContainer.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Serialize/ObjectGraphWriter.h"
#include "Guid.h"
#include "Image.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/RHI/RHITypes.h"
#include <Json/json.hpp>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
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

void WriteBinaryFile(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "png";
}

void WriteBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

void WriteTextFile(const std::filesystem::path& path, const std::string& contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
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

NLS::Render::Assets::MeshArtifactData OversizedMeshArtifact()
{
    NLS::Render::Assets::MeshArtifactData mesh;
    constexpr size_t triangleCount = 9000u;
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

void ExpectGpuPreviewDefersWithoutRenderer(
    const std::filesystem::path& root,
    NLS::Editor::Assets::AssetThumbnailRequest request)
{
    using namespace NLS::Editor::Assets;

    AssetThumbnailService service;
    ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_EQ(generated->diagnostic, "thumbnail-gpu-preview-renderer-unavailable");
    ASSERT_TRUE(generated->cacheEntry.has_value());
    EXPECT_FALSE(std::filesystem::exists(generated->cacheEntry->imagePath));
    EXPECT_EQ(EvaluateAssetThumbnailCache(request).status, AssetThumbnailCacheStatus::Missing);
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
    ASSERT_TRUE(generated.has_value());
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
    EXPECT_EQ(materialRequest->sourceAssetPath, "Assets/Models/Hero.gltf");
    EXPECT_EQ(materialRequest->subAssetKey, "material:Body");
    EXPECT_EQ(materialRequest->artifactPath, "Library/Artifacts/47/47b24ab4b128645b99328e0a68370de1202b0ba370eafc30e8bb0b0b7cf8b5ae");

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
    EXPECT_EQ(
        prefabRequest->artifactPath,
        "Library/Artifacts/67/670d35a0d13abf40dfcf953b26cff38db2ba16c57287f484aa491e4fcb490772");

    AssetBrowserItem folder;
    folder.kind = AssetBrowserItemKind::Folder;
    folder.type = AssetBrowserItemType::Folder;
    EXPECT_FALSE(BuildAssetThumbnailRequestForItem(root, folder, 96u).has_value());

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

TEST(AssetThumbnailCacheTests, ServiceDefersGpuPreviewThumbnailsWithoutRenderer)
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
    ASSERT_TRUE(firstGenerated.has_value());
    EXPECT_EQ(firstGenerated->status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_EQ(firstGenerated->diagnostic, "thumbnail-gpu-preview-renderer-unavailable");
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
    const auto artifactDatabasePath = root / "Library" / "ArtifactDB";
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
    const auto artifactDatabasePath = root / "Library" / "ArtifactDB";
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

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto generated = service.GenerateNextThumbnail();
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

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto generated = service.GenerateNextThumbnail();
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

TEST(AssetThumbnailCacheTests, ServiceRecordsOversizedMeshPreviewAsStableFailure)
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
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_EQ(generated->diagnostic, "thumbnail-gpu-preview-renderer-unavailable");
    EXPECT_FALSE(std::filesystem::exists(entry->metadataPath));
    EXPECT_FALSE(std::filesystem::exists(entry->imagePath));
    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead),
        0u);
    EXPECT_EQ(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy),
        0u);
    EXPECT_EQ(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::CpuDeserialize),
        0u);

    const auto evaluated = EvaluateAssetThumbnailCache(request);
    EXPECT_EQ(evaluated.status, AssetThumbnailCacheStatus::Missing);

    const auto repeated = service.GetThumbnail(request);
    EXPECT_EQ(repeated.status, AssetThumbnailServiceStatus::Pending);
    EXPECT_EQ(service.GetQueuedRequestCount(), 1u);

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
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_EQ(generated->diagnostic, "thumbnail-gpu-preview-renderer-unavailable");

    const auto telemetry = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(
        CountArtifactTelemetryStage(telemetry, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead),
        0u);
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
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_EQ(generated->diagnostic, "thumbnail-gpu-preview-renderer-unavailable");
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

TEST(AssetThumbnailCacheTests, ServiceRecordsOversizedPrefabPreviewBeforePayloadCopy)
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
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_EQ(generated->diagnostic, "thumbnail-gpu-preview-renderer-unavailable");
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
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_EQ(generated->diagnostic, "thumbnail-gpu-preview-renderer-unavailable");

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
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fallback);
    EXPECT_EQ(generated->diagnostic, "thumbnail-gpu-preview-renderer-unavailable");

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

TEST(AssetThumbnailCacheTests, ServiceKeepsGpuPreviewThumbnailsPendingWithoutRenderer)
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
    ExpectGpuPreviewDefersWithoutRenderer(root, materialRequest);

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
