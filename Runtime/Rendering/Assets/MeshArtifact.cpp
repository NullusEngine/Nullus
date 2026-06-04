#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Geometry/BoundingSphereUtils.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/NativeArtifactContainer.h"

#include <cstring>
#include <array>
#include <fstream>
#include <limits>

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

std::optional<MeshArtifactData> LoadMeshArtifact(const std::filesystem::path& path)
{
    return LoadMeshArtifact(path, nullptr);
}

std::optional<MeshArtifactData> LoadMeshArtifact(
    const std::filesystem::path& path,
    const std::atomic_bool* cancellationFlag)
{
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
