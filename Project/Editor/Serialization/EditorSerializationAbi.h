#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "Serialization/SerializedObject.h"
#include "Serialization/SerializedObjectHandleRegistry.h"
#include "Serialization/SerializedPropertyHandleRegistry.h"

namespace NLS::Editor::Serialization
{
struct EditorSerializationAbiHeader
{
    uint32_t size = 0u;
    uint32_t abiVersion = 1u;
    uint64_t schemaHash = 0u;
};

// Blittable value carriers used by the Editor-only managed bridge. Keep these
// POD layouts independent from the engine math classes so the ABI remains
// stable if a native math type gains methods or alignment changes.
struct EditorSerializationVector2
{
    float x = 0.0f;
    float y = 0.0f;
};

struct EditorSerializationVector3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct EditorSerializationVector4
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

struct EditorSerializationColor
{
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

struct EditorSerializationRect
{
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct EditorSerializationVector2Int
{
    int32_t x = 0;
    int32_t y = 0;
};

struct EditorSerializationVector3Int
{
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
};

struct EditorSerializationRectInt
{
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
};

struct EditorSerializationBounds
{
    float centerX = 0.0f;
    float centerY = 0.0f;
    float centerZ = 0.0f;
    float sizeX = 0.0f;
    float sizeY = 0.0f;
    float sizeZ = 0.0f;
};

struct EditorSerializationBoundsInt
{
    int32_t positionX = 0;
    int32_t positionY = 0;
    int32_t positionZ = 0;
    int32_t sizeX = 0;
    int32_t sizeY = 0;
    int32_t sizeZ = 0;
};

// Blittable, versioned bridge consumed by the Editor-only managed assembly.
// String access uses a caller-provided buffer and returns the required UTF-8
// byte count; no managed code receives a borrowed native pointer.
struct EditorSerializationApiTable
{
    EditorSerializationAbiHeader header;
    uint8_t (*isObjectValid)(SerializedObjectHandle) = nullptr;
    void (*releaseObject)(SerializedObjectHandle) = nullptr;
    void (*update)(SerializedObjectHandle) = nullptr;
    uint8_t (*updateIfRequiredOrScript)(SerializedObjectHandle) = nullptr;
    uint8_t (*hasModifiedProperties)(SerializedObjectHandle) = nullptr;
    uint8_t (*applyModifiedProperties)(SerializedObjectHandle, uint8_t withoutUndo) = nullptr;
    SerializedPropertyHandle (*findProperty)(SerializedObjectHandle, const char* path) = nullptr;
    uint8_t (*isPropertyValid)(SerializedPropertyHandle) = nullptr;
    void (*releaseProperty)(SerializedPropertyHandle) = nullptr;
    int32_t (*getPropertyType)(SerializedPropertyHandle) = nullptr;
    uint8_t (*getPropertyBool)(SerializedPropertyHandle) = nullptr;
    int64_t (*getPropertyLong)(SerializedPropertyHandle) = nullptr;
    uint64_t (*getPropertyUnsignedLong)(SerializedPropertyHandle) = nullptr;
    double (*getPropertyDouble)(SerializedPropertyHandle) = nullptr;
    uint32_t (*getPropertyString)(SerializedPropertyHandle, char* output, uint32_t capacity) = nullptr;
    uint8_t (*setPropertyBool)(SerializedPropertyHandle, uint8_t value) = nullptr;
    uint8_t (*setPropertyLong)(SerializedPropertyHandle, int64_t value) = nullptr;
    uint8_t (*setPropertyUnsignedLong)(SerializedPropertyHandle, uint64_t value) = nullptr;
    uint8_t (*setPropertyDouble)(SerializedPropertyHandle, double value) = nullptr;
    uint8_t (*setPropertyString)(SerializedPropertyHandle, const char* value) = nullptr;
    uint8_t (*getPropertyVector2)(SerializedPropertyHandle, EditorSerializationVector2* output) = nullptr;
    uint8_t (*getPropertyVector3)(SerializedPropertyHandle, EditorSerializationVector3* output) = nullptr;
    uint8_t (*getPropertyVector4)(SerializedPropertyHandle, EditorSerializationVector4* output) = nullptr;
    uint8_t (*getPropertyQuaternion)(SerializedPropertyHandle, EditorSerializationVector4* output) = nullptr;
    uint8_t (*getPropertyColor)(SerializedPropertyHandle, EditorSerializationColor* output) = nullptr;
    uint8_t (*getPropertyRect)(SerializedPropertyHandle, EditorSerializationRect* output) = nullptr;
    uint8_t (*setPropertyVector2)(SerializedPropertyHandle, float x, float y) = nullptr;
    uint8_t (*setPropertyVector3)(SerializedPropertyHandle, float x, float y, float z) = nullptr;
    uint8_t (*setPropertyVector4)(SerializedPropertyHandle, float x, float y, float z, float w) = nullptr;
    uint8_t (*setPropertyQuaternion)(SerializedPropertyHandle, float x, float y, float z, float w) = nullptr;
    uint8_t (*setPropertyColor)(SerializedPropertyHandle, float r, float g, float b, float a) = nullptr;
    uint8_t (*setPropertyRect)(SerializedPropertyHandle, float x, float y, float width, float height) = nullptr;
    uint8_t (*isPropertyArray)(SerializedPropertyHandle) = nullptr;
    uint32_t (*getPropertyArraySize)(SerializedPropertyHandle) = nullptr;
    uint32_t (*getPropertyMinArraySize)(SerializedPropertyHandle) = nullptr;
    uint32_t (*getPropertyFixedBufferSize)(SerializedPropertyHandle) = nullptr;
    uint8_t (*setPropertyArraySize)(SerializedPropertyHandle, uint32_t value) = nullptr;
    uint8_t (*insertPropertyArrayElement)(SerializedPropertyHandle, uint32_t index) = nullptr;
    uint8_t (*deletePropertyArrayElement)(SerializedPropertyHandle, uint32_t index) = nullptr;
    uint8_t (*movePropertyArrayElement)(SerializedPropertyHandle, uint32_t sourceIndex, uint32_t destinationIndex) = nullptr;
    uint8_t (*clearPropertyArray)(SerializedPropertyHandle) = nullptr;
    int32_t (*getPropertyObjectReferenceInstanceId)(SerializedPropertyHandle) = nullptr;
    uint8_t (*setPropertyObjectReferenceInstanceId)(SerializedPropertyHandle, int32_t instanceId) = nullptr;
    uint8_t (*getPropertyNumericType)(SerializedPropertyHandle) = nullptr;
    uint32_t (*getPropertyName)(SerializedPropertyHandle, char* output, uint32_t capacity) = nullptr;
    uint32_t (*getPropertyDisplayName)(SerializedPropertyHandle, char* output, uint32_t capacity) = nullptr;
    uint32_t (*getPropertyTooltip)(SerializedPropertyHandle, char* output, uint32_t capacity) = nullptr;
    uint32_t (*getPropertyDepth)(SerializedPropertyHandle) = nullptr;
    uint8_t (*getPropertyHasChildren)(SerializedPropertyHandle) = nullptr;
    uint8_t (*getPropertyHasVisibleChildren)(SerializedPropertyHandle) = nullptr;
    uint8_t (*getPropertyEditable)(SerializedPropertyHandle) = nullptr;
    uint8_t (*getPropertyIsExpanded)(SerializedPropertyHandle) = nullptr;
    uint8_t (*setPropertyIsExpanded)(SerializedPropertyHandle, uint8_t expanded) = nullptr;
    uint8_t (*getPropertyHasMultipleDifferentValues)(SerializedPropertyHandle) = nullptr;
    uint8_t (*getPropertyPrefabOverride)(SerializedPropertyHandle) = nullptr;
    uint8_t (*getPropertyDefaultOverride)(SerializedPropertyHandle) = nullptr;
    uint8_t (*getPropertyInstantiatedPrefab)(SerializedPropertyHandle) = nullptr;
    SerializedPropertyHandle (*getIterator)(SerializedObjectHandle) = nullptr;
    SerializedPropertyHandle (*nextProperty)(SerializedPropertyHandle, uint8_t enterChildren, uint8_t visible) = nullptr;
    // the external editor's parameter is includeInvisible. It selects the iterator domain
    // used to find the end sentinel; it does not mean include children.
    SerializedPropertyHandle (*getEndProperty)(SerializedPropertyHandle, uint8_t includeInvisible) = nullptr;
    SerializedPropertyHandle (*copyProperty)(SerializedPropertyHandle) = nullptr;
    uint8_t (*equalPropertyContents)(SerializedPropertyHandle left, SerializedPropertyHandle right) = nullptr;
    uint32_t (*getPropertyPath)(SerializedPropertyHandle, char* output, uint32_t capacity) = nullptr;
    uint32_t (*getTargetCount)(SerializedObjectHandle) = nullptr;
    uint8_t (*getInspectorMode)(SerializedObjectHandle) = nullptr;
    void (*setInspectorMode)(SerializedObjectHandle, uint8_t mode) = nullptr;
    uint32_t (*getMaxArraySizeForMultiEditing)(SerializedObjectHandle) = nullptr;
    uint8_t (*setMaxArraySizeForMultiEditing)(SerializedObjectHandle, uint32_t value) = nullptr;
    void (*setIsDifferentCacheDirty)(SerializedObjectHandle) = nullptr;
    uint8_t (*copyFromSerializedProperty)(SerializedObjectHandle, SerializedPropertyHandle) = nullptr;
    uint8_t (*getPropertyVector2Int)(SerializedPropertyHandle, EditorSerializationVector2Int* output) = nullptr;
    uint8_t (*getPropertyVector3Int)(SerializedPropertyHandle, EditorSerializationVector3Int* output) = nullptr;
    uint8_t (*getPropertyRectInt)(SerializedPropertyHandle, EditorSerializationRectInt* output) = nullptr;
    uint8_t (*getPropertyBounds)(SerializedPropertyHandle, EditorSerializationBounds* output) = nullptr;
    uint8_t (*getPropertyBoundsInt)(SerializedPropertyHandle, EditorSerializationBoundsInt* output) = nullptr;
    uint8_t (*setPropertyVector2Int)(SerializedPropertyHandle, int32_t x, int32_t y) = nullptr;
    uint8_t (*setPropertyVector3Int)(SerializedPropertyHandle, int32_t x, int32_t y, int32_t z) = nullptr;
    uint8_t (*setPropertyRectInt)(SerializedPropertyHandle, int32_t x, int32_t y, int32_t width, int32_t height) = nullptr;
    uint8_t (*setPropertyBounds)(SerializedPropertyHandle, float centerX, float centerY, float centerZ, float sizeX, float sizeY, float sizeZ) = nullptr;
    uint8_t (*setPropertyBoundsInt)(SerializedPropertyHandle, int32_t positionX, int32_t positionY, int32_t positionZ, int32_t sizeX, int32_t sizeY, int32_t sizeZ) = nullptr;
    uint32_t (*getPropertyEnumDisplayNames)(SerializedPropertyHandle, char* output, uint32_t capacity) = nullptr;
    // Variable-length native payloads for the three external editor special values. The
    // getter is two-pass (output=nullptr, capacity=0 returns byte count); the
    // setter validates the complete payload before staging it.
    uint32_t (*getPropertyAnimationCurve)(SerializedPropertyHandle, uint8_t* output, uint32_t capacity) = nullptr;
    uint8_t (*setPropertyAnimationCurve)(SerializedPropertyHandle, const uint8_t* input, uint32_t size) = nullptr;
    uint32_t (*getPropertyGradient)(SerializedPropertyHandle, uint8_t* output, uint32_t capacity) = nullptr;
    uint8_t (*setPropertyGradient)(SerializedPropertyHandle, const uint8_t* input, uint32_t size) = nullptr;
    uint32_t (*getPropertyExposedReference)(SerializedPropertyHandle, uint8_t* output, uint32_t capacity) = nullptr;
    uint8_t (*setPropertyExposedReference)(SerializedPropertyHandle, const uint8_t* input, uint32_t size) = nullptr;
    // Append-only ABI extensions for the remaining editor-compatible managed
    // surface. Existing fields stay byte-for-byte stable for older hosts.
    uint8_t (*copyFromSerializedPropertyIfDifferent)(SerializedObjectHandle, SerializedPropertyHandle) = nullptr;
    uint32_t (*getPropertyTypeName)(SerializedPropertyHandle, char* output, uint32_t capacity) = nullptr;
    uint64_t (*getPropertyExactTypeId)(SerializedPropertyHandle) = nullptr;
    // Retains its original ABI shape for older managed hosts. The bool is
    // ignored: the external editor's public CountInProperty() has no traversal parameter.
    uint32_t (*getPropertyCountInProperty)(SerializedPropertyHandle, uint8_t reserved) = nullptr;
    uint32_t (*getPropertyCountRemaining)(SerializedPropertyHandle) = nullptr;
    uint8_t (*getPropertyReadOnly)(SerializedPropertyHandle) = nullptr;
    int32_t (*getTargetObjectInstanceId)(SerializedObjectHandle, uint32_t index) = nullptr;
    uint32_t (*getObjectLastError)(SerializedObjectHandle, char* output, uint32_t capacity) = nullptr;
    SerializedObjectHandle (*createObjectFromInstanceIds)(const int32_t* instanceIds, uint32_t count) = nullptr;
    void (*discardModifiedProperties)(SerializedObjectHandle) = nullptr;
    // Append-only context/ExposedReference APIs. A context is an editor-only
    // stable target used by ExposedReference name resolution.
    int32_t (*getContextObjectInstanceId)(SerializedObjectHandle) = nullptr;
    int32_t (*getResolvedExposedReferenceInstanceId)(SerializedPropertyHandle) = nullptr;
    SerializedObjectHandle (*createObjectFromInstanceIdsWithContext)(
        const int32_t* instanceIds,
        uint32_t count,
        int32_t contextInstanceId) = nullptr;
    void (*resetProperty)(SerializedPropertyHandle) = nullptr;
    uint8_t (*dataEquals)(SerializedPropertyHandle left, SerializedPropertyHandle right) = nullptr;
    uint8_t (*deletePropertyCommand)(SerializedPropertyHandle) = nullptr;
    uint8_t (*duplicatePropertyCommand)(SerializedPropertyHandle) = nullptr;
    // Kept separate from arraySize so callers can inspect an empty array without
    // manufacturing element zero.  This is append-only: older managed hosts
    // simply observe a smaller table and leave the delegate null.
    uint32_t (*getPropertyArrayElementType)(SerializedPropertyHandle, char* output, uint32_t capacity) = nullptr;
    // the external editor's exposedReferenceValue is resolver-backed when SerializedObject
    // has a context. This setter writes that context table (or stages the raw
    // default when no resolver is present) using an instance id only at the ABI
    // boundary; native code immediately converts it to a durable target handle.
    uint8_t (*setResolvedExposedReferenceInstanceId)(SerializedPropertyHandle, int32_t instanceId) = nullptr;
    // Append-only editor-compatible property state. Older managed hosts copy a
    // smaller table and simply observe a null delegate.
    uint8_t (*getPropertyIsAnimated)(SerializedPropertyHandle) = nullptr;
};

[[nodiscard]] const EditorSerializationApiTable& GetEditorSerializationApiTable();

// The Editor host registers a shared canonical object before exposing its
// managed handle. The returned value is zero for null objects.
[[nodiscard]] SerializedObjectHandle RegisterEditorSerializedObject(
    std::shared_ptr<SerializedObject> object);
} // namespace NLS::Editor::Serialization
