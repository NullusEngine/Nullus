#include "Assets/AssetThumbnailService.h"

#include "Assets/AssetMeta.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/EditorAssetManifestJson.h"
#include "Assets/EditorAssetPath.h"
#include "Assets/NativeArtifactContainer.h"
#include "Image.h"
#include "Serialize/ObjectGraphReader.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/RHI/RHITypes.h"

#define STBIWDEF static
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define stbiw__linear_to_rgbe nls_asset_thumbnail_service_stbiw__linear_to_rgbe
#define stbiw__write_run_data nls_asset_thumbnail_service_stbiw__write_run_data
#define stbiw__write_dump_data nls_asset_thumbnail_service_stbiw__write_dump_data
#define stbiw__write_hdr_scanline nls_asset_thumbnail_service_stbiw__write_hdr_scanline
#define stbi_zlib_compress nls_asset_thumbnail_service_stbi_zlib_compress
#define stbi_write_png_to_mem nls_asset_thumbnail_service_stbi_write_png_to_mem
#include <stb/stb_image_write.h>
#undef stbi_write_png_to_mem
#undef stbi_zlib_compress
#undef stbiw__write_hdr_scanline
#undef stbiw__write_dump_data
#undef stbiw__write_run_data
#undef stbiw__linear_to_rgbe

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

namespace NLS::Editor::Assets
{
namespace
{
using AssetThumbnailCancelToken = std::weak_ptr<AssetThumbnailGenerationCancelToken>;
using AssetThumbnailGenerator = AssetThumbnailServiceResult (*)(const AssetThumbnailRequest&, const AssetThumbnailCancelToken&);

AssetThumbnailServiceResult GenerateTextureThumbnail(const AssetThumbnailRequest& request, const AssetThumbnailCancelToken& cancelToken);
AssetThumbnailServiceResult GenerateMaterialThumbnail(const AssetThumbnailRequest& request, const AssetThumbnailCancelToken& cancelToken);
AssetThumbnailServiceResult GenerateModelThumbnail(const AssetThumbnailRequest& request, const AssetThumbnailCancelToken& cancelToken);
AssetThumbnailServiceResult GeneratePrefabThumbnail(const AssetThumbnailRequest& request, const AssetThumbnailCancelToken& cancelToken);

struct AssetThumbnailKindPolicy
{
    AssetThumbnailKind kind = AssetThumbnailKind::GenericPreview;
    const char* fallbackIcon = "Icon_Unknown";
    AssetThumbnailGenerator generator = nullptr;
    const char* unsupportedDiagnostic = "thumbnail-generation-unsupported";
};

constexpr std::array<AssetThumbnailKindPolicy, kAssetThumbnailKindCount> kAssetThumbnailKindPolicies {{
    { AssetThumbnailKind::Icon, "Icon_Unknown", nullptr, "thumbnail-generation-unsupported" },
    { AssetThumbnailKind::Texture, "Icon_Texture", GenerateTextureThumbnail, "thumbnail-generation-unsupported" },
    { AssetThumbnailKind::MaterialSphere, "Icon_Material", GenerateMaterialThumbnail, "thumbnail-material-preview-generation-failed" },
    { AssetThumbnailKind::ModelPreview, "Icon_Model", GenerateModelThumbnail, "thumbnail-model-preview-generation-failed" },
    { AssetThumbnailKind::PrefabPreview, "Icon_Model", GeneratePrefabThumbnail, "thumbnail-prefab-preview-generation-failed" },
    { AssetThumbnailKind::GenericPreview, "Icon_Unknown", nullptr, "thumbnail-generation-unsupported" }
}};

constexpr size_t kMaxMeshPreviewVertices = 20000u;
constexpr size_t kMaxMeshPreviewTriangles = 8000u;
constexpr size_t kMaxObsoleteThumbnailGenerationInFlightRequests = 2u;
constexpr size_t kMaxThumbnailGenerationTotalInFlightSlots =
    kMaxObsoleteThumbnailGenerationInFlightRequests + 1u;
constexpr uint64_t kMaxSourceThumbnailImageBytes = 128ull * 1024ull * 1024ull;
constexpr uint64_t kMaxSourceThumbnailPixels = 4096ull * 4096ull;
constexpr uint64_t kMaxStructurePreviewArtifactPayloadBytes = 1024ull * 1024ull;
constexpr uint64_t kMaxThumbnailPreviewNativeArtifactFileBytes = 128ull * 1024ull * 1024ull;
constexpr const char* kMeshPreviewBudgetExceededDiagnostic =
    "thumbnail-model-mesh-preview-budget-exceeded";
constexpr const char* kSourcePreviewBudgetExceededDiagnostic =
    "thumbnail-source-preview-budget-exceeded";
constexpr const char* kMaterialPreviewBudgetExceededDiagnostic =
    "thumbnail-material-preview-budget-exceeded";
constexpr const char* kPrefabPreviewBudgetExceededDiagnostic =
    "thumbnail-prefab-preview-budget-exceeded";

constexpr bool AssetThumbnailKindPoliciesAreExhaustive()
{
    if (kAssetThumbnailKindPolicies.size() != kAssetThumbnailKindCount)
        return false;

    std::array<bool, kAssetThumbnailKindCount> seen {};
    for (const auto& policy : kAssetThumbnailKindPolicies)
    {
        const auto index = static_cast<size_t>(policy.kind);
        if (index >= kAssetThumbnailKindCount || seen[index])
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

static_assert(AssetThumbnailKindPoliciesAreExhaustive());

const AssetThumbnailKindPolicy* PolicyForKind(const AssetThumbnailKind kind)
{
    const auto index = static_cast<size_t>(kind);
    if (index >= kAssetThumbnailKindCount)
        return nullptr;

    for (const auto& policy : kAssetThumbnailKindPolicies)
    {
        if (policy.kind == kind)
            return &policy;
    }
    return nullptr;
}

AssetThumbnailKind ThumbnailKindForItem(const AssetBrowserItem& item)
{
    switch (item.type)
    {
    case AssetBrowserItemType::Texture:
        return AssetThumbnailKind::Texture;
    case AssetBrowserItemType::Material:
        return AssetThumbnailKind::MaterialSphere;
    case AssetBrowserItemType::Model:
        return AssetThumbnailKind::ModelPreview;
    case AssetBrowserItemType::Prefab:
        return AssetThumbnailKind::PrefabPreview;
    default:
        return AssetThumbnailKind::Icon;
    }
}

std::string FallbackIconForKind(const AssetThumbnailKind kind)
{
    const auto* policy = PolicyForKind(kind);
    return policy != nullptr ? policy->fallbackIcon : "Icon_Unknown";
}

bool CanGenerateThumbnail(const AssetThumbnailKind kind)
{
    const auto* policy = PolicyForKind(kind);
    return policy != nullptr && policy->generator != nullptr;
}

AssetThumbnailGenerator GeneratorForKind(const AssetThumbnailKind kind)
{
    const auto* policy = PolicyForKind(kind);
    return policy != nullptr ? policy->generator : nullptr;
}

std::string UnsupportedDiagnosticForKind(const AssetThumbnailKind kind)
{
    const auto* policy = PolicyForKind(kind);
    return policy != nullptr
        ? policy->unsupportedDiagnostic
        : "thumbnail-generation-unsupported";
}

bool IsThumbnailGenerationCancelled(const AssetThumbnailCancelToken& cancelToken)
{
    const auto token = cancelToken.lock();
    return token == nullptr || token->cancelled.load(std::memory_order_relaxed);
}

bool IsRetryableThumbnailFailureDiagnostic(const std::string& diagnostic)
{
    return diagnostic == "thumbnail-material-preview-hook-unavailable" ||
        diagnostic == "thumbnail-model-preview-hook-unavailable" ||
        diagnostic == "thumbnail-prefab-preview-hook-unavailable" ||
        diagnostic == "thumbnail-material-preview-generation-failed" ||
        diagnostic == "thumbnail-model-preview-generation-failed" ||
        diagnostic == "thumbnail-prefab-preview-generation-failed" ||
        diagnostic == "thumbnail-generation-worker-start-failed";
}

bool IsMeshPreviewWithinBudget(const NLS::Render::Assets::MeshArtifactData& mesh)
{
    const auto triangleCount = mesh.indices.size() / 3u;
    return mesh.vertices.size() <= kMaxMeshPreviewVertices &&
        triangleCount <= kMaxMeshPreviewTriangles;
}

bool IsTextureThumbnailSourceExtension(const std::filesystem::path& path)
{
    auto extension = path.extension().generic_string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });

    return extension == ".png" ||
        extension == ".jpg" ||
        extension == ".jpeg" ||
        extension == ".tga" ||
        extension == ".bmp";
}

uint16_t ReadBigEndianUInt16(const uint8_t* data)
{
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(data[0]) << 8u) |
        static_cast<uint16_t>(data[1]));
}

uint32_t ReadBigEndianUInt32(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24u) |
        (static_cast<uint32_t>(data[1]) << 16u) |
        (static_cast<uint32_t>(data[2]) << 8u) |
        static_cast<uint32_t>(data[3]);
}

uint16_t ReadLittleEndianUInt16(const uint8_t* data)
{
    return static_cast<uint16_t>(
        static_cast<uint16_t>(data[0]) |
        (static_cast<uint16_t>(data[1]) << 8u));
}

uint32_t ReadLittleEndianUInt32(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8u) |
        (static_cast<uint32_t>(data[2]) << 16u) |
        (static_cast<uint32_t>(data[3]) << 24u);
}

bool ReadFilePrefix(
    const std::filesystem::path& path,
    std::vector<uint8_t>& bytes,
    const size_t maxBytes)
{
    std::ifstream input(path, std::ios::binary);
    if (!input || maxBytes == 0u)
        return false;

    bytes.resize(maxBytes);
    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    const auto readCount = input.gcount();
    if (readCount <= 0)
    {
        bytes.clear();
        return false;
    }

    bytes.resize(static_cast<size_t>(readCount));
    return true;
}

struct ImageHeaderDimensions
{
    uint32_t width = 0u;
    uint32_t height = 0u;
};

std::optional<ImageHeaderDimensions> ReadPngHeaderDimensions(const std::filesystem::path& path)
{
    constexpr std::array<uint8_t, 8u> kPngSignature {
        0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au
    };
    std::vector<uint8_t> header;
    if (!ReadFilePrefix(path, header, 33u) || header.size() < 33u)
        return std::nullopt;
    if (!std::equal(kPngSignature.begin(), kPngSignature.end(), header.begin()))
        return std::nullopt;
    if (ReadBigEndianUInt32(header.data() + 8u) != 13u ||
        header[12u] != 'I' ||
        header[13u] != 'H' ||
        header[14u] != 'D' ||
        header[15u] != 'R')
    {
        return std::nullopt;
    }

    ImageHeaderDimensions dimensions;
    dimensions.width = ReadBigEndianUInt32(header.data() + 16u);
    dimensions.height = ReadBigEndianUInt32(header.data() + 20u);
    if (dimensions.width == 0u || dimensions.height == 0u)
        return std::nullopt;
    return dimensions;
}

std::optional<ImageHeaderDimensions> ReadBmpHeaderDimensions(const std::filesystem::path& path)
{
    std::vector<uint8_t> header;
    if (!ReadFilePrefix(path, header, 26u) || header.size() < 26u)
        return std::nullopt;
    if (header[0u] != 'B' || header[1u] != 'M')
        return std::nullopt;

    const auto dibHeaderSize = ReadLittleEndianUInt32(header.data() + 14u);
    ImageHeaderDimensions dimensions;
    if (dibHeaderSize == 12u)
    {
        dimensions.width = ReadLittleEndianUInt16(header.data() + 18u);
        dimensions.height = ReadLittleEndianUInt16(header.data() + 20u);
    }
    else if (dibHeaderSize >= 40u)
    {
        dimensions.width = ReadLittleEndianUInt32(header.data() + 18u);
        const auto signedHeight = static_cast<int64_t>(
            static_cast<int32_t>(ReadLittleEndianUInt32(header.data() + 22u)));
        dimensions.height = static_cast<uint32_t>(signedHeight < 0 ? -signedHeight : signedHeight);
    }
    if (dimensions.width == 0u || dimensions.height == 0u)
        return std::nullopt;
    return dimensions;
}

std::optional<ImageHeaderDimensions> ReadTgaHeaderDimensions(const std::filesystem::path& path)
{
    std::vector<uint8_t> header;
    if (!ReadFilePrefix(path, header, 18u) || header.size() < 18u)
        return std::nullopt;

    ImageHeaderDimensions dimensions;
    dimensions.width = ReadLittleEndianUInt16(header.data() + 12u);
    dimensions.height = ReadLittleEndianUInt16(header.data() + 14u);
    if (dimensions.width == 0u || dimensions.height == 0u)
        return std::nullopt;
    return dimensions;
}

bool IsJpegStartOfFrameMarker(const uint8_t marker)
{
    switch (marker)
    {
    case 0xC0u:
    case 0xC1u:
    case 0xC2u:
    case 0xC3u:
    case 0xC5u:
    case 0xC6u:
    case 0xC7u:
    case 0xC9u:
    case 0xCAu:
    case 0xCBu:
    case 0xCDu:
    case 0xCEu:
    case 0xCFu:
        return true;
    default:
        return false;
    }
}

std::optional<ImageHeaderDimensions> ReadJpegHeaderDimensions(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;

    const std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    if (bytes.size() < 4u)
        return std::nullopt;
    if (bytes[0u] != 0xFFu || bytes[1u] != 0xD8u)
        return std::nullopt;

    size_t offset = 2u;
    while (offset + 3u < bytes.size())
    {
        while (offset < bytes.size() && bytes[offset] != 0xFFu)
            ++offset;
        while (offset < bytes.size() && bytes[offset] == 0xFFu)
            ++offset;
        if (offset >= bytes.size())
            break;

        const auto marker = bytes[offset++];
        if (marker == 0xD9u || marker == 0xDAu)
            break;
        if ((marker >= 0xD0u && marker <= 0xD7u) || marker == 0x01u)
            continue;
        if (offset + 2u > bytes.size())
            break;

        const auto segmentLength = ReadBigEndianUInt16(bytes.data() + offset);
        if (segmentLength < 2u || offset + segmentLength > bytes.size())
            break;

        if (IsJpegStartOfFrameMarker(marker) && segmentLength >= 7u)
        {
            ImageHeaderDimensions dimensions;
            dimensions.height = ReadBigEndianUInt16(bytes.data() + offset + 3u);
            dimensions.width = ReadBigEndianUInt16(bytes.data() + offset + 5u);
            if (dimensions.width == 0u || dimensions.height == 0u)
                return std::nullopt;
            return dimensions;
        }
        offset += segmentLength;
    }
    return std::nullopt;
}

bool IsKnownSourceImageExtension(const std::filesystem::path& path)
{
    auto extension = path.extension().generic_string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return extension == ".png" ||
        extension == ".bmp" ||
        extension == ".tga" ||
        extension == ".jpg" ||
        extension == ".jpeg";
}

std::optional<ImageHeaderDimensions> ReadImageHeaderDimensions(const std::filesystem::path& path)
{
    auto extension = path.extension().generic_string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });

    if (extension == ".png")
        return ReadPngHeaderDimensions(path);
    if (extension == ".bmp")
        return ReadBmpHeaderDimensions(path);
    if (extension == ".tga")
        return ReadTgaHeaderDimensions(path);
    if (extension == ".jpg" || extension == ".jpeg")
        return ReadJpegHeaderDimensions(path);
    return std::nullopt;
}

bool ImageDimensionsExceedPreviewBudget(const ImageHeaderDimensions& dimensions)
{
    const auto pixels =
        static_cast<uint64_t>(dimensions.width) *
        static_cast<uint64_t>(dimensions.height);
    return pixels > kMaxSourceThumbnailPixels;
}

bool MeshHeaderPreviewExceedsBudget(const NLS::Render::Assets::MeshArtifactHeaderPreview& header)
{
    return header.vertexCount > kMaxMeshPreviewVertices ||
        header.indexCount / 3u > kMaxMeshPreviewTriangles;
}

std::vector<uint8_t> ConvertToRgba8(const NLS::Image& image)
{
    const auto* source = image.GetData();
    if (source == nullptr)
        return {};

    const auto width = image.GetWidth();
    const auto height = image.GetHeight();
    const auto channels = image.GetChannels();
    if (width <= 0 || height <= 0 || channels <= 0 || channels > 4)
        return {};

    const auto pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<uint8_t> rgba(pixelCount * 4u, 255u);
    for (size_t pixel = 0u; pixel < pixelCount; ++pixel)
    {
        const auto sourceIndex = pixel * static_cast<size_t>(channels);
        const auto targetIndex = pixel * 4u;
        switch (channels)
        {
        case 1:
            rgba[targetIndex + 0u] = source[sourceIndex + 0u];
            rgba[targetIndex + 1u] = source[sourceIndex + 0u];
            rgba[targetIndex + 2u] = source[sourceIndex + 0u];
            break;
        case 2:
            rgba[targetIndex + 0u] = source[sourceIndex + 0u];
            rgba[targetIndex + 1u] = source[sourceIndex + 0u];
            rgba[targetIndex + 2u] = source[sourceIndex + 0u];
            rgba[targetIndex + 3u] = source[sourceIndex + 1u];
            break;
        case 3:
            rgba[targetIndex + 0u] = source[sourceIndex + 0u];
            rgba[targetIndex + 1u] = source[sourceIndex + 1u];
            rgba[targetIndex + 2u] = source[sourceIndex + 2u];
            break;
        case 4:
            rgba[targetIndex + 0u] = source[sourceIndex + 0u];
            rgba[targetIndex + 1u] = source[sourceIndex + 1u];
            rgba[targetIndex + 2u] = source[sourceIndex + 2u];
            rgba[targetIndex + 3u] = source[sourceIndex + 3u];
            break;
        default:
            return {};
        }
    }
    return rgba;
}

struct DownsampledThumbnail
{
    std::vector<uint8_t> pixels;
    uint32_t width = 0u;
    uint32_t height = 0u;
};

DownsampledThumbnail DownsampleRgba8ToThumbnail(
    const uint8_t* sourcePixels,
    const uint32_t sourceWidth,
    const uint32_t sourceHeight,
    const uint32_t sourceRowPitch,
    const uint32_t requestedSize)
{
    DownsampledThumbnail thumbnail;
    const auto clampedSize = std::max(1u, requestedSize);
    if (sourcePixels == nullptr || sourceWidth == 0u || sourceHeight == 0u || sourceRowPitch < sourceWidth * 4u)
        return thumbnail;

    const auto largestDimension = (std::max)(sourceWidth, sourceHeight);
    const auto targetLargestDimension = (std::min)(largestDimension, clampedSize);
    thumbnail.width = (std::max)(1u, static_cast<uint32_t>(
        (static_cast<uint64_t>(sourceWidth) * targetLargestDimension + largestDimension - 1u) /
        largestDimension));
    thumbnail.height = (std::max)(1u, static_cast<uint32_t>(
        (static_cast<uint64_t>(sourceHeight) * targetLargestDimension + largestDimension - 1u) /
        largestDimension));

    thumbnail.pixels.resize(static_cast<size_t>(thumbnail.width) * thumbnail.height * 4u);
    for (uint32_t y = 0u; y < thumbnail.height; ++y)
    {
        const auto sourceY = (std::min)(
            static_cast<uint32_t>((static_cast<uint64_t>(y) * sourceHeight) / thumbnail.height),
            sourceHeight - 1u);
        for (uint32_t x = 0u; x < thumbnail.width; ++x)
        {
            const auto sourceX = (std::min)(
                static_cast<uint32_t>((static_cast<uint64_t>(x) * sourceWidth) / thumbnail.width),
                sourceWidth - 1u);
            const auto* source = sourcePixels + static_cast<size_t>(sourceY) * sourceRowPitch + sourceX * 4u;
            auto* target = thumbnail.pixels.data() +
                (static_cast<size_t>(y) * thumbnail.width + x) * 4u;
            std::copy_n(source, 4u, target);
        }
    }
    return thumbnail;
}

DownsampledThumbnail DownsampleImageToThumbnail(
    const NLS::Image& image,
    const uint32_t requestedSize)
{
    DownsampledThumbnail thumbnail;
    const auto sourceWidth = image.GetWidth();
    const auto sourceHeight = image.GetHeight();
    if (sourceWidth <= 0 || sourceHeight <= 0)
        return thumbnail;

    const auto sourcePixels = ConvertToRgba8(image);
    if (sourcePixels.empty())
        return {};

    return DownsampleRgba8ToThumbnail(
        sourcePixels.data(),
        static_cast<uint32_t>(sourceWidth),
        static_cast<uint32_t>(sourceHeight),
        static_cast<uint32_t>(sourceWidth) * 4u,
        requestedSize);
}

std::filesystem::path ResolveThumbnailSourcePath(const AssetThumbnailRequest& request)
{
    return ResolveEditorAssetPath(
        MakeProjectEditorAssetRoots(request.projectRoot),
        request.sourceAssetPath);
}

std::filesystem::path ResolveThumbnailArtifactPath(const AssetThumbnailRequest& request)
{
    if (request.artifactPath.empty() || !request.assetId.IsValid())
        return {};

    const auto rawPath = std::filesystem::path(request.artifactPath).lexically_normal();
    const auto sourceArtifactRoot = NLS::Core::Assets::NormalizeAssetPath(
        request.projectRoot / "Library" / "Artifacts" / request.assetId.ToString());
    if (sourceArtifactRoot.empty())
        return {};

    auto resolveCandidate = [&sourceArtifactRoot](const std::filesystem::path& candidate)
        -> std::filesystem::path
    {
        const auto normalized = NLS::Core::Assets::NormalizeAssetPath(candidate);
        if (!normalized.empty() &&
            IsPhysicalRegularFileInsideEditorAssetRoot(normalized, sourceArtifactRoot))
        {
            return normalized;
        }
        return {};
    };

    if (rawPath.is_absolute())
        return resolveCandidate(rawPath);

    const auto candidate = resolveCandidate(request.projectRoot / rawPath);
    if (!candidate.empty())
        return candidate;

    const auto artifactRootCandidate = resolveCandidate(sourceArtifactRoot / rawPath);
    if (!artifactRootCandidate.empty())
        return artifactRootCandidate;

    return {};
}

std::vector<uint8_t> ReadAllBytes(const std::filesystem::path& path);

std::string FileStamp(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return "missing";

    error.clear();
    const auto writeTime = std::filesystem::last_write_time(path, error);
    if (error)
        return "missing";

    const auto writeTimeTicks = static_cast<std::intmax_t>(writeTime.time_since_epoch().count());
    return std::to_string(size) + ":" + std::to_string(writeTimeTicks);
}

void AddSourceFreshnessInputs(AssetThumbnailRequest& request)
{
    const auto sourcePath = ResolveThumbnailSourcePath(request);
    if (sourcePath.empty())
    {
        request.freshnessInputs.push_back({"source-file", "missing"});
        request.freshnessInputs.push_back({"source-meta", "missing"});
        return;
    }

    request.freshnessInputs.push_back({"source-file", FileStamp(sourcePath)});
    request.freshnessInputs.push_back({
        "source-meta",
        FileStamp(NLS::Core::Assets::GetAssetMetaPath(sourcePath))
    });
}

void AddArtifactFreshnessInputs(
    AssetThumbnailRequest& request,
    const AssetBrowserItem& item)
{
    const bool usesArtifactManifest =
        item.kind == AssetBrowserItemKind::GeneratedSubAsset ||
        item.type == AssetBrowserItemType::Model ||
        item.type == AssetBrowserItemType::Prefab;
    if (usesArtifactManifest)
    {
        const auto manifestPath =
            request.projectRoot /
            "Library" /
            "Artifacts" /
            request.assetId.ToString() /
            "manifest.json";
        request.freshnessInputs.push_back({"artifact-manifest", FileStamp(manifestPath)});
    }

    if (item.artifactPath.empty())
        return;

    request.freshnessInputs.push_back({"artifact-file", FileStamp(ResolveThumbnailArtifactPath(request))});
}

bool IsFileFreshnessInputStillCurrent(
    const AssetThumbnailRequest& request,
    const AssetThumbnailFreshnessInput& input)
{
    if (input.name == "source-file")
        return input.stamp == FileStamp(ResolveThumbnailSourcePath(request));
    if (input.name == "source-meta")
    {
        const auto sourcePath = ResolveThumbnailSourcePath(request);
        if (sourcePath.empty())
            return input.stamp == "missing";
        return input.stamp == FileStamp(NLS::Core::Assets::GetAssetMetaPath(sourcePath));
    }
    if (input.name == "artifact-file")
        return input.stamp == FileStamp(ResolveThumbnailArtifactPath(request));
    if (input.name == "artifact-manifest")
    {
        const auto manifestPath =
            request.projectRoot /
            "Library" /
            "Artifacts" /
            request.assetId.ToString() /
            "manifest.json";
        return input.stamp == FileStamp(manifestPath);
    }
    return true;
}

bool IsThumbnailRequestStillFresh(const AssetThumbnailRequest& request)
{
    for (const auto& input : request.freshnessInputs)
    {
        if (!IsFileFreshnessInputStillCurrent(request, input))
            return false;
    }
    return true;
}

AssetThumbnailServiceResult BuildStaleThumbnailRequestResult(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation)
{
    AssetThumbnailServiceResult result;
    result.status = AssetThumbnailServiceStatus::Failed;
    result.cacheEntry = evaluation.entry;
    result.fallbackIcon = FallbackIconForKind(request.kind);
    if (evaluation.entry.has_value())
        result.imagePath = evaluation.entry->imagePath;
    result.diagnostic = "thumbnail-request-stale";
    return result;
}

AssetThumbnailServiceResult BuildCancelledThumbnailRequestResult(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation)
{
    AssetThumbnailServiceResult result;
    result.status = AssetThumbnailServiceStatus::Failed;
    result.cacheEntry = evaluation.entry;
    result.fallbackIcon = FallbackIconForKind(request.kind);
    if (evaluation.entry.has_value())
        result.imagePath = evaluation.entry->imagePath;
    result.diagnostic = "thumbnail-generation-cancelled";
    return result;
}

AssetThumbnailServiceResult BuildResultFromEvaluation(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation,
    const AssetThumbnailServiceStatus status)
{
    AssetThumbnailServiceResult result;
    result.status = status;
    result.cacheEntry = evaluation.entry;
    result.diagnostic = evaluation.diagnostic;
    result.fallbackIcon = FallbackIconForKind(request.kind);
    if (evaluation.entry.has_value())
        result.imagePath = evaluation.entry->imagePath;
    return result;
}

std::vector<uint8_t> EncodeThumbnailPng(const DownsampledThumbnail& thumbnail)
{
    if (thumbnail.pixels.empty() || thumbnail.width == 0u || thumbnail.height == 0u)
        return {};

    int encodedLength = 0;
    unsigned char* encoded = nls_asset_thumbnail_service_stbi_write_png_to_mem(
        const_cast<uint8_t*>(thumbnail.pixels.data()),
        static_cast<int>(thumbnail.width * 4u),
        static_cast<int>(thumbnail.width),
        static_cast<int>(thumbnail.height),
        4,
        &encodedLength);
    if (encoded == nullptr || encodedLength <= 0)
        return {};

    std::vector<uint8_t> bytes(
        encoded,
        encoded + static_cast<size_t>(encodedLength));
    std::free(encoded);
    return bytes;
}

AssetThumbnailServiceResult WriteThumbnailPngResult(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation,
    const DownsampledThumbnail& thumbnail,
    const std::string& emptyDiagnostic,
    const AssetThumbnailCancelToken& cancelToken)
{
    auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
    if (!evaluation.entry.has_value())
    {
        result.diagnostic = evaluation.diagnostic.empty()
            ? "thumbnail-cache-path-invalid"
            : evaluation.diagnostic;
        return result;
    }

    if (!IsThumbnailRequestStillFresh(request))
        return BuildStaleThumbnailRequestResult(request, evaluation);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);

    if (thumbnail.pixels.empty() || thumbnail.width == 0u || thumbnail.height == 0u)
    {
        result.diagnostic = emptyDiagnostic;
        WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

    const auto encoded = EncodeThumbnailPng(thumbnail);
    if (encoded.empty())
    {
        result.diagnostic = "thumbnail-cache-image-encode-failed";
        WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

    if (!WriteAssetThumbnailCacheFile(request, evaluation.entry->imagePath, encoded))
    {
        result.diagnostic = "thumbnail-cache-image-write-failed";
        WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

    if (!IsThumbnailRequestStillFresh(request))
        return BuildStaleThumbnailRequestResult(request, evaluation);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);

    if (!WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Fresh, {}))
    {
        result.diagnostic = "thumbnail-cache-metadata-write-failed";
        return result;
    }

    result.status = AssetThumbnailServiceStatus::Fresh;
    result.imagePath = evaluation.entry->imagePath;
    result.diagnostic.clear();
    return result;
}

std::vector<uint8_t> ReadAllBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
}

uint64_t FileSizeOrMax(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return error ? std::numeric_limits<uint64_t>::max() : static_cast<uint64_t>(size);
}

bool HasNativeArtifactMagic(const std::filesystem::path& path)
{
    std::array<uint8_t, 4u> bytes {};
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;

    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size()))
        return false;

    return bytes[0] == 'N' &&
        bytes[1] == 'L' &&
        bytes[2] == 'S' &&
        bytes[3] == 'A';
}

std::optional<NLS::Core::Assets::NativeArtifactPayloadPrefix> ReadStrictStructurePreviewPrefix(
    const std::filesystem::path& path,
    const NLS::Core::Assets::ArtifactType artifactType,
    const uint32_t schemaVersion)
{
    const auto prefix = NLS::Core::Assets::ReadNativeArtifactPayloadPrefixFromFile(
        path,
        artifactType,
        schemaVersion,
        1u,
        kMaxStructurePreviewArtifactPayloadBytes);
    if (!prefix.has_value())
        return std::nullopt;

    const auto fileSize = FileSizeOrMax(path);
    if (prefix->payloadOffset > fileSize ||
        prefix->payloadSize > fileSize - prefix->payloadOffset ||
        prefix->payloadOffset + prefix->payloadSize != fileSize)
    {
        return std::nullopt;
    }
    return prefix;
}

std::optional<std::string> ReadNativeOrPlainTextArtifact(
    const std::filesystem::path& path,
    const NLS::Core::Assets::ArtifactType artifactType,
    const uint32_t schemaVersion)
{
    if (FileSizeOrMax(path) > kMaxStructurePreviewArtifactPayloadBytes)
        return std::nullopt;

    const auto bytes = ReadAllBytes(path);
    if (bytes.empty())
        return std::nullopt;

    if (NLS::Core::Assets::IsNativeArtifactContainer(bytes))
    {
        const auto container = NLS::Core::Assets::ReadNativeArtifactContainer(
            bytes,
            artifactType,
            schemaVersion);
        if (!container.has_value())
            return std::nullopt;

        return std::string(
            container->payload.begin(),
            container->payload.end());
    }
    return std::string(bytes.begin(), bytes.end());
}

bool StructurePreviewArtifactExceedsBudget(
    const std::filesystem::path& path,
    const NLS::Core::Assets::ArtifactType artifactType,
    const uint32_t schemaVersion)
{
    const auto prefix = ReadStrictStructurePreviewPrefix(path, artifactType, schemaVersion);
    if (prefix.has_value())
        return prefix->payloadSize > kMaxStructurePreviewArtifactPayloadBytes;

    return HasNativeArtifactMagic(path) ||
        FileSizeOrMax(path) > kMaxStructurePreviewArtifactPayloadBytes;
}

bool NativeArtifactFileExceedsThumbnailPreviewBudget(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return !error && size > kMaxThumbnailPreviewNativeArtifactFileBytes;
}

std::optional<std::filesystem::path> ResolveArtifactPathForPreview(
    const AssetThumbnailRequest& request,
    const std::string& artifactPath)
{
    if (artifactPath.empty())
        return std::nullopt;

    auto copy = request;
    copy.artifactPath = artifactPath;
    const auto resolved = ResolveThumbnailArtifactPath(copy);
    if (resolved.empty())
        return std::nullopt;
    return resolved;
}

std::optional<NLS::Core::Assets::ArtifactManifest> LoadThumbnailArtifactManifest(
    const AssetThumbnailRequest& request)
{
    if (!request.assetId.IsValid())
        return std::nullopt;

    const auto manifestPath =
        request.projectRoot /
        "Library" /
        "Artifacts" /
        request.assetId.ToString() /
        "manifest.json";

    std::ifstream input(manifestPath, std::ios::binary);
    if (!input)
        return std::nullopt;

    auto root = nlohmann::json::parse(input, nullptr, false);
    if (root.is_discarded())
        return std::nullopt;
    return ParseArtifactManifestJson(root, true);
}

std::optional<std::filesystem::path> ResolveFirstMeshArtifactPath(
    const AssetThumbnailRequest& request)
{
    if (const auto path = ResolveArtifactPathForPreview(request, request.artifactPath);
        path.has_value() && path->extension() == ".nmesh")
    {
        return path;
    }

    const auto manifest = LoadThumbnailArtifactManifest(request);
    if (!manifest.has_value())
        return std::nullopt;

    for (const auto& artifact : manifest->subAssets)
    {
        if (artifact.artifactType != NLS::Core::Assets::ArtifactType::Mesh)
            continue;

        if (auto resolved = ResolveArtifactPathForPreview(request, artifact.artifactPath);
            resolved.has_value())
        {
            return resolved;
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> ResolvePreviewArtifactOrSourcePath(
    const AssetThumbnailRequest& request)
{
    if (auto artifactPath = ResolveArtifactPathForPreview(request, request.artifactPath);
        artifactPath.has_value())
    {
        return artifactPath;
    }
    if (!request.artifactPath.empty())
        return std::nullopt;

    const auto sourcePath = ResolveThumbnailSourcePath(request);
    if (sourcePath.empty())
        return std::nullopt;
    return sourcePath;
}

struct RgbaCanvas
{
    std::vector<uint8_t> pixels;
    uint32_t width = 0u;
    uint32_t height = 0u;
};

RgbaCanvas MakeCanvas(const uint32_t requestedSize)
{
    const auto size = std::max(1u, requestedSize);
    RgbaCanvas canvas;
    canvas.width = size;
    canvas.height = size;
    canvas.pixels.assign(static_cast<size_t>(size) * size * 4u, 0u);
    return canvas;
}

void PutPixel(
    RgbaCanvas& canvas,
    const int x,
    const int y,
    const uint8_t r,
    const uint8_t g,
    const uint8_t b,
    const uint8_t a = 255u)
{
    if (x < 0 || y < 0 ||
        x >= static_cast<int>(canvas.width) ||
        y >= static_cast<int>(canvas.height))
    {
        return;
    }

    const auto index = (static_cast<size_t>(y) * canvas.width + static_cast<size_t>(x)) * 4u;
    canvas.pixels[index + 0u] = r;
    canvas.pixels[index + 1u] = g;
    canvas.pixels[index + 2u] = b;
    canvas.pixels[index + 3u] = a;
}

void DrawLine(
    RgbaCanvas& canvas,
    int x0,
    int y0,
    const int x1,
    const int y1,
    const uint8_t r,
    const uint8_t g,
    const uint8_t b)
{
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;

    for (;;)
    {
        PutPixel(canvas, x0, y0, r, g, b);
        PutPixel(canvas, x0 + 1, y0, r, g, b, 210u);
        PutPixel(canvas, x0, y0 + 1, r, g, b, 210u);
        if (x0 == x1 && y0 == y1)
            break;

        const int doubledError = 2 * error;
        if (doubledError >= dy)
        {
            error += dy;
            x0 += sx;
        }
        if (doubledError <= dx)
        {
            error += dx;
            y0 += sy;
        }
    }
}

void FillTriangle(
    RgbaCanvas& canvas,
    const std::array<int, 2u>& p0,
    const std::array<int, 2u>& p1,
    const std::array<int, 2u>& p2,
    const uint8_t r,
    const uint8_t g,
    const uint8_t b,
    const uint8_t a)
{
    const auto edge = [](const std::array<int, 2u>& a, const std::array<int, 2u>& b, const int x, const int y)
    {
        return (x - a[0]) * (b[1] - a[1]) - (y - a[1]) * (b[0] - a[0]);
    };

    const int minX = std::max(0, std::min({p0[0], p1[0], p2[0]}));
    const int maxX = std::min(static_cast<int>(canvas.width) - 1, std::max({p0[0], p1[0], p2[0]}));
    const int minY = std::max(0, std::min({p0[1], p1[1], p2[1]}));
    const int maxY = std::min(static_cast<int>(canvas.height) - 1, std::max({p0[1], p1[1], p2[1]}));
    if (minX > maxX || minY > maxY)
        return;

    const auto area = edge(p0, p1, p2[0], p2[1]);
    if (area == 0)
        return;

    for (int y = minY; y <= maxY; ++y)
    {
        for (int x = minX; x <= maxX; ++x)
        {
            const auto w0 = edge(p1, p2, x, y);
            const auto w1 = edge(p2, p0, x, y);
            const auto w2 = edge(p0, p1, x, y);
            const bool insidePositive = w0 >= 0 && w1 >= 0 && w2 >= 0;
            const bool insideNegative = w0 <= 0 && w1 <= 0 && w2 <= 0;
            if (insidePositive || insideNegative)
                PutPixel(canvas, x, y, r, g, b, a);
        }
    }
}

DownsampledThumbnail CanvasToThumbnail(RgbaCanvas canvas)
{
    DownsampledThumbnail thumbnail;
    thumbnail.width = canvas.width;
    thumbnail.height = canvas.height;
    thumbnail.pixels = std::move(canvas.pixels);
    return thumbnail;
}

std::optional<std::vector<float>> ParseFloatList(const std::string& value)
{
    std::istringstream stream(value);
    std::vector<float> values;
    float number = 0.0f;
    while (stream >> number)
        values.push_back(number);
    if (values.empty())
        return std::nullopt;
    return values;
}

std::optional<std::string> ExtractXmlAttribute(
    const std::string& element,
    const std::string& attribute)
{
    const auto key = attribute + "=\"";
    const auto begin = element.find(key);
    if (begin == std::string::npos)
        return std::nullopt;

    const auto valueBegin = begin + key.size();
    const auto valueEnd = element.find('"', valueBegin);
    if (valueEnd == std::string::npos)
        return std::nullopt;
    return element.substr(valueBegin, valueEnd - valueBegin);
}

std::array<float, 4u> ExtractMaterialBaseColor(const std::string& xml)
{
    auto parseNamedValue = [&xml](const std::string& name) -> std::optional<std::array<float, 4u>>
    {
        size_t position = 0u;
        while ((position = xml.find("name=\"" + name + "\"", position)) != std::string::npos)
        {
            const auto elementBegin = xml.rfind('<', position);
            const auto elementEnd = xml.find('>', position);
            if (elementBegin == std::string::npos || elementEnd == std::string::npos)
            {
                position += name.size();
                continue;
            }

            const auto element = xml.substr(elementBegin, elementEnd - elementBegin + 1u);
            const auto value = ExtractXmlAttribute(element, "value");
            const auto values = value.has_value() ? ParseFloatList(*value) : std::nullopt;
            if (values.has_value())
            {
                std::array<float, 4u> color {0.75f, 0.75f, 0.75f, 1.0f};
                for (size_t index = 0u; index < color.size() && index < values->size(); ++index)
                    color[index] = std::clamp((*values)[index], 0.0f, 1.0f);
                return color;
            }
            position = elementEnd + 1u;
        }
        return std::nullopt;
    };

    if (auto uniform = parseNamedValue("u_Albedo");
        uniform.has_value())
    {
        return *uniform;
    }
    if (auto factor = parseNamedValue("BaseColor");
        factor.has_value())
    {
        return *factor;
    }
    return {0.72f, 0.74f, 0.78f, 1.0f};
}

DownsampledThumbnail RenderMaterialSphereThumbnail(
    const std::string& materialPayload,
    const uint32_t requestedSize)
{
    auto canvas = MakeCanvas(requestedSize);
    const auto baseColor = ExtractMaterialBaseColor(materialPayload);
    const auto center = (static_cast<float>(canvas.width) - 1.0f) * 0.5f;
    const auto radius = std::max(1.0f, static_cast<float>(canvas.width) * 0.42f);
    constexpr float lightX = -0.35f;
    constexpr float lightY = -0.55f;
    constexpr float lightZ = 0.76f;

    for (uint32_t y = 0u; y < canvas.height; ++y)
    {
        for (uint32_t x = 0u; x < canvas.width; ++x)
        {
            const float nx = (static_cast<float>(x) - center) / radius;
            const float ny = (static_cast<float>(y) - center) / radius;
            const float rr = nx * nx + ny * ny;
            if (rr > 1.0f)
                continue;

            const float nz = std::sqrt(std::max(0.0f, 1.0f - rr));
            const float diffuse = std::max(0.0f, nx * lightX + ny * lightY + nz * lightZ);
            const float rim = std::pow(std::max(0.0f, 1.0f - nz), 2.0f) * 0.18f;
            const float shade = std::clamp(0.22f + diffuse * 0.78f + rim, 0.0f, 1.0f);
            PutPixel(
                canvas,
                static_cast<int>(x),
                static_cast<int>(y),
                static_cast<uint8_t>(std::clamp(baseColor[0] * shade * 255.0f, 0.0f, 255.0f)),
                static_cast<uint8_t>(std::clamp(baseColor[1] * shade * 255.0f, 0.0f, 255.0f)),
                static_cast<uint8_t>(std::clamp(baseColor[2] * shade * 255.0f, 0.0f, 255.0f)),
                static_cast<uint8_t>(std::clamp(baseColor[3] * 255.0f, 0.0f, 255.0f)));
        }
    }
    return CanvasToThumbnail(std::move(canvas));
}

DownsampledThumbnail RenderMeshThumbnail(
    const NLS::Render::Assets::MeshArtifactData& mesh,
    const uint32_t requestedSize)
{
    auto canvas = MakeCanvas(requestedSize);
    if (mesh.vertices.empty())
        return CanvasToThumbnail(std::move(canvas));

    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = std::numeric_limits<float>::lowest();
    bool useZForVertical = false;
    for (const auto& vertex : mesh.vertices)
    {
        minX = std::min(minX, vertex.position[0]);
        maxX = std::max(maxX, vertex.position[0]);
        minY = std::min(minY, vertex.position[1]);
        maxY = std::max(maxY, vertex.position[1]);
    }

    if (maxY - minY < 0.0001f)
    {
        useZForVertical = true;
        minY = std::numeric_limits<float>::max();
        maxY = std::numeric_limits<float>::lowest();
        for (const auto& vertex : mesh.vertices)
        {
            minY = std::min(minY, vertex.position[2]);
            maxY = std::max(maxY, vertex.position[2]);
        }
    }

    const auto rangeX = std::max(0.0001f, maxX - minX);
    const auto rangeY = std::max(0.0001f, maxY - minY);
    const auto scale = static_cast<float>(canvas.width) * 0.78f / std::max(rangeX, rangeY);
    const auto centerX = (minX + maxX) * 0.5f;
    const auto centerY = (minY + maxY) * 0.5f;
    const auto project = [&](const NLS::Render::Geometry::Vertex& vertex)
    {
        const auto vx = vertex.position[0];
        const auto vy = useZForVertical ? vertex.position[2] : vertex.position[1];
        const auto px = static_cast<int>(std::lround((vx - centerX) * scale + canvas.width * 0.5f));
        const auto py = static_cast<int>(std::lround(canvas.height * 0.5f - (vy - centerY) * scale));
        return std::array<int, 2u>{px, py};
    };

    for (size_t index = 0u; index + 2u < mesh.indices.size(); index += 3u)
    {
        const auto i0 = mesh.indices[index + 0u];
        const auto i1 = mesh.indices[index + 1u];
        const auto i2 = mesh.indices[index + 2u];
        if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size())
            continue;

        const auto p0 = project(mesh.vertices[i0]);
        const auto p1 = project(mesh.vertices[i1]);
        const auto p2 = project(mesh.vertices[i2]);
        const auto shade = static_cast<uint8_t>(90u + (index / 3u % 5u) * 16u);
        FillTriangle(canvas, p0, p1, p2, shade, static_cast<uint8_t>(shade + 24u), 170u, 180u);
        DrawLine(canvas, p0[0], p0[1], p1[0], p1[1], 220u, 226u, 235u);
        DrawLine(canvas, p1[0], p1[1], p2[0], p2[1], 220u, 226u, 235u);
        DrawLine(canvas, p2[0], p2[1], p0[0], p0[1], 120u, 190u, 255u);
    }

    if (mesh.indices.empty())
    {
        for (const auto& vertex : mesh.vertices)
        {
            const auto p = project(vertex);
            PutPixel(canvas, p[0], p[1], 150u, 210u, 255u);
        }
    }

    return CanvasToThumbnail(std::move(canvas));
}

DownsampledThumbnail RenderPrefabStructureThumbnail(
    const std::string& prefabPayload,
    const uint32_t requestedSize)
{
    auto canvas = MakeCanvas(requestedSize);
    const auto document = NLS::Engine::Serialize::ObjectGraphReader::Read(prefabPayload);
    if (!document.has_value() || document->objects.empty())
        return CanvasToThumbnail(std::move(canvas));

    const auto size = static_cast<int>(canvas.width);
    const int rootLeft = std::max(2, size / 4);
    const int rootRight = std::min(size - 3, (size * 3) / 4);
    const int rootTop = std::max(2, size / 5);
    const int rootBottom = std::min(size - 3, rootTop + std::max(4, size / 6));
    for (int y = rootTop; y <= rootBottom; ++y)
    {
        for (int x = rootLeft; x <= rootRight; ++x)
            PutPixel(canvas, x, y, 116u, 172u, 232u);
    }

    const auto childCount = std::min<size_t>(document->objects.size() - 1u, 5u);
    const int childTop = std::min(size - 4, rootBottom + std::max(4, size / 7));
    const int slotWidth = std::max(3, size / 7);
    for (size_t child = 0u; child < childCount; ++child)
    {
        const int x = std::max(2, size / 2 - static_cast<int>(childCount) * slotWidth / 2 + static_cast<int>(child) * slotWidth);
        DrawLine(canvas, size / 2, rootBottom, x + slotWidth / 2, childTop, 160u, 170u, 185u);
        for (int yy = childTop; yy < std::min(size - 2, childTop + slotWidth); ++yy)
        {
            for (int xx = x; xx < std::min(size - 2, x + slotWidth); ++xx)
                PutPixel(canvas, xx, yy, 185u, 154u, 90u);
        }
    }

    return CanvasToThumbnail(std::move(canvas));
}

AssetThumbnailServiceResult GenerateMaterialThumbnail(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCancelToken& cancelToken)
{
    const auto evaluation = EvaluateAssetThumbnailCache(request);
    auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
    if (!IsThumbnailRequestStillFresh(request))
        return BuildStaleThumbnailRequestResult(request, evaluation);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);

    const auto previewPath = ResolvePreviewArtifactOrSourcePath(request);
    if (!previewPath.has_value())
    {
        result.diagnostic = "thumbnail-material-artifact-path-invalid";
        WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

    if (StructurePreviewArtifactExceedsBudget(
            *previewPath,
            NLS::Core::Assets::ArtifactType::Material,
            1u))
    {
        result.status = AssetThumbnailServiceStatus::Fallback;
        result.diagnostic = kMaterialPreviewBudgetExceededDiagnostic;
        WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

    const auto payload = ReadNativeOrPlainTextArtifact(
        *previewPath,
        NLS::Core::Assets::ArtifactType::Material,
        1u);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);
    if (!payload.has_value())
    {
        result.diagnostic = "thumbnail-material-artifact-read-failed";
        WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

    return WriteThumbnailPngResult(
        request,
        evaluation,
        RenderMaterialSphereThumbnail(*payload, request.requestedSize),
        "thumbnail-material-preview-generation-failed",
        cancelToken);
}

AssetThumbnailServiceResult GenerateMeshBackedThumbnail(
    const AssetThumbnailRequest& request,
    const std::string& missingDiagnostic,
    const AssetThumbnailCancelToken& cancelToken)
{
    const auto evaluation = EvaluateAssetThumbnailCache(request);
    auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
    if (!IsThumbnailRequestStillFresh(request))
        return BuildStaleThumbnailRequestResult(request, evaluation);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);

    const auto meshPath = ResolveFirstMeshArtifactPath(request);
    if (!meshPath.has_value())
    {
        result.diagnostic = missingDiagnostic;
        WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

    const auto meshHeader = NLS::Render::Assets::ReadMeshArtifactHeaderPreview(
        *meshPath,
        kMaxStructurePreviewArtifactPayloadBytes);
    if (!meshHeader.has_value() ||
        NativeArtifactFileExceedsThumbnailPreviewBudget(*meshPath))
    {
        result.diagnostic = "thumbnail-model-mesh-artifact-read-failed";
        WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }
    if (MeshHeaderPreviewExceedsBudget(*meshHeader))
    {
        result.status = AssetThumbnailServiceStatus::Fallback;
        result.diagnostic = kMeshPreviewBudgetExceededDiagnostic;
        WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

    const auto mesh = NLS::Render::Assets::LoadMeshArtifact(*meshPath);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);
    if (!mesh.has_value())
    {
        result.diagnostic = "thumbnail-model-mesh-artifact-read-failed";
        WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

    if (!IsMeshPreviewWithinBudget(*mesh))
    {
        result.status = AssetThumbnailServiceStatus::Fallback;
        result.diagnostic = kMeshPreviewBudgetExceededDiagnostic;
        WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

    return WriteThumbnailPngResult(
        request,
        evaluation,
        RenderMeshThumbnail(*mesh, request.requestedSize),
        "thumbnail-model-preview-generation-failed",
        cancelToken);
}

AssetThumbnailServiceResult GenerateModelThumbnail(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCancelToken& cancelToken)
{
    return GenerateMeshBackedThumbnail(request, "thumbnail-model-mesh-artifact-missing", cancelToken);
}

AssetThumbnailServiceResult GeneratePrefabThumbnail(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCancelToken& cancelToken)
{
    if (ResolveFirstMeshArtifactPath(request).has_value())
        return GenerateMeshBackedThumbnail(request, "thumbnail-prefab-mesh-artifact-missing", cancelToken);

    const auto evaluation = EvaluateAssetThumbnailCache(request);
    auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
    if (!IsThumbnailRequestStillFresh(request))
        return BuildStaleThumbnailRequestResult(request, evaluation);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);

    const auto previewPath = ResolvePreviewArtifactOrSourcePath(request);
    if (!previewPath.has_value())
    {
        result.diagnostic = "thumbnail-prefab-artifact-path-invalid";
        WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

    if (StructurePreviewArtifactExceedsBudget(
            *previewPath,
            NLS::Core::Assets::ArtifactType::Prefab,
            1u))
    {
        result.status = AssetThumbnailServiceStatus::Fallback;
        result.diagnostic = kPrefabPreviewBudgetExceededDiagnostic;
        WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

    const auto payload = ReadNativeOrPlainTextArtifact(
        *previewPath,
        NLS::Core::Assets::ArtifactType::Prefab,
        1u);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);
    if (!payload.has_value())
    {
        result.diagnostic = "thumbnail-prefab-artifact-read-failed";
        WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

    return WriteThumbnailPngResult(
        request,
        evaluation,
        RenderPrefabStructureThumbnail(*payload, request.requestedSize),
        "thumbnail-prefab-preview-generation-failed",
        cancelToken);
}

AssetThumbnailServiceResult GenerateTextureThumbnail(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCancelToken& cancelToken)
{
    auto evaluation = EvaluateAssetThumbnailCache(request);
    auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
    if (!IsThumbnailRequestStillFresh(request))
        return BuildStaleThumbnailRequestResult(request, evaluation);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);

    if (!evaluation.entry.has_value())
    {
        result.diagnostic = evaluation.diagnostic.empty()
            ? "thumbnail-cache-path-invalid"
            : evaluation.diagnostic;
        return result;
    }

    std::error_code error;
    if (!request.artifactPath.empty())
    {
        const auto artifactPath = ResolveThumbnailArtifactPath(request);
        if (artifactPath.empty())
        {
            result.diagnostic = "thumbnail-texture-artifact-path-invalid";
            WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
            return result;
        }

        const auto textureHeader = NLS::Render::Assets::ReadTextureArtifactHeaderPreview(
            artifactPath,
            kMaxStructurePreviewArtifactPayloadBytes);
        if (!textureHeader.has_value() ||
            NativeArtifactFileExceedsThumbnailPreviewBudget(artifactPath))
        {
            result.diagnostic = "thumbnail-texture-artifact-unsupported";
            result.status = AssetThumbnailServiceStatus::Fallback;
            WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
            return result;
        }
        if (ImageDimensionsExceedPreviewBudget({textureHeader->width, textureHeader->height}))
        {
            result.status = AssetThumbnailServiceStatus::Fallback;
            result.diagnostic = kSourcePreviewBudgetExceededDiagnostic;
            WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
            return result;
        }

        const auto textureArtifact = NLS::Render::Assets::LoadTextureArtifact(artifactPath);
        if (IsThumbnailGenerationCancelled(cancelToken))
            return BuildCancelledThumbnailRequestResult(request, evaluation);
        if (!textureArtifact.has_value() ||
            textureArtifact->format != NLS::Render::RHI::TextureFormat::RGBA8 ||
            textureArtifact->mips.empty() ||
            textureArtifact->mips.front().pixels.empty() ||
            textureArtifact->mips.front().width == 0u ||
            textureArtifact->mips.front().height == 0u)
        {
            result.diagnostic = "thumbnail-texture-artifact-unsupported";
            result.status = AssetThumbnailServiceStatus::Fallback;
            WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
            return result;
        }

        std::filesystem::create_directories(evaluation.entry->imagePath.parent_path(), error);
        if (error)
        {
            result.diagnostic = "thumbnail-cache-directory-create-failed";
            WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
            return result;
        }

        const auto& mip = textureArtifact->mips.front();
        return WriteThumbnailPngResult(
            request,
            evaluation,
            DownsampleRgba8ToThumbnail(
                mip.pixels.data(),
                mip.width,
                mip.height,
                mip.rowPitch,
                request.requestedSize),
            "thumbnail-texture-artifact-downsample-failed",
            cancelToken);
    }

    const auto sourcePath = ResolveThumbnailSourcePath(request);
    if (sourcePath.empty())
    {
        result.diagnostic = "thumbnail-source-path-invalid";
        WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

    if (!IsTextureThumbnailSourceExtension(sourcePath))
    {
        result.diagnostic = "thumbnail-texture-extension-unsupported";
        result.status = AssetThumbnailServiceStatus::Fallback;
        WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

    if (!std::filesystem::is_regular_file(sourcePath, error) || error)
    {
        result.diagnostic = "thumbnail-source-missing";
        WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

    error.clear();
    const auto sourceSize = std::filesystem::file_size(sourcePath, error);
    if (!error && sourceSize > kMaxSourceThumbnailImageBytes)
    {
        result.status = AssetThumbnailServiceStatus::Fallback;
        result.diagnostic = kSourcePreviewBudgetExceededDiagnostic;
        WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

    const auto dimensions = ReadImageHeaderDimensions(sourcePath);
    if ((dimensions.has_value() && ImageDimensionsExceedPreviewBudget(*dimensions)) ||
        (!dimensions.has_value() && IsKnownSourceImageExtension(sourcePath)))
    {
        result.status = AssetThumbnailServiceStatus::Fallback;
        result.diagnostic = kSourcePreviewBudgetExceededDiagnostic;
        WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

    NLS::Image sourceImage(sourcePath.string(), false);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);
    if (sourceImage.GetData() == nullptr ||
        sourceImage.GetWidth() <= 0 ||
        sourceImage.GetHeight() <= 0 ||
        sourceImage.GetChannels() <= 0)
    {
        result.diagnostic = "thumbnail-source-decode-failed";
        WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

    const auto thumbnail = DownsampleImageToThumbnail(sourceImage, request.requestedSize);
    if (thumbnail.pixels.empty() || thumbnail.width == 0u || thumbnail.height == 0u)
    {
        result.diagnostic = "thumbnail-source-downsample-failed";
        WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

    return WriteThumbnailPngResult(
        request,
        evaluation,
        thumbnail,
        "thumbnail-source-downsample-failed",
        cancelToken);
}

AssetThumbnailServiceResult GenerateUnsupportedPreviewThumbnail(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCancelToken&)
{
    auto evaluation = EvaluateAssetThumbnailCache(request);
    auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Fallback);

    result.diagnostic = UnsupportedDiagnosticForKind(request.kind);
    return result;
}

AssetThumbnailServiceResult GenerateThumbnailForRequest(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCancelToken& cancelToken)
{
    const auto evaluation = EvaluateAssetThumbnailCache(request);
    if (evaluation.status == AssetThumbnailCacheStatus::Fresh)
        return BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Fresh);

    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);

    if (const auto generator = GeneratorForKind(request.kind);
        generator != nullptr)
    {
        return generator(request, cancelToken);
    }

    return GenerateUnsupportedPreviewThumbnail(request, cancelToken);
}

AssetThumbnailServiceResult BuildExceptionThumbnailResult(
    const AssetThumbnailRequest& request,
    const std::string& diagnostic)
{
    AssetThumbnailServiceResult result;
    result.status = AssetThumbnailServiceStatus::Failed;
    result.fallbackIcon = FallbackIconForKind(request.kind);
    result.diagnostic = diagnostic;
    try
    {
        const auto evaluation = EvaluateAssetThumbnailCache(request);
        result.cacheEntry = evaluation.entry;
        if (evaluation.entry.has_value())
        {
            try
            {
                (void)WriteAssetThumbnailCacheMetadata(
                    request,
                    AssetThumbnailCacheStatus::Failed,
                    result.diagnostic);
            }
            catch (...)
            {
            }
        }
    }
    catch (...)
    {
    }
    return result;
}

AssetThumbnailServiceResult TryGenerateThumbnailForRequest(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCancelToken& cancelToken)
{
    try
    {
        return GenerateThumbnailForRequest(request, cancelToken);
    }
    catch (const std::bad_alloc&)
    {
        return BuildExceptionThumbnailResult(request, "thumbnail-generation-out-of-memory");
    }
    catch (...)
    {
        return BuildExceptionThumbnailResult(request, "thumbnail-generation-exception");
    }
}

std::string ItemFreshnessIdentity(
    const AssetBrowserItem& item,
    const NLS::Core::Assets::AssetId assetId)
{
    auto appendPart = [](std::string& result, const char* label, const std::string& value)
    {
        result += label;
        result.push_back('=');
        result += std::to_string(value.size());
        result.push_back(':');
        result += value;
        result.push_back('|');
    };

    std::string result;
    appendPart(result, "source", item.sourceAssetPath);
    appendPart(result, "subAsset", item.subAssetKey);
    appendPart(result, "assetId", assetId.ToString());
    appendPart(result, "kind", std::to_string(static_cast<int>(item.kind)));
    appendPart(result, "type", std::to_string(static_cast<int>(item.type)));
    appendPart(result, "artifactType", std::to_string(static_cast<int>(item.artifactType)));
    return result;
}

std::optional<NLS::Core::Assets::AssetId> LoadSourceAssetIdFromMeta(
    const std::filesystem::path& projectRoot,
    const std::string& sourceAssetPath)
{
    const auto sourcePath = ResolveEditorAssetPath(
        MakeProjectEditorAssetRoots(projectRoot),
        sourceAssetPath);
    if (sourcePath.empty())
        return std::nullopt;

    const auto meta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(sourcePath));
    if (!meta.has_value() || !meta->id.IsValid())
        return std::nullopt;

    return meta->id;
}
}

std::optional<AssetThumbnailRequest> BuildAssetThumbnailRequestForItem(
    const std::filesystem::path& projectRoot,
    const AssetBrowserItem& item,
    const uint32_t requestedSize)
{
    if (projectRoot.empty() ||
        item.kind == AssetBrowserItemKind::Folder ||
        item.sourceAssetPath.empty())
    {
        return std::nullopt;
    }

    auto assetId = item.assetId;
    if (!assetId.IsValid())
    {
        const auto metaAssetId = LoadSourceAssetIdFromMeta(projectRoot, item.sourceAssetPath);
        if (!metaAssetId.has_value())
            return std::nullopt;
        assetId = *metaAssetId;
    }

    AssetThumbnailRequest request;
    request.projectRoot = projectRoot;
    request.assetId = assetId;
    request.sourceAssetPath = item.sourceAssetPath;
    request.subAssetKey = item.subAssetKey;
    if (item.kind == AssetBrowserItemKind::GeneratedSubAsset ||
        item.type == AssetBrowserItemType::Model ||
        item.type == AssetBrowserItemType::Prefab)
    {
        request.artifactPath = item.artifactPath;
    }
    request.kind = ThumbnailKindForItem(item);
    request.requestedSize = std::max(1u, requestedSize);
    request.settingsFingerprint = "asset-browser-thumbnail:v2-downsampled";
    request.freshnessInputs.push_back({
        "item",
        ItemFreshnessIdentity(item, assetId)
    });
    AddSourceFreshnessInputs(request);
    AddArtifactFreshnessInputs(request, item);
    return request;
}

AssetThumbnailService::~AssetThumbnailService()
{
    if (m_generationCancelToken)
        m_generationCancelToken->cancelled.store(true, std::memory_order_relaxed);
    for (const auto& request : m_inFlightThumbnails)
    {
        if (request.cancelToken)
            request.cancelToken->cancelled.store(true, std::memory_order_relaxed);
    }
    m_generationCancelToken.reset();
    WaitForInFlightRequests();
}

AssetThumbnailServiceResult AssetThumbnailService::GetThumbnail(
    const AssetThumbnailRequest& request)
{
    AssetThumbnailServiceResult result;
    result.fallbackIcon = FallbackIconForKind(request.kind);

    const auto evaluation = EvaluateAssetThumbnailCache(request, AssetThumbnailCacheIntegrityMode::Fast);
    result.cacheEntry = evaluation.entry;
    result.diagnostic = evaluation.diagnostic;

    if (evaluation.status == AssetThumbnailCacheStatus::Fresh &&
        evaluation.entry.has_value())
    {
        result.status = AssetThumbnailServiceStatus::Fresh;
        result.imagePath = evaluation.entry->imagePath;
        return result;
    }

    if (evaluation.status == AssetThumbnailCacheStatus::Failed &&
        !IsRetryableThumbnailFailureDiagnostic(evaluation.diagnostic))
    {
        result.status = AssetThumbnailServiceStatus::Failed;
        return result;
    }

    if (!CanGenerateThumbnail(request.kind))
    {
        result.status = AssetThumbnailServiceStatus::Fallback;
        result.diagnostic = UnsupportedDiagnosticForKind(request.kind);
        return result;
    }

    if (evaluation.entry.has_value())
    {
        if (AdoptMatchingInFlightRequest(evaluation.entry->cacheKey))
        {
            result.status = AssetThumbnailServiceStatus::Pending;
            return result;
        }

        const auto [_, inserted] = m_queuedRequestsByCacheKey.emplace(evaluation.entry->cacheKey, request);
        if (inserted)
            m_queuedCacheKeys.push(evaluation.entry->cacheKey);
    }
    result.status = AssetThumbnailServiceStatus::Pending;
    return result;
}

std::optional<AssetThumbnailServiceResult> AssetThumbnailService::GenerateNextThumbnail()
{
    if (!m_generationCancelToken)
        m_generationCancelToken = std::make_shared<AssetThumbnailGenerationCancelToken>();
    m_generationCancelToken->generation = m_generationSerial;

    while (!m_queuedCacheKeys.empty())
    {
        const auto cacheKey = m_queuedCacheKeys.front();
        m_queuedCacheKeys.pop();

        const auto requestIterator = m_queuedRequestsByCacheKey.find(cacheKey);
        if (requestIterator == m_queuedRequestsByCacheKey.end())
            continue;

        const auto request = requestIterator->second;
        m_queuedRequestsByCacheKey.erase(requestIterator);

        return TryGenerateThumbnailForRequest(request, m_generationCancelToken);
    }

    return std::nullopt;
}

bool AssetThumbnailService::StartNextThumbnailGeneration()
{
    for (auto iterator = m_inFlightThumbnails.begin(); iterator != m_inFlightThumbnails.end();)
    {
        if (!iterator->future.valid())
        {
            iterator = m_inFlightThumbnails.erase(iterator);
            continue;
        }

        if (iterator->generation != m_generationSerial &&
            iterator->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            try
            {
                (void)iterator->future.get();
            }
            catch (...)
            {
            }
            iterator = m_inFlightThumbnails.erase(iterator);
            continue;
        }

        ++iterator;
    }

    if (HasCurrentGenerationInFlightRequest() ||
        m_inFlightThumbnails.size() >= kMaxThumbnailGenerationTotalInFlightSlots)
    {
        return false;
    }

    while (!m_queuedCacheKeys.empty())
    {
        const auto cacheKey = m_queuedCacheKeys.front();
        m_queuedCacheKeys.pop();

        const auto requestIterator = m_queuedRequestsByCacheKey.find(cacheKey);
        if (requestIterator == m_queuedRequestsByCacheKey.end())
            continue;

        const auto request = requestIterator->second;
        m_queuedRequestsByCacheKey.erase(requestIterator);
        if (!m_generationCancelToken)
            m_generationCancelToken = std::make_shared<AssetThumbnailGenerationCancelToken>();
        m_generationCancelToken->generation = m_generationSerial;
        const auto cancelToken = m_generationCancelToken;

        try
        {
            m_inFlightThumbnails.push_back({
                cacheKey,
                m_generationSerial,
                cancelToken,
                std::async(
                    std::launch::async,
                    [request, cancelToken]
                    {
                        return TryGenerateThumbnailForRequest(request, cancelToken);
                    })
            });
        }
        catch (...)
        {
            (void)BuildExceptionThumbnailResult(request, "thumbnail-generation-worker-start-failed");
            return false;
        }
        return true;
    }

    return false;
}

std::optional<AssetThumbnailServiceResult> AssetThumbnailService::ConsumeCompletedThumbnail()
{
    for (auto iterator = m_inFlightThumbnails.begin(); iterator != m_inFlightThumbnails.end();)
    {
        if (!iterator->future.valid())
        {
            iterator = m_inFlightThumbnails.erase(iterator);
            continue;
        }

        if (iterator->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            ++iterator;
            continue;
        }

        AssetThumbnailServiceResult result;
        try
        {
            result = iterator->future.get();
        }
        catch (const std::bad_alloc&)
        {
            result.status = AssetThumbnailServiceStatus::Failed;
            result.fallbackIcon = "Icon_Unknown";
            result.diagnostic = "thumbnail-generation-out-of-memory";
        }
        catch (...)
        {
            result.status = AssetThumbnailServiceStatus::Failed;
            result.fallbackIcon = "Icon_Unknown";
            result.diagnostic = "thumbnail-generation-exception";
        }
        const bool currentGeneration = iterator->generation == m_generationSerial;
        iterator = m_inFlightThumbnails.erase(iterator);
        if (currentGeneration)
            return result;
    }
    return std::nullopt;
}

bool AssetThumbnailService::HasInFlightRequest() const
{
    return !m_inFlightThumbnails.empty();
}

bool AssetThumbnailService::HasCurrentGenerationInFlightRequest() const
{
    return std::any_of(
        m_inFlightThumbnails.begin(),
        m_inFlightThumbnails.end(),
        [this](const InFlightThumbnailRequest& request)
        {
            return request.generation == m_generationSerial &&
                request.future.valid();
        });
}

size_t AssetThumbnailService::GetQueuedRequestCount() const
{
    return m_queuedRequestsByCacheKey.size();
}

void AssetThumbnailService::ClearQueuedRequests()
{
    ++m_generationSerial;
    ClearPendingQueuedRequests();
}

void AssetThumbnailService::ClearPendingQueuedRequests()
{
    while (!m_queuedCacheKeys.empty())
        m_queuedCacheKeys.pop();
    m_queuedRequestsByCacheKey.clear();
}

void AssetThumbnailService::SupersedeQueuedRequestsForGeneration(
    const std::string& generationFingerprint)
{
    if (m_generationFingerprint == generationFingerprint)
        return;

    m_generationFingerprint = generationFingerprint;
    ++m_generationSerial;
    m_generationCancelToken = std::make_shared<AssetThumbnailGenerationCancelToken>();
    m_generationCancelToken->generation = m_generationSerial;
    ClearPendingQueuedRequests();
}

void AssetThumbnailService::WaitForInFlightRequests()
{
    for (auto& request : m_inFlightThumbnails)
    {
        if (request.future.valid())
        {
            try
            {
                (void)request.future.get();
            }
            catch (...)
            {
            }
        }
    }
    m_inFlightThumbnails.clear();
}

bool AssetThumbnailService::AdoptMatchingInFlightRequest(const std::string& cacheKey)
{
    for (auto& request : m_inFlightThumbnails)
    {
        if (request.cacheKey == cacheKey && request.future.valid())
        {
            request.generation = m_generationSerial;
            return true;
        }
    }
    return false;
}
}
