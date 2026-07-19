#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/ArtifactFileReader.h"
#include "Rendering/Geometry/BoundingSphereUtils.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/NativeArtifactContainer.h"

#include <meshoptimizer.h>

#include <cstring>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <numeric>
#include <string>
#include <unordered_map>

namespace NLS::Render::Assets
{
namespace
{
constexpr uint32_t kMeshArtifactMagic = 0x484D4E4Eu; // "NNMH" little-endian.
constexpr uint32_t kMeshArtifactVersion = 1u;
constexpr uint32_t kMeshArtifactVersionWithBoundingSphere = 2u;
constexpr uint32_t kMeshArtifactContainerSchemaVersion = 3u;

struct MeshArtifactHeader
{
    uint32_t magic = kMeshArtifactMagic;
    uint32_t version = kMeshArtifactVersionWithBoundingSphere;
    uint32_t vertexStride = sizeof(Geometry::Vertex);
    uint32_t vertexCount = 0u;
    uint32_t indexCount = 0u;
    uint32_t materialIndex = 0u;
};

static_assert(sizeof(MeshArtifactHeader) == 24u);

constexpr uint32_t kNativeArtifactMagic = 0x41534C4Eu;
constexpr uint32_t kNativeArtifactContainerVersion = 1u;
constexpr uint32_t kNativeArtifactHeaderSize = 64u;
constexpr uint32_t kNativeArtifactFlagsLittleEndian = 1u;
constexpr uint32_t kPreviewSimplificationMinimumIndexCount = 3u;
constexpr float kPreviewSimplificationMaximumRelativeError = 1.0f;
constexpr size_t kPreviewSimplificationMaximumAttempts = 8u;

struct NativeArtifactHeaderPreview
{
    uint32_t magic = 0u;
    uint32_t containerVersion = 0u;
    uint32_t headerSize = 0u;
    uint32_t flags = 0u;
    uint32_t artifactType = 0u;
    uint32_t schemaVersion = 0u;
    uint64_t metadataSize = 0u;
    uint64_t payloadSize = 0u;
    uint64_t payloadOffset = 0u;
    uint64_t reserved0 = 0u;
    uint64_t reserved1 = 0u;
};

struct ByteView
{
    const uint8_t* data = nullptr;
    size_t size = 0u;
};

void AppendUInt32(std::vector<uint8_t>& bytes, const uint32_t value)
{
    bytes.push_back(static_cast<uint8_t>(value & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((value >> 16u) & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((value >> 24u) & 0xFFu));
}

template<typename T>
void AppendArray(std::vector<uint8_t>& bytes, const std::vector<T>& values)
{
    if (values.empty())
        return;

    const auto* begin = reinterpret_cast<const uint8_t*>(values.data());
    bytes.insert(bytes.end(), begin, begin + values.size() * sizeof(T));
}

void AppendFloat32(std::vector<uint8_t>& bytes, const float value)
{
    uint32_t bits = 0u;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    AppendUInt32(bytes, bits);
}

void AppendBoundingSphere(std::vector<uint8_t>& bytes, const Geometry::BoundingSphere& boundingSphere)
{
    AppendFloat32(bytes, boundingSphere.position.x);
    AppendFloat32(bytes, boundingSphere.position.y);
    AppendFloat32(bytes, boundingSphere.position.z);
    AppendFloat32(bytes, boundingSphere.radius);
}

void AppendHeader(std::vector<uint8_t>& bytes, const MeshArtifactHeader& header)
{
    AppendUInt32(bytes, header.magic);
    AppendUInt32(bytes, header.version);
    AppendUInt32(bytes, header.vertexStride);
    AppendUInt32(bytes, header.vertexCount);
    AppendUInt32(bytes, header.indexCount);
    AppendUInt32(bytes, header.materialIndex);
}

bool ReadUInt32(const ByteView bytes, size_t& offset, uint32_t& value)
{
    if (offset + sizeof(uint32_t) > bytes.size)
        return false;

    value =
        static_cast<uint32_t>(bytes.data[offset]) |
        (static_cast<uint32_t>(bytes.data[offset + 1u]) << 8u) |
        (static_cast<uint32_t>(bytes.data[offset + 2u]) << 16u) |
        (static_cast<uint32_t>(bytes.data[offset + 3u]) << 24u);
    offset += sizeof(uint32_t);
    return true;
}

bool ReadHeader(const ByteView bytes, MeshArtifactHeader& header)
{
    if (bytes.size < sizeof(MeshArtifactHeader))
        return false;

    size_t offset = 0u;
    if (!ReadUInt32(bytes, offset, header.magic) ||
        !ReadUInt32(bytes, offset, header.version) ||
        !ReadUInt32(bytes, offset, header.vertexStride) ||
        !ReadUInt32(bytes, offset, header.vertexCount) ||
        !ReadUInt32(bytes, offset, header.indexCount) ||
        !ReadUInt32(bytes, offset, header.materialIndex))
    {
        return false;
    }

    return header.magic == kMeshArtifactMagic &&
        (header.version == kMeshArtifactVersion ||
            header.version == kMeshArtifactVersionWithBoundingSphere) &&
        header.vertexStride == sizeof(Geometry::Vertex);
}

bool ReadNativeArtifactHeaderPreview(
    const std::array<uint8_t, kNativeArtifactHeaderSize>& bytes,
    NativeArtifactHeaderPreview& header)
{
    const ByteView view {bytes.data(), bytes.size()};
    size_t offset = 0u;
    if (!ReadUInt32(view, offset, header.magic) ||
        !ReadUInt32(view, offset, header.containerVersion) ||
        !ReadUInt32(view, offset, header.headerSize) ||
        !ReadUInt32(view, offset, header.flags) ||
        !ReadUInt32(view, offset, header.artifactType) ||
        !ReadUInt32(view, offset, header.schemaVersion))
    {
        return false;
    }

    auto read64 = [&view, &offset](uint64_t& value)
    {
        if (offset + sizeof(uint64_t) > view.size)
            return false;
        value = 0u;
        for (uint32_t byteIndex = 0u; byteIndex < 8u; ++byteIndex)
            value |= static_cast<uint64_t>(view.data[offset + byteIndex]) << (byteIndex * 8u);
        offset += sizeof(uint64_t);
        return true;
    };

    return read64(header.metadataSize) &&
        read64(header.payloadSize) &&
        read64(header.payloadOffset) &&
        read64(header.reserved0) &&
        read64(header.reserved1) &&
        header.magic == kNativeArtifactMagic &&
        header.containerVersion == kNativeArtifactContainerVersion &&
        header.headerSize == kNativeArtifactHeaderSize &&
        header.flags == kNativeArtifactFlagsLittleEndian &&
        header.artifactType == static_cast<uint32_t>(NLS::Core::Assets::ArtifactType::Mesh) &&
        header.schemaVersion == kMeshArtifactContainerSchemaVersion;
}

bool ReadFloat(const ByteView bytes, size_t& offset, float& value)
{
    uint32_t bits = 0u;
    if (!ReadUInt32(bytes, offset, bits))
        return false;

    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&value, &bits, sizeof(value));
    return true;
}

bool ReadBoundingSphere(const ByteView bytes, size_t& offset, Geometry::BoundingSphere& boundingSphere)
{
    return ReadFloat(bytes, offset, boundingSphere.position.x) &&
        ReadFloat(bytes, offset, boundingSphere.position.y) &&
        ReadFloat(bytes, offset, boundingSphere.position.z) &&
        ReadFloat(bytes, offset, boundingSphere.radius);
}

uint32_t AlignSampleCountToTriangleIndexCount(uint32_t value)
{
    return value - (value % 3u);
}

bool ReadExactAt(
    std::ifstream& input,
    const uint64_t offset,
    uint8_t* destination,
    const size_t size)
{
    if (size == 0u)
        return true;
    if (offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()))
        return false;
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input)
        return false;
    input.read(reinterpret_cast<char*>(destination), static_cast<std::streamsize>(size));
    return input.gcount() == static_cast<std::streamsize>(size);
}
}

std::vector<uint8_t> SerializeMeshArtifactPayload(const MeshArtifactData& mesh)
{
    if (mesh.vertices.size() > std::numeric_limits<uint32_t>::max() ||
        mesh.indices.size() > std::numeric_limits<uint32_t>::max())
    {
        return {};
    }

    MeshArtifactHeader header;
    header.vertexCount = static_cast<uint32_t>(mesh.vertices.size());
    header.indexCount = static_cast<uint32_t>(mesh.indices.size());
    header.materialIndex = mesh.materialIndex;
    const auto boundingSphere = mesh.hasBoundingSphere
        ? mesh.boundingSphere
        : Geometry::ComputeBoundingSphere(mesh.vertices);

    std::vector<uint8_t> bytes;
    bytes.reserve(
        sizeof(MeshArtifactHeader) +
        sizeof(float) * 4u +
        mesh.vertices.size() * sizeof(Geometry::Vertex) +
        mesh.indices.size() * sizeof(uint32_t));
    AppendHeader(bytes, header);
    AppendBoundingSphere(bytes, boundingSphere);
    AppendArray(bytes, mesh.vertices);
    AppendArray(bytes, mesh.indices);
    return bytes;
}

std::vector<uint8_t> SerializeMeshArtifact(const MeshArtifactData& mesh)
{
    auto payload = SerializeMeshArtifactPayload(mesh);
    if (payload.empty())
        return {};

    NLS::Core::Assets::NativeArtifactMetadata metadata;
    metadata.artifactType = NLS::Core::Assets::ArtifactType::Mesh;
    metadata.schemaName = "mesh";
    metadata.schemaVersion = kMeshArtifactContainerSchemaVersion;
    return NLS::Core::Assets::WriteNativeArtifactContainer(std::move(metadata), payload);
}

namespace
{
struct MeshArtifactPayloadLayout
{
    MeshArtifactHeader header;
    Geometry::BoundingSphere boundingSphere;
    uint64_t vertexBytes = 0u;
    uint64_t indexBytes = 0u;
    size_t dataOffset = 0u;
    bool hasBoundingSphere = false;
};

uint32_t AlignToTriangleIndexCount(const size_t count)
{
    const auto clamped = (std::min)(
        count,
        static_cast<size_t>(std::numeric_limits<uint32_t>::max()));
    return static_cast<uint32_t>(clamped - clamped % 3u);
}

bool IsValidSimplificationInput(const MeshArtifactData& mesh)
{
    if (mesh.vertices.empty() ||
        mesh.indices.size() < kPreviewSimplificationMinimumIndexCount ||
        mesh.indices.size() % 3u != 0u)
    {
        return false;
    }

    for (const auto& vertex : mesh.vertices)
    {
        if (!std::isfinite(vertex.position[0]) ||
            !std::isfinite(vertex.position[1]) ||
            !std::isfinite(vertex.position[2]))
        {
            return false;
        }
    }
    return std::all_of(
        mesh.indices.begin(),
        mesh.indices.end(),
        [&mesh](const uint32_t index)
        {
            return index < mesh.vertices.size();
        });
}

std::optional<MeshArtifactData> CompactPreviewMesh(
    const MeshArtifactData& source,
    const uint32_t* indices,
    const size_t indexCount,
    const uint32_t maxVertices)
{
    if (indices == nullptr || indexCount < 3u || indexCount % 3u != 0u)
        return std::nullopt;

    constexpr uint32_t kUnmappedVertex = std::numeric_limits<uint32_t>::max();
    std::vector<uint32_t> remap(source.vertices.size(), kUnmappedVertex);
    MeshArtifactData result;
    result.vertices.reserve((std::min)(static_cast<size_t>(maxVertices), indexCount));
    result.indices.reserve(indexCount);
    result.materialIndex = source.materialIndex;

    for (size_t indexOffset = 0u; indexOffset < indexCount; ++indexOffset)
    {
        const auto sourceIndex = indices[indexOffset];
        auto& compactedIndex = remap[sourceIndex];
        if (compactedIndex == kUnmappedVertex)
        {
            if (result.vertices.size() >= maxVertices)
                return std::nullopt;
            compactedIndex = static_cast<uint32_t>(result.vertices.size());
            result.vertices.push_back(source.vertices[sourceIndex]);
        }
        result.indices.push_back(compactedIndex);
    }

    result.boundingSphere = Geometry::ComputeBoundingSphere(result.vertices);
    result.hasBoundingSphere = true;
    return result;
}

std::optional<MeshArtifactData> BuildEvenTriangleSubset(
    const MeshArtifactData& source,
    const uint32_t maxVertices,
    const uint32_t maxIndices)
{
    const auto triangleCount = source.indices.size() / 3u;
    const auto targetTriangleCount = (std::min)(
        triangleCount,
        static_cast<size_t>(maxIndices / 3u));
    if (targetTriangleCount == 0u)
        return std::nullopt;

    std::vector<uint32_t> selectedIndices;
    selectedIndices.reserve(targetTriangleCount * 3u);
    std::vector<uint8_t> selectedVertices(source.vertices.size(), 0u);
    size_t selectedVertexCount = 0u;
    for (size_t outputTriangle = 0u; outputTriangle < targetTriangleCount; ++outputTriangle)
    {
        const auto sourceTriangle = outputTriangle * triangleCount / targetTriangleCount;
        const auto sourceOffset = sourceTriangle * 3u;
        const auto first = source.indices[sourceOffset];
        const auto second = source.indices[sourceOffset + 1u];
        const auto third = source.indices[sourceOffset + 2u];
        if (first == second || first == third || second == third)
            continue;
        const auto newVertexCount =
            static_cast<size_t>(selectedVertices[first] == 0u) +
            static_cast<size_t>(selectedVertices[second] == 0u && second != first) +
            static_cast<size_t>(selectedVertices[third] == 0u && third != first && third != second);
        if (selectedVertexCount + newVertexCount > maxVertices)
            continue;
        selectedVertices[first] = 1u;
        selectedVertices[second] = 1u;
        selectedVertices[third] = 1u;
        selectedVertexCount += newVertexCount;
        selectedIndices.insert(selectedIndices.end(), {first, second, third});
    }

    if (selectedIndices.empty())
        return std::nullopt;
    return CompactPreviewMesh(
        source,
        selectedIndices.data(),
        selectedIndices.size(),
        maxVertices);
}

std::optional<MeshArtifactPayloadLayout> ParseMeshArtifactPayloadLayout(
    const ByteView payloadPrefix,
    const uint64_t payloadSize)
{
    MeshArtifactPayloadLayout layout;
    if (!ReadHeader(payloadPrefix, layout.header))
        return std::nullopt;

    layout.vertexBytes =
        static_cast<uint64_t>(layout.header.vertexCount) * sizeof(Geometry::Vertex);
    layout.indexBytes =
        static_cast<uint64_t>(layout.header.indexCount) * sizeof(uint32_t);
    const auto boundsBytes = layout.header.version >= kMeshArtifactVersionWithBoundingSphere
        ? sizeof(float) * 4u
        : 0u;
    if (layout.vertexBytes > (std::numeric_limits<uint64_t>::max)() - layout.indexBytes)
        return std::nullopt;

    const auto dataBytes = layout.vertexBytes + layout.indexBytes;
    const auto headerAndBoundsBytes = static_cast<uint64_t>(sizeof(MeshArtifactHeader) + boundsBytes);
    if (dataBytes > (std::numeric_limits<uint64_t>::max)() - headerAndBoundsBytes ||
        headerAndBoundsBytes + dataBytes != payloadSize)
    {
        return std::nullopt;
    }

    layout.dataOffset = sizeof(MeshArtifactHeader);
    if (layout.header.version >= kMeshArtifactVersionWithBoundingSphere)
    {
        if (!ReadBoundingSphere(payloadPrefix, layout.dataOffset, layout.boundingSphere))
            return std::nullopt;
        layout.hasBoundingSphere = true;
    }
    return layout;
}

std::optional<MeshArtifactData> DeserializeMeshArtifactInternal(
    const std::vector<uint8_t>& bytes,
    const NLS::Core::Assets::NativeArtifactPayloadValidation payloadValidation)
{
    const auto deserializeBegin = std::chrono::steady_clock::now();
    struct ScopedDeserializeTelemetry
    {
        std::chrono::steady_clock::time_point begin;
        size_t byteCount;

        ~ScopedDeserializeTelemetry()
        {
            NLS::Core::Assets::RecordArtifactLoadTelemetry({
                NLS::Core::Assets::ArtifactLoadTelemetryStage::CpuDeserialize,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - begin),
                byteCount});
        }
    } telemetry {deserializeBegin, bytes.size()};

    auto container = NLS::Core::Assets::ReadNativeArtifactContainerView(
        bytes,
        NLS::Core::Assets::ArtifactType::Mesh,
        kMeshArtifactContainerSchemaVersion,
        payloadValidation);
    if (!container.has_value())
        return std::nullopt;

    const ByteView payload {container->payloadData, container->payloadSize};
    const auto layout = ParseMeshArtifactPayloadLayout(payload, payload.size);
    if (!layout.has_value() ||
        layout->vertexBytes > (std::numeric_limits<size_t>::max)() ||
        layout->indexBytes > (std::numeric_limits<size_t>::max)())
    {
        return std::nullopt;
    }

    MeshArtifactData mesh;
    mesh.materialIndex = layout->header.materialIndex;
    mesh.boundingSphere = layout->boundingSphere;
    mesh.hasBoundingSphere = layout->hasBoundingSphere;

    const auto payloadCopyBegin = std::chrono::steady_clock::now();
    mesh.vertices.resize(layout->header.vertexCount);
    mesh.indices.resize(layout->header.indexCount);
    size_t offset = layout->dataOffset;
    if (!mesh.vertices.empty())
    {
        std::memcpy(
            mesh.vertices.data(),
            payload.data + offset,
            static_cast<size_t>(layout->vertexBytes));
        offset += static_cast<size_t>(layout->vertexBytes);
    }
    if (!mesh.indices.empty())
    {
        std::memcpy(
            mesh.indices.data(),
            payload.data + offset,
            static_cast<size_t>(layout->indexBytes));
    }
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy,
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - payloadCopyBegin),
        static_cast<size_t>(layout->vertexBytes + layout->indexBytes)});
    if (layout->header.version < kMeshArtifactVersionWithBoundingSphere)
    {
        mesh.boundingSphere = Geometry::ComputeBoundingSphere(mesh.vertices);
        mesh.hasBoundingSphere = true;
    }

    return mesh;
}

std::optional<MeshArtifactData> LoadContentAddressedMeshArtifactDirect(
    const std::filesystem::path& path,
    const std::atomic_bool* cancellationFlag)
{
    const auto isCancelled = [cancellationFlag]
    {
        return cancellationFlag != nullptr &&
            cancellationFlag->load(std::memory_order_acquire);
    };
    if (isCancelled())
        return std::nullopt;

    constexpr size_t kPayloadPrefixBytes =
        sizeof(MeshArtifactHeader) + sizeof(float) * 4u;
    const auto prefixReadBegin = std::chrono::steady_clock::now();
    auto prefix = NLS::Core::Assets::ReadNativeArtifactPayloadPrefixFromFile(
        path,
        NLS::Core::Assets::ArtifactType::Mesh,
        kMeshArtifactContainerSchemaVersion,
        kPayloadPrefixBytes);
    const auto prefixReadElapsed = std::chrono::steady_clock::now() - prefixReadBegin;
    if (isCancelled() ||
        !prefix.has_value() ||
        prefix->metadata.dependencyHash !=
            NLS::Core::Assets::ComputeNativeArtifactDependencyHash(prefix->metadata.dependencies))
    {
        return std::nullopt;
    }

    std::error_code fileSizeError;
    const auto fileSize = std::filesystem::file_size(path, fileSizeError);
    if (fileSizeError ||
        prefix->payloadOffset > fileSize ||
        prefix->payloadSize != fileSize - prefix->payloadOffset)
    {
        return std::nullopt;
    }

    const auto cpuDeserializeBegin = std::chrono::steady_clock::now();
    const ByteView payloadPrefix {prefix->bytes.data(), prefix->bytes.size()};
    const auto layout = ParseMeshArtifactPayloadLayout(payloadPrefix, prefix->payloadSize);
    if (!layout.has_value() ||
        layout->vertexBytes > (std::numeric_limits<size_t>::max)() ||
        layout->indexBytes > (std::numeric_limits<size_t>::max)())
    {
        return std::nullopt;
    }

    MeshArtifactData mesh;
    mesh.materialIndex = layout->header.materialIndex;
    mesh.boundingSphere = layout->boundingSphere;
    mesh.hasBoundingSphere = layout->hasBoundingSphere;
    const auto allocationBegin = std::chrono::steady_clock::now();
    mesh.vertices.resize(layout->header.vertexCount);
    mesh.indices.resize(layout->header.indexCount);
    const auto allocationEnd = std::chrono::steady_clock::now();
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy,
        std::chrono::duration_cast<std::chrono::microseconds>(allocationEnd - allocationBegin),
        static_cast<size_t>(layout->vertexBytes + layout->indexBytes)});
    auto cpuDeserializeElapsed = allocationEnd - cpuDeserializeBegin;

    const std::array<Detail::ArtifactFileReadRange, 2u> ranges {{
        {mesh.vertices.empty() ? nullptr : mesh.vertices.data(), static_cast<size_t>(layout->vertexBytes)},
        {mesh.indices.empty() ? nullptr : mesh.indices.data(), static_cast<size_t>(layout->indexBytes)}
    }};
    const auto payloadReadBegin = std::chrono::steady_clock::now();
    if (!Detail::ReadArtifactFileRanges(
        path,
        prefix->payloadOffset + layout->dataOffset,
        ranges,
        cancellationFlag))
    {
        return std::nullopt;
    }
    const auto payloadReadElapsed = std::chrono::steady_clock::now() - payloadReadBegin;

    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead,
        std::chrono::duration_cast<std::chrono::microseconds>(
            prefixReadElapsed + payloadReadElapsed),
        static_cast<size_t>(fileSize),
        path.generic_string()});
    if (layout->header.version < kMeshArtifactVersionWithBoundingSphere)
    {
        const auto boundsBegin = std::chrono::steady_clock::now();
        mesh.boundingSphere = Geometry::ComputeBoundingSphere(mesh.vertices);
        mesh.hasBoundingSphere = true;
        cpuDeserializeElapsed += std::chrono::steady_clock::now() - boundsBegin;
    }
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        NLS::Core::Assets::ArtifactLoadTelemetryStage::CpuDeserialize,
        std::chrono::duration_cast<std::chrono::microseconds>(cpuDeserializeElapsed),
        static_cast<size_t>(prefix->payloadSize)});
    return mesh;
}
}

std::optional<MeshArtifactData> DeserializeMeshArtifact(const std::vector<uint8_t>& bytes)
{
    return DeserializeMeshArtifactInternal(
        bytes,
        NLS::Core::Assets::NativeArtifactPayloadValidation::VerifyHash);
}

#if defined(NLS_ENABLE_TEST_HOOKS)
std::optional<MeshArtifactData> DeserializeMeshArtifactTrustedForTesting(
    const std::vector<uint8_t>& bytes)
{
    return DeserializeMeshArtifactInternal(
        bytes,
        NLS::Core::Assets::NativeArtifactPayloadValidation::TrustContentAddressedStorage);
}
#endif

std::optional<MeshArtifactHeaderPreview> ReadMeshArtifactHeaderPreview(
    const std::filesystem::path& path,
    const uint64_t maxMetadataBytes)
{
    const auto prefix = NLS::Core::Assets::ReadNativeArtifactPayloadPrefixFromFile(
        path,
        NLS::Core::Assets::ArtifactType::Mesh,
        kMeshArtifactContainerSchemaVersion,
        sizeof(MeshArtifactHeader) + sizeof(float) * 4u,
        maxMetadataBytes);
    if (!prefix.has_value())
        return std::nullopt;

    const ByteView payload {prefix->bytes.data(), prefix->bytes.size()};
    MeshArtifactHeader header;
    if (!ReadHeader(payload, header))
        return std::nullopt;

    MeshArtifactHeaderPreview preview;
    preview.vertexCount = header.vertexCount;
    preview.indexCount = header.indexCount;
    preview.materialIndex = header.materialIndex;
    if (header.version >= kMeshArtifactVersionWithBoundingSphere)
    {
        size_t offset = sizeof(MeshArtifactHeader);
        if (!ReadBoundingSphere(payload, offset, preview.boundingSphere))
            return std::nullopt;
        preview.hasBoundingSphere = true;
    }
    return preview;
}

bool IsMeshArtifactFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;

    std::array<uint8_t, kNativeArtifactHeaderSize> headerBytes {};
    input.read(
        reinterpret_cast<char*>(headerBytes.data()),
        static_cast<std::streamsize>(headerBytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(headerBytes.size()))
        return false;

    NativeArtifactHeaderPreview header;
    if (!ReadNativeArtifactHeaderPreview(headerBytes, header) ||
        header.metadataSize > std::numeric_limits<uint64_t>::max() - kNativeArtifactHeaderSize ||
        header.payloadOffset != kNativeArtifactHeaderSize + header.metadataSize)
    {
        return false;
    }

    std::error_code error;
    const auto fileSize = std::filesystem::file_size(path, error);
    return !error &&
        header.payloadOffset <= fileSize &&
        header.payloadSize == fileSize - header.payloadOffset;
}

std::optional<MeshArtifactData> SimplifyMeshArtifactForPreview(
    const MeshArtifactData& mesh,
    const uint32_t maxVertices,
    const uint32_t maxIndices)
{
    const auto targetIndexCount = AlignToTriangleIndexCount(
        (std::min)(mesh.indices.size(), static_cast<size_t>(maxIndices)));
    if (maxVertices == 0u ||
        targetIndexCount < kPreviewSimplificationMinimumIndexCount ||
        !IsValidSimplificationInput(mesh))
    {
        return std::nullopt;
    }

    if (mesh.indices.size() <= targetIndexCount)
    {
        auto compacted = CompactPreviewMesh(
            mesh,
            mesh.indices.data(),
            mesh.indices.size(),
            maxVertices);
        if (compacted.has_value())
            return compacted;
    }

    std::vector<uint32_t> simplifiedIndices(mesh.indices.size());
    auto attemptTarget = targetIndexCount;
    for (size_t attempt = 0u;
        attempt < kPreviewSimplificationMaximumAttempts &&
        attemptTarget >= kPreviewSimplificationMinimumIndexCount;
        ++attempt)
    {
        const auto simplifiedCount = meshopt_simplify(
            simplifiedIndices.data(),
            mesh.indices.data(),
            mesh.indices.size(),
            mesh.vertices.front().position,
            mesh.vertices.size(),
            sizeof(Geometry::Vertex),
            attemptTarget,
            kPreviewSimplificationMaximumRelativeError,
            meshopt_SimplifyLockBorder |
                meshopt_SimplifyRegularize,
            nullptr);
        if (simplifiedCount >= kPreviewSimplificationMinimumIndexCount &&
            simplifiedCount <= targetIndexCount)
        {
            auto compacted = CompactPreviewMesh(
                mesh,
                simplifiedIndices.data(),
                simplifiedCount,
                maxVertices);
            if (compacted.has_value())
                return compacted;
        }

        if (attemptTarget == kPreviewSimplificationMinimumIndexCount)
            break;
        attemptTarget = AlignToTriangleIndexCount((std::max)(
            static_cast<size_t>(kPreviewSimplificationMinimumIndexCount),
            static_cast<size_t>(attemptTarget) * 3u / 4u));
    }

    attemptTarget = targetIndexCount;
    for (size_t attempt = 0u;
        attempt < kPreviewSimplificationMaximumAttempts &&
        attemptTarget >= kPreviewSimplificationMinimumIndexCount;
        ++attempt)
    {
        const auto simplifiedCount = meshopt_simplifySloppy(
            simplifiedIndices.data(),
            mesh.indices.data(),
            mesh.indices.size(),
            mesh.vertices.front().position,
            mesh.vertices.size(),
            sizeof(Geometry::Vertex),
            nullptr,
            attemptTarget,
            kPreviewSimplificationMaximumRelativeError,
            nullptr);
        if (simplifiedCount >= kPreviewSimplificationMinimumIndexCount &&
            simplifiedCount <= targetIndexCount)
        {
            auto compacted = CompactPreviewMesh(
                mesh,
                simplifiedIndices.data(),
                simplifiedCount,
                maxVertices);
            if (compacted.has_value())
                return compacted;
        }

        if (attemptTarget == kPreviewSimplificationMinimumIndexCount)
            break;
        attemptTarget = AlignToTriangleIndexCount((std::max)(
            static_cast<size_t>(kPreviewSimplificationMinimumIndexCount),
            static_cast<size_t>(attemptTarget) * 3u / 4u));
    }
    // Some imported triangle soups and degenerate CAD meshes have no valid
    // QEM/sloppy collapse at thumbnail budgets. Keep a spatially distributed
    // triangle subset instead of failing the entire prefab back to 405 draws.
    return BuildEvenTriangleSubset(mesh, maxVertices, maxIndices);
}

std::optional<MeshArtifactData> LoadMeshArtifactPreviewSample(
    const std::filesystem::path& path,
    const uint32_t maxVertices,
    const uint32_t maxIndices,
    const uint64_t maxMetadataBytes)
{
    if (maxVertices == 0u || maxIndices < 3u)
        return std::nullopt;

    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead,
        {},
        sizeof(MeshArtifactHeader),
        path.generic_string()
    });

    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;

    std::array<uint8_t, kNativeArtifactHeaderSize> nativeHeaderBytes {};
    input.read(
        reinterpret_cast<char*>(nativeHeaderBytes.data()),
        static_cast<std::streamsize>(nativeHeaderBytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(nativeHeaderBytes.size()))
        return std::nullopt;

    NativeArtifactHeaderPreview nativeHeader;
    if (!ReadNativeArtifactHeaderPreview(nativeHeaderBytes, nativeHeader) ||
        nativeHeader.metadataSize > maxMetadataBytes ||
        nativeHeader.metadataSize > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        nativeHeader.payloadOffset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()))
    {
        return std::nullopt;
    }

    std::error_code error;
    const auto fileSize = std::filesystem::file_size(path, error);
    if (error ||
        nativeHeader.payloadOffset > fileSize ||
        nativeHeader.payloadSize > fileSize - nativeHeader.payloadOffset ||
        nativeHeader.payloadOffset != kNativeArtifactHeaderSize + nativeHeader.metadataSize)
    {
        return std::nullopt;
    }

    const uint64_t payloadHeaderBytes =
        sizeof(MeshArtifactHeader) + sizeof(float) * 4u;
    if (nativeHeader.payloadSize < payloadHeaderBytes)
        return std::nullopt;

    std::array<uint8_t, sizeof(MeshArtifactHeader) + sizeof(float) * 4u> meshHeaderBytes {};
    if (!ReadExactAt(
            input,
            nativeHeader.payloadOffset,
            meshHeaderBytes.data(),
            meshHeaderBytes.size()))
    {
        return std::nullopt;
    }

    const ByteView payloadPrefix {meshHeaderBytes.data(), meshHeaderBytes.size()};
    MeshArtifactHeader meshHeader;
    if (!ReadHeader(payloadPrefix, meshHeader) ||
        meshHeader.version < kMeshArtifactVersionWithBoundingSphere)
    {
        return std::nullopt;
    }

    const uint64_t vertexBytes =
        static_cast<uint64_t>(meshHeader.vertexCount) * sizeof(Geometry::Vertex);
    const uint64_t indexBytes =
        static_cast<uint64_t>(meshHeader.indexCount) * sizeof(uint32_t);
    if (payloadHeaderBytes + vertexBytes + indexBytes != nativeHeader.payloadSize)
        return std::nullopt;

    MeshArtifactData mesh;
    mesh.materialIndex = meshHeader.materialIndex;
    size_t offset = sizeof(MeshArtifactHeader);
    if (!ReadBoundingSphere(payloadPrefix, offset, mesh.boundingSphere))
        return std::nullopt;
    mesh.hasBoundingSphere = true;

    const uint32_t sampledIndexCount =
        AlignSampleCountToTriangleIndexCount(std::min(meshHeader.indexCount, maxIndices));
    if (sampledIndexCount < 3u)
        return std::nullopt;

    const uint64_t indexStartOffset =
        nativeHeader.payloadOffset + payloadHeaderBytes + vertexBytes;
    constexpr uint32_t kCandidateOversampleFactor = 4u;
    constexpr uint32_t kMaximumSampleWindows = 64u;
    const uint32_t totalTriangleCount = meshHeader.indexCount / 3u;
    const uint32_t sampledTriangleCount = sampledIndexCount / 3u;
    const uint32_t candidateTriangleCount = static_cast<uint32_t>((std::min)(
        static_cast<uint64_t>(totalTriangleCount),
        static_cast<uint64_t>(sampledTriangleCount) * kCandidateOversampleFactor));
    const uint32_t sampleWindowCount = (std::min)({
        kMaximumSampleWindows,
        sampledTriangleCount,
        candidateTriangleCount
    });
    if (sampleWindowCount == 0u)
        return std::nullopt;

    std::vector<uint32_t> candidateIndices;
    candidateIndices.reserve(static_cast<size_t>(candidateTriangleCount) * 3u);
    for (uint32_t window = 0u; window < sampleWindowCount; ++window)
    {
        const uint32_t sourceBegin = static_cast<uint32_t>(
            (static_cast<uint64_t>(window) * totalTriangleCount) / sampleWindowCount);
        const uint32_t sourceEnd = static_cast<uint32_t>(
            (static_cast<uint64_t>(window + 1u) * totalTriangleCount) / sampleWindowCount);
        const uint32_t sourceCount = sourceEnd - sourceBegin;
        const uint32_t requestedCount =
            candidateTriangleCount / sampleWindowCount +
            (window < candidateTriangleCount % sampleWindowCount ? 1u : 0u);
        const uint32_t readTriangleCount = (std::min)(sourceCount, requestedCount);
        if (readTriangleCount == 0u)
            continue;

        const uint32_t readBegin = sourceBegin + (sourceCount - readTriangleCount) / 2u;
        const size_t destinationOffset = candidateIndices.size();
        candidateIndices.resize(destinationOffset + static_cast<size_t>(readTriangleCount) * 3u);
        if (!ReadExactAt(
                input,
                indexStartOffset + static_cast<uint64_t>(readBegin) * 3u * sizeof(uint32_t),
                reinterpret_cast<uint8_t*>(candidateIndices.data() + destinationOffset),
                static_cast<size_t>(readTriangleCount) * 3u * sizeof(uint32_t)))
        {
            return std::nullopt;
        }
    }

    std::unordered_map<uint32_t, uint32_t> remappedVertexIndices;
    remappedVertexIndices.reserve((std::min)(
        static_cast<size_t>(maxVertices),
        candidateIndices.size()));
    std::vector<uint32_t> originalVertexBySampleIndex;
    originalVertexBySampleIndex.reserve((std::min)(
        static_cast<size_t>(maxVertices),
        candidateIndices.size()));
    mesh.indices.reserve(sampledIndexCount);

    const size_t loadedCandidateTriangleCount = candidateIndices.size() / 3u;
    std::vector<bool> inspectedCandidateTriangles(loadedCandidateTriangleCount, false);
    const auto tryAppendCandidateTriangle = [&](const size_t candidateTriangleIndex)
    {
        if (candidateTriangleIndex >= loadedCandidateTriangleCount ||
            inspectedCandidateTriangles[candidateTriangleIndex] ||
            mesh.indices.size() + 2u >= sampledIndexCount)
        {
            return;
        }
        inspectedCandidateTriangles[candidateTriangleIndex] = true;
        const size_t index = candidateTriangleIndex * 3u;
        const std::array<uint32_t, 3u> triangle {
            candidateIndices[index + 0u],
            candidateIndices[index + 1u],
            candidateIndices[index + 2u]
        };
        if (triangle[0] >= meshHeader.vertexCount ||
            triangle[1] >= meshHeader.vertexCount ||
            triangle[2] >= meshHeader.vertexCount)
        {
            return;
        }

        size_t newVertexCount = 0u;
        for (const auto originalIndex : triangle)
        {
            if (remappedVertexIndices.find(originalIndex) == remappedVertexIndices.end())
                ++newVertexCount;
        }
        if (originalVertexBySampleIndex.size() + newVertexCount > maxVertices)
            return;

        for (const auto originalIndex : triangle)
        {
            auto [iterator, inserted] = remappedVertexIndices.emplace(
                originalIndex,
                static_cast<uint32_t>(originalVertexBySampleIndex.size()));
            if (inserted)
                originalVertexBySampleIndex.push_back(originalIndex);
            mesh.indices.push_back(iterator->second);
        }
    };

    for (uint32_t phase = 0u;
        phase < kCandidateOversampleFactor && mesh.indices.size() < sampledIndexCount;
        ++phase)
    {
        for (uint32_t sample = 0u;
            sample < sampledTriangleCount && mesh.indices.size() < sampledIndexCount;
            ++sample)
        {
            const auto candidateTriangleIndex = static_cast<size_t>(
                ((static_cast<uint64_t>(sample) * kCandidateOversampleFactor + phase) *
                    loadedCandidateTriangleCount) /
                (static_cast<uint64_t>(sampledTriangleCount) * kCandidateOversampleFactor));
            tryAppendCandidateTriangle(candidateTriangleIndex);
        }
    }
    for (size_t candidateTriangleIndex = 0u;
        candidateTriangleIndex < loadedCandidateTriangleCount && mesh.indices.size() < sampledIndexCount;
        ++candidateTriangleIndex)
    {
        tryAppendCandidateTriangle(candidateTriangleIndex);
    }
    if (mesh.indices.empty())
        return std::nullopt;

    mesh.vertices.resize(originalVertexBySampleIndex.size());
    std::vector<uint32_t> sampleOrder(originalVertexBySampleIndex.size());
    std::iota(sampleOrder.begin(), sampleOrder.end(), 0u);
    std::sort(
        sampleOrder.begin(),
        sampleOrder.end(),
        [&originalVertexBySampleIndex](const uint32_t left, const uint32_t right)
        {
            return originalVertexBySampleIndex[left] < originalVertexBySampleIndex[right];
        });

    const uint64_t vertexStartOffset = nativeHeader.payloadOffset + payloadHeaderBytes;
    for (size_t orderIndex = 0u; orderIndex < sampleOrder.size();)
    {
        const uint32_t firstSampleIndex = sampleOrder[orderIndex];
        const uint32_t firstOriginalIndex = originalVertexBySampleIndex[firstSampleIndex];
        size_t runCount = 1u;
        while (orderIndex + runCount < sampleOrder.size())
        {
            const uint32_t previousSampleIndex = sampleOrder[orderIndex + runCount - 1u];
            const uint32_t currentSampleIndex = sampleOrder[orderIndex + runCount];
            if (originalVertexBySampleIndex[currentSampleIndex] !=
                originalVertexBySampleIndex[previousSampleIndex] + 1u)
            {
                break;
            }
            ++runCount;
        }

        std::vector<Geometry::Vertex> runVertices(runCount);
        if (!ReadExactAt(
                input,
                vertexStartOffset + static_cast<uint64_t>(firstOriginalIndex) * sizeof(Geometry::Vertex),
                reinterpret_cast<uint8_t*>(runVertices.data()),
                runVertices.size() * sizeof(Geometry::Vertex)))
        {
            return std::nullopt;
        }
        for (size_t runIndex = 0u; runIndex < runCount; ++runIndex)
            mesh.vertices[sampleOrder[orderIndex + runIndex]] = runVertices[runIndex];

        orderIndex += runCount;
    }

    return mesh;
}

std::optional<MeshArtifactData> LoadMeshArtifact(const std::filesystem::path& path)
{
    return LoadMeshArtifact(path, nullptr);
}

std::optional<MeshArtifactData> LoadMeshArtifact(
    const std::filesystem::path& path,
    const std::atomic_bool* cancellationFlag)
{
    const auto portableArtifactPath =
        NLS::Core::Assets::TryMakePortableContentArtifactPath(path.generic_string());
    if (!portableArtifactPath.empty() &&
        !NLS::Core::Assets::IsRuntimeArtifactPathAuthorized(portableArtifactPath))
    {
        return std::nullopt;
    }
    const auto payloadValidation = portableArtifactPath.empty()
        ? NLS::Core::Assets::NativeArtifactPayloadValidation::VerifyHash
        : NLS::Core::Assets::NativeArtifactPayloadValidation::TrustContentAddressedStorage;

    auto isCancelled = [cancellationFlag]
    {
        return cancellationFlag != nullptr && cancellationFlag->load(std::memory_order_acquire);
    };
    if (isCancelled())
        return std::nullopt;

    if (payloadValidation ==
        NLS::Core::Assets::NativeArtifactPayloadValidation::TrustContentAddressedStorage)
    {
        auto directArtifact = LoadContentAddressedMeshArtifactDirect(path, cancellationFlag);
        if (directArtifact.has_value() || isCancelled())
            return directArtifact;
    }

    std::vector<uint8_t> bytes;
    const auto fileReadBegin = std::chrono::steady_clock::now();
    if (!Detail::ReadArtifactFileBytes(path, bytes, cancellationFlag))
        return std::nullopt;

    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead,
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - fileReadBegin),
        bytes.size(),
        path.generic_string()
    });

    auto artifact = DeserializeMeshArtifactInternal(
        bytes,
        payloadValidation);
    if (isCancelled())
        return std::nullopt;
    return artifact;
}
}
