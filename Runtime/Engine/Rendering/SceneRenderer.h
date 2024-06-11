
#pragma once

#include <map>

#include <Rendering/Core/CompositeRenderer.h>
#include <Rendering/Resources/Mesh.h>
#include <Rendering/Data/Frustum.h>
#include <Rendering/Entities/Drawable.h>
#include <Rendering/Buffers/UniformBuffer.h>
#include <Rendering/Buffers/ShaderStorageBuffer.h>

#include "Rendering/Data/Material.h"
#include "GameObject.h"
#include "Components/CameraComponent.h"
#include "SceneSystem/Scene.h"
#include "EngineDef.h"
namespace NLS
{
	namespace Engine::Rendering
	{
		/**
		* Extension of the CompositeRenderer adding support for the scene system (parsing/drawing entities)
		*/
		class NLS_ENGINE_API SceneRenderer : public NLS::Rendering::Core::CompositeRenderer
		{
		public:
			using OpaqueDrawables = std::multimap<float, NLS::Rendering::Entities::Drawable, std::less<float>>;
			using TransparentDrawables = std::multimap<float, NLS::Rendering::Entities::Drawable, std::greater<float >> ;

			struct AllDrawables
			{
				OpaqueDrawables opaques;
				TransparentDrawables transparents;
			};

			struct SceneDescriptor
			{
				SceneSystem::Scene& scene;
				std::optional<NLS::Rendering::Data::Frustum> frustumOverride;
				NLS::Rendering::Data::Material overrideMaterial;
				NLS::Rendering::Data::Material fallbackMaterial;
			};

			/**
			* Constructor of the Renderer
			* @param p_driver
			*/
			SceneRenderer(NLS::Rendering::Context::Driver& p_driver);

			/**
			* Begin Frame
			* @param p_frameDescriptor
			*/
			virtual void BeginFrame(const NLS::Rendering::Data::FrameDescriptor& p_frameDescriptor) override;

			/**
			* Draw a model with a single material
			* @param p_pso
			* @param p_model
			* @param p_material
			* @param p_modelMatrix
			*/
			virtual void DrawModelWithSingleMaterial(
				NLS::Rendering::Data::PipelineState p_pso,
				NLS::Rendering::Resources::Model& p_model,
				NLS::Rendering::Data::Material& p_material,
				const Maths::Matrix4& p_modelMatrix
			);

		protected:
			AllDrawables ParseScene();
		};
	}

}
