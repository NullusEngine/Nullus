#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/ArtifactFileReader.h"
#include "Rendering/Geometry/BoundingSphereUtils.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/NativeArtifactContainer.h"

#include <cstring>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <string>

namespace NLS::Render::Assets
{
namespace
{
constexpr uint32_t kMeshArtifactMagic = 0x484D4E4Eu; // "NNMH" little-endian.
constexpr uint32_t kMeshArtifactVersion = 1u;
constexpr uint32_t kMeshArtifactVersionWithBoundingSphere = 2u;
constexpr uint32_t kMeshArtifactContainerSchemaVersion = 3u;
constexpr uint32_t kMeshArtifactBundleMagic = 0x444F4C4Eu; // "NLOD"
constexpr uint32_t kMeshArtifactBundleVersion = 1u;

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
    {
        std::ifstream input(path, std::ios::binary);
        uint32_t magic = 0u;
        if (input.read(reinterpret_cast<char*>(&magic), sizeof(magic)) &&
            magic == kMeshArtifactBundleMagic)
        {
            const auto bundle = LoadMeshArtifactBundle(path);
            if (!bundle.has_value() || bundle->lodResources.empty())
                return std::nullopt;

            const auto& mesh = bundle->lodResources.front().mesh;
            MeshArtifactHeaderPreview preview;
            preview.vertexCount = static_cast<uint32_t>(mesh.vertices.size());
            preview.indexCount = static_cast<uint32_t>(mesh.indices.size());
            preview.materialIndex = mesh.materialIndex;
            preview.boundingSphere = mesh.boundingSphere;
            preview.hasBoundingSphere = mesh.hasBoundingSphere;
            return preview;
        }
    }

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

    uint32_t prefixMagic = 0u;
    input.read(reinterpret_cast<char*>(&prefixMagic), sizeof(prefixMagic));
    if (input.gcount() == static_cast<std::streamsize>(sizeof(prefixMagic)) &&
        prefixMagic == kMeshArtifactBundleMagic)
    {
        return LoadMeshArtifactBundle(path).has_value();
    }
    input.clear();
    input.seekg(0, std::ios::beg);

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

    if (auto bundle = DeserializeMeshArtifactBundle(bytes);
        bundle.has_value() && !bundle->lodResources.empty())
    {
        return std::move(bundle->lodResources.front().mesh);
    }

    auto artifact = DeserializeMeshArtifactInternal(
        bytes,
        payloadValidation);
    if (isCancelled())
        return std::nullopt;
    return artifact;
}

std::vector<uint8_t> SerializeMeshArtifactBundle(const MeshArtifactBundle& bundle)
{
    if (bundle.lodResources.empty() ||
        bundle.lodResources.size() > std::numeric_limits<uint32_t>::max())
    {
        return {};
    }

    std::vector<uint8_t> bytes;
    AppendUInt32(bytes, kMeshArtifactBundleMagic);
    AppendUInt32(bytes, kMeshArtifactBundleVersion);
    AppendUInt32(bytes, static_cast<uint32_t>(bundle.lodResources.size()));
    for (const auto& lod : bundle.lodResources)
    {
        const auto meshBytes = SerializeMeshArtifact(lod.mesh);
        if (meshBytes.empty() || meshBytes.size() > std::numeric_limits<uint32_t>::max())
            return {};
        AppendFloat32(bytes, lod.screenSize);
        AppendUInt32(bytes, static_cast<uint32_t>(meshBytes.size()));
        bytes.insert(bytes.end(), meshBytes.begin(), meshBytes.end());
    }
    return bytes;
}

std::optional<MeshArtifactBundle> DeserializeMeshArtifactBundle(
    const std::vector<uint8_t>& bytes)
{
    const ByteView view {bytes.data(), bytes.size()};
    size_t offset = 0u;
    uint32_t magic = 0u;
    uint32_t version = 0u;
    uint32_t lodCount = 0u;
    if (!ReadUInt32(view, offset, magic) ||
        !ReadUInt32(view, offset, version) ||
        !ReadUInt32(view, offset, lodCount) ||
        magic != kMeshArtifactBundleMagic ||
        version != kMeshArtifactBundleVersion ||
        lodCount == 0u)
    {
        return std::nullopt;
    }

    MeshArtifactBundle bundle;
    bundle.schemaVersion = version;
    bundle.lodResources.reserve(lodCount);
    for (uint32_t lodIndex = 0u; lodIndex < lodCount; ++lodIndex)
    {
        float screenSize = 0.0f;
        uint32_t meshByteCount = 0u;
        if (!ReadFloat(view, offset, screenSize) ||
            !ReadUInt32(view, offset, meshByteCount) ||
            meshByteCount > view.size - offset)
        {
            return std::nullopt;
        }
        std::vector<uint8_t> meshBytes(
            view.data + offset,
            view.data + offset + meshByteCount);
        offset += meshByteCount;
        auto mesh = DeserializeMeshArtifact(meshBytes);
        if (!mesh.has_value())
            return std::nullopt;
        bundle.lodResources.push_back({std::move(*mesh), screenSize});
    }
    if (offset != view.size)
        return std::nullopt;
    return bundle;
}

uint32_t SelectMeshArtifactLOD(
    const MeshArtifactBundle& bundle,
    const float screenSize,
    const uint32_t minLOD,
    const uint32_t maxLOD)
{
    if (bundle.lodResources.empty())
        return 0u;

    uint32_t selectedLOD = static_cast<uint32_t>(bundle.lodResources.size() - 1u);
    for (size_t index = 0u; index < bundle.lodResources.size(); ++index)
    {
        if (screenSize >= bundle.lodResources[index].screenSize)
        {
            selectedLOD = static_cast<uint32_t>(index);
            break;
        }
    }

    const auto availableMax = static_cast<uint32_t>(bundle.lodResources.size() - 1u);
    const auto clampedMin = (std::min)(minLOD, availableMax);
    const auto clampedMax = (std::min)((std::max)(maxLOD, clampedMin), availableMax);
    return std::clamp(selectedLOD, clampedMin, clampedMax);
}

std::optional<MeshArtifactBundle> LoadMeshArtifactBundle(const std::filesystem::path& path)
{
    const auto portableArtifactPath =
        NLS::Core::Assets::TryMakePortableContentArtifactPath(path.generic_string());
    if (!portableArtifactPath.empty() &&
        !NLS::Core::Assets::IsRuntimeArtifactPathAuthorized(portableArtifactPath))
    {
        return std::nullopt;
    }

    std::vector<uint8_t> bytes;
    if (!Detail::ReadArtifactFileBytes(path, bytes, nullptr))
        return std::nullopt;
    if (auto bundle = DeserializeMeshArtifactBundle(bytes))
        return bundle;
    if (auto mesh = DeserializeMeshArtifact(bytes))
    {
        MeshArtifactBundle bundle;
        bundle.lodResources.push_back({std::move(*mesh), 1.0f});
        return bundle;
    }
    return std::nullopt;
}

std::optional<MeshArtifactData> LoadMeshArtifactLOD(
    const std::filesystem::path& path,
    const float screenSize,
    const uint32_t minLOD,
    const uint32_t maxLOD)
{
    auto bundle = LoadMeshArtifactBundle(path);
    if (!bundle.has_value() || bundle->lodResources.empty())
        return std::nullopt;
    const auto selectedLOD = SelectMeshArtifactLOD(*bundle, screenSize, minLOD, maxLOD);
    return std::move(bundle->lodResources[selectedLOD].mesh);
}
}
