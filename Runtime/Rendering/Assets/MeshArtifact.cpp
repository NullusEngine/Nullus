#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Geometry/BoundingSphereUtils.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/NativeArtifactContainer.h"

#include <cstring>
#include <algorithm>
#include <array>
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

std::optional<MeshArtifactData> DeserializeMeshArtifact(const std::vector<uint8_t>& bytes)
{
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        NLS::Core::Assets::ArtifactLoadTelemetryStage::CpuDeserialize});

    auto container = NLS::Core::Assets::ReadNativeArtifactContainerView(
        bytes,
        NLS::Core::Assets::ArtifactType::Mesh,
        kMeshArtifactContainerSchemaVersion);
    if (!container.has_value())
        return std::nullopt;

    const ByteView payload {container->payloadData, container->payloadSize};
    MeshArtifactHeader header;
    if (!ReadHeader(payload, header))
        return std::nullopt;

    const auto vertexBytes = static_cast<uint64_t>(header.vertexCount) * sizeof(Geometry::Vertex);
    const auto indexBytes = static_cast<uint64_t>(header.indexCount) * sizeof(uint32_t);
    const auto boundsBytes = header.version >= kMeshArtifactVersionWithBoundingSphere
        ? sizeof(float) * 4u
        : 0u;
    const auto payloadBytes = vertexBytes + indexBytes;
    if (payloadBytes > std::numeric_limits<size_t>::max())
        return std::nullopt;
    if (sizeof(MeshArtifactHeader) + boundsBytes + static_cast<size_t>(payloadBytes) != payload.size)
        return std::nullopt;

    MeshArtifactData mesh;
    mesh.vertices.resize(header.vertexCount);
    mesh.indices.resize(header.indexCount);
    mesh.materialIndex = header.materialIndex;

    size_t offset = sizeof(MeshArtifactHeader);
    if (header.version >= kMeshArtifactVersionWithBoundingSphere)
    {
        if (!ReadBoundingSphere(payload, offset, mesh.boundingSphere))
            return std::nullopt;
        mesh.hasBoundingSphere = true;
    }
    if (!mesh.vertices.empty())
    {
        std::memcpy(mesh.vertices.data(), payload.data + offset, static_cast<size_t>(vertexBytes));
        offset += static_cast<size_t>(vertexBytes);
    }

    if (!mesh.indices.empty())
        std::memcpy(mesh.indices.data(), payload.data + offset, static_cast<size_t>(indexBytes));
    if (header.version < kMeshArtifactVersionWithBoundingSphere)
    {
        mesh.boundingSphere = Geometry::ComputeBoundingSphere(mesh.vertices);
        mesh.hasBoundingSphere = true;
    }

    return mesh;
}

std::optional<MeshArtifactHeaderPreview> ReadMeshArtifactHeaderPreview(
    const std::filesystem::path& path,
    const uint64_t maxMetadataBytes)
{
    const auto prefix = NLS::Core::Assets::ReadNativeArtifactPayloadPrefixFromFile(
        path,
        NLS::Core::Assets::ArtifactType::Mesh,
        kMeshArtifactContainerSchemaVersion,
        sizeof(MeshArtifactHeader),
        maxMetadataBytes);
    if (!prefix.has_value())
        return std::nullopt;

    const ByteView payload {prefix->bytes.data(), prefix->bytes.size()};
    MeshArtifactHeader header;
    if (!ReadHeader(payload, header))
        return std::nullopt;

    return MeshArtifactHeaderPreview {
        header.vertexCount,
        header.indexCount
    };
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
    const uint32_t candidateIndexCount =
        AlignSampleCountToTriangleIndexCount(std::min(meshHeader.indexCount, sampledIndexCount * 4u));
    std::vector<uint32_t> candidateIndices(candidateIndexCount);
    if (!ReadExactAt(
            input,
            indexStartOffset,
            reinterpret_cast<uint8_t*>(candidateIndices.data()),
            static_cast<size_t>(candidateIndexCount) * sizeof(uint32_t)))
    {
        return std::nullopt;
    }

    std::unordered_map<uint32_t, uint32_t> remappedVertexIndices;
    remappedVertexIndices.reserve(std::min<uint32_t>(maxVertices, candidateIndexCount));
    std::vector<uint32_t> originalVertexBySampleIndex;
    originalVertexBySampleIndex.reserve(std::min<uint32_t>(maxVertices, candidateIndexCount));
    mesh.indices.reserve(sampledIndexCount);

    for (size_t index = 0u;
        index + 2u < candidateIndices.size() && mesh.indices.size() + 2u < sampledIndexCount;
        index += 3u)
    {
        const std::array<uint32_t, 3u> triangle {
            candidateIndices[index + 0u],
            candidateIndices[index + 1u],
            candidateIndices[index + 2u]
        };
        if (triangle[0] >= meshHeader.vertexCount ||
            triangle[1] >= meshHeader.vertexCount ||
            triangle[2] >= meshHeader.vertexCount)
        {
            continue;
        }

        size_t newVertexCount = 0u;
        for (const auto originalIndex : triangle)
        {
            if (remappedVertexIndices.find(originalIndex) == remappedVertexIndices.end())
                ++newVertexCount;
        }
        if (originalVertexBySampleIndex.size() + newVertexCount > maxVertices)
            continue;

        for (const auto originalIndex : triangle)
        {
            auto [iterator, inserted] = remappedVertexIndices.emplace(
                originalIndex,
                static_cast<uint32_t>(originalVertexBySampleIndex.size()));
            if (inserted)
                originalVertexBySampleIndex.push_back(originalIndex);
            mesh.indices.push_back(iterator->second);
        }
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

    NLS::Core::Assets::ArtifactLoadTelemetryRecord telemetry;
    telemetry.stage = NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead;
    telemetry.path = path.generic_string();
    NLS::Core::Assets::RecordArtifactLoadTelemetry(telemetry);

    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;

    auto isCancelled = [cancellationFlag]
    {
        return cancellationFlag != nullptr && cancellationFlag->load(std::memory_order_acquire);
    };
    if (isCancelled())
        return std::nullopt;

    std::vector<uint8_t> bytes;
    std::array<char, 64u * 1024u> buffer{};
    while (input)
    {
        if (isCancelled())
            return std::nullopt;

        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto readCount = input.gcount();
        if (readCount <= 0)
            break;

        const auto* begin = reinterpret_cast<const uint8_t*>(buffer.data());
        bytes.insert(bytes.end(), begin, begin + static_cast<size_t>(readCount));
    }

    if (isCancelled())
        return std::nullopt;

    auto artifact = DeserializeMeshArtifact(bytes);
    if (isCancelled())
        return std::nullopt;
    return artifact;
}
}
