#include "ScriptEngineApi.h"

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
};

NLS::Engine::SceneSystem::Scene* g_scene = nullptr;
NLS::Engine::SceneSystem::SceneManager* g_sceneManager = nullptr;
NLS::Windowing::Inputs::InputManager* g_input = nullptr;
float g_timeScale = 1.0f;
float g_fixedDeltaTime = 0.02f;
float g_maximumDeltaTime = 0.1f;
std::vector<DeferredDestroy> g_deferred;
std::unordered_set<uint64_t> g_deferredHandles;
std::optional<std::string> g_deferredSceneLoad;
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
    if (!handle.IsValid() || delaySeconds <= 0.0f)
        return false;
    if (!g_deferredHandles.insert(handle.value).second)
        return true;
    g_deferred.push_back({handle, delaySeconds});
    return true;
}

bool ScriptEngineApi::QueueSceneLoad(std::string path)
{
    if (!g_sceneManager || path.empty())
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
        g_sceneManager->LoadScene(path, false);
    }
}

void ScriptEngineApi::ClearDeferredDestructions()
{
    g_deferred.clear();
    g_deferredHandles.clear();
    g_deferredSceneLoad.reset();
}
}
