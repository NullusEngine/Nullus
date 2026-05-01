#pragma once

#include <map>

#include <Rendering/Core/CompositeRenderer.h>
#include <Rendering/Resources/Mesh.h>
#include <Rendering/Data/Frustum.h>
#include <Rendering/Entities/Drawable.h>
#include <Rendering/Context/RenderScenePackageBuilder.h>
#include <Rendering/Context/ThreadedRenderingLifecycle.h>
#include "Rendering/LightGridPrepass.h"

#include "Rendering/Resources/Material.h"
#include "GameObject.h"
#include "Components/CameraComponent.h"
#include "SceneSystem/Scene.h"
#include "EngineDef.h"

class FrameGraph;

namespace NLS::Render::FrameGraph
{
	struct CompiledThreadedRenderSceneGraphPass;
	struct ThreadedRenderScenePassMetadata;
}

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
		};

		using SnapshotRenderScenePackageBuildMode = Render::Context::SnapshotRenderScenePackageBuildMode;

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
		const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& GetLightGridGraphicsPassBindingSet() const;
		bool HasPendingLightGridFrameInputs() const { return false; }

	protected:
		void RefreshSceneLightingDescriptor(SceneSystem::Scene& scene);
		AllDrawables ParseScene();
		std::optional<NLS::Render::Context::FrameSnapshot> BuildFrameSnapshot(
			const Render::Data::FrameDescriptor& frameDescriptor) const override;
		static void RefreshFrameSnapshotVisibility(
			NLS::Render::Context::FrameSnapshot& snapshot,
			const AllDrawables& drawables);
		bool CaptureThreadedPreparedDraw(
			PipelineState pso,
			const Drawable& drawable,
			PreparedRecordedDraw& outDraw);
		bool CaptureThreadedPreparedDraw(
			const Drawable& drawable,
			Render::Resources::MaterialPipelineStateOverrides pipelineOverrides,
			Render::Settings::EComparaisonAlgorithm depthCompareOverride,
			PreparedRecordedDraw& outDraw);
		static NLS::Render::Context::RenderScenePackage BuildSnapshotOwnedRenderScenePackage(
			const NLS::Render::Context::FrameSnapshot& snapshot,
			SnapshotRenderScenePackageBuildMode buildMode = SnapshotRenderScenePackageBuildMode::BuildDefaultPassInputs);
		NLS::Render::Context::RenderScenePackage BuildRenderScenePackage(
			const NLS::Render::Context::FrameSnapshot& snapshot) const;
		std::optional<LightGridPrepass::PreparedFrameInputs> BuildLightGridFrameInputs(
			bool hasSkyboxTexture = false) const;
		const std::shared_ptr<LightGridPrepass>& GetLightGridPrepass() const;

	public:
		static const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& GetPreparedPassBindingSetPlaceholder();
		static void ResolvePreparedPassBindingSetPlaceholders(
			NLS::Render::Context::RenderScenePackage& package,
			const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& resolvedBindingSet);

	private:
		std::shared_ptr<LightGridPrepass> m_lightGridPrepass;
		std::unique_ptr<SceneLightingProvider> m_sceneLightingProvider;
	};
}
