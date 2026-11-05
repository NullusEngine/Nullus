#pragma once
#include "ScriptRuntime.h"
#include <Engine/UI/UIRuntimeService.h>
#include <Engine/SceneSystem/SceneManager.h>
#include <Engine/Audio/AudioDevice.h>
#include <Input/InputDomain.h>
#include <Physics/FixedStepClock.h>
#include <Engine/ECS/RuntimeWorld.h>
namespace NLS::Windowing::Inputs { class PlatformInputBackend; }

namespace NLS::Scripting
{
// Simulation belongs to a session, never to a camera or an editor repaint.
class NLS_SCRIPTING_API RuntimePlaySession final
{
public:
    ~RuntimePlaySession();
    ScriptStatus Begin(ScriptRuntime& scripts, Engine::SceneSystem::SceneManager& scenes, uint64_t sessionId,
        std::shared_ptr<Engine::Audio::AudioDevice> audio = {});
    void Tick(float unscaledDeltaTime);
    void Step();
    using ECSSetup = std::function<void(Engine::ECSIntegration::RuntimeWorld&, Engine::SceneSystem::Scene&)>;
    void EnableECS(ECSSetup setup = {}, ECS::WorldConfig config = {});
    void DisableECS();
    Engine::ECSIntegration::RuntimeWorld* GetECS() { return m_ecs.get(); }
    const Engine::ECSIntegration::RuntimeWorld* GetECS() const { return m_ecs.get(); }
    bool ConfigurePhysicsTiming(const Physics::FixedStepSettings& settings, std::string& error);
    void ConfigureInput(Input::InputSystem& system, Windowing::Inputs::PlatformInputBackend* textBackend = nullptr);
    std::shared_ptr<Input::InputUser> GetInputUser() const { return m_inputUser; }
    Input::InputDomain* GetInputDomain() { return m_inputDomain.get(); }
    void SetPaused(bool paused);
    void NotifyFocus(bool focused);
    void SetMuted(bool muted) { if(m_audio)m_audio->SetMuted(m_sessionId,muted); }
    size_t GetAudioVoiceCount() const { return m_audio ? m_audio->GetVoiceCount(m_sessionId) : 0; }
    ScriptStatus End(bool restoreScriptHost = true);
    void PrepareToStop();
    [[nodiscard]] bool IsActive() const { return m_scripts != nullptr; }
    [[nodiscard]] uint64_t GetSessionId() const { return m_sessionId; }
    [[nodiscard]] const ScriptFrameContext& GetFrame() const { return m_frame; }
    [[nodiscard]] double GetLastTickMilliseconds() const { return m_lastTickMilliseconds; }
    [[nodiscard]] const Physics::FixedStepClock& GetPhysicsClock() const { return m_fixedClock; }
    [[nodiscard]] double GetPhysicsInterpolationAlpha() const { return m_physicsInterpolationAlpha; }
    [[nodiscard]] double GetDroppedPhysicsTime() const { return m_fixedClock.GetDroppedTime() + m_frameClippedPhysicsTime; }
    Engine::UI::UIRuntimeService& GetUI() { return m_ui; }
    void ConfigureUI(std::string assetsRoot,uint32_t width,uint32_t height,float dpi=1,uint32_t display=0);
private:
    void TickFrame(float unscaledDeltaTime, bool singleStep);
    void RunECSPhase(ECS::SystemPhase phase, const ScriptFrameContext& frame);
    bool m_ecsEnabled = false, m_ecsStarted = false;
    ECS::WorldConfig m_ecsConfig;
    ECSSetup m_ecsSetup;
    std::unique_ptr<Engine::ECSIntegration::RuntimeWorld> m_ecs;
    void PumpInput();
    Input::InputSystem* m_inputSystem = nullptr;
    Windowing::Inputs::PlatformInputBackend* m_textBackend = nullptr;
    std::weak_ptr<const void> m_textBackendLifetime;
    uint64_t m_compositionSequence = 0;
    bool m_composing = false;
    std::unique_ptr<Input::InputDomain> m_inputDomain;
    std::shared_ptr<Input::InputUser> m_inputUser;
    uint64_t m_inputFrame = UINT64_MAX;
    bool m_stepInputActive = false;
    Engine::UI::UIVector2 m_uiPointer;
    bool m_hasUiPointer = false;
    void RunPhase(ScriptCallback callback, const ScriptFrameContext& frame, const std::function<void()>& body);
    void DispatchEvent(Engine::GameObject* owner, ScriptCallback callback, const ScriptFrameContext& frame,
        NativeObjectHandle other = {}, bool value = false);
    void DispatchContacts(Engine::SceneSystem::Scene& scene, const std::vector<Physics::ContactEvent>& contacts, const ScriptFrameContext& frame);
    ScriptRuntime* m_scripts = nullptr;
    Engine::SceneSystem::SceneManager* m_scenes = nullptr;
    ScriptFrameContext m_frame;
    Physics::FixedStepClock m_fixedClock;
    bool m_physicsTimingConfigured = false;
    double m_physicsInterpolationAlpha = 0;
    double m_frameClippedPhysicsTime = 0;
    double m_fixedTime = 0;
    uint64_t m_sessionId = 0;
    Engine::UI::UIRuntimeService m_ui;
    std::shared_ptr<Engine::Audio::AudioDevice> m_audio;
    bool m_paused=false;
    bool m_focused=false;
    bool m_endStarted=false;
    std::string m_assetsRoot;
    uint32_t m_uiWidth=1,m_uiHeight=1;
    float m_uiDpi=1;
    double m_lastTickMilliseconds=0;
};
}
