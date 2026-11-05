#pragma once

#include "ScriptTypes.h"
#include "GameInputFrame.h"

#include <string>

namespace NLS::Engine::SceneSystem
{
class Scene;
class SceneManager;
}

namespace NLS::Windowing::Inputs
{
class InputManager;
}

namespace NLS::Scripting
{
// Process-local services shared by the Game and Editor script hosts. The
// managed and Lua backends intentionally read the same state in either host.
class NLS_SCRIPTING_API ScriptEngineApi final
{
public:
    static void ResetPlayState(uint64_t sessionId);
    static uint64_t GetPlaySessionId();
    static void StopAcceptingPlayRequests();
    static bool IsAcceptingPlayRequests();
    static NativeObjectHandle GetEventOther();
    static bool GetEventBoolean();
    static void SetPlayEvent(NativeObjectHandle other, bool value = false);
    static const ScriptFrameContext& GetFrameContext();
    static void SetFrameContext(const ScriptFrameContext& frame);
    static void SetScreenSize(uint32_t width,uint32_t height);
    static uint32_t GetScreenWidth();
    static uint32_t GetScreenHeight();
    static void SetGameInputFrame(GameInputFrame frame);
    static const GameInputFrame* GetGameInputFrame();
    static bool ReadKey(int key, int transition = 0);
    static bool ReadMouseButton(int button, int transition = 0);
    static Maths::Vector2 ReadMousePosition();
    static Maths::Vector2 ReadMouseScroll();
    static void SetScene(NLS::Engine::SceneSystem::Scene* scene);
    static NLS::Engine::SceneSystem::Scene* GetScene();
    static void SetSceneManager(NLS::Engine::SceneSystem::SceneManager* sceneManager);
    static NLS::Engine::SceneSystem::SceneManager* GetSceneManager();
    static void SetInputManager(NLS::Windowing::Inputs::InputManager* input);
    static NLS::Windowing::Inputs::InputManager* GetInputManager();

    static float GetTimeScale();
    static void SetTimeScale(float value);
    static float GetFixedDeltaTime();
    static float GetMaximumDeltaTime();
    static void SetFrameTiming(float fixedDeltaTime, float maximumDeltaTime);
    static float ScaleDeltaTime(float unscaledDeltaTime);

    // Delayed Object.Destroy requests are collected at a frame boundary after
    // LateUpdate. Requests are handle based so a deleted object is never
    // dereferenced by the queue.
    static bool QueueDestroy(NativeObjectHandle handle, float delaySeconds);
    static bool QueueSceneLoad(std::string path);
    static void AdvanceFrame(float unscaledDeltaTime);
    static void FlushDeferredDestructions();
    static void ClearDeferredDestructions();
};
}
