#include "Assets/AssetThumbnailService.h"

#include "Assets/AssetMeta.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/EditorThumbnailPreviewRenderer.h"
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
#include <string_view>
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

struct AssetThumbnailKindPolicy
{
    AssetThumbnailKind kind = AssetThumbnailKind::GenericPreview;
    const char* fallbackIcon = "editor.icon.asset.default";
    AssetThumbnailGenerator generator = nullptr;
    const char* unsupportedDiagnostic = "thumbnail-generation-unsupported";
};

constexpr std::array<AssetThumbnailKindPolicy, kAssetThumbnailKindCount> kAssetThumbnailKindPolicies {{
    { AssetThumbnailKind::Icon, "editor.icon.asset.default", nullptr, "thumbnail-generation-unsupported" },
    { AssetThumbnailKind::Texture, "editor.icon.asset.texture", GenerateTextureThumbnail, "thumbnail-generation-unsupported" },
    { AssetThumbnailKind::MaterialSphere, "editor.icon.asset.material", nullptr, "thumbnail-material-gpu-preview-required" },
    { AssetThumbnailKind::ModelPreview, "editor.icon.asset.mesh", nullptr, "thumbnail-model-gpu-preview-required" },
    { AssetThumbnailKind::PrefabPreview, "editor.icon.asset.prefab", nullptr, "thumbnail-prefab-gpu-preview-required" },
    { AssetThumbnailKind::GenericPreview, "editor.icon.asset.default", nullptr, "thumbnail-generation-unsupported" }
}};

constexpr size_t kMaxMeshPreviewLoadedVertices = 240000u;
constexpr size_t kMaxMeshPreviewLoadedIndices = 720000u;
constexpr size_t kMaxMeshPreviewRenderedTriangles = 12000u;
constexpr float kUnityMeshPreviewFieldOfViewDegrees = 30.0f;
constexpr float kUnityMeshPreviewYawDegrees = -120.0f;
constexpr float kUnityMeshPreviewPitchDegrees = 20.0f;
constexpr float kDegreesToRadians = 3.14159265358979323846f / 180.0f;
constexpr size_t kMaxObsoleteThumbnailGenerationInFlightRequests = 2u;
constexpr size_t kMaxThumbnailGenerationTotalInFlightSlots =
    kMaxObsoleteThumbnailGenerationInFlightRequests + 1u;
constexpr uint64_t kMaxSourceThumbnailImageBytes = 128ull * 1024ull * 1024ull;
constexpr uint64_t kMaxSourceThumbnailPixels = 4096ull * 4096ull;
constexpr uint32_t kMaxTextureThumbnailGenerationSize = 96u;
constexpr uint64_t kMaxStructurePreviewArtifactPayloadBytes = 1024ull * 1024ull;
constexpr uint64_t kMaxThumbnailPreviewNativeArtifactFileBytes = 128ull * 1024ull * 1024ull;
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
        if (item.kind == AssetBrowserItemKind::SourceAsset)
            return AssetThumbnailKind::PrefabPreview;
        return AssetThumbnailKind::ModelPreview;
    case AssetBrowserItemType::Mesh:
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
    return policy != nullptr ? policy->fallbackIcon : "editor.icon.asset.default";
}

bool CanGenerateThumbnail(const AssetThumbnailKind kind)
{
    const auto* policy = PolicyForKind(kind);
    return policy != nullptr && policy->generator != nullptr;
}

bool SupportsGpuThumbnailPreview(const AssetThumbnailKind kind)
{
    return kind == AssetThumbnailKind::MaterialSphere ||
        kind == AssetThumbnailKind::ModelPreview ||
        kind == AssetThumbnailKind::PrefabPreview;
}

bool CanRequestThumbnailGeneration(const AssetThumbnailKind kind)
{
    return CanGenerateThumbnail(kind) || SupportsGpuThumbnailPreview(kind);
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
    if (diagnostic == "thumbnail-gpu-preview-empty-frame")
        return false;
    if (diagnostic.rfind("thumbnail-gpu-preview-", 0u) == 0u)
        return true;
    return diagnostic == "thumbnail-material-preview-hook-unavailable" ||
        diagnostic == "thumbnail-model-preview-hook-unavailable" ||
        diagnostic == "thumbnail-prefab-preview-hook-unavailable" ||
        diagnostic == "thumbnail-material-gpu-preview-required" ||
        diagnostic == "thumbnail-model-gpu-preview-required" ||
        diagnostic == "thumbnail-prefab-gpu-preview-required" ||
        diagnostic == "thumbnail-material-artifact-missing" ||
        diagnostic == "thumbnail-prefab-artifact-missing" ||
        diagnostic == "thumbnail-material-preview-generation-failed" ||
        diagnostic == "thumbnail-model-preview-generation-failed" ||
        diagnostic == "thumbnail-prefab-preview-generation-failed" ||
        diagnostic == "thumbnail-generation-worker-start-failed";
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

std::string ToLowerAscii(std::string value)
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

struct ThumbnailTextureSampleData
{
    std::vector<uint8_t> pixels;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t rowPitch = 0u;
    bool flipV = false;
};

struct MaterialTextureReference
{
    std::string resourcePath;
    std::string textureKey;
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

uint32_t GetTextureThumbnailGenerationSize(const AssetThumbnailRequest& request)
{
    return (std::min)(std::max(1u, request.requestedSize), kMaxTextureThumbnailGenerationSize);
}

bool IsRgba8TextureArtifactMipUsable(const NLS::Render::Assets::TextureArtifactMip& mip)
{
    return mip.width > 0u &&
        mip.height > 0u &&
        mip.rowPitch >= mip.width * 4u &&
        !mip.pixels.empty();
}

const NLS::Render::Assets::TextureArtifactMip* SelectTextureThumbnailMip(
    const NLS::Render::Assets::TextureArtifactData& texture,
    const uint32_t targetSize)
{
    const NLS::Render::Assets::TextureArtifactMip* best = nullptr;
    uint64_t bestPixels = 0u;
    const auto minUsableDimension = std::max(1u, targetSize);

    for (const auto& mip : texture.mips)
    {
        if (!IsRgba8TextureArtifactMipUsable(mip))
            continue;

        const auto pixels = static_cast<uint64_t>(mip.width) * static_cast<uint64_t>(mip.height);
        const auto coversTarget = mip.width >= minUsableDimension || mip.height >= minUsableDimension;
        if (!best)
        {
            best = &mip;
            bestPixels = pixels;
            continue;
        }

        const auto bestCoversTarget = best->width >= minUsableDimension || best->height >= minUsableDimension;
        if (coversTarget != bestCoversTarget)
        {
            if (coversTarget)
            {
                best = &mip;
                bestPixels = pixels;
            }
            continue;
        }

        if ((coversTarget && pixels < bestPixels) || (!coversTarget && pixels > bestPixels))
        {
            best = &mip;
            bestPixels = pixels;
        }
    }

    return best;
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

bool IsMissingThumbnailArtifactPath(const AssetThumbnailRequest& request)
{
    if (request.artifactPath.empty() || !request.assetId.IsValid())
        return false;

    const auto sourceArtifactRoot = NLS::Core::Assets::NormalizeAssetPath(
        request.projectRoot / "Library" / "Artifacts" / request.assetId.ToString());
    if (sourceArtifactRoot.empty())
        return false;

    const auto rawPath = std::filesystem::path(request.artifactPath).lexically_normal();
    std::vector<std::filesystem::path> candidates;
    if (rawPath.is_absolute())
    {
        candidates.push_back(rawPath);
    }
    else
    {
        candidates.push_back(request.projectRoot / rawPath);
        candidates.push_back(sourceArtifactRoot / rawPath);
    }

    for (const auto& candidate : candidates)
    {
        const auto normalized = NLS::Core::Assets::NormalizeAssetPath(candidate);
        if (normalized.empty() || !IsPathInsideEditorAssetRoot(normalized, sourceArtifactRoot))
            continue;

        std::error_code error;
        const bool exists = std::filesystem::exists(normalized, error);
        if (!error && !exists)
            return true;
        if (!error && exists)
            return false;
    }

    return false;
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

AssetThumbnailServiceResult WriteRgbaThumbnailResult(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation,
    const uint8_t* pixels,
    const uint32_t width,
    const uint32_t height,
    const std::string& emptyDiagnostic,
    const AssetThumbnailCancelToken& cancelToken)
{
    DownsampledThumbnail thumbnail;
    if (pixels != nullptr && width > 0u && height > 0u)
    {
        thumbnail.pixels.assign(
            pixels,
            pixels + static_cast<size_t>(width) * height * 4u);
        thumbnail.width = width;
        thumbnail.height = height;
    }
    return WriteThumbnailPngResult(
        request,
        evaluation,
        thumbnail,
        emptyDiagnostic,
        cancelToken);
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

std::optional<std::filesystem::path> ResolveSourceMaterialPathForPreview(
    const AssetThumbnailRequest& request,
    const std::string& artifactPath)
{
    if (artifactPath.empty())
        return std::nullopt;

    const auto rawPath = std::filesystem::path(artifactPath).lexically_normal();
    if (rawPath.extension() != ".mat")
        return std::nullopt;

    const auto assetsRoot = NLS::Core::Assets::NormalizeAssetPath(request.projectRoot / "Assets");
    if (assetsRoot.empty())
        return std::nullopt;

    const auto candidate = rawPath.is_absolute()
        ? rawPath
        : request.projectRoot / rawPath;
    const auto normalized = NLS::Core::Assets::NormalizeAssetPath(candidate);
    if (normalized.empty() ||
        !IsPhysicalRegularFileInsideEditorAssetRoot(normalized, assetsRoot))
    {
        return std::nullopt;
    }
    return normalized;
}

std::optional<std::filesystem::path> ResolveSourceMaterialPathForPreview(
    const AssetThumbnailRequest& request)
{
    if (auto resolved = ResolveSourceMaterialPathForPreview(request, request.artifactPath);
        resolved.has_value())
    {
        return resolved;
    }
    return ResolveSourceMaterialPathForPreview(request, request.sourceAssetPath);
}

bool IsGpuPreviewClearFrame(
    const std::vector<uint8_t>& rgbaPixels,
    const uint32_t width,
    const uint32_t height)
{
    const size_t pixelCount = static_cast<size_t>(width) * height;
    if (pixelCount == 0u || rgbaPixels.size() < pixelCount * 4u)
        return true;

    const uint8_t firstR = rgbaPixels[0u];
    const uint8_t firstG = rgbaPixels[1u];
    const uint8_t firstB = rgbaPixels[2u];
    const uint8_t firstA = rgbaPixels[3u];
    if (firstA != 0u)
        return false;

    for (size_t pixel = 0u; pixel < pixelCount; ++pixel)
    {
        const uint8_t r = rgbaPixels[pixel * 4u + 0u];
        const uint8_t g = rgbaPixels[pixel * 4u + 1u];
        const uint8_t b = rgbaPixels[pixel * 4u + 2u];
        const uint8_t a = rgbaPixels[pixel * 4u + 3u];
        if (r != firstR || g != firstG || b != firstB || a != firstA)
            return false;
    }
    return true;
}

bool PreviewArtifactPathResolvesForRequest(
    const AssetThumbnailRequest& request,
    const std::string& artifactPath)
{
    if (ResolveArtifactPathForPreview(request, artifactPath).has_value())
        return true;

    return request.kind == AssetThumbnailKind::MaterialSphere &&
        ResolveSourceMaterialPathForPreview(request, artifactPath).has_value();
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

std::string GpuPreviewArtifactPathInvalidDiagnostic(const AssetThumbnailKind kind)
{
    switch (kind)
    {
    case AssetThumbnailKind::MaterialSphere:
        return "thumbnail-material-artifact-path-invalid";
    case AssetThumbnailKind::PrefabPreview:
        return "thumbnail-prefab-artifact-path-invalid";
    case AssetThumbnailKind::ModelPreview:
        return "thumbnail-model-mesh-artifact-path-invalid";
    default:
        return "thumbnail-artifact-path-invalid";
    }
}

std::optional<std::string> ValidateGpuPreviewRequestArtifactPaths(const AssetThumbnailRequest& request)
{
    if (!SupportsGpuThumbnailPreview(request.kind))
        return std::nullopt;

    if (!request.artifactPath.empty() &&
        !ResolveArtifactPathForPreview(request, request.artifactPath).has_value() &&
        !(request.kind == AssetThumbnailKind::MaterialSphere &&
            ResolveSourceMaterialPathForPreview(request, request.artifactPath).has_value()))
    {
        return GpuPreviewArtifactPathInvalidDiagnostic(request.kind);
    }

    const auto manifest = LoadThumbnailArtifactManifest(request);
    if (!manifest.has_value())
        return std::nullopt;

    auto validateArtifactPath = [&request](const NLS::Core::Assets::ImportedArtifact& artifact)
        -> std::optional<std::string>
    {
        if (artifact.artifactPath.empty() ||
            PreviewArtifactPathResolvesForRequest(request, artifact.artifactPath))
        {
            return std::nullopt;
        }
        return GpuPreviewArtifactPathInvalidDiagnostic(request.kind);
    };

    if (!request.subAssetKey.empty())
    {
        const auto* artifact = manifest->FindSubAsset(request.subAssetKey);
        if (artifact == nullptr)
            return std::nullopt;

        const bool matchesRequest =
            (request.kind == AssetThumbnailKind::MaterialSphere &&
                artifact->artifactType == NLS::Core::Assets::ArtifactType::Material) ||
            (request.kind == AssetThumbnailKind::PrefabPreview &&
                artifact->artifactType == NLS::Core::Assets::ArtifactType::Prefab) ||
            (request.kind == AssetThumbnailKind::ModelPreview &&
                artifact->artifactType == NLS::Core::Assets::ArtifactType::Mesh);
        if (!matchesRequest)
            return std::nullopt;
        return validateArtifactPath(*artifact);
    }

    for (const auto& artifact : manifest->subAssets)
    {
        const bool relevant =
            (request.kind == AssetThumbnailKind::MaterialSphere &&
                artifact.artifactType == NLS::Core::Assets::ArtifactType::Material) ||
            (request.kind == AssetThumbnailKind::PrefabPreview &&
                artifact.artifactType == NLS::Core::Assets::ArtifactType::Prefab) ||
            (request.kind == AssetThumbnailKind::ModelPreview &&
                artifact.artifactType == NLS::Core::Assets::ArtifactType::Mesh);
        if (!relevant || artifact.artifactPath.empty())
            continue;
        if (auto diagnostic = validateArtifactPath(artifact);
            diagnostic.has_value())
        {
            return diagnostic;
        }
    }
    return std::nullopt;
}

const std::optional<NLS::Core::Assets::ArtifactManifest>* LoadThumbnailArtifactManifestCached(
    const AssetThumbnailRequest& request,
    AssetThumbnailRequestBuildContext* context)
{
    if (context == nullptr)
        return nullptr;

    const auto key = request.assetId.ToString();
    auto [iterator, inserted] = context->artifactManifestsByAssetId.emplace(
        key,
        std::optional<NLS::Core::Assets::ArtifactManifest> {});
    if (inserted)
        iterator->second = LoadThumbnailArtifactManifest(request);
    return &iterator->second;
}

const NLS::Core::Assets::ImportedArtifact* FindThumbnailArtifactForItem(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const AssetBrowserItem& item)
{
    if (!item.subAssetKey.empty())
    {
        if (const auto* subAsset = manifest.FindSubAsset(item.subAssetKey))
            return subAsset;
    }

    const auto wantedType = item.type == AssetBrowserItemType::Prefab ||
            (item.type == AssetBrowserItemType::Model && item.kind == AssetBrowserItemKind::SourceAsset)
        ? NLS::Core::Assets::ArtifactType::Prefab
        : item.type == AssetBrowserItemType::Material
            ? NLS::Core::Assets::ArtifactType::Material
            : item.type == AssetBrowserItemType::Mesh || item.type == AssetBrowserItemType::Model
                ? NLS::Core::Assets::ArtifactType::Mesh
                : NLS::Core::Assets::ArtifactType::Unknown;

    if (const auto* primary = manifest.FindPrimaryArtifact())
    {
        if ((wantedType != NLS::Core::Assets::ArtifactType::Unknown && primary->artifactType == wantedType) ||
            (wantedType == NLS::Core::Assets::ArtifactType::Unknown &&
                (primary->artifactType == item.artifactType ||
                    item.artifactType == NLS::Core::Assets::ArtifactType::Unknown)))
        {
            return primary;
        }
    }

    if (wantedType == NLS::Core::Assets::ArtifactType::Unknown)
        return nullptr;

    for (const auto& artifact : manifest.subAssets)
    {
        if (artifact.artifactType == wantedType)
            return &artifact;
    }
    return nullptr;
}

bool HasExtension(const std::filesystem::path& path, const char* extension)
{
    return ToLowerAscii(path.extension().generic_string()) == extension;
}

bool IsRgba8TextureArtifactMipUsable(const NLS::Render::Assets::TextureArtifactData& artifact)
{
    return artifact.format == NLS::Render::RHI::TextureFormat::RGBA8 &&
        SelectTextureThumbnailMip(artifact, kMaxTextureThumbnailGenerationSize) != nullptr;
}

std::string TextureSourceKeyFromSubAssetKey(const std::string& subAssetKey)
{
    constexpr std::string_view kPrefix = "texture:";
    if (subAssetKey.rfind(kPrefix, 0u) != 0u)
        return {};
    return subAssetKey.substr(kPrefix.size());
}

std::optional<std::string> TextureDependencySourcePath(
    const NLS::Core::Assets::AssetDependencyRecord& dependency,
    const std::string& textureSourceKey)
{
    if (dependency.kind != NLS::Core::Assets::AssetDependencyKind::PostprocessorVersion ||
        textureSourceKey.empty())
    {
        return std::nullopt;
    }

    const std::string expectedValue = "texture-build:texture:" + textureSourceKey;
    if (dependency.value != expectedValue)
        return std::nullopt;

    constexpr std::string_view kSourcePathToken = "sourcePath=";
    const auto sourceBegin = dependency.hashOrVersion.find(kSourcePathToken);
    if (sourceBegin == std::string::npos)
        return std::nullopt;

    const auto valueBegin = sourceBegin + kSourcePathToken.size();
    auto valueEnd = dependency.hashOrVersion.find('|', valueBegin);
    if (valueEnd == std::string::npos)
        valueEnd = dependency.hashOrVersion.size();
    if (valueEnd <= valueBegin)
        return std::nullopt;

    auto sourcePath = dependency.hashOrVersion.substr(valueBegin, valueEnd - valueBegin);
    std::replace(sourcePath.begin(), sourcePath.end(), '\\', '/');
    while (sourcePath.rfind("./", 0u) == 0u)
        sourcePath.erase(0u, 2u);
    return sourcePath.empty() ? std::nullopt : std::optional<std::string>(sourcePath);
}

std::optional<std::filesystem::path> ResolveTextureSourceDependencyPath(const AssetThumbnailRequest& request)
{
    const auto textureSourceKey = TextureSourceKeyFromSubAssetKey(request.subAssetKey);
    if (textureSourceKey.empty())
        return std::nullopt;
    const auto manifest = LoadThumbnailArtifactManifest(request);
    if (!manifest.has_value())
        return std::nullopt;

    for (const auto& dependency : manifest->dependencies)
    {
        const auto sourcePathText = TextureDependencySourcePath(dependency, textureSourceKey);
        if (!sourcePathText.has_value())
            continue;

        const auto sourcePath = std::filesystem::path(*sourcePathText).lexically_normal();
        std::vector<std::filesystem::path> candidates;
        if (sourcePath.is_absolute())
        {
            candidates.push_back(sourcePath);
        }
        else
        {
            candidates.push_back(request.projectRoot / sourcePath);
            if (!request.sourceAssetPath.empty())
            {
                candidates.push_back(
                    request.projectRoot /
                    std::filesystem::path(request.sourceAssetPath).parent_path() /
                    sourcePath);
            }
        }

        const auto assetRoots = MakeProjectEditorAssetRoots(request.projectRoot);
        for (const auto& candidate : candidates)
        {
            const auto normalized = NLS::Core::Assets::NormalizeAssetPath(candidate);
            if (normalized.empty() || !IsTextureThumbnailSourceExtension(normalized))
                continue;

            const auto editorAssetPath = ToEditorAssetPath(assetRoots, normalized);
            if (!ResolveEditorAssetPath(assetRoots, editorAssetPath).empty())
                return normalized;
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> ResolveTextureSourceDependencyPathForKey(
    const AssetThumbnailRequest& request,
    const std::string& textureSourceKey)
{
    if (textureSourceKey.empty())
        return std::nullopt;

    AssetThumbnailRequest textureRequest = request;
    textureRequest.subAssetKey = "texture:" + textureSourceKey;
    return ResolveTextureSourceDependencyPath(textureRequest);
}

bool ShouldFlipMaterialSourceTextureVertically(const AssetThumbnailRequest& request)
{
    const auto extension = ToLowerAscii(std::filesystem::path(request.sourceAssetPath).extension().generic_string());
    return extension != ".gltf" && extension != ".glb";
}

std::vector<std::filesystem::path> ResolveMeshArtifactPaths(
    const AssetThumbnailRequest& request)
{
    std::vector<std::filesystem::path> paths;
    const auto directMeshPath = ResolveArtifactPathForPreview(request, request.artifactPath);
    const bool directPathIsMesh =
        directMeshPath.has_value() && HasExtension(*directMeshPath, ".nmesh");

    const auto manifest = LoadThumbnailArtifactManifest(request);
    if (!manifest.has_value())
    {
        if (directPathIsMesh)
            paths.push_back(*directMeshPath);
        return paths;
    }

    if (!request.subAssetKey.empty())
    {
        for (const auto& artifact : manifest->subAssets)
        {
            if (artifact.artifactType != NLS::Core::Assets::ArtifactType::Mesh ||
                artifact.subAssetKey != request.subAssetKey)
            {
                continue;
            }

            if (auto resolved = ResolveArtifactPathForPreview(request, artifact.artifactPath);
                resolved.has_value())
            {
                paths.push_back(*resolved);
                return paths;
            }
        }

        if (directPathIsMesh)
            paths.push_back(*directMeshPath);
        return paths;
    }

    for (const auto& artifact : manifest->subAssets)
    {
        if (artifact.artifactType != NLS::Core::Assets::ArtifactType::Mesh)
            continue;

        if (auto resolved = ResolveArtifactPathForPreview(request, artifact.artifactPath);
            resolved.has_value())
        {
            paths.push_back(*resolved);
        }
    }
    if (paths.empty() && directPathIsMesh)
        paths.push_back(*directMeshPath);
    return paths;
}

std::optional<std::filesystem::path> ResolveFirstMeshArtifactPath(
    const AssetThumbnailRequest& request)
{
    const auto paths = ResolveMeshArtifactPaths(request);
    if (paths.empty())
        return std::nullopt;
    return paths.front();
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

std::string UnescapeXmlAttributeValue(std::string value)
{
    auto replaceAll = [&value](const std::string_view from, const std::string_view to)
    {
        size_t position = 0u;
        while ((position = value.find(from, position)) != std::string::npos)
        {
            value.replace(position, from.size(), to);
            position += to.size();
        }
    };

    replaceAll("&quot;", "\"");
    replaceAll("&apos;", "'");
    replaceAll("&lt;", "<");
    replaceAll("&gt;", ">");
    replaceAll("&amp;", "&");
    return value;
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

std::optional<MaterialTextureReference> ExtractMaterialTextureReference(const std::string& xml)
{
    constexpr std::array<std::string_view, 6u> kPreferredTextureSlotNames {
        "BaseColor",
        "Albedo",
        "Diffuse",
        "baseColor",
        "albedo",
        "diffuse"
    };
    constexpr std::array<std::string_view, 8u> kTextureUniformNames {
        "u_AlbedoMap",
        "u_DiffuseMap",
        "BaseColorTexture",
        "BaseColorMap",
        "DiffuseTexture",
        "DiffuseMap",
        "AlbedoTexture",
        "AlbedoMap"
    };

    for (const auto slotName : kPreferredTextureSlotNames)
    {
        size_t position = 0u;
        const std::string needle = "name=\"" + std::string(slotName) + "\"";
        while ((position = xml.find(needle, position)) != std::string::npos)
        {
            const auto elementBegin = xml.rfind('<', position);
            const auto elementEnd = xml.find('>', position);
            if (elementBegin == std::string::npos || elementEnd == std::string::npos)
            {
                position += needle.size();
                continue;
            }

            const auto element = xml.substr(elementBegin, elementEnd - elementBegin + 1u);
            if (element.find("<textureSlot") == std::string::npos)
            {
                position = elementEnd + 1u;
                continue;
            }

            MaterialTextureReference reference;
            if (auto key = ExtractXmlAttribute(element, "texture");
                key.has_value() && !key->empty())
            {
                reference.textureKey = UnescapeXmlAttributeValue(*key);
            }
            for (const auto attribute : {"resourcePath", "texture", "value"})
            {
                if (auto value = ExtractXmlAttribute(element, attribute);
                    value.has_value() && !value->empty())
                {
                    reference.resourcePath = UnescapeXmlAttributeValue(*value);
                    return reference;
                }
            }
            position = elementEnd + 1u;
        }
    }

    for (const auto uniformName : kTextureUniformNames)
    {
        size_t position = 0u;
        const std::string needle = "name=\"" + std::string(uniformName) + "\"";
        while ((position = xml.find(needle, position)) != std::string::npos)
        {
            const auto elementBegin = xml.rfind('<', position);
            const auto elementEnd = xml.find('>', position);
            if (elementBegin == std::string::npos || elementEnd == std::string::npos)
            {
                position += needle.size();
                continue;
            }

            const auto element = xml.substr(elementBegin, elementEnd - elementBegin + 1u);
            if (auto value = ExtractXmlAttribute(element, "value");
                value.has_value() && !value->empty())
            {
                return MaterialTextureReference {UnescapeXmlAttributeValue(*value), {}};
            }
            position = elementEnd + 1u;
        }
    }

    size_t textureSlotPosition = 0u;
    while ((textureSlotPosition = xml.find("<textureSlot", textureSlotPosition)) != std::string::npos)
    {
        const auto elementEnd = xml.find('>', textureSlotPosition);
        if (elementEnd == std::string::npos)
            break;

        const auto element = xml.substr(textureSlotPosition, elementEnd - textureSlotPosition + 1u);
        MaterialTextureReference reference;
        if (auto key = ExtractXmlAttribute(element, "texture");
            key.has_value() && !key->empty())
        {
            reference.textureKey = UnescapeXmlAttributeValue(*key);
        }
        for (const auto attribute : {"resourcePath", "texture", "value"})
        {
            if (auto value = ExtractXmlAttribute(element, attribute);
                value.has_value() && !value->empty())
            {
                reference.resourcePath = UnescapeXmlAttributeValue(*value);
                return reference;
            }
        }
        textureSlotPosition = elementEnd + 1u;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> ResolveMaterialSourceTextureDependency(
    const AssetThumbnailRequest& request,
    const std::string& materialPayload,
    const std::string& textureKey)
{
    if (textureKey.empty())
        return std::nullopt;

    if (auto manifestSource = ResolveTextureSourceDependencyPathForKey(request, textureKey);
        manifestSource.has_value())
    {
        return manifestSource;
    }

    const std::string needle = "texture-build:texture:" + textureKey + "\\p";
    const auto position = materialPayload.find(needle);
    if (position == std::string::npos)
        return std::nullopt;

    const auto sourcePathBegin = materialPayload.find("\\psourcePath=", position + needle.size());
    if (sourcePathBegin == std::string::npos)
        return std::nullopt;

    const auto valueBegin = sourcePathBegin + std::string_view("\\psourcePath=").size();
    auto valueEnd = materialPayload.find("\\\\p", valueBegin);
    if (valueEnd == std::string::npos)
        valueEnd = materialPayload.find("\\p", valueBegin);
    if (valueEnd == std::string::npos || valueEnd <= valueBegin)
        return std::nullopt;

    auto sourcePathText = materialPayload.substr(valueBegin, valueEnd - valueBegin);
    std::replace(sourcePathText.begin(), sourcePathText.end(), '\\', '/');
    while (sourcePathText.rfind("./", 0u) == 0u)
        sourcePathText.erase(0u, 2u);
    if (sourcePathText.empty())
        return std::nullopt;

    std::vector<std::filesystem::path> candidates;
    const auto sourcePath = std::filesystem::path(sourcePathText).lexically_normal();
    if (sourcePath.is_absolute())
    {
        candidates.push_back(sourcePath);
    }
    else
    {
        candidates.push_back(request.projectRoot / sourcePath);
        if (!request.sourceAssetPath.empty())
        {
            candidates.push_back(
                request.projectRoot /
                std::filesystem::path(request.sourceAssetPath).parent_path() /
                sourcePath);
        }
    }

    const auto assetRoots = MakeProjectEditorAssetRoots(request.projectRoot);
    for (const auto& candidate : candidates)
    {
        const auto normalized = NLS::Core::Assets::NormalizeAssetPath(candidate);
        if (normalized.empty() || !IsTextureThumbnailSourceExtension(normalized))
            continue;

        const auto editorAssetPath = ToEditorAssetPath(assetRoots, normalized);
        if (!ResolveEditorAssetPath(assetRoots, editorAssetPath).empty())
            return normalized;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> ResolveTexturePathFromMaterialPayload(
    const AssetThumbnailRequest& request,
    const std::string& materialPayload)
{
    const auto textureReference = ExtractMaterialTextureReference(materialPayload);
    if (!textureReference.has_value() || textureReference->resourcePath.empty())
        return std::nullopt;

    auto texturePath = std::filesystem::path(textureReference->resourcePath).lexically_normal();
    if (!HasExtension(texturePath, ".ntex") &&
        !IsTextureThumbnailSourceExtension(texturePath))
    {
        return ResolveMaterialSourceTextureDependency(
            request,
            materialPayload,
            textureReference->textureKey);
    }

    std::vector<std::filesystem::path> candidates;
    if (texturePath.is_absolute())
    {
        candidates.push_back(texturePath);
    }
    else
    {
        candidates.push_back(request.projectRoot / texturePath);
        if (request.assetId.IsValid())
        {
            candidates.push_back(
                request.projectRoot /
                "Library" /
                "Artifacts" /
                request.assetId.ToString() /
                texturePath);
        }
    }

    const auto projectRoot = NLS::Core::Assets::NormalizeAssetPath(request.projectRoot);
    const auto artifactRoot = NLS::Core::Assets::NormalizeAssetPath(projectRoot / "Library" / "Artifacts");
    const auto assetRoots = MakeProjectEditorAssetRoots(projectRoot);
    for (const auto& candidate : candidates)
    {
        const auto normalized = NLS::Core::Assets::NormalizeAssetPath(candidate);
        if (normalized.empty())
            continue;

        const bool libraryArtifact =
            HasExtension(normalized, ".ntex") &&
            !artifactRoot.empty() &&
            IsPhysicalRegularFileInsideEditorAssetRoot(normalized, artifactRoot);
        const auto editorAssetPath = ToEditorAssetPath(assetRoots, normalized);
        const bool sourceTexture =
            IsTextureThumbnailSourceExtension(normalized) &&
            !ResolveEditorAssetPath(assetRoots, editorAssetPath).empty();
        if (libraryArtifact || sourceTexture)
            return normalized;
    }
    return ResolveMaterialSourceTextureDependency(
        request,
        materialPayload,
        textureReference->textureKey);
}

std::optional<std::filesystem::path> ResolveTextureSamplePathFromMaterialPayload(
    const AssetThumbnailRequest& request,
    const std::string& materialPayload)
{
    const auto textureReference = ExtractMaterialTextureReference(materialPayload);
    const auto sourceExtension = ToLowerAscii(std::filesystem::path(request.sourceAssetPath).extension().generic_string());
    if (textureReference.has_value() &&
        (sourceExtension == ".fbx" || sourceExtension == ".obj"))
    {
        if (auto sourceTexture = ResolveMaterialSourceTextureDependency(
                request,
                materialPayload,
                textureReference->textureKey);
            sourceTexture.has_value())
        {
            return sourceTexture;
        }
    }

    const auto texturePath = ResolveTexturePathFromMaterialPayload(request, materialPayload);
    if (!texturePath.has_value())
        return std::nullopt;

    if (!HasExtension(*texturePath, ".ntex"))
        return texturePath;

    const auto artifact = NLS::Render::Assets::LoadTextureArtifact(*texturePath);
    if (artifact.has_value() &&
        IsRgba8TextureArtifactMipUsable(*artifact))
    {
        return texturePath;
    }

    if (textureReference.has_value())
    {
        return ResolveMaterialSourceTextureDependency(
            request,
            materialPayload,
            textureReference->textureKey);
    }
    return std::nullopt;
}

DownsampledThumbnail RenderTexturePathThumbnail(
    const std::filesystem::path& texturePath,
    const uint32_t requestedSize)
{
    if (HasExtension(texturePath, ".ntex"))
    {
        const auto artifact = NLS::Render::Assets::LoadTextureArtifact(texturePath);
        if (!artifact.has_value() ||
            !IsRgba8TextureArtifactMipUsable(*artifact))
        {
            return {};
        }

        const auto& mip = artifact->mips.front();
        return DownsampleRgba8ToThumbnail(
            mip.pixels.data(),
            mip.width,
            mip.height,
            mip.rowPitch,
            requestedSize);
    }

    NLS::Image sourceImage(texturePath.string(), false);
    if (sourceImage.GetData() == nullptr ||
        sourceImage.GetWidth() <= 0 ||
        sourceImage.GetHeight() <= 0 ||
        sourceImage.GetChannels() <= 0)
    {
        return {};
    }
    return DownsampleImageToThumbnail(sourceImage, requestedSize);
}

const NLS::Render::Assets::TextureArtifactMip* SelectTexturePreviewMip(
    const NLS::Render::Assets::TextureArtifactData& artifact,
    const uint32_t requestedSize)
{
    if (artifact.mips.empty())
        return nullptr;

    const auto targetSize = std::max(1u, requestedSize);
    const NLS::Render::Assets::TextureArtifactMip* bestMip = nullptr;
    uint32_t bestScore = std::numeric_limits<uint32_t>::max();
    for (const auto& mip : artifact.mips)
    {
        if (mip.width == 0u || mip.height == 0u || mip.rowPitch < mip.width * 4u || mip.pixels.empty())
            continue;

        const auto mipLargestDimension = (std::max)(mip.width, mip.height);
        const auto score = mipLargestDimension > targetSize
            ? mipLargestDimension - targetSize
            : (targetSize - mipLargestDimension) * 2u;
        if (bestMip == nullptr || score < bestScore)
        {
            bestMip = &mip;
            bestScore = score;
        }
    }
    return bestMip;
}

std::optional<ThumbnailTextureSampleData> LoadTextureSampleData(
    const std::filesystem::path& texturePath,
    const uint32_t requestedSize)
{
    ThumbnailTextureSampleData data;
    if (HasExtension(texturePath, ".ntex"))
    {
        const auto artifact = NLS::Render::Assets::LoadTextureArtifact(texturePath);
        if (!artifact.has_value() ||
            !IsRgba8TextureArtifactMipUsable(*artifact))
        {
            return std::nullopt;
        }

        const auto* mip = SelectTexturePreviewMip(*artifact, requestedSize);
        if (mip == nullptr)
            return std::nullopt;

        data.pixels = mip->pixels;
        data.width = mip->width;
        data.height = mip->height;
        data.rowPitch = mip->rowPitch;
    }
    else
    {
        NLS::Image sourceImage(texturePath.string(), false);
        if (sourceImage.GetData() == nullptr ||
            sourceImage.GetWidth() <= 0 ||
            sourceImage.GetHeight() <= 0 ||
            sourceImage.GetChannels() <= 0)
        {
            return std::nullopt;
        }

        const auto sourcePixels = ConvertToRgba8(sourceImage);
        if (sourcePixels.empty())
            return std::nullopt;

        const auto downsampled = DownsampleRgba8ToThumbnail(
            sourcePixels.data(),
            static_cast<uint32_t>(sourceImage.GetWidth()),
            static_cast<uint32_t>(sourceImage.GetHeight()),
            static_cast<uint32_t>(sourceImage.GetWidth()) * 4u,
            std::max(1u, requestedSize));
        if (downsampled.pixels.empty() || downsampled.width == 0u || downsampled.height == 0u)
            return std::nullopt;

        data.pixels = downsampled.pixels;
        data.width = downsampled.width;
        data.height = downsampled.height;
        data.rowPitch = data.width * 4u;
    }

    if (data.pixels.empty() || data.width == 0u || data.height == 0u || data.rowPitch < data.width * 4u)
        return std::nullopt;
    return data;
}

std::array<float, 4u> SampleTextureNearest(
    const ThumbnailTextureSampleData& texture,
    float u,
    float v)
{
    if (texture.pixels.empty() || texture.width == 0u || texture.height == 0u || texture.rowPitch < texture.width * 4u)
        return {1.0f, 1.0f, 1.0f, 1.0f};

    u = u - std::floor(u);
    v = v - std::floor(v);
    if (texture.flipV)
        v = 1.0f - v;
    const auto x = std::min(
        texture.width - 1u,
        static_cast<uint32_t>(std::floor(u * static_cast<float>(texture.width))));
    const auto y = std::min(
        texture.height - 1u,
        static_cast<uint32_t>(std::floor((1.0f - v) * static_cast<float>(texture.height))));
    const auto* source = texture.pixels.data() + static_cast<size_t>(y) * texture.rowPitch + x * 4u;
    return {
        static_cast<float>(source[0]) / 255.0f,
        static_cast<float>(source[1]) / 255.0f,
        static_cast<float>(source[2]) / 255.0f,
        static_cast<float>(source[3]) / 255.0f
    };
}

struct MaterialPreviewStyle
{
    std::array<float, 4u> baseColor {0.58f, 0.66f, 0.76f, 1.0f};
    std::optional<ThumbnailTextureSampleData> albedoTexture;
};

MaterialPreviewStyle BuildMaterialPreviewStyle(
    const AssetThumbnailRequest& request,
    const std::string& materialPayload,
    const uint32_t requestedSize)
{
    MaterialPreviewStyle style;
    style.baseColor = ExtractMaterialBaseColor(materialPayload);
    if (const auto texturePath = ResolveTextureSamplePathFromMaterialPayload(request, materialPayload);
        texturePath.has_value())
    {
        style.albedoTexture = LoadTextureSampleData(*texturePath, requestedSize);
        if (style.albedoTexture.has_value() && !HasExtension(*texturePath, ".ntex"))
            style.albedoTexture->flipV = ShouldFlipMaterialSourceTextureVertically(request);
    }
    return style;
}

std::optional<size_t> MaterialPreviewIndexForSubAssetKey(const std::string& subAssetKey)
{
    constexpr std::string_view kPrefix = "material:";
    if (subAssetKey.rfind(kPrefix, 0u) != 0u)
        return std::nullopt;

    auto token = subAssetKey.substr(kPrefix.size());
    if (const auto separator = token.find_last_of("/\\:");
        separator != std::string::npos && separator + 1u < token.size())
    {
        token = token.substr(separator + 1u);
    }

    if (token.empty() || !std::all_of(token.begin(), token.end(), [](const unsigned char character)
        {
            return std::isdigit(character) != 0;
        }))
    {
        return std::nullopt;
    }

    try
    {
        return static_cast<size_t>(std::stoull(token));
    }
    catch (...)
    {
        return std::nullopt;
    }
}

struct MaterialPreviewArtifact
{
    std::filesystem::path path;
    std::string subAssetKey;
};

std::vector<MaterialPreviewArtifact> ResolveMaterialArtifactPaths(
    const AssetThumbnailRequest& request)
{
    std::vector<MaterialPreviewArtifact> paths;
    if (!request.subAssetKey.empty() &&
        request.subAssetKey.rfind("material:", 0u) == 0u)
    {
        if (auto resolved = ResolveArtifactPathForPreview(request, request.artifactPath);
            resolved.has_value())
        {
            paths.push_back({*resolved, request.subAssetKey});
            return paths;
        }
        if (auto sourceMaterial = ResolveSourceMaterialPathForPreview(request, request.artifactPath);
            sourceMaterial.has_value())
        {
            paths.push_back({*sourceMaterial, request.subAssetKey});
            return paths;
        }
        if (auto sourceMaterial = ResolveSourceMaterialPathForPreview(request);
            sourceMaterial.has_value())
        {
            paths.push_back({*sourceMaterial, request.subAssetKey});
            return paths;
        }
    }

    const auto manifest = LoadThumbnailArtifactManifest(request);
    if (!manifest.has_value())
    {
        if (auto sourceMaterial = ResolveSourceMaterialPathForPreview(request);
            sourceMaterial.has_value())
        {
            paths.push_back({*sourceMaterial, request.subAssetKey});
        }
        return paths;
    }

    for (const auto& artifact : manifest->subAssets)
    {
        if (artifact.artifactType != NLS::Core::Assets::ArtifactType::Material)
            continue;

        if (auto resolved = ResolveArtifactPathForPreview(request, artifact.artifactPath);
            resolved.has_value())
        {
            paths.push_back({*resolved, artifact.subAssetKey});
        }
        else if (auto sourceMaterial = ResolveSourceMaterialPathForPreview(request, artifact.artifactPath);
            sourceMaterial.has_value())
        {
            paths.push_back({*sourceMaterial, artifact.subAssetKey});
        }
    }
    if (paths.empty())
    {
        if (auto sourceMaterial = ResolveSourceMaterialPathForPreview(request);
            sourceMaterial.has_value())
        {
            paths.push_back({*sourceMaterial, request.subAssetKey});
        }
    }
    return paths;
}

std::vector<MaterialPreviewStyle> LoadMaterialPreviewStyles(const AssetThumbnailRequest& request)
{
    std::vector<MaterialPreviewStyle> styles;
    size_t sequentialIndex = 0u;
    for (const auto& materialArtifact : ResolveMaterialArtifactPaths(request))
    {
        const auto& materialPath = materialArtifact.path;
        if (StructurePreviewArtifactExceedsBudget(
                materialPath,
                NLS::Core::Assets::ArtifactType::Material,
                1u))
        {
            const auto materialIndex = MaterialPreviewIndexForSubAssetKey(materialArtifact.subAssetKey)
                .value_or(sequentialIndex);
            if (materialIndex >= styles.size())
                styles.resize(materialIndex + 1u);
            ++sequentialIndex;
            continue;
        }

        const auto payload = ReadNativeOrPlainTextArtifact(
            materialPath,
            NLS::Core::Assets::ArtifactType::Material,
            1u);
        const auto materialIndex = MaterialPreviewIndexForSubAssetKey(materialArtifact.subAssetKey)
            .value_or(sequentialIndex);
        if (materialIndex >= styles.size())
            styles.resize(materialIndex + 1u);
        styles[materialIndex] = payload.has_value()
            ? BuildMaterialPreviewStyle(request, *payload, request.requestedSize)
            : MaterialPreviewStyle {};
        ++sequentialIndex;
    }
    return styles;
}

DownsampledThumbnail RenderMaterialSphereThumbnail(
    const MaterialPreviewStyle& style,
    const uint32_t requestedSize)
{
    auto canvas = MakeCanvas(requestedSize);
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
            auto materialColor = style.baseColor;
            if (style.albedoTexture.has_value())
            {
                const float u = 0.5f + std::atan2(nx, nz) / (2.0f * 3.14159265358979323846f);
                const float v = 0.5f - std::asin(std::clamp(ny, -1.0f, 1.0f)) / 3.14159265358979323846f;
                const auto texel = SampleTextureNearest(*style.albedoTexture, u, v);
                materialColor[0] *= texel[0];
                materialColor[1] *= texel[1];
                materialColor[2] *= texel[2];
                materialColor[3] *= texel[3];
            }

            const float diffuse = std::max(0.0f, nx * lightX + ny * lightY + nz * lightZ);
            const float rim = std::pow(std::max(0.0f, 1.0f - nz), 2.0f) * 0.18f;
            const float shade = std::clamp(0.22f + diffuse * 0.78f + rim, 0.0f, 1.0f);
            PutPixel(
                canvas,
                static_cast<int>(x),
                static_cast<int>(y),
                static_cast<uint8_t>(std::clamp(materialColor[0] * shade * 255.0f, 0.0f, 255.0f)),
                static_cast<uint8_t>(std::clamp(materialColor[1] * shade * 255.0f, 0.0f, 255.0f)),
                static_cast<uint8_t>(std::clamp(materialColor[2] * shade * 255.0f, 0.0f, 255.0f)),
                static_cast<uint8_t>(std::clamp(materialColor[3] * 255.0f, 0.0f, 255.0f)));
        }
    }
    return CanvasToThumbnail(std::move(canvas));
}

struct MeshPreviewTriangle
{
    struct Vertex
    {
        std::array<float, 3u> screen {};
        std::array<float, 3u> normal {};
        std::array<float, 2u> uv {};
    };
    std::array<Vertex, 3u> vertices {};
    size_t materialIndex = 0u;
};

float Dot3(const std::array<float, 3u>& left, const std::array<float, 3u>& right)
{
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

std::array<float, 3u> Normalize3(std::array<float, 3u> value)
{
    const auto length = std::sqrt(std::max(0.000001f, Dot3(value, value)));
    value[0] /= length;
    value[1] /= length;
    value[2] /= length;
    return value;
}

std::array<float, 3u> RotateUnityPreviewVector(std::array<float, 3u> value)
{
    const auto yaw = kUnityMeshPreviewYawDegrees * kDegreesToRadians;
    const auto pitch = kUnityMeshPreviewPitchDegrees * kDegreesToRadians;

    const auto cy = std::cos(yaw);
    const auto sy = std::sin(yaw);
    std::array<float, 3u> rotated {
        value[0] * cy + value[2] * sy,
        value[1],
        -value[0] * sy + value[2] * cy
    };

    const auto cp = std::cos(pitch);
    const auto sp = std::sin(pitch);
    return {
        rotated[0],
        rotated[1] * cp - rotated[2] * sp,
        rotated[1] * sp + rotated[2] * cp
    };
}

std::array<float, 3u> TransformUnityPreviewPoint(
    const NLS::Render::Geometry::Vertex& vertex,
    const std::array<float, 3u>& center,
    const float cameraDistance)
{
    auto rotated = RotateUnityPreviewVector({
        vertex.position[0] - center[0],
        vertex.position[1] - center[1],
        vertex.position[2] - center[2]
    });
    rotated[2] += cameraDistance;
    return rotated;
}

std::array<float, 4u> ShadeUnityPreviewMaterial(
    const MaterialPreviewStyle& material,
    const std::array<float, 3u>& normal,
    const std::array<float, 2u>& uv)
{
    std::array<float, 4u> color = material.baseColor;
    if (material.albedoTexture.has_value())
    {
        const auto texel = SampleTextureNearest(*material.albedoTexture, uv[0], uv[1]);
        color[0] *= texel[0];
        color[1] *= texel[1];
        color[2] *= texel[2];
        color[3] *= texel[3];
    }

    const auto n = Normalize3(normal);
    const auto light0 = Normalize3({0.58f, 0.64f, 0.50f});
    const auto light1 = Normalize3({-0.35f, 0.25f, 0.90f});
    const auto diffuse =
        std::max(0.0f, Dot3(n, light0)) * 1.15f +
        std::max(0.0f, Dot3(n, light1)) * 0.45f;
    const auto shade = std::clamp(0.18f + diffuse, 0.0f, 1.35f);
    color[0] = std::clamp(color[0] * shade, 0.0f, 1.0f);
    color[1] = std::clamp(color[1] * shade, 0.0f, 1.0f);
    color[2] = std::clamp(color[2] * shade, 0.0f, 1.0f);
    color[3] = std::clamp(color[3], 0.0f, 1.0f);
    return color;
}

DownsampledThumbnail RenderMeshSetThumbnail(
    const std::vector<NLS::Render::Assets::MeshArtifactData>& meshes,
    const std::vector<MaterialPreviewStyle>& materials,
    const uint32_t requestedSize)
{
    auto canvas = MakeCanvas(requestedSize);
    if (meshes.empty())
        return CanvasToThumbnail(std::move(canvas));

    std::array<float, 3u> minBounds {
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()
    };
    std::array<float, 3u> maxBounds {
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()
    };
    size_t vertexCount = 0u;
    for (const auto& mesh : meshes)
    {
        for (const auto& vertex : mesh.vertices)
        {
            ++vertexCount;
            for (size_t axis = 0u; axis < 3u; ++axis)
            {
                minBounds[axis] = std::min(minBounds[axis], vertex.position[axis]);
                maxBounds[axis] = std::max(maxBounds[axis], vertex.position[axis]);
            }
        }
    }
    if (vertexCount == 0u)
        return CanvasToThumbnail(std::move(canvas));

    const std::array<float, 3u> center {
        (minBounds[0] + maxBounds[0]) * 0.5f,
        (minBounds[1] + maxBounds[1]) * 0.5f,
        (minBounds[2] + maxBounds[2]) * 0.5f
    };
    const auto extentX = maxBounds[0] - minBounds[0];
    const auto extentY = maxBounds[1] - minBounds[1];
    const auto extentZ = maxBounds[2] - minBounds[2];
    const auto halfSize = std::max(0.0001f, 0.5f * std::sqrt(extentX * extentX + extentY * extentY + extentZ * extentZ));
    const auto cameraDistance = halfSize * 4.0f;
    const auto focalLength = (static_cast<float>(canvas.height) * 0.5f) /
        std::tan((kUnityMeshPreviewFieldOfViewDegrees * 0.5f) * kDegreesToRadians);
    const auto project = [&](const NLS::Render::Geometry::Vertex& vertex) -> MeshPreviewTriangle::Vertex
    {
        const auto view = TransformUnityPreviewPoint(vertex, center, cameraDistance);
        const auto depth = std::max(0.0001f, view[2]);
        return {
            {
                view[0] * focalLength / depth + static_cast<float>(canvas.width) * 0.5f,
                static_cast<float>(canvas.height) * 0.5f - view[1] * focalLength / depth,
                depth
            },
            Normalize3(RotateUnityPreviewVector({
                vertex.normals[0],
                vertex.normals[1],
                vertex.normals[2]
            })),
            {vertex.texCoords[0], vertex.texCoords[1]}
        };
    };

    size_t totalTriangleCount = 0u;
    for (const auto& mesh : meshes)
        totalTriangleCount += mesh.indices.size() / 3u;
    const auto triangleStride = totalTriangleCount > kMaxMeshPreviewRenderedTriangles
        ? (totalTriangleCount + kMaxMeshPreviewRenderedTriangles - 1u) / kMaxMeshPreviewRenderedTriangles
        : 1u;

    std::vector<float> depthBuffer(
        static_cast<size_t>(canvas.width) * canvas.height,
        std::numeric_limits<float>::max());
    std::vector<MeshPreviewTriangle> triangles;
    triangles.reserve(std::min(totalTriangleCount, kMaxMeshPreviewRenderedTriangles));
    size_t globalTriangleIndex = 0u;
    for (const auto& mesh : meshes)
    {
        for (size_t index = 0u; index + 2u < mesh.indices.size(); index += 3u)
        {
            if ((globalTriangleIndex++ % triangleStride) != 0u)
                continue;

            const auto i0 = mesh.indices[index + 0u];
            const auto i1 = mesh.indices[index + 1u];
            const auto i2 = mesh.indices[index + 2u];
            if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size())
                continue;

            const auto& v0 = mesh.vertices[i0];
            const auto& v1 = mesh.vertices[i1];
            const auto& v2 = mesh.vertices[i2];
            triangles.push_back({{project(v0), project(v1), project(v2)}, mesh.materialIndex});
        }
    }

    for (const auto& triangle : triangles)
    {
        const auto& a = triangle.vertices[0];
        const auto& b = triangle.vertices[1];
        const auto& c = triangle.vertices[2];
        const int minX = std::max(0, static_cast<int>(std::floor(std::min({a.screen[0], b.screen[0], c.screen[0]}))));
        const int maxX = std::min(static_cast<int>(canvas.width) - 1, static_cast<int>(std::ceil(std::max({a.screen[0], b.screen[0], c.screen[0]}))));
        const int minY = std::max(0, static_cast<int>(std::floor(std::min({a.screen[1], b.screen[1], c.screen[1]}))));
        const int maxY = std::min(static_cast<int>(canvas.height) - 1, static_cast<int>(std::ceil(std::max({a.screen[1], b.screen[1], c.screen[1]}))));
        if (minX > maxX || minY > maxY)
            continue;

        const auto edge = [](const MeshPreviewTriangle::Vertex& left, const MeshPreviewTriangle::Vertex& right, const float x, const float y)
        {
            return (x - left.screen[0]) * (right.screen[1] - left.screen[1]) -
                (y - left.screen[1]) * (right.screen[0] - left.screen[0]);
        };
        const auto area = edge(a, b, c.screen[0], c.screen[1]);
        if (std::abs(area) < 0.0001f)
            continue;

        const auto material = triangle.materialIndex < materials.size()
            ? materials[triangle.materialIndex]
            : MaterialPreviewStyle {};
        for (int y = minY; y <= maxY; ++y)
        {
            for (int x = minX; x <= maxX; ++x)
            {
                const auto px = static_cast<float>(x) + 0.5f;
                const auto py = static_cast<float>(y) + 0.5f;
                const auto w0 = edge(b, c, px, py);
                const auto w1 = edge(c, a, px, py);
                const auto w2 = edge(a, b, px, py);
                const bool insidePositive = w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f;
                const bool insideNegative = w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f;
                if (!insidePositive && !insideNegative)
                    continue;

                const auto invArea = 1.0f / area;
                const auto b0 = w0 * invArea;
                const auto b1 = w1 * invArea;
                const auto b2 = w2 * invArea;
                const auto depth = a.screen[2] * b0 + b.screen[2] * b1 + c.screen[2] * b2;
                const auto depthIndex = static_cast<size_t>(y) * canvas.width + static_cast<size_t>(x);
                if (depth >= depthBuffer[depthIndex])
                    continue;
                depthBuffer[depthIndex] = depth;

                const std::array<float, 3u> normal {
                    a.normal[0] * b0 + b.normal[0] * b1 + c.normal[0] * b2,
                    a.normal[1] * b0 + b.normal[1] * b1 + c.normal[1] * b2,
                    a.normal[2] * b0 + b.normal[2] * b1 + c.normal[2] * b2
                };
                const std::array<float, 2u> uv {
                    a.uv[0] * b0 + b.uv[0] * b1 + c.uv[0] * b2,
                    a.uv[1] * b0 + b.uv[1] * b1 + c.uv[1] * b2
                };
                const auto shaded = ShadeUnityPreviewMaterial(material, normal, uv);
                PutPixel(
                    canvas,
                    x,
                    y,
                    static_cast<uint8_t>(std::clamp(shaded[0] * 255.0f, 0.0f, 255.0f)),
                    static_cast<uint8_t>(std::clamp(shaded[1] * 255.0f, 0.0f, 255.0f)),
                    static_cast<uint8_t>(std::clamp(shaded[2] * 255.0f, 0.0f, 255.0f)),
                    static_cast<uint8_t>(std::clamp(shaded[3] * 255.0f, 0.0f, 255.0f)));
            }
        }
    }

    if (triangles.empty())
    {
        for (const auto& mesh : meshes)
        {
            for (const auto& vertex : mesh.vertices)
            {
                const auto p = project(vertex);
                PutPixel(
                    canvas,
                    static_cast<int>(std::lround(p.screen[0])),
                    static_cast<int>(std::lround(p.screen[1])),
                    150u,
                    210u,
                    255u);
            }
        }
    }

    return CanvasToThumbnail(std::move(canvas));
}

DownsampledThumbnail RenderMeshThumbnail(
    const NLS::Render::Assets::MeshArtifactData& mesh,
    const uint32_t requestedSize)
{
    return RenderMeshSetThumbnail({mesh}, {}, requestedSize);
}

DownsampledThumbnail RenderMeshSetThumbnail(
    const std::vector<NLS::Render::Assets::MeshArtifactData>& meshes,
    const uint32_t requestedSize)
{
    return RenderMeshSetThumbnail(meshes, {}, requestedSize);
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

DownsampledThumbnail RenderMaterialPreviewThumbnail(
    const AssetThumbnailRequest& request,
    const std::string& materialPayload,
    const uint32_t requestedSize)
{
    return RenderMaterialSphereThumbnail(
        BuildMaterialPreviewStyle(request, materialPayload, requestedSize),
        requestedSize);
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
        result.diagnostic = IsMissingThumbnailArtifactPath(request)
            ? "thumbnail-material-artifact-missing"
            : "thumbnail-material-artifact-path-invalid";
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
        RenderMaterialPreviewThumbnail(request, *payload, request.requestedSize),
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
    const auto mesh = NLS::Render::Assets::LoadMeshArtifact(*meshPath);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);
    if (!mesh.has_value())
    {
        result.diagnostic = "thumbnail-model-mesh-artifact-read-failed";
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

AssetThumbnailServiceResult GenerateMeshSetThumbnail(
    const AssetThumbnailRequest& request,
    const std::vector<std::filesystem::path>& meshPaths,
    const std::string& missingDiagnostic,
    const AssetThumbnailCancelToken& cancelToken)
{
    const auto evaluation = EvaluateAssetThumbnailCache(request);
    auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
    if (!IsThumbnailRequestStillFresh(request))
        return BuildStaleThumbnailRequestResult(request, evaluation);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);
    if (meshPaths.empty())
    {
        result.diagnostic = missingDiagnostic;
        WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

    struct MeshPreviewArtifactCandidate
    {
        std::filesystem::path path;
        NLS::Render::Assets::MeshArtifactHeaderPreview header;
        size_t score = 0u;
    };

    std::vector<MeshPreviewArtifactCandidate> candidates;
    candidates.reserve(meshPaths.size());
    for (const auto& meshPath : meshPaths)
    {
        const auto meshHeader = NLS::Render::Assets::ReadMeshArtifactHeaderPreview(
            meshPath,
            kMaxStructurePreviewArtifactPayloadBytes);
        if (!meshHeader.has_value() ||
            NativeArtifactFileExceedsThumbnailPreviewBudget(meshPath))
        {
            result.diagnostic = "thumbnail-model-mesh-artifact-read-failed";
            WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
            return result;
        }

        candidates.push_back({
            meshPath,
            *meshHeader,
            static_cast<size_t>(meshHeader->vertexCount) + static_cast<size_t>(meshHeader->indexCount)
        });
    }

    std::stable_sort(
        candidates.begin(),
        candidates.end(),
        [](const MeshPreviewArtifactCandidate& left, const MeshPreviewArtifactCandidate& right)
        {
            return left.score > right.score;
        });

    std::vector<NLS::Render::Assets::MeshArtifactData> meshes;
    meshes.reserve(candidates.size());
    size_t loadedVertices = 0u;
    size_t loadedIndices = 0u;
    for (const auto& candidate : candidates)
    {
        const bool wouldExceedBudget =
            !meshes.empty() &&
            (loadedVertices + candidate.header.vertexCount > kMaxMeshPreviewLoadedVertices ||
                loadedIndices + candidate.header.indexCount > kMaxMeshPreviewLoadedIndices);
        if (wouldExceedBudget)
            continue;

        const auto mesh = NLS::Render::Assets::LoadMeshArtifact(candidate.path);
        if (IsThumbnailGenerationCancelled(cancelToken))
            return BuildCancelledThumbnailRequestResult(request, evaluation);
        if (!mesh.has_value())
        {
            result.diagnostic = "thumbnail-model-mesh-artifact-read-failed";
            WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
            return result;
        }
        loadedVertices += mesh->vertices.size();
        loadedIndices += mesh->indices.size();
        meshes.push_back(*mesh);
    }

    if (meshes.empty())
    {
        result.diagnostic = "thumbnail-model-mesh-artifact-read-failed";
        WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

    return WriteThumbnailPngResult(
        request,
        evaluation,
        RenderMeshSetThumbnail(
            meshes,
            request.kind == AssetThumbnailKind::PrefabPreview
                ? LoadMaterialPreviewStyles(request)
                : std::vector<MaterialPreviewStyle> {},
            request.requestedSize),
        "thumbnail-model-preview-generation-failed",
        cancelToken);
}

AssetThumbnailServiceResult GenerateModelThumbnail(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCancelToken& cancelToken)
{
    const auto meshPaths = ResolveMeshArtifactPaths(request);
    if (meshPaths.empty())
        return GenerateMeshBackedThumbnail(request, "thumbnail-model-mesh-artifact-missing", cancelToken);
    if (meshPaths.size() > 1u || !HasExtension(meshPaths.front(), ".nmesh"))
    {
        return GenerateMeshSetThumbnail(request, meshPaths, "thumbnail-model-mesh-artifact-missing", cancelToken);
    }
    return GenerateMeshSetThumbnail(request, meshPaths, "thumbnail-model-mesh-artifact-missing", cancelToken);
}

AssetThumbnailServiceResult GeneratePrefabThumbnail(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCancelToken& cancelToken)
{
    if (const auto meshPaths = ResolveMeshArtifactPaths(request);
        !meshPaths.empty())
    {
        return GenerateMeshSetThumbnail(request, meshPaths, "thumbnail-prefab-mesh-artifact-missing", cancelToken);
    }

    const auto evaluation = EvaluateAssetThumbnailCache(request);
    auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
    if (!IsThumbnailRequestStillFresh(request))
        return BuildStaleThumbnailRequestResult(request, evaluation);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);

    const auto previewPath = ResolvePreviewArtifactOrSourcePath(request);
    if (!previewPath.has_value())
    {
        result.diagnostic = IsMissingThumbnailArtifactPath(request)
            ? "thumbnail-prefab-artifact-missing"
            : "thumbnail-prefab-artifact-path-invalid";
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
    const auto generationSize = GetTextureThumbnailGenerationSize(request);
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
            !IsRgba8TextureArtifactMipUsable(*textureArtifact))
        {
            auto sourcePath = ResolveThumbnailSourcePath(request);
            if (sourcePath.empty() || !IsTextureThumbnailSourceExtension(sourcePath))
            {
                if (auto dependencySourcePath = ResolveTextureSourceDependencyPath(request);
                    dependencySourcePath.has_value())
                {
                    sourcePath = *dependencySourcePath;
                }
            }
            if (!sourcePath.empty() && IsTextureThumbnailSourceExtension(sourcePath))
            {
                const auto dimensions = ReadImageHeaderDimensions(sourcePath);
                if (FileSizeOrMax(sourcePath) <= kMaxSourceThumbnailImageBytes &&
                    !(dimensions.has_value() && ImageDimensionsExceedPreviewBudget(*dimensions)) &&
                    !(!dimensions.has_value() && IsKnownSourceImageExtension(sourcePath)))
                {
                    NLS::Image sourceImage(sourcePath.string(), false);
                    if (sourceImage.GetData() != nullptr &&
                        sourceImage.GetWidth() > 0 &&
                        sourceImage.GetHeight() > 0 &&
                        sourceImage.GetChannels() > 0)
                    {
                        return WriteThumbnailPngResult(
                            request,
                            evaluation,
                            DownsampleImageToThumbnail(sourceImage, generationSize),
                            "thumbnail-source-downsample-failed",
                            cancelToken);
                    }
                }
            }

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

        const auto* mip = SelectTextureThumbnailMip(*textureArtifact, generationSize);
        if (mip == nullptr)
        {
            result.diagnostic = "thumbnail-texture-artifact-unsupported";
            result.status = AssetThumbnailServiceStatus::Fallback;
            WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
            return result;
        }
        return WriteThumbnailPngResult(
            request,
            evaluation,
            DownsampleRgba8ToThumbnail(
                mip->pixels.data(),
                mip->width,
                mip->height,
                mip->rowPitch,
                generationSize),
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

    const auto thumbnail = DownsampleImageToThumbnail(sourceImage, generationSize);
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

    if (SupportsGpuThumbnailPreview(request.kind))
    {
        auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Pending);
        result.diagnostic = UnsupportedDiagnosticForKind(request.kind);
        return result;
    }

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

std::optional<AssetThumbnailRequest> BuildAssetThumbnailRequestForItemWithContext(
    const std::filesystem::path& projectRoot,
    const AssetBrowserItem& item,
    uint32_t requestedSize,
    AssetThumbnailRequestBuildContext* context);
}

std::optional<AssetThumbnailRequest> BuildAssetThumbnailRequestForItem(
    const std::filesystem::path& projectRoot,
    const AssetBrowserItem& item,
    const uint32_t requestedSize)
{
    return BuildAssetThumbnailRequestForItemWithContext(projectRoot, item, requestedSize, nullptr);
}

std::optional<AssetThumbnailRequest> BuildAssetThumbnailRequestForItem(
    const std::filesystem::path& projectRoot,
    const AssetBrowserItem& item,
    const uint32_t requestedSize,
    AssetThumbnailRequestBuildContext& context)
{
    return BuildAssetThumbnailRequestForItemWithContext(projectRoot, item, requestedSize, &context);
}

namespace
{
std::optional<AssetThumbnailRequest> BuildAssetThumbnailRequestForItemWithContext(
    const std::filesystem::path& projectRoot,
    const AssetBrowserItem& item,
    const uint32_t requestedSize,
    AssetThumbnailRequestBuildContext* context)
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
    request.artifactPath = item.artifactPath;
    auto cachedManifest = LoadThumbnailArtifactManifestCached(request, context);
    const auto localManifest = cachedManifest == nullptr
        ? LoadThumbnailArtifactManifest(request)
        : std::optional<NLS::Core::Assets::ArtifactManifest> {};
    const auto& manifest = cachedManifest != nullptr ? *cachedManifest : localManifest;
    if (manifest.has_value())
    {
        if (const auto* artifact = FindThumbnailArtifactForItem(*manifest, item))
        {
            if (request.subAssetKey.empty())
                request.subAssetKey = artifact->subAssetKey;
            if (request.artifactPath.empty())
                request.artifactPath = artifact->artifactPath;
        }
    }
    if (request.subAssetKey.empty() && item.type == AssetBrowserItemType::Prefab)
        request.subAssetKey = "prefab:" + std::filesystem::path(item.sourceAssetPath).stem().generic_string();
    if (item.kind == AssetBrowserItemKind::GeneratedSubAsset ||
        item.type == AssetBrowserItemType::Model ||
        item.type == AssetBrowserItemType::Prefab)
    {
        request.artifactPath = request.artifactPath.empty() ? item.artifactPath : request.artifactPath;
    }
    request.kind = ThumbnailKindForItem(item);
    request.requestedSize = request.kind == AssetThumbnailKind::Texture
        ? (std::min)(std::max(1u, requestedSize), kMaxTextureThumbnailGenerationSize)
        : std::max(1u, requestedSize);
    request.settingsFingerprint = request.kind == AssetThumbnailKind::Texture
        ? "asset-browser-thumbnail:v15-lowres-image-thumbnails"
        : "asset-browser-thumbnail:v19-gpu-preview-textured-materials";
    request.freshnessInputs.push_back({
        "item",
        ItemFreshnessIdentity(item, assetId)
    });
    AddSourceFreshnessInputs(request);
    AddArtifactFreshnessInputs(request, item);
    return request;
}
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

    if (!CanRequestThumbnailGeneration(request.kind))
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

    std::vector<std::string> deferredCacheKeys;
    auto restoreDeferredCacheKeys = [&deferredCacheKeys, this]()
    {
        for (const auto& deferred : deferredCacheKeys)
            m_queuedCacheKeys.push(deferred);
        deferredCacheKeys.clear();
    };

    while (!m_queuedCacheKeys.empty())
    {
        const auto cacheKey = m_queuedCacheKeys.front();
        m_queuedCacheKeys.pop();

        const auto requestIterator = m_queuedRequestsByCacheKey.find(cacheKey);
        if (requestIterator == m_queuedRequestsByCacheKey.end())
            continue;

        const auto request = requestIterator->second;
        if (SupportsGpuThumbnailPreview(request.kind))
        {
            m_queuedRequestsByCacheKey.erase(requestIterator);
            const auto evaluation = EvaluateAssetThumbnailCache(request);
            if (const auto invalidPathDiagnostic = ValidateGpuPreviewRequestArtifactPaths(request);
                invalidPathDiagnostic.has_value())
            {
                auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
                result.diagnostic = *invalidPathDiagnostic;
                WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
                restoreDeferredCacheKeys();
                return result;
            }
            auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Fallback);
            result.diagnostic = "thumbnail-gpu-preview-renderer-unavailable";
            restoreDeferredCacheKeys();
            return result;
        }

        m_queuedRequestsByCacheKey.erase(requestIterator);

        restoreDeferredCacheKeys();
        return TryGenerateThumbnailForRequest(request, m_generationCancelToken);
    }

    restoreDeferredCacheKeys();
    return std::nullopt;
}

std::optional<AssetThumbnailServiceResult> AssetThumbnailService::GenerateNextThumbnail(
    EditorThumbnailPreviewRenderer& previewRenderer)
{
    if (!m_generationCancelToken)
        m_generationCancelToken = std::make_shared<AssetThumbnailGenerationCancelToken>();
    m_generationCancelToken->generation = m_generationSerial;

    std::vector<std::string> deferredCacheKeys;
    auto restoreDeferredCacheKeys = [&deferredCacheKeys, this]()
    {
        for (const auto& deferred : deferredCacheKeys)
            m_queuedCacheKeys.push(deferred);
        deferredCacheKeys.clear();
    };
    while (!m_queuedCacheKeys.empty())
    {
        const auto cacheKey = m_queuedCacheKeys.front();
        m_queuedCacheKeys.pop();

        const auto requestIterator = m_queuedRequestsByCacheKey.find(cacheKey);
        if (requestIterator == m_queuedRequestsByCacheKey.end())
            continue;

        const auto request = requestIterator->second;
        if (!previewRenderer.Supports(request))
        {
            deferredCacheKeys.push_back(cacheKey);
            continue;
        }

        m_queuedRequestsByCacheKey.erase(requestIterator);

        const auto evaluation = EvaluateAssetThumbnailCache(request);
        if (const auto invalidPathDiagnostic = ValidateGpuPreviewRequestArtifactPaths(request);
            invalidPathDiagnostic.has_value())
        {
            auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
            result.diagnostic = *invalidPathDiagnostic;
            WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
            restoreDeferredCacheKeys();
            return result;
        }
        if (!evaluation.entry.has_value())
        {
            restoreDeferredCacheKeys();
            return BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
        }
        if (!IsThumbnailRequestStillFresh(request))
        {
            restoreDeferredCacheKeys();
            return BuildStaleThumbnailRequestResult(request, evaluation);
        }
        if (IsThumbnailGenerationCancelled(m_generationCancelToken))
        {
            restoreDeferredCacheKeys();
            return BuildCancelledThumbnailRequestResult(request, evaluation);
        }

        const auto preview = previewRenderer.Render(request);
        if (preview.rgbaPixels.empty() || preview.width == 0u || preview.height == 0u)
        {
            const auto diagnostic = preview.diagnostic.empty()
                ? std::string("thumbnail-gpu-preview-generation-failed")
                : preview.diagnostic;
            const bool retryableGpuFailure = IsRetryableThumbnailFailureDiagnostic(diagnostic);
            auto result = BuildResultFromEvaluation(
                request,
                evaluation,
                retryableGpuFailure
                    ? AssetThumbnailServiceStatus::Fallback
                    : AssetThumbnailServiceStatus::Failed);
            result.diagnostic = diagnostic;
            if (!retryableGpuFailure)
                WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
            restoreDeferredCacheKeys();
            return result;
        }
        if (IsGpuPreviewClearFrame(preview.rgbaPixels, preview.width, preview.height))
        {
            auto result = BuildResultFromEvaluation(
                request,
                evaluation,
                AssetThumbnailServiceStatus::Failed);
            result.diagnostic = "thumbnail-gpu-preview-empty-frame";
            WriteAssetThumbnailCacheMetadata(request, AssetThumbnailCacheStatus::Failed, result.diagnostic);
            restoreDeferredCacheKeys();
            return result;
        }

        restoreDeferredCacheKeys();

        return WriteRgbaThumbnailResult(
            request,
            evaluation,
            preview.rgbaPixels.data(),
            preview.width,
            preview.height,
            "thumbnail-gpu-preview-generation-failed",
            m_generationCancelToken);
    }

    restoreDeferredCacheKeys();
    return std::nullopt;
}

bool AssetThumbnailService::StartNextThumbnailGeneration()
{
    return StartNextThumbnailGeneration(nullptr);
}

bool AssetThumbnailService::StartNextThumbnailGeneration(EditorThumbnailPreviewRenderer& previewRenderer)
{
    return StartNextThumbnailGeneration(&previewRenderer);
}

bool AssetThumbnailService::StartNextThumbnailGeneration(EditorThumbnailPreviewRenderer* previewRenderer)
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

    std::vector<std::string> deferredCacheKeys;
    auto restoreDeferredCacheKeys = [&deferredCacheKeys, this]()
    {
        for (const auto& deferred : deferredCacheKeys)
            m_queuedCacheKeys.push(deferred);
        deferredCacheKeys.clear();
    };

    while (!m_queuedCacheKeys.empty())
    {
        const auto cacheKey = m_queuedCacheKeys.front();
        m_queuedCacheKeys.pop();

        const auto requestIterator = m_queuedRequestsByCacheKey.find(cacheKey);
        if (requestIterator == m_queuedRequestsByCacheKey.end())
            continue;

        const auto request = requestIterator->second;
        if (SupportsGpuThumbnailPreview(request.kind))
        {
            deferredCacheKeys.push_back(cacheKey);
            continue;
        }
        if (previewRenderer != nullptr && previewRenderer->Supports(request))
        {
            deferredCacheKeys.push_back(cacheKey);
            continue;
        }
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
            restoreDeferredCacheKeys();
            return false;
        }
        restoreDeferredCacheKeys();
        return true;
    }

    restoreDeferredCacheKeys();
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
            result.fallbackIcon = "editor.icon.asset.default";
            result.diagnostic = "thumbnail-generation-out-of-memory";
        }
        catch (...)
        {
            result.status = AssetThumbnailServiceStatus::Failed;
            result.fallbackIcon = "editor.icon.asset.default";
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
