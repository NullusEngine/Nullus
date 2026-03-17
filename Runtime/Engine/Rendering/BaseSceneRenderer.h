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
	class NLS_ENGINE_API BaseSceneRenderer : public NLS::Render::Core::CompositeRenderer
	{
	public:
		using OpaqueDrawables = std::multimap<float, NLS::Render::Entities::Drawable, std::less<float>>;
		using TransparentDrawables = std::multimap<float, NLS::Render::Entities::Drawable, std::greater<float>>;
		using SkyboxDrawables = std::multimap<float, NLS::Render::Entities::Drawable, std::less<float>>;

		struct AllDrawables
		{
			OpaqueDrawables opaques;
			TransparentDrawables transparents;
			SkyboxDrawables skyboxes;
		};

		struct SceneDescriptor
		{
			SceneSystem::Scene& scene;
			std::optional<NLS::Render::Data::Frustum> frustumOverride;
			NLS::Render::Resources::Material* overrideMaterial;
			NLS::Render::Resources::Material* fallbackMaterial;
		};

		explicit BaseSceneRenderer(NLS::Render::Context::Driver& p_driver);

		void BeginFrame(const NLS::Render::Data::FrameDescriptor& p_frameDescriptor) override;

		virtual void DrawModelWithSingleMaterial(
			NLS::Render::Data::PipelineState p_pso,
			NLS::Render::Resources::Model& p_model,
			NLS::Render::Resources::Material& p_material,
			const Maths::Matrix4& p_modelMatrix
		);

	protected:
		AllDrawables ParseScene();
	};
}
