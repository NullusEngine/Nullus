#include "ScriptEngineApi.h"
#include "InputScriptApi.h"

#include "Base/Object/Object.h"
#include "Engine/Components/Component.h"
#include "Engine/Components/TransformComponent.h"
#include "Engine/GameObject.h"
#include "Engine/SceneSystem/SceneManager.h"
#include "Windowing/Inputs/InputManager.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <vector>
#include <optional>
#include <string>

namespace NLS::Scripting
{
namespace
{
struct DeferredDestroy
{
    NativeObjectHandle handle;
    float remaining = 0.0f;
    uint64_t sessionId = 0;
};

NLS::Engine::SceneSystem::Scene* g_scene = nullptr;
NLS::Engine::SceneSystem::SceneManager* g_sceneManager = nullptr;
NLS::Windowing::Inputs::InputManager* g_input = nullptr;
float g_timeScale = 1.0f;
float g_fixedDeltaTime = 1.0f / 60.0f;
float g_maximumDeltaTime = 0.25f;
std::vector<DeferredDestroy> g_deferred;
std::unordered_set<uint64_t> g_deferredHandles;
std::optional<std::string> g_deferredSceneLoad;
uint64_t g_playSessionId = 0;
bool g_acceptPlayRequests = true;
NativeObjectHandle g_eventOther;
bool g_eventBoolean = false;
ScriptFrameContext g_frameContext;
uint32_t g_screenWidth=1,g_screenHeight=1;
std::optional<GameInputFrame> g_gameInput;
}

void ScriptEngineApi::ResetPlayState(uint64_t sessionId)
{
    InputScriptApi::Reset();
    ClearDeferredDestructions();
    g_playSessionId = sessionId;
    g_timeScale = 1.0f;
    g_fixedDeltaTime = 1.0f / 60.0f;
    g_maximumDeltaTime = 0.25f;
    g_scene = nullptr;
    g_sceneManager = nullptr;
    g_input = nullptr;
    g_eventOther={};g_eventBoolean=false;
    g_frameContext={};g_screenWidth=g_screenHeight=1;
    g_acceptPlayRequests = true;
    g_gameInput = sessionId ? std::optional<GameInputFrame>(GameInputFrame{}) : std::nullopt;
}
uint64_t ScriptEngineApi::GetPlaySessionId() { return g_playSessionId; }
void ScriptEngineApi::StopAcceptingPlayRequests() { g_acceptPlayRequests = false; ClearDeferredDestructions(); }
bool ScriptEngineApi::IsAcceptingPlayRequests() { return g_acceptPlayRequests; }
NativeObjectHandle ScriptEngineApi::GetEventOther() { return g_eventOther; }
bool ScriptEngineApi::GetEventBoolean() { return g_eventBoolean; }
void ScriptEngineApi::SetPlayEvent(NativeObjectHandle other, bool value) { g_eventOther=other;g_eventBoolean=value; }
const ScriptFrameContext& ScriptEngineApi::GetFrameContext(){return g_frameContext;}
void ScriptEngineApi::SetFrameContext(const ScriptFrameContext& frame){g_frameContext=frame;}
void ScriptEngineApi::SetScreenSize(uint32_t width,uint32_t height){g_screenWidth=std::max(1u,width);g_screenHeight=std::max(1u,height);}
uint32_t ScriptEngineApi::GetScreenWidth(){return g_screenWidth;}
uint32_t ScriptEngineApi::GetScreenHeight(){return g_screenHeight;}
void ScriptEngineApi::SetGameInputFrame(GameInputFrame frame)
{
    if (g_playSessionId && frame.sessionId != g_playSessionId) return;
    g_gameInput = std::move(frame);
}
const GameInputFrame* ScriptEngineApi::GetGameInputFrame() { return g_gameInput ? &*g_gameInput : nullptr; }
bool ScriptEngineApi::ReadKey(int key, int transition)
{
    if (g_gameInput)
    {
        if (!g_gameInput->focused || key < 0 || key >= 512) return false;
        return transition == 1 ? g_gameInput->pressed[key] : transition == 2 ? g_gameInput->released[key] : g_gameInput->keys[key];
    }
    if (!g_input || key < 32 || key > 348) return false;
    const auto code = static_cast<Windowing::Inputs::EKey>(key);
    return transition == 1 ? g_input->IsKeyPressed(code) : transition == 2 ? g_input->IsKeyReleased(code)
        : g_input->GetKeyState(code) == Windowing::Inputs::EKeyState::KEY_DOWN;
}
bool ScriptEngineApi::ReadMouseButton(int button, int transition)
{
    if (button < 0 || button >= 8) return false;
    if (g_gameInput)
    {
        if (!g_gameInput->focused) return false;
        return transition == 1 ? g_gameInput->mousePressed[button] : transition == 2 ? g_gameInput->mouseReleased[button] : g_gameInput->mouse[button];
    }
    if (!g_input) return false;
    const auto code = static_cast<Windowing::Inputs::EMouseButton>(button);
    return transition == 1 ? g_input->IsMouseButtonPressed(code) : transition == 2 ? g_input->IsMouseButtonReleased(code)
        : g_input->GetMouseButtonState(code) == Windowing::Inputs::EMouseButtonState::MOUSE_DOWN;
}
Maths::Vector2 ScriptEngineApi::ReadMousePosition()
{
    return g_gameInput ? (g_gameInput->focused ? g_gameInput->ScriptMousePosition() : Maths::Vector2{})
        : g_input ? g_input->GetMousePosition() : Maths::Vector2{};
}
Maths::Vector2 ScriptEngineApi::ReadMouseScroll()
{
    return g_gameInput ? (g_gameInput->focused ? g_gameInput->scroll : Maths::Vector2{})
        : g_input ? g_input->GetWheelMovement() : Maths::Vector2{};
}

void ScriptEngineApi::SetScene(NLS::Engine::SceneSystem::Scene* scene) { g_scene = scene; }
NLS::Engine::SceneSystem::Scene* ScriptEngineApi::GetScene() { return g_scene; }
void ScriptEngineApi::SetSceneManager(NLS::Engine::SceneSystem::SceneManager* sceneManager) { g_sceneManager = sceneManager; }
NLS::Engine::SceneSystem::SceneManager* ScriptEngineApi::GetSceneManager() { return g_sceneManager; }
void ScriptEngineApi::SetInputManager(NLS::Windowing::Inputs::InputManager* input) { g_input = input; }
NLS::Windowing::Inputs::InputManager* ScriptEngineApi::GetInputManager() { return g_input; }

float ScriptEngineApi::GetTimeScale() { return g_timeScale; }
void ScriptEngineApi::SetTimeScale(float value)
{
    if (std::isfinite(value))
        g_timeScale = std::max(0.0f, value);
}
float ScriptEngineApi::GetFixedDeltaTime() { return g_fixedDeltaTime; }
float ScriptEngineApi::GetMaximumDeltaTime() { return g_maximumDeltaTime; }
void ScriptEngineApi::SetFrameTiming(float fixedDeltaTime, float maximumDeltaTime)
{
    if (std::isfinite(fixedDeltaTime) && fixedDeltaTime > 0.0f)
        g_fixedDeltaTime = fixedDeltaTime;
    if (std::isfinite(maximumDeltaTime) && maximumDeltaTime > 0.0f)
        g_maximumDeltaTime = maximumDeltaTime;
}
float ScriptEngineApi::ScaleDeltaTime(float unscaledDeltaTime)
{
    if (!std::isfinite(unscaledDeltaTime) || unscaledDeltaTime <= 0.0f)
        return 0.0f;
    return std::min(unscaledDeltaTime * g_timeScale, g_maximumDeltaTime);
}

bool ScriptEngineApi::QueueDestroy(NativeObjectHandle handle, float delaySeconds)
{
    if (!g_acceptPlayRequests || !handle.IsValid())
        return false;
    if (!g_deferredHandles.insert(handle.value).second)
        return true;
    g_deferred.push_back({handle, std::max(0.0f, delaySeconds), g_playSessionId});
    return true;
}

bool ScriptEngineApi::QueueSceneLoad(std::string path)
{
    if (!g_acceptPlayRequests || !g_sceneManager || path.empty())
        return false;
    g_deferredSceneLoad = std::move(path);
    return true;
}

void ScriptEngineApi::AdvanceFrame(float unscaledDeltaTime)
{
    const auto delta = std::max(0.0f, std::isfinite(unscaledDeltaTime) ? unscaledDeltaTime : 0.0f);
    for (auto& request : g_deferred)
        request.remaining -= delta;
}

void ScriptEngineApi::FlushDeferredDestructions()
{
    std::vector<DeferredDestroy> ready;
    std::vector<DeferredDestroy> pending;
    ready.reserve(g_deferred.size());
    pending.reserve(g_deferred.size());
    for (const auto& request : g_deferred)
    {
        if (request.remaining <= 0.0f)
            ready.push_back(request);
        else
            pending.push_back(request);
    }
    g_deferred = std::move(pending);
    for (const auto& request : ready)
    {
        if (request.sessionId != g_playSessionId) continue;
        g_deferredHandles.erase(request.handle.value);
        auto* object = NLS::Object::IDToPointerNoThreadCheck(
            static_cast<NLS::InstanceID>(request.handle.value & 0xFFFFFFFFu));
        if (!object)
            continue;
        if (auto* gameObject = dynamic_cast<NLS::Engine::GameObject*>(object))
        {
            gameObject->MarkAsDestroy();
            continue;
        }
        if (auto* component = dynamic_cast<NLS::Engine::Components::Component*>(object))
        {
            auto* owner = component->gameobject();
            if (owner && component->GetType() != NLS_TYPEOF(NLS::Engine::Components::TransformComponent))
                owner->RemoveComponent(component);
        }
    }

    if (g_sceneManager && g_deferredSceneLoad.has_value())
    {
        auto path = std::move(*g_deferredSceneLoad);
        g_deferredSceneLoad.reset();
        // Scene replacement is intentionally performed after all lifecycle
        // callbacks and deferred object destruction for this frame.
        const bool playing=g_scene && g_scene->IsPlaying();
        if(g_sceneManager->LoadScene(path, false))
        {
            SetScene(g_sceneManager->GetCurrentScene());
            if(playing && g_scene && !g_scene->IsPlaying())g_scene->Play();
        }
    }
}

void ScriptEngineApi::ClearDeferredDestructions()
{
    g_deferred.clear();
    g_deferredHandles.clear();
    g_deferredSceneLoad.reset();
}
}
