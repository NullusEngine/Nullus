#pragma once

#include "ScriptingDef.h"

#include "Assets/AssetId.h"
#include "Math/Color.h"
#include "Math/Quaternion.h"
#include "Math/Vector2.h"
#include "Math/Vector3.h"
#include "Math/Vector4.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace NLS::Scripting
{
enum class ScriptLanguage : uint16_t
{
    Unknown = 0,
    CSharp = 1,
    Lua = 2
};

enum class ScriptCallback : uint16_t
{
    Awake = 0,
    Start,
    OnEnable,
    OnDisable,
    Update,
    FixedUpdate,
    LateUpdate,
    OnDestroy,
    Count,

    // Source compatibility for the original P0 names.  The ABI uses the
    // The names above and both spellings resolve to the same bit.
    Enable = OnEnable,
    Disable = OnDisable,
    Destroy = OnDestroy
};

using ScriptCallbackMask = uint32_t;

constexpr ScriptCallbackMask ScriptCallbackBit(ScriptCallback callback)
{
    return 1u << static_cast<uint32_t>(callback);
}

struct NLS_SCRIPTING_API ScriptBackendId
{
    uint16_t value = 0;

    constexpr bool IsValid() const { return value != 0; }
    friend constexpr bool operator==(ScriptBackendId, ScriptBackendId) = default;
};

struct NLS_SCRIPTING_API NativeObjectHandle
{
    uint64_t value = 0;

    constexpr bool IsValid() const { return value != 0; }
    constexpr int32_t GetInstanceId() const { return static_cast<int32_t>(value & 0xFFFFFFFFu); }
    static constexpr NativeObjectHandle FromInstanceId(int32_t instanceId)
    {
        return {static_cast<uint64_t>(static_cast<uint32_t>(instanceId))};
    }
    friend constexpr bool operator==(NativeObjectHandle, NativeObjectHandle) = default;
};

// Persistent references deliberately carry ObjectIdentifier data instead of
// the process-local NativeObjectHandle/InstanceID.  Runtime resolution can
// fill a handle later without ever writing that handle into a scene or prefab.
struct NLS_SCRIPTING_API ScriptObjectReference
{
    std::string guid;
    int64_t localIdentifierInFile = 0;
    int32_t fileType = 0;
    std::string filePath;
    friend bool operator==(const ScriptObjectReference&, const ScriptObjectReference&) = default;
};

struct NLS_SCRIPTING_API ScriptInstanceHandle
{
    uint16_t backendId = 0;
    uint16_t generation = 0;
    uint32_t index = 0;

    constexpr bool IsValid() const { return backendId != 0 && generation != 0 && index != 0; }
    friend constexpr bool operator==(ScriptInstanceHandle, ScriptInstanceHandle) = default;
};

enum class ScriptValueKind : uint8_t
{
    Null = 0,
    Bool,
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
    String,
    Enum,
    Vector2,
    Vector3,
    Vector4,
    Quaternion,
    Color,
    ObjectReference,
    Struct
};

struct NLS_SCRIPTING_API ScriptEnumValue
{
    uint64_t typeId = 0;
    int64_t value = 0;
    friend constexpr bool operator==(ScriptEnumValue, ScriptEnumValue) = default;
};

struct NLS_SCRIPTING_API ScriptStructValue
{
    uint64_t typeId = 0;
    std::vector<uint8_t> bytes;
    friend bool operator==(const ScriptStructValue&, const ScriptStructValue&) = default;
};

struct NLS_SCRIPTING_API ScriptType
{
    uint64_t id = 0;
    ScriptValueKind kind = ScriptValueKind::Null;
    std::string name;
    uint32_t size = 0;
    uint32_t alignment = 0;
    bool portable = true;

    bool IsValid() const { return id != 0 || kind != ScriptValueKind::Null; }
};

using ScriptValue = std::variant<
    std::monostate,
    bool,
    int8_t,
    uint8_t,
    int16_t,
    uint16_t,
    int32_t,
    uint32_t,
    int64_t,
    uint64_t,
    float,
    double,
    std::string,
    ScriptEnumValue,
    NLS::Maths::Vector2,
    NLS::Maths::Vector3,
    NLS::Maths::Vector4,
    NLS::Maths::Quaternion,
    NLS::Maths::Color,
    NativeObjectHandle,
    ScriptObjectReference,
    ScriptStructValue>;

using ScriptFieldId = uint64_t;
using ScriptTypeId = uint64_t;
using ScriptMemberId = uint64_t;

struct NLS_SCRIPTING_API ScriptFieldDescriptor
{
    ScriptFieldId id = 0;
    std::string name;
    ScriptType type;
    bool serialized = true;
    std::vector<std::string> aliases;
    std::optional<ScriptValue> defaultValue;
};

struct NLS_SCRIPTING_API ScriptParameterDescriptor
{
    ScriptType type;
    std::string name;
};

struct NLS_SCRIPTING_API ScriptMethodDescriptor
{
    ScriptMemberId id = 0;
    std::string name;
    ScriptType returnType;
    std::vector<ScriptParameterDescriptor> parameters;
    ScriptCallbackMask callbacks = 0;
    bool scriptable = false;
    bool isStatic = false;
    std::string signature;
};

struct NLS_SCRIPTING_API ScriptPropertyDescriptor
{
    ScriptMemberId id = 0;
    std::string name;
    ScriptType type;
    bool readable = false;
    bool writable = false;
    ScriptMemberId getterId = 0;
    ScriptMemberId setterId = 0;
    bool scriptable = false;
};

struct NLS_SCRIPTING_API ScriptTypeDescriptor
{
    ScriptTypeId id = 0;
    std::string name;
    std::vector<ScriptMethodDescriptor> methods;
    std::vector<ScriptPropertyDescriptor> properties;
    std::vector<ScriptFieldDescriptor> fields;
};

struct NLS_SCRIPTING_API ScriptAsset
{
    NLS::Core::Assets::AssetId assetId;
    ScriptLanguage language = ScriptLanguage::Unknown;
    std::string sourcePath;
    std::string sourceText;
    ScriptTypeId scriptType = 0;
    uint64_t contentHash = 0;
    // Set by the editor importer. Before the first successful C# build this
    // is a conservative source-shape fallback; once the generated Behaviour
    // manifest is available the editor replaces it with semantic metadata.
    // Runtime backends may still load non-component assets for tooling, but
    // editor component workflows must only offer assets marked true.
    bool isComponent = false;
};

using SerializedScriptFields = std::unordered_map<ScriptFieldId, ScriptValue>;

struct NLS_SCRIPTING_API ScriptFrameContext
{
    float deltaTime = 0.0f;
    float unscaledDeltaTime = 0.0f;
    double time = 0.0;
    uint64_t frameIndex = 0;
    float fixedDeltaTime = 0.02f;
    float timeScale = 1.0f;
    double unscaledTime = 0.0;
    uint64_t fixedFrameIndex = 0;
};

struct NLS_SCRIPTING_API ScriptInvocationContext
{
    ScriptFrameContext frame;
    NativeObjectHandle owner;
    // When a scheduler submits a batch, this span contains one owner per
    // instance in the corresponding InvokeBatch range. It is borrowed for the
    // duration of the call and is intentionally absent from the native ABI.
    std::span<const NativeObjectHandle> batchOwners;
};

enum class ScriptStatusCode : uint8_t
{
    Ok = 0,
    InvalidArgument,
    InvalidHandle,
    StaleHandle,
    BackendUnavailable,
    BackendClosed,
    NotInitialized,
    AssetNotLoaded,
    Unsupported,
    RuntimeError,
    SchemaMismatch,
    SchemaCollision,
    AlreadyDestroyed,
    AlreadyInitialized,
    CompilationFailed,
    HotReloadRejected,
    Exception
};

struct NLS_SCRIPTING_API ScriptStatus
{
    ScriptStatusCode code = ScriptStatusCode::Ok;
    std::string message;

    bool Succeeded() const { return code == ScriptStatusCode::Ok; }
    static ScriptStatus Ok() { return {}; }
    static ScriptStatus Error(ScriptStatusCode statusCode, std::string text)
    {
        return {statusCode, std::move(text)};
    }
};

enum class ScriptErrorSeverity : uint8_t
{
    Info,
    Warning,
    Error,
    Fatal
};

struct NLS_SCRIPTING_API ScriptError
{
    ScriptLanguage language = ScriptLanguage::Unknown;
    ScriptBackendId backend;
    NLS::Core::Assets::AssetId scriptAsset;
    ScriptInstanceHandle instance;
    ScriptErrorSeverity severity = ScriptErrorSeverity::Error;
    std::string message;
    std::string stackTrace;
    std::string sourcePath;
    int line = 0;
    int column = 0;
};

struct NLS_SCRIPTING_API ScriptBackendCapabilities
{
    bool supportsBatchInvoke = false;
    bool supportsHotReload = false;
    bool supportsFieldAccess = false;
    bool supportsFrameSnapshot = false;
};

class NLS_SCRIPTING_API ScriptApiDatabase final
{
public:
    bool RegisterType(ScriptTypeDescriptor descriptor);
    const ScriptTypeDescriptor* FindType(ScriptTypeId id) const;
    const ScriptTypeDescriptor* FindType(const std::string& name) const;
    const std::vector<ScriptTypeDescriptor>& GetTypes() const { return m_types; }
    uint64_t GetSchemaHash() const;
    std::array<uint8_t, 32> GetSchemaHashBytes() const;
    std::string GetSchemaHashHex() const;
    void Clear();

private:
    std::vector<ScriptTypeDescriptor> m_types;
    std::unordered_map<ScriptTypeId, size_t> m_typeIndex;
    std::unordered_map<ScriptMemberId, std::string> m_memberIds;
};

NLS_SCRIPTING_API uint64_t MakeStableScriptId(const std::string& canonicalName);
// Content hashes use SHA-256-derived bits and are intentionally separate from
// the FNV-1a IDs used for persistent type/member identity.
NLS_SCRIPTING_API uint64_t MakeScriptContentHash(std::string_view source);
NLS_SCRIPTING_API const char* ToString(ScriptLanguage language);
NLS_SCRIPTING_API const char* ToString(ScriptCallback callback);
}

namespace std
{
template<> struct hash<NLS::Scripting::ScriptBackendId>
{
    size_t operator()(NLS::Scripting::ScriptBackendId id) const noexcept { return id.value; }
};

template<> struct hash<NLS::Scripting::NativeObjectHandle>
{
    size_t operator()(NLS::Scripting::NativeObjectHandle handle) const noexcept
    {
        return hash<uint64_t>{}(handle.value);
    }
};

template<> struct hash<NLS::Scripting::ScriptInstanceHandle>
{
    size_t operator()(NLS::Scripting::ScriptInstanceHandle handle) const noexcept
    {
        size_t result = handle.backendId;
        result = result * 31u + handle.generation;
        return result * 31u + handle.index;
    }
};
}
