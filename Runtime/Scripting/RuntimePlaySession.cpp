#include <Profiling/Profiler.h>
#include "RuntimePlaySession.h"
#include "Core/Modules/ModuleServices.h"
#include "ScriptEngineApi.h"
#include "InputScriptApi.h"
#include "ScriptComponent.h"
#include <Engine/Components/UICanvasComponent.h>
#include <Engine/UI/UIDocumentSourceResolver.h>
#include <filesystem>
#include <Debug/Logger.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <Core/ServiceLocator.h>
#include <Windowing/Inputs/PlatformInputBackend.h>

namespace NLS::Scripting
{
namespace
{
ECS::SystemTime ECSTime(const ScriptFrameContext& frame)
{
    ECS::SystemTime time;
    time.deltaTime = frame.deltaTime; time.unscaledDeltaTime = frame.unscaledDeltaTime;
    time.time = frame.time; time.unscaledTime = frame.unscaledTime;
    time.fixedDeltaTime = frame.fixedDeltaTime; time.timeScale = frame.timeScale;
    time.frameIndex = frame.frameIndex; time.fixedFrameIndex = frame.fixedFrameIndex;
    return time;
}
}
void RuntimePlaySession::EnableECS(ECSSetup setup, ECS::WorldConfig config)
{
    if (IsActive()) throw std::logic_error("Configure ECS before beginning a Play session");
    m_ecsEnabled = true; m_ecsConfig = std::move(config); m_ecsSetup = std::move(setup);
}
void RuntimePlaySession::DisableECS()
{
    if (IsActive()) throw std::logic_error("Configure ECS before beginning a Play session");
    m_ecsEnabled = false; m_ecsSetup = {};
}
void RuntimePlaySession::RunECSPhase(ECS::SystemPhase phase, const ScriptFrameContext& frame)
{ if (m_ecs) m_ecs->RunPhase(phase, ECSTime(frame)); }
RuntimePlaySession::~RuntimePlaySession()
{
    if (m_inputDomain && Core::ServiceLocator::Contains<Input::InputDomain>() && &Core::ServiceLocator::Get<Input::InputDomain>() == m_inputDomain.get())
        Core::ServiceLocator::Remove<Input::InputDomain>();
}
void RuntimePlaySession::ConfigureInput(Input::InputSystem& system, Windowing::Inputs::PlatformInputBackend* textBackend)
{
    if (IsActive()) throw std::logic_error("Configure Play input before Begin");
    m_inputSystem = &system;
    m_textBackend = textBackend;
    m_textBackendLifetime = textBackend ? textBackend->GetLifetime() : std::weak_ptr<const void>{};
}
void RuntimePlaySession::PumpInput()
{
    InputScriptApi::UpdateRebindings();
    if (!m_inputDomain || m_inputFrame == m_inputSystem->GetFrameIndex()) return;
    m_inputFrame = m_inputSystem->GetFrameIndex();
    // Input collection is independent of simulation scheduling. Commands wait
    // for Tick/Step; Fixed edges have their own once-only consumer.
    m_inputDomain->Update(m_inputSystem->Now(), false);
}
bool RuntimePlaySession::ConfigurePhysicsTiming(const Physics::FixedStepSettings& settings, std::string& error)
{
    if (!std::isfinite(static_cast<float>(settings.fixedDeltaTime)) || static_cast<float>(settings.fixedDeltaTime) <= 0 ||
        !std::isfinite(static_cast<float>(settings.maximumAccumulatedTime)))
    {
        error = "Play physics timing must be representable by the scripting runtime's float time values.";
        return false;
    }
    if (!m_fixedClock.Configure(settings, error)) return false;
    m_physicsTimingConfigured = true;
    if (IsActive()) ScriptEngineApi::SetFrameTiming(static_cast<float>(settings.fixedDeltaTime),
        static_cast<float>(settings.maximumAccumulatedTime));
    return true;
}
void RuntimePlaySession::Step()
{
    if (!IsActive()) return;
    const bool paused = m_paused;
    if (!m_stepInputActive)
    {
        m_stepInputActive = true;
        if (m_inputDomain) m_inputDomain->SetSuspended(!m_focused);
    }
    m_paused = false;
    try { TickFrame(ScriptEngineApi::GetFixedDeltaTime(), true); }
    catch (...) { m_paused = paused; throw; }
    m_paused = paused;
}
ScriptStatus RuntimePlaySession::Begin(ScriptRuntime& scripts, Engine::SceneSystem::SceneManager& scenes, uint64_t sessionId,
    std::shared_ptr<Engine::Audio::AudioDevice> audio)
{
    if (IsActive()) return ScriptStatus::Error(ScriptStatusCode::AlreadyInitialized, "A Play session is already active.");
    if (!scenes.GetCurrentScene()) return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "Play requires a scene.");
    if (scenes.GetCurrentScene()->IsSerializationReadOnly())
        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument,"Play is blocked while plugin component types are unresolved.");
    if (Core::ServiceLocator::Contains<Core::Modules::RuntimeExtensionService>() &&
        !Core::ServiceLocator::Get<Core::Modules::RuntimeExtensionService>().Prepare())
        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "PrePlay plugin startup failed.");
    std::unique_ptr<Engine::ECSIntegration::RuntimeWorld> ecs;
    if (m_ecsEnabled)
    {
        try
        {
            ecs = std::make_unique<Engine::ECSIntegration::RuntimeWorld>(m_ecsConfig);
            ecs->AttachScene(*scenes.GetCurrentScene());
            if (m_ecsSetup) m_ecsSetup(*ecs, *scenes.GetCurrentScene());
        }
        catch (const std::exception& exception)
        { return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, std::string("ECS setup failed: ") + exception.what()); }
    }
    auto status = scripts.BeginPlaySession(sessionId);
    if (!status.Succeeded()) return status;
    for(auto* owner:scenes.GetCurrentScene()->GetGameObjects())
        if(owner)for(const auto& component:owner->GetComponents())
            if(auto* script=dynamic_cast<ScriptComponent*>(component.get()))
            {
                script->SetRuntime(&scripts);
                const auto& asset=script->GetScriptAsset();
                if(asset.language==ScriptLanguage::Unknown)continue;
                status=scripts.LoadScript(asset);
                if(!status.Succeeded()) { scripts.EndPlaySession(); return status; }
            }
    m_scripts = &scripts;
    m_ecs = std::move(ecs); m_ecsStarted = false;
    m_scenes = &scenes;
    m_sessionId = sessionId;
    m_frame = {};
    m_frame.timeScale = 1;
    m_fixedClock.Reset();
    m_physicsInterpolationAlpha = m_fixedTime = m_frameClippedPhysicsTime = 0;
    m_lastTickMilliseconds=0;
    m_ui = {};
    m_paused=false;
    m_focused=false;
    m_endStarted=false;
    m_stepInputActive = false;
    m_hasUiPointer = false;
    m_compositionSequence = 0; m_composing = false;
    m_inputFrame = UINT64_MAX;
    if (m_inputSystem)
    {
        m_inputDomain = std::make_unique<Input::InputDomain>(*m_inputSystem, "Runtime Play " + std::to_string(sessionId));
        m_inputUser = m_inputDomain->CreateUser();
        m_inputDomain->SetSuspended(true, Input::CancelReason::FocusLost);
        Core::ServiceLocator::Provide<Input::InputDomain>(*m_inputDomain);
    }
    m_audio=std::move(audio);
    if(m_audio)m_audio->BeginSession(sessionId);
    ScriptEngineApi::ResetPlayState(sessionId);
    if (m_physicsTimingConfigured)
    {
        const auto& timing = m_fixedClock.GetSettings();
        ScriptEngineApi::SetFrameTiming(static_cast<float>(timing.fixedDeltaTime),
            static_cast<float>(timing.maximumAccumulatedTime));
    }
    ScriptEngineApi::SetSceneManager(&scenes);
    ScriptEngineApi::SetScene(scenes.GetCurrentScene());
    m_frame.fixedDeltaTime = ScriptEngineApi::GetFixedDeltaTime();
    if (Core::ServiceLocator::Contains<Core::Modules::RuntimeExtensionService>())
        Core::ServiceLocator::Get<Core::Modules::RuntimeExtensionService>().Begin();
    return ScriptStatus::Ok();
}
void RuntimePlaySession::RunPhase(ScriptCallback callback, const ScriptFrameContext& frame, const std::function<void()>& body)
{
    NLS_PROFILE_NAMED_SCOPE("Play::ScriptPhase");
    ScriptEngineApi::SetFrameContext(frame);
    const auto status = m_scripts->BeginScheduledFrame(callback, frame);
    body();
    if (status.Succeeded())
    {
        const auto flushed = m_scripts->FlushScheduledFrame();
        if (!flushed.Succeeded()) NLS_LOG_WARNING("Play script phase failed: " + flushed.message);
    }
}
void RuntimePlaySession::Tick(float unscaledDeltaTime)
{
    TickFrame(unscaledDeltaTime, false);
}
void RuntimePlaySession::TickFrame(float unscaledDeltaTime, bool singleStep)
{
    NLS_PROFILE_NAMED_SCOPE("Play::TickFrame");
    if (!m_scripts || !m_scenes || m_paused) return;
    auto* scene = m_scenes->GetCurrentScene();
    if (!scene || !scene->IsPlaying()) return;
    const auto sceneId = scene->GetInstanceID();
    const auto canContinueScene=[&]
    {
        auto* current=m_scenes ? m_scenes->GetCurrentScene() : nullptr;
        if (!m_scripts || current!=scene || current->GetInstanceID()!=sceneId) return false;
        if (current->IsPhysicsStopPending()) current->SyncTransforms();
        return current->IsPlaying();
    };
    if (!canContinueScene()) return;
    const auto tickStarted=std::chrono::steady_clock::now();
    if (m_ecs)
    {
        m_ecs->AttachScene(*scene);
        if (!m_ecsStarted)
        {
            auto startup = m_frame;
            startup.deltaTime = startup.unscaledDeltaTime = 0;
            startup.fixedDeltaTime = ScriptEngineApi::GetFixedDeltaTime();
            startup.timeScale = ScriptEngineApi::GetTimeScale();
            RunECSPhase(ECS::SystemPhase::Initialization, startup);
            m_ecsStarted = true;
        }
    }
    const float unscaled = std::isfinite(unscaledDeltaTime) ? std::max(0.0f, unscaledDeltaTime) : 0.0f;
    ScriptEngineApi::SetScene(scene);
    const float delta = singleStep ? ScriptEngineApi::GetFixedDeltaTime() : ScriptEngineApi::ScaleDeltaTime(unscaled);
    m_frame.deltaTime = delta;
    m_frame.unscaledDeltaTime = unscaled;
    m_frame.time += delta;
    m_frame.unscaledTime += unscaled;
    m_frame.timeScale = ScriptEngineApi::GetTimeScale();
    m_frame.fixedDeltaTime = ScriptEngineApi::GetFixedDeltaTime();
    ++m_frame.frameIndex;
    ScriptEngineApi::SetFrameContext(m_frame);
    if (Core::ServiceLocator::Contains<Core::Modules::RuntimeExtensionService>())
        Core::ServiceLocator::Get<Core::Modules::RuntimeExtensionService>().Tick(delta);
#if defined(NLS_ENABLE_GAME_UI)
    m_ui.SyncCanvases(scene->GetFastAccessComponents().uiCanvases);
    for(auto* canvas : m_ui.GetCanvases())
        if(canvas && !canvas->GetDocument() && !canvas->GetDocumentPath().empty())
        {
            const auto path=Engine::UI::ResolveProjectUIDocumentPath(m_assetsRoot,canvas->GetDocumentPath());
            std::error_code error;
            if(!path.empty()&&std::filesystem::is_regular_file(path,error)&&!error) canvas->LoadDocumentResolvedPath(path,nullptr);
        }
    const Engine::UI::UIVector2 viewport{float(m_uiWidth),float(m_uiHeight)};
    m_ui.Tick(0,viewport,m_uiDpi,m_frame.frameIndex);
    bool uiKeyboardConsumed = false, uiPointerConsumed = false;
    if(const auto* input=ScriptEngineApi::GetGameInputFrame();input&&input->focused)
    {
        Engine::UI::InputEventQueue events;
        auto push=[&](Engine::UI::UIEventType type,Engine::UI::UIInputDevice device,uint32_t key=0,float scroll=0)
        {
            Engine::UI::UIInputEvent event{};event.type=type;event.device=device;
            event.position={input->position.x,input->position.y};event.keyCode=key;event.wheel=scroll;
            event.delta={input->delta.x,input->delta.y};event.timestamp=m_frame.frameIndex;
            events.Push(event);
        };
        if (input->hasOrderedEvents && m_inputSystem)
        {
            auto pointerPosition = m_hasUiPointer ? m_uiPointer : Engine::UI::UIVector2{input->position.x, input->position.y};
            const bool current = input->sourceInputFrame == m_inputSystem->GetFrameIndex();
            for (const auto& raw : current ? input->orderedEvents : std::span<const Input::InputEvent>{})
            {
                const auto* device = m_inputSystem->FindDevice(raw.device);
                if (!device || device->descriptor.kind == Input::DeviceKind::Gamepad) continue;
                const bool pointer = device->descriptor.kind == Input::DeviceKind::Mouse;
                if (pointer && !input->pointerAvailable) continue;
                Engine::UI::UIInputEvent event{};
                event.device = pointer ? Engine::UI::UIInputDevice::Mouse : Engine::UI::UIInputDevice::Keyboard;
                event.position = pointerPosition; event.keyCode = raw.control; event.timestamp = raw.sequence; event.modifiers = raw.modifiers;
                if ((static_cast<uint8_t>(raw.flags) & static_cast<uint8_t>(Input::InputEventFlags::FocusReset)) != 0) continue;
                if (raw.type == Input::InputEventType::ButtonDown || raw.type == Input::InputEventType::ButtonRepeat)
                    event.type = pointer ? Engine::UI::UIEventType::PointerDown : Engine::UI::UIEventType::KeyDown;
                else if (raw.type == Input::InputEventType::ButtonUp) event.type = pointer ? Engine::UI::UIEventType::PointerUp : Engine::UI::UIEventType::KeyUp;
                else if (raw.type == Input::InputEventType::PointerMove)
                {
                    event.type = Engine::UI::UIEventType::PointerMove;
                    const bool screen = (static_cast<uint8_t>(raw.flags) & static_cast<uint8_t>(Input::InputEventFlags::ScreenPosition)) != 0;
                    event.position = {float((screen ? raw.screenX - input->pointerOrigin.x : raw.x) * input->pointerScale.x),
                        float((screen ? raw.screenY - input->pointerOrigin.y : raw.y) * input->pointerScale.y)};
                    pointerPosition = event.position;
                }
                else if (raw.type == Input::InputEventType::Wheel) { event.type = Engine::UI::UIEventType::Scroll; event.wheel = float(raw.y); }
                else continue;
                const bool consumed = m_ui.DispatchInput(event);
                (pointer ? uiPointerConsumed : uiKeyboardConsumed) |= consumed;
            }
            m_uiPointer = pointerPosition; m_hasUiPointer = true;
            if (!input->pointerAvailable) m_ui.CancelInput(false);
        }
        else if(input->pointerAvailable)
        {
            push(Engine::UI::UIEventType::PointerMove,Engine::UI::UIInputDevice::Mouse);
            if(input->mousePressed[0])push(Engine::UI::UIEventType::PointerDown,Engine::UI::UIInputDevice::Mouse);
            if(input->mouseReleased[0])push(Engine::UI::UIEventType::PointerUp,Engine::UI::UIInputDevice::Mouse);
        }
        else m_ui.CancelInput(false);
        if (!input->hasOrderedEvents || !m_inputSystem) for(uint32_t key=0;key<input->keys.size();++key)
        {
            if(input->pressed[key])push(Engine::UI::UIEventType::KeyDown,Engine::UI::UIInputDevice::Keyboard,key);
            if(input->released[key])push(Engine::UI::UIEventType::KeyUp,Engine::UI::UIInputDevice::Keyboard,key);
        }
        if((!input->hasOrderedEvents || !m_inputSystem) && input->scroll.y!=0)push(Engine::UI::UIEventType::Scroll,Engine::UI::UIInputDevice::Mouse,0,input->scroll.y);
        if (input->compositionSequence && input->compositionSequence != m_compositionSequence)
        {
            Engine::UI::UIInputEvent composition; composition.device = Engine::UI::UIInputDevice::Ime; composition.timestamp = input->compositionSequence;
            if (input->composing && !m_composing) { composition.type = Engine::UI::UIEventType::ImeCompositionStart; m_ui.DispatchInput(composition); }
            composition.type = Engine::UI::UIEventType::ImeCompositionUpdate; composition.text = input->composition;
            m_ui.DispatchInput(composition); m_compositionSequence = input->compositionSequence; m_composing = input->composing;
        }
        if (m_textBackend && !m_textBackendLifetime.expired() && input->pointerScale.x > 0 && input->pointerScale.y > 0)
            if (const auto rect = m_ui.GetTextInputRect())
            {
                const auto window = m_inputSystem->GetPointerWindow();
                m_textBackend->SetTextInputRect(window ? window : m_textBackend->GetMainWindowId(),
                    {input->pointerOrigin.x + rect->x / input->pointerScale.x, input->pointerOrigin.y + (rect->y + rect->height) / input->pointerScale.y},
                    rect->height / input->pointerScale.y);
            }
        if(!input->text.empty() && (!input->hasOrderedEvents || !m_inputSystem || input->sourceInputFrame == m_inputSystem->GetFrameIndex()))
        {Engine::UI::UIInputEvent text;text.type=Engine::UI::UIEventType::TextInput;text.device=Engine::UI::UIInputDevice::Keyboard;text.text=input->text;events.Push(std::move(text));}
        m_ui.DispatchInputQueue(events);
    }
    if (m_inputDomain)
        for (const auto& user : m_inputDomain->GetUsers())
        {
            user->SetDeviceBlocked(Input::DeviceKind::Keyboard, uiKeyboardConsumed || m_ui.WantsKeyboardInput());
            const auto* input = ScriptEngineApi::GetGameInputFrame();
            user->SetDeviceBlocked(Input::DeviceKind::Mouse, uiPointerConsumed || m_ui.WantsPointerInput() || (input && !input->pointerAvailable));
        }
#endif
    PumpInput();
    if (m_inputDomain) m_inputDomain->DispatchDynamicEvents();
    ScriptEngineApi::AdvanceFrame(delta);
    auto timing = m_fixedClock.GetSettings();
    timing.fixedDeltaTime = m_frame.fixedDeltaTime;
    timing.maximumAccumulatedTime = (std::max)(timing.maximumAccumulatedTime, timing.fixedDeltaTime);
    std::string timingError;
    if (!m_fixedClock.Configure(timing, timingError)) throw std::logic_error(timingError);
    Physics::FixedStepBatch batch;
    if (singleStep)
    {
        batch.stepCount = 1;
        batch.fixedDeltaTime = m_frame.fixedDeltaTime;
        batch.interpolationAlpha = 1;
    }
    else
    {
        const double rawScaled = static_cast<double>(unscaled) * m_frame.timeScale;
        batch = m_fixedClock.Advance(delta);
        m_frameClippedPhysicsTime += (std::max)(0.0, rawScaled - delta);
    }
    m_physicsInterpolationAlpha = batch.interpolationAlpha;
    for (uint32_t step = 0; step < batch.stepCount && canContinueScene(); ++step)
    {
        auto fixed = m_frame;
        fixed.deltaTime = fixed.fixedDeltaTime;
        fixed.unscaledDeltaTime = singleStep ? fixed.fixedDeltaTime :
            fixed.timeScale > 0 ? fixed.fixedDeltaTime / fixed.timeScale : 0;
        m_fixedTime += fixed.fixedDeltaTime;
        fixed.time = m_fixedTime;
        fixed.fixedFrameIndex = ++m_frame.fixedFrameIndex;
        if (m_inputDomain) m_inputDomain->BeginFixedTick(fixed.fixedFrameIndex, fixed.fixedDeltaTime);
        RunECSPhase(ECS::SystemPhase::PreFixedUpdate, fixed);
        if (!canContinueScene()) break;
        RunPhase(ScriptCallback::FixedUpdate, fixed, [&] { scene->FixedUpdate(fixed.deltaTime); });
        if (!canContinueScene()) break;
        RunECSPhase(ECS::SystemPhase::FixedSimulation, fixed);
        if (!canContinueScene()) break;
        const auto contacts=scene->SimulatePhysics(fixed.deltaTime);
        DispatchContacts(*scene,contacts,fixed);
        if (!canContinueScene()) break;
        RunECSPhase(ECS::SystemPhase::PostPhysics, fixed);
    }
    if (canContinueScene()) RunPhase(ScriptCallback::Update, m_frame, [&] { scene->UpdateGameplay(delta); });
    if (canContinueScene()) RunECSPhase(ECS::SystemPhase::Update, m_frame);
    if (canContinueScene()) scene->UpdateAnimations(delta);
    if (canContinueScene()) RunPhase(ScriptCallback::LateUpdate, m_frame, [&] { scene->LateUpdate(delta); });
    if (canContinueScene()) RunECSPhase(ECS::SystemPhase::LateUpdate, m_frame);
    ScriptEngineApi::FlushDeferredDestructions();
    auto* publishedScene=m_scenes->GetCurrentScene();
    if(publishedScene!=scene)m_ui={};
    if(publishedScene)publishedScene->CollectGarbages();
    if (publishedScene)
    {
        if (singleStep) publishedScene->ResetPhysicsInterpolation();
        publishedScene->SetPhysicsInterpolationAlpha(publishedScene->GetInstanceID() == sceneId ? batch.interpolationAlpha : 1);
    }
    if (m_ecs)
    {
        if (publishedScene) m_ecs->AttachScene(*publishedScene);
        RunECSPhase(ECS::SystemPhase::Presentation, m_frame);
        RunECSPhase(ECS::SystemPhase::Cleanup, m_frame);
    }
#if defined(NLS_ENABLE_GAME_UI)
    m_ui.SyncCanvases(publishedScene?publishedScene->GetFastAccessComponents().uiCanvases:std::vector<Engine::Components::UICanvasComponent*>{});
    m_ui.Tick(delta,{float(m_uiWidth),float(m_uiHeight)},m_uiDpi,m_frame.frameIndex);
#endif
    if(m_audio)m_audio->TickSilent(unscaled);
    m_lastTickMilliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-tickStarted).count();
}
ScriptStatus RuntimePlaySession::End(bool restoreScriptHost)
{
    if (!m_scripts) return ScriptStatus::Ok();
    if (m_scenes && m_scenes->GetCurrentScene() &&
        (m_scenes->GetCurrentScene()->IsPhysicsMutationDeferred() || m_scenes->GetCurrentScene()->IsPhysicsSynchronizing()))
        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument,"Stop the Play session after the physics callback returns.");
    if (!m_endStarted)
    {
        PrepareToStop();
        if (Core::ServiceLocator::Contains<Core::Modules::RuntimeExtensionService>())
            Core::ServiceLocator::Get<Core::Modules::RuntimeExtensionService>().End();
        DispatchEvent(nullptr,ScriptCallback::OnApplicationQuit,m_frame);
        if (m_ecs) { m_ecs->Stop(); m_ecs.reset(); m_ecsStarted = false; }
        // Stop delivers callbacks while the sandbox is still the active world.
        if (m_scenes && m_scenes->GetCurrentScene()) m_scenes->GetCurrentScene()->Stop();
        m_ui = {};
        if(m_audio)m_audio->EndSession(m_sessionId);
        m_audio.reset();
        ScriptEngineApi::ResetPlayState(0);
        m_endStarted=true;
    }
    const auto status=m_scripts->EndPlaySession(restoreScriptHost);
    if (!status.Succeeded()) return status;
    m_scripts = nullptr;
    m_scenes = nullptr;
    m_inputUser.reset();
    if (m_inputDomain && Core::ServiceLocator::Contains<Input::InputDomain>() && &Core::ServiceLocator::Get<Input::InputDomain>() == m_inputDomain.get())
        Core::ServiceLocator::Remove<Input::InputDomain>();
    m_inputDomain.reset();
    m_endStarted=false;
    return ScriptStatus::Ok();
}
void RuntimePlaySession::ConfigureUI(std::string assetsRoot,uint32_t width,uint32_t height,float dpi,uint32_t display)
{
    m_assetsRoot=std::move(assetsRoot);m_uiWidth=std::max(1u,width);m_uiHeight=std::max(1u,height);m_uiDpi=dpi;
    m_ui.SetInputDisplay(display);
    m_ui.ConfigureOutput(display,{float(m_uiWidth),float(m_uiHeight)},dpi);
    if(IsActive())ScriptEngineApi::SetScreenSize(m_uiWidth,m_uiHeight);
}
void RuntimePlaySession::PrepareToStop()
{
    if(!IsActive())return;
    if (m_inputDomain)
    {
        m_inputDomain->Cancel(Input::CancelReason::Stop);
        m_inputDomain->DispatchDynamicEvents();
    }
    ScriptEngineApi::StopAcceptingPlayRequests();
    GameInputFrame neutral;neutral.sessionId=m_sessionId;neutral.width=m_uiWidth;neutral.height=m_uiHeight;
    ScriptEngineApi::SetGameInputFrame(std::move(neutral));
    if(m_scenes&&m_scenes->GetCurrentScene())m_ui.SyncCanvases(m_scenes->GetCurrentScene()->GetFastAccessComponents().uiCanvases);
    m_ui.CancelInput();
    m_paused=true;
    if(m_audio)m_audio->SetPaused(m_sessionId,true);
}

void RuntimePlaySession::DispatchEvent(Engine::GameObject* owner, ScriptCallback callback,
    const ScriptFrameContext& frame, NativeObjectHandle other, bool value)
{
    if(!m_scripts || !m_scenes || !m_scenes->GetCurrentScene())return;
    std::vector<InstanceID> ids;
    const auto collect=[&](Engine::GameObject* object)
    {
        if(object)for(const auto& component:object->GetComponents())
            if(dynamic_cast<ScriptComponent*>(component.get())) ids.push_back(component->GetInstanceID());
    };
    if(owner)collect(owner);else for(auto* object:m_scenes->GetCurrentScene()->GetGameObjects())collect(object);
    const auto previousOther=ScriptEngineApi::GetEventOther();const bool previousValue=ScriptEngineApi::GetEventBoolean();
    ScriptEngineApi::SetPlayEvent(other,value);
    for(const auto id:ids)
        if(auto* script=dynamic_cast<ScriptComponent*>(Object::IDToPointerNoThreadCheck(id));script&&script->IsActiveAndEnabled())
        {
            ScriptInvocationContext context;context.frame=frame;context.owner=NativeObjectHandle::FromInstanceId(id);
            (void)m_scripts->Invoke(script->GetInstance(),callback,context);
        }
    ScriptEngineApi::SetPlayEvent(previousOther,previousValue);
}
void RuntimePlaySession::DispatchContacts(Engine::SceneSystem::Scene& scene,
    const std::vector<Physics::ContactEvent>& contacts,const ScriptFrameContext& frame)
{
    const Engine::SceneSystem::Scene::PhysicsMutationDeferral deferral(scene);
    struct Delivery {InstanceID first,second;ScriptCallback callback;};
    std::vector<Delivery> deliveries;
    deliveries.reserve(contacts.size());
    for(const auto& contact:contacts)
    {
        InstanceID first=static_cast<InstanceID>(contact.firstOwner),second=static_cast<InstanceID>(contact.secondOwner);
        if (first==InstanceID_None || second==InstanceID_None)
        for(auto* collider:scene.GetFastAccessComponents().physicsColliders)
            if(collider)
            {
                if(first==InstanceID_None && (contact.firstShape.IsValid() ? collider->GetShapeHandle()==contact.firstShape : collider->GetBodyHandle()==contact.first))first=collider->GetInstanceID();
                if(second==InstanceID_None && (contact.secondShape.IsValid() ? collider->GetShapeHandle()==contact.secondShape : collider->GetBodyHandle()==contact.second))second=collider->GetInstanceID();
            }
        const auto callback=static_cast<ScriptCallback>(static_cast<int>(contact.trigger?ScriptCallback::OnTriggerEnter:ScriptCallback::OnCollisionEnter)+static_cast<int>(contact.phase));
        deliveries.push_back({first,second,callback});
    }
    // Resolve stable ownership before user code; structural changes remain
    // deferred until the next safe synchronization point.
    for(const auto& [first,second,callback]:deliveries)
    {
        const auto deliver=[&](InstanceID self,InstanceID other)
        {
            auto* collider=dynamic_cast<Engine::Components::PhysicsColliderComponent*>(Object::IDToPointerNoThreadCheck(self));
            auto* otherCollider=dynamic_cast<Engine::Components::PhysicsColliderComponent*>(Object::IDToPointerNoThreadCheck(other));
            if(collider && otherCollider && collider->gameobject() && otherCollider->gameobject() &&
                collider->gameobject()->GetScene()==&scene && otherCollider->gameobject()->GetScene()==&scene &&
                collider->gameobject()->IsAlive() && otherCollider->gameobject()->IsAlive())
                DispatchEvent(collider->gameobject(),callback,frame,NativeObjectHandle::FromInstanceId(other));
        };
        deliver(first,second);deliver(second,first);
    }
}
void RuntimePlaySession::SetPaused(bool paused)
{
    if(m_paused==paused)return;
    if (m_stepInputActive && m_inputDomain) m_inputDomain->Cancel(Input::CancelReason::Pause);
    m_stepInputActive = false;
    if (m_inputDomain) m_inputDomain->SetSuspended(paused || !m_focused, Input::CancelReason::Pause);
    m_paused=paused;
    if (paused && m_scenes)
        if (auto* scene = m_scenes->GetCurrentScene())
        {
            scene->ResetPhysicsInterpolation();
            m_physicsInterpolationAlpha = 1;
        }
    if(paused && m_scenes && m_scenes->GetCurrentScene())
    { m_ui.SyncCanvases(m_scenes->GetCurrentScene()->GetFastAccessComponents().uiCanvases);m_ui.CancelInput(); }
    if(m_audio)m_audio->SetPaused(m_sessionId,paused);
    DispatchEvent(nullptr,ScriptCallback::OnApplicationPause,m_frame,{},paused);
}
void RuntimePlaySession::NotifyFocus(bool focused)
{
    if (m_inputDomain) m_inputDomain->SetSuspended(!focused || (m_paused && !m_stepInputActive), Input::CancelReason::FocusLost);
    const auto* frame = ScriptEngineApi::GetGameInputFrame();
    if (!frame || !frame->hasOrderedEvents || m_paused) PumpInput();
    if(m_focused==focused)return;
    m_focused=focused;
    if(!focused && m_scenes && m_scenes->GetCurrentScene())
    { m_ui.SyncCanvases(m_scenes->GetCurrentScene()->GetFastAccessComponents().uiCanvases);m_ui.CancelInput(); }
    DispatchEvent(nullptr,ScriptCallback::OnApplicationFocus,m_frame,{},focused);
}
}
