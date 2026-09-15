#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <Reflection/Type.h>

#include "BinaryBuffer.h"
#include "EngineDef.h"
#include "Serialize/PPtr.h"

namespace NLS::Engine::Serialize
{
using SerializedNodeId = uint32_t;
using SerializedFieldId = uint64_t;

enum class SerializedNodeKind : uint8_t
{
    Generic,
    Integer,
    Boolean,
    Float,
    String,
    Color,
    ObjectReference,
    LayerMask,
    Enum,
    Vector2,
    Vector3,
    Vector4,
    Rect,
    ArraySize,
    Character,
    AnimationCurve,
    Bounds,
    Gradient,
    Quaternion,
    ExposedReference,
    FixedBufferSize,
    Vector2Int,
    Vector3Int,
    RectInt,
    BoundsInt,
    Opaque
};

enum class SerializedNumericKind : uint8_t
{
    Unknown,
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float32,
    Float64,
    Character
};

enum class SerializedNodeFlags : uint32_t
{
    None = 0u,
    Array = 1u << 0u,
    FixedBuffer = 1u << 1u,
    Align4After = 1u << 2u,
    HideInInspector = 1u << 3u,
    ReadOnly = 1u << 4u,
    DebugOnly = 1u << 5u,
    HasObjectReference = 1u << 6u,
    Unsupported = 1u << 7u,
    // The field is currently driven by an animation binding. This is stream
    // schema metadata, not a UI-only flag, so native and managed properties
    // observe the same isAnimated answer.
    Animated = 1u << 8u
};

constexpr SerializedNodeFlags operator|(const SerializedNodeFlags left, const SerializedNodeFlags right)
{
    return static_cast<SerializedNodeFlags>(static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
}

constexpr SerializedNodeFlags operator&(const SerializedNodeFlags left, const SerializedNodeFlags right)
{
    return static_cast<SerializedNodeFlags>(static_cast<uint32_t>(left) & static_cast<uint32_t>(right));
}

constexpr bool HasSerializedNodeFlag(const SerializedNodeFlags value, const SerializedNodeFlags flag)
{
    return (value & flag) != SerializedNodeFlags::None;
}

struct SerializedTypeTreeNode
{
    SerializedNodeId id = 0u;
    SerializedNodeId parent = 0u;
    SerializedNodeId firstChild = 0u;
    SerializedNodeId nextSibling = 0u;
    SerializedFieldId fieldId = 0u;
    std::string serializedName;
    std::vector<std::string> aliases;
    std::string typeName;
    meta::TypeID typeId = meta::InvalidTypeID;
    SerializedNodeKind kind = SerializedNodeKind::Generic;
    SerializedNumericKind numericKind = SerializedNumericKind::Unknown;
    int32_t fixedByteSize = -1;
    SerializedNodeFlags flags = SerializedNodeFlags::None;
};

class NLS_ENGINE_API SerializedTypeTree final
{
public:
    SerializedTypeTree() = default;

    SerializedNodeId AddNode(SerializedTypeTreeNode node);
    void RebuildIndexes();

    [[nodiscard]] const SerializedTypeTreeNode* FindNode(SerializedNodeId id) const;
    [[nodiscard]] const SerializedTypeTreeNode* FindPath(std::string_view path) const;
    [[nodiscard]] const SerializedTypeTreeNode* FindFieldId(SerializedFieldId fieldId) const;
    [[nodiscard]] const SerializedTypeTreeNode* GetParent(SerializedNodeId id) const;
    [[nodiscard]] const SerializedTypeTreeNode* GetFirstChild(SerializedNodeId id) const;
    [[nodiscard]] const SerializedTypeTreeNode* GetNextSibling(SerializedNodeId id) const;
    [[nodiscard]] std::string GetPath(SerializedNodeId id) const;
    [[nodiscard]] uint64_t GetSchemaHash() const { return m_schemaHash; }
    [[nodiscard]] uint64_t GetTypeSignature() const { return m_schemaHash; }
    [[nodiscard]] SerializedNodeId GetRoot() const { return m_root; }
    [[nodiscard]] const std::vector<SerializedTypeTreeNode>& GetNodes() const { return m_nodes; }
    [[nodiscard]] std::vector<SerializedTypeTreeNode>& GetNodes() { return m_nodes; }

private:
    void RebuildNodePath(SerializedNodeId id, const std::string& parentPath);
    void RecomputeHash();

    std::vector<SerializedTypeTreeNode> m_nodes;
    std::unordered_map<std::string, SerializedNodeId> m_pathIndex;
    std::unordered_map<SerializedFieldId, SerializedNodeId> m_fieldIndex;
    std::unordered_map<SerializedNodeId, std::string> m_paths;
    SerializedNodeId m_root = 0u;
    uint64_t m_schemaHash = 0u;
};

struct SerializedReferenceEntry
{
    ObjectIdentifier identifier;
    std::string resourcePath;
    std::string expectedTypeName;
    bool isNull = false;

    friend bool operator==(const SerializedReferenceEntry& left, const SerializedReferenceEntry& right)
    {
        return left.identifier == right.identifier &&
            left.resourcePath == right.resourcePath &&
            left.expectedTypeName == right.expectedTypeName &&
            left.isNull == right.isNull;
    }
};

class NLS_ENGINE_API SerializedReferenceTable final
{
public:
    uint32_t Intern(SerializedReferenceEntry entry);
    [[nodiscard]] const SerializedReferenceEntry* Find(uint32_t slot) const;
    [[nodiscard]] const std::vector<SerializedReferenceEntry>& GetEntries() const { return m_entries; }
    [[nodiscard]] std::vector<SerializedReferenceEntry> CopyEntries() const { return m_entries; }
    void ReplaceEntries(std::vector<SerializedReferenceEntry> entries);

private:
    std::vector<SerializedReferenceEntry> m_entries;
    std::unordered_map<std::string, uint32_t> m_index;
};

struct NLS_ENGINE_API SerializedByteSlice
{
    struct ReferenceRelocation
    {
        uint32_t byteOffset = 0u;
        uint32_t referenceSlot = 0u;

        friend bool operator==(const ReferenceRelocation&, const ReferenceRelocation&) = default;
    };

    std::vector<uint8_t> bytes;
    uint64_t typeSignature = 0u;
    std::vector<SerializedReferenceEntry> references;
    std::vector<ReferenceRelocation> referenceRelocations;
    bool valid = false;

    [[nodiscard]] bool IsValid() const { return valid; }
    [[nodiscard]] bool Equivalent(const SerializedByteSlice& other) const;
};

class NLS_ENGINE_API SerializedByteStream final
{
public:
    SerializedByteStream() = default;
    explicit SerializedByteStream(std::shared_ptr<const SerializedTypeTree> schema);

    [[nodiscard]] bool IsValid() const { return m_schema != nullptr; }
    [[nodiscard]] const std::shared_ptr<const SerializedTypeTree>& GetSchema() const { return m_schema; }
    void SetSchema(std::shared_ptr<const SerializedTypeTree> schema);
    [[nodiscard]] const std::vector<uint8_t>& GetBytes() const { return m_bytes; }
    [[nodiscard]] std::vector<uint8_t>& GetBytes() { return m_bytes; }
    void SetBytes(std::vector<uint8_t> bytes);
    [[nodiscard]] const std::shared_ptr<SerializedReferenceTable>& GetReferences() const { return m_references; }
    void SetReferences(std::shared_ptr<SerializedReferenceTable> references);

    [[nodiscard]] uint64_t GetDataRevision() const { return m_dataRevision; }
    [[nodiscard]] uint64_t GetLayoutRevision() const { return m_layoutRevision; }
    [[nodiscard]] uint64_t GetStructureRevision() const { return m_structureRevision; }
    void MarkDataChanged(bool layoutChanged, bool structureChanged);

    [[nodiscard]] bool Read(size_t offset, void* output, size_t size) const;
    [[nodiscard]] bool Write(size_t offset, const void* input, size_t size);
    [[nodiscard]] bool Replace(size_t offset, size_t length, std::span<const uint8_t> replacement);
    [[nodiscard]] std::optional<SerializedByteSlice> CaptureSlice(
        size_t offset,
        size_t length,
        uint64_t typeSignature) const;

private:
    std::shared_ptr<const SerializedTypeTree> m_schema;
    std::vector<uint8_t> m_bytes;
    std::shared_ptr<SerializedReferenceTable> m_references = std::make_shared<SerializedReferenceTable>();
    uint64_t m_dataRevision = 1u;
    uint64_t m_layoutRevision = 1u;
    uint64_t m_structureRevision = 1u;
};

class NLS_ENGINE_API SerializedByteCursor final
{
public:
    SerializedByteCursor() = default;
    SerializedByteCursor(
        std::shared_ptr<SerializedByteStream> stream,
        SerializedNodeId node,
        size_t offset,
        size_t length,
        std::string path);

    [[nodiscard]] bool IsValid() const;
    [[nodiscard]] SerializedNodeId GetNodeId() const { return m_node; }
    [[nodiscard]] size_t GetByteOffset() const { return m_offset; }
    [[nodiscard]] size_t GetByteSize() const { return m_length; }
    [[nodiscard]] std::string_view GetPath() const { return m_path; }
    [[nodiscard]] bool Read(void* output, size_t size, size_t relativeOffset = 0u) const;
    [[nodiscard]] bool Write(const void* input, size_t size, size_t relativeOffset = 0u);
    [[nodiscard]] std::optional<SerializedByteSlice> Capture(uint64_t typeSignature) const;

private:
    std::shared_ptr<SerializedByteStream> m_stream;
    SerializedNodeId m_node = 0u;
    size_t m_offset = 0u;
    size_t m_length = 0u;
    std::string m_path;
};

struct TypeTreeResult
{
    std::shared_ptr<SerializedTypeTree> tree;
    std::string error;

    [[nodiscard]] bool Succeeded() const { return tree != nullptr && error.empty(); }
};

struct StreamResult
{
    std::shared_ptr<SerializedByteStream> stream;
    std::string error;

    [[nodiscard]] bool Succeeded() const { return stream != nullptr && error.empty(); }
};

struct SerializedTypeTreeBuildOptions
{
    std::string rootName = "root";
    bool includeHidden = false;
};

class NLS_ENGINE_API NativeSerializedStreamCodec final
{
public:
    [[nodiscard]] static std::shared_ptr<SerializedTypeTree> BuildTypeTree(
        const meta::Type& type,
        const SerializedTypeTreeBuildOptions& options = {});

    [[nodiscard]] static bool EncodeVariant(
        const meta::Variant& value,
        const SerializedTypeTree& tree,
        SerializedReferenceTable& references,
        std::vector<uint8_t>& output,
        std::string* error = nullptr,
        size_t alignmentOrigin = 0u);

    [[nodiscard]] static std::optional<meta::Variant> DecodeVariant(
        const std::vector<uint8_t>& bytes,
        const SerializedTypeTree& tree,
        const meta::Type& type,
        const SerializedReferenceTable& references,
        std::string* error = nullptr,
        size_t alignmentOrigin = 0u);

    // Rewrites every object-reference slot in a value subtree into the
    // destination table.  The byte payload remains otherwise unchanged, which
    // lets independently captured Property values be spliced into one stream
    // without colliding local slot numbers.
    [[nodiscard]] static bool RelocateReferenceSlots(
        std::vector<uint8_t>& bytes,
        const SerializedTypeTree& tree,
        const SerializedReferenceTable& sourceReferences,
        SerializedReferenceTable& destinationReferences,
        std::string* error = nullptr);

    [[nodiscard]] static std::optional<SerializedByteCursor> ResolveCursor(
        const std::shared_ptr<SerializedByteStream>& stream,
        std::string_view path);
};
} // namespace NLS::Engine::Serialize
