#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <Math/Color.h>
#include <Math/Bounds.h>
#include <Math/BoundsInt.h>
#include <Math/Quaternion.h>
#include <Math/Rect.h>
#include <Math/RectInt.h>
#include <Math/Vector2.h>
#include <Math/Vector2Int.h>
#include <Math/Vector3.h>
#include <Math/Vector3Int.h>
#include <Math/Vector4.h>
#include <Reflection/Variant.h>

#include "Serialization/SerializedObject.h"
#include "Serialize/SerializedSpecialTypes.h"

namespace NLS::Editor::Serialization
{
// the established editor baseline's public enum values are part of the managed API contract.
// Keep these values stable even when Nullus adds a richer native type id.
enum class SerializedPropertyType : int32_t
{
    Generic = -1,
    Integer = 0,
    Boolean = 1,
    Float = 2,
    String = 3,
    Color = 4,
    ObjectReference = 5,
    LayerMask = 6,
    Enum = 7,
    Vector2 = 8,
    Vector3 = 9,
    Vector4 = 10,
    Rect = 11,
    ArraySize = 12,
    Character = 13,
    AnimationCurve = 14,
    Bounds = 15,
    Gradient = 16,
    Quaternion = 17,
    ExposedReference = 18,
    FixedBufferSize = 19,
    Vector2Int = 20,
    Vector3Int = 21,
    RectInt = 22,
    BoundsInt = 23
};

static_assert(static_cast<int32_t>(SerializedPropertyType::Generic) == -1);
static_assert(static_cast<int32_t>(SerializedPropertyType::Integer) == 0);
static_assert(static_cast<int32_t>(SerializedPropertyType::Boolean) == 1);
static_assert(static_cast<int32_t>(SerializedPropertyType::Float) == 2);
static_assert(static_cast<int32_t>(SerializedPropertyType::String) == 3);
static_assert(static_cast<int32_t>(SerializedPropertyType::BoundsInt) == 23);

enum class SerializedNumericType : uint8_t
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
    Float,
    Double,
    Char
};

// A Property is a cursor into an owning SerializedObject. It deliberately has
// no target pointer. The owner keeps the schema and staged values alive while
// the generation fields detect stale paths after a structural refresh.
class SerializedProperty final
{
public:
    SerializedProperty() = default;

    [[nodiscard]] bool IsValid() const;
    [[nodiscard]] explicit operator bool() const { return IsValid(); }

    [[nodiscard]] std::string_view GetPropertyPath() const;
    [[nodiscard]] std::string_view GetName() const;
    [[nodiscard]] std::string_view GetDisplayName() const;
    [[nodiscard]] std::string_view GetTooltip() const;
    [[nodiscard]] SerializedPropertyType GetPropertyType() const;
    [[nodiscard]] SerializedNumericType GetNumericType() const;
    [[nodiscard]] std::string_view GetExactTypeName() const;
    [[nodiscard]] uint64_t GetExactTypeId() const;
    [[nodiscard]] size_t GetDepth() const;
    [[nodiscard]] bool IsExpanded() const;
    void SetExpanded(bool expanded) const;
    [[nodiscard]] bool IsEditable() const;
    // the external editor's isAnimated surface. The bit belongs to the canonical TypeTree so
    // a future animation backend can mark driven fields without teaching the
    // Inspector about animation state.
    [[nodiscard]] bool IsAnimated() const;
    [[nodiscard]] bool HasChildren() const;
    [[nodiscard]] bool HasVisibleChildren() const;
    [[nodiscard]] bool IsArray() const;
    [[nodiscard]] std::string GetArrayElementType() const;
    [[nodiscard]] size_t GetArraySize() const;
    [[nodiscard]] size_t GetMinArraySize() const;
    [[nodiscard]] std::vector<size_t> GetArraySizes() const;
    bool SetArrayOrder(std::span<const size_t> order) const;
    bool RemoveArrayElements(std::span<const size_t> indices) const;
    bool DuplicateArrayElements(std::span<const size_t> indices) const;
    bool AppendArrayValues(std::span<const meta::Variant> values) const;
    [[nodiscard]] bool IsFixedBuffer() const;
    [[nodiscard]] size_t GetFixedBufferSize() const;
    // Fixed buffers expose a fixed element count and are not resizable; the
    // element cursor still resolves through the same .Array.data[i] path.
    [[nodiscard]] SerializedProperty GetFixedBufferElementAtIndex(size_t index) const;
    [[nodiscard]] bool HasMultipleDifferentValues() const;
    [[nodiscard]] bool IsPrefabOverride() const;
    [[nodiscard]] bool IsDefaultOverride() const;
    [[nodiscard]] bool IsInstantiatedPrefab() const;

    [[nodiscard]] meta::Variant GetValue() const;
    [[nodiscard]] bool SetValue(const meta::Variant& value) const;
    [[nodiscard]] bool SetValueComponents(const meta::Variant& value,uint64_t components) const;
    [[nodiscard]] std::vector<meta::Variant> GetTargetValues() const;

    // engine-style scalar spellings retained in the native API. The Get*/Set*
    // forms above are the canonical C++ names; these aliases make custom
    // editors portable without introducing another property implementation.
    [[nodiscard]] int GetIntValue() const { return static_cast<int>(GetLongValue()); }
    bool SetIntValue(int value) const { return SetLongValue(value); }
    [[nodiscard]] bool GetBooleanValue() const { return GetBoolValue(); }
    bool SetBooleanValue(bool value) const { return SetBoolValue(value); }

    [[nodiscard]] bool GetBoolValue() const;
    [[nodiscard]] int64_t GetLongValue() const;
    [[nodiscard]] uint64_t GetUnsignedLongValue() const;
    [[nodiscard]] double GetDoubleValue() const;
    [[nodiscard]] float GetFloatValue() const;
    [[nodiscard]] std::string GetStringValue() const;
    [[nodiscard]] Maths::Vector2 GetVector2Value() const;
    [[nodiscard]] Maths::Vector3 GetVector3Value() const;
    [[nodiscard]] Maths::Vector4 GetVector4Value() const;
    [[nodiscard]] Maths::Quaternion GetQuaternionValue() const;
    [[nodiscard]] Maths::Color GetColorValue() const;
    [[nodiscard]] Maths::Rect GetRectValue() const;
    [[nodiscard]] Maths::Vector2Int GetVector2IntValue() const;
    [[nodiscard]] Maths::Vector3Int GetVector3IntValue() const;
    [[nodiscard]] Maths::RectInt GetRectIntValue() const;
    [[nodiscard]] Maths::Bounds GetBoundsValue() const;
    [[nodiscard]] Maths::BoundsInt GetBoundsIntValue() const;
    [[nodiscard]] Engine::Serialize::AnimationCurve GetAnimationCurveValue() const;
    [[nodiscard]] Engine::Serialize::Gradient GetGradientValue() const;
    [[nodiscard]] Engine::Serialize::ExposedReference GetExposedReferenceValue() const;
    [[nodiscard]] int GetEnumValueIndex() const;
    [[nodiscard]] std::vector<std::string> GetEnumDisplayNames() const;

    bool SetBoolValue(bool value) const;
    bool SetLongValue(int64_t value) const;
    bool SetUnsignedLongValue(uint64_t value) const;
    bool SetDoubleValue(double value) const;
    bool SetFloatValue(float value) const;
    bool SetStringValue(std::string_view value) const;
    bool SetVector2Value(const Maths::Vector2& value) const;
    bool SetVector3Value(const Maths::Vector3& value) const;
    bool SetVector4Value(const Maths::Vector4& value) const;
    bool SetQuaternionValue(const Maths::Quaternion& value) const;
    bool SetColorValue(const Maths::Color& value) const;
    bool SetRectValue(const Maths::Rect& value) const;
    bool SetVector2IntValue(const Maths::Vector2Int& value) const;
    bool SetVector3IntValue(const Maths::Vector3Int& value) const;
    bool SetRectIntValue(const Maths::RectInt& value) const;
    bool SetBoundsValue(const Maths::Bounds& value) const;
    bool SetBoundsIntValue(const Maths::BoundsInt& value) const;
    bool SetAnimationCurveValue(const Engine::Serialize::AnimationCurve& value) const;
    bool SetGradientValue(const Engine::Serialize::Gradient& value) const;
    bool SetExposedReferenceValue(const Engine::Serialize::ExposedReference& value) const;
    bool SetEnumValueIndex(int value) const;

    [[nodiscard]] SerializedProperty FindPropertyRelative(std::string_view relativePath) const;
    [[nodiscard]] SerializedProperty GetArrayElementAtIndex(size_t index) const;
    bool InsertArrayElementAtIndex(size_t index) const;
    bool DeleteArrayElementAtIndex(size_t index) const;
    bool MoveArrayElement(size_t sourceIndex, size_t destinationIndex) const;
    bool ClearArray() const;
    bool SetArraySize(size_t size) const;

    // engine-style depth-first iterator operations.
    [[nodiscard]] SerializedProperty Copy() const;
    void Reset() const;
    bool Next(bool enterChildren = true) const;
    bool NextVisible(bool enterChildren = true) const;
    // Matches the established editor baseline: false advances through visible properties only;
    // true also considers hidden/debug-only properties when finding the end
    // sentinel. It never means "include children".
    [[nodiscard]] SerializedProperty GetEndProperty(bool includeInvisible = false) const;
    // Matches the established editor baseline CountInProperty(): visible descendants plus this
    // property. Traversal policy is intentionally not part of the public API.
    [[nodiscard]] size_t CountInProperty() const;
    [[nodiscard]] size_t CountRemaining() const;
    [[nodiscard]] static bool EqualContents(
        const SerializedProperty& left,
        const SerializedProperty& right);
    [[nodiscard]] static bool DataEquals(
        const SerializedProperty& left,
        const SerializedProperty& right);
    bool DeleteCommand() const;
    bool DuplicateCommand() const;

    [[nodiscard]] const Inspector::EditorPropertyMetadata& GetMetadata() const;
    [[nodiscard]] Inspector::PropertyValueState GetValueState() const;
    [[nodiscard]] size_t GetTargetCount() const;
    [[nodiscard]] bool IsReadOnly() const { return !IsEditable(); }

private:
    friend class SerializedObject;

    [[nodiscard]] bool SyncStreamCursor() const;

    SerializedProperty(
        std::shared_ptr<SerializedObject::State> owner,
        std::string path,
        uint64_t schemaGeneration,
        uint64_t structureGeneration,
        size_t iteratorIndex = 0u,
        Engine::Serialize::SerializedNodeId streamNodeId = 0u)
        : m_owner(std::move(owner))
        , m_path(std::move(path))
        , m_schemaGeneration(schemaGeneration)
        , m_structureGeneration(structureGeneration)
        , m_iteratorIndex(iteratorIndex)
        , m_streamNodeId(streamNodeId)
    {
    }

    std::shared_ptr<SerializedObject::State> m_owner;
    mutable std::string m_path;
    mutable std::string m_exactTypeNameCache;
    uint64_t m_schemaGeneration = 0u;
    uint64_t m_structureGeneration = 0u;
    mutable Engine::Serialize::SerializedNodeId m_streamNodeId = 0u;
    mutable size_t m_cachedByteOffset = 0u;
    mutable uint64_t m_observedLayoutRevision = 0u;
    mutable uint64_t m_observedStreamStructureRevision = 0u;
    mutable size_t m_iteratorIndex = 0u;
};
} // namespace NLS::Editor::Serialization
