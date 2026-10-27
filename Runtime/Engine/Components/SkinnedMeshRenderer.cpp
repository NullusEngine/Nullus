#include "SkinnedMeshRenderer.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "GameObject.h"
#include <Core/ServiceLocator.h>
#include <Rendering/Context/Driver.h>
#include <Rendering/Context/DriverAccess.h>
#include <Core/ResourceManagement/MaterialManager.h>
#include "Serialize/ObjectReferenceResolver.h"

namespace NLS::Engine::Components
{
SkinnedMeshRenderer::SkinnedMeshRenderer()=default;
SkinnedMeshRenderer::~SkinnedMeshRenderer()=default;
Serialize::PPtr<Assets::AnimationAsset> SkinnedMeshRenderer::GetMeshReference()const{return mesh;}
void SkinnedMeshRenderer::SetMeshReference(Serialize::PPtr<Assets::AnimationAsset> value){mesh=value;m_asset.reset();m_proxy.reset();m_revision=0;PublishPose();}
Serialize::PPtr<Component> SkinnedMeshRenderer::GetAnimatorReference()const{return animator;}
void SkinnedMeshRenderer::SetAnimatorReference(Serialize::PPtr<Component> value)
{
    if(value.Get()&&!dynamic_cast<AnimatorComponent*>(value.Get())){m_error="Animator reference must identify an Animator component";return;}
    animator=value;m_revision=0;PublishPose();
}
bool SkinnedMeshRenderer::SetModel(std::shared_ptr<const Animation::Model> model,size_t index,std::string& error)
{
    if(!model||index>=model->meshes.size()){error="Skinned mesh index is invalid";return false;}
    m_model=std::move(model);m_meshIndex=index;m_proxy.reset();m_revision=0;PublishPose();error=m_error;return error.empty();
}
void SkinnedMeshRenderer::OnCreate(){PublishPose();}
void SkinnedMeshRenderer::OnDestroy(){m_proxy.reset();m_model.reset();m_asset.reset();}
void SkinnedMeshRenderer::PublishPose()
{
    auto* owner=gameobject();if(!owner)return;
    if(mesh.GetInstanceID()!=NLS::InstanceID_None)
    {
        Serialize::ObjectIdentifier id;if(!Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(mesh.GetInstanceID(),id))return;
        const auto* entry=Serialize::ResolveRuntimeAssetReferenceEntry(id);
        if(!m_asset||(entry&&entry->contentHash!=m_loadedHash))
        {auto asset=Assets::LoadAnimationAsset(id,m_error);if(!asset||!asset->Model())return;m_loadedHash=asset->ContentHash();m_asset=std::move(asset);m_model=m_asset->Model();m_meshIndex=0;m_proxy.reset();m_revision=0;}
    }
    if(!m_model||m_meshIndex>=m_model->meshes.size())return;
    // A cooked Player starts with stable material references and an empty
    // resource table. Request those dependencies before visibility can build a
    // draw command; an unresolved material cannot request itself through a draw.
    if(auto* renderer=owner->GetComponent<MeshRenderer>();renderer&&Core::ServiceLocator::Contains<Core::ResourceManagement::MaterialManager>())
    {
        auto& manager=NLS_SERVICE(Core::ResourceManagement::MaterialManager);const auto paths=renderer->GetMaterialPaths();
        for(size_t i=0;i<paths.size();++i)if(!paths[i].empty()&&!renderer->ResolveMaterialAtIndex(uint32_t(i)))
            if(auto request=manager.TryRequestAsyncArtifact(paths[i]);request&&request->resource&&request->resource->IsValid())
                renderer->SetResolvedMaterialFromReference(uint32_t(i),*request->resource);
    }
    auto* controller=dynamic_cast<AnimatorComponent*>(animator.Get());if(!controller)
        for(auto* parent=owner;parent&&!controller;parent=parent->GetParent())controller=parent->GetComponent<AnimatorComponent>();
    if(!controller)return;auto frame=controller->CaptureFrame();if(!frame||frame->revision==m_revision)return;
    if(!Core::ServiceLocator::Contains<Render::Context::Driver>())return;
    const auto device=Render::Context::DriverRendererAccess::GetExplicitDevice(Core::ServiceLocator::Get<Render::Context::Driver>());if(!device)return;
    if(!m_proxy){auto proxy=std::make_unique<Render::AnimationRendering::SkinnedMeshRenderProxy>();if(!proxy->Initialize(device,m_model,m_meshIndex,m_error))return;m_proxy=std::move(proxy);}
    auto rendered=m_proxy->Publish(frame,m_error);if(!rendered)return;
    auto* filter=owner->GetComponent<MeshFilter>();if(!filter)filter=owner->AddComponent<MeshFilter>();
    filter->SetResolvedTransientMeshFromReference(std::move(rendered));
    if(auto* renderer=owner->GetComponent<MeshRenderer>())renderer->SetAffectDistanceField(false);
    m_revision=frame->revision;m_error.clear();
}
}
