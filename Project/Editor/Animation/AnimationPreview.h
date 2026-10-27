#pragma once
#include "Panels/AViewControllable.h"
#include <Components/AnimatorComponent.h>
#include <Engine/SceneSystem/Scene.h>

namespace NLS::Editor::AnimationTools
{
class AnimationPreview final:public Panels::AViewControllable
{
public:
    AnimationPreview(const std::string& title,bool opened,const UI::PanelWindowSettings& settings);
    ~AnimationPreview()override;
    Engine::SceneSystem::Scene* GetScene()override{return &m_scene;}
    void EnsureRenderer()override;
    void Update(float delta)override;
    void SetModel(std::shared_ptr<const Animation::Model> model);
    bool SetProgram(std::shared_ptr<const Animation::AnimationProgram> program,std::string& error);
    Engine::Components::AnimatorComponent* Animator()const{return m_animator;}
    std::shared_ptr<const Animation::Model> Model()const{return m_model;}
    void SelectClip(size_t clip);
    const std::string& Error()const{return m_error;}
protected:
    void ApplyViewSettings()override;
private:
    Engine::SceneSystem::Scene m_scene;
    Engine::GameObject* m_root=nullptr;
    Engine::Components::AnimatorComponent* m_animator=nullptr;
    std::shared_ptr<const Animation::Model> m_model;
    std::shared_ptr<Render::Resources::Material> m_material;
    std::string m_error;
};
}
