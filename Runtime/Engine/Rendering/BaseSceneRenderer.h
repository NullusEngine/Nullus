#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <atomic>

#include <Rendering/Core/CompositeRenderer.h>
#include <Rendering/Data/Frustum.h>
#include <Rendering/Entities/Drawable.h>
#include <Rendering/Context/RenderScenePackageBuilder.h>
#include <Rendering/Context/ThreadedRenderingLifecycle.h>
#include <Rendering/FrameGraph/SceneRenderGraphBuilder.h>
#include <Math/Quaternion.h>
#include <Vector3.h>
#include "Rendering/LargeSceneSettings.h"
#include "Rendering/LightGridPrepass.h"
#include "Rendering/RenderScene.h"
#include "Rendering/SceneOcclusion.h"
#include "Rendering/SceneStreamingResidency.h"

#include "EngineDef.h"

class FrameGraph;

namespace NLS::Core::ResourceManagement
{
	class ShaderManager;
}

namespace NLS::Render::FrameGraph
{
	struct CompiledThreadedRenderSceneGraphPass;
	struct ThreadedRenderScenePassMetadata;
}

namespace NLS::Render::Resources
{
	class Material;
	class Mesh;
}

namespace NLS::Engine::SceneSystem
{
	class Scene;
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
        using Mesh = Render::Resources::Mesh;
        using Driver = Render::Context::Driver;

		using SceneDrawables = std::vector<std::pair<float, Drawable>>;
		using OpaqueDrawables = SceneDrawables;
		using DecalDrawables = SceneDrawables;
		using TransparentDrawables = SceneDrawables;
		using SkyboxDrawables = SceneDrawables;

		struct AllDrawables
		{
			OpaqueDrawables opaques;
			DecalDrawables decals;
			TransparentDrawables transparents;
			SkyboxDrawables skyboxes;
		};

		struct SceneDescriptor
		{
			SceneSystem::Scene& scene;
			std::optional<Frustum> frustumOverride;
			Material* overrideMaterial;
				std::vector<SceneSystem::Scene*> additiveScenes;
				bool includeSkyboxes = true;
					bool requireExplicitMaterialTextures = false;
					bool allowDefaultMaterialForUnresolvedExplicitMaterials = false;
					bool skipSceneDrawables = false;
					bool suppressVisibleMaterialTextureRequests = false;
					bool suppressHZBOcclusion = false;
					bool suppressLightGridCompute = false;
					bool trustSceneRenderContentRevision = false;
				};

		using SnapshotRenderScenePackageBuildMode = Render::Context::SnapshotRenderScenePackageBuildMode;

		explicit BaseSceneRenderer(Driver& p_driver);
		~BaseSceneRenderer() override;

		static bool PreloadSceneFallbackShader(
			NLS::Core::ResourceManagement::ShaderManager& shaderManager,
			bool logWarningOnFailure = true);

		void BeginFrame(const Render::Data::FrameDescriptor& p_frameDescriptor) override;
		void BeginFrameForBackgroundPreview(const Render::Data::FrameDescriptor& p_frameDescriptor) override;

		struct BackgroundPreviewDrawPrewarmResult
		{
			size_t nextDrawIndex = 0u;
			size_t totalDrawCount = 0u;
			bool supported = false;
			bool complete = false;
		};
		virtual BackgroundPreviewDrawPrewarmResult PrewarmBackgroundPreviewDraws(
			const Render::Data::FrameDescriptor& frameDescriptor,
			size_t firstDrawIndex,
			size_t maxDraws);

		virtual void DrawModelWithSingleMaterial(
			PipelineState p_pso,
			Mesh& p_mesh,
			Material& p_material,
			const Maths::Matrix4& p_modelMatrix
		);

		SceneLightingProvider& GetSceneLightingProvider();
		const SceneLightingProvider& GetSceneLightingProvider() const;
		void SetLargeSceneSettings(const LargeSceneSettings& settings);
		[[nodiscard]] const LargeSceneSettings& GetLargeSceneSettings() const;
			const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& GetLightGridGraphicsPassBindingSet() const;
			NLS::Render::FrameGraph::LightGridCompileContext BuildLightGridCompileContext(
				bool hasSkyboxTexture = false) const;
			NLS::Render::FrameGraph::LightGridCompileContext BuildLightGridCompileContext(
				bool hasSkyboxTexture,
				bool suppressCompute) const;
		bool HasPendingLightGridFrameInputs() const { return false; }
		const SceneOcclusionPrimitivePacketBuildResult& GetLastHZBOcclusionPrimitivePacketBuildResult() const;
		const SceneOcclusionHistory& GetHZBOcclusionHistoryForTesting() const;
		[[nodiscard]] bool HasLastVisiblePickablePrimitiveDrawSources() const;
		[[nodiscard]] const std::vector<ScenePickablePrimitiveDrawSource>&
			GetLastVisiblePickablePrimitiveDrawSources() const;
		[[nodiscard]] bool HasCompletedVisibleMaterialTexturePumpForReadback() const;
				const Render::Data::DrawCallOptimizationStats& GetLastDrawCallOptimizationStats() const;
#if defined(NLS_ENABLE_TEST_HOOKS)
				const Render::Data::DrawCallOptimizationStats& GetLastDrawCallOptimizationStatsForTesting() const;
#endif

	protected:
		void RefreshSceneLightingDescriptor(SceneSystem::Scene& scene);
		AllDrawables ParseScene();
		std::optional<NLS::Render::Context::FrameSnapshot> BuildFrameSnapshot(
			const Render::Data::FrameDescriptor& frameDescriptor) const override;
		static void RefreshFrameSnapshotVisibility(
			NLS::Render::Context::FrameSnapshot& snapshot,
			const AllDrawables& drawables);
		virtual bool ShouldPublishCullReasonDebugSnapshots() const;
		virtual uint64_t GetCullReasonDebugSnapshotMaxEntries() const;
		virtual std::vector<const Engine::GameObject*> GetEditorInspectionRoots() const;
		bool CaptureThreadedPreparedDraw(
			PipelineState pso,
			const Drawable& drawable,
			PreparedRecordedDraw& outDraw);
		bool CaptureThreadedPreparedDraw(
			const Drawable& drawable,
			Render::Resources::MaterialPipelineStateOverrides pipelineOverrides,
			Render::Settings::EComparaisonAlgorithm depthCompareOverride,
			std::string_view lightMode,
			PreparedRecordedDraw& outDraw);
		bool CaptureThreadedPreparedDraw(
			const Drawable& drawable,
			Material& effectiveMaterial,
			Render::Resources::MaterialPipelineStateOverrides pipelineOverrides,
			Render::Settings::EComparaisonAlgorithm depthCompareOverride,
			std::string_view lightMode,
			PreparedRecordedDraw& outDraw);
		const SceneOcclusionFrameInput& GetLastHZBOcclusionFrameInput() const;
		bool HasPendingHZBOcclusionObservationFrame() const;
		void DiscardPendingHZBOcclusionObservationFrame();
		void BeginHZBOcclusionObservationFrame(
			const SceneOcclusionFrameInput& frame,
			std::span<const SceneOcclusionPrimitiveInput> primitiveInputs);
		SceneOcclusionObservationStats CompleteHZBOcclusionObservationFrame(
			std::span<const uint32_t> primitiveResultFlags);
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
		static void ResolvePreparedScenePassBindingSetPlaceholders(
			NLS::Render::Context::RenderScenePackage& package,
			const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& resolvedBindingSet,
			uint64_t sceneDrawCount);

	private:
		struct LightGridCompileContextCache
		{
			bool valid = false;
			bool hasSkyboxTexture = false;
			NLS::Render::Data::FrameDescriptor frameDescriptor{};
			Maths::Vector3 cameraPosition{};
			Maths::Quaternion cameraRotation{};
			NLS::Render::FrameGraph::LightGridCompileContext context;
		};

		void InvalidateLightGridCompileContextCache() const;
		void SetLastCullReasonDebugSnapshot(
			const std::shared_ptr<const SceneCullReasonDebugSnapshot>& snapshot) const;
		Material* ResolveDefaultSceneMaterial();
		bool IsLightGridCompileContextCacheHit(
			const NLS::Render::Data::FrameDescriptor& frameDescriptor,
			bool hasSkyboxTexture) const;
		bool AreSameLightGridFrameInputs(
			const LightGridCompileContextCache& cached,
			const NLS::Render::Data::FrameDescriptor& current) const;

		std::shared_ptr<LightGridPrepass> m_lightGridPrepass;
		std::unique_ptr<SceneLightingProvider> m_sceneLightingProvider;
		std::unique_ptr<Material> m_sceneFallbackMaterial;
		NLS::Render::Resources::Shader* m_sceneFallbackShader = nullptr;
		uint64_t m_sceneFallbackShaderInstanceId = 0u;
		uint64_t m_sceneFallbackShaderGeneration = 0u;
		std::string m_sceneFallbackShaderResourcePath;
		RenderScene m_renderScene;
		std::unordered_map<SceneSystem::Scene*, RenderScene> m_additiveRenderScenes;
		std::vector<ScenePickablePrimitiveDrawSource> m_lastVisiblePickablePrimitiveDrawSources;
		bool m_hasLastVisiblePickablePrimitiveDrawSources = false;
		std::shared_ptr<NLS::Render::RHI::RHIBindingSet> m_threadedFrameBindingSet;
		bool m_threadedFrameBindingSetCaptureAttempted = false;
			mutable std::mutex m_lightGridCompileContextCacheMutex;
			mutable LightGridCompileContextCache m_lightGridCompileContextCache;
			mutable NLS::Render::Context::LargeSceneCullReasonDebugSnapshot m_lastCullReasonDebugSnapshot;
		SceneOcclusionPrimitivePacketBuildResult m_lastHZBOcclusionPrimitivePacketBuildResult;
		SceneOcclusionFrameInput m_lastHZBOcclusionFrameInput;
		SceneOcclusionHistory m_hzbOcclusionHistory;
		SceneOcclusionObservationBatch m_hzbPendingOcclusionObservationBatch;
			uint64_t m_hzbOcclusionFrameSerial = 0u;
			SceneStreamingResidency m_streamingResidency;
			LargeSceneSettings m_largeSceneSettings;
			RepresentationResidencySnapshot m_lastRepresentationResidency;
			std::vector<uint64_t> m_lastStreamingDependencyPins;
			uint64_t m_streamingResidencyFrameSerial = 0u;
	};
}
