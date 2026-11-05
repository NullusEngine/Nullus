#include "PlayScriptBindings.h"
#include "ScriptEngineApi.h"
#include <Engine/Components/RigidbodyComponent.h>
#include <Engine/Components/PhysicsColliderComponent.h>
#include <Engine/Components/AudioSourceComponent.h>
#include <Engine/Components/CameraComponent.h>
#include <Engine/Components/PlayerInputComponent.h>
#include <Engine/GameObject.h>
#include <array>
#include <cstring>
#include <stdexcept>
#include <cmath>

namespace NLS::Scripting
{
bool ResolvePlayComponent(Engine::GameObject& owner,uint64_t type,bool create,Engine::Components::Component*& result)
{
    result=nullptr;
    if(create&&!ScriptEngineApi::IsAcceptingPlayRequests())return true;
    const auto resolve=[&]<class T>(const char* shortName,const char* managedName,const char* nativeName)
    {
        if(type!=MakeStableScriptId(shortName)&&type!=MakeStableScriptId(managedName)&&type!=MakeStableScriptId(nativeName))return false;
        result=owner.GetComponent<T>();if(!result&&create)result=owner.AddComponent<T>();return true;
    };
    return resolve.operator()<Engine::Components::RigidbodyComponent>("Rigidbody","Nullus.Rigidbody","NLS::Engine::Components::RigidbodyComponent") ||
        resolve.operator()<Engine::Components::PhysicsColliderComponent>("Collider","Nullus.Collider","NLS::Engine::Components::PhysicsColliderComponent") ||
        resolve.operator()<Engine::Components::AudioSourceComponent>("AudioSource","Nullus.AudioSource","NLS::Engine::Components::AudioSourceComponent") ||
        resolve.operator()<Engine::Components::PlayerInputComponent>("PlayerInput","Nullus.PlayerInput","NLS::Engine::Components::PlayerInputComponent") ||
        resolve.operator()<Engine::Components::PlayerInputComponent>("PlayerInputComponent","Nullus.PlayerInputComponent","NLS::Engine::Components::PlayerInputComponent");
}
ScriptAbiResult InvokePlayMember(uint64_t owner,uint64_t member,const ScriptAbiValue* arguments,uint32_t count,ScriptAbiValue* result)
{
    thread_local std::string error;
    thread_local std::array<float,4> vector;
    const auto fail=[&](ScriptStatusCode code,const char* message) { error=message;return ScriptAbiResult{code,error.c_str()}; };
    if(!result || (count && !arguments))return fail(ScriptStatusCode::InvalidArgument,"Invalid Play binding arguments.");
    *result={};
    const auto matches=[&](const char* name){return member==MakeStableScriptId(std::string("Nullus.Play::")+name);};
    const auto number=[&](uint32_t index=0) { if(index>=count || !std::isfinite(arguments[index].floatingValue))throw std::invalid_argument("Play member requires a finite scalar argument.");return float(arguments[index].floatingValue); };
    const auto vec3=[&] {return Maths::Vector3{number(0),number(1),number(2)};};
    const auto scalar=[&](double value){result->kind=uint32_t(ScriptValueKind::Double);result->floatingValue=value;return ScriptAbiResult{};};
    const auto object=[&](const Object* value){result->kind=uint32_t(ScriptValueKind::ObjectReference);result->objectValue=value?NativeObjectHandle::FromInstanceId(value->GetInstanceID()).value:0;return ScriptAbiResult{};};
    const auto vectorResult=[&](Maths::Vector4 value,uint32_t size)
    {vector={value.x,value.y,value.z,value.w};result->kind=uint32_t(size==3?ScriptValueKind::Vector3:ScriptValueKind::Vector4);result->bytes=reinterpret_cast<const uint8_t*>(vector.data());result->byteSize=size*sizeof(float);return ScriptAbiResult{};};
    try
    {
        if(matches("Event.other")){result->objectValue=ScriptEngineApi::GetEventOther().value;return {};}
        if(matches("Event.boolean"))return scalar(ScriptEngineApi::GetEventBoolean()?1:0);
        if(matches("Screen.width"))return scalar(ScriptEngineApi::GetScreenWidth());
        if(matches("Screen.height"))return scalar(ScriptEngineApi::GetScreenHeight());
        auto* native=Object::IDToPointerNoThreadCheck(static_cast<InstanceID>(owner&0xffffffffu));
        if(!native)return fail(ScriptStatusCode::InvalidHandle,"The Play component is no longer alive.");
        if(auto* body=dynamic_cast<Engine::Components::RigidbodyComponent*>(native))
        {
            if(matches("Rigidbody.mass.get"))return scalar(body->GetMass());
            if(matches("Rigidbody.mass.set")){body->SetMass(number());return {};}
            if(matches("Rigidbody.isKinematic.get"))return scalar(body->IsKinematic());
            if(matches("Rigidbody.isKinematic.set")){body->SetKinematic(number()!=0);return {};}
            if(matches("Rigidbody.useGravity.get"))return scalar(body->GetUseGravity());
            if(matches("Rigidbody.useGravity.set")){body->SetUseGravity(number()!=0);return {};}
            if(matches("Rigidbody.velocity.get")){const auto v=body->GetVelocity();return vectorResult({v.x,v.y,v.z,0},3);}
            if(matches("Rigidbody.velocity.set")){body->SetVelocity(vec3());return {};}
            if(matches("Rigidbody.AddForce")){body->AddForce(vec3());return {};}
        }
        if(auto* collider=dynamic_cast<Engine::Components::PhysicsColliderComponent*>(native))
        {
            if(matches("Collider.isTrigger.get"))return scalar(collider->IsTrigger());
            if(matches("Collider.isTrigger.set")){collider->SetTrigger(number()!=0);return {};}
            if(matches("Collider.radius.get"))return scalar(collider->GetRadius());
            if(matches("Collider.radius.set")){collider->SetRadius(number());return {};}
            if(matches("Collider.size.get")){const auto v=collider->GetSize();return vectorResult({v.x,v.y,v.z,0},3);}
            if(matches("Collider.size.set")){collider->SetSize(vec3());return {};}
            if(matches("Collider.shape.get"))return scalar(collider->GetShapeValue());
            if(matches("Collider.shape.set")){collider->SetShapeValue(uint32_t(std::clamp(number(),0.0f,2.0f)));return {};}
        }
        if(auto* audio=dynamic_cast<Engine::Components::AudioSourceComponent*>(native))
        {
            if(matches("AudioSource.Play")){audio->Play();return {};}
            if(matches("AudioSource.Pause")){audio->Pause();return {};}
            if(matches("AudioSource.Stop")){audio->Stop();return {};}
            if(matches("AudioSource.isPlaying.get"))return scalar(audio->IsPlaying());
            if(matches("AudioSource.volume.get"))return scalar(audio->GetVolume());
            if(matches("AudioSource.volume.set")){audio->SetVolume(number());return {};}
            if(matches("AudioSource.loop.get"))return scalar(audio->GetLoop());
            if(matches("AudioSource.loop.set")){audio->SetLoop(number()!=0);return {};}
            if(matches("AudioSource.playOnAwake.get"))return scalar(audio->GetPlayOnAwake());
            if(matches("AudioSource.playOnAwake.set")){audio->SetPlayOnAwake(number()!=0);return {};}
            if(matches("AudioSource.clip.get"))return object(audio->GetClip().Get());
            if(matches("AudioSource.clip.set"))
            {
                if(!count)throw std::invalid_argument("AudioSource.clip requires a reference.");
                auto* clip=dynamic_cast<Engine::Assets::AudioClip*>(Object::IDToPointerNoThreadCheck(static_cast<InstanceID>(arguments[0].objectValue&0xffffffffu)));
                if(arguments[0].objectValue&&!clip)return fail(ScriptStatusCode::InvalidHandle,"AudioClip is no longer alive.");
                audio->SetClip(Engine::Serialize::PPtr<Engine::Assets::AudioClip>(clip));return {};
            }
        }
        if(auto* camera=dynamic_cast<Engine::Components::CameraComponent*>(native))
        {
            if(matches("Camera.targetDisplay.get"))return scalar(camera->GetTargetDisplay());
            if(matches("Camera.targetDisplay.set")){camera->SetTargetDisplay(int(number()));return {};}
            if(matches("Camera.depth.get"))return scalar(camera->GetDepth());
            if(matches("Camera.depth.set")){camera->SetDepth(number());return {};}
            if(matches("Camera.rect.get"))return vectorResult(camera->GetViewportRect(),4);
            if(matches("Camera.rect.set")){camera->SetViewportRect({number(0),number(1),number(2),number(3)});return {};}
            if(matches("Camera.clearFlags.get"))return scalar(static_cast<int>(camera->GetClearMode()));
            if(matches("Camera.clearFlags.set")){camera->SetClearMode(static_cast<Engine::Components::CameraComponent::ECameraClearMode>(std::clamp(int(number()),0,3)));return {};}
        }
        return fail(ScriptStatusCode::Unsupported,"The requested Play member is not registered for this component.");
    }
    catch(const std::exception& exception){return fail(ScriptStatusCode::Exception,exception.what());}
    catch(...){return fail(ScriptStatusCode::Exception,"Native Play binding failed.");}
}
}
