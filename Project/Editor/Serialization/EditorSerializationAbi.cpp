#include "Serialization/EditorSerializationAbi.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

#include <Object/Object.h>
#include <GameObject.h>

#include "Serialization/NativeReflectionSerializedBackend.h"
#include "Serialization/SerializedObject.h"
#include "Serialization/SerializedPropertyHandleRegistry.h"
#include "Serialize/BinaryBuffer.h"
#include "Serialize/NativeSerializedStream.h"
#include "Serialize/SerializedSpecialTypes.h"

namespace NLS::Editor::Serialization
{
namespace
{
SerializedObjectHandleRegistry& ObjectRegistry()
{
    static SerializedObjectHandleRegistry registry;
    return registry;
}

SerializedPropertyHandleRegistry& PropertyRegistry()
{
    static SerializedPropertyHandleRegistry registry;
    return registry;
}

std::shared_ptr<SerializedObject> ResolveObject(const SerializedObjectHandle handle)
{
    return ObjectRegistry().Resolve(handle);
}

SerializedProperty ResolveProperty(
    const SerializedPropertyHandle handle,
    std::shared_ptr<SerializedObject>& object)
{
    SerializedObjectHandle objectHandle = 0u;
    std::string path;
    if (!PropertyRegistry().Resolve(handle, objectHandle, path))
        return {};
    object = ResolveObject(objectHandle);
    if (object == nullptr)
        return {};
    // Empty paths are the stream-root iterator sentinel. A zero handle remains
    // the end-property marker; callers never register an empty end cursor.
    return path.empty() ? object->GetIterator() : object->FindProperty(path);
}

template <typename T>
std::optional<std::vector<uint8_t>> EncodeSpecialValue(const T& value)
{
    const auto schema = NLS::Engine::Serialize::NativeSerializedStreamCodec::BuildTypeTree(NLS_TYPEOF(T), {"value", false});
    if (schema == nullptr)
        return std::nullopt;
    NLS::Engine::Serialize::SerializedReferenceTable references;
    std::vector<uint8_t> bytes;
    std::string error;
    if (!NLS::Engine::Serialize::NativeSerializedStreamCodec::EncodeVariant(
            meta::Variant(value),
            *schema,
            references,
            bytes,
            &error) || !references.GetEntries().empty())
    {
        // AnimationCurve and Gradient intentionally contain no object
        // references. If a future schema grows one, keep this ABI deterministic
        // rather than silently dropping its reference table.
        return std::nullopt;
    }
    return bytes;
}

template <typename T>
std::optional<T> DecodeSpecialValue(const uint8_t* input, const uint32_t size)
{
    if (input == nullptr && size != 0u)
        return std::nullopt;
    const auto schema = NLS::Engine::Serialize::NativeSerializedStreamCodec::BuildTypeTree(NLS_TYPEOF(T), {"value", false});
    if (schema == nullptr)
        return std::nullopt;
    std::vector<uint8_t> bytes;
    if (size != 0u)
        bytes.assign(input, input + size);
    NLS::Engine::Serialize::SerializedReferenceTable references;
    std::string error;
    const auto decoded = NLS::Engine::Serialize::NativeSerializedStreamCodec::DecodeVariant(
        bytes,
        *schema,
        NLS_TYPEOF(T),
        references,
        &error);
    if (!decoded.has_value() || !decoded->IsValid())
        return std::nullopt;
    return decoded->GetValue<T>();
}

std::vector<uint8_t> EncodeExposedReferenceValue(const Engine::Serialize::ExposedReference& value)
{
    NLS::Engine::Serialize::BinaryBufferWriter writer;
    writer.WriteI32(static_cast<int32_t>(value.defaultValue.GetInstanceID()));
    writer.WriteAlignedString(value.exposedName);
    return std::move(writer.bytes);
}

std::optional<Engine::Serialize::ExposedReference> DecodeExposedReferenceValue(
    const uint8_t* input,
    const uint32_t size)
{
    if (input == nullptr && size != 0u)
        return std::nullopt;
    NLS::Engine::Serialize::BinaryBufferReader reader(input, size);
    int32_t instanceId = NLS::Engine::Serialize::InstanceID_None;
    std::string exposedName;
    if (!reader.ReadI32(instanceId) || !reader.ReadAlignedString(exposedName) || !reader.IsComplete())
        return std::nullopt;
    Engine::Serialize::ExposedReference value;
    value.defaultValue.SetInstanceID(instanceId);
    value.exposedName = std::move(exposedName);
    return value;
}

uint32_t CopyBytes(
    const std::vector<uint8_t>& bytes,
    uint8_t* output,
    const uint32_t capacity)
{
    const auto required = (std::min)(bytes.size(), static_cast<size_t>((std::numeric_limits<uint32_t>::max)()));
    if (output != nullptr && capacity != 0u)
    {
        const auto copySize = (std::min)(bytes.size(), static_cast<size_t>(capacity));
        if (copySize != 0u)
            std::memcpy(output, bytes.data(), copySize);
    }
    return static_cast<uint32_t>(required);
}

uint8_t IsObjectValid(const SerializedObjectHandle handle)
{
    const auto object = ResolveObject(handle);
    return object != nullptr && object->IsValid() ? 1u : 0u;
}

void ReleaseObject(const SerializedObjectHandle handle)
{
    PropertyRegistry().InvalidateObject(handle);
    static_cast<void>(ObjectRegistry().Release(handle));
}

void UpdateObject(const SerializedObjectHandle handle)
{
    if (const auto object = ResolveObject(handle))
        object->Update();
}

uint8_t UpdateIfRequired(const SerializedObjectHandle handle)
{
    const auto object = ResolveObject(handle);
    return object != nullptr && object->UpdateIfRequiredOrScript() ? 1u : 0u;
}

uint8_t HasModified(const SerializedObjectHandle handle)
{
    const auto object = ResolveObject(handle);
    return object != nullptr && object->HasModifiedProperties() ? 1u : 0u;
}

uint8_t ApplyObject(
    const SerializedObjectHandle handle,
    const uint8_t withoutUndo)
{
    const auto object = ResolveObject(handle);
    if (object == nullptr)
        return 0u;
    return (withoutUndo != 0u
        ? object->ApplyModifiedPropertiesWithoutUndo()
        : object->ApplyModifiedProperties()) ? 1u : 0u;
}

SerializedPropertyHandle FindProperty(
    const SerializedObjectHandle objectHandle,
    const char* path)
{
    if (path == nullptr)
        return 0u;
    const auto object = ResolveObject(objectHandle);
    if (object == nullptr)
        return 0u;
    const auto property = object->FindProperty(path);
    return property ? PropertyRegistry().Register(objectHandle, path) : 0u;
}

uint8_t IsPropertyValid(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && object != nullptr ? 1u : 0u;
}

void ReleaseProperty(const SerializedPropertyHandle handle)
{
    static_cast<void>(PropertyRegistry().Release(handle));
}

int32_t GetPropertyType(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property ? static_cast<int32_t>(property.GetPropertyType()) :
        static_cast<int32_t>(SerializedPropertyType::Generic);
}

uint8_t GetPropertyBool(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.GetBoolValue() ? 1u : 0u;
}

int64_t GetPropertyLong(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property ? property.GetLongValue() : 0;
}

uint64_t GetPropertyUnsignedLong(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property ? property.GetUnsignedLongValue() : 0u;
}

double GetPropertyDouble(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property ? property.GetDoubleValue() : 0.0;
}

uint32_t GetPropertyString(
    const SerializedPropertyHandle handle,
    char* output,
    const uint32_t capacity)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    const auto value = property ? property.GetStringValue() : std::string {};
    if (output != nullptr && capacity > 0u)
    {
        const auto copySize = (std::min)(
            value.size(),
            static_cast<size_t>(capacity - 1u));
        std::memcpy(output, value.data(), copySize);
        output[copySize] = '\0';
    }
    return static_cast<uint32_t>(value.size());
}

uint8_t SetPropertyBool(
    const SerializedPropertyHandle handle,
    const uint8_t value)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.SetBoolValue(value != 0u) ? 1u : 0u;
}

uint8_t SetPropertyLong(
    const SerializedPropertyHandle handle,
    const int64_t value)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.SetLongValue(value) ? 1u : 0u;
}

uint8_t SetPropertyUnsignedLong(
    const SerializedPropertyHandle handle,
    const uint64_t value)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.SetUnsignedLongValue(value) ? 1u : 0u;
}

uint8_t SetPropertyDouble(
    const SerializedPropertyHandle handle,
    const double value)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.SetDoubleValue(value) ? 1u : 0u;
}

uint8_t SetPropertyString(
    const SerializedPropertyHandle handle,
    const char* value)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.SetStringValue(value != nullptr ? value : "") ? 1u : 0u;
}

uint8_t GetPropertyVector2(
    const SerializedPropertyHandle handle,
    EditorSerializationVector2* output)
{
    if (output == nullptr)
        return 0u;
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property)
        return 0u;
    const auto value = property.GetVector2Value();
    output->x = value.x;
    output->y = value.y;
    return 1u;
}

uint8_t GetPropertyVector3(
    const SerializedPropertyHandle handle,
    EditorSerializationVector3* output)
{
    if (output == nullptr)
        return 0u;
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property)
        return 0u;
    const auto value = property.GetVector3Value();
    output->x = value.x;
    output->y = value.y;
    output->z = value.z;
    return 1u;
}

uint8_t GetPropertyVector4(
    const SerializedPropertyHandle handle,
    EditorSerializationVector4* output)
{
    if (output == nullptr)
        return 0u;
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property)
        return 0u;
    const auto value = property.GetVector4Value();
    output->x = value.x;
    output->y = value.y;
    output->z = value.z;
    output->w = value.w;
    return 1u;
}

uint8_t GetPropertyQuaternion(
    const SerializedPropertyHandle handle,
    EditorSerializationVector4* output)
{
    if (output == nullptr)
        return 0u;
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property)
        return 0u;
    const auto value = property.GetQuaternionValue();
    output->x = value.x;
    output->y = value.y;
    output->z = value.z;
    output->w = value.w;
    return 1u;
}

uint8_t GetPropertyColor(
    const SerializedPropertyHandle handle,
    EditorSerializationColor* output)
{
    if (output == nullptr)
        return 0u;
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property)
        return 0u;
    const auto value = property.GetColorValue();
    output->r = value.r;
    output->g = value.g;
    output->b = value.b;
    output->a = value.a;
    return 1u;
}

uint8_t GetPropertyRect(
    const SerializedPropertyHandle handle,
    EditorSerializationRect* output)
{
    if (output == nullptr)
        return 0u;
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property)
        return 0u;
    const auto value = property.GetRectValue();
    output->x = value.x;
    output->y = value.y;
    output->width = value.width;
    output->height = value.height;
    return 1u;
}

uint8_t SetPropertyVector2(
    const SerializedPropertyHandle handle,
    const float x,
    const float y)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.SetVector2Value({x, y}) ? 1u : 0u;
}

uint8_t SetPropertyVector3(
    const SerializedPropertyHandle handle,
    const float x,
    const float y,
    const float z)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.SetVector3Value({x, y, z}) ? 1u : 0u;
}

uint8_t SetPropertyVector4(
    const SerializedPropertyHandle handle,
    const float x,
    const float y,
    const float z,
    const float w)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.SetVector4Value({x, y, z, w}) ? 1u : 0u;
}

uint8_t SetPropertyQuaternion(
    const SerializedPropertyHandle handle,
    const float x,
    const float y,
    const float z,
    const float w)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.SetQuaternionValue({x, y, z, w}) ? 1u : 0u;
}

uint8_t SetPropertyColor(
    const SerializedPropertyHandle handle,
    const float r,
    const float g,
    const float b,
    const float a)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.SetColorValue({r, g, b, a}) ? 1u : 0u;
}

uint8_t SetPropertyRect(
    const SerializedPropertyHandle handle,
    const float x,
    const float y,
    const float width,
    const float height)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.SetRectValue({x, y, width, height}) ? 1u : 0u;
}

uint8_t IsPropertyArray(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.IsArray() ? 1u : 0u;
}

uint32_t GetPropertyArraySize(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property)
        return 0u;
    return static_cast<uint32_t>((std::min)(property.GetArraySize(), static_cast<size_t>((std::numeric_limits<uint32_t>::max)())));
}

uint32_t GetPropertyMinArraySize(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property)
        return 0u;
    return static_cast<uint32_t>((std::min)(property.GetMinArraySize(), static_cast<size_t>((std::numeric_limits<uint32_t>::max)())));
}

uint32_t GetPropertyFixedBufferSize(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property)
        return 0u;
    return static_cast<uint32_t>((std::min)(property.GetFixedBufferSize(), static_cast<size_t>((std::numeric_limits<uint32_t>::max)())));
}

uint8_t SetPropertyArraySize(
    const SerializedPropertyHandle handle,
    const uint32_t value)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.SetArraySize(value) ? 1u : 0u;
}

uint8_t InsertPropertyArrayElement(
    const SerializedPropertyHandle handle,
    const uint32_t index)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.InsertArrayElementAtIndex(index) ? 1u : 0u;
}

uint8_t DeletePropertyArrayElement(
    const SerializedPropertyHandle handle,
    const uint32_t index)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.DeleteArrayElementAtIndex(index) ? 1u : 0u;
}

uint8_t MovePropertyArrayElement(
    const SerializedPropertyHandle handle,
    const uint32_t sourceIndex,
    const uint32_t destinationIndex)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.MoveArrayElement(sourceIndex, destinationIndex) ? 1u : 0u;
}

uint8_t ClearPropertyArray(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.ClearArray() ? 1u : 0u;
}

template <typename TGetter>
uint32_t CopyPropertyText(
    const SerializedPropertyHandle handle,
    char* output,
    const uint32_t capacity,
    TGetter&& getter)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    const std::string value = property ? std::string(getter(property)) : std::string {};
    if (output != nullptr && capacity > 0u)
    {
        const auto copySize = (std::min)(
            value.size(),
            static_cast<size_t>(capacity - 1u));
        std::memcpy(output, value.data(), copySize);
        output[copySize] = '\0';
    }
    return static_cast<uint32_t>((std::min)(
        value.size(),
        static_cast<size_t>((std::numeric_limits<uint32_t>::max)())));
}

uint32_t CopyPropertyTextValue(
    const std::string& value,
    char* output,
    const uint32_t capacity)
{
    if (output != nullptr && capacity > 0u)
    {
        const auto copySize = (std::min)(
            value.size(),
            static_cast<size_t>(capacity - 1u));
        std::memcpy(output, value.data(), copySize);
        output[copySize] = '\0';
    }
    return static_cast<uint32_t>((std::min)(
        value.size(),
        static_cast<size_t>((std::numeric_limits<uint32_t>::max)())));
}

uint8_t GetPropertyNumericType(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property ? static_cast<uint8_t>(property.GetNumericType()) : 0u;
}

uint32_t GetPropertyName(
    const SerializedPropertyHandle handle,
    char* output,
    const uint32_t capacity)
{
    return CopyPropertyText(handle, output, capacity, [](const SerializedProperty& property)
    {
        return property.GetName();
    });
}

uint32_t GetPropertyDisplayName(
    const SerializedPropertyHandle handle,
    char* output,
    const uint32_t capacity)
{
    return CopyPropertyText(handle, output, capacity, [](const SerializedProperty& property)
    {
        return property.GetDisplayName();
    });
}

uint32_t GetPropertyTooltip(
    const SerializedPropertyHandle handle,
    char* output,
    const uint32_t capacity)
{
    return CopyPropertyText(handle, output, capacity, [](const SerializedProperty& property)
    {
        return property.GetTooltip();
    });
}

uint32_t GetPropertyDepth(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property)
        return 0u;
    return static_cast<uint32_t>((std::min)(
        property.GetDepth(),
        static_cast<size_t>((std::numeric_limits<uint32_t>::max)())));
}

uint8_t GetPropertyHasChildren(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.HasChildren() ? 1u : 0u;
}

uint8_t GetPropertyHasVisibleChildren(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.HasVisibleChildren() ? 1u : 0u;
}

uint8_t GetPropertyEditable(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.IsEditable() ? 1u : 0u;
}

uint8_t GetPropertyIsExpanded(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.IsExpanded() ? 1u : 0u;
}

uint8_t SetPropertyIsExpanded(
    const SerializedPropertyHandle handle,
    const uint8_t expanded)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property)
        return 0u;
    property.SetExpanded(expanded != 0u);
    return 1u;
}

uint8_t GetPropertyHasMultipleDifferentValues(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.HasMultipleDifferentValues() ? 1u : 0u;
}

uint8_t GetPropertyPrefabOverride(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.IsPrefabOverride() ? 1u : 0u;
}

uint8_t GetPropertyDefaultOverride(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.IsDefaultOverride() ? 1u : 0u;
}

uint8_t GetPropertyInstantiatedPrefab(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.IsInstantiatedPrefab() ? 1u : 0u;
}

SerializedPropertyHandle GetIterator(const SerializedObjectHandle objectHandle)
{
    const auto object = ResolveObject(objectHandle);
    if (object == nullptr)
        return 0u;
    const auto property = object->GetIterator();
    return property
        ? PropertyRegistry().Register(objectHandle, std::string(property.GetPropertyPath()))
        : 0u;
}

SerializedPropertyHandle NextProperty(
    const SerializedPropertyHandle handle,
    const uint8_t enterChildren,
    const uint8_t visible)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    SerializedObjectHandle objectHandle = 0u;
    std::string path;
    if (!property || !PropertyRegistry().Resolve(handle, objectHandle, path))
        return 0u;

    auto next = property;
    const bool advanced = visible != 0u
        ? next.NextVisible(enterChildren != 0u)
        : next.Next(enterChildren != 0u);
    if (!advanced || next.GetPropertyPath().empty())
        return 0u;
    return PropertyRegistry().Register(objectHandle, std::string(next.GetPropertyPath()));
}

SerializedPropertyHandle GetEndProperty(
    const SerializedPropertyHandle handle,
    const uint8_t includeInvisible)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    SerializedObjectHandle objectHandle = 0u;
    std::string path;
    if (!property || !PropertyRegistry().Resolve(handle, objectHandle, path))
        return 0u;
    const auto end = property.GetEndProperty(includeInvisible != 0u);
    const auto endPath = std::string(end.GetPropertyPath());
    return endPath.empty() ? 0u : PropertyRegistry().Register(objectHandle, endPath);
}

SerializedPropertyHandle CopyProperty(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    SerializedObjectHandle objectHandle = 0u;
    std::string path;
    if (!property || !PropertyRegistry().Resolve(handle, objectHandle, path))
        return 0u;
    const auto copy = property.Copy();
    return copy ? PropertyRegistry().Register(objectHandle, std::string(copy.GetPropertyPath())) : 0u;
}

uint8_t EqualPropertyContents(
    const SerializedPropertyHandle left,
    const SerializedPropertyHandle right)
{
    if (left == 0u || right == 0u)
        return left == right ? 1u : 0u;
    SerializedObjectHandle leftObject = 0u;
    SerializedObjectHandle rightObject = 0u;
    std::string leftPath;
    std::string rightPath;
    if (!PropertyRegistry().Resolve(left, leftObject, leftPath) ||
        !PropertyRegistry().Resolve(right, rightObject, rightPath))
    {
        return 0u;
    }
    return leftObject == rightObject && leftPath == rightPath ? 1u : 0u;
}

void ResetProperty(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (property)
        property.Reset();
}

uint8_t DataEquals(
    const SerializedPropertyHandle left,
    const SerializedPropertyHandle right)
{
    std::shared_ptr<SerializedObject> leftObject;
    std::shared_ptr<SerializedObject> rightObject;
    const auto leftProperty = ResolveProperty(left, leftObject);
    const auto rightProperty = ResolveProperty(right, rightObject);
    return leftProperty && rightProperty && SerializedProperty::DataEquals(leftProperty, rightProperty)
        ? 1u
        : 0u;
}

uint8_t DeletePropertyCommand(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.DeleteCommand() ? 1u : 0u;
}

uint8_t DuplicatePropertyCommand(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.DuplicateCommand() ? 1u : 0u;
}

uint32_t GetPropertyPath(
    const SerializedPropertyHandle handle,
    char* output,
    const uint32_t capacity)
{
    return CopyPropertyText(handle, output, capacity, [](const SerializedProperty& property)
    {
        return property.GetPropertyPath();
    });
}

uint32_t GetTargetCount(const SerializedObjectHandle handle)
{
    const auto object = ResolveObject(handle);
    return object != nullptr
        ? static_cast<uint32_t>((std::min)(object->GetTargetHandles().size(), static_cast<size_t>((std::numeric_limits<uint32_t>::max)())))
        : 0u;
}

uint8_t GetInspectorMode(const SerializedObjectHandle handle)
{
    const auto object = ResolveObject(handle);
    return object != nullptr ? static_cast<uint8_t>(object->GetInspectorMode()) : 0u;
}

void SetInspectorMode(
    const SerializedObjectHandle handle,
    const uint8_t mode)
{
    const auto object = ResolveObject(handle);
    if (object == nullptr || mode > static_cast<uint8_t>(SerializedInspectorMode::DebugInternal))
        return;
    object->SetInspectorMode(static_cast<SerializedInspectorMode>(mode));
}

uint32_t GetMaxArraySizeForMultiEditing(const SerializedObjectHandle handle)
{
    const auto object = ResolveObject(handle);
    return object != nullptr
        ? static_cast<uint32_t>((std::min)(object->GetMaxArraySizeForMultiEditing(), static_cast<size_t>((std::numeric_limits<uint32_t>::max)())))
        : 0u;
}

uint8_t SetMaxArraySizeForMultiEditing(
    const SerializedObjectHandle handle,
    const uint32_t value)
{
    const auto object = ResolveObject(handle);
    if (object == nullptr)
        return 0u;
    object->SetMaxArraySizeForMultiEditing(value);
    return 1u;
}

void SetIsDifferentCacheDirty(const SerializedObjectHandle handle)
{
    if (const auto object = ResolveObject(handle))
        object->SetIsDifferentCacheDirty();
}

uint8_t CopyFromSerializedProperty(
    const SerializedObjectHandle objectHandle,
    const SerializedPropertyHandle sourceHandle)
{
    const auto object = ResolveObject(objectHandle);
    if (object == nullptr)
        return 0u;
    std::shared_ptr<SerializedObject> sourceObject;
    const auto source = ResolveProperty(sourceHandle, sourceObject);
    if (!source || sourceObject == nullptr)
        return 0u;
    std::string sourcePath;
    SerializedObjectHandle ignoredObject = 0u;
    if (!PropertyRegistry().Resolve(sourceHandle, ignoredObject, sourcePath) || sourcePath.empty())
        return 0u;
    const auto destination = object->FindProperty(sourcePath);
    return destination && destination.SetValue(source.GetValue()) ? 1u : 0u;
}

int32_t GetPropertyObjectReferenceInstanceId(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property)
        return InstanceID_None;
    const auto value = property.GetValue();
    if (!value.IsValid() || value.GetType() != NLS_TYPEOF(Object*))
        return InstanceID_None;
    auto* referenced = value.GetValue<Object*>();
    return referenced != nullptr ? referenced->GetInstanceID() : InstanceID_None;
}

uint8_t SetPropertyObjectReferenceInstanceId(
    const SerializedPropertyHandle handle,
    const int32_t instanceId)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property)
        return 0u;
    auto* referenced = Object::IDToPointer(instanceId);
    meta::Variant value(referenced, meta::variant_policy::WrapObject {});
    return property.SetValue(value) ? 1u : 0u;
}

uint8_t GetPropertyVector2Int(
    const SerializedPropertyHandle handle,
    EditorSerializationVector2Int* output)
{
    if (output == nullptr)
        return 0u;
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property)
        return 0u;
    const auto value = property.GetVector2IntValue();
    output->x = value.x;
    output->y = value.y;
    return 1u;
}

uint8_t GetPropertyVector3Int(
    const SerializedPropertyHandle handle,
    EditorSerializationVector3Int* output)
{
    if (output == nullptr)
        return 0u;
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property)
        return 0u;
    const auto value = property.GetVector3IntValue();
    output->x = value.x;
    output->y = value.y;
    output->z = value.z;
    return 1u;
}

uint8_t GetPropertyRectInt(
    const SerializedPropertyHandle handle,
    EditorSerializationRectInt* output)
{
    if (output == nullptr)
        return 0u;
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property)
        return 0u;
    const auto value = property.GetRectIntValue();
    output->x = value.x;
    output->y = value.y;
    output->width = value.width;
    output->height = value.height;
    return 1u;
}

uint8_t GetPropertyBounds(
    const SerializedPropertyHandle handle,
    EditorSerializationBounds* output)
{
    if (output == nullptr)
        return 0u;
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property)
        return 0u;
    const auto value = property.GetBoundsValue();
    output->centerX = value.center.x;
    output->centerY = value.center.y;
    output->centerZ = value.center.z;
    output->sizeX = value.size.x;
    output->sizeY = value.size.y;
    output->sizeZ = value.size.z;
    return 1u;
}

uint8_t GetPropertyBoundsInt(
    const SerializedPropertyHandle handle,
    EditorSerializationBoundsInt* output)
{
    if (output == nullptr)
        return 0u;
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property)
        return 0u;
    const auto value = property.GetBoundsIntValue();
    output->positionX = value.position.x;
    output->positionY = value.position.y;
    output->positionZ = value.position.z;
    output->sizeX = value.size.x;
    output->sizeY = value.size.y;
    output->sizeZ = value.size.z;
    return 1u;
}

uint8_t SetPropertyVector2Int(
    const SerializedPropertyHandle handle,
    const int32_t x,
    const int32_t y)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.SetVector2IntValue({x, y}) ? 1u : 0u;
}

uint8_t SetPropertyVector3Int(
    const SerializedPropertyHandle handle,
    const int32_t x,
    const int32_t y,
    const int32_t z)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.SetVector3IntValue({x, y, z}) ? 1u : 0u;
}

uint8_t SetPropertyRectInt(
    const SerializedPropertyHandle handle,
    const int32_t x,
    const int32_t y,
    const int32_t width,
    const int32_t height)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.SetRectIntValue({x, y, width, height}) ? 1u : 0u;
}

uint8_t SetPropertyBounds(
    const SerializedPropertyHandle handle,
    const float centerX,
    const float centerY,
    const float centerZ,
    const float sizeX,
    const float sizeY,
    const float sizeZ)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.SetBoundsValue({
        {centerX, centerY, centerZ},
        {sizeX, sizeY, sizeZ}}) ? 1u : 0u;
}

uint8_t SetPropertyBoundsInt(
    const SerializedPropertyHandle handle,
    const int32_t positionX,
    const int32_t positionY,
    const int32_t positionZ,
    const int32_t sizeX,
    const int32_t sizeY,
    const int32_t sizeZ)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.SetBoundsIntValue({
        {positionX, positionY, positionZ},
        {sizeX, sizeY, sizeZ}}) ? 1u : 0u;
}

uint32_t GetPropertyAnimationCurve(
    const SerializedPropertyHandle handle,
    uint8_t* output,
    const uint32_t capacity)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property || property.GetPropertyType() != SerializedPropertyType::AnimationCurve)
        return 0u;
    const auto bytes = EncodeSpecialValue(property.GetAnimationCurveValue());
    return bytes.has_value() ? CopyBytes(*bytes, output, capacity) : 0u;
}

uint8_t SetPropertyAnimationCurve(
    const SerializedPropertyHandle handle,
    const uint8_t* input,
    const uint32_t size)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property || property.GetPropertyType() != SerializedPropertyType::AnimationCurve)
        return 0u;
    const auto value = DecodeSpecialValue<Engine::Serialize::AnimationCurve>(input, size);
    return value.has_value() && property.SetAnimationCurveValue(*value) ? 1u : 0u;
}

uint32_t GetPropertyGradient(
    const SerializedPropertyHandle handle,
    uint8_t* output,
    const uint32_t capacity)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property || property.GetPropertyType() != SerializedPropertyType::Gradient)
        return 0u;
    const auto bytes = EncodeSpecialValue(property.GetGradientValue());
    return bytes.has_value() ? CopyBytes(*bytes, output, capacity) : 0u;
}

uint8_t SetPropertyGradient(
    const SerializedPropertyHandle handle,
    const uint8_t* input,
    const uint32_t size)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property || property.GetPropertyType() != SerializedPropertyType::Gradient)
        return 0u;
    const auto value = DecodeSpecialValue<Engine::Serialize::Gradient>(input, size);
    return value.has_value() && property.SetGradientValue(*value) ? 1u : 0u;
}

uint32_t GetPropertyExposedReference(
    const SerializedPropertyHandle handle,
    uint8_t* output,
    const uint32_t capacity)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property || property.GetPropertyType() != SerializedPropertyType::ExposedReference)
        return 0u;
    return CopyBytes(
        EncodeExposedReferenceValue(property.GetExposedReferenceValue()),
        output,
        capacity);
}

uint8_t SetPropertyExposedReference(
    const SerializedPropertyHandle handle,
    const uint8_t* input,
    const uint32_t size)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property || property.GetPropertyType() != SerializedPropertyType::ExposedReference)
        return 0u;
    const auto value = DecodeExposedReferenceValue(input, size);
    return value.has_value() && property.SetExposedReferenceValue(*value) ? 1u : 0u;
}

uint32_t GetPropertyEnumDisplayNames(
    const SerializedPropertyHandle handle,
    char* output,
    const uint32_t capacity)
{
    return CopyPropertyText(handle, output, capacity, [](const SerializedProperty& property)
    {
        const auto names = property.GetEnumDisplayNames();
        std::string result;
        for (size_t index = 0u; index < names.size(); ++index)
        {
            if (index != 0u)
                result.push_back('\n');
            result += names[index];
        }
        return result;
    });
}

uint8_t CopyFromSerializedPropertyIfDifferent(
    const SerializedObjectHandle objectHandle,
    const SerializedPropertyHandle sourceHandle)
{
    const auto object = ResolveObject(objectHandle);
    if (object == nullptr)
        return 0u;
    std::shared_ptr<SerializedObject> sourceObject;
    const auto source = ResolveProperty(sourceHandle, sourceObject);
    if (!source || sourceObject == nullptr)
        return 0u;
    return object->CopyFromSerializedPropertyIfDifferent(source) ? 1u : 0u;
}

uint32_t GetPropertyTypeName(
    const SerializedPropertyHandle handle,
    char* output,
    const uint32_t capacity)
{
    return CopyPropertyText(handle, output, capacity, [](const SerializedProperty& property)
    {
        return property.GetExactTypeName();
    });
}

uint32_t GetPropertyArrayElementType(
    const SerializedPropertyHandle handle,
    char* output,
    const uint32_t capacity)
{
    return CopyPropertyText(handle, output, capacity, [](const SerializedProperty& property)
    {
        return property.GetArrayElementType();
    });
}

uint64_t GetPropertyExactTypeId(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property ? property.GetExactTypeId() : 0u;
}

uint32_t GetPropertyCountInProperty(
    const SerializedPropertyHandle handle,
    const uint8_t /* reserved */)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property)
        return 0u;
    return static_cast<uint32_t>((std::min)(
        property.CountInProperty(),
        static_cast<size_t>((std::numeric_limits<uint32_t>::max)())));
}

uint8_t GetPropertyIsAnimated(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.IsAnimated() ? 1u : 0u;
}

uint32_t GetPropertyCountRemaining(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property)
        return 0u;
    return static_cast<uint32_t>((std::min)(
        property.CountRemaining(),
        static_cast<size_t>((std::numeric_limits<uint32_t>::max)())));
}

uint8_t GetPropertyReadOnly(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    return property && property.IsReadOnly() ? 1u : 0u;
}

int32_t ResolveTargetInstanceId(const Inspector::InspectorTargetHandle& target)
{
    if (target.instanceId != InstanceID_None)
        return target.instanceId;
    if (target.objectIdentifier.IsValid())
    {
        InstanceID resolved = InstanceID_None;
        if (NLS::Engine::Serialize::PersistentManager::Instance().TryGetInstanceID(
                target.objectIdentifier,
                resolved))
        {
            return resolved;
        }
    }
    return InstanceID_None;
}

int32_t GetTargetObjectInstanceId(
    const SerializedObjectHandle handle,
    const uint32_t index)
{
    const auto object = ResolveObject(handle);
    if (object == nullptr)
        return InstanceID_None;
    const auto targets = object->GetTargetHandles();
    return index < targets.size()
        ? ResolveTargetInstanceId(targets[index])
        : InstanceID_None;
}

int32_t GetContextObjectInstanceId(const SerializedObjectHandle handle)
{
    const auto object = ResolveObject(handle);
    const auto* context = object != nullptr ? object->GetContext() : nullptr;
    return context != nullptr ? ResolveTargetInstanceId(*context) : InstanceID_None;
}

int32_t GetResolvedExposedReferenceInstanceId(const SerializedPropertyHandle handle)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property || object == nullptr)
        return InstanceID_None;
    const auto resolved = object->ResolveExposedReference(property);
    return resolved.has_value() ? ResolveTargetInstanceId(*resolved) : InstanceID_None;
}

uint8_t SetResolvedExposedReferenceInstanceId(
    const SerializedPropertyHandle handle,
    const int32_t instanceId)
{
    std::shared_ptr<SerializedObject> object;
    const auto property = ResolveProperty(handle, object);
    if (!property || object == nullptr ||
        property.GetPropertyType() != SerializedPropertyType::ExposedReference)
    {
        return 0u;
    }

    std::optional<Inspector::InspectorTargetHandle> target;
    if (instanceId != InstanceID_None)
    {
        auto* gameObject = Object::IDToPointer<NLS::Engine::GameObject>(instanceId);
        if (gameObject == nullptr)
            return 0u;
        target = Inspector::InspectorTargetHandle::FromObject(
            *gameObject,
            Inspector::InspectorTargetKind::GameObject);
    }
    return object->SetResolvedExposedReference(property, std::move(target)) ? 1u : 0u;
}

uint32_t GetObjectLastError(
    const SerializedObjectHandle handle,
    char* output,
    const uint32_t capacity)
{
    const auto object = ResolveObject(handle);
    return CopyPropertyTextValue(
        object != nullptr ? object->GetLastError() : std::string {},
        output,
        capacity);
}

SerializedObjectHandle CreateObjectFromInstanceIds(
    const int32_t* instanceIds,
    const uint32_t count)
{
    if (instanceIds == nullptr || count == 0u)
        return 0u;
    Inspector::InspectorTargetSet targets;
    targets.targets.reserve(count);
    for (uint32_t index = 0u; index < count; ++index)
    {
        auto* object = Object::IDToPointer(instanceIds[index]);
        if (object == nullptr)
            return 0u;
        const auto kind = dynamic_cast<NLS::Engine::GameObject*>(object) != nullptr
            ? Inspector::InspectorTargetKind::GameObject
            : Inspector::InspectorTargetKind::NativeObject;
        targets.targets.push_back(Inspector::InspectorTargetHandle::FromObject(*object, kind));
    }
    targets.Normalize();
    auto serialized = std::make_shared<SerializedObject>(
        std::move(targets),
        NativeReflectionSerializedBackend::Create());
    return serialized->IsValid() ? RegisterEditorSerializedObject(std::move(serialized)) : 0u;
}

SerializedObjectHandle CreateObjectFromInstanceIdsWithContext(
    const int32_t* instanceIds,
    const uint32_t count,
    const int32_t contextInstanceId)
{
    const auto handle = CreateObjectFromInstanceIds(instanceIds, count);
    if (handle == 0u || contextInstanceId == InstanceID_None)
        return handle;
    const auto object = ResolveObject(handle);
    auto* contextObject = Object::IDToPointer(contextInstanceId);
    if (object == nullptr || contextObject == nullptr)
        return handle;
    const auto kind = dynamic_cast<NLS::Engine::GameObject*>(contextObject) != nullptr
        ? Inspector::InspectorTargetKind::GameObject
        : Inspector::InspectorTargetKind::NativeObject;
    object->SetContext(Inspector::InspectorTargetHandle::FromObject(*contextObject, kind));
    return handle;
}

void DiscardModifiedProperties(const SerializedObjectHandle handle)
{
    if (const auto object = ResolveObject(handle))
        object->DiscardModifiedProperties();
}
} // namespace

const EditorSerializationApiTable& GetEditorSerializationApiTable()
{
    static const EditorSerializationApiTable table {
        EditorSerializationAbiHeader {
            static_cast<uint32_t>(sizeof(EditorSerializationApiTable)),
            1u,
            static_cast<uint64_t>(std::hash<std::string> {}("Nullus.Editor.SerializedObject.v1"))},
        &IsObjectValid,
        &ReleaseObject,
        &UpdateObject,
        &UpdateIfRequired,
        &HasModified,
        &ApplyObject,
        &FindProperty,
        &IsPropertyValid,
        &ReleaseProperty,
        &GetPropertyType,
        &GetPropertyBool,
        &GetPropertyLong,
        &GetPropertyUnsignedLong,
        &GetPropertyDouble,
        &GetPropertyString,
        &SetPropertyBool,
        &SetPropertyLong,
        &SetPropertyUnsignedLong,
        &SetPropertyDouble,
        &SetPropertyString,
        &GetPropertyVector2,
        &GetPropertyVector3,
        &GetPropertyVector4,
        &GetPropertyQuaternion,
        &GetPropertyColor,
        &GetPropertyRect,
        &SetPropertyVector2,
        &SetPropertyVector3,
        &SetPropertyVector4,
        &SetPropertyQuaternion,
        &SetPropertyColor,
        &SetPropertyRect,
        &IsPropertyArray,
        &GetPropertyArraySize,
        &GetPropertyMinArraySize,
        &GetPropertyFixedBufferSize,
        &SetPropertyArraySize,
        &InsertPropertyArrayElement,
        &DeletePropertyArrayElement,
        &MovePropertyArrayElement,
        &ClearPropertyArray,
        &GetPropertyObjectReferenceInstanceId,
        &SetPropertyObjectReferenceInstanceId,
        &GetPropertyNumericType,
        &GetPropertyName,
        &GetPropertyDisplayName,
        &GetPropertyTooltip,
        &GetPropertyDepth,
        &GetPropertyHasChildren,
        &GetPropertyHasVisibleChildren,
        &GetPropertyEditable,
        &GetPropertyIsExpanded,
        &SetPropertyIsExpanded,
        &GetPropertyHasMultipleDifferentValues,
        &GetPropertyPrefabOverride,
        &GetPropertyDefaultOverride,
        &GetPropertyInstantiatedPrefab,
        &GetIterator,
        &NextProperty,
        &GetEndProperty,
        &CopyProperty,
        &EqualPropertyContents,
        &GetPropertyPath,
        &GetTargetCount,
        &GetInspectorMode,
        &SetInspectorMode,
        &GetMaxArraySizeForMultiEditing,
        &SetMaxArraySizeForMultiEditing,
        &SetIsDifferentCacheDirty,
        &CopyFromSerializedProperty,
        &GetPropertyVector2Int,
        &GetPropertyVector3Int,
        &GetPropertyRectInt,
        &GetPropertyBounds,
        &GetPropertyBoundsInt,
        &SetPropertyVector2Int,
        &SetPropertyVector3Int,
        &SetPropertyRectInt,
        &SetPropertyBounds,
        &SetPropertyBoundsInt,
        &GetPropertyEnumDisplayNames,
        &GetPropertyAnimationCurve,
        &SetPropertyAnimationCurve,
        &GetPropertyGradient,
        &SetPropertyGradient,
        &GetPropertyExposedReference,
        &SetPropertyExposedReference,
        &CopyFromSerializedPropertyIfDifferent,
        &GetPropertyTypeName,
        &GetPropertyExactTypeId,
        &GetPropertyCountInProperty,
        &GetPropertyCountRemaining,
        &GetPropertyReadOnly,
        &GetTargetObjectInstanceId,
        &GetObjectLastError,
        &CreateObjectFromInstanceIds,
        &DiscardModifiedProperties,
        &GetContextObjectInstanceId,
        &GetResolvedExposedReferenceInstanceId,
        &CreateObjectFromInstanceIdsWithContext,
        &ResetProperty,
        &DataEquals,
        &DeletePropertyCommand,
        &DuplicatePropertyCommand,
        &GetPropertyArrayElementType,
        &SetResolvedExposedReferenceInstanceId,
        &GetPropertyIsAnimated};
    return table;
}

SerializedObjectHandle RegisterEditorSerializedObject(
    std::shared_ptr<SerializedObject> object)
{
    return ObjectRegistry().Register(std::move(object));
}
} // namespace NLS::Editor::Serialization
