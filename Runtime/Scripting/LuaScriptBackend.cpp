#include "LuaScriptBackend.h"
#include "ScriptEngineApi.h"

#include "Base/Object/Object.h"
#include "Engine/Components/TransformComponent.h"
#include "Engine/Components/Component.h"
#include "Engine/Components/CameraComponent.h"
#include "Engine/Components/LightComponent.h"
#include "Engine/GameObject.h"
#include "Engine/SceneSystem/Scene.h"
#include "Engine/SceneSystem/SceneManager.h"
#include "ScriptComponent.h"
#include "Windowing/Inputs/InputManager.h"
#include "Windowing/Inputs/EKey.h"
#include "Windowing/Inputs/EKeyState.h"
#include "Windowing/Inputs/EMouseButton.h"
#include "Windowing/Inputs/EMouseButtonState.h"

#include <Debug/Logger.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

#ifndef NLS_HAS_LUA_VM
#define NLS_HAS_LUA_VM 0
#endif
#ifndef NLS_HAS_LUAPANDA_SOCKET
#define NLS_HAS_LUAPANDA_SOCKET 0
#endif
#ifndef NLS_ENABLE_LUAPANDA_DEBUGGING
#define NLS_ENABLE_LUAPANDA_DEBUGGING 0
#endif
#ifndef NLS_LUAPANDA_SOURCE_PATH
#define NLS_LUAPANDA_SOURCE_PATH "ThirdParty/LuaPanda/Debugger/LuaPanda.lua"
#endif

#if NLS_HAS_LUA_VM
extern "C"
{
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}
#if NLS_HAS_LUAPANDA_SOCKET
extern "C" int luaopen_socket_core(lua_State* state);
#endif
#endif

namespace NLS::Scripting
{
namespace
{
ScriptValueKind ValueKind(const ScriptValue& value)
{
    return std::visit([](const auto& item)
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
    }, value);
}

int ConversionCost(ScriptValueKind actual, ScriptValueKind expected)
{
    if (actual == expected)
        return 0;
    const auto numeric = [](ScriptValueKind kind)
    {
        return kind >= ScriptValueKind::Int8 && kind <= ScriptValueKind::Double;
    };
    if (numeric(actual) && numeric(expected))
        return 1;
    return std::numeric_limits<int>::max();
}

#if NLS_HAS_LUA_VM
struct LuaDiagnosticData
{
    std::string message;
    std::string stackTrace;
    std::string sourcePath;
    int line = 0;
    int column = 0;
};

LuaDiagnosticData LuaError(lua_State* state, const ScriptAsset& asset, const char* fallback)
{
    LuaDiagnosticData result;
    const auto* text = lua_tostring(state, -1);
    result.message = text && *text ? text : fallback;
    result.sourcePath = asset.sourcePath;
    if (lua_gettop(state) > 0)
    {
        luaL_traceback(state, state, result.message.c_str(), 1);
        if (const auto* trace = lua_tostring(state, -1))
            result.stackTrace = trace;
    }
    if (!asset.sourcePath.empty())
    {
        // Lua formats chunk diagnostics as `[string "path"]:line: ...`.
        // Normalize that to the same source path and line used by Native.
        const auto marker = result.message.find("\"]:");
        if (marker != std::string::npos)
        {
            const auto lineStart = marker + 3;
            const auto lineEnd = result.message.find(':', lineStart);
            if (lineEnd != std::string::npos && lineEnd > lineStart)
            {
                const auto lineText = result.message.substr(lineStart, lineEnd - lineStart);
                result.line = std::atoi(lineText.c_str());
                result.message = asset.sourcePath + ":" + lineText + ": " + result.message.substr(lineEnd + 1);
            }
        }
        else if (result.message.rfind(asset.sourcePath + ":", 0) == 0)
        {
            const auto lineStart = asset.sourcePath.size() + 1;
            const auto lineEnd = result.message.find(':', lineStart);
            if (lineEnd != std::string::npos)
                result.line = std::atoi(result.message.substr(lineStart, lineEnd - lineStart).c_str());
        }
        else
            result.message = asset.sourcePath + ": " + result.message;
    }
    lua_settop(state, 0);
    return result;
}

ScriptError MakeLuaScriptError(
    const ScriptAsset& asset,
    ScriptInstanceHandle instance,
    const LuaDiagnosticData& diagnostic)
{
    ScriptError error;
    error.language = ScriptLanguage::Lua;
    error.instance = instance;
    error.scriptAsset = asset.assetId;
    error.severity = ScriptErrorSeverity::Error;
    error.message = diagnostic.message;
    error.stackTrace = diagnostic.stackTrace;
    error.sourcePath = diagnostic.sourcePath;
    error.line = diagnostic.line;
    error.column = diagnostic.column;
    return error;
}

void OpenSafeLibraries(lua_State* state)
{
    // Do not call luaL_openlibs: io/os/debug/package are deliberately absent.
    luaL_requiref(state, "_G", luaopen_base, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_COLIBNAME, luaopen_coroutine, 1);
    lua_pop(state, 1);
    for (const auto* forbidden : {"dofile", "loadfile"})
    {
        lua_pushnil(state);
        lua_setglobal(state, forbidden);
    }
}

const char* CallbackName(ScriptCallback callback)
{
    return ToString(callback);
}

struct LuaNativeObject
{
    uint64_t handle = 0;
};

constexpr const char* kNativeObjectMetatable = "NLS.NativeObject";

LuaNativeObject* TestNativeObject(lua_State* state, int index)
{
    return static_cast<LuaNativeObject*>(luaL_testudata(state, index, kNativeObjectMetatable));
}

void PushNativeObject(lua_State* state, NativeObjectHandle handle)
{
    auto* object = static_cast<LuaNativeObject*>(lua_newuserdatauv(state, sizeof(LuaNativeObject), 0));
    object->handle = handle.value;
    luaL_getmetatable(state, kNativeObjectMetatable);
    lua_setmetatable(state, -2);
}

void PushVector3(lua_State* state, const NLS::Maths::Vector3& value)
{
    lua_createtable(state, 0, 3);
    lua_pushnumber(state, value.x);
    lua_setfield(state, -2, "x");
    lua_pushnumber(state, value.y);
    lua_setfield(state, -2, "y");
    lua_pushnumber(state, value.z);
    lua_setfield(state, -2, "z");
}

void PushVector2(lua_State* state, const NLS::Maths::Vector2& value)
{
    lua_createtable(state, 0, 2);
    lua_pushnumber(state, value.x);
    lua_setfield(state, -2, "x");
    lua_pushnumber(state, value.y);
    lua_setfield(state, -2, "y");
}

void PushQuaternion(lua_State* state, const NLS::Maths::Quaternion& value)
{
    lua_createtable(state, 0, 4);
    lua_pushnumber(state, value.x);
    lua_setfield(state, -2, "x");
    lua_pushnumber(state, value.y);
    lua_setfield(state, -2, "y");
    lua_pushnumber(state, value.z);
    lua_setfield(state, -2, "z");
    lua_pushnumber(state, value.w);
    lua_setfield(state, -2, "w");
}

bool ReadVector3(lua_State* state, int index, NLS::Maths::Vector3& output)
{
    if (!lua_istable(state, index))
        return false;
    lua_getfield(state, index, "x");
    lua_getfield(state, index, "y");
    lua_getfield(state, index, "z");
    const bool valid = lua_isnumber(state, -3) && lua_isnumber(state, -2) && lua_isnumber(state, -1);
    if (valid)
        output = {static_cast<float>(lua_tonumber(state, -3)), static_cast<float>(lua_tonumber(state, -2)), static_cast<float>(lua_tonumber(state, -1))};
    lua_pop(state, 3);
    return valid;
}

bool ReadQuaternion(lua_State* state, int index, NLS::Maths::Quaternion& output)
{
    if (!lua_istable(state, index))
        return false;
    lua_getfield(state, index, "x");
    lua_getfield(state, index, "y");
    lua_getfield(state, index, "z");
    lua_getfield(state, index, "w");
    const bool valid = lua_isnumber(state, -4) && lua_isnumber(state, -3)
        && lua_isnumber(state, -2) && lua_isnumber(state, -1);
    if (valid)
    {
        output = {
            static_cast<float>(lua_tonumber(state, -4)),
            static_cast<float>(lua_tonumber(state, -3)),
            static_cast<float>(lua_tonumber(state, -2)),
            static_cast<float>(lua_tonumber(state, -1))};
    }
    lua_pop(state, 4);
    return valid;
}

int NativeSetActive(lua_State* state)
{
    auto* userdata = TestNativeObject(state, 1);
    auto* object = userdata ? NLS::Object::IDToPointerNoThreadCheck(static_cast<NLS::InstanceID>(userdata->handle & 0xFFFFFFFFu)) : nullptr;
    auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object);
    if (!gameObject || !lua_isboolean(state, 2))
        return luaL_error(state, "SetActive requires a live GameObject and boolean value.");
    gameObject->SetActive(lua_toboolean(state, 2) != 0);
    return 0;
}

int NativeGetActive(lua_State* state)
{
    auto* userdata = TestNativeObject(state, 1);
    auto* object = userdata ? NLS::Object::IDToPointerNoThreadCheck(static_cast<NLS::InstanceID>(userdata->handle & 0xFFFFFFFFu)) : nullptr;
    auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object);
    if (!gameObject)
        return luaL_error(state, "GetActive requires a live GameObject.");
    lua_pushboolean(state, gameObject->GetActive());
    return 1;
}

NLS::Object* NativeObjectPointer(lua_State* state, int index)
{
    auto* userdata = TestNativeObject(state, index);
    return userdata
        ? NLS::Object::IDToPointerNoThreadCheck(static_cast<NLS::InstanceID>(userdata->handle & 0xFFFFFFFFu))
        : nullptr;
}

int NativeGetName(lua_State* state)
{
    auto* object = NativeObjectPointer(state, 1);
    if (auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object)) lua_pushstring(state, gameObject->GetName().c_str());
    else if (auto* component = dynamic_cast<NLS::Engine::Components::Component*>(object); component && component->gameobject()) lua_pushstring(state, component->gameobject()->GetName().c_str());
    else return luaL_error(state, "name requires a live Object.");
    return 1;
}

int NativeGetTag(lua_State* state)
{
    auto* object = NativeObjectPointer(state, 1);
    auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object);
    if (!gameObject)
        if (auto* component = dynamic_cast<NLS::Engine::Components::Component*>(object)) gameObject = component->gameobject();
    if (!gameObject) return luaL_error(state, "tag requires a live Component or GameObject.");
    lua_pushstring(state, gameObject->GetTag().c_str());
    return 1;
}

int NativeGetLayer(lua_State* state)
{
    auto* object = NativeObjectPointer(state, 1);
    auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object);
    if (!gameObject)
        if (auto* component = dynamic_cast<NLS::Engine::Components::Component*>(object)) gameObject = component->gameobject();
    if (!gameObject) return luaL_error(state, "layer requires a live Component or GameObject.");
    lua_pushinteger(state, gameObject->GetLayer());
    return 1;
}

int NativeSetLayer(lua_State* state)
{
    auto* object = NativeObjectPointer(state, 1);
    auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object);
    if (!gameObject)
        if (auto* component = dynamic_cast<NLS::Engine::Components::Component*>(object)) gameObject = component->gameobject();
    if (!gameObject || !lua_isinteger(state, 2)) return luaL_error(state, "layer assignment requires an integer.");
    gameObject->SetLayer(static_cast<int>(lua_tointeger(state, 2)));
    return 0;
}

int NativeSetName(lua_State* state)
{
    auto* object = NativeObjectPointer(state, 1);
    auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object);
    if (!gameObject)
        if (auto* component = dynamic_cast<NLS::Engine::Components::Component*>(object)) gameObject = component->gameobject();
    if (!gameObject || !lua_isstring(state, 2)) return luaL_error(state, "name assignment requires a string.");
    gameObject->SetName(lua_tostring(state, 2));
    return 0;
}

int NativeSetTag(lua_State* state)
{
    auto* object = NativeObjectPointer(state, 1);
    auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object);
    if (!gameObject)
        if (auto* component = dynamic_cast<NLS::Engine::Components::Component*>(object)) gameObject = component->gameobject();
    if (!gameObject || !lua_isstring(state, 2)) return luaL_error(state, "tag assignment requires a string.");
    gameObject->SetTag(lua_tostring(state, 2));
    return 0;
}

int NativeGetInstanceId(lua_State* state)
{
    auto* userdata = TestNativeObject(state, 1);
    if (!userdata || !NativeObjectPointer(state, 1))
        return luaL_error(state, "GetInstanceID requires a live Object.");
    lua_pushinteger(state, static_cast<lua_Integer>(userdata->handle & 0xFFFFFFFFu));
    return 1;
}

int NativeGetEnabled(lua_State* state)
{
    auto* component = dynamic_cast<NLS::Engine::Components::Component*>(NativeObjectPointer(state, 1));
    if (!component)
        return luaL_error(state, "enabled requires a live Component.");
    lua_pushboolean(state, component->IsSelfEnabled());
    return 1;
}

int NativeSetEnabled(lua_State* state)
{
    auto* component = dynamic_cast<NLS::Engine::Components::Component*>(NativeObjectPointer(state, 1));
    if (!component || !lua_isboolean(state, 2))
        return luaL_error(state, "enabled assignment requires a live Component and boolean value.");
    component->SetEnabled(lua_toboolean(state, 2) != 0);
    return 0;
}

int NativeGetIsActiveAndEnabled(lua_State* state)
{
    auto* component = dynamic_cast<NLS::Engine::Components::Component*>(NativeObjectPointer(state, 1));
    if (!component)
        return luaL_error(state, "isActiveAndEnabled requires a live Component.");
    lua_pushboolean(state, component->IsActiveAndEnabled());
    return 1;
}

int NativeCompareTag(lua_State* state)
{
    auto* object = NativeObjectPointer(state, 1);
    auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object);
    if (!gameObject)
        if (auto* component = dynamic_cast<NLS::Engine::Components::Component*>(object)) gameObject = component->gameobject();
    if (!gameObject || !lua_isstring(state, 2))
        return luaL_error(state, "CompareTag requires a live Component or GameObject and string tag.");
    lua_pushboolean(state, gameObject->GetTag() == lua_tostring(state, 2));
    return 1;
}

uint64_t StableScriptId(std::string_view value)
{
    constexpr uint64_t offset = 14695981039346656037ull;
    constexpr uint64_t prime = 1099511628211ull;
    uint64_t hash = offset;
    for (const auto byte : value)
    {
        hash ^= static_cast<uint8_t>(byte);
        hash *= prime;
    }
    return hash == 0 ? 1 : hash;
}

NLS::Engine::GameObject* NativeGameObject(lua_State* state, int index)
{
    auto* object = NativeObjectPointer(state, index);
    if (auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object))
        return gameObject;
    if (auto* component = dynamic_cast<NLS::Engine::Components::Component*>(object))
        return component->gameobject();
    return nullptr;
}

uint64_t LuaComponentTypeId(lua_State* state, int index)
{
    if (lua_isinteger(state, index))
        return static_cast<uint64_t>(lua_tointeger(state, index));
    if (!lua_isstring(state, index))
        return 0;
    const std::string_view name(lua_tostring(state, index));
    if (name == "Transform" || name == "Nullus.Transform" || name == "NLS::Engine::Components::TransformComponent")
        return StableScriptId("Nullus.Transform");
    if (name == "Camera" || name == "Nullus.Camera" || name == "NLS::Engine::Components::CameraComponent")
        return StableScriptId("Nullus.Camera");
    if (name == "Light" || name == "Nullus.Light" || name == "NLS::Engine::Components::LightComponent")
        return StableScriptId("Nullus.Light");
    if (const auto* runtime = GetActiveScriptRuntime())
    {
        if (const auto* descriptor = runtime->GetApi().FindType(std::string(name)))
            return descriptor->id;
        if (name.rfind("global::", 0) == 0)
            if (const auto* descriptor = runtime->GetApi().FindType(std::string(name.substr(8))))
                return descriptor->id;
    }
    return StableScriptId(name);
}

int NativeGetComponent(lua_State* state)
{
    auto* gameObject = NativeGameObject(state, 1);
    const auto typeId = LuaComponentTypeId(state, 2);
    if (!gameObject || typeId == 0)
        return luaL_error(state, "GetComponent requires a live GameObject and registered component type.");
    NLS::Engine::Components::Component* component = nullptr;
    if (typeId == StableScriptId("Nullus.Transform")) component = gameObject->GetTransform();
    else if (typeId == StableScriptId("Nullus.Camera")) component = gameObject->GetComponent<NLS::Engine::Components::CameraComponent>();
    else if (typeId == StableScriptId("Nullus.Light")) component = gameObject->GetComponent<NLS::Engine::Components::LightComponent>();
    else
    {
        for (const auto& candidate : gameObject->GetComponents())
        {
            auto* script = dynamic_cast<NLS::Scripting::ScriptComponent*>(candidate.get());
            if (script && script->GetScriptAsset().scriptType == typeId)
            {
                component = script;
                break;
            }
        }
    }
    if (component) PushNativeObject(state, NativeObjectHandle::FromInstanceId(component->GetInstanceID()));
    else lua_pushnil(state);
    return 1;
}

int NativeAddComponent(lua_State* state)
{
    auto* gameObject = NativeGameObject(state, 1);
    const auto typeId = LuaComponentTypeId(state, 2);
    if (!gameObject || typeId == 0)
        return luaL_error(state, "AddComponent requires a live GameObject and registered component type.");
    NLS::Engine::Components::Component* component = nullptr;
    if (typeId == StableScriptId("Nullus.Transform")) component = gameObject->GetTransform();
    else if (typeId == StableScriptId("Nullus.Camera"))
    {
        component = gameObject->GetComponent<NLS::Engine::Components::CameraComponent>();
        if (!component) component = gameObject->AddComponent<NLS::Engine::Components::CameraComponent>();
    }
    else if (typeId == StableScriptId("Nullus.Light"))
    {
        component = gameObject->GetComponent<NLS::Engine::Components::LightComponent>();
        if (!component) component = gameObject->AddComponent<NLS::Engine::Components::LightComponent>();
    }
    else if (const auto* asset = FindRegisteredScriptAsset(typeId))
    {
        auto* script = gameObject->AddComponent<NLS::Scripting::ScriptComponent>();
        if (script)
        {
            script->SetScriptAsset(*asset);
            script->SetRuntime(GetActiveScriptRuntime());
            component = script;
        }
    }
    if (!component)
        return luaL_error(state, "The requested component type is not registered or could not be created.");
    PushNativeObject(state, NativeObjectHandle::FromInstanceId(component->GetInstanceID()));
    return 1;
}

int NativeGetParent(lua_State* state)
{
    auto* transform = dynamic_cast<NLS::Engine::Components::TransformComponent*>(NativeObjectPointer(state, 1));
    if (!transform) return luaL_error(state, "parent requires a live Transform.");
    auto* parent = transform->gameobject() ? transform->gameobject()->GetParent() : nullptr;
    if (parent && parent->GetTransform()) PushNativeObject(state, NativeObjectHandle::FromInstanceId(parent->GetTransform()->GetInstanceID()));
    else lua_pushnil(state);
    return 1;
}

int NativeSetParent(lua_State* state)
{
    auto* transform = dynamic_cast<NLS::Engine::Components::TransformComponent*>(NativeObjectPointer(state, 1));
    if (!transform) return luaL_error(state, "parent assignment requires a live Transform.");
    if (lua_isnil(state, 2)) { transform->RemoveParent(); return 0; }
    auto* parent = dynamic_cast<NLS::Engine::Components::TransformComponent*>(NativeObjectPointer(state, 2));
    if (!parent) return luaL_error(state, "parent assignment requires a Transform or nil.");
    transform->SetParent(*parent);
    return 0;
}

int NativeDestroyObject(lua_State* state)
{
    auto* userdata = TestNativeObject(state, 1);
    auto* object = NativeObjectPointer(state, 1);
    const auto delay = lua_isnumber(state, 2) ? static_cast<float>(lua_tonumber(state, 2)) : 0.0f;
    if (!userdata || !object) return luaL_error(state, "Destroy requires a live Object.");
    const NativeObjectHandle handle{userdata->handle};
    if (delay > 0.0f)
    {
        if (!ScriptEngineApi::QueueDestroy(handle, delay)) return luaL_error(state, "Invalid delayed destruction request.");
        return 0;
    }
    if (auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object)) gameObject->MarkAsDestroy();
    else if (auto* component = dynamic_cast<NLS::Engine::Components::Component*>(object))
    {
        if (!component->gameobject() || component->GetType() == NLS_TYPEOF(NLS::Engine::Components::TransformComponent)) return luaL_error(state, "Transform cannot be destroyed.");
        component->gameobject()->RemoveComponent(component);
    }
    return 0;
}

int LuaFind(lua_State* state)
{
    const auto name = lua_isstring(state, 1) ? lua_tostring(state, 1) : "";
    auto* scene = ScriptEngineApi::GetScene();
    auto* object = scene ? scene->FindGameObjectByName(name) : nullptr;
    if (object) PushNativeObject(state, NativeObjectHandle::FromInstanceId(object->GetInstanceID())); else lua_pushnil(state);
    return 1;
}

int LuaFindWithTag(lua_State* state)
{
    const auto tag = lua_isstring(state, 1) ? lua_tostring(state, 1) : "";
    auto* scene = ScriptEngineApi::GetScene();
    auto* object = scene ? scene->FindGameObjectByTag(tag) : nullptr;
    if (object) PushNativeObject(state, NativeObjectHandle::FromInstanceId(object->GetInstanceID())); else lua_pushnil(state);
    return 1;
}

int LuaGetActiveScene(lua_State* state)
{
    const auto* manager = ScriptEngineApi::GetSceneManager();
    const auto path = manager ? manager->GetCurrentSceneSourcePath() : std::string{};
    auto name = path;
    if (const auto separator = name.find_last_of("/\\"); separator != std::string::npos)
        name.erase(0, separator + 1);
    if (const auto extension = name.find_last_of('.'); extension != std::string::npos)
        name.erase(extension);
    lua_createtable(state, 0, 4);
    lua_pushlstring(state, name.data(), name.size());
    lua_setfield(state, -2, "name");
    lua_pushlstring(state, path.data(), path.size());
    lua_setfield(state, -2, "path");
    lua_pushboolean(state, manager ? manager->HasCurrentScene() : ScriptEngineApi::GetScene() != nullptr);
    lua_setfield(state, -2, "isLoaded");
    lua_pushinteger(state, -1);
    lua_setfield(state, -2, "buildIndex");
    return 1;
}

int LuaLoadScene(lua_State* state)
{
    const auto* path = luaL_checkstring(state, 1);
    if (!ScriptEngineApi::QueueSceneLoad(path ? path : ""))
        return luaL_error(state, "No active SceneManager is available.");
    return 0;
}

NLS::Engine::GameObject* LuaCloneGameObjectTree(
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
                copy->SetRuntime(GetActiveScriptRuntime());
            }
        }
    }
    for (auto* child : source.GetChildren())
        if (child)
            LuaCloneGameObjectTree(*child, scene, &clone);
    return &clone;
}

int LuaInstantiate(lua_State* state)
{
    auto* source = dynamic_cast<NLS::Engine::GameObject*>(NativeObjectPointer(state, 1));
    if (!source || !source->GetScene()) return luaL_error(state, "Instantiate requires a scene GameObject.");
    auto* clone = LuaCloneGameObjectTree(*source, *source->GetScene(), nullptr);
    if (!clone) return luaL_error(state, "Native GameObject instantiate failed.");
    PushNativeObject(state, NativeObjectHandle::FromInstanceId(clone->GetInstanceID()));
    return 1;
}

int LuaGetKey(lua_State* state)
{
    const auto* input = ScriptEngineApi::GetInputManager();
    const auto key = static_cast<NLS::Windowing::Inputs::EKey>(luaL_optinteger(state, 1, 0));
    lua_pushboolean(state, input && input->GetKeyState(key) == NLS::Windowing::Inputs::EKeyState::KEY_DOWN);
    return 1;
}

int LuaGetKeyDown(lua_State* state)
{
    const auto* input = ScriptEngineApi::GetInputManager();
    const auto key = static_cast<NLS::Windowing::Inputs::EKey>(luaL_optinteger(state, 1, 0));
    lua_pushboolean(state, input && input->IsKeyPressed(key));
    return 1;
}

int LuaGetKeyUp(lua_State* state)
{
    const auto* input = ScriptEngineApi::GetInputManager();
    const auto key = static_cast<NLS::Windowing::Inputs::EKey>(luaL_optinteger(state, 1, 0));
    lua_pushboolean(state, input && input->IsKeyReleased(key));
    return 1;
}

int LuaGetMouseButton(lua_State* state)
{
    const auto* input = ScriptEngineApi::GetInputManager();
    const auto button = static_cast<NLS::Windowing::Inputs::EMouseButton>(luaL_optinteger(state, 1, 0));
    lua_pushboolean(state, input && input->GetMouseButtonState(button) == NLS::Windowing::Inputs::EMouseButtonState::MOUSE_DOWN);
    return 1;
}

int LuaGetMouseButtonDown(lua_State* state)
{
    const auto* input = ScriptEngineApi::GetInputManager();
    const auto button = static_cast<NLS::Windowing::Inputs::EMouseButton>(luaL_optinteger(state, 1, 0));
    lua_pushboolean(state, input && input->IsMouseButtonPressed(button));
    return 1;
}

int LuaGetMouseButtonUp(lua_State* state)
{
    const auto* input = ScriptEngineApi::GetInputManager();
    const auto button = static_cast<NLS::Windowing::Inputs::EMouseButton>(luaL_optinteger(state, 1, 0));
    lua_pushboolean(state, input && input->IsMouseButtonReleased(button));
    return 1;
}

int NativeSetLocalPosition(lua_State* state)
{
    auto* userdata = TestNativeObject(state, 1);
    auto* object = userdata ? NLS::Object::IDToPointerNoThreadCheck(static_cast<NLS::InstanceID>(userdata->handle & 0xFFFFFFFFu)) : nullptr;
    auto* transform = dynamic_cast<NLS::Engine::Components::TransformComponent*>(object);
    NLS::Maths::Vector3 value;
    if (!transform || !ReadVector3(state, 2, value))
        return luaL_error(state, "SetLocalPosition requires a live Transform and {x,y,z} table.");
    transform->SetLocalPosition(value);
    return 0;
}

int NativeGetLocalPosition(lua_State* state)
{
    auto* userdata = TestNativeObject(state, 1);
    auto* object = userdata ? NLS::Object::IDToPointerNoThreadCheck(static_cast<NLS::InstanceID>(userdata->handle & 0xFFFFFFFFu)) : nullptr;
    auto* transform = dynamic_cast<NLS::Engine::Components::TransformComponent*>(object);
    if (!transform)
        return luaL_error(state, "GetLocalPosition requires a live Transform.");
    PushVector3(state, transform->GetLocalPosition());
    return 1;
}

int NativeCreatePrimitive(lua_State* state)
{
    auto* userdata = TestNativeObject(state, 1);
    auto* object = userdata
        ? NLS::Object::IDToPointerNoThreadCheck(static_cast<NLS::InstanceID>(userdata->handle & 0xFFFFFFFFu))
        : nullptr;
    auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object);
    const auto* typeName = lua_isstring(state, 2) ? lua_tostring(state, 2) : "Cube";
    if (!gameObject)
        return luaL_error(state, "createPrimitive requires a live GameObject.");
    auto* primitive = gameObject->CreatePrimitive(typeName ? typeName : "Cube");
    if (!primitive)
        return luaL_error(state, "Unable to create primitive '%s'.", typeName ? typeName : "Cube");
    PushNativeObject(state, NativeObjectHandle::FromInstanceId(primitive->GetInstanceID()));
    return 1;
}

int NativeGetLocalRotation(lua_State* state)
{
    auto* userdata = TestNativeObject(state, 1);
    auto* object = userdata
        ? NLS::Object::IDToPointerNoThreadCheck(static_cast<NLS::InstanceID>(userdata->handle & 0xFFFFFFFFu))
        : nullptr;
    auto* transform = dynamic_cast<NLS::Engine::Components::TransformComponent*>(object);
    if (!transform)
        return luaL_error(state, "GetLocalRotation requires a live Transform.");
    PushQuaternion(state, transform->GetLocalRotation());
    return 1;
}

int NativeObjectIndex(lua_State* state)
{
    auto* userdata = TestNativeObject(state, 1);
    const auto* key = lua_tostring(state, 2);
    if (!userdata || !key)
        return 0;
    auto* object = NLS::Object::IDToPointerNoThreadCheck(static_cast<NLS::InstanceID>(userdata->handle & 0xFFFFFFFFu));
    if (!object)
        return luaL_error(state, "Native object handle %llu is no longer alive.", static_cast<unsigned long long>(userdata->handle));

    if (std::strcmp(key, "setActive") == 0 || std::strcmp(key, "SetActive") == 0)
    {
        lua_pushcfunction(state, &NativeSetActive);
        return 1;
    }
    if (std::strcmp(key, "getName") == 0 || std::strcmp(key, "GetName") == 0)
    {
        lua_pushcfunction(state, &NativeGetName);
        return 1;
    }
    if (std::strcmp(key, "setName") == 0 || std::strcmp(key, "SetName") == 0)
    {
        lua_pushcfunction(state, &NativeSetName);
        return 1;
    }
    if (std::strcmp(key, "getTag") == 0 || std::strcmp(key, "GetTag") == 0)
    {
        lua_pushcfunction(state, &NativeGetTag);
        return 1;
    }
    if (std::strcmp(key, "setTag") == 0 || std::strcmp(key, "SetTag") == 0)
    {
        lua_pushcfunction(state, &NativeSetTag);
        return 1;
    }
    if (std::strcmp(key, "getLayer") == 0 || std::strcmp(key, "GetLayer") == 0)
    {
        lua_pushcfunction(state, &NativeGetLayer);
        return 1;
    }
    if (std::strcmp(key, "setLayer") == 0 || std::strcmp(key, "SetLayer") == 0)
    {
        lua_pushcfunction(state, &NativeSetLayer);
        return 1;
    }
    if (std::strcmp(key, "getParent") == 0 || std::strcmp(key, "GetParent") == 0)
    {
        lua_pushcfunction(state, &NativeGetParent);
        return 1;
    }
    if (std::strcmp(key, "setParent") == 0 || std::strcmp(key, "SetParent") == 0)
    {
        lua_pushcfunction(state, &NativeSetParent);
        return 1;
    }
    if (std::strcmp(key, "getInstanceID") == 0 || std::strcmp(key, "GetInstanceID") == 0)
    {
        lua_pushcfunction(state, &NativeGetInstanceId);
        return 1;
    }
    if (std::strcmp(key, "compareTag") == 0 || std::strcmp(key, "CompareTag") == 0)
    {
        lua_pushcfunction(state, &NativeCompareTag);
        return 1;
    }
    if (std::strcmp(key, "getEnabled") == 0 || std::strcmp(key, "GetEnabled") == 0)
    {
        lua_pushcfunction(state, &NativeGetEnabled);
        return 1;
    }
    if (std::strcmp(key, "setEnabled") == 0 || std::strcmp(key, "SetEnabled") == 0)
    {
        lua_pushcfunction(state, &NativeSetEnabled);
        return 1;
    }
    if (std::strcmp(key, "getIsActiveAndEnabled") == 0 || std::strcmp(key, "IsActiveAndEnabled") == 0)
    {
        lua_pushcfunction(state, &NativeGetIsActiveAndEnabled);
        return 1;
    }
    if (std::strcmp(key, "GetComponent") == 0 || std::strcmp(key, "getComponent") == 0)
    {
        lua_pushcfunction(state, &NativeGetComponent);
        return 1;
    }
    if (std::strcmp(key, "AddComponent") == 0 || std::strcmp(key, "addComponent") == 0)
    {
        lua_pushcfunction(state, &NativeAddComponent);
        return 1;
    }
    if (std::strcmp(key, "Destroy") == 0 || std::strcmp(key, "destroy") == 0)
    {
        lua_pushcfunction(state, &NativeDestroyObject);
        return 1;
    }
    if (std::strcmp(key, "Instantiate") == 0 || std::strcmp(key, "instantiate") == 0)
    {
        lua_pushcfunction(state, &LuaInstantiate);
        return 1;
    }
    if (std::strcmp(key, "getActive") == 0 || std::strcmp(key, "GetActive") == 0)
    {
        lua_pushcfunction(state, &NativeGetActive);
        return 1;
    }
    if (std::strcmp(key, "setLocalPosition") == 0 || std::strcmp(key, "SetLocalPosition") == 0)
    {
        lua_pushcfunction(state, &NativeSetLocalPosition);
        return 1;
    }
    if (std::strcmp(key, "getLocalPosition") == 0 || std::strcmp(key, "GetLocalPosition") == 0)
    {
        lua_pushcfunction(state, &NativeGetLocalPosition);
        return 1;
    }
    if (std::strcmp(key, "createPrimitive") == 0 || std::strcmp(key, "CreatePrimitive") == 0)
    {
        lua_pushcfunction(state, &NativeCreatePrimitive);
        return 1;
    }
    if (std::strcmp(key, "getLocalRotation") == 0 || std::strcmp(key, "GetLocalRotation") == 0)
    {
        lua_pushcfunction(state, &NativeGetLocalRotation);
        return 1;
    }

    if (std::strcmp(key, "transform") == 0)
    {
        if (auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object); gameObject && gameObject->GetTransform())
        {
            PushNativeObject(state, NativeObjectHandle::FromInstanceId(gameObject->GetTransform()->GetInstanceID()));
            return 1;
        }
        return 0;
    }
    if (std::strcmp(key, "active") == 0)
    {
        if (auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object))
        {
            lua_pushboolean(state, gameObject->GetActive());
            return 1;
        }
        return 0;
    }
    if (auto* camera = dynamic_cast<NLS::Engine::Components::CameraComponent*>(object))
    {
        if (std::strcmp(key, "fieldOfView") == 0)
        {
            lua_pushnumber(state, camera->GetFov());
            return 1;
        }
        if (std::strcmp(key, "orthographicSize") == 0)
        {
            lua_pushnumber(state, camera->GetSize());
            return 1;
        }
        if (std::strcmp(key, "nearClipPlane") == 0)
        {
            lua_pushnumber(state, camera->GetNear());
            return 1;
        }
        if (std::strcmp(key, "farClipPlane") == 0)
        {
            lua_pushnumber(state, camera->GetFar());
            return 1;
        }
        if (std::strcmp(key, "clearColor") == 0)
        {
            PushVector3(state, camera->GetClearColor());
            return 1;
        }
        if (std::strcmp(key, "orthographic") == 0)
        {
            lua_pushboolean(state, camera->GetProjectionMode() == NLS::Render::Settings::EProjectionMode::ORTHOGRAPHIC);
            return 1;
        }
    }
    if (auto* light = dynamic_cast<NLS::Engine::Components::LightComponent*>(object))
    {
        if (std::strcmp(key, "color") == 0)
        {
            PushVector3(state, light->GetColor());
            return 1;
        }
        if (std::strcmp(key, "intensity") == 0)
        {
            lua_pushnumber(state, light->GetIntensity());
            return 1;
        }
        if (std::strcmp(key, "range") == 0)
        {
            lua_pushnumber(state, light->GetRange());
            return 1;
        }
        if (std::strcmp(key, "spotAngle") == 0)
        {
            lua_pushnumber(state, light->GetOuterCutoff());
            return 1;
        }
        if (std::strcmp(key, "type") == 0)
        {
            lua_pushinteger(state, static_cast<lua_Integer>(light->GetLightType()));
            return 1;
        }
    }
    if (std::strcmp(key, "name") == 0)
    {
        return NativeGetName(state);
    }
    if (std::strcmp(key, "tag") == 0)
    {
        return NativeGetTag(state);
    }
    if (std::strcmp(key, "layer") == 0)
    {
        return NativeGetLayer(state);
    }
    if (std::strcmp(key, "parent") == 0)
    {
        return NativeGetParent(state);
    }
    if (std::strcmp(key, "enabled") == 0)
    {
        return NativeGetEnabled(state);
    }
    if (std::strcmp(key, "isActiveAndEnabled") == 0)
    {
        return NativeGetIsActiveAndEnabled(state);
    }
    if (std::strcmp(key, "localPosition") == 0 || std::strcmp(key, "position") == 0)
    {
        if (auto* transform = dynamic_cast<NLS::Engine::Components::TransformComponent*>(object))
        {
            PushVector3(state, std::strcmp(key, "position") == 0 ? transform->GetWorldPosition() : transform->GetLocalPosition());
            return 1;
        }
        return 0;
    }
    if (std::strcmp(key, "localScale") == 0 || std::strcmp(key, "lossyScale") == 0)
    {
        if (auto* transform = dynamic_cast<NLS::Engine::Components::TransformComponent*>(object))
        {
            PushVector3(state, std::strcmp(key, "lossyScale") == 0 ? transform->GetWorldScale() : transform->GetLocalScale());
            return 1;
        }
        return 0;
    }
    if (std::strcmp(key, "localRotation") == 0 || std::strcmp(key, "rotation") == 0)
    {
        auto* transform = dynamic_cast<NLS::Engine::Components::TransformComponent*>(object);
        if (transform)
        {
            PushQuaternion(state, std::strcmp(key, "rotation") == 0 ? transform->GetWorldRotation() : transform->GetLocalRotation());
            return 1;
        }
        return 0;
    }
    if (std::strcmp(key, "gameObject") == 0)
    {
        if (auto* transform = dynamic_cast<NLS::Engine::Components::TransformComponent*>(object); transform && transform->gameobject())
        {
            PushNativeObject(state, NativeObjectHandle::FromInstanceId(transform->gameobject()->GetInstanceID()));
            return 1;
        }
        if (auto* component = dynamic_cast<NLS::Engine::Components::Component*>(object); component && component->gameobject())
        {
            PushNativeObject(state, NativeObjectHandle::FromInstanceId(component->gameobject()->GetInstanceID()));
            return 1;
        }
        return 0;
    }
    return 0;
}

int NativeObjectNewIndex(lua_State* state)
{
    auto* userdata = TestNativeObject(state, 1);
    const auto* key = lua_tostring(state, 2);
    if (!userdata || !key)
        return luaL_error(state, "Native object assignment requires an object and member name.");
    auto* object = NLS::Object::IDToPointerNoThreadCheck(static_cast<NLS::InstanceID>(userdata->handle & 0xFFFFFFFFu));
    if (!object)
        return luaL_error(state, "Native object handle %llu is no longer alive.", static_cast<unsigned long long>(userdata->handle));
    if (std::strcmp(key, "active") == 0)
    {
        auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object);
        if (!gameObject || !lua_isboolean(state, 3))
            return luaL_error(state, "Native active assignment requires a GameObject and boolean value.");
        gameObject->SetActive(lua_toboolean(state, 3) != 0);
        return 0;
    }
    if (auto* camera = dynamic_cast<NLS::Engine::Components::CameraComponent*>(object))
    {
        if (std::strcmp(key, "fieldOfView") == 0)
        {
            if (!lua_isnumber(state, 3)) return luaL_error(state, "fieldOfView assignment requires a number.");
            camera->SetFov(static_cast<float>(lua_tonumber(state, 3)));
            return 0;
        }
        if (std::strcmp(key, "orthographicSize") == 0)
        {
            if (!lua_isnumber(state, 3)) return luaL_error(state, "orthographicSize assignment requires a number.");
            camera->SetSize(static_cast<float>(lua_tonumber(state, 3)));
            return 0;
        }
        if (std::strcmp(key, "nearClipPlane") == 0)
        {
            if (!lua_isnumber(state, 3)) return luaL_error(state, "nearClipPlane assignment requires a number.");
            camera->SetNear(static_cast<float>(lua_tonumber(state, 3)));
            return 0;
        }
        if (std::strcmp(key, "farClipPlane") == 0)
        {
            if (!lua_isnumber(state, 3)) return luaL_error(state, "farClipPlane assignment requires a number.");
            camera->SetFar(static_cast<float>(lua_tonumber(state, 3)));
            return 0;
        }
        if (std::strcmp(key, "clearColor") == 0)
        {
            NLS::Maths::Vector3 value;
            if (!ReadVector3(state, 3, value)) return luaL_error(state, "clearColor assignment requires a {x,y,z} table.");
            camera->SetClearColor(value);
            return 0;
        }
        if (std::strcmp(key, "orthographic") == 0)
        {
            if (!lua_isboolean(state, 3)) return luaL_error(state, "orthographic assignment requires a boolean.");
            camera->SetProjectionMode(lua_toboolean(state, 3) != 0
                ? NLS::Render::Settings::EProjectionMode::ORTHOGRAPHIC
                : NLS::Render::Settings::EProjectionMode::PERSPECTIVE);
            return 0;
        }
    }
    if (auto* light = dynamic_cast<NLS::Engine::Components::LightComponent*>(object))
    {
        if (std::strcmp(key, "color") == 0)
        {
            NLS::Maths::Vector3 value;
            if (!ReadVector3(state, 3, value)) return luaL_error(state, "color assignment requires a {x,y,z} table.");
            light->SetColor(value);
            return 0;
        }
        if (std::strcmp(key, "intensity") == 0)
        {
            if (!lua_isnumber(state, 3)) return luaL_error(state, "intensity assignment requires a number.");
            light->SetIntensity(static_cast<float>(lua_tonumber(state, 3)));
            return 0;
        }
        if (std::strcmp(key, "range") == 0)
        {
            if (!lua_isnumber(state, 3)) return luaL_error(state, "range assignment requires a number.");
            light->SetRange(static_cast<float>(lua_tonumber(state, 3)));
            return 0;
        }
        if (std::strcmp(key, "spotAngle") == 0)
        {
            if (!lua_isnumber(state, 3)) return luaL_error(state, "spotAngle assignment requires a number.");
            light->SetOuterCutoff(static_cast<float>(lua_tonumber(state, 3)));
            return 0;
        }
        if (std::strcmp(key, "type") == 0)
        {
            if (!lua_isinteger(state, 3)) return luaL_error(state, "type assignment requires an integer.");
            const auto value = lua_tointeger(state, 3);
            if (value < 0 || value > 4) return luaL_error(state, "type must be a LightType value from 0 to 4.");
            light->SetLightType(static_cast<NLS::Render::Settings::ELightType>(value));
            return 0;
        }
    }
    if (std::strcmp(key, "name") == 0)
    {
        auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object);
        if (!gameObject)
            if (auto* component = dynamic_cast<NLS::Engine::Components::Component*>(object)) gameObject = component->gameobject();
        if (!gameObject || !lua_isstring(state, 3))
            return luaL_error(state, "name assignment requires a live Object and string value.");
        gameObject->SetName(lua_tostring(state, 3));
        return 0;
    }
    if (std::strcmp(key, "tag") == 0)
    {
        auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object);
        if (!gameObject)
            if (auto* component = dynamic_cast<NLS::Engine::Components::Component*>(object)) gameObject = component->gameobject();
        if (!gameObject || !lua_isstring(state, 3))
            return luaL_error(state, "tag assignment requires a live Object and string value.");
        gameObject->SetTag(lua_tostring(state, 3));
        return 0;
    }
    if (std::strcmp(key, "layer") == 0)
    {
        auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object);
        if (!gameObject)
            if (auto* component = dynamic_cast<NLS::Engine::Components::Component*>(object)) gameObject = component->gameobject();
        if (!gameObject || !lua_isinteger(state, 3))
            return luaL_error(state, "layer assignment requires a live Object and integer value.");
        gameObject->SetLayer(static_cast<int>(lua_tointeger(state, 3)));
        return 0;
    }
    if (std::strcmp(key, "enabled") == 0)
    {
        auto* component = dynamic_cast<NLS::Engine::Components::Component*>(object);
        if (!component || !lua_isboolean(state, 3))
            return luaL_error(state, "enabled assignment requires a live Component and boolean value.");
        component->SetEnabled(lua_toboolean(state, 3) != 0);
        return 0;
    }
    if (std::strcmp(key, "parent") == 0)
    {
        auto* transform = dynamic_cast<NLS::Engine::Components::TransformComponent*>(object);
        if (!transform)
            return luaL_error(state, "parent assignment requires a live Transform.");
        if (lua_isnil(state, 3))
        {
            transform->RemoveParent();
            return 0;
        }
        auto* parent = dynamic_cast<NLS::Engine::Components::TransformComponent*>(NativeObjectPointer(state, 3));
        if (!parent)
            return luaL_error(state, "parent assignment requires a Transform or nil.");
        transform->SetParent(*parent);
        return 0;
    }
    if (std::strcmp(key, "localPosition") == 0 || std::strcmp(key, "position") == 0)
    {
        auto* transform = dynamic_cast<NLS::Engine::Components::TransformComponent*>(object);
        NLS::Maths::Vector3 value;
        if (!transform || !ReadVector3(state, 3, value))
            return luaL_error(state, "Native localPosition assignment requires a Transform and {x,y,z} table.");
        if (std::strcmp(key, "position") == 0)
            transform->SetWorldPosition(value);
        else
            transform->SetLocalPosition(value);
        return 0;
    }
    if (std::strcmp(key, "localRotation") == 0 || std::strcmp(key, "rotation") == 0)
    {
        auto* transform = dynamic_cast<NLS::Engine::Components::TransformComponent*>(object);
        NLS::Maths::Quaternion value;
        if (!transform || !ReadQuaternion(state, 3, value))
            return luaL_error(state, "Native localRotation assignment requires a Transform and {x,y,z,w} table.");
        if (std::strcmp(key, "rotation") == 0)
            transform->SetWorldRotation(value);
        else
            transform->SetLocalRotation(value);
        return 0;
    }
    return luaL_error(state, "Native member '%s' is read-only or unsupported.", key);
}

void InstallNativeObjectMetatable(lua_State* state)
{
    if (luaL_newmetatable(state, kNativeObjectMetatable) != 0)
    {
        lua_pushcfunction(state, &NativeObjectIndex);
        lua_setfield(state, -2, "__index");
        lua_pushcfunction(state, &NativeObjectNewIndex);
        lua_setfield(state, -2, "__newindex");
    }
    lua_pop(state, 1);
}
#endif
}

ScriptStatus LuaScriptBackend::ResolveOverload(
    std::span<const OverloadCandidate> candidates,
    std::span<const ScriptValue> arguments,
    ScriptMemberId& output)
{
    output = 0;
    int bestCost = std::numeric_limits<int>::max();
    bool ambiguous = false;
    for (const auto& candidate : candidates)
    {
        if (candidate.parameters.size() != arguments.size())
            continue;
        int cost = 0;
        for (size_t index = 0; index < arguments.size(); ++index)
        {
            const auto conversion = ConversionCost(ValueKind(arguments[index]), candidate.parameters[index].kind);
            if (conversion == std::numeric_limits<int>::max())
            {
                cost = conversion;
                break;
            }
            cost += conversion;
        }
        if (cost < bestCost)
        {
            bestCost = cost;
            output = candidate.id;
            ambiguous = false;
        }
        else if (cost == bestCost && cost != std::numeric_limits<int>::max())
        {
            ambiguous = true;
        }
    }
    if (output == 0)
        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "No Lua overload accepts the supplied arguments.");
    if (ambiguous)
    {
        output = 0;
        return ScriptStatus::Error(ScriptStatusCode::RuntimeError, "Lua overload resolution is ambiguous.");
    }
    return ScriptStatus::Ok();
}

LuaScriptBackend::LuaScriptBackend(ScriptBackendId id)
    : m_id(id)
{
}

ScriptStatus LuaScriptBackend::Initialize(const ScriptApiDatabase& api)
{
#if !NLS_ENABLE_LUA_SCRIPTING
    (void)api;
    return ScriptStatus::Error(ScriptStatusCode::BackendUnavailable, "Lua scripting is disabled by NLS_ENABLE_LUA_SCRIPTING.");
#elif !NLS_HAS_LUA_VM
    (void)api;
    return ScriptStatus::Error(ScriptStatusCode::BackendUnavailable, "Lua 5.4.8 is not linked into this build.");
#else
    if (m_initialized)
        return ScriptStatus::Error(ScriptStatusCode::AlreadyInitialized, "Lua backend is already initialized.");
    m_api = api;
    m_state = luaL_newstate();
    if (!m_state)
        return ScriptStatus::Error(ScriptStatusCode::RuntimeError, "Lua 5.4.8 could not allocate a state.");
    OpenSafeLibraries(m_state);
    InstallNativeObjectMetatable(m_state);
    lua_newtable(m_state);
    lua_newtable(m_state);
    lua_pushnumber(m_state, 0.0);
    lua_setfield(m_state, -2, "deltaTime");
    lua_pushnumber(m_state, 0.0);
    lua_setfield(m_state, -2, "unscaledDeltaTime");
    lua_pushnumber(m_state, 0.02);
    lua_setfield(m_state, -2, "fixedDeltaTime");
    lua_pushnumber(m_state, 0.1);
    lua_setfield(m_state, -2, "maximumDeltaTime");
    lua_pushnumber(m_state, 1.0);
    lua_setfield(m_state, -2, "timeScale");
    lua_pushnumber(m_state, 0.0);
    lua_setfield(m_state, -2, "time");
    lua_pushnumber(m_state, 0.0);
    lua_setfield(m_state, -2, "unscaledTime");
    lua_pushinteger(m_state, 0);
    lua_setfield(m_state, -2, "frameCount");
    lua_pushinteger(m_state, 0);
    lua_setfield(m_state, -2, "fixedFrameCount");
    lua_setfield(m_state, -2, "Time");
    lua_newtable(m_state);
    lua_pushcfunction(m_state, &LuaGetKey);
    lua_setfield(m_state, -2, "GetKey");
    lua_pushcfunction(m_state, &LuaGetKeyDown);
    lua_setfield(m_state, -2, "GetKeyDown");
    lua_pushcfunction(m_state, &LuaGetKeyUp);
    lua_setfield(m_state, -2, "GetKeyUp");
    lua_pushcfunction(m_state, &LuaGetMouseButton);
    lua_setfield(m_state, -2, "GetMouseButton");
    lua_pushcfunction(m_state, &LuaGetMouseButtonDown);
    lua_setfield(m_state, -2, "GetMouseButtonDown");
    lua_pushcfunction(m_state, &LuaGetMouseButtonUp);
    lua_setfield(m_state, -2, "GetMouseButtonUp");
    PushVector3(m_state, {});
    lua_setfield(m_state, -2, "mousePosition");
    PushVector2(m_state, {});
    lua_setfield(m_state, -2, "mouseScrollDelta");
    lua_setfield(m_state, -2, "Input");
    lua_newtable(m_state);
    lua_pushcfunction(m_state, &LuaGetActiveScene);
    lua_setfield(m_state, -2, "GetActiveScene");
    lua_pushcfunction(m_state, &LuaLoadScene);
    lua_setfield(m_state, -2, "LoadScene");
    lua_setfield(m_state, -2, "SceneManager");
    lua_pushcfunction(m_state, &LuaFind);
    lua_setfield(m_state, -2, "Find");
    lua_pushcfunction(m_state, &LuaFindWithTag);
    lua_setfield(m_state, -2, "FindWithTag");
    lua_pushcfunction(m_state, &LuaInstantiate);
    lua_setfield(m_state, -2, "Instantiate");
    lua_pushcfunction(m_state, &NativeDestroyObject);
    lua_setfield(m_state, -2, "Destroy");
    lua_pushcfunction(m_state, &NativeGetComponent);
    lua_setfield(m_state, -2, "GetComponent");
    lua_pushcfunction(m_state, &NativeAddComponent);
    lua_setfield(m_state, -2, "AddComponent");
    lua_setglobal(m_state, "Nullus");
    m_lastPublishedTimeScale = ScriptEngineApi::GetTimeScale();
    if (m_luaPandaEnabled)
    {
        const auto debugStatus = InitializeLuaPanda();
        if (!debugStatus.Succeeded())
        {
            lua_close(m_state);
            m_state = nullptr;
            return debugStatus;
        }
    }
    m_initialized = true;
    return ScriptStatus::Ok();
#endif
}

void LuaScriptBackend::Shutdown()
{
#if NLS_HAS_LUA_VM
    if (m_state)
    {
        ShutdownLuaPanda();
        for (auto& [instance, state] : m_instances)
        {
            (void)instance;
            ReleaseInstance(state);
        }
        for (auto& [assetId, artifact] : m_artifacts)
        {
            (void)assetId;
            ReleaseModule(artifact);
        }
        for (auto& [assetId, artifact] : m_pendingArtifacts)
        {
            (void)assetId;
            ReleaseModule(artifact);
        }
        lua_close(m_state);
        m_state = nullptr;
    }
#endif
    m_instances.clear();
    m_artifacts.clear();
    m_pendingArtifacts.clear();
    m_frames.clear();
    m_destroyed.clear();
    m_lastDiagnostic.reset();
    m_initialized = false;
}

ScriptStatus LuaScriptBackend::SetLuaPandaDebugging(bool enabled, std::string host, uint16_t port)
{
    if (enabled && host != "127.0.0.1")
        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "LuaPanda accepts only 127.0.0.1 in the Editor debugger.");
    if (enabled && port == 0)
        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "LuaPanda requires a non-zero local connection port.");

    if (!enabled)
    {
        m_luaPandaEnabled = false;
        m_luaPandaConnectionState = LuaPandaConnectionState::Disabled;
        m_luaPandaHost = std::move(host);
        m_luaPandaPort = port;
#if NLS_HAS_LUA_VM
        if (m_initialized && m_luaPandaEnvironmentReference >= 0)
            ShutdownLuaPanda();
#endif
        return ScriptStatus::Ok();
    }

#if !NLS_ENABLE_LUAPANDA_DEBUGGING || !NLS_HAS_LUAPANDA_SOCKET
    m_luaPandaConnectionState = LuaPandaConnectionState::Disabled;
    return ScriptStatus::Error(
        ScriptStatusCode::BackendUnavailable,
        "LuaPanda debugging is not compiled with the repository-local LuaSocket dependency.");
#else
    m_luaPandaHost = std::move(host);
    m_luaPandaPort = port;
    m_luaPandaEnabled = true;
    m_luaPandaConnectionState = LuaPandaConnectionState::WaitingForAttach;
#if NLS_HAS_LUA_VM
    if (m_initialized && m_luaPandaEnvironmentReference < 0)
    {
        const auto status = InitializeLuaPanda();
        if (!status.Succeeded())
        {
            m_luaPandaEnabled = false;
            return status;
        }
    }
#endif
    return ScriptStatus::Ok();
#endif
}

#if NLS_HAS_LUA_VM
int LuaScriptBackend::LuaPandaConnectSuccessHook(lua_State* state)
{
    auto* backend = static_cast<LuaScriptBackend*>(lua_touserdata(state, lua_upvalueindex(2)));
    const int argumentCount = lua_gettop(state);
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_insert(state, 1);
    if (lua_pcall(state, argumentCount, LUA_MULTRET, 0) != LUA_OK)
        return lua_error(state);
    if (backend != nullptr)
        backend->m_luaPandaConnectionState = LuaPandaConnectionState::Connected;
    return lua_gettop(state);
}

int LuaScriptBackend::LuaPandaDisconnectHook(lua_State* state)
{
    auto* backend = static_cast<LuaScriptBackend*>(lua_touserdata(state, lua_upvalueindex(2)));
    const int argumentCount = lua_gettop(state);
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_insert(state, 1);
    if (lua_pcall(state, argumentCount, LUA_MULTRET, 0) != LUA_OK)
        return lua_error(state);
    if (backend != nullptr)
        backend->m_luaPandaConnectionState = LuaPandaConnectionState::Disabled;
    return lua_gettop(state);
}

void LuaScriptBackend::InstallLuaPandaHooks(lua_State* state, const int environment)
{
    lua_getfield(state, environment, "LuaPanda");
    if (!lua_istable(state, -1))
    {
        lua_pop(state, 1);
        return;
    }

    const int debugger = lua_absindex(state, -1);
    const auto install = [state, debugger, this](
                             const char* name,
                             lua_CFunction replacement)
    {
        lua_getfield(state, debugger, name);
        if (!lua_isfunction(state, -1))
        {
            lua_pop(state, 1);
            return;
        }
        lua_pushvalue(state, -1);
        lua_pushlightuserdata(state, this);
        lua_pushcclosure(state, replacement, 2);
        lua_setfield(state, debugger, name);
        lua_pop(state, 1);
    };
    install("connectSuccess", &LuaScriptBackend::LuaPandaConnectSuccessHook);
    install("disconnect", &LuaScriptBackend::LuaPandaDisconnectHook);
    lua_pop(state, 1);
}
#endif

ScriptStatus LuaScriptBackend::InitializeLuaPanda()
{
#if !NLS_ENABLE_LUAPANDA_DEBUGGING || !NLS_HAS_LUAPANDA_SOCKET
    return ScriptStatus::Error(
        ScriptStatusCode::BackendUnavailable,
        "LuaPanda debugging is not compiled with the repository-local LuaSocket dependency.");
#else
    if (!m_state)
        return ScriptStatus::Error(ScriptStatusCode::BackendClosed, "Lua state is not initialized.");
    if (m_luaPandaHost != "127.0.0.1" || m_luaPandaPort == 0)
        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "LuaPanda accepts only 127.0.0.1 and a non-zero port.");

    std::ifstream input(NLS_LUAPANDA_SOURCE_PATH, std::ios::binary);
    if (!input)
        return ScriptStatus::Error(ScriptStatusCode::BackendUnavailable, "The repository-local LuaPanda.lua asset is missing.");
    const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    ScriptAsset debuggerAsset;
    debuggerAsset.language = ScriptLanguage::Lua;
    debuggerAsset.sourcePath = NLS_LUAPANDA_SOURCE_PATH;
    debuggerAsset.sourceText = source;

    lua_newtable(m_state);
    const int environment = lua_gettop(m_state);
    for (const auto* library : {"math", "string", "table", "utf8", "coroutine"})
    {
        lua_getglobal(m_state, library);
        lua_setfield(m_state, environment, library);
    }
    for (const auto* function : {
        "assert", "error", "getmetatable", "ipairs", "next", "pairs", "pcall",
        "print", "rawequal", "rawget", "rawlen", "rawset", "select", "setmetatable",
        "tonumber", "tostring", "type", "warn", "xpcall", "_VERSION", "load"})
    {
        lua_getglobal(m_state, function);
        lua_setfield(m_state, environment, function);
    }
    lua_pushnil(m_state);
    lua_setglobal(m_state, "load");

    // Load debugger-only libraries, then remove their globals before any game
    // chunk is executed. The private environment is the only owner of these
    // tables, so gameplay still observes nil for debug/io/os/package.
    luaL_requiref(m_state, LUA_DBLIBNAME, luaopen_debug, 1);
    lua_pop(m_state, 1);
    lua_getglobal(m_state, LUA_DBLIBNAME);
    lua_setfield(m_state, environment, LUA_DBLIBNAME);
    lua_pushnil(m_state);
    lua_setglobal(m_state, LUA_DBLIBNAME);

    luaL_requiref(m_state, LUA_IOLIBNAME, luaopen_io, 1);
    lua_pop(m_state, 1);
    lua_getglobal(m_state, LUA_IOLIBNAME);
    lua_setfield(m_state, environment, LUA_IOLIBNAME);
    lua_pushnil(m_state);
    lua_setglobal(m_state, LUA_IOLIBNAME);

    luaL_requiref(m_state, LUA_OSLIBNAME, luaopen_os, 1);
    lua_pop(m_state, 1);
    lua_getglobal(m_state, LUA_OSLIBNAME);
    lua_setfield(m_state, environment, LUA_OSLIBNAME);
    lua_pushnil(m_state);
    lua_setglobal(m_state, LUA_OSLIBNAME);

    luaL_requiref(m_state, LUA_LOADLIBNAME, luaopen_package, 1);
    lua_pop(m_state, 1);
    lua_getglobal(m_state, LUA_LOADLIBNAME);
    lua_setfield(m_state, environment, LUA_LOADLIBNAME);
    lua_pushnil(m_state);
    lua_setglobal(m_state, LUA_LOADLIBNAME);
    lua_getglobal(m_state, "require");
    lua_setfield(m_state, environment, "require");
    lua_pushnil(m_state);
    lua_setglobal(m_state, "require");
    lua_getglobal(m_state, "collectgarbage");
    lua_setfield(m_state, environment, "collectgarbage");
    lua_pushnil(m_state);
    lua_setglobal(m_state, "collectgarbage");
    lua_pushvalue(m_state, environment);
    lua_setfield(m_state, environment, "_G");

    luaopen_socket_core(m_state);
    if (!lua_istable(m_state, -1))
    {
        lua_settop(m_state, 0);
        return ScriptStatus::Error(ScriptStatusCode::BackendUnavailable, "LuaSocket socket.core did not return a module table.");
    }
    const int socketModule = lua_gettop(m_state);
    lua_pushvalue(m_state, socketModule);
    lua_setfield(m_state, environment, "socket");
    lua_getfield(m_state, environment, LUA_LOADLIBNAME);
    if (lua_istable(m_state, -1))
    {
        lua_getfield(m_state, -1, "loaded");
        if (lua_istable(m_state, -1))
        {
            lua_pushvalue(m_state, socketModule);
            lua_setfield(m_state, -2, "socket.core");
        }
    }
    lua_settop(m_state, environment);

    if (luaL_loadbufferx(m_state, source.data(), source.size(), debuggerAsset.sourcePath.c_str(), "t") != LUA_OK)
    {
        const auto diagnostic = LuaError(m_state, debuggerAsset, "LuaPanda source could not be loaded.");
        m_lastDiagnostic = MakeLuaScriptError(debuggerAsset, {}, diagnostic);
        return ScriptStatus::Error(ScriptStatusCode::CompilationFailed, diagnostic.message);
    }
    lua_pushvalue(m_state, environment);
    lua_setupvalue(m_state, -2, 1);
    if (lua_pcall(m_state, 0, 0, 0) != LUA_OK)
    {
        const auto diagnostic = LuaError(m_state, debuggerAsset, "LuaPanda module execution failed.");
        m_lastDiagnostic = MakeLuaScriptError(debuggerAsset, {}, diagnostic);
        return ScriptStatus::Error(ScriptStatusCode::CompilationFailed, diagnostic.message);
    }
    lua_getfield(m_state, environment, "LuaPanda");
    if (!lua_istable(m_state, -1))
    {
        lua_settop(m_state, environment);
        return ScriptStatus::Error(ScriptStatusCode::BackendUnavailable, "LuaPanda module did not expose a LuaPanda table.");
    }
    const int debugger = lua_absindex(m_state, -1);
    InstallLuaPandaHooks(m_state, environment);
    m_luaPandaConnectionState = LuaPandaConnectionState::WaitingForAttach;
    lua_getfield(m_state, debugger, "start");
    lua_pushstring(m_state, m_luaPandaHost.c_str());
    lua_pushinteger(m_state, static_cast<lua_Integer>(m_luaPandaPort));
    if (lua_pcall(m_state, 2, 0, 0) != LUA_OK)
    {
        m_luaPandaConnectionState = LuaPandaConnectionState::Disabled;
        const auto diagnostic = LuaError(m_state, debuggerAsset, "LuaPanda debugger could not start.");
        m_lastDiagnostic = MakeLuaScriptError(debuggerAsset, {}, diagnostic);
        return ScriptStatus::Error(ScriptStatusCode::RuntimeError, diagnostic.message);
    }
    lua_settop(m_state, environment);
    m_luaPandaEnvironmentReference = luaL_ref(m_state, LUA_REGISTRYINDEX);
    NLS_LOG_INFO(
        std::string("LuaPanda debugger is waiting for VS Code at ") +
        m_luaPandaHost + ":" + std::to_string(m_luaPandaPort));
    return ScriptStatus::Ok();
#endif
}

void LuaScriptBackend::ShutdownLuaPanda()
{
#if NLS_HAS_LUA_VM
    if (!m_state || m_luaPandaEnvironmentReference < 0)
        return;
    lua_rawgeti(m_state, LUA_REGISTRYINDEX, m_luaPandaEnvironmentReference);
    lua_getfield(m_state, -1, "LuaPanda");
    lua_getfield(m_state, -1, "disconnect");
    if (lua_isfunction(m_state, -1))
        (void)lua_pcall(m_state, 0, 0, 0);
    lua_settop(m_state, 0);
    luaL_unref(m_state, LUA_REGISTRYINDEX, m_luaPandaEnvironmentReference);
    m_luaPandaEnvironmentReference = -2;
#endif
    m_luaPandaConnectionState = LuaPandaConnectionState::Disabled;
}

ScriptStatus LuaScriptBackend::CaptureFrame(const ScriptFrameContext& frame)
{
    if (!m_initialized)
        return ScriptStatus::Error(ScriptStatusCode::BackendClosed, "Lua state is not initialized.");
    m_frames.push_back(frame);
    return ScriptStatus::Ok();
}

ScriptStatus LuaScriptBackend::ValidateSource(const ScriptAsset& asset)
{
    if (asset.language != ScriptLanguage::Lua || !asset.assetId.IsValid())
        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "Lua assets require a valid AssetId.");
    if (asset.sourceText.empty())
        return ScriptStatus::Error(ScriptStatusCode::CompilationFailed, "Lua source is empty.");
    // The corresponding libraries are absent from the VM, but reject actual
    // global accesses during import as well so an accidental sandbox escape is
    // reported with an asset location instead of silently becoming nil.  This
    // small lexer deliberately skips strings/comments; a raw substring scan
    // would reject harmless text such as "os" in a diagnostic message.
    const auto isIdentifierStart = [](const char value)
    {
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || value == '_';
    };
    const auto isIdentifierPart = [&isIdentifierStart](const char value)
    {
        return isIdentifierStart(value) || (value >= '0' && value <= '9');
    };
    const auto skipLongBracket = [&asset](size_t& position, int& line)
    {
        if (position >= asset.sourceText.size() || asset.sourceText[position] != '[')
            return false;
        size_t cursor = position + 1;
        while (cursor < asset.sourceText.size() && asset.sourceText[cursor] == '=')
            ++cursor;
        if (cursor >= asset.sourceText.size() || asset.sourceText[cursor] != '[')
            return false;
        const auto equals = cursor - position - 1;
        cursor++;
        while (cursor < asset.sourceText.size())
        {
            if (asset.sourceText[cursor] == '\n')
                ++line;
            if (asset.sourceText[cursor] == ']')
            {
                size_t close = cursor + 1;
                size_t count = 0;
                while (count < equals && close < asset.sourceText.size() && asset.sourceText[close] == '=')
                {
                    ++count;
                    ++close;
                }
                if (count == equals && close < asset.sourceText.size() && asset.sourceText[close] == ']')
                {
                    position = close + 1;
                    return true;
                }
            }
            ++cursor;
        }
        position = asset.sourceText.size();
        return true;
    };

    size_t position = 0;
    int line = 1;
    char previousSignificant = 0;
    std::string previousToken;
    while (position < asset.sourceText.size())
    {
        const auto current = asset.sourceText[position];
        if (current == '\n')
        {
            ++line;
            ++position;
            previousSignificant = 0;
            previousToken.clear();
            continue;
        }
        if (current == '-' && position + 1 < asset.sourceText.size() && asset.sourceText[position + 1] == '-')
        {
            position += 2;
            if (!skipLongBracket(position, line))
                while (position < asset.sourceText.size() && asset.sourceText[position] != '\n')
                    ++position;
            continue;
        }
        if (current == '\'' || current == '"')
        {
            const auto quote = current;
            ++position;
            while (position < asset.sourceText.size())
            {
                if (asset.sourceText[position] == '\n')
                    ++line;
                if (asset.sourceText[position] == '\\')
                {
                    position += std::min<size_t>(2, asset.sourceText.size() - position);
                    continue;
                }
                if (asset.sourceText[position++] == quote)
                    break;
            }
            previousSignificant = quote;
            previousToken.clear();
            continue;
        }
        if (current == '[' && skipLongBracket(position, line))
        {
            previousSignificant = ']';
            previousToken.clear();
            continue;
        }
        if (!isIdentifierStart(current))
        {
            if (current != ' ' && current != '\t' && current != '\r')
                previousSignificant = current;
            ++position;
            continue;
        }

        const auto start = position++;
        while (position < asset.sourceText.size() && isIdentifierPart(asset.sourceText[position]))
            ++position;
        const auto token = asset.sourceText.substr(start, position - start);
        const auto next = [&asset, position]
        {
            auto cursor = position;
            while (cursor < asset.sourceText.size() && (asset.sourceText[cursor] == ' ' || asset.sourceText[cursor] == '\t' || asset.sourceText[cursor] == '\r'))
                ++cursor;
            return cursor < asset.sourceText.size() ? asset.sourceText[cursor] : '\0';
        }();
        const bool localBinding = previousToken == "local" && next == '=';
        const bool tableKey = (previousSignificant == '{' || previousSignificant == ',') && next == '=';
        const bool memberAccess = previousSignificant == '.' || previousSignificant == ':';
        if ((token == "io" || token == "os" || token == "debug") && !localBinding && !tableKey && !memberAccess)
        {
            auto message = asset.sourcePath.empty() ? std::string{} : asset.sourcePath + ":" + std::to_string(line) + ": ";
            message += "Lua standard library access is disabled for ScriptAsset code.";
            return ScriptStatus::Error(ScriptStatusCode::CompilationFailed, std::move(message));
        }
        previousToken = token;
        previousSignificant = '_';
    }
    return ScriptStatus::Ok();
}

ScriptStatus LuaScriptBackend::LoadModule(const ScriptAsset& asset, Artifact& output)
{
#if !NLS_HAS_LUA_VM
    (void)asset;
    (void)output;
    return ScriptStatus::Error(ScriptStatusCode::BackendUnavailable, "Lua 5.4.8 is not linked into this build.");
#else
    if (luaL_loadbufferx(m_state, asset.sourceText.data(), asset.sourceText.size(), asset.sourcePath.c_str(), "t") != LUA_OK)
    {
        const auto diagnostic = LuaError(m_state, asset, "Lua syntax validation failed.");
        m_lastDiagnostic = MakeLuaScriptError(asset, {}, diagnostic);
        return ScriptStatus::Error(ScriptStatusCode::CompilationFailed, diagnostic.message);
    }
    if (lua_pcall(m_state, 0, 1, 0) != LUA_OK)
    {
        const auto diagnostic = LuaError(m_state, asset, "Lua module execution failed.");
        m_lastDiagnostic = MakeLuaScriptError(asset, {}, diagnostic);
        return ScriptStatus::Error(ScriptStatusCode::CompilationFailed, diagnostic.message);
    }
    if (!lua_istable(m_state, -1))
    {
        lua_settop(m_state, 0);
        return ScriptStatus::Error(ScriptStatusCode::CompilationFailed, "A Lua ScriptAsset must return a module table.");
    }
    output = {asset, asset.contentHash != 0 ? asset.contentHash : MakeScriptContentHash(asset.sourceText), luaL_ref(m_state, LUA_REGISTRYINDEX)};
    return ScriptStatus::Ok();
#endif
}

ScriptStatus LuaScriptBackend::LoadScript(const ScriptAsset& asset)
{
    m_lastDiagnostic.reset();
    if (!m_initialized)
        return ScriptStatus::Error(ScriptStatusCode::BackendClosed, "Lua state is not initialized.");
    const auto validation = ValidateSource(asset);
    if (!validation.Succeeded())
        return validation;
    Artifact replacement;
    const auto status = LoadModule(asset, replacement);
    if (!status.Succeeded())
        return status;
    const auto previous = m_artifacts.find(asset.assetId);
    if (previous == m_artifacts.end())
    {
        m_artifacts.emplace(asset.assetId, std::move(replacement));
        return ScriptStatus::Ok();
    }

    // The runtime uses LoadScript(previousAsset) to clear a staged artifact
    // after a rejected commit.  Avoid compiling a second copy of the active
    // module when that rollback asset is byte-for-byte the accepted one.
    if (previous->second.contentHash == replacement.contentHash
        && previous->second.asset.sourcePath == replacement.asset.sourcePath
        && previous->second.asset.scriptType == replacement.asset.scriptType)
    {
        ReleaseModule(replacement);
        const auto pending = m_pendingArtifacts.find(asset.assetId);
        if (pending != m_pendingArtifacts.end())
        {
            ReleaseModule(pending->second);
            m_pendingArtifacts.erase(pending);
        }
        return ScriptStatus::Ok();
    }

    // Existing assets are staged until ScriptRuntime reaches the explicit
    // frame-boundary commit.  This keeps OnDisable on the old prototype and
    // lets a failed reload leave both instances and the active artifact alone.
    const auto pending = m_pendingArtifacts.find(asset.assetId);
    if (pending != m_pendingArtifacts.end())
    {
        ReleaseModule(pending->second);
        m_pendingArtifacts.erase(pending);
    }
    m_pendingArtifacts.emplace(asset.assetId, std::move(replacement));
    return ScriptStatus::Ok();
}

ScriptStatus LuaScriptBackend::UnloadScript(const NLS::Core::Assets::AssetId& assetId)
{
    const auto pending = m_pendingArtifacts.find(assetId);
    if (pending != m_pendingArtifacts.end())
    {
#if NLS_HAS_LUA_VM
        ReleaseModule(pending->second);
#endif
        m_pendingArtifacts.erase(pending);
    }
    const auto found = m_artifacts.find(assetId);
    if (found == m_artifacts.end())
        return ScriptStatus::Ok();
#if NLS_HAS_LUA_VM
    ReleaseModule(found->second);
#endif
    m_artifacts.erase(found);
    return ScriptStatus::Ok();
}

ScriptStatus LuaScriptBackend::CreateInstance(const ScriptAsset& asset, NativeObjectHandle owner, ScriptInstanceHandle& output)
{
    output = {};
    const auto artifact = m_artifacts.find(asset.assetId);
    if (!m_initialized)
        return ScriptStatus::Error(ScriptStatusCode::BackendClosed, "Lua state is not initialized.");
    if (artifact == m_artifacts.end())
        return ScriptStatus::Error(ScriptStatusCode::AssetNotLoaded, "Lua ScriptAsset has not been compiled into an artifact.");
#if !NLS_HAS_LUA_VM
    (void)owner;
    return ScriptStatus::Error(ScriptStatusCode::BackendUnavailable, "Lua 5.4.8 is not linked into this build.");
#else
    lua_rawgeti(m_state, LUA_REGISTRYINDEX, artifact->second.moduleReference);
    lua_newtable(m_state);
    lua_newtable(m_state);
    lua_pushvalue(m_state, -3);
    lua_setfield(m_state, -2, "__index");
    lua_setmetatable(m_state, -2);
    lua_pushinteger(m_state, static_cast<lua_Integer>(owner.value));
    lua_setfield(m_state, -2, "__nls_owner");
    if (owner.IsValid())
    {
        auto* native = NLS::Object::IDToPointerNoThreadCheck(
            static_cast<NLS::InstanceID>(owner.value & 0xFFFFFFFFu));
        auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(native);
        if (!gameObject)
        {
            if (auto* component = dynamic_cast<NLS::Engine::Components::Component*>(native))
                gameObject = component->gameobject();
        }
        if (gameObject)
            PushNativeObject(m_state, NativeObjectHandle::FromInstanceId(gameObject->GetInstanceID()));
        else
            PushNativeObject(m_state, owner);
        lua_setfield(m_state, -2, "gameObject");
        PushNativeObject(m_state, owner);
        lua_setfield(m_state, -2, "owner");
    }
    const auto tableReference = luaL_ref(m_state, LUA_REGISTRYINDEX);
    lua_pop(m_state, 1); // module prototype
    output = {m_id.value, m_nextGeneration++, m_nextIndex++};
    m_instances.emplace(output, Instance{artifact->second.asset, owner, {}, tableReference});
    return ScriptStatus::Ok();
#endif
}

ScriptStatus LuaScriptBackend::DestroyInstance(ScriptInstanceHandle instance)
{
    if (!instance.IsValid())
        return ScriptStatus::Ok();
    const auto found = m_instances.find(instance);
    if (found == m_instances.end())
        return m_destroyed.contains(instance)
            ? ScriptStatus::Ok()
            : ScriptStatus::Error(ScriptStatusCode::InvalidHandle, "Lua instance handle is invalid.");
#if NLS_HAS_LUA_VM
    ReleaseInstance(found->second);
#endif
    m_instances.erase(found);
    m_destroyed.insert(instance);
    return ScriptStatus::Ok();
}

ScriptStatus LuaScriptBackend::Invoke(ScriptInstanceHandle instance, ScriptCallback callback, const ScriptInvocationContext& context)
{
    if (!m_instances.contains(instance))
        return ScriptStatus::Error(m_destroyed.contains(instance) ? ScriptStatusCode::AlreadyDestroyed : ScriptStatusCode::InvalidHandle, "Lua instance handle is invalid.");
    if (m_callback)
        return m_callback(instance, callback, context);
    return InvokeLua(instance, callback, context);
}

ScriptStatus LuaScriptBackend::InvokeLua(ScriptInstanceHandle instance, ScriptCallback callback, const ScriptInvocationContext& context)
{
#if !NLS_HAS_LUA_VM
    (void)instance;
    (void)callback;
    (void)context;
    return ScriptStatus::Error(ScriptStatusCode::BackendUnavailable, "Lua 5.4.8 is not linked into this build.");
#else
    const auto found = m_instances.find(instance);
    if (found == m_instances.end())
        return ScriptStatus::Error(ScriptStatusCode::InvalidHandle, "Lua instance handle is invalid.");
    lua_getglobal(m_state, "Nullus");
    lua_getfield(m_state, -1, "Time");
    lua_getfield(m_state, -1, "timeScale");
    if (lua_isnumber(m_state, -1))
    {
        const auto requestedScale = static_cast<float>(lua_tonumber(m_state, -1));
        if (std::abs(requestedScale - m_lastPublishedTimeScale) > 1e-6f)
            ScriptEngineApi::SetTimeScale(requestedScale);
    }
    lua_pop(m_state, 1);
    const auto timeScale = ScriptEngineApi::GetTimeScale();
    lua_pushnumber(m_state, context.frame.deltaTime); lua_setfield(m_state, -2, "deltaTime");
    lua_pushnumber(m_state, context.frame.unscaledDeltaTime); lua_setfield(m_state, -2, "unscaledDeltaTime");
    lua_pushnumber(m_state, context.frame.fixedDeltaTime); lua_setfield(m_state, -2, "fixedDeltaTime");
    lua_pushnumber(m_state, timeScale); lua_setfield(m_state, -2, "timeScale");
    lua_pushnumber(m_state, context.frame.time); lua_setfield(m_state, -2, "time");
    lua_pushnumber(m_state, context.frame.unscaledTime); lua_setfield(m_state, -2, "unscaledTime");
    lua_pushinteger(m_state, static_cast<lua_Integer>(context.frame.frameIndex)); lua_setfield(m_state, -2, "frameCount");
    lua_pushinteger(m_state, static_cast<lua_Integer>(context.frame.fixedFrameIndex)); lua_setfield(m_state, -2, "fixedFrameCount");
    lua_getfield(m_state, -2, "Input");
    if (lua_istable(m_state, -1))
    {
        const auto* input = ScriptEngineApi::GetInputManager();
        const auto mousePosition = input ? input->GetMousePosition() : NLS::Maths::Vector2{};
        const auto mouseScroll = input ? input->GetWheelMovement() : NLS::Maths::Vector2{};
        PushVector3(m_state, {mousePosition.x, mousePosition.y, 0.0f});
        lua_setfield(m_state, -2, "mousePosition");
        PushVector2(m_state, mouseScroll);
        lua_setfield(m_state, -2, "mouseScrollDelta");
    }
    lua_pop(m_state, 1);
    m_lastPublishedTimeScale = timeScale;
    lua_pop(m_state, 2);
    lua_rawgeti(m_state, LUA_REGISTRYINDEX, found->second.tableReference);
    lua_getfield(m_state, -1, CallbackName(callback));
    if (lua_isnil(m_state, -1))
    {
        lua_settop(m_state, 0);
        return ScriptStatus::Ok();
    }
    if (!lua_isfunction(m_state, -1))
    {
        lua_settop(m_state, 0);
        return ScriptStatus::Error(ScriptStatusCode::RuntimeError, "Lua lifecycle member is not callable.");
    }
    lua_pushvalue(m_state, -2);
    // All lifecycle methods use the same no-argument contract as C#; scripts
    // read scaled and unscaled values from Nullus.Time.
    if (lua_pcall(m_state, 1, 0, 0) != LUA_OK)
    {
        const auto diagnostic = LuaError(m_state, found->second.asset, "Lua lifecycle callback failed.");
        m_lastDiagnostic = MakeLuaScriptError(found->second.asset, instance, diagnostic);
        return ScriptStatus::Error(ScriptStatusCode::RuntimeError, diagnostic.message);
    }
    lua_settop(m_state, 0);
    return ScriptStatus::Ok();
#endif
}

ScriptStatus LuaScriptBackend::Reload(const NLS::Core::Assets::AssetId& assetId, const ScriptApiDatabase& api)
{
    if (!m_initialized)
        return ScriptStatus::Error(ScriptStatusCode::BackendClosed, "Lua state is not initialized.");
    const auto old = m_artifacts.find(assetId);
    if (old == m_artifacts.end())
        return ScriptStatus::Error(ScriptStatusCode::AssetNotLoaded, "Lua ScriptAsset has not been compiled into an artifact.");
    const auto replacementIt = m_pendingArtifacts.find(assetId);
    if (replacementIt == m_pendingArtifacts.end())
        return ScriptStatus::Error(ScriptStatusCode::AssetNotLoaded, "Lua replacement artifact has not been staged.");
    if (api.GetSchemaHashHex() != m_api.GetSchemaHashHex())
    {
        ReleaseModule(replacementIt->second);
        m_pendingArtifacts.erase(replacementIt);
        return ScriptStatus::Error(ScriptStatusCode::SchemaMismatch, "Lua Script API schema mismatch.");
    }

    Artifact replacement = std::move(replacementIt->second);
    replacementIt->second.moduleReference = -2;
    m_pendingArtifacts.erase(replacementIt);

#if NLS_HAS_LUA_VM
    // LoadScript has already compiled the replacement into the pending map.
    // Existing instances are rebound only after OnDisable has completed.
    for (auto& [instance, state] : m_instances)
    {
        (void)instance;
        if (state.asset.assetId != assetId)
            continue;
        state.asset = old->second.asset;
        lua_rawgeti(m_state, LUA_REGISTRYINDEX, state.tableReference);
        if (lua_getmetatable(m_state, -1) != 0)
        {
            lua_rawgeti(m_state, LUA_REGISTRYINDEX, replacement.moduleReference);
            lua_setfield(m_state, -2, "__index");
            lua_pop(m_state, 1);
        }
        lua_pop(m_state, 1);
        state.asset = replacement.asset;
    }
#else
    for (auto& [instance, state] : m_instances)
        if (state.asset.assetId == assetId)
            state.asset = replacement.asset;
#endif
    ReleaseModule(old->second);
    old->second = std::move(replacement);
    m_api = api;
    return ScriptStatus::Ok();
}

const ScriptFieldDescriptor* LuaScriptBackend::FindField(const Instance& instance, ScriptFieldId field) const
{
    const auto* type = m_api.FindType(instance.asset.scriptType);
    if (!type)
        return nullptr;
    const auto found = std::find_if(type->fields.begin(), type->fields.end(), [field](const auto& descriptor) { return descriptor.id == field; });
    return found == type->fields.end() ? nullptr : &*found;
}

#if NLS_HAS_LUA_VM
bool LuaScriptBackend::PushValue(lua_State* state, const ScriptValue& value)
{
    return std::visit([state](const auto& item)
    {
        using Value = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Value, std::monostate>) lua_pushnil(state);
        else if constexpr (std::is_same_v<Value, bool>) lua_pushboolean(state, item);
        else if constexpr (std::is_integral_v<Value> && std::is_signed_v<Value>) lua_pushinteger(state, static_cast<lua_Integer>(item));
        else if constexpr (std::is_integral_v<Value> && std::is_unsigned_v<Value>) lua_pushinteger(state, static_cast<lua_Integer>(item));
        else if constexpr (std::is_floating_point_v<Value>) lua_pushnumber(state, static_cast<lua_Number>(item));
        else if constexpr (std::is_same_v<Value, std::string>) lua_pushlstring(state, item.data(), item.size());
        else if constexpr (std::is_same_v<Value, NativeObjectHandle>) PushNativeObject(state, item);
        else if constexpr (std::is_same_v<Value, NLS::Maths::Vector2>)
        {
            lua_createtable(state, 0, 2); lua_pushnumber(state, item.x); lua_setfield(state, -2, "x"); lua_pushnumber(state, item.y); lua_setfield(state, -2, "y");
        }
        else if constexpr (std::is_same_v<Value, NLS::Maths::Vector3>)
        {
            lua_createtable(state, 0, 3); lua_pushnumber(state, item.x); lua_setfield(state, -2, "x"); lua_pushnumber(state, item.y); lua_setfield(state, -2, "y"); lua_pushnumber(state, item.z); lua_setfield(state, -2, "z");
        }
        else if constexpr (std::is_same_v<Value, NLS::Maths::Vector4>)
        {
            lua_createtable(state, 0, 4); lua_pushnumber(state, item.x); lua_setfield(state, -2, "x"); lua_pushnumber(state, item.y); lua_setfield(state, -2, "y"); lua_pushnumber(state, item.z); lua_setfield(state, -2, "z"); lua_pushnumber(state, item.w); lua_setfield(state, -2, "w");
        }
        else if constexpr (std::is_same_v<Value, NLS::Maths::Quaternion>)
        {
            lua_createtable(state, 0, 4); lua_pushnumber(state, item.x); lua_setfield(state, -2, "x"); lua_pushnumber(state, item.y); lua_setfield(state, -2, "y"); lua_pushnumber(state, item.z); lua_setfield(state, -2, "z"); lua_pushnumber(state, item.w); lua_setfield(state, -2, "w");
        }
        else if constexpr (std::is_same_v<Value, NLS::Maths::Color>)
        {
            lua_createtable(state, 0, 4); lua_pushnumber(state, item.r); lua_setfield(state, -2, "r"); lua_pushnumber(state, item.g); lua_setfield(state, -2, "g"); lua_pushnumber(state, item.b); lua_setfield(state, -2, "b"); lua_pushnumber(state, item.a); lua_setfield(state, -2, "a");
        }
        else return false;
        return true;
    }, value);
}

bool LuaScriptBackend::ReadValue(lua_State* state, int index, const ScriptType& expected, ScriptValue& output)
{
    const auto readNumber = [state, index](const char* name, float& result)
    {
        lua_getfield(state, index, name);
        const bool valid = lua_isnumber(state, -1);
        if (valid)
            result = static_cast<float>(lua_tonumber(state, -1));
        lua_pop(state, 1);
        return valid;
    };
    switch (expected.kind)
    {
    case ScriptValueKind::Bool:
        if (!lua_isboolean(state, index)) return false;
        output = lua_toboolean(state, index) != 0; return true;
    case ScriptValueKind::String:
        if (!lua_isstring(state, index)) return false;
        output = std::string(lua_tostring(state, index)); return true;
    case ScriptValueKind::Int8: case ScriptValueKind::Int16: case ScriptValueKind::Int32: case ScriptValueKind::Int64:
        if (!lua_isinteger(state, index)) return false;
        switch (expected.kind) { case ScriptValueKind::Int8: output = static_cast<int8_t>(lua_tointeger(state,index)); break; case ScriptValueKind::Int16: output = static_cast<int16_t>(lua_tointeger(state,index)); break; case ScriptValueKind::Int32: output = static_cast<int32_t>(lua_tointeger(state,index)); break; default: output = static_cast<int64_t>(lua_tointeger(state,index)); break; } return true;
    case ScriptValueKind::UInt8: case ScriptValueKind::UInt16: case ScriptValueKind::UInt32: case ScriptValueKind::UInt64:
        if (!lua_isinteger(state, index) || lua_tointeger(state,index) < 0) return false;
        switch (expected.kind) { case ScriptValueKind::UInt8: output = static_cast<uint8_t>(lua_tointeger(state,index)); break; case ScriptValueKind::UInt16: output = static_cast<uint16_t>(lua_tointeger(state,index)); break; case ScriptValueKind::UInt32: output = static_cast<uint32_t>(lua_tointeger(state,index)); break; default: output = static_cast<uint64_t>(lua_tointeger(state,index)); break; } return true;
    case ScriptValueKind::Float: output = static_cast<float>(luaL_checknumber(state, index)); return true;
    case ScriptValueKind::Double: output = static_cast<double>(luaL_checknumber(state, index)); return true;
    case ScriptValueKind::ObjectReference:
        if (const auto* object = TestNativeObject(state, index))
        {
            output = NativeObjectHandle{object->handle};
            return true;
        }
        if (!lua_isinteger(state, index)) return false;
        output = NativeObjectHandle{static_cast<uint64_t>(lua_tointeger(state,index))}; return true;
    case ScriptValueKind::Vector2:
    {
        if (!lua_istable(state, index)) return false; float x = 0, y = 0;
        if (!readNumber("x", x) || !readNumber("y", y)) return false;
        output = NLS::Maths::Vector2{x, y}; return true;
    }
    case ScriptValueKind::Vector3:
    {
        if (!lua_istable(state, index)) return false; float x = 0, y = 0, z = 0;
        if (!readNumber("x", x) || !readNumber("y", y) || !readNumber("z", z)) return false;
        output = NLS::Maths::Vector3{x, y, z}; return true;
    }
    case ScriptValueKind::Vector4:
    case ScriptValueKind::Quaternion:
    {
        if (!lua_istable(state, index)) return false; float x = 0, y = 0, z = 0, w = 0;
        if (!readNumber("x", x) || !readNumber("y", y) || !readNumber("z", z) || !readNumber("w", w)) return false;
        if (expected.kind == ScriptValueKind::Vector4) output = NLS::Maths::Vector4{x, y, z, w};
        else output = NLS::Maths::Quaternion{x, y, z, w};
        return true;
    }
    case ScriptValueKind::Color:
    {
        if (!lua_istable(state, index)) return false; float r = 0, g = 0, b = 0, a = 1;
        if (!readNumber("r", r) || !readNumber("g", g) || !readNumber("b", b) || !readNumber("a", a)) return false;
        output = NLS::Maths::Color{r, g, b, a}; return true;
    }
    default:
        return false;
    }
}
#else
bool LuaScriptBackend::PushValue(lua_State*, const ScriptValue&) { return false; }
bool LuaScriptBackend::ReadValue(lua_State*, int, const ScriptType&, ScriptValue&) { return false; }
#endif

bool LuaScriptBackend::GetField(ScriptInstanceHandle instance, ScriptFieldId field, ScriptValue& output)
{
    const auto found = m_instances.find(instance);
    if (found == m_instances.end())
        return false;
    const auto* descriptor = FindField(found->second, field);
    if (!descriptor)
        return found->second.fields.contains(field) && (output = found->second.fields.at(field), true);
#if NLS_HAS_LUA_VM
    lua_rawgeti(m_state, LUA_REGISTRYINDEX, found->second.tableReference);
    lua_getfield(m_state, -1, descriptor->name.c_str());
    const bool present = !lua_isnil(m_state, -1) && ReadValue(m_state, -1, descriptor->type, output);
    lua_settop(m_state, 0);
    return present;
#else
    return false;
#endif
}

bool LuaScriptBackend::SetField(ScriptInstanceHandle instance, ScriptFieldId field, const ScriptValue& value)
{
    const auto found = m_instances.find(instance);
    if (found == m_instances.end() || field == 0)
        return false;
    const auto* descriptor = FindField(found->second, field);
    if (!descriptor)
    {
        found->second.fields[field] = value;
        return true;
    }
#if NLS_HAS_LUA_VM
    if (!PushValue(m_state, value))
        return false;
    lua_rawgeti(m_state, LUA_REGISTRYINDEX, found->second.tableReference);
    lua_insert(m_state, -2);
    lua_setfield(m_state, -2, descriptor->name.c_str());
    lua_pop(m_state, 1);
    found->second.fields[field] = value;
    return true;
#else
    return false;
#endif
}

std::optional<ScriptError> LuaScriptBackend::ConsumeLastDiagnostic()
{
    auto diagnostic = std::move(m_lastDiagnostic);
    m_lastDiagnostic.reset();
    return diagnostic;
}

void LuaScriptBackend::ReleaseModule(Artifact& artifact)
{
#if NLS_HAS_LUA_VM
    if (m_state && artifact.moduleReference >= 0)
        luaL_unref(m_state, LUA_REGISTRYINDEX, artifact.moduleReference);
#endif
    artifact.moduleReference = -2;
}

void LuaScriptBackend::ReleaseInstance(Instance& instance)
{
#if NLS_HAS_LUA_VM
    if (m_state && instance.tableReference >= 0)
        luaL_unref(m_state, LUA_REGISTRYINDEX, instance.tableReference);
#endif
    instance.tableReference = -2;
}
}
