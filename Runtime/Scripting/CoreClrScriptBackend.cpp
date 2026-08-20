#include "CoreClrScriptBackend.h"

#include "Base/Object/Object.h"
#include "Engine/Components/TransformComponent.h"
#include "Engine/Components/Component.h"
#include "Engine/Components/CameraComponent.h"
#include "Engine/Components/LightComponent.h"
#include "Engine/GameObject.h"
#include "Engine/SceneSystem/Scene.h"
#include "Engine/SceneSystem/SceneManager.h"
#include "ScriptEngineApi.h"
#include "ScriptComponent.h"
#include "Windowing/Inputs/InputManager.h"
#include "Windowing/Inputs/EKey.h"
#include "Windowing/Inputs/EMouseButton.h"

#include <Json/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <type_traits>
#include <utility>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

namespace NLS::Scripting
{
namespace
{
constexpr int32_t kHostFxrDelegateLoadAssemblyAndGetFunctionPointer = 5;
std::string g_hostFxrError;
NLS::Engine::SceneSystem::Scene* g_nativeScriptScene = nullptr;
CoreClrScriptBackend* g_activeCoreClrBackend = nullptr;

// CoreCLR itself is process-wide even though each ScriptRuntime owns its
// managed state and collectible project ALC.  hostfxr rejects a second
// initialization after the first runtime has been loaded, so retain the
// hosting context and delegate pointers for the lifetime of the process and
// let each CoreClrHost borrow them.  ManagedExports still performs the
// per-runtime shutdown/reload work above this host layer.
struct SharedHostFxrState
{
    void* library = nullptr;
    void* context = nullptr;
    void* close = nullptr;
    void* getRuntimeDelegate = nullptr;
    void* setErrorWriter = nullptr;
    void* loadAssemblyAndGetFunctionPointer = nullptr;
    std::filesystem::path hostFxrPath;
    size_t references = 0;
};

SharedHostFxrState g_sharedHostFxr;

#if defined(_WIN32)
void HostFxrErrorWriter(const wchar_t* message)
{
    if (!message)
        return;
    const auto required = WideCharToMultiByte(CP_UTF8, 0, message, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1)
        return;
    std::string converted(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, message, -1, converted.data(), required, nullptr, nullptr);
    converted.resize(static_cast<size_t>(required - 1));
    g_hostFxrError = std::move(converted);
}
#else
void HostFxrErrorWriter(const char* message)
{
    if (message)
        g_hostFxrError = message;
}
#endif

static_assert(sizeof(ScriptAbiHeader) == 16, "Script ABI header must remain a fixed 16-byte layout.");
static_assert(sizeof(ScriptAbiResult) == 16, "Script ABI result must remain a fixed 16-byte layout.");
static_assert(sizeof(ScriptAbiValue) == 80, "Script ABI value must remain a fixed 80-byte layout.");
static_assert(offsetof(ScriptAbiValue, utf8Data) == 48, "Script ABI value string pointer offset changed.");
static_assert(offsetof(ScriptAbiValue, bytes) == 64, "Script ABI value byte pointer offset changed.");
static_assert(sizeof(NativeApiTable) == 304, "Native scripting API table layout changed.");
static_assert(offsetof(NativeApiTable, isAlive) == 24, "Native scripting API table pointer layout changed.");
static_assert(offsetof(ScriptAbiDiagnostic, sourcePath) == 16, "Managed diagnostic source pointer layout changed.");

uint64_t AssetKey(const NLS::Core::Assets::AssetId& assetId)
{
    uint64_t value = 14695981039346656037ull;
    for (const auto byte : assetId.GetGuid().GetBytes())
    {
        value ^= byte;
        value *= 1099511628211ull;
    }
    return value == 0 ? 1 : value;
}

std::string NormalizeManifestPath(std::string_view path)
{
    std::string normalized(path);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    while (normalized.size() > 1 && normalized.back() == '/')
        normalized.pop_back();
#if defined(_WIN32)
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char value)
    {
        return static_cast<char>(std::tolower(value));
    });
#endif
    return normalized;
}

std::string ManifestFileName(std::string_view path)
{
    const auto normalized = NormalizeManifestPath(path);
    const auto separator = normalized.find_last_of('/');
    return separator == std::string::npos ? normalized : normalized.substr(separator + 1);
}

ScriptValueKind ParseManagedValueKind(std::string_view kind)
{
    if (kind == "Bool") return ScriptValueKind::Bool;
    if (kind == "Int8") return ScriptValueKind::Int8;
    if (kind == "UInt8") return ScriptValueKind::UInt8;
    if (kind == "Int16") return ScriptValueKind::Int16;
    if (kind == "UInt16") return ScriptValueKind::UInt16;
    if (kind == "Int32") return ScriptValueKind::Int32;
    if (kind == "UInt32") return ScriptValueKind::UInt32;
    if (kind == "Int64") return ScriptValueKind::Int64;
    if (kind == "UInt64") return ScriptValueKind::UInt64;
    if (kind == "Float") return ScriptValueKind::Float;
    if (kind == "Double") return ScriptValueKind::Double;
    if (kind == "String") return ScriptValueKind::String;
    if (kind == "Enum") return ScriptValueKind::Enum;
    if (kind == "Vector2") return ScriptValueKind::Vector2;
    if (kind == "Vector3") return ScriptValueKind::Vector3;
    if (kind == "Vector4") return ScriptValueKind::Vector4;
    if (kind == "Quaternion") return ScriptValueKind::Quaternion;
    if (kind == "Color") return ScriptValueKind::Color;
    if (kind == "ObjectReference") return ScriptValueKind::ObjectReference;
    return ScriptValueKind::Struct;
}

ScriptType MakeManagedFieldType(std::string_view typeName, std::string_view kind)
{
    ScriptType type;
    type.name = std::string(typeName);
    type.kind = ParseManagedValueKind(kind);
    type.id = MakeStableScriptId(type.name);
    type.portable = true;
    switch (type.kind)
    {
    case ScriptValueKind::Bool: type.size = sizeof(bool); type.alignment = alignof(bool); break;
    case ScriptValueKind::Int8: type.size = sizeof(int8_t); type.alignment = alignof(int8_t); break;
    case ScriptValueKind::UInt8: type.size = sizeof(uint8_t); type.alignment = alignof(uint8_t); break;
    case ScriptValueKind::Int16: type.size = sizeof(int16_t); type.alignment = alignof(int16_t); break;
    case ScriptValueKind::UInt16: type.size = sizeof(uint16_t); type.alignment = alignof(uint16_t); break;
    case ScriptValueKind::Int32: type.size = sizeof(int32_t); type.alignment = alignof(int32_t); break;
    case ScriptValueKind::UInt32: type.size = sizeof(uint32_t); type.alignment = alignof(uint32_t); break;
    case ScriptValueKind::Int64: type.size = sizeof(int64_t); type.alignment = alignof(int64_t); break;
    case ScriptValueKind::UInt64: type.size = sizeof(uint64_t); type.alignment = alignof(uint64_t); break;
    case ScriptValueKind::Float: type.size = sizeof(float); type.alignment = alignof(float); break;
    case ScriptValueKind::Double: type.size = sizeof(double); type.alignment = alignof(double); break;
    case ScriptValueKind::Vector2: type.size = sizeof(NLS::Maths::Vector2); type.alignment = alignof(NLS::Maths::Vector2); break;
    case ScriptValueKind::Vector3: type.size = sizeof(NLS::Maths::Vector3); type.alignment = alignof(NLS::Maths::Vector3); break;
    case ScriptValueKind::Vector4: type.size = sizeof(NLS::Maths::Vector4); type.alignment = alignof(NLS::Maths::Vector4); break;
    case ScriptValueKind::Quaternion: type.size = sizeof(NLS::Maths::Quaternion); type.alignment = alignof(NLS::Maths::Quaternion); break;
    case ScriptValueKind::Color: type.size = sizeof(NLS::Maths::Color); type.alignment = alignof(NLS::Maths::Color); break;
    case ScriptValueKind::String:
    case ScriptValueKind::Enum:
    case ScriptValueKind::ObjectReference:
    case ScriptValueKind::Struct:
    case ScriptValueKind::Null:
    default:
        break;
    }
    return type;
}

ScriptAbiValue ToAbiValue(
    const ScriptValue& value,
    std::string& stringStorage,
    std::vector<uint8_t>& byteStorage,
    bool& supported)
{
    supported = true;
    ScriptAbiValue result;
    result.kind = static_cast<uint32_t>(std::visit([](const auto& item)
    {
        using Value = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Value, std::monostate>) return ScriptValueKind::Null;
        if constexpr (std::is_same_v<Value, bool>) return ScriptValueKind::Bool;
        if constexpr (std::is_same_v<Value, int8_t>) return ScriptValueKind::Int8;
        if constexpr (std::is_same_v<Value, uint8_t>) return ScriptValueKind::UInt8;
        if constexpr (std::is_same_v<Value, int16_t>) return ScriptValueKind::Int16;
        if constexpr (std::is_same_v<Value, uint16_t>) return ScriptValueKind::UInt16;
        if constexpr (std::is_same_v<Value, int32_t>) return ScriptValueKind::Int32;
        if constexpr (std::is_same_v<Value, uint32_t>) return ScriptValueKind::UInt32;
        if constexpr (std::is_same_v<Value, int64_t>) return ScriptValueKind::Int64;
        if constexpr (std::is_same_v<Value, uint64_t>) return ScriptValueKind::UInt64;
        if constexpr (std::is_same_v<Value, float>) return ScriptValueKind::Float;
        if constexpr (std::is_same_v<Value, double>) return ScriptValueKind::Double;
        if constexpr (std::is_same_v<Value, std::string>) return ScriptValueKind::String;
        if constexpr (std::is_same_v<Value, ScriptEnumValue>) return ScriptValueKind::Enum;
        if constexpr (std::is_same_v<Value, NLS::Maths::Vector2>) return ScriptValueKind::Vector2;
        if constexpr (std::is_same_v<Value, NLS::Maths::Vector3>) return ScriptValueKind::Vector3;
        if constexpr (std::is_same_v<Value, NLS::Maths::Vector4>) return ScriptValueKind::Vector4;
        if constexpr (std::is_same_v<Value, NLS::Maths::Quaternion>) return ScriptValueKind::Quaternion;
        if constexpr (std::is_same_v<Value, NLS::Maths::Color>) return ScriptValueKind::Color;
        if constexpr (std::is_same_v<Value, NativeObjectHandle>) return ScriptValueKind::ObjectReference;
        return ScriptValueKind::Struct;
    }, value));

    std::visit([&](const auto& item)
    {
        using Value = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Value, bool>) result.unsignedValue = item ? 1u : 0u;
        else if constexpr (std::is_integral_v<Value> && std::is_signed_v<Value>) result.signedValue = item;
        else if constexpr (std::is_integral_v<Value> && std::is_unsigned_v<Value>) result.unsignedValue = item;
        else if constexpr (std::is_floating_point_v<Value>) result.floatingValue = item;
        else if constexpr (std::is_same_v<Value, std::string>)
        {
            stringStorage = item;
            result.utf8Data = stringStorage.data();
            result.utf8Size = static_cast<uint32_t>(stringStorage.size());
        }
        else if constexpr (std::is_same_v<Value, ScriptEnumValue>)
        {
            result.typeId = item.typeId;
            result.signedValue = item.value;
        }
        else if constexpr (std::is_same_v<Value, NativeObjectHandle>) result.objectValue = item.value;
        else if constexpr (std::is_same_v<Value, ScriptObjectReference>)
        {
            // ObjectIdentifier-backed references are persistent scene data and
            // must be resolved to a runtime handle before crossing the managed
            // ABI.  Never copy this owning C++ value into ScriptAbiValue.
            supported = false;
        }
        else if constexpr (!std::is_same_v<Value, std::monostate>)
        {
            byteStorage.resize(sizeof(Value));
            std::memcpy(byteStorage.data(), &item, sizeof(Value));
            result.bytes = byteStorage.data();
            result.byteSize = static_cast<uint32_t>(byteStorage.size());
        }
    }, value);
    return result;
}

bool FromAbiValue(const ScriptAbiValue& input, ScriptValue& output)
{
    const auto kind = static_cast<ScriptValueKind>(input.kind);
    switch (kind)
    {
    case ScriptValueKind::Null: output = std::monostate{}; return true;
    case ScriptValueKind::Bool: output = input.unsignedValue != 0; return true;
    case ScriptValueKind::Int8: output = static_cast<int8_t>(input.signedValue); return true;
    case ScriptValueKind::UInt8: output = static_cast<uint8_t>(input.unsignedValue); return true;
    case ScriptValueKind::Int16: output = static_cast<int16_t>(input.signedValue); return true;
    case ScriptValueKind::UInt16: output = static_cast<uint16_t>(input.unsignedValue); return true;
    case ScriptValueKind::Int32: output = static_cast<int32_t>(input.signedValue); return true;
    case ScriptValueKind::UInt32: output = static_cast<uint32_t>(input.unsignedValue); return true;
    case ScriptValueKind::Int64: output = input.signedValue; return true;
    case ScriptValueKind::UInt64: output = input.unsignedValue; return true;
    case ScriptValueKind::Float: output = static_cast<float>(input.floatingValue); return true;
    case ScriptValueKind::Double: output = input.floatingValue; return true;
    case ScriptValueKind::String:
        if (!input.utf8Data) return false;
        output = std::string(input.utf8Data, input.utf8Size);
        return true;
    case ScriptValueKind::Enum: output = ScriptEnumValue{input.typeId, input.signedValue}; return true;
    case ScriptValueKind::ObjectReference: output = NativeObjectHandle{input.objectValue}; return true;
    case ScriptValueKind::Vector2:
    case ScriptValueKind::Vector3:
    case ScriptValueKind::Vector4:
    case ScriptValueKind::Quaternion:
    case ScriptValueKind::Color:
        if (!input.bytes || input.byteSize == 0) return false;
        if (kind == ScriptValueKind::Vector2 && input.byteSize == sizeof(NLS::Maths::Vector2)) { NLS::Maths::Vector2 value; std::memcpy(&value, input.bytes, sizeof(value)); output = value; return true; }
        if (kind == ScriptValueKind::Vector3 && input.byteSize == sizeof(NLS::Maths::Vector3)) { NLS::Maths::Vector3 value; std::memcpy(&value, input.bytes, sizeof(value)); output = value; return true; }
        if (kind == ScriptValueKind::Vector4 && input.byteSize == sizeof(NLS::Maths::Vector4)) { NLS::Maths::Vector4 value; std::memcpy(&value, input.bytes, sizeof(value)); output = value; return true; }
        if (kind == ScriptValueKind::Quaternion && input.byteSize == sizeof(NLS::Maths::Quaternion)) { NLS::Maths::Quaternion value; std::memcpy(&value, input.bytes, sizeof(value)); output = value; return true; }
        if (kind == ScriptValueKind::Color && input.byteSize == sizeof(NLS::Maths::Color)) { NLS::Maths::Color value; std::memcpy(&value, input.bytes, sizeof(value)); output = value; return true; }
        return false;
    case ScriptValueKind::Struct:
        if (!input.bytes) return false;
        output = ScriptStructValue{input.typeId, std::vector<uint8_t>(input.bytes, input.bytes + input.byteSize)};
        return true;
    default:
        return false;
    }
}

ScriptAbiResult NativeOk()
{
    return {ScriptStatusCode::Ok, nullptr};
}

ScriptAbiResult NativeError(ScriptStatusCode code, const char* message)
{
    return {code, message};
}

uint8_t NativeIsAlive(uint64_t handle)
{
    if (handle == 0)
        return 0;
    auto* object = NLS::Object::IDToPointerNoThreadCheck(static_cast<NLS::InstanceID>(handle & 0xFFFFFFFFu));
    if (!object)
        return 0;
    if (auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object))
        return gameObject->IsAlive() ? 1 : 0;
    if (auto* component = dynamic_cast<NLS::Engine::Components::Component*>(object))
        return component->gameobject() != nullptr ? 1 : 0;
    return 1;
}

ScriptAbiResult NativeGetObject(uint64_t owner, uint64_t memberId, uint64_t* output)
{
    if (!output)
        return NativeError(ScriptStatusCode::InvalidArgument, "Native object output is null.");
    *output = 0;
    auto* object = NLS::Object::IDToPointerNoThreadCheck(static_cast<NLS::InstanceID>(owner & 0xFFFFFFFFu));
    auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object);
    auto* component = dynamic_cast<NLS::Engine::Components::Component*>(object);
    if (!gameObject && component)
        gameObject = component->gameobject();
    if (!gameObject || !gameObject->IsAlive())
        return NativeError(ScriptStatusCode::InvalidHandle, "Native object handle is no longer alive.");
    if (memberId == MakeStableScriptId("NLS::Engine::Components::Component::GetGameObject()"))
    {
        *output = NativeObjectHandle::FromInstanceId(gameObject->GetInstanceID()).value;
        return NativeOk();
    }
    if (memberId == MakeStableScriptId("NLS::Engine::GameObject::GetTransform()"))
    {
        auto* transform = gameObject->GetTransform();
        if (!transform)
            return NativeError(ScriptStatusCode::InvalidHandle, "Native GameObject has no Transform component.");
        *output = NativeObjectHandle::FromInstanceId(transform->GetInstanceID()).value;
        return NativeOk();
    }
    if (auto* transform = dynamic_cast<NLS::Engine::Components::TransformComponent*>(component);
        transform && memberId == MakeStableScriptId("NLS::Engine::Components::TransformComponent::GetParent()"))
    {
        auto* parent = gameObject->GetParent();
        *output = parent && parent->GetTransform()
            ? NativeObjectHandle::FromInstanceId(parent->GetTransform()->GetInstanceID()).value
            : 0;
        return NativeOk();
    }
    return NativeError(ScriptStatusCode::Unsupported, "The requested Native object member is not registered.");
}

ScriptAbiResult NativeGetVector3(uint64_t owner, uint64_t memberId, float* x, float* y, float* z)
{
    if (!x || !y || !z)
        return NativeError(ScriptStatusCode::InvalidArgument, "Native Vector3 output is null.");
    auto* object = NLS::Object::IDToPointerNoThreadCheck(static_cast<NLS::InstanceID>(owner & 0xFFFFFFFFu));
    NLS::Maths::Vector3 value{};
    if (auto* transform = dynamic_cast<NLS::Engine::Components::TransformComponent*>(object))
    {
        if (memberId == MakeStableScriptId("NLS::Engine::Components::TransformComponent::GetLocalPosition()")) value = transform->GetLocalPosition();
        else if (memberId == MakeStableScriptId("NLS::Engine::Components::TransformComponent::GetLocalScale()")) value = transform->GetLocalScale();
        else if (memberId == MakeStableScriptId("NLS::Engine::Components::TransformComponent::GetWorldPosition()")) value = transform->GetWorldPosition();
        else if (memberId == MakeStableScriptId("NLS::Engine::Components::TransformComponent::GetWorldScale()")) value = transform->GetWorldScale();
        else return NativeError(ScriptStatusCode::Unsupported, "The requested Native Vector3 member is not registered.");
    }
    else if (auto* camera = dynamic_cast<NLS::Engine::Components::CameraComponent*>(object))
    {
        if (memberId != MakeStableScriptId("NLS::Engine::Components::CameraComponent::GetClearColor()"))
            return NativeError(ScriptStatusCode::Unsupported, "The requested Native Vector3 member is not registered.");
        value = camera->GetClearColor();
    }
    else if (auto* light = dynamic_cast<NLS::Engine::Components::LightComponent*>(object))
    {
        if (memberId == MakeStableScriptId("NLS::Engine::Components::LightComponent::GetColor()")) value = light->GetColor();
        else if (memberId == MakeStableScriptId("NLS::Engine::Components::LightComponent::GetSize()")) value = light->GetSize();
        else return NativeError(ScriptStatusCode::Unsupported, "The requested Native Vector3 member is not registered.");
    }
    else
        return NativeError(ScriptStatusCode::InvalidHandle, "Native Vector3 owner is no longer alive.");
    *x = value.x;
    *y = value.y;
    *z = value.z;
    return NativeOk();
}

ScriptAbiResult NativeSetVector3(uint64_t owner, uint64_t memberId, float x, float y, float z)
{
    const NLS::Maths::Vector3 value{x, y, z};
    auto* object = NLS::Object::IDToPointerNoThreadCheck(static_cast<NLS::InstanceID>(owner & 0xFFFFFFFFu));
    if (auto* transform = dynamic_cast<NLS::Engine::Components::TransformComponent*>(object))
    {
        if (memberId == MakeStableScriptId("NLS::Engine::Components::TransformComponent::SetLocalScale(NLS::Maths::Vector3)")) transform->SetLocalScale(value);
        else if (memberId == MakeStableScriptId("NLS::Engine::Components::TransformComponent::SetWorldPosition(NLS::Maths::Vector3)")) transform->SetWorldPosition(value);
        else if (memberId == MakeStableScriptId("NLS::Engine::Components::TransformComponent::SetLocalPosition(NLS::Maths::Vector3)")) transform->SetLocalPosition(value);
        else return NativeError(ScriptStatusCode::Unsupported, "The requested Native Vector3 member is not registered.");
    }
    else if (auto* camera = dynamic_cast<NLS::Engine::Components::CameraComponent*>(object))
    {
        if (memberId != MakeStableScriptId("NLS::Engine::Components::CameraComponent::SetClearColor(NLS::Maths::Vector3&)") &&
            memberId != MakeStableScriptId("NLS::Engine::Components::CameraComponent::SetClearColor(NLS::Maths::Vector3)"))
            return NativeError(ScriptStatusCode::Unsupported, "The requested Native Vector3 member is not registered.");
        camera->SetClearColor(value);
    }
    else if (auto* light = dynamic_cast<NLS::Engine::Components::LightComponent*>(object))
    {
        if (memberId == MakeStableScriptId("NLS::Engine::Components::LightComponent::SetColor(NLS::Maths::Vector3&)") ||
            memberId == MakeStableScriptId("NLS::Engine::Components::LightComponent::SetColor(NLS::Maths::Vector3)")) light->SetColor(value);
        else if (memberId == MakeStableScriptId("NLS::Engine::Components::LightComponent::SetSize(NLS::Maths::Vector3&)") ||
            memberId == MakeStableScriptId("NLS::Engine::Components::LightComponent::SetSize(NLS::Maths::Vector3)")) light->SetSize(value);
        else return NativeError(ScriptStatusCode::Unsupported, "The requested Native Vector3 member is not registered.");
    }
    else
        return NativeError(ScriptStatusCode::InvalidHandle, "Native Vector3 owner is no longer alive.");
    return NativeOk();
}

ScriptAbiResult NativeSetBool(uint64_t owner, uint64_t memberId, uint8_t value)
{
    auto* object = NLS::Object::IDToPointerNoThreadCheck(static_cast<NLS::InstanceID>(owner & 0xFFFFFFFFu));
    auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object);
    if (memberId != MakeStableScriptId("NLS::Engine::GameObject::SetActive(bool)"))
    {
        if (memberId == MakeStableScriptId("NLS::Engine::Components::Component::SetEnabled(bool)"))
        {
            auto* component = dynamic_cast<NLS::Engine::Components::Component*>(object);
            if (!component)
                return NativeError(ScriptStatusCode::InvalidHandle, "Native Component handle is no longer alive.");
            component->SetEnabled(value != 0);
            return NativeOk();
        }
        return NativeError(ScriptStatusCode::Unsupported, "The requested Native boolean member is not registered.");
    }
    if (!gameObject)
        return NativeError(ScriptStatusCode::InvalidHandle, "Native GameObject handle is no longer alive.");
    gameObject->SetActive(value != 0);
    return NativeOk();
}

ScriptAbiResult NativeGetQuaternion(
    uint64_t owner,
    uint64_t memberId,
    float* x,
    float* y,
    float* z,
    float* w)
{
    if (!x || !y || !z || !w)
        return NativeError(ScriptStatusCode::InvalidArgument, "Native Quaternion output is null.");
    auto* transform = NLS::Object::IDToPointer<NLS::Engine::Components::TransformComponent>(
        static_cast<NLS::InstanceID>(owner & 0xFFFFFFFFu));
    if (!transform)
        return NativeError(ScriptStatusCode::InvalidHandle, "Native Transform handle is no longer alive.");
    const bool world = memberId == MakeStableScriptId("NLS::Engine::Components::TransformComponent::GetWorldRotation()");
    if (!world && memberId != MakeStableScriptId("NLS::Engine::Components::TransformComponent::GetLocalRotation()"))
        return NativeError(ScriptStatusCode::Unsupported, "The requested Native Quaternion member is not registered.");
    const auto value = world ? transform->GetWorldRotation() : transform->GetLocalRotation();
    *x = value.x;
    *y = value.y;
    *z = value.z;
    *w = value.w;
    return NativeOk();
}

ScriptAbiResult NativeSetQuaternion(
    uint64_t owner,
    uint64_t memberId,
    float x,
    float y,
    float z,
    float w)
{
    auto* transform = NLS::Object::IDToPointer<NLS::Engine::Components::TransformComponent>(
        static_cast<NLS::InstanceID>(owner & 0xFFFFFFFFu));
    if (!transform)
        return NativeError(ScriptStatusCode::InvalidHandle, "Native Transform handle is no longer alive.");
    const bool world = memberId == MakeStableScriptId("NLS::Engine::Components::TransformComponent::SetWorldRotation(NLS::Maths::Quaternion)");
    if (!world && memberId != MakeStableScriptId("NLS::Engine::Components::TransformComponent::SetLocalRotation(NLS::Maths::Quaternion)"))
        return NativeError(ScriptStatusCode::Unsupported, "The requested Native Quaternion member is not registered.");
    if (world)
        transform->SetWorldRotation(NLS::Maths::Quaternion{x, y, z, w});
    else
        transform->SetLocalRotation(NLS::Maths::Quaternion{x, y, z, w});
    return NativeOk();
}

ScriptAbiResult NativeCreatePrimitive(uint64_t owner, const char* typeName, uint64_t* output)
{
    if (!output)
        return NativeError(ScriptStatusCode::InvalidArgument, "Native primitive output is null.");
    *output = 0;
    auto* gameObject = NLS::Object::IDToPointer<NLS::Engine::GameObject>(
        static_cast<NLS::InstanceID>(owner & 0xFFFFFFFFu));
    if (!gameObject)
        return NativeError(ScriptStatusCode::InvalidHandle, "Native GameObject handle is no longer alive.");
    try
    {
        auto* primitive = gameObject->CreatePrimitive(typeName ? typeName : "Cube");
        if (!primitive)
            return NativeError(ScriptStatusCode::InvalidArgument, "Unknown primitive type or GameObject is not attached to a scene.");
        *output = NativeObjectHandle::FromInstanceId(primitive->GetInstanceID()).value;
        return NativeOk();
    }
    catch (const std::exception& exception)
    {
        return NativeError(ScriptStatusCode::Exception, exception.what());
    }
    catch (...)
    {
        return NativeError(ScriptStatusCode::Exception, "Native primitive creation failed.");
    }
}

ScriptAbiResult NativeCreateGameObject(const char* name, const char* tag, uint64_t* output)
{
    if (!output)
        return NativeError(ScriptStatusCode::InvalidArgument, "Native GameObject output is null.");
    *output = 0;
    auto* scene = ScriptEngineApi::GetScene();
    if (!scene)
        return NativeError(ScriptStatusCode::InvalidHandle, "No active scene is available for GameObject creation.");
    auto& gameObject = scene->CreateGameObject(
        name && *name ? name : "New GameObject",
        tag ? tag : "");
    *output = NativeObjectHandle::FromInstanceId(gameObject.GetInstanceID()).value;
    return NativeOk();
}

NLS::Engine::GameObject* NativeOwnerGameObject(uint64_t owner)
{
    auto* object = NLS::Object::IDToPointerNoThreadCheck(static_cast<NLS::InstanceID>(owner & 0xFFFFFFFFu));
    if (auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object))
        return gameObject;
    if (auto* component = dynamic_cast<NLS::Engine::Components::Component*>(object))
        return component->gameobject();
    return nullptr;
}

ScriptAbiResult NativeGetString(uint64_t owner, uint64_t memberId, const char** output, uint32_t* size)
{
    if (!output || !size)
        return NativeError(ScriptStatusCode::InvalidArgument, "Native string output is null.");
    auto* gameObject = NativeOwnerGameObject(owner);
    if (!gameObject)
        return NativeError(ScriptStatusCode::InvalidHandle, "Native object handle is no longer alive.");
    thread_local std::string value;
    if (memberId == MakeStableScriptId("NLS::Engine::GameObject::GetName()")) value = gameObject->GetName();
    else if (memberId == MakeStableScriptId("NLS::Engine::GameObject::GetTag()")) value = gameObject->GetTag();
    else return NativeError(ScriptStatusCode::Unsupported, "The requested Native string member is not registered.");
    *output = value.data();
    *size = static_cast<uint32_t>(value.size());
    return NativeOk();
}

ScriptAbiResult NativeSetString(uint64_t owner, uint64_t memberId, const char* input)
{
    auto* gameObject = NativeOwnerGameObject(owner);
    if (!gameObject)
        return NativeError(ScriptStatusCode::InvalidHandle, "Native object handle is no longer alive.");
    const std::string value = input ? input : "";
    if (memberId == MakeStableScriptId("NLS::Engine::GameObject::SetName(std::string&)")) gameObject->SetName(value);
    else if (memberId == MakeStableScriptId("NLS::Engine::GameObject::SetTag(std::string&)")) gameObject->SetTag(value);
    else return NativeError(ScriptStatusCode::Unsupported, "The requested Native string member is not registered.");
    return NativeOk();
}

ScriptAbiResult NativeGetBool(uint64_t owner, uint64_t memberId, uint8_t* output)
{
    if (!output)
        return NativeError(ScriptStatusCode::InvalidArgument, "Native bool output is null.");
    auto* object = NLS::Object::IDToPointerNoThreadCheck(static_cast<NLS::InstanceID>(owner & 0xFFFFFFFFu));
    auto* gameObject = NativeOwnerGameObject(owner);
    if (!gameObject)
        return NativeError(ScriptStatusCode::InvalidHandle, "Native object handle is no longer alive.");
    if (memberId == MakeStableScriptId("NLS::Engine::GameObject::GetActive()"))
        *output = gameObject->GetActive() ? 1 : 0;
    else if (memberId == MakeStableScriptId("NLS::Engine::GameObject::IsActive()"))
        *output = gameObject->IsActive() ? 1 : 0;
    else if (memberId == MakeStableScriptId("NLS::Engine::Components::Component::IsActiveAndEnabled()"))
    {
        auto* component = dynamic_cast<NLS::Engine::Components::Component*>(object);
        if (!component)
            return NativeError(ScriptStatusCode::InvalidHandle, "Native Component handle is no longer alive.");
        *output = component->IsActiveAndEnabled() ? 1 : 0;
    }
    else if (memberId == MakeStableScriptId("NLS::Engine::Components::Component::IsSelfEnabled()"))
    {
        auto* component = dynamic_cast<NLS::Engine::Components::Component*>(object);
        if (!component)
            return NativeError(ScriptStatusCode::InvalidHandle, "Native Component handle is no longer alive.");
        *output = component->IsSelfEnabled() ? 1 : 0;
    }
    else
        return NativeError(ScriptStatusCode::Unsupported, "The requested Native bool member is not registered.");
    return NativeOk();
}

ScriptAbiResult NativeGetComponent(uint64_t owner, uint64_t typeId, uint64_t* output)
{
    if (!output)
        return NativeError(ScriptStatusCode::InvalidArgument, "Native component output is null.");
    auto* gameObject = NativeOwnerGameObject(owner);
    if (!gameObject)
        return NativeError(ScriptStatusCode::InvalidHandle, "Native GameObject handle is no longer alive.");
    const auto transformId = MakeStableScriptId("Nullus.Transform");
    const auto nativeTransformId = MakeStableScriptId("NLS::Engine::Components::TransformComponent");
    const auto cameraId = MakeStableScriptId("Nullus.Camera");
    const auto nativeCameraId = MakeStableScriptId("NLS::Engine::Components::CameraComponent");
    const auto lightId = MakeStableScriptId("Nullus.Light");
    const auto nativeLightId = MakeStableScriptId("NLS::Engine::Components::LightComponent");
    if (typeId == cameraId || typeId == nativeCameraId)
    {
        auto* component = gameObject->GetComponent<NLS::Engine::Components::CameraComponent>();
        *output = component ? NativeObjectHandle::FromInstanceId(component->GetInstanceID()).value : 0;
        return NativeOk();
    }
    if (typeId == lightId || typeId == nativeLightId)
    {
        auto* component = gameObject->GetComponent<NLS::Engine::Components::LightComponent>();
        *output = component ? NativeObjectHandle::FromInstanceId(component->GetInstanceID()).value : 0;
        return NativeOk();
    }
    if (typeId != transformId && typeId != nativeTransformId)
    {
        // A component call may query a native sibling.  ScriptComponent
        // queries are resolved by the runtime registry in NativeAddComponent;
        // unknown types intentionally behave like a null result.
        *output = 0;
        return NativeOk();
    }
    if (!gameObject->GetTransform())
    {
        *output = 0;
        return NativeOk();
    }
    *output = NativeObjectHandle::FromInstanceId(gameObject->GetTransform()->GetInstanceID()).value;
    return NativeOk();
}

ScriptAbiResult NativeGetInt32(uint64_t owner, uint64_t memberId, int32_t* output)
{
    if (!output)
        return NativeError(ScriptStatusCode::InvalidArgument, "Native integer output is null.");
    auto* object = NLS::Object::IDToPointerNoThreadCheck(static_cast<NLS::InstanceID>(owner & 0xFFFFFFFFu));
    if (auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object))
    {
        if (memberId != MakeStableScriptId("NLS::Engine::GameObject::GetLayer()"))
            return NativeError(ScriptStatusCode::Unsupported, "The requested Native integer member is not registered.");
        *output = gameObject->GetLayer();
        return NativeOk();
    }
    if (auto* camera = dynamic_cast<NLS::Engine::Components::CameraComponent*>(object))
    {
        if (memberId != MakeStableScriptId("NLS::Engine::Components::CameraComponent::GetProjectionMode()"))
            return NativeError(ScriptStatusCode::Unsupported, "The requested Native integer member is not registered.");
        *output = static_cast<int32_t>(camera->GetProjectionMode());
        return NativeOk();
    }
    if (auto* light = dynamic_cast<NLS::Engine::Components::LightComponent*>(object))
    {
        if (memberId != MakeStableScriptId("NLS::Engine::Components::LightComponent::GetLightType()"))
            return NativeError(ScriptStatusCode::Unsupported, "The requested Native integer member is not registered.");
        *output = static_cast<int32_t>(light->GetLightType());
        return NativeOk();
    }
    return NativeError(ScriptStatusCode::InvalidHandle, "Native integer owner is no longer alive.");
}

ScriptAbiResult NativeSetInt32(uint64_t owner, uint64_t memberId, int32_t value)
{
    auto* object = NLS::Object::IDToPointerNoThreadCheck(static_cast<NLS::InstanceID>(owner & 0xFFFFFFFFu));
    if (auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object))
    {
        if (memberId != MakeStableScriptId("NLS::Engine::GameObject::SetLayer(int)"))
            return NativeError(ScriptStatusCode::Unsupported, "The requested Native integer member is not registered.");
        gameObject->SetLayer(value);
        return NativeOk();
    }
    if (auto* camera = dynamic_cast<NLS::Engine::Components::CameraComponent*>(object))
    {
        if (memberId != MakeStableScriptId("NLS::Engine::Components::CameraComponent::SetProjectionMode(NLS::Render::Settings::EProjectionMode)"))
            return NativeError(ScriptStatusCode::Unsupported, "The requested Native integer member is not registered.");
        camera->SetProjectionMode(static_cast<NLS::Render::Settings::EProjectionMode>(value == 0 ? 0 : 1));
        return NativeOk();
    }
    if (auto* light = dynamic_cast<NLS::Engine::Components::LightComponent*>(object))
    {
        if (memberId != MakeStableScriptId("NLS::Engine::Components::LightComponent::SetLightType(NLS::Render::Settings::ELightType)"))
            return NativeError(ScriptStatusCode::Unsupported, "The requested Native integer member is not registered.");
        light->SetLightType(static_cast<NLS::Render::Settings::ELightType>(std::clamp(value, 0, 4)));
        return NativeOk();
    }
    return NativeError(ScriptStatusCode::InvalidHandle, "Native integer owner is no longer alive.");
}

ScriptAbiResult NativeGetFloat(uint64_t owner, uint64_t memberId, float* output)
{
    if (!output)
        return NativeError(ScriptStatusCode::InvalidArgument, "Native float output is null.");
    auto* object = NLS::Object::IDToPointerNoThreadCheck(static_cast<NLS::InstanceID>(owner & 0xFFFFFFFFu));
    if (auto* camera = dynamic_cast<NLS::Engine::Components::CameraComponent*>(object))
    {
        if (memberId == MakeStableScriptId("NLS::Engine::Components::CameraComponent::GetFov()")) *output = camera->GetFov();
        else if (memberId == MakeStableScriptId("NLS::Engine::Components::CameraComponent::GetSize()")) *output = camera->GetSize();
        else if (memberId == MakeStableScriptId("NLS::Engine::Components::CameraComponent::GetNear()")) *output = camera->GetNear();
        else if (memberId == MakeStableScriptId("NLS::Engine::Components::CameraComponent::GetFar()")) *output = camera->GetFar();
        else return NativeError(ScriptStatusCode::Unsupported, "The requested Native float member is not registered.");
        return NativeOk();
    }
    if (auto* light = dynamic_cast<NLS::Engine::Components::LightComponent*>(object))
    {
        if (memberId == MakeStableScriptId("NLS::Engine::Components::LightComponent::GetIntensity()")) *output = light->GetIntensity();
        else if (memberId == MakeStableScriptId("NLS::Engine::Components::LightComponent::GetRange()")) *output = light->GetRange();
        else if (memberId == MakeStableScriptId("NLS::Engine::Components::LightComponent::GetCutoff()")) *output = light->GetCutoff();
        else if (memberId == MakeStableScriptId("NLS::Engine::Components::LightComponent::GetOuterCutoff()")) *output = light->GetOuterCutoff();
        else return NativeError(ScriptStatusCode::Unsupported, "The requested Native float member is not registered.");
        return NativeOk();
    }
    return NativeError(ScriptStatusCode::InvalidHandle, "Native float owner is no longer alive.");
}

ScriptAbiResult NativeSetFloat(uint64_t owner, uint64_t memberId, float value)
{
    auto* object = NLS::Object::IDToPointerNoThreadCheck(static_cast<NLS::InstanceID>(owner & 0xFFFFFFFFu));
    if (auto* camera = dynamic_cast<NLS::Engine::Components::CameraComponent*>(object))
    {
        if (memberId == MakeStableScriptId("NLS::Engine::Components::CameraComponent::SetFov(float)")) camera->SetFov(value);
        else if (memberId == MakeStableScriptId("NLS::Engine::Components::CameraComponent::SetSize(float)")) camera->SetSize(value);
        else if (memberId == MakeStableScriptId("NLS::Engine::Components::CameraComponent::SetNear(float)")) camera->SetNear(value);
        else if (memberId == MakeStableScriptId("NLS::Engine::Components::CameraComponent::SetFar(float)")) camera->SetFar(value);
        else return NativeError(ScriptStatusCode::Unsupported, "The requested Native float member is not registered.");
        return NativeOk();
    }
    if (auto* light = dynamic_cast<NLS::Engine::Components::LightComponent*>(object))
    {
        if (memberId == MakeStableScriptId("NLS::Engine::Components::LightComponent::SetIntensity(float)")) light->SetIntensity(value);
        else if (memberId == MakeStableScriptId("NLS::Engine::Components::LightComponent::SetRange(float)")) light->SetRange(value);
        else if (memberId == MakeStableScriptId("NLS::Engine::Components::LightComponent::SetCutoff(float)")) light->SetCutoff(value);
        else if (memberId == MakeStableScriptId("NLS::Engine::Components::LightComponent::SetOuterCutoff(float)")) light->SetOuterCutoff(value);
        else return NativeError(ScriptStatusCode::Unsupported, "The requested Native float member is not registered.");
        return NativeOk();
    }
    return NativeError(ScriptStatusCode::InvalidHandle, "Native float owner is no longer alive.");
}

ScriptAbiResult NativeSetObject(uint64_t owner, uint64_t memberId, uint64_t value)
{
    auto* object = NLS::Object::IDToPointerNoThreadCheck(static_cast<NLS::InstanceID>(owner & 0xFFFFFFFFu));
    auto* transform = dynamic_cast<NLS::Engine::Components::TransformComponent*>(object);
    if (!transform)
        return NativeError(ScriptStatusCode::InvalidHandle, "Native Transform handle is no longer alive.");
    if (memberId != MakeStableScriptId("NLS::Engine::Components::TransformComponent::SetParent(NLS::Engine::Components::TransformComponent*)"))
        return NativeError(ScriptStatusCode::Unsupported, "The requested Native object setter is not registered.");
    if (value == 0)
    {
        transform->RemoveParent();
        return NativeOk();
    }
    auto* parent = NLS::Object::IDToPointer<NLS::Engine::Components::TransformComponent>(
        static_cast<NLS::InstanceID>(value & 0xFFFFFFFFu));
    if (!parent)
        return NativeError(ScriptStatusCode::InvalidHandle, "Native parent Transform handle is no longer alive.");
    transform->SetParent(*parent);
    return NativeOk();
}

ScriptAbiResult NativeInputResult(bool value, uint8_t* output)
{
    if (!output)
        return NativeError(ScriptStatusCode::InvalidArgument, "Native input output is null.");
    *output = value ? 1 : 0;
    return NativeOk();
}

ScriptAbiResult NativeGetKey(int32_t key, uint8_t* output)
{
    auto* input = ScriptEngineApi::GetInputManager();
    return NativeInputResult(input && input->GetKeyState(static_cast<NLS::Windowing::Inputs::EKey>(key)) == NLS::Windowing::Inputs::EKeyState::KEY_DOWN, output);
}

ScriptAbiResult NativeGetKeyDown(int32_t key, uint8_t* output)
{
    auto* input = ScriptEngineApi::GetInputManager();
    return NativeInputResult(input && input->IsKeyPressed(static_cast<NLS::Windowing::Inputs::EKey>(key)), output);
}

ScriptAbiResult NativeGetKeyUp(int32_t key, uint8_t* output)
{
    auto* input = ScriptEngineApi::GetInputManager();
    return NativeInputResult(input && input->IsKeyReleased(static_cast<NLS::Windowing::Inputs::EKey>(key)), output);
}

ScriptAbiResult NativeGetMouseButton(int32_t button, uint8_t* output)
{
    auto* input = ScriptEngineApi::GetInputManager();
    return NativeInputResult(input && input->GetMouseButtonState(static_cast<NLS::Windowing::Inputs::EMouseButton>(button)) == NLS::Windowing::Inputs::EMouseButtonState::MOUSE_DOWN, output);
}

ScriptAbiResult NativeGetMouseButtonDown(int32_t button, uint8_t* output)
{
    auto* input = ScriptEngineApi::GetInputManager();
    return NativeInputResult(input && input->IsMouseButtonPressed(static_cast<NLS::Windowing::Inputs::EMouseButton>(button)), output);
}

ScriptAbiResult NativeGetMouseButtonUp(int32_t button, uint8_t* output)
{
    auto* input = ScriptEngineApi::GetInputManager();
    return NativeInputResult(input && input->IsMouseButtonReleased(static_cast<NLS::Windowing::Inputs::EMouseButton>(button)), output);
}

ScriptAbiResult NativeGetMousePosition(float* x, float* y)
{
    if (!x || !y)
        return NativeError(ScriptStatusCode::InvalidArgument, "Native mouse position output is null.");
    const auto value = ScriptEngineApi::GetInputManager()
        ? ScriptEngineApi::GetInputManager()->GetMousePosition()
        : NLS::Maths::Vector2{};
    *x = value.x;
    *y = value.y;
    return NativeOk();
}

ScriptAbiResult NativeGetMouseScrollDelta(float* x, float* y)
{
    if (!x || !y)
        return NativeError(ScriptStatusCode::InvalidArgument, "Native mouse scroll output is null.");
    const auto value = ScriptEngineApi::GetInputManager()
        ? ScriptEngineApi::GetInputManager()->GetWheelMovement()
        : NLS::Maths::Vector2{};
    *x = value.x;
    *y = value.y;
    return NativeOk();
}

ScriptAbiResult NativeGetTimeScale(float* output)
{
    if (!output)
        return NativeError(ScriptStatusCode::InvalidArgument, "Native time scale output is null.");
    *output = ScriptEngineApi::GetTimeScale();
    return NativeOk();
}

ScriptAbiResult NativeSetTimeScale(float value)
{
    ScriptEngineApi::SetTimeScale(value);
    return NativeOk();
}

ScriptAbiResult NativeGetActiveScene(const char** output, uint32_t* size, uint8_t* loaded)
{
    if (!output || !size || !loaded)
        return NativeError(ScriptStatusCode::InvalidArgument, "Native active scene output is null.");
    auto* manager = ScriptEngineApi::GetSceneManager();
    if (!manager)
    {
        *output = "";
        *size = 0;
        *loaded = ScriptEngineApi::GetScene() ? 1 : 0;
        return NativeOk();
    }
    thread_local std::string path;
    path = manager->GetCurrentSceneSourcePath();
    *output = path.data();
    *size = static_cast<uint32_t>(path.size());
    *loaded = manager->HasCurrentScene() ? 1 : 0;
    return NativeOk();
}

ScriptAbiResult NativeLoadScene(const char* path)
{
    if (!path || !*path)
        return NativeError(ScriptStatusCode::InvalidArgument, "SceneManager.LoadScene requires a non-empty path.");
    return ScriptEngineApi::QueueSceneLoad(path)
        ? NativeOk()
        : NativeError(ScriptStatusCode::InvalidHandle, "No active SceneManager is available.");
}

ScriptAbiResult NativeDestroy(uint64_t owner, float delay)
{
    auto* object = NLS::Object::IDToPointerNoThreadCheck(static_cast<NLS::InstanceID>(owner & 0xFFFFFFFFu));
    if (!object)
        return NativeError(ScriptStatusCode::InvalidHandle, "Native object handle is no longer alive.");
    auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object);
    if (gameObject)
    {
        if (delay > 0.0f)
            return ScriptEngineApi::QueueDestroy({owner}, delay)
                ? NativeOk()
                : NativeError(ScriptStatusCode::InvalidArgument, "Invalid delayed GameObject destruction request.");
        gameObject->MarkAsDestroy();
        return NativeOk();
    }
    auto* component = dynamic_cast<NLS::Engine::Components::Component*>(object);
    if (!component || !component->gameobject())
        return NativeError(ScriptStatusCode::InvalidHandle, "Native Component handle is no longer alive.");
    if (delay > 0.0f)
        return ScriptEngineApi::QueueDestroy({owner}, delay)
            ? NativeOk()
            : NativeError(ScriptStatusCode::InvalidArgument, "Invalid delayed Component destruction request.");
    if (component->GetType() == NLS_TYPEOF(NLS::Engine::Components::TransformComponent))
        return NativeError(ScriptStatusCode::Unsupported, "Transform cannot be destroyed.");
    return component->gameobject()->RemoveComponent(component)
        ? NativeOk()
        : NativeError(ScriptStatusCode::InvalidHandle, "Native Component could not be destroyed.");
}

NLS::Engine::GameObject* CloneGameObjectTree(
    NLS::Engine::GameObject& source,
    NLS::Engine::SceneSystem::Scene& scene,
    NLS::Engine::GameObject* parent)
{
    auto& clone = scene.CreateGameObject(source.GetName(), source.GetTag());
    clone.SetLayer(source.GetLayer());
    clone.SetActive(source.GetActive());
    if (source.GetTransform() && clone.GetTransform())
    {
        clone.GetTransform()->SetLocalPosition(source.GetTransform()->GetLocalPosition());
        clone.GetTransform()->SetLocalRotation(source.GetTransform()->GetLocalRotation());
        clone.GetTransform()->SetLocalScale(source.GetTransform()->GetLocalScale());
    }
    if (parent)
        clone.SetParent(*parent);
    for (const auto& component : source.GetComponents())
    {
        if (auto* script = dynamic_cast<NLS::Scripting::ScriptComponent*>(component.get()))
        {
            auto* copy = clone.AddComponent<NLS::Scripting::ScriptComponent>();
            if (copy)
            {
                copy->SetScriptAsset(script->GetScriptAsset());
                copy->GetSerializedFields() = script->GetSerializedFields();
                copy->GetOrphanFields() = script->GetOrphanFields();
            }
        }
    }
    for (auto* child : source.GetChildren())
        if (child)
            CloneGameObjectTree(*child, scene, &clone);
    return &clone;
}

ScriptAbiResult NativeInstantiate(uint64_t owner, uint64_t* output)
{
    if (!output)
        return NativeError(ScriptStatusCode::InvalidArgument, "Native instantiate output is null.");
    *output = 0;
    auto* source = NLS::Object::IDToPointer<NLS::Engine::GameObject>(static_cast<NLS::InstanceID>(owner & 0xFFFFFFFFu));
    if (!source || !source->GetScene())
        return NativeError(ScriptStatusCode::InvalidHandle, "Native GameObject is not attached to a scene.");
    auto* clone = CloneGameObjectTree(*source, *source->GetScene(), nullptr);
    if (!clone)
        return NativeError(ScriptStatusCode::RuntimeError, "Native GameObject instantiate failed.");
    *output = NativeObjectHandle::FromInstanceId(clone->GetInstanceID()).value;
    return NativeOk();
}

ScriptAbiResult NativeFind(const char* name, uint64_t* output)
{
    if (!output)
        return NativeError(ScriptStatusCode::InvalidArgument, "Native find output is null.");
    *output = 0;
    if (!g_nativeScriptScene)
        return NativeOk();
    if (auto* result = g_nativeScriptScene->FindGameObjectByName(name ? name : ""))
        *output = NativeObjectHandle::FromInstanceId(result->GetInstanceID()).value;
    return NativeOk();
}

ScriptAbiResult NativeFindWithTag(const char* tag, uint64_t* output)
{
    if (!output)
        return NativeError(ScriptStatusCode::InvalidArgument, "Native tag lookup output is null.");
    *output = 0;
    if (!g_nativeScriptScene)
        return NativeOk();
    if (auto* result = g_nativeScriptScene->FindGameObjectByTag(tag ? tag : ""))
        *output = NativeObjectHandle::FromInstanceId(result->GetInstanceID()).value;
    return NativeOk();
}

ScriptAbiResult NativeAddComponent(uint64_t owner, uint64_t typeId, uint64_t* output)
{
    if (!output)
        return NativeError(ScriptStatusCode::InvalidArgument, "Native AddComponent output is null.");
    *output = 0;
    auto* gameObject = NLS::Object::IDToPointer<NLS::Engine::GameObject>(static_cast<NLS::InstanceID>(owner & 0xFFFFFFFFu));
    if (!gameObject)
        return NativeError(ScriptStatusCode::InvalidHandle, "Native GameObject handle is no longer alive.");
    if (typeId == MakeStableScriptId("Nullus.Transform") ||
        typeId == MakeStableScriptId("NLS::Engine::Components::TransformComponent"))
    {
        *output = gameObject->GetTransform()
            ? NativeObjectHandle::FromInstanceId(gameObject->GetTransform()->GetInstanceID()).value
            : 0;
        return NativeOk();
    }
    if (typeId == MakeStableScriptId("Nullus.Camera") ||
        typeId == MakeStableScriptId("NLS::Engine::Components::CameraComponent"))
    {
        auto* component = gameObject->GetComponent<NLS::Engine::Components::CameraComponent>();
        if (!component)
            component = gameObject->AddComponent<NLS::Engine::Components::CameraComponent>();
        *output = component ? NativeObjectHandle::FromInstanceId(component->GetInstanceID()).value : 0;
        return component ? NativeOk() : NativeError(ScriptStatusCode::RuntimeError, "Native Camera creation failed.");
    }
    if (typeId == MakeStableScriptId("Nullus.Light") ||
        typeId == MakeStableScriptId("NLS::Engine::Components::LightComponent"))
    {
        auto* component = gameObject->GetComponent<NLS::Engine::Components::LightComponent>();
        if (!component)
            component = gameObject->AddComponent<NLS::Engine::Components::LightComponent>();
        *output = component ? NativeObjectHandle::FromInstanceId(component->GetInstanceID()).value : 0;
        return component ? NativeOk() : NativeError(ScriptStatusCode::RuntimeError, "Native Light creation failed.");
    }
    const ManagedBehaviourManifestEntry* manifestEntry = nullptr;
    if (g_activeCoreClrBackend)
    {
        for (const auto& entry : g_activeCoreClrBackend->GetBehaviourManifest())
        {
            if (entry.scriptTypeId == typeId ||
                MakeStableScriptId(entry.typeName) == typeId ||
                MakeStableScriptId("global::" + entry.typeName) == typeId)
            {
                manifestEntry = &entry;
                break;
            }
        }
    }
    if (manifestEntry && manifestEntry->isComponent)
    {
        ScriptAsset asset;
        if (const auto* registered = FindRegisteredScriptAsset(manifestEntry->scriptTypeId))
            asset = *registered;
        else
        {
            asset.language = ScriptLanguage::CSharp;
            asset.sourcePath = manifestEntry->assetPath;
            asset.scriptType = manifestEntry->scriptTypeId;
            asset.isComponent = true;
            asset.assetId = NLS::Core::Assets::AssetId(
                NLS::Guid::NewDeterministic("Nullus.RuntimeScript:" + asset.sourcePath));
            std::ifstream source(asset.sourcePath, std::ios::binary);
            if (source)
                asset.sourceText.assign(std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>());
        }
        auto* script = gameObject->AddComponent<ScriptComponent>();
        if (!script)
            return NativeError(ScriptStatusCode::RuntimeError, "Native ScriptComponent creation failed.");
        script->SetScriptAsset(std::move(asset));
        if (auto* runtime = GetActiveScriptRuntime())
            script->SetRuntime(runtime);
        *output = NativeObjectHandle::FromInstanceId(script->GetInstanceID()).value;
        return NativeOk();
    }
    return NativeError(ScriptStatusCode::Unsupported, "The requested component type is not registered with the Native scripting runtime.");
}

}

void SetNativeScriptScene(NLS::Engine::SceneSystem::Scene* scene)
{
    g_nativeScriptScene = scene;
    ScriptEngineApi::SetScene(scene);
}

std::filesystem::path CoreClrHost::LocateHostFxr(const std::filesystem::path& dotnetRoot)
{
    auto root = dotnetRoot;
    if (root.empty())
    {
        if (const auto* environmentRoot = std::getenv("DOTNET_ROOT_X64"); environmentRoot && *environmentRoot)
            root = environmentRoot;
    }
    if (root.empty())
    {
        if (const auto* environmentRoot = std::getenv("DOTNET_ROOT"); environmentRoot && *environmentRoot)
            root = environmentRoot;
    }
    if (root.empty())
    {
#if defined(_WIN32)
        root = "C:/Program Files/dotnet";
#elif defined(__APPLE__)
        root = "/usr/local/share/dotnet";
#else
        root = "/usr/share/dotnet";
#endif
    }

#if defined(_WIN32)
    constexpr auto fileName = "host/fxr";
    constexpr auto libraryName = "hostfxr.dll";
#elif defined(__APPLE__)
    constexpr auto fileName = "host/fxr";
    constexpr auto libraryName = "libhostfxr.dylib";
#else
    constexpr auto fileName = "host/fxr";
    constexpr auto libraryName = "libhostfxr.so";
#endif
    if (!root.empty() && std::filesystem::exists(root / libraryName))
        return root / libraryName;
    const auto fxrRoot = root / fileName;
    if (!std::filesystem::exists(fxrRoot))
        return {};
    std::vector<std::filesystem::path> versions;
    for (const auto& version : std::filesystem::directory_iterator(fxrRoot))
        versions.push_back(version.path());
    std::sort(versions.begin(), versions.end(), std::greater<>());
    for (const auto& version : versions)
    {
        const auto candidate = version / libraryName;
        if (std::filesystem::exists(candidate))
            return candidate;
    }
    return {};
}

ScriptStatus CoreClrHost::Load(const std::filesystem::path& runtimeConfig, const std::filesystem::path& assembly)
{
    if (runtimeConfig.empty() || assembly.empty() || !std::filesystem::exists(runtimeConfig) || !std::filesystem::exists(assembly))
        return ScriptStatus::Error(ScriptStatusCode::AssetNotLoaded, "CoreCLR runtimeconfig or managed assembly was not found.");
    if (m_loaded)
        return ScriptStatus::Error(ScriptStatusCode::AlreadyInitialized, "CoreCLR host is already loaded.");

    g_hostFxrError.clear();
    const auto hostFxrPath = LocateHostFxr(m_dotnetRoot);
    if (hostFxrPath.empty())
        return ScriptStatus::Error(ScriptStatusCode::BackendUnavailable, "hostfxr for the configured .NET runtime was not found.");

    if (g_sharedHostFxr.context && g_sharedHostFxr.hostFxrPath == hostFxrPath)
    {
        m_runtimeConfig = runtimeConfig;
        m_assembly = assembly;
        m_library = g_sharedHostFxr.library;
        m_context = g_sharedHostFxr.context;
        m_close = reinterpret_cast<HostFxrCloseFn>(g_sharedHostFxr.close);
        m_getRuntimeDelegate = reinterpret_cast<HostFxrGetRuntimeDelegateFn>(g_sharedHostFxr.getRuntimeDelegate);
        m_setErrorWriter = reinterpret_cast<HostFxrSetErrorWriterFn>(g_sharedHostFxr.setErrorWriter);
        m_loadAssemblyAndGetFunctionPointer = reinterpret_cast<LoadAssemblyAndGetFunctionPointerFn>(g_sharedHostFxr.loadAssemblyAndGetFunctionPointer);
        ++g_sharedHostFxr.references;
        m_loaded = true;
        return ScriptStatus::Ok();
    }

#if defined(_WIN32)
    auto* library = static_cast<void*>(LoadLibraryW(hostFxrPath.c_str()));
    const auto resolve = [library](const char* name) -> void*
    {
        return library ? reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name)) : nullptr;
    };
#else
    auto* library = dlopen(hostFxrPath.string().c_str(), RTLD_NOW | RTLD_LOCAL);
    const auto resolve = [library](const char* name) -> void*
    {
        return library ? dlsym(library, name) : nullptr;
    };
#endif
    using HostFxrInitializeFn = int32_t (*)(const char*, const void*, void**);
    auto* initialize = resolve("hostfxr_initialize_for_runtime_config");
    auto* close = resolve("hostfxr_close");
    auto* getRuntimeDelegate = resolve("hostfxr_get_runtime_delegate");
    auto* setErrorWriter = resolve("hostfxr_set_error_writer");
    if (!initialize || !close || !getRuntimeDelegate)
    {
#if defined(_WIN32)
        if (library) FreeLibrary(static_cast<HMODULE>(library));
#else
        if (library) dlclose(library);
#endif
        return ScriptStatus::Error(ScriptStatusCode::BackendUnavailable, "hostfxr does not expose the required initialization entry points.");
    }

    void* context = nullptr;
    // hostfxr uses char_t, which is wchar_t on Windows and char elsewhere.
    // The filesystem path representation matches that ABI on each platform.
#if defined(_WIN32)
    using HostFxrInitializeWideFn = int32_t (*)(const wchar_t*, const void*, void**);
    const auto initializeWide = reinterpret_cast<HostFxrInitializeWideFn>(initialize);
    // hostfxr accepts a null parameter block only for the first runtime
    // initialization in a process.  Script tests and editor play contexts can
    // create more than one ScriptRuntime, so always provide the versioned
    // initialization parameters and pin framework resolution to our bundled
    // dotnet root.
    struct HostFxrInitializeParameters
    {
        size_t size;
        const wchar_t* hostPath;
        const wchar_t* dotnetRoot;
    };
    const auto dotnetRoot = m_dotnetRoot.empty() ? std::wstring{} : m_dotnetRoot.wstring();
    const HostFxrInitializeParameters parameters{
        sizeof(HostFxrInitializeParameters),
        assembly.c_str(),
        dotnetRoot.empty() ? nullptr : dotnetRoot.c_str()};
    const auto result = initializeWide(runtimeConfig.c_str(), &parameters, &context);
#else
    const auto initializeNarrow = reinterpret_cast<HostFxrInitializeFn>(initialize);
    struct HostFxrInitializeParameters
    {
        size_t size;
        const char* hostPath;
        const char* dotnetRoot;
    };
    const auto dotnetRoot = m_dotnetRoot.empty() ? std::string{} : m_dotnetRoot.string();
    const auto hostPath = assembly.string();
    const HostFxrInitializeParameters parameters{
        sizeof(HostFxrInitializeParameters),
        hostPath.c_str(),
        dotnetRoot.empty() ? nullptr : dotnetRoot.c_str()};
    const auto runtimeConfigString = runtimeConfig.string();
    const auto result = initializeNarrow(runtimeConfigString.c_str(), &parameters, &context);
#endif
    if (result != 0 || !context)
    {
#if defined(_WIN32)
        if (library) FreeLibrary(static_cast<HMODULE>(library));
#else
        if (library) dlclose(library);
#endif
        return ScriptStatus::Error(ScriptStatusCode::BackendUnavailable, "hostfxr failed to initialize the configured runtimeconfig.");
    }

    m_runtimeConfig = runtimeConfig;
    m_assembly = assembly;
    m_library = library;
    m_context = context;
    m_close = reinterpret_cast<HostFxrCloseFn>(close);
    m_getRuntimeDelegate = reinterpret_cast<HostFxrGetRuntimeDelegateFn>(getRuntimeDelegate);
    m_setErrorWriter = reinterpret_cast<HostFxrSetErrorWriterFn>(setErrorWriter);
    if (m_setErrorWriter)
        (void)m_setErrorWriter(&HostFxrErrorWriter);
    void* loadAssemblyDelegate = nullptr;
    if (m_getRuntimeDelegate(
            m_context,
            kHostFxrDelegateLoadAssemblyAndGetFunctionPointer,
            &loadAssemblyDelegate) != 0
        || !loadAssemblyDelegate)
    {
        Unload();
        return ScriptStatus::Error(ScriptStatusCode::BackendUnavailable, "hostfxr could not provide load_assembly_and_get_function_pointer.");
    }
    m_loadAssemblyAndGetFunctionPointer = reinterpret_cast<LoadAssemblyAndGetFunctionPointerFn>(loadAssemblyDelegate);

    g_sharedHostFxr.library = m_library;
    g_sharedHostFxr.context = m_context;
    g_sharedHostFxr.close = reinterpret_cast<void*>(m_close);
    g_sharedHostFxr.getRuntimeDelegate = reinterpret_cast<void*>(m_getRuntimeDelegate);
    g_sharedHostFxr.setErrorWriter = reinterpret_cast<void*>(m_setErrorWriter);
    g_sharedHostFxr.loadAssemblyAndGetFunctionPointer = reinterpret_cast<void*>(m_loadAssemblyAndGetFunctionPointer);
    g_sharedHostFxr.hostFxrPath = hostFxrPath;
    g_sharedHostFxr.references = 1;
    m_loaded = true;
    return ScriptStatus::Ok();
}

ScriptStatus CoreClrHost::GetUnmanagedFunctionPointer(
    const std::filesystem::path& assembly,
    std::string_view typeName,
    std::string_view methodName,
    void*& output) const
{
    output = nullptr;
    if (!m_loaded || !m_loadAssemblyAndGetFunctionPointer)
        return ScriptStatus::Error(ScriptStatusCode::BackendClosed, "CoreCLR host is not loaded.");
    if (assembly.empty() || typeName.empty() || methodName.empty())
        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "Managed function resolution requires an assembly, type, and method.");
    if (!std::filesystem::exists(assembly))
        return ScriptStatus::Error(ScriptStatusCode::AssetNotLoaded, "Managed assembly was not found while resolving an ABI entry point.");

#if defined(_WIN32)
    const auto toWide = [](std::string_view value)
    {
        if (value.empty())
            return std::wstring{};
        const auto required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (required <= 0)
            return std::wstring{};
        std::wstring result(static_cast<size_t>(required), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), required) <= 0)
            return std::wstring{};
        return result;
    };
    const auto typeWide = toWide(typeName);
    const auto methodWide = toWide(methodName);
    if (typeWide.empty() || methodWide.empty())
        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "Managed type or method name is not valid UTF-8.");
    const auto result = m_loadAssemblyAndGetFunctionPointer(
        assembly.c_str(), typeWide.c_str(), methodWide.c_str(), reinterpret_cast<const wchar_t*>(-1), nullptr, &output);
#else
    const auto typeString = std::string(typeName);
    const auto methodString = std::string(methodName);
    const auto result = m_loadAssemblyAndGetFunctionPointer(
        assembly.string().c_str(), typeString.c_str(), methodString.c_str(), reinterpret_cast<const char*>(-1), nullptr, &output);
#endif
    if (result != 0 || !output)
    {
        output = nullptr;
        const auto detail = g_hostFxrError.empty() ? std::string{} : " " + g_hostFxrError;
        return ScriptStatus::Error(
            ScriptStatusCode::BackendUnavailable,
            "hostfxr failed to resolve the managed ABI entry point (result=" + std::to_string(result) + ")." + detail);
    }
    return ScriptStatus::Ok();
}

CoreClrHost::~CoreClrHost()
{
    Unload();
}

void CoreClrHost::Unload()
{
    const bool sharedContext = m_context && g_sharedHostFxr.context == m_context;
    if (sharedContext)
    {
        if (g_sharedHostFxr.references > 0)
            --g_sharedHostFxr.references;
        // Do not close or unload the shared hostfxr context.  CoreCLR remains
        // loaded after the first managed call and hostfxr cannot initialize a
        // fresh context in the same process; the OS reclaims this process-wide
        // host state at exit.
    }
    else
    {
        if (m_setErrorWriter)
            (void)m_setErrorWriter(nullptr);
        if (m_context && m_close)
            (void)m_close(m_context);
#if defined(_WIN32)
        if (m_library)
            FreeLibrary(static_cast<HMODULE>(m_library));
#else
        if (m_library)
            dlclose(m_library);
#endif
    }
    m_close = nullptr;
    m_getRuntimeDelegate = nullptr;
    m_setErrorWriter = nullptr;
    m_loadAssemblyAndGetFunctionPointer = nullptr;
    m_context = nullptr;
    m_library = nullptr;
    m_loaded = false;
    m_runtimeConfig.clear();
    m_assembly.clear();
}

CoreClrScriptBackend::CoreClrScriptBackend(ScriptBackendId id)
    : m_id(id)
{
    g_activeCoreClrBackend = this;
    InstallDefaultNativeApi();
}

void CoreClrScriptBackend::InstallDefaultNativeApi()
{
    if (!m_nativeApi.isAlive)
        m_nativeApi.isAlive = &NativeIsAlive;
    if (!m_nativeApi.getObject)
        m_nativeApi.getObject = &NativeGetObject;
    if (!m_nativeApi.getVector3)
        m_nativeApi.getVector3 = &NativeGetVector3;
    if (!m_nativeApi.setVector3)
        m_nativeApi.setVector3 = &NativeSetVector3;
    if (!m_nativeApi.setBool)
        m_nativeApi.setBool = &NativeSetBool;
    if (!m_nativeApi.getQuaternion)
        m_nativeApi.getQuaternion = &NativeGetQuaternion;
    if (!m_nativeApi.setQuaternion)
        m_nativeApi.setQuaternion = &NativeSetQuaternion;
    if (!m_nativeApi.createPrimitive)
        m_nativeApi.createPrimitive = &NativeCreatePrimitive;
    if (!m_nativeApi.getString)
        m_nativeApi.getString = &NativeGetString;
    if (!m_nativeApi.setString)
        m_nativeApi.setString = &NativeSetString;
    if (!m_nativeApi.getBool)
        m_nativeApi.getBool = &NativeGetBool;
    if (!m_nativeApi.getComponent)
        m_nativeApi.getComponent = &NativeGetComponent;
    if (!m_nativeApi.getInt32)
        m_nativeApi.getInt32 = &NativeGetInt32;
    if (!m_nativeApi.setInt32)
        m_nativeApi.setInt32 = &NativeSetInt32;
    if (!m_nativeApi.getFloat)
        m_nativeApi.getFloat = &NativeGetFloat;
    if (!m_nativeApi.setFloat)
        m_nativeApi.setFloat = &NativeSetFloat;
    if (!m_nativeApi.destroy)
        m_nativeApi.destroy = &NativeDestroy;
    if (!m_nativeApi.instantiate)
        m_nativeApi.instantiate = &NativeInstantiate;
    if (!m_nativeApi.find)
        m_nativeApi.find = &NativeFind;
    if (!m_nativeApi.findWithTag)
        m_nativeApi.findWithTag = &NativeFindWithTag;
    if (!m_nativeApi.addComponent)
        m_nativeApi.addComponent = &NativeAddComponent;
    if (!m_nativeApi.setObject)
        m_nativeApi.setObject = &NativeSetObject;
    if (!m_nativeApi.getKey)
        m_nativeApi.getKey = &NativeGetKey;
    if (!m_nativeApi.getKeyDown)
        m_nativeApi.getKeyDown = &NativeGetKeyDown;
    if (!m_nativeApi.getKeyUp)
        m_nativeApi.getKeyUp = &NativeGetKeyUp;
    if (!m_nativeApi.getMouseButton)
        m_nativeApi.getMouseButton = &NativeGetMouseButton;
    if (!m_nativeApi.getMouseButtonDown)
        m_nativeApi.getMouseButtonDown = &NativeGetMouseButtonDown;
    if (!m_nativeApi.getMouseButtonUp)
        m_nativeApi.getMouseButtonUp = &NativeGetMouseButtonUp;
    if (!m_nativeApi.getMousePosition)
        m_nativeApi.getMousePosition = &NativeGetMousePosition;
    if (!m_nativeApi.getMouseScrollDelta)
        m_nativeApi.getMouseScrollDelta = &NativeGetMouseScrollDelta;
    if (!m_nativeApi.getTimeScale)
        m_nativeApi.getTimeScale = &NativeGetTimeScale;
    if (!m_nativeApi.setTimeScale)
        m_nativeApi.setTimeScale = &NativeSetTimeScale;
    if (!m_nativeApi.createGameObject)
        m_nativeApi.createGameObject = &NativeCreateGameObject;
    if (!m_nativeApi.getActiveScene)
        m_nativeApi.getActiveScene = &NativeGetActiveScene;
    if (!m_nativeApi.loadScene)
        m_nativeApi.loadScene = &NativeLoadScene;
}

ScriptStatus CoreClrScriptBackend::ValidateTable(bool validateSchema) const
{
    constexpr size_t requiredSize = offsetof(ManagedApiTable, invoke) + sizeof(ManagedInvokeFn);
    if (m_managedApi.header.abiVersion != 2 || m_managedApi.header.size < requiredSize)
        return ScriptStatus::Error(ScriptStatusCode::SchemaMismatch, "Managed scripting ABI version or table size is unsupported.");
    if (validateSchema && m_managedApi.header.schemaHash != m_api.GetSchemaHash())
        return ScriptStatus::Error(ScriptStatusCode::SchemaMismatch, "Managed scripting schema hash does not match Native.");
    if (!m_managedApi.initialize || !m_managedApi.shutdown || !m_managedApi.loadScript || !m_managedApi.createInstance
        || !m_managedApi.destroyInstance || !m_managedApi.invoke)
        return ScriptStatus::Error(ScriptStatusCode::BackendUnavailable, "Managed scripting API table is incomplete.");
    return ScriptStatus::Ok();
}

ScriptStatus CoreClrScriptBackend::Initialize(const ScriptApiDatabase& api)
{
#if !NLS_ENABLE_CSHARP_SCRIPTING
    (void)api;
    return ScriptStatus::Error(ScriptStatusCode::BackendUnavailable, "C# scripting is disabled by NLS_ENABLE_CSHARP_SCRIPTING.");
#else
    if (m_initialized)
        return ScriptStatus::Error(ScriptStatusCode::AlreadyInitialized, "CoreCLR backend is already initialized.");
    m_api = api;
    m_nativeApi.header.schemaHash = m_api.GetSchemaHash();
    InstallDefaultNativeApi();
    if (!m_managedApi.initialize && (!m_runtimeConfig.empty() || !m_assembly.empty()))
    {
        const auto bindStatus = BindManagedApi();
        if (!bindStatus.Succeeded())
            return bindStatus;
    }
    const auto tableStatus = ValidateTable(false);
    if (!tableStatus.Succeeded())
        return tableStatus;
    // BindManagedApi may already have loaded hostfxr in order to resolve the
    // managed entry point.  Loading the same runtime context a second time is
    // rejected by hostfxr and used to make the normal configured startup path
    // fail with AlreadyInitialized.
    if (!m_host.IsLoaded() && (!m_runtimeConfig.empty() || !m_assembly.empty()))
    {
        const auto hostStatus = m_host.Load(m_runtimeConfig, m_assembly);
        if (!hostStatus.Succeeded())
            return hostStatus;
    }
    const auto result = m_managedApi.initialize(&m_managedApi.header, m_api.GetSchemaHashHex().c_str(), &m_nativeApi);
    auto status = FromAbiResult(result, ScriptLanguage::CSharp);
    if (status.Succeeded())
    {
        const auto schemaStatus = ValidateTable(true);
        if (!schemaStatus.Succeeded())
            return schemaStatus;
        const auto manifestStatus = RefreshBehaviourManifest();
        if (!manifestStatus.Succeeded() && manifestStatus.code != ScriptStatusCode::Unsupported)
            return manifestStatus;
        m_initialized = true;
    }
    return status;
#endif
}

void CoreClrScriptBackend::Shutdown()
{
    if (m_initialized && m_managedApi.shutdown)
        (void)m_managedApi.shutdown();
    m_instances.clear();
    m_assets.clear();
    m_destroyed.clear();
    m_host.Unload();
    if (g_activeCoreClrBackend == this)
        g_activeCoreClrBackend = nullptr;
    m_lastDiagnostic.reset();
    m_behaviourManifest.clear();
    m_behaviourManifestLoaded = false;
    m_initialized = false;
}

ScriptStatus CoreClrScriptBackend::CaptureFrame(const ScriptFrameContext& frame)
{
    if (!m_initialized)
        return ScriptStatus::Error(ScriptStatusCode::BackendClosed, "CoreCLR backend is not initialized.");
    m_frames.push_back(frame);
    return ScriptStatus::Ok();
}

ScriptStatus CoreClrScriptBackend::LoadScript(const ScriptAsset& asset)
{
    if (!m_initialized)
        return ScriptStatus::Error(ScriptStatusCode::BackendClosed, "CoreCLR backend is not initialized.");
    if (asset.language != ScriptLanguage::CSharp || !asset.assetId.IsValid())
        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "C# assets require a valid AssetId.");
    uint64_t contentHash = asset.contentHash != 0 ? asset.contentHash : MakeScriptContentHash(asset.sourceText);
    const auto result = m_managedApi.loadScript(AssetKey(asset.assetId), asset.sourcePath.c_str(), asset.sourceText.c_str(), &contentHash);
    const auto status = FromAbiResult(result, ScriptLanguage::CSharp);
    if (status.Succeeded())
        m_assets[asset.assetId] = {asset, contentHash};
    return status;
}

ScriptStatus CoreClrScriptBackend::UnloadScript(const NLS::Core::Assets::AssetId& assetId)
{
    m_assets.erase(assetId);
    return ScriptStatus::Ok();
}

ScriptStatus CoreClrScriptBackend::CreateInstance(const ScriptAsset& asset, NativeObjectHandle owner, ScriptInstanceHandle& output)
{
    output = {};
    const auto loaded = m_assets.find(asset.assetId);
    if (!m_initialized)
        return ScriptStatus::Error(ScriptStatusCode::BackendClosed, "CoreCLR backend is not initialized.");
    if (loaded == m_assets.end())
        return ScriptStatus::Error(ScriptStatusCode::AssetNotLoaded, "C# ScriptAsset has not been loaded.");
    uint64_t managedToken = 0;
    const auto result = m_managedApi.createInstance(AssetKey(asset.assetId), owner.value, &managedToken);
    auto status = FromAbiResult(result, ScriptLanguage::CSharp);
    if (!status.Succeeded())
        return status;
    if (managedToken == 0)
        return ScriptStatus::Error(ScriptStatusCode::RuntimeError, "Managed runtime returned an empty instance token.");
    output = {m_id.value, m_nextGeneration++, m_nextIndex++};
    m_instances.emplace(output, Instance{managedToken, owner, loaded->second.asset});
    return ScriptStatus::Ok();
}

ScriptStatus CoreClrScriptBackend::DestroyInstance(ScriptInstanceHandle instance)
{
    const auto found = m_instances.find(instance);
    if (found == m_instances.end())
        return !instance.IsValid() || m_destroyed.contains(instance)
            ? ScriptStatus::Ok()
            : ScriptStatus::Error(ScriptStatusCode::InvalidHandle, "Managed instance handle is invalid.");
    const auto status = FromAbiResult(m_managedApi.destroyInstance(found->second.managedToken), ScriptLanguage::CSharp, instance);
    if (status.Succeeded())
    {
        m_instances.erase(found);
        m_destroyed.insert(instance);
    }
    return status;
}

ScriptStatus CoreClrScriptBackend::Invoke(ScriptInstanceHandle instance, ScriptCallback callback, const ScriptInvocationContext& context)
{
    const auto found = m_instances.find(instance);
    if (found == m_instances.end())
        return ScriptStatus::Error(ScriptStatusCode::InvalidHandle, "Managed instance handle is invalid.");
    ++m_invokeCallCount;
    return FromAbiResult(m_managedApi.invoke(found->second.managedToken, static_cast<uint16_t>(callback), &context.frame, context.owner.value), ScriptLanguage::CSharp, instance);
}

ScriptStatus CoreClrScriptBackend::InvokeBatch(
    ScriptCallback callback,
    std::span<const ScriptInstanceHandle> instances,
    const ScriptInvocationContext& context)
{
    if (instances.empty())
        return ScriptStatus::Ok();
    if (!m_initialized)
        return ScriptStatus::Error(ScriptStatusCode::BackendClosed, "CoreCLR backend is not initialized.");
    if (!m_managedApi.invokeBatch)
        return IScriptBackend::InvokeBatch(callback, instances, context);
    if (instances.size() > (std::numeric_limits<uint32_t>::max)())
        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "Managed batch invocation exceeds the ABI count limit.");

    m_batchTokens.clear();
    m_batchOwners.clear();
    m_batchTokens.reserve(instances.size());
    m_batchOwners.reserve(instances.size());
    for (size_t index = 0; index < instances.size(); ++index)
    {
        const auto found = m_instances.find(instances[index]);
        if (found == m_instances.end())
            return ScriptStatus::Error(
                m_destroyed.contains(instances[index]) ? ScriptStatusCode::AlreadyDestroyed : ScriptStatusCode::InvalidHandle,
                "Managed batch contains an invalid instance handle.");
        m_batchTokens.push_back(found->second.managedToken);
        m_batchOwners.push_back(
            context.batchOwners.size() == instances.size()
                ? context.batchOwners[index].value
                : found->second.owner.value);
    }

    ++m_batchCallCount;
    m_batchInstanceCount += instances.size();
    const auto result = m_managedApi.invokeBatch(
        static_cast<uint16_t>(callback),
        m_batchTokens.data(),
        m_batchOwners.data(),
        static_cast<uint32_t>(instances.size()),
        &context.frame);
    return FromAbiResult(result, ScriptLanguage::CSharp, instances.front());
}

ScriptStatus CoreClrScriptBackend::Reload(const NLS::Core::Assets::AssetId& assetId, const ScriptApiDatabase& api)
{
    if (!m_initialized || (!m_managedApi.reloadAssembly && !m_managedApi.reload))
        return ScriptStatus::Error(ScriptStatusCode::Unsupported, "Managed backend does not provide hot reload.");
    const auto asset = m_assets.find(assetId);
    if (asset == m_assets.end())
        return ScriptStatus::Error(ScriptStatusCode::AssetNotLoaded, "C# ScriptAsset has not been loaded.");
    if (api.GetSchemaHash() != m_api.GetSchemaHash())
        return ScriptStatus::Error(ScriptStatusCode::SchemaMismatch, "Managed reload schema hash mismatch.");
    ScriptAbiResult result;
    if (m_managedApi.reloadAssembly && !m_assembly.empty())
        result = m_managedApi.reloadAssembly(AssetKey(assetId), m_assembly.string().c_str(), api.GetSchemaHashHex().c_str());
    else if (m_managedApi.reload)
        result = m_managedApi.reload(AssetKey(assetId), asset->second.asset.sourceText.c_str(), api.GetSchemaHashHex().c_str());
    else
        return ScriptStatus::Error(ScriptStatusCode::Unsupported, "Managed backend has no compatible hot reload entry point.");
    auto status = FromAbiResult(result, ScriptLanguage::CSharp);
    if (status.Succeeded())
    {
        const auto manifestStatus = RefreshBehaviourManifest();
        if (!manifestStatus.Succeeded() && manifestStatus.code != ScriptStatusCode::Unsupported)
            status = manifestStatus;
    }
    if (status.Succeeded())
        for (auto& [instance, state] : m_instances)
        {
            (void)instance;
            if (state.asset.assetId == assetId)
                state.asset = asset->second.asset;
        }
    return status;
}

ScriptStatus CoreClrScriptBackend::ReloadProjectAssembly(
    const std::filesystem::path& assembly,
    const ScriptApiDatabase& api)
{
    if (!m_initialized || !m_managedApi.reloadAssembly)
        return ScriptStatus::Error(ScriptStatusCode::Unsupported, "Managed backend does not provide project assembly reload.");
    if (assembly.empty() || !std::filesystem::exists(assembly))
        return ScriptStatus::Error(ScriptStatusCode::AssetNotLoaded, "Managed replacement assembly was not found.");
    if (api.GetSchemaHash() != m_api.GetSchemaHash())
        return ScriptStatus::Error(ScriptStatusCode::SchemaMismatch, "Managed project reload schema hash mismatch.");

    ScriptInvocationContext context;
    ScriptStatus firstLifecycleError = ScriptStatus::Ok();
    for (const auto& [instance, state] : m_instances)
    {
        const auto status = Invoke(instance, ScriptCallback::OnDisable, context);
        if (!status.Succeeded() && firstLifecycleError.Succeeded())
            firstLifecycleError = status;
        (void)state;
    }

    const auto result = m_managedApi.reloadAssembly(
        0,
        assembly.string().c_str(),
        api.GetSchemaHashHex().c_str());
    auto status = FromAbiResult(result, ScriptLanguage::CSharp);
    if (status.Succeeded())
    {
        m_assembly = assembly;
        const auto manifestStatus = RefreshBehaviourManifest();
        if (!manifestStatus.Succeeded() && manifestStatus.code != ScriptStatusCode::Unsupported)
            status = manifestStatus;
        for (const auto& [instance, state] : m_instances)
        {
            const auto enableStatus = Invoke(instance, ScriptCallback::OnEnable, context);
            if (!enableStatus.Succeeded() && firstLifecycleError.Succeeded())
                firstLifecycleError = enableStatus;
            (void)state;
        }
        if (!firstLifecycleError.Succeeded())
            status = firstLifecycleError;
    }
    else
    {
        // The managed side commits the replacement transactionally.  If it
        // rejects the new assembly, restore the old callback state before
        // returning to the scheduler.
        for (const auto& [instance, state] : m_instances)
        {
            const auto enableStatus = Invoke(instance, ScriptCallback::OnEnable, context);
            if (!enableStatus.Succeeded() && firstLifecycleError.Succeeded())
                firstLifecycleError = enableStatus;
            (void)state;
        }
    }
    return status;
}

ScriptStatus CoreClrScriptBackend::RefreshBehaviourManifest()
{
    m_behaviourManifest.clear();
    m_scriptTypes.clear();
    m_behaviourManifestLoaded = false;
    if (!m_managedApi.getBehaviourManifest)
        return ScriptStatus::Error(ScriptStatusCode::Unsupported, "Managed ABI does not provide a Behaviour manifest.");

    const char* data = nullptr;
    uint32_t size = 0;
    const auto result = m_managedApi.getBehaviourManifest(&data, &size);
    const auto status = FromAbiResult(result, ScriptLanguage::CSharp);
    if (!status.Succeeded())
        return status;
    if (!data || size == 0)
        return ScriptStatus::Error(ScriptStatusCode::RuntimeError, "Managed Behaviour manifest is empty.");

    const auto document = nlohmann::json::parse(std::string(data, size), nullptr, false);
    if (document.is_discarded() || !document.is_object())
        return ScriptStatus::Error(ScriptStatusCode::SchemaMismatch, "Managed Behaviour manifest is not valid JSON.");
    const auto schema = document.value("schemaHash", std::string{});
    if (schema != m_api.GetSchemaHashHex())
        return ScriptStatus::Error(
            ScriptStatusCode::SchemaMismatch,
            "Managed Behaviour manifest schema hash does not match Native (managed='" +
                schema + "', native='" + m_api.GetSchemaHashHex() + "').");
    const auto behaviours = document.find("behaviours");
    if (behaviours == document.end() || !behaviours->is_array())
        return ScriptStatus::Error(ScriptStatusCode::SchemaMismatch, "Managed Behaviour manifest has no behaviours array.");

    for (const auto& item : *behaviours)
    {
        if (!item.is_object())
            return ScriptStatus::Error(ScriptStatusCode::SchemaMismatch, "Managed Behaviour manifest contains a non-object entry.");
        ManagedBehaviourManifestEntry entry;
        // The generated manifest uses the stable public field names.  Keep
        // the legacy aliases so an older GameScripts assembly can still be
        // loaded during an editor upgrade or hot-reload rollback.
        entry.assetPath = item.value("scriptAssetPath", item.value("assetPath", std::string{}));
        entry.fileName = item.value("fileName", std::string{});
        entry.typeName = item.value("fullTypeName", item.value("typeName", std::string{}));
        entry.simpleName = item.value("simpleName", std::string{});
        entry.scriptTypeId = item.value("scriptTypeId", 0ull);
        if (entry.scriptTypeId == 0 && !entry.assetPath.empty())
            entry.scriptTypeId = MakeStableScriptId("CSharp:" + entry.assetPath);
        entry.isPublic = item.value("isPublic", false);
        entry.isConcrete = item.value("isConcrete", false);
        entry.hasNativeObjectHandleConstructor = item.value("hasNativeObjectHandleConstructor", false);
        entry.isComponent = item.value("isComponent", false);
        entry.callbackMask = item.value("callbackMask", 0u);
        if (entry.typeName.empty() || entry.fileName.empty())
            return ScriptStatus::Error(ScriptStatusCode::SchemaMismatch, "Managed Behaviour manifest contains an incomplete entry.");

        const auto fields = item.find("serializedFields");
        if (fields != item.end() && !fields->is_array())
            return ScriptStatus::Error(ScriptStatusCode::SchemaMismatch, "Managed Behaviour manifest serializedFields is not an array.");
        if (fields != item.end())
        {
            for (const auto& fieldJson : *fields)
            {
                if (!fieldJson.is_object())
                    return ScriptStatus::Error(ScriptStatusCode::SchemaMismatch, "Managed Behaviour manifest contains a non-object field.");
                ManagedBehaviourManifestEntry::Field field;
                field.id = fieldJson.value("id", 0ull);
                field.name = fieldJson.value("name", std::string{});
                field.typeName = fieldJson.value("typeName", std::string{});
                field.kind = fieldJson.value("kind", std::string{});
                field.serialized = fieldJson.value("serialized", true);
                field.aliases = fieldJson.value("aliases", std::vector<std::string>{});
                if (field.id == 0 || field.name.empty() || field.typeName.empty())
                    return ScriptStatus::Error(ScriptStatusCode::SchemaMismatch, "Managed Behaviour manifest contains an incomplete field.");
                if (field.kind.empty())
                    return ScriptStatus::Error(ScriptStatusCode::SchemaMismatch, "Managed Behaviour manifest field has no value kind.");
                entry.fields.push_back(std::move(field));
            }
        }

        ScriptTypeDescriptor descriptor;
        descriptor.id = entry.scriptTypeId;
        descriptor.name = entry.typeName;
        descriptor.fields.reserve(entry.fields.size());
        for (const auto& field : entry.fields)
        {
            ScriptFieldDescriptor nativeField;
            nativeField.id = field.id;
            nativeField.name = field.name;
            nativeField.type = MakeManagedFieldType(field.typeName, field.kind);
            nativeField.serialized = field.serialized;
            nativeField.aliases = field.aliases;
            descriptor.fields.push_back(std::move(nativeField));
        }
        if (descriptor.id != 0)
            m_scriptTypes[descriptor.id] = std::move(descriptor);
        m_behaviourManifest.push_back(std::move(entry));
    }

    std::sort(m_behaviourManifest.begin(), m_behaviourManifest.end(), [](const auto& left, const auto& right)
    {
        if (left.assetPath != right.assetPath)
            return left.assetPath < right.assetPath;
        return left.typeName < right.typeName;
    });
    m_behaviourManifestLoaded = true;
    return ScriptStatus::Ok();
}

const ScriptTypeDescriptor* CoreClrScriptBackend::FindScriptType(ScriptTypeId id) const
{
    const auto found = m_scriptTypes.find(id);
    return found == m_scriptTypes.end() ? nullptr : &found->second;
}

std::optional<bool> CoreClrScriptBackend::IsComponentAsset(std::string_view assetPath) const
{
    if (!m_behaviourManifestLoaded)
        return std::nullopt;

    const auto normalizedPath = NormalizeManifestPath(assetPath);
    const auto fileName = ManifestFileName(assetPath);
    const ManagedBehaviourManifestEntry* match = nullptr;
    size_t matches = 0;
    for (const auto& entry : m_behaviourManifest)
    {
        const auto entryPath = NormalizeManifestPath(entry.assetPath);
        if (!entryPath.empty() && entryPath == normalizedPath)
        {
            if (match && match->isComponent != entry.isComponent)
                return std::nullopt;
            match = &entry;
            ++matches;
        }
    }
    if (matches == 1 && match)
        return match->isComponent;
    if (matches > 1)
        return std::nullopt;

    match = nullptr;
    matches = 0;
    for (const auto& entry : m_behaviourManifest)
    {
        if (NormalizeManifestPath(entry.fileName) != fileName)
            continue;
        match = &entry;
        ++matches;
    }
    return matches == 1 && match ? std::optional<bool>(match->isComponent) : std::nullopt;
}

bool CoreClrScriptBackend::GetField(ScriptInstanceHandle instance, ScriptFieldId field, ScriptValue& output)
{
    const auto found = m_instances.find(instance);
    if (found == m_instances.end() || !m_managedApi.getField)
        return false;
    ScriptAbiValue abiValue;
    const auto result = m_managedApi.getField(found->second.managedToken, field, &abiValue);
    return result.code == ScriptStatusCode::Ok && FromAbiValue(abiValue, output);
}

bool CoreClrScriptBackend::SetField(ScriptInstanceHandle instance, ScriptFieldId field, const ScriptValue& value)
{
    const auto found = m_instances.find(instance);
    if (found == m_instances.end() || !m_managedApi.setField)
        return false;
    std::string stringStorage;
    std::vector<uint8_t> byteStorage;
    bool supported = false;
    auto abiValue = ToAbiValue(value, stringStorage, byteStorage, supported);
    if (!supported)
        return false;
    return m_managedApi.setField(found->second.managedToken, field, &abiValue).code == ScriptStatusCode::Ok;
}

ScriptStatus CoreClrScriptBackend::SetManagedApi(const ManagedApiTable& table)
{
    if (m_initialized)
        return ScriptStatus::Error(ScriptStatusCode::AlreadyInitialized, "Managed API cannot be replaced while CoreCLR is running.");
    m_managedApi = table;
    return ValidateTable(false);
}

ScriptStatus CoreClrScriptBackend::BindManagedApi(std::string_view typeName, std::string_view methodName)
{
    if (m_initialized)
        return ScriptStatus::Error(ScriptStatusCode::AlreadyInitialized, "Managed API cannot be rebound while CoreCLR is running.");
    if (!m_host.IsLoaded())
    {
        if (m_runtimeConfig.empty() || m_assembly.empty())
            return ScriptStatus::Error(ScriptStatusCode::AssetNotLoaded, "CoreCLR runtime artifacts have not been configured.");
        const auto hostStatus = m_host.Load(m_runtimeConfig, m_assembly);
        if (!hostStatus.Succeeded())
            return hostStatus;
    }

    void* function = nullptr;
    auto resolveStatus = m_host.GetUnmanagedFunctionPointer(m_assembly, typeName, methodName, function);
    if (!resolveStatus.Succeeded()
        && typeName == "Nullus.GameScripts.GameScriptsExports, GameScripts"
        && methodName == "GetApiTable")
    {
        // hostfxr already receives the project assembly path, so its primary
        // lookup form is the unqualified type name.  Resolve this before
        // falling back to Nullus.Managed: the project entry point explicitly
        // runs the generated registration initializer, which installs the
        // factory and Behaviour manifest for this project's scripts.
        resolveStatus = m_host.GetUnmanagedFunctionPointer(
            m_assembly,
            "Nullus.GameScripts.GameScriptsExports",
            methodName,
            function);
    }
    if (!resolveStatus.Succeeded()
        && typeName == "Nullus.GameScripts.GameScriptsExports, GameScripts"
        && methodName == "GetApiTable")
    {
        // hostfxr accepts both assembly-qualified and assembly-local type
        // names.  The unqualified form is useful for single-file/project
        // outputs whose AssemblyName is rewritten during packaging.
        resolveStatus = m_host.GetUnmanagedFunctionPointer(
            m_assembly,
            "Nullus.GameScripts.GameScriptsExports",
            methodName,
            function);
    }
    if (!resolveStatus.Succeeded()
        && typeName == "Nullus.GameScripts.GameScriptsExports, GameScripts"
        && methodName == "GetApiTable")
    {
        // A runtime-only deployment may not carry GameScripts.dll. Keep the
        // portable managed ABI as the final compatibility fallback.
        resolveStatus = m_host.GetUnmanagedFunctionPointer(
            m_assembly,
            "Nullus.ManagedExports, Nullus",
            methodName,
            function);
    }
    if (!resolveStatus.Succeeded()
        && typeName == "Nullus.ManagedExports, Nullus"
        && methodName == "GetApiTable")
    {
        resolveStatus = m_host.GetUnmanagedFunctionPointer(
            m_assembly,
            "Nullus.GameScripts.GameScriptsExports, GameScripts",
            methodName,
            function);
    }
    if (!resolveStatus.Succeeded())
        return resolveStatus;
    using GetApiTableFn = void* (*)();
    const auto tableAddress = reinterpret_cast<GetApiTableFn>(function)();
    if (!tableAddress)
        return ScriptStatus::Error(ScriptStatusCode::BackendUnavailable, "Managed API entry point returned a null table.");
    // The table is versioned and extensible.  Read the fixed header first and
    // copy only the bytes advertised by the managed side so an older table can
    // be rejected by ValidateTable without over-reading its allocation.
    ScriptAbiHeader remoteHeader{};
    std::memcpy(&remoteHeader, tableAddress, sizeof(remoteHeader));
    if (remoteHeader.size < sizeof(ScriptAbiHeader) || remoteHeader.abiVersion != 2)
        return ScriptStatus::Error(ScriptStatusCode::SchemaMismatch, "Managed scripting ABI header is unsupported.");
    ManagedApiTable table{};
    const auto copySize = std::min<size_t>(remoteHeader.size, sizeof(table));
    std::memcpy(&table, tableAddress, copySize);
    return SetManagedApi(table);
}

ScriptStatus CoreClrScriptBackend::FromAbiResult(ScriptAbiResult result, ScriptLanguage language, ScriptInstanceHandle instance)
{
    if (result.code == ScriptStatusCode::Ok)
        return ScriptStatus::Ok();
    const auto message = result.message ? result.message : "Managed scripting call failed.";
    if (m_nativeApi.log)
        m_nativeApi.log(ScriptErrorSeverity::Error, message);

    ScriptError diagnostic;
    diagnostic.language = language;
    diagnostic.backend = m_id;
    diagnostic.instance = instance;
    diagnostic.severity = ScriptErrorSeverity::Error;
    diagnostic.message = message;
    if (m_managedApi.getLastDiagnostic)
    {
        if (auto* managedDiagnostic = m_managedApi.getLastDiagnostic())
        {
            if (managedDiagnostic->size >= offsetof(ScriptAbiDiagnostic, sourcePath))
            {
                if (managedDiagnostic->size >= offsetof(ScriptAbiDiagnostic, severity) + sizeof(uint32_t))
                    diagnostic.severity = static_cast<ScriptErrorSeverity>(std::min<uint32_t>(
                        managedDiagnostic->severity,
                        static_cast<uint32_t>(ScriptErrorSeverity::Fatal)));
                if (managedDiagnostic->size >= offsetof(ScriptAbiDiagnostic, line) + sizeof(int32_t))
                    diagnostic.line = managedDiagnostic->line;
                if (managedDiagnostic->size >= offsetof(ScriptAbiDiagnostic, column) + sizeof(int32_t))
                    diagnostic.column = managedDiagnostic->column;
                if (managedDiagnostic->size >= offsetof(ScriptAbiDiagnostic, sourcePath) + sizeof(const char*) && managedDiagnostic->sourcePath)
                    diagnostic.sourcePath = managedDiagnostic->sourcePath;
                if (managedDiagnostic->size >= offsetof(ScriptAbiDiagnostic, message) + sizeof(const char*) && managedDiagnostic->message && *managedDiagnostic->message)
                    diagnostic.message = managedDiagnostic->message;
                if (managedDiagnostic->size >= offsetof(ScriptAbiDiagnostic, stackTrace) + sizeof(const char*) && managedDiagnostic->stackTrace)
                    diagnostic.stackTrace = managedDiagnostic->stackTrace;
            }
        }
    }
    m_lastDiagnostic = std::move(diagnostic);
    return ScriptStatus::Error(result.code, message);
}

std::optional<ScriptError> CoreClrScriptBackend::ConsumeLastDiagnostic()
{
    auto diagnostic = std::move(m_lastDiagnostic);
    m_lastDiagnostic.reset();
    return diagnostic;
}
}
