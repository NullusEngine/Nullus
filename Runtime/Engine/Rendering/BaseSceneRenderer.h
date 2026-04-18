#pragma once

#include <map>

#include <Rendering/Core/CompositeRenderer.h>
#include <Rendering/Resources/Mesh.h>
#include <Rendering/Data/Frustum.h>
#include <Rendering/Entities/Drawable.h>

#include "Rendering/Resources/Material.h"
#include "GameObject.h"
#include "Components/CameraComponent.h"
#include "SceneSystem/Scene.h"
#include "EngineDef.h"

namespace NLS::Engine::Rendering
{
	class EngineFrameObjectBindingProvider;
	class SceneLightingProvider;

	class NLS_ENGINE_API BaseSceneRenderer : public NLS::Render::Core::CompositeRenderer
	{
	public:
        using Drawable = Render::Entities::Drawable;
        using Frustum = Render::Data::Frustum;
        using PipelineState = Render::Data::PipelineState;
        using Material = Render::Resources::Material;
        using Model = Render::Resources::Model;
        using Driver = Render::Context::Driver;

		using OpaqueDrawables = std::multimap<float, Drawable, std::less<float>>;
		using TransparentDrawables = std::multimap<float, Drawable, std::greater<float>>;
		using SkyboxDrawables = std::multimap<float, Drawable, std::less<float>>;

		struct AllDrawables
		{
			OpaqueDrawables opaques;
			TransparentDrawables transparents;
			SkyboxDrawables skyboxes;
		};

		struct SceneDescriptor
		{
			SceneSystem::Scene& scene;
			std::optional<Frustum> frustumOverride;
			Material* overrideMaterial;
			Material* fallbackMaterial;
		};

		explicit BaseSceneRenderer(Driver& p_driver);
		~BaseSceneRenderer() override;

		void BeginFrame(const Render::Data::FrameDescriptor& p_frameDescriptor) override;

		virtual void DrawModelWithSingleMaterial(
			PipelineState p_pso,
			Model& p_model,
			Material& p_material,
			const Maths::Matrix4& p_modelMatrix
		);

		SceneLightingProvider& GetSceneLightingProvider();
		const SceneLightingProvider& GetSceneLightingProvider() const;

	protected:
		void RefreshSceneLightingDescriptor(SceneSystem::Scene& scene);
		AllDrawables ParseScene();

	private:
		std::unique_ptr<SceneLightingProvider> m_sceneLightingProvider;
	};
}
