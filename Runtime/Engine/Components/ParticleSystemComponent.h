#pragma once

#include "Components/Component.h"
#include "Assets/ParticleSystemAsset.h"
#include <VFX/SystemInstance.h>
#include <VFX/SystemRenderProxy.h>
#include <Math/Vector4.h>
#include "Components/ParticleSystemComponent.generated.h"

namespace NLS::Engine::Components
{
CLASS(NLS_ENGINE_API ParticleSystemComponent, Scriptable, ComponentMenu("Effects/Particle System"), DisallowMultipleComponent) : public Component
{
public:
    GENERATED_BODY()
    ParticleSystemComponent();
    ~ParticleSystemComponent() override;
    void OnCreate() override;
    void OnStart() override;
    void OnUpdate(float deltaTime) override;
    void OnDestroy() override;
    FUNCTION()
    Serialize::PPtr<Assets::ParticleSystemAsset> GetSystemReference() const;
    FUNCTION()
    void SetSystemReference(Serialize::PPtr<Assets::ParticleSystemAsset> reference);
    bool SetCompiledSystem(std::shared_ptr<const VFX::CompiledSystem> system, std::string& error);
    FUNCTION(Scriptable)
    bool ReloadSystem();
    FUNCTION(Scriptable)
    void Activate();
    FUNCTION(Scriptable)
    void Stop(bool immediate = false);
    FUNCTION(Scriptable)
    void Restart();
    FUNCTION(Scriptable)
    void SetPaused(bool paused);
    FUNCTION(Scriptable)
    float GetAge() const;
    FUNCTION(Scriptable)
    bool SetFloatParameter(const std::string& name, float value);
    FUNCTION(Scriptable)
    bool SetIntParameter(const std::string& name, int32_t value);
    FUNCTION(Scriptable)
    bool SetBoolParameter(const std::string& name, bool value);
    FUNCTION(Scriptable)
    bool SetVectorParameter(const std::string& name, const Maths::Vector4& value);
    bool SetParameter(std::string name, VFX::Value value);
    void SetCollisionWorld(std::shared_ptr<Physics::PhysicsWorld> world);
    // Restores automatic binding to the owning scene's query world.
    void ClearCollisionWorldOverride();
    void PrepareForRendering();
    [[nodiscard]] std::shared_ptr<const VFX::SystemFrame> CaptureFrame() const;
    [[nodiscard]] std::shared_ptr<VFX::SystemRenderProxy> RenderProxy() const;
    [[nodiscard]] const std::string& LastError() const;

private:
    void Publish();
    void ResolveSceneCollisionWorld();
    PROPERTY(DisplayName("System"), Getter(GetSystemReference), Setter(SetSystemReference))
    Serialize::PPtr<Assets::ParticleSystemAsset> system;
    PROPERTY(DisplayName("Play On Awake"))
    bool playOnAwake = true;
    struct State;
    std::unique_ptr<State> m_state;
};
}
