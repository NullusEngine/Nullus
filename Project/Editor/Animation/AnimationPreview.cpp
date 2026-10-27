#include "AnimationPreview.h"
#include "Core/EditorActions.h"
#include "Rendering/GridRenderPass.h"
#include <Rendering/SceneRendererFactory.h>
#include <Components/LightComponent.h>
#include <Components/MeshFilter.h>
#include <Components/MeshRenderer.h>
#include <Components/SkinnedMeshRenderer.h>
#include <Components/TransformComponent.h>

namespace NLS::Editor::AnimationTools
{
AnimationPreview::AnimationPreview(const std::string& title,bool opened,const UI::PanelWindowSettings& settings):AViewControllable(title,opened,settings){}
AnimationPreview::~AnimationPreview(){if(m_renderer)m_renderer->DrainPublishedFramesBeforeTeardown();}
void AnimationPreview::ApplyViewSettings(){m_camera.SetClearColor({.04f,.045f,.055f});m_cameraController.SetSpeed(5);}
void AnimationPreview::EnsureRenderer()
{
    if(m_renderer||!IsSceneRendererAvailable())return;
    m_renderer=Engine::Rendering::CreateSceneRenderer(*EDITOR_CONTEXT(driver));SetRequiresRetiredFrameConsumption(true);
    m_renderer->AddPass<Rendering::GridRenderPass>("Grid",Render::Settings::ERenderPassOrder::First);
    auto* light=m_scene.CreateGameObject("Animation Preview Light").AddComponent<Engine::Components::LightComponent>();light->SetLightType(Render::Settings::ELightType::AMBIENT_SPHERE);light->SetRange(100000);light->SetIntensity(.8f);
    m_material=std::make_shared<Render::Resources::Material>();m_material->SetShader(EDITOR_CONTEXT(shaderManager)[":Shaders/Unlit.hlsl"]);m_material->Set("u_Diffuse",Maths::Vector4(.7f,.75f,.85f,1));
    m_camera.SetFar(10000);
}
void AnimationPreview::SetModel(std::shared_ptr<const Animation::Model> model)
{
    EnsureRenderer();if(m_root)m_root->MarkAsDestroy();m_scene.CollectGarbages();m_root=nullptr;m_animator=nullptr;m_model=std::move(model);if(!m_model)return;
    m_root=&m_scene.CreateGameObject("Animation Preview");m_animator=m_root->AddComponent<Engine::Components::AnimatorComponent>();m_animator->SetModelClip(m_model,0,m_error);
    for(size_t i=0;i<m_model->meshes.size();++i)
    {
        auto& object=m_scene.CreateGameObject(m_model->meshes[i].name);object.SetParent(*m_root);
        const auto& mesh=m_model->meshes[i];const auto binding=std::find_if(m_model->skins.begin(),m_model->skins.end(),[&](const auto& b){return b.id==mesh.skin;});
        if(binding!=m_model->skins.end())
        {Animation::Transform transform;std::string ignored;if(Animation::Decompose(Maths::Matrix4::Inverse(binding->meshFromSkeleton),transform,ignored)){object.GetTransform()->SetLocalPosition(transform.translation);object.GetTransform()->SetLocalRotation(transform.rotation);object.GetTransform()->SetLocalScale(transform.scale);}}
        object.AddComponent<Engine::Components::MeshFilter>();auto* renderer=object.AddComponent<Engine::Components::MeshRenderer>();if(m_material)renderer->FillWithMaterial(*m_material);
        auto* skinned=object.AddComponent<Engine::Components::SkinnedMeshRenderer>();skinned->SetModel(m_model,i,m_error);
    }
    m_cameraController.LockTargetGameObject(*m_root);m_animator->Pause();Open();Focus();
}
bool AnimationPreview::SetProgram(std::shared_ptr<const Animation::AnimationProgram> program,std::string& error)
{if(!m_animator){error="Choose a preview character first";return false;}return m_animator->SetCompiledProgram(std::move(program),error);}
void AnimationPreview::SelectClip(size_t clip){if(m_animator&&m_model)m_animator->SetModelClip(m_model,clip,m_error);}
void AnimationPreview::Update(float delta){AViewControllable::Update(delta);if(m_animator)m_scene.Update(delta);}
}
