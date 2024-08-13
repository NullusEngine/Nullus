#include <Rendering/Features/FrameInfoRenderFeature.h>

#include <Utils/PathParser.h>

#include <UI/Plugins/DDTarget.h>
#include <Components/TransformComponent.h>
#include <Components/LightComponent.h>
#include <Engine/Rendering/SceneRenderer.h>

#include "Core/EditorActions.h"
#include "Panels/AssetView.h"
#include "Rendering/GridRenderPass.h"
#include "Rendering/DebugModelRenderFeature.h"
using namespace NLS;
Editor::Panels::AssetView::AssetView
(
	const std::string& p_title,
	bool p_opened,
	const UI::Settings::PanelWindowSettings& p_windowSettings
) : AViewControllable(p_title, p_opened, p_windowSettings)
{
	m_renderer = std::make_unique<Engine::Rendering::SceneRenderer>(*EDITOR_CONTEXT(driver));
	m_renderer->AddFeature<Editor::Rendering::DebugModelRenderFeature>();
	m_renderer->AddFeature<Render::Features::DebugShapeRenderFeature>();
    m_renderer->AddFeature<Render::Features::FrameInfoRenderFeature>();

	m_renderer->AddPass<Editor::Rendering::GridRenderPass>("Grid", Render::Settings::ERenderPassOrder::First);

	m_camera.SetFar(5000.0f);

	auto& directionalLightGO = m_scene.CreateGameObject("Directional Light");
    directionalLightGO.GetTransform()->SetLocalPosition({0.0f, 10.0f, 0.0f});
    directionalLightGO.GetTransform()->SetLocalRotation(Maths::Quaternion({120.0f, -40.0f, 0.0f}));
    auto directionalLight = directionalLightGO.AddComponent<Engine::Components::LightComponent>();
    directionalLight->SetLightType(Render::Settings::ELightType::DIRECTIONAL);
    directionalLight->SetIntensity(0.75f);

	auto& ambientLightGo = m_scene.CreateGameObject("Ambient Light");
    auto ambientLight = ambientLightGo.AddComponent<Engine::Components::LightComponent>();
    ambientLight->SetLightType(Render::Settings::ELightType::AMBIENT_SPHERE);
    ambientLight->SetRadius(10000.0f);

	m_assetActor = &m_scene.CreateGameObject("Asset");
    m_modelRenderer = m_assetActor->AddComponent<Engine::Components::MeshRenderer>();
    m_materialRenderer = m_assetActor->AddComponent<Engine::Components::MaterialRenderer>();

	m_cameraController.LockTargetActor(*m_assetActor);
	
	/* Default Material */
	m_defaultMaterial.SetShader(EDITOR_CONTEXT(shaderManager)[":Shaders/Standard.glsl"]);
	m_defaultMaterial.Set("u_Diffuse", Maths::Vector4(1.f, 1.f, 1.f, 1.f));
	m_defaultMaterial.Set("u_Shininess", 100.0f);
	m_defaultMaterial.Set<Render::Resources::Texture2D*>("u_DiffuseMap", nullptr);

	/* Texture Material */
	m_textureMaterial.SetShader(EDITOR_CONTEXT(shaderManager)[":Shaders/Unlit.glsl"]);
	m_textureMaterial.Set("u_Diffuse", Maths::Vector4(1.f, 1.f, 1.f, 1.f));
	m_textureMaterial.SetBackfaceCulling(false);
	m_textureMaterial.SetBlendable(true);
	m_textureMaterial.Set<Render::Resources::Texture2D*>("u_DiffuseMap", nullptr);

	m_image->AddPlugin<UI::Plugins::DDTarget<std::pair<std::string, UI::Widgets::Layout::Group*>>>("File").DataReceivedEvent += [this](auto p_data)
	{
		std::string path = p_data.first;

		switch (Utils::PathParser::GetFileType(path))
		{
		case Utils::PathParser::EFileType::MODEL:
			if (auto resource = NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::ModelManager>().GetResource(path); resource)
				SetModel(*resource);
			break;
		case Utils::PathParser::EFileType::TEXTURE:
            if (auto resource = NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::TextureManager>().GetResource(path); resource)
				SetTexture(*resource);
			break;

		case Utils::PathParser::EFileType::MATERIAL:
            if (auto resource = NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::MaterialManager>().GetResource(path); resource)
				SetMaterial(*resource);
			break;
		}
	};
}

NLS::Engine::SceneSystem::Scene* Editor::Panels::AssetView::GetScene()
{
	return &m_scene;
}

void Editor::Panels::AssetView::SetResource(ViewableResource p_resource)
{
	if (auto pval = std::get_if<Render::Resources::Model*>(&p_resource); pval && *pval)
	{
		SetModel(**pval);
	}
	else if (auto pval = std::get_if<Render::Resources::Texture2D*>(&p_resource); pval && *pval)
	{
		SetTexture(**pval);
	}
    else if (auto pval = std::get_if<Render::Resources::Material*>(&p_resource); pval && *pval)
	{
		SetMaterial(**pval);
	}
}

void Editor::Panels::AssetView::ClearResource()
{
	m_resource = static_cast<Render::Resources::Texture2D*>(nullptr);
	m_modelRenderer->SetModel(nullptr);
}

void Editor::Panels::AssetView::SetTexture(Render::Resources::Texture2D& p_texture)
{
	m_resource = &p_texture;
	m_assetActor->GetTransform()->SetLocalRotation(Maths::Quaternion({ -90.0f, 0.0f, 0.0f }));
	m_assetActor->GetTransform()->SetLocalScale(Maths::Vector3::One * 3.0f);
	m_modelRenderer->SetModel(EDITOR_CONTEXT(editorResources)->GetModel("Plane"));
	m_textureMaterial.Set<Render::Resources::Texture2D*>("u_DiffuseMap", &p_texture);
	m_materialRenderer->FillWithMaterial(m_textureMaterial);

	m_cameraController.MoveToTarget(*m_assetActor);
}

void Editor::Panels::AssetView::SetModel(Render::Resources::Model& p_model)
{
	m_resource = &p_model;
    m_assetActor->GetTransform()->SetLocalRotation(Maths::Quaternion::Identity);
    m_assetActor->GetTransform()->SetLocalScale(Maths::Vector3::One);
	m_modelRenderer->SetModel(&p_model);
	m_materialRenderer->FillWithMaterial(m_defaultMaterial);

	m_cameraController.MoveToTarget(*m_assetActor);
}

void Editor::Panels::AssetView::SetMaterial(Render::Resources::Material& p_material)
{
	m_resource = &p_material;
    m_assetActor->GetTransform()->SetLocalRotation(Maths::Quaternion::Identity);
    m_assetActor->GetTransform()->SetLocalScale(Maths::Vector3::One);
	m_modelRenderer->SetModel(EDITOR_CONTEXT(editorResources)->GetModel("Sphere"));
	m_materialRenderer->FillWithMaterial(p_material);

	m_cameraController.MoveToTarget(*m_assetActor);
}

const Editor::Panels::AssetView::ViewableResource& Editor::Panels::AssetView::GetResource() const
{
	return m_resource;
}
