#include "ParticleSystemComponent.h"

#include <cmath>
#include <Debug/Logger.h>
#include <Core/ServiceLocator.h>
#include <Rendering/Context/Driver.h>
#include <Rendering/Context/DriverAccess.h>
#include <Rendering/Context/RenderCommandPipe.h>
#include "GameObject.h"
#include "SceneSystem/Scene.h"
#include "Serialize/ObjectReferenceResolver.h"

namespace NLS::Engine::Components
{
struct ParticleSystemComponent::State
{
    std::unique_ptr<VFX::SystemInstance> instance;
    std::shared_ptr<VFX::SystemRenderProxy> proxy;
    std::shared_ptr<Assets::ParticleSystemAsset> asset;
    std::shared_ptr<Physics::PhysicsWorld> physics;
    std::shared_ptr<const VFX::SystemFrame> scheduled;
    std::shared_ptr<const VFX::SystemFrame> published;
    std::string error, failedHash;
    bool loadRequested = false;
    bool physicsExplicit = false;
};
ParticleSystemComponent::ParticleSystemComponent() : m_state(std::make_unique<State>()) {}
ParticleSystemComponent::~ParticleSystemComponent() = default;

Serialize::PPtr<Assets::ParticleSystemAsset> ParticleSystemComponent::GetSystemReference() const { return system; }
void ParticleSystemComponent::SetSystemReference(Serialize::PPtr<Assets::ParticleSystemAsset> reference)
{
    system = reference; m_state->loadRequested = true; m_state->failedHash.clear();
    if (system.GetInstanceID() == NLS::InstanceID_None)
    { m_state->instance.reset(); m_state->proxy.reset(); m_state->asset.reset(); m_state->scheduled.reset(); m_state->published.reset(); m_state->physics.reset(); }
    MarkRenderStateChanged();
}

bool ParticleSystemComponent::SetCompiledSystem(std::shared_ptr<const VFX::CompiledSystem> program, std::string& error)
{
    auto instance = std::make_unique<VFX::SystemInstance>(); instance->SetCollisionWorld(m_state->physics);
    if (!instance->Initialize(program,error)) return false;
    if (m_state->instance)
    {
        const auto previous = m_state->instance->Capture();
        if (!instance->SetParameters(previous->parameters))
        { error = "Previous particle parameters are incompatible with the new Simulation Stage controls"; return false; }
        if (previous->paused) instance->SetPaused(true);
        if (!previous->spawning) instance->Deactivate();
    }
    auto proxy = std::make_shared<VFX::SystemRenderProxy>(program);
    m_state->instance = std::move(instance); m_state->proxy = std::move(proxy); m_state->scheduled.reset();
    m_state->error.clear(); m_state->loadRequested = false; ResolveSceneCollisionWorld(); Publish(); return true;
}

bool ParticleSystemComponent::ReloadSystem()
{
    Serialize::ObjectIdentifier identifier;
    if (!Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(system.GetInstanceID(),identifier))
    { m_state->error = "Particle system reference is empty or invalid"; return false; }
    auto asset = Assets::LoadParticleSystemAsset(identifier,m_state->error);
    if (!asset) return false;
    if (m_state->asset == asset && m_state->instance && m_state->instance->Capture()->system == asset->Program()) return true;
    if (!SetCompiledSystem(asset->Program(),m_state->error)) return false;
    m_state->asset = std::move(asset); return true;
}

void ParticleSystemComponent::PrepareForRendering()
{
    ResolveSceneCollisionWorld();
    if (system.GetInstanceID() != NLS::InstanceID_None)
    {
        Serialize::ObjectIdentifier identifier;
        if (Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(system.GetInstanceID(),identifier))
        {
            const auto* entry = Serialize::ResolveRuntimeAssetReferenceEntry(identifier);
            const auto hash = entry ? entry->contentHash : std::string{};
            if (m_state->loadRequested || (entry && (!m_state->asset || m_state->asset->ContentHash() != hash) && m_state->failedHash != hash))
            {
                if (!ReloadSystem()) m_state->failedHash = hash;
                m_state->loadRequested = false;
            }
        }
    }
    Publish();
}

void ParticleSystemComponent::Publish()
{
    if (!m_state->instance || !m_state->proxy) return;
    const auto frame = m_state->instance->Capture();
    if (frame != m_state->published) { m_state->published = frame; MarkRenderStateChanged(); }
    if (!Core::ServiceLocator::Contains<Render::Context::Driver>()) return;
    const auto device = Render::Context::DriverRendererAccess::GetExplicitDevice(Core::ServiceLocator::Get<Render::Context::Driver>());
    if (!device || frame == m_state->scheduled) return;
    m_state->scheduled = frame;
    Render::Context::EnqueueRenderCommand<struct ParticleWorldFrame>([device,frame,proxy = m_state->proxy]
    {
        std::string error;
        if (!proxy->Prepare(device,frame,error)) NLS_LOG_ERROR("Particle world update: "+error);
    });
}

void ParticleSystemComponent::OnCreate() { PrepareForRendering(); }
void ParticleSystemComponent::OnStart() { PrepareForRendering(); if (playOnAwake) Restart(); else Stop(true); }
void ParticleSystemComponent::OnUpdate(float deltaTime)
{
    PrepareForRendering();
    if (m_state->instance && m_state->instance->Tick(deltaTime)) Publish();
}
void ParticleSystemComponent::OnDestroy()
{
    m_state->instance.reset(); m_state->proxy.reset(); m_state->asset.reset();
    m_state->scheduled.reset(); m_state->published.reset(); m_state->physics.reset();
}
void ParticleSystemComponent::Activate() { if (m_state->instance) { m_state->instance->Activate(); Publish(); } }
void ParticleSystemComponent::Stop(bool immediate) { if (m_state->instance) { m_state->instance->Deactivate(immediate); Publish(); } }
void ParticleSystemComponent::Restart() { if (m_state->instance) { m_state->instance->Reset(); Publish(); } }
void ParticleSystemComponent::SetPaused(bool paused) { if (m_state->instance) { m_state->instance->SetPaused(paused); Publish(); } }
float ParticleSystemComponent::GetAge() const { return m_state->instance ? static_cast<float>(m_state->instance->Age()) : 0; }
bool ParticleSystemComponent::SetFloatParameter(const std::string& name, float value) { if (!m_state->instance || !m_state->instance->SetTypedParameter(name,VFX::ValueType::Float,{value,0,0,0})) return false; Publish(); return true; }
bool ParticleSystemComponent::SetIntParameter(const std::string& name, int32_t value) { if (!m_state->instance || !m_state->instance->SetTypedParameter(name,VFX::ValueType::Int32,VFX::IntValue(value))) return false; Publish(); return true; }
bool ParticleSystemComponent::SetBoolParameter(const std::string& name, bool value) { if (!m_state->instance || !m_state->instance->SetTypedParameter(name,VFX::ValueType::Bool,VFX::BoolValue(value))) return false; Publish(); return true; }
bool ParticleSystemComponent::SetVectorParameter(const std::string& name, const Maths::Vector4& value) { if (!m_state->instance || !m_state->instance->SetTypedParameter(name,VFX::ValueType::Float4,{value.x,value.y,value.z,value.w})) return false; Publish(); return true; }
bool ParticleSystemComponent::SetParameter(std::string name, VFX::Value value)
{
    if (!m_state->instance || !m_state->instance->SetParameter(std::move(name),value)) return false;
    Publish(); return true;
}
void ParticleSystemComponent::SetCollisionWorld(std::shared_ptr<Physics::PhysicsWorld> world)
{
    m_state->physicsExplicit = true;
    m_state->physics = std::move(world);
    if (m_state->instance) m_state->instance->SetCollisionWorld(m_state->physics);
}
void ParticleSystemComponent::ClearCollisionWorldOverride()
{
    m_state->physicsExplicit = false;
    ResolveSceneCollisionWorld();
}
// Collision geometry is only resolved when an emitter actually queries it, so
// scenes without particle collision never pay for a PhysX world.
void ParticleSystemComponent::ResolveSceneCollisionWorld()
{
    if (m_state->physicsExplicit || !m_state->instance || !m_state->instance->RequiresCollisionWorld()) return;
    auto* owner = gameobject();
    auto* scene = owner != nullptr ? owner->GetScene() : nullptr;
    if (scene == nullptr) return;
    const auto& world = scene->GetPhysicsWorld();
    if (world == nullptr || world == m_state->physics) return;
    m_state->physics = world;
    m_state->instance->SetCollisionWorld(world);
}
std::shared_ptr<const VFX::SystemFrame> ParticleSystemComponent::CaptureFrame() const { return m_state->instance ? m_state->instance->Capture() : nullptr; }
std::shared_ptr<VFX::SystemRenderProxy> ParticleSystemComponent::RenderProxy() const { return m_state->proxy; }
const std::string& ParticleSystemComponent::LastError() const { return m_state->error; }
}
