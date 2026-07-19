#include "Assets/NativeArtifactContainer.h"

#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/AssetPath.h"

#include <algorithm>
#include <charconv>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>

namespace NLS::Core::Assets
{
namespace
{
constexpr uint32_t kNativeArtifactMagic = 0x41534C4Eu; // "NLSA" little-endian.
constexpr uint32_t kNativeArtifactContainerVersion = 1u;
constexpr uint32_t kNativeArtifactFlagsLittleEndian = 1u;
constexpr uint64_t kNativeArtifactHeaderSize = 64u;

struct NativeArtifactHeader
{
    uint32_t magic = kNativeArtifactMagic;
    uint32_t containerVersion = kNativeArtifactContainerVersion;
    uint32_t headerSize = static_cast<uint32_t>(kNativeArtifactHeaderSize);
    uint32_t flags = kNativeArtifactFlagsLittleEndian;
    uint32_t artifactType = static_cast<uint32_t>(ArtifactType::Unknown);
    uint32_t schemaVersion = 1u;
    uint64_t metadataSize = 0u;
    uint64_t payloadSize = 0u;
    uint64_t payloadOffset = 0u;
    uint64_t reserved0 = 0u;
    uint64_t reserved1 = 0u;
};

static_assert(sizeof(NativeArtifactHeader) == kNativeArtifactHeaderSize);

const char* ToMetadataString(const ArtifactType type)
{
    switch (type)
    {
    case ArtifactType::Model: return "model";
    case ArtifactType::Mesh: return "mesh";
    case ArtifactType::Material: return "material";
    case ArtifactType::Texture: return "texture";
    case ArtifactType::Skeleton: return "skeleton";
    case ArtifactType::Skin: return "skin";
    case ArtifactType::AnimationClip: return "animation";
    case ArtifactType::MorphTarget: return "morph";
    case ArtifactType::Prefab: return "prefab";
    case ArtifactType::Scene: return "scene";
    case ArtifactType::Shader: return "shader";
    case ArtifactType::Audio: return "audio";
    case ArtifactType::Unknown:
    case ArtifactType::Count:
        break;
    }
    return "unknown";
}

ArtifactType ArtifactTypeFromMetadataString(const std::string& value)
{
    if (value == "model") return ArtifactType::Model;
    if (value == "mesh") return ArtifactType::Mesh;
    if (value == "material") return ArtifactType::Material;
    if (value == "texture") return ArtifactType::Texture;
    if (value == "skeleton") return ArtifactType::Skeleton;
    if (value == "skin") return ArtifactType::Skin;
    if (value == "animation") return ArtifactType::AnimationClip;
    if (value == "morph") return ArtifactType::MorphTarget;
    if (value == "prefab") return ArtifactType::Prefab;
    if (value == "scene") return ArtifactType::Scene;
    if (value == "shader") return ArtifactType::Shader;
    if (value == "audio") return ArtifactType::Audio;
    return ArtifactType::Unknown;
}

const char* ToMetadataString(const AssetDependencyKind kind)
{
    switch (kind)
    {
    case AssetDependencyKind::SourceFileHash: return "source-file-hash";
    case AssetDependencyKind::SourceAssetGuid: return "source-asset-guid";
    case AssetDependencyKind::ImportedArtifact: return "imported-artifact";
    case AssetDependencyKind::PathToGuidMapping: return "path-to-guid-mapping";
    case AssetDependencyKind::BuildTarget: return "build-target";
    case AssetDependencyKind::ImporterVersion: return "importer-version";
    case AssetDependencyKind::PostprocessorVersion: return "postprocessor-version";
    case AssetDependencyKind::PrefabBase: return "prefab-base";
    case AssetDependencyKind::NestedPrefab: return "nested-prefab";
    case AssetDependencyKind::PrefabOverrideTarget: return "prefab-override-target";
    case AssetDependencyKind::RuntimeComponentCapability: return "runtime-component-capability";
    case AssetDependencyKind::RawPackageFile: return "raw-package-file";
    case AssetDependencyKind::PrefabValidation: return "prefab-validation";
    }
    return "unknown";
}

std::optional<AssetDependencyKind> DependencyKindFromMetadataString(const std::string& value)
{
    if (value == "source-file-hash") return AssetDependencyKind::SourceFileHash;
    if (value == "source-asset-guid") return AssetDependencyKind::SourceAssetGuid;
    if (value == "imported-artifact") return AssetDependencyKind::ImportedArtifact;
    if (value == "path-to-guid-mapping") return AssetDependencyKind::PathToGuidMapping;
    if (value == "build-target") return AssetDependencyKind::BuildTarget;
    if (value == "importer-version") return AssetDependencyKind::ImporterVersion;
    if (value == "postprocessor-version") return AssetDependencyKind::PostprocessorVersion;
    if (value == "prefab-base") return AssetDependencyKind::PrefabBase;
    if (value == "nested-prefab") return AssetDependencyKind::NestedPrefab;
    if (value == "prefab-override-target") return AssetDependencyKind::PrefabOverrideTarget;
    if (value == "runtime-component-capability") return AssetDependencyKind::RuntimeComponentCapability;
    if (value == "raw-package-file") return AssetDependencyKind::RawPackageFile;
    if (value == "prefab-validation") return AssetDependencyKind::PrefabValidation;
    return std::nullopt;
}

void AppendUInt32(std::vector<uint8_t>& bytes, const uint32_t value)
{
    bytes.push_back(static_cast<uint8_t>(value & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((value >> 16u) & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((value >> 24u) & 0xFFu));
}

void AppendUInt64(std::vector<uint8_t>& bytes, const uint64_t value)
{
    for (uint32_t byteIndex = 0u; byteIndex < 8u; ++byteIndex)
        bytes.push_back(static_cast<uint8_t>((value >> (byteIndex * 8u)) & 0xFFu));
}

bool ReadUInt32(const std::span<const uint8_t> bytes, size_t& offset, uint32_t& value)
{
    if (offset + sizeof(uint32_t) > bytes.size())
        return false;
    value =
        static_cast<uint32_t>(bytes[offset]) |
        (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
        (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
        (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
    offset += sizeof(uint32_t);
    return true;
}

bool ReadUInt64(const std::span<const uint8_t> bytes, size_t& offset, uint64_t& value)
{
    if (offset + sizeof(uint64_t) > bytes.size())
        return false;
    value = 0u;
    for (uint32_t byteIndex = 0u; byteIndex < 8u; ++byteIndex)
        value |= static_cast<uint64_t>(bytes[offset + byteIndex]) << (byteIndex * 8u);
    offset += sizeof(uint64_t);
    return true;
}

void AppendHeader(std::vector<uint8_t>& bytes, const NativeArtifactHeader& header)
{
    AppendUInt32(bytes, header.magic);
    AppendUInt32(bytes, header.containerVersion);
    AppendUInt32(bytes, header.headerSize);
    AppendUInt32(bytes, header.flags);
    AppendUInt32(bytes, header.artifactType);
    AppendUInt32(bytes, header.schemaVersion);
    AppendUInt64(bytes, header.metadataSize);
    AppendUInt64(bytes, header.payloadSize);
    AppendUInt64(bytes, header.payloadOffset);
    AppendUInt64(bytes, header.reserved0);
    AppendUInt64(bytes, header.reserved1);
}

bool ReadHeader(
    const std::span<const uint8_t> bytes,
    NativeArtifactHeader& header,
    std::string* diagnostics = nullptr)
{
    if (bytes.size() < kNativeArtifactHeaderSize)
    {
        if (diagnostics)
            *diagnostics = "Native artifact container is smaller than the header.";
        return false;
    }

    size_t offset = 0u;
    if (!ReadUInt32(bytes, offset, header.magic) ||
        !ReadUInt32(bytes, offset, header.containerVersion) ||
        !ReadUInt32(bytes, offset, header.headerSize) ||
        !ReadUInt32(bytes, offset, header.flags) ||
        !ReadUInt32(bytes, offset, header.artifactType) ||
        !ReadUInt32(bytes, offset, header.schemaVersion) ||
        !ReadUInt64(bytes, offset, header.metadataSize) ||
        !ReadUInt64(bytes, offset, header.payloadSize) ||
        !ReadUInt64(bytes, offset, header.payloadOffset) ||
        !ReadUInt64(bytes, offset, header.reserved0) ||
        !ReadUInt64(bytes, offset, header.reserved1))
    {
        if (diagnostics)
            *diagnostics = "Native artifact container header is truncated.";
        return false;
    }

    if (header.magic != kNativeArtifactMagic ||
        header.containerVersion != kNativeArtifactContainerVersion ||
        header.headerSize != kNativeArtifactHeaderSize ||
        header.flags != kNativeArtifactFlagsLittleEndian)
    {
        if (diagnostics)
            *diagnostics = "Native artifact container header is invalid.";
        return false;
    }

    const auto byteSize = static_cast<uint64_t>(bytes.size());
    if (header.metadataSize > byteSize - kNativeArtifactHeaderSize)
    {
        if (diagnostics)
            *diagnostics = "Native artifact metadata extends beyond the container.";
        return false;
    }

    const uint64_t expectedPayloadOffset = kNativeArtifactHeaderSize + header.metadataSize;
    if (header.payloadOffset != expectedPayloadOffset ||
        header.payloadOffset > byteSize ||
        header.payloadSize > byteSize - header.payloadOffset)
    {
        if (diagnostics)
            *diagnostics = "Native artifact payload is missing or truncated.";
        return false;
    }

    if (header.metadataSize > std::numeric_limits<size_t>::max() ||
        header.payloadSize > std::numeric_limits<size_t>::max() ||
        header.payloadOffset > std::numeric_limits<size_t>::max() ||
        header.metadataSize > static_cast<uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()) ||
        header.payloadSize > static_cast<uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()) ||
        header.payloadOffset > static_cast<uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()))
    {
        if (diagnostics)
            *diagnostics = "Native artifact container sizes exceed platform address limits.";
        return false;
    }

    if (header.payloadOffset + header.payloadSize != byteSize)
    {
        if (diagnostics)
            *diagnostics = "Native artifact container has unexpected trailing or missing payload bytes.";
        return false;
    }

    return true;
}

bool ReadHeaderPrefix(const std::vector<uint8_t>& bytes, NativeArtifactHeader& header)
{
    if (bytes.size() < kNativeArtifactHeaderSize)
        return false;

    size_t offset = 0u;
    if (!ReadUInt32(bytes, offset, header.magic) ||
        !ReadUInt32(bytes, offset, header.containerVersion) ||
        !ReadUInt32(bytes, offset, header.headerSize) ||
        !ReadUInt32(bytes, offset, header.flags) ||
        !ReadUInt32(bytes, offset, header.artifactType) ||
        !ReadUInt32(bytes, offset, header.schemaVersion) ||
        !ReadUInt64(bytes, offset, header.metadataSize) ||
        !ReadUInt64(bytes, offset, header.payloadSize) ||
        !ReadUInt64(bytes, offset, header.payloadOffset) ||
        !ReadUInt64(bytes, offset, header.reserved0) ||
        !ReadUInt64(bytes, offset, header.reserved1))
    {
        return false;
    }

    if (header.magic != kNativeArtifactMagic ||
        header.containerVersion != kNativeArtifactContainerVersion ||
        header.headerSize != kNativeArtifactHeaderSize ||
        header.flags != kNativeArtifactFlagsLittleEndian)
    {
        return false;
    }

    return header.metadataSize <= std::numeric_limits<size_t>::max() &&
        header.payloadSize <= std::numeric_limits<size_t>::max() &&
        header.payloadOffset <= std::numeric_limits<size_t>::max() &&
        header.metadataSize <= static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) &&
        header.payloadSize <= static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) &&
        header.payloadOffset <= static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max());
}

std::string EscapeValue(std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value)
    {
        switch (character)
        {
        case '\\': escaped += "\\\\"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        case '|': escaped += "\\p"; break;
        default: escaped.push_back(character); break;
        }
    }
    return escaped;
}

std::string UnescapeValue(std::string_view value)
{
    std::string unescaped;
    unescaped.reserve(value.size());
    bool escaping = false;
    for (const char character : value)
    {
        if (!escaping)
        {
            if (character == '\\')
                escaping = true;
            else
                unescaped.push_back(character);
            continue;
        }

        switch (character)
        {
        case 'n': unescaped.push_back('\n'); break;
        case 'r': unescaped.push_back('\r'); break;
        case 't': unescaped.push_back('\t'); break;
        case 'p': unescaped.push_back('|'); break;
        default: unescaped.push_back(character); break;
        }
        escaping = false;
    }
    if (escaping)
        unescaped.push_back('\\');
    return unescaped;
}

std::vector<std::string> SplitEscapedList(std::string_view text)
{
    std::vector<std::string> parts;
    std::string current;
    bool escaping = false;
    for (const char character : text)
    {
        if (!escaping && character == '|')
        {
            parts.push_back(current);
            current.clear();
            continue;
        }

        current.push_back(character);
        escaping = !escaping && character == '\\';
        if (escaping)
            continue;
        escaping = false;
    }
    parts.push_back(current);
    return parts;
}

std::optional<uint32_t> ParseUInt32(const std::string& text)
{
    uint32_t value = 0u;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc {} || result.ptr != end)
        return std::nullopt;
    return value;
}

void AppendMetadataLine(std::string& output, const std::string& key, std::string_view value)
{
    output += key;
    output.push_back('=');
    output += EscapeValue(value);
    output.push_back('\n');
}

std::string SerializeMetadata(const NativeArtifactMetadata& metadata)
{
    std::string text;
    AppendMetadataLine(text, "ARTIFACT_TYPE", ToMetadataString(metadata.artifactType));
    AppendMetadataLine(text, "SCHEMA_NAME", metadata.schemaName);
    AppendMetadataLine(text, "SCHEMA_VERSION", std::to_string(metadata.schemaVersion));
    AppendMetadataLine(text, "SOURCE_ASSET_ID", metadata.sourceAssetId.ToString());
    AppendMetadataLine(text, "SUB_ASSET_KEY", metadata.subAssetKey);
    AppendMetadataLine(text, "DISPLAY_NAME", metadata.displayName);
    AppendMetadataLine(text, "IMPORTER_ID", metadata.importerId);
    AppendMetadataLine(text, "IMPORTER_VERSION", std::to_string(metadata.importerVersion));
    AppendMetadataLine(text, "TARGET_PLATFORM", metadata.targetPlatform);
    AppendMetadataLine(text, "PAYLOAD_HASH", metadata.payloadHash);
    AppendMetadataLine(text, "DEPENDENCY_HASH", metadata.dependencyHash);
    for (const auto& dependency : metadata.dependencies)
    {
        std::string value = ToMetadataString(dependency.kind);
        value.push_back('|');
        value += EscapeValue(dependency.value);
        value.push_back('|');
        value += EscapeValue(dependency.hashOrVersion);
        AppendMetadataLine(text, "DEPENDENCY", value);
    }
    return text;
}

std::optional<NativeArtifactMetadata> DeserializeMetadata(std::string_view text)
{
    NativeArtifactMetadata metadata;
    std::stringstream stream {std::string(text)};
    std::string line;
    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;

        const auto equals = line.find('=');
        if (equals == std::string::npos)
            return std::nullopt;

        const auto key = line.substr(0u, equals);
        const auto value = UnescapeValue(std::string_view(line).substr(equals + 1u));
        if (key == "ARTIFACT_TYPE")
            metadata.artifactType = ArtifactTypeFromMetadataString(value);
        else if (key == "SCHEMA_NAME")
            metadata.schemaName = value;
        else if (key == "SCHEMA_VERSION")
        {
            const auto parsed = ParseUInt32(value);
            if (!parsed.has_value())
                return std::nullopt;
            metadata.schemaVersion = *parsed;
        }
        else if (key == "SOURCE_ASSET_ID")
        {
            if (!value.empty())
            {
                const auto parsed = NLS::Guid::TryParse(value);
                if (!parsed.has_value())
                    return std::nullopt;
                metadata.sourceAssetId = AssetId(*parsed);
            }
        }
        else if (key == "SUB_ASSET_KEY")
            metadata.subAssetKey = value;
        else if (key == "DISPLAY_NAME")
            metadata.displayName = value;
        else if (key == "IMPORTER_ID")
            metadata.importerId = value;
        else if (key == "IMPORTER_VERSION")
        {
            const auto parsed = ParseUInt32(value);
            if (!parsed.has_value())
                return std::nullopt;
            metadata.importerVersion = *parsed;
        }
        else if (key == "TARGET_PLATFORM")
            metadata.targetPlatform = value;
        else if (key == "PAYLOAD_HASH")
            metadata.payloadHash = value;
        else if (key == "DEPENDENCY_HASH")
            metadata.dependencyHash = value;
        else if (key == "DEPENDENCY")
        {
            const auto parts = SplitEscapedList(value);
            if (parts.size() != 3u)
                return std::nullopt;
            const auto kind = DependencyKindFromMetadataString(parts[0]);
            if (!kind.has_value())
                return std::nullopt;
            metadata.dependencies.push_back({
                *kind,
                UnescapeValue(parts[1]),
                UnescapeValue(parts[2])
            });
        }
    }

    if (metadata.artifactType == ArtifactType::Unknown ||
        metadata.schemaName.empty() ||
        metadata.schemaVersion == 0u ||
        metadata.payloadHash.empty() ||
        metadata.dependencyHash.empty())
    {
        return std::nullopt;
    }
    return metadata;
}

uint64_t Fnv1a64(std::string_view bytes)
{
    uint64_t hash = 1469598103934665603ull;
    for (const auto byte : bytes)
    {
        hash ^= static_cast<uint8_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}
}

size_t NativeArtifactContainerHeaderSize()
{
    return static_cast<size_t>(kNativeArtifactHeaderSize);
}

std::string ComputeNativeArtifactPayloadHash(const std::vector<uint8_t>& payload)
{
    return ComputeNativeArtifactPayloadHash(payload.data(), payload.size());
}

std::string ComputeNativeArtifactPayloadHash(const uint8_t* payload, const size_t payloadSize)
{
    uint64_t hash = 1469598103934665603ull;
    for (size_t byteIndex = 0u; byteIndex < payloadSize; ++byteIndex)
    {
        hash ^= payload[byteIndex];
        hash *= 1099511628211ull;
    }
    return "fnv1a64:" + std::to_string(hash);
}

std::string ComputeNativeArtifactDependencyHash(const std::vector<AssetDependencyRecord>& dependencies)
{
    std::vector<std::string> normalized;
    normalized.reserve(dependencies.size());
    for (const auto& dependency : dependencies)
    {
        normalized.push_back(
            std::string(ToMetadataString(dependency.kind)) +
            "|" + dependency.value +
            "|" + dependency.hashOrVersion);
    }
    std::sort(normalized.begin(), normalized.end());

    std::string joined;
    for (const auto& dependency : normalized)
    {
        joined += dependency;
        joined.push_back('\n');
    }
    return "fnv1a64:" + std::to_string(Fnv1a64(joined));
}

std::vector<uint8_t> WriteNativeArtifactContainer(
    NativeArtifactMetadata metadata,
    const std::vector<uint8_t>& payload)
{
    if (metadata.artifactType == ArtifactType::Unknown ||
        metadata.schemaName.empty() ||
        metadata.schemaVersion == 0u)
    {
        return {};
    }

    metadata.payloadHash = ComputeNativeArtifactPayloadHash(payload);
    metadata.dependencyHash = ComputeNativeArtifactDependencyHash(metadata.dependencies);
    const auto metadataText = SerializeMetadata(metadata);

    if (metadataText.size() > std::numeric_limits<uint64_t>::max() ||
        payload.size() > std::numeric_limits<uint64_t>::max() ||
        static_cast<uint64_t>(metadataText.size()) > std::numeric_limits<uint64_t>::max() - kNativeArtifactHeaderSize ||
        static_cast<uint64_t>(payload.size()) > std::numeric_limits<uint64_t>::max() - kNativeArtifactHeaderSize - static_cast<uint64_t>(metadataText.size()))
    {
        return {};
    }

    NativeArtifactHeader header;
    header.artifactType = static_cast<uint32_t>(metadata.artifactType);
    header.schemaVersion = metadata.schemaVersion;
    header.metadataSize = static_cast<uint64_t>(metadataText.size());
    header.payloadSize = static_cast<uint64_t>(payload.size());
    header.payloadOffset = kNativeArtifactHeaderSize + header.metadataSize;

    std::vector<uint8_t> bytes;
    bytes.reserve(static_cast<size_t>(header.payloadOffset + header.payloadSize));
    AppendHeader(bytes, header);
    bytes.insert(bytes.end(), metadataText.begin(), metadataText.end());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

std::optional<NativeArtifactContainer> ReadNativeArtifactContainer(
    const std::vector<uint8_t>& bytes,
    const ArtifactType expectedType,
    const uint32_t expectedSchemaVersion)
{
    auto view = ReadNativeArtifactContainerView(bytes, expectedType, expectedSchemaVersion);
    if (!view.has_value())
        return std::nullopt;

    NativeArtifactContainer container;
    container.metadata = std::move(view->metadata);
    container.payload.assign(view->payloadData, view->payloadData + view->payloadSize);
    RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy,
        {},
        view->payloadSize});
    return container;
}

std::optional<NativeArtifactContainerView> ReadNativeArtifactContainerView(
    const std::vector<uint8_t>& bytes,
    const ArtifactType expectedType,
    const uint32_t expectedSchemaVersion,
    std::string* diagnostics)
{
    return ReadNativeArtifactContainerView(
        std::span<const uint8_t>(bytes.data(), bytes.size()),
        expectedType,
        expectedSchemaVersion,
        NativeArtifactPayloadValidation::VerifyHash,
        diagnostics);
}

std::optional<NativeArtifactContainerView> ReadNativeArtifactContainerView(
    const std::vector<uint8_t>& bytes,
    const ArtifactType expectedType,
    const uint32_t expectedSchemaVersion,
    const NativeArtifactPayloadValidation payloadValidation,
    std::string* diagnostics)
{
    return ReadNativeArtifactContainerView(
        std::span<const uint8_t>(bytes.data(), bytes.size()),
        expectedType,
        expectedSchemaVersion,
        payloadValidation,
        diagnostics);
}

std::optional<NativeArtifactContainerView> ReadNativeArtifactContainerView(
    const std::span<const uint8_t> bytes,
    const ArtifactType expectedType,
    const uint32_t expectedSchemaVersion,
    const NativeArtifactPayloadValidation payloadValidation,
    std::string* diagnostics)
{
    const auto parseBegin = std::chrono::steady_clock::now();
    struct ScopedParseTelemetry
    {
        std::chrono::steady_clock::time_point begin;
        size_t byteCount;

        ~ScopedParseTelemetry()
        {
            RecordArtifactLoadTelemetry({
                ArtifactLoadTelemetryStage::NativeContainerParseHash,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - begin),
                byteCount});
        }
    } telemetry {parseBegin, bytes.size()};

    NativeArtifactHeader header;
    if (!ReadHeader(bytes, header, diagnostics) ||
        static_cast<ArtifactType>(header.artifactType) != expectedType ||
        header.schemaVersion != expectedSchemaVersion)
    {
        if (diagnostics && diagnostics->empty())
            *diagnostics = "Native artifact header type or schema does not match the expected artifact.";
        return std::nullopt;
    }

    const auto metadataBegin = bytes.data() + static_cast<size_t>(header.headerSize);
    const std::string metadataText(
        reinterpret_cast<const char*>(metadataBegin),
        static_cast<size_t>(header.metadataSize));
    auto metadata = DeserializeMetadata(metadataText);
    if (!metadata.has_value() ||
        metadata->artifactType != expectedType ||
        metadata->schemaVersion != expectedSchemaVersion)
    {
        if (diagnostics)
            *diagnostics = "Native artifact metadata is invalid or does not match the expected artifact.";
        return std::nullopt;
    }

    const auto payloadData = bytes.data() + static_cast<size_t>(header.payloadOffset);
    const auto payloadSize = static_cast<size_t>(header.payloadSize);
    if (payloadValidation == NativeArtifactPayloadValidation::VerifyHash &&
        metadata->payloadHash != ComputeNativeArtifactPayloadHash(payloadData, payloadSize))
    {
        if (diagnostics)
            *diagnostics = "Native artifact payload hash mismatch.";
        return std::nullopt;
    }
    if (metadata->dependencyHash != ComputeNativeArtifactDependencyHash(metadata->dependencies))
    {
        if (diagnostics)
            *diagnostics = "Native artifact dependency hash mismatch.";
        return std::nullopt;
    }

    RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::NativeArtifactLowCopyView,
        {},
        payloadSize});

    NativeArtifactContainerView view;
    view.metadata = std::move(*metadata);
    view.payloadData = payloadData;
    view.payloadSize = payloadSize;
    return view;
}

bool IsNativeArtifactContainer(const std::vector<uint8_t>& bytes)
{
    if (bytes.size() < sizeof(uint32_t))
        return false;

    size_t offset = 0u;
    uint32_t magic = 0u;
    return ReadUInt32(bytes, offset, magic) && magic == kNativeArtifactMagic;
}

std::optional<NativeArtifactPayloadPrefix> ReadNativeArtifactPayloadPrefix(
    const std::vector<uint8_t>& bytes,
    const ArtifactType expectedType,
    const uint32_t expectedSchemaVersion,
    const size_t prefixSize)
{
    NativeArtifactHeader header;
    if (!ReadHeader(bytes, header) ||
        static_cast<ArtifactType>(header.artifactType) != expectedType ||
        header.schemaVersion != expectedSchemaVersion ||
        header.payloadSize < prefixSize)
    {
        return std::nullopt;
    }

    const auto metadataBegin = bytes.begin() + static_cast<std::ptrdiff_t>(header.headerSize);
    const auto metadataEnd = metadataBegin + static_cast<std::ptrdiff_t>(header.metadataSize);
    const std::string metadataText(metadataBegin, metadataEnd);
    auto metadata = DeserializeMetadata(metadataText);
    if (!metadata.has_value() ||
        metadata->artifactType != expectedType ||
        metadata->schemaVersion != expectedSchemaVersion)
    {
        return std::nullopt;
    }

    NativeArtifactPayloadPrefix result;
    result.metadata = std::move(*metadata);
    result.bytes.resize(prefixSize);
    if (prefixSize > 0u)
    {
        const auto payloadBegin = bytes.begin() + static_cast<std::ptrdiff_t>(header.payloadOffset);
        std::copy(payloadBegin, payloadBegin + static_cast<std::ptrdiff_t>(prefixSize), result.bytes.begin());
    }
    result.payloadSize = header.payloadSize;
    result.payloadOffset = header.payloadOffset;
    return result;
}

std::optional<NativeArtifactPayloadPrefix> ReadNativeArtifactPayloadPrefixFromFile(
    const std::filesystem::path& path,
    const ArtifactType expectedType,
    const uint32_t expectedSchemaVersion,
    const size_t prefixSize,
    const uint64_t maxMetadataBytes)
{
    RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::NativeArtifactFileRead,
        {},
        prefixSize,
        path.generic_string()
    });

    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;

    std::array<uint8_t, kNativeArtifactHeaderSize> headerBytes {};
    input.read(
        reinterpret_cast<char*>(headerBytes.data()),
        static_cast<std::streamsize>(headerBytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(headerBytes.size()))
        return std::nullopt;

    NativeArtifactHeader header;
    std::vector<uint8_t> headerVector(headerBytes.begin(), headerBytes.end());
    if (!ReadHeaderPrefix(headerVector, header) ||
        static_cast<ArtifactType>(header.artifactType) != expectedType ||
        header.schemaVersion != expectedSchemaVersion ||
        header.payloadSize < prefixSize ||
        header.metadataSize > maxMetadataBytes ||
        header.metadataSize > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        header.payloadOffset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()))
    {
        return std::nullopt;
    }

    std::error_code error;
    const auto fileSize = std::filesystem::file_size(path, error);
    if (error ||
        header.payloadOffset > fileSize ||
        header.payloadSize > fileSize - header.payloadOffset ||
        header.payloadOffset != kNativeArtifactHeaderSize + header.metadataSize)
    {
        return std::nullopt;
    }

    std::string metadataText;
    metadataText.resize(static_cast<size_t>(header.metadataSize));
    if (header.metadataSize > 0u)
    {
        input.read(metadataText.data(), static_cast<std::streamsize>(metadataText.size()));
        if (input.gcount() != static_cast<std::streamsize>(metadataText.size()))
            return std::nullopt;
    }

    auto metadata = DeserializeMetadata(metadataText);
    if (!metadata.has_value() ||
        metadata->artifactType != expectedType ||
        metadata->schemaVersion != expectedSchemaVersion)
    {
        return std::nullopt;
    }

    input.seekg(static_cast<std::streamoff>(header.payloadOffset), std::ios::beg);
    if (!input)
        return std::nullopt;

    NativeArtifactPayloadPrefix result;
    result.metadata = std::move(*metadata);
    result.bytes.resize(prefixSize);
    if (prefixSize > 0u)
    {
        input.read(
            reinterpret_cast<char*>(result.bytes.data()),
            static_cast<std::streamsize>(result.bytes.size()));
        if (input.gcount() != static_cast<std::streamsize>(result.bytes.size()))
            return std::nullopt;
    }
    result.payloadSize = header.payloadSize;
    result.payloadOffset = header.payloadOffset;
    return result;
}

std::optional<NativeArtifactPayloadText> ReadNativeArtifactPayloadTextFromFile(
    const std::filesystem::path& path,
    const ArtifactType expectedType,
    const uint32_t expectedSchemaVersion,
    const uint64_t maxMetadataBytes)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;

    std::array<uint8_t, kNativeArtifactHeaderSize> headerBytes {};
    input.read(
        reinterpret_cast<char*>(headerBytes.data()),
        static_cast<std::streamsize>(headerBytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(headerBytes.size()))
        return std::nullopt;

    NativeArtifactHeader header;
    std::vector<uint8_t> headerVector(headerBytes.begin(), headerBytes.end());
    if (!ReadHeaderPrefix(headerVector, header) ||
        static_cast<ArtifactType>(header.artifactType) != expectedType ||
        header.schemaVersion != expectedSchemaVersion ||
        header.metadataSize > maxMetadataBytes ||
        header.metadataSize > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        header.payloadSize > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        header.payloadSize > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()) ||
        header.payloadOffset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()))
    {
        return std::nullopt;
    }

    std::error_code error;
    const auto fileSize = std::filesystem::file_size(path, error);
    if (error ||
        header.payloadOffset > fileSize ||
        header.payloadSize > fileSize - header.payloadOffset ||
        header.payloadOffset != kNativeArtifactHeaderSize + header.metadataSize)
    {
        return std::nullopt;
    }

    RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::NativeArtifactFileRead,
        {},
        static_cast<size_t>(fileSize),
        path.generic_string()
    });

    std::string metadataText;
    metadataText.resize(static_cast<size_t>(header.metadataSize));
    if (header.metadataSize > 0u)
    {
        input.read(metadataText.data(), static_cast<std::streamsize>(metadataText.size()));
        if (input.gcount() != static_cast<std::streamsize>(metadataText.size()))
            return std::nullopt;
    }

    auto metadata = DeserializeMetadata(metadataText);
    if (!metadata.has_value() ||
        metadata->artifactType != expectedType ||
        metadata->schemaVersion != expectedSchemaVersion)
    {
        return std::nullopt;
    }

    std::string payload;
    payload.resize(static_cast<size_t>(header.payloadSize));
    if (header.payloadSize > 0u)
    {
        input.read(payload.data(), static_cast<std::streamsize>(payload.size()));
        if (input.gcount() != static_cast<std::streamsize>(payload.size()))
            return std::nullopt;
    }

    RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::NativeContainerParseHash,
        {},
        payload.size(),
        path.generic_string()
    });

    const auto* payloadData = reinterpret_cast<const uint8_t*>(payload.data());
    if (metadata->payloadHash != ComputeNativeArtifactPayloadHash(payloadData, payload.size()))
        return std::nullopt;
    if (metadata->dependencyHash != ComputeNativeArtifactDependencyHash(metadata->dependencies))
        return std::nullopt;

    NativeArtifactPayloadText result;
    result.metadata = std::move(*metadata);
    result.payload = std::move(payload);
    return result;
}
}
