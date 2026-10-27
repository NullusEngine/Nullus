#pragma once
#include "Components/Component.h"
#include "Components/AnimatorComponent.h"
#include <Rendering/Animation/SkinnedMeshRenderProxy.h>
#include "Components/SkinnedMeshRenderer.generated.h"

namespace NLS::Engine::Components
{
CLASS(NLS_ENGINE_API SkinnedMeshRenderer,ComponentMenu("Animation/Skinned Mesh Renderer"),DisallowMultipleComponent,
    RequireComponent("NLS::Engine::Components::MeshFilter"),RequireComponent("NLS::Engine::Components::MeshRenderer")):public Component
{
public:
    GENERATED_BODY()
    SkinnedMeshRenderer();
    ~SkinnedMeshRenderer()override;
    void OnCreate()override;
    void OnDestroy()override;
    FUNCTION()
    Serialize::PPtr<Assets::AnimationAsset> GetMeshReference()const;
    FUNCTION()
    void SetMeshReference(Serialize::PPtr<Assets::AnimationAsset> reference);
    FUNCTION()
    Serialize::PPtr<Component> GetAnimatorReference()const;
    FUNCTION()
    void SetAnimatorReference(Serialize::PPtr<Component> reference);
    bool SetModel(std::shared_ptr<const Animation::Model> model,size_t mesh,std::string& error);
    void PublishPose();
    const std::string& LastError()const{return m_error;}
private:
    PROPERTY(DisplayName("Skinned Mesh"),Getter(GetMeshReference),Setter(SetMeshReference))
    Serialize::PPtr<Assets::AnimationAsset> mesh;
    PROPERTY(DisplayName("Animator"),Getter(GetAnimatorReference),Setter(SetAnimatorReference))
    Serialize::PPtr<Component> animator;
    std::shared_ptr<Assets::AnimationAsset> m_asset;
    std::shared_ptr<const Animation::Model> m_model;
    std::unique_ptr<Render::AnimationRendering::SkinnedMeshRenderProxy> m_proxy;
    uint64_t m_revision=0;
    size_t m_meshIndex=0;
    std::string m_error;
    std::string m_loadedHash;
};
}
