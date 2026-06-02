#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <Math/Matrix4.h>
#include <Rendering/Data/DrawCallOptimizationStats.h>
#include <Rendering/Data/Frustum.h>
#include <Rendering/Entities/Drawable.h>
#include <Rendering/Geometry/BoundingSphere.h>

#include "Components/MeshRenderer.h"
#include "EngineDef.h"

namespace NLS::Engine
{
	class GameObject;
}

namespace NLS::Engine::Components
{
	class MeshRenderer;
}

namespace NLS::Engine::SceneSystem
{
	class Scene;
}

namespace NLS::Render::Resources
{
	class Material;
	class Mesh;
}

namespace NLS::Engine::Rendering
{
	struct RenderSceneSyncOptions
	{
		NLS::Render::Resources::Material* defaultMaterial = nullptr;
		NLS::Render::Resources::Material* overrideMaterial = nullptr;
	};

	struct RenderSceneSyncStats
	{
		uint64_t addedPrimitiveCount = 0u;
		uint64_t reusedPrimitiveCount = 0u;
		uint64_t removedPrimitiveCount = 0u;
		uint64_t rebuiltCachedCommandCount = 0u;
	};

	struct RenderSceneVisibilityOptions
	{
		const NLS::Render::Data::Frustum* frustum = nullptr;
		Maths::Vector3 cameraPosition{};
	};

	struct RenderSceneVisibleQueues
	{
		using SceneDrawables = std::vector<std::pair<float, NLS::Render::Entities::Drawable>>;

		SceneDrawables opaques;
		SceneDrawables transparents;
		SceneDrawables skyboxes;
	};

	using DrawCallOptimizationStats = NLS::Render::Data::DrawCallOptimizationStats;

	enum class RenderSceneVisibilityMode
	{
		Auto,
		Serial,
		Parallel
	};

	struct RenderSceneVisibilitySnapshot
	{
		std::vector<uint64_t> primitiveBits;
		std::vector<uint64_t> meshBits;
		uint64_t primitiveCount = 0u;
		uint64_t meshCount = 0u;
		uint64_t visiblePrimitiveCount = 0u;
		uint64_t visibleMeshCount = 0u;
		bool usedParallelEvaluation = false;
	};

	struct RenderCachedDrawCommand
	{
		NLS::Render::Resources::Mesh* mesh = nullptr;
		NLS::Render::Resources::Material* material = nullptr;
		NLS::Render::Data::StateMask stateMask{};
		NLS::Render::Settings::EPrimitiveMode primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;
		uint64_t buildSerial = 0u;
	};

	class NLS_ENGINE_API RenderScene
	{
	public:
		RenderSceneSyncStats Synchronize(
			SceneSystem::Scene& scene,
			const RenderSceneSyncOptions& options = {});

		[[nodiscard]] RenderSceneVisibleQueues GatherVisibleCommands(
			const RenderSceneVisibilityOptions& options = {},
			RenderSceneVisibilityMode mode = RenderSceneVisibilityMode::Auto) const;

		[[nodiscard]] size_t GetPrimitiveCount() const;
		[[nodiscard]] uint64_t GetCachedCommandBuildCountForTesting() const;
		[[nodiscard]] const DrawCallOptimizationStats& GetLastDrawCallOptimizationStats() const;
		[[nodiscard]] const DrawCallOptimizationStats& GetLastDrawCallOptimizationStatsForTesting() const;
		[[nodiscard]] RenderSceneVisibilitySnapshot EvaluateVisibilityForTesting(
			const RenderSceneVisibilityOptions& options = {},
			RenderSceneVisibilityMode mode = RenderSceneVisibilityMode::Auto) const;

	private:
		struct RenderSceneVisibilityRangeSnapshot
		{
			std::vector<uint64_t> primitiveBits;
			std::vector<uint64_t> meshBits;
			size_t primitiveBegin = 0u;
			size_t meshBegin = 0u;
			uint64_t visiblePrimitiveCount = 0u;
			uint64_t visibleMeshCount = 0u;
		};

		struct CachedCommandInputStamp
		{
			NLS::Render::Resources::Mesh* mesh = nullptr;
			NLS::Render::Resources::Material* material = nullptr;
			uint64_t materialInstanceId = 0u;
			uint64_t materialParameterRevision = 0u;
			uint64_t materialRenderStateRevision = 0u;
			uint8_t stateMask = 0u;
			NLS::Render::Settings::EPrimitiveMode primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;

			[[nodiscard]] bool operator==(const CachedCommandInputStamp& other) const;
			[[nodiscard]] bool operator!=(const CachedCommandInputStamp& other) const { return !(*this == other); }
		};

		struct CachedCommandSlot
		{
			CachedCommandInputStamp stamp;
			RenderCachedDrawCommand command;
			bool valid = false;
		};

		struct RenderPrimitive
		{
			Components::MeshRenderer* meshRenderer = nullptr;
			Engine::GameObject* owner = nullptr;
			NLS::Render::Resources::Mesh* mesh = nullptr;
			std::vector<CachedCommandSlot> cachedCommands;
			NLS::Render::Geometry::BoundingSphere modelBoundingSphere;
			Components::MeshRenderer::EFrustumBehaviour frustumBehaviour;
		};

		RenderPrimitive& FindOrCreatePrimitive(
			Components::MeshRenderer& meshRenderer,
			RenderSceneSyncStats& stats);
		void SynchronizePrimitive(
			RenderPrimitive& primitive,
			const RenderSceneSyncOptions& options,
			RenderSceneSyncStats& stats);
		void RemoveMissingPrimitives(
			const std::unordered_set<Components::MeshRenderer*>& liveMeshRenderers,
			RenderSceneSyncStats& stats);
		NLS::Render::Resources::Material* ResolveMaterialForMesh(
			RenderPrimitive& primitive,
			NLS::Render::Resources::Mesh& mesh,
			const RenderSceneSyncOptions& options) const;
		CachedCommandInputStamp BuildCommandInputStamp(
			const RenderPrimitive& primitive,
			NLS::Render::Resources::Mesh& mesh,
			NLS::Render::Resources::Material& material) const;
		void RebuildCachedCommand(
			CachedCommandSlot& slot,
			const CachedCommandInputStamp& stamp,
			RenderSceneSyncStats& stats);
		[[nodiscard]] bool IsPrimitiveVisible(
			const RenderPrimitive& primitive,
			const RenderSceneVisibilityOptions& options) const;
		[[nodiscard]] bool IsMeshVisible(
			const RenderPrimitive& primitive,
			const NLS::Render::Resources::Mesh& mesh,
			const RenderSceneVisibilityOptions& options) const;
		[[nodiscard]] std::vector<size_t> BuildMeshBaseIndices() const;
		[[nodiscard]] RenderSceneVisibilitySnapshot EvaluateVisibility(
			const RenderSceneVisibilityOptions& options,
			RenderSceneVisibilityMode mode) const;
		[[nodiscard]] RenderSceneVisibilitySnapshot EvaluateVisibility(
			const RenderSceneVisibilityOptions& options,
			RenderSceneVisibilityMode mode,
			const std::vector<size_t>& meshBaseIndices) const;
		[[nodiscard]] RenderSceneVisibilitySnapshot EvaluateVisibilitySerialRange(
			const RenderSceneVisibilityOptions& options,
			const std::vector<size_t>& meshBaseIndices,
			size_t primitiveBegin,
			size_t primitiveEnd) const;
		[[nodiscard]] RenderSceneVisibilityRangeSnapshot EvaluateVisibilityCompactRange(
			const RenderSceneVisibilityOptions& options,
			const std::vector<size_t>& meshBaseIndices,
			size_t primitiveBegin,
			size_t primitiveEnd) const;
		[[nodiscard]] RenderSceneVisibilitySnapshot EvaluateVisibilityParallel(
			const RenderSceneVisibilityOptions& options,
			const std::vector<size_t>& meshBaseIndices) const;
		void MergeVisibilityRangeSnapshot(
			RenderSceneVisibilitySnapshot& target,
			const RenderSceneVisibilityRangeSnapshot& source) const;
		void AppendVisibleDrawable(
			RenderSceneVisibleQueues& output,
			const RenderPrimitive& primitive,
			const RenderCachedDrawCommand& command,
			const RenderSceneVisibilityOptions& options) const;
		void FinalizeOpaqueQueue(RenderSceneVisibleQueues::SceneDrawables& opaques) const;
		void AssignVisibleObjectIndices(RenderSceneVisibleQueues& output) const;

		std::vector<RenderPrimitive> m_primitives;
		std::unordered_map<Components::MeshRenderer*, size_t> m_primitiveIndexByMeshRenderer;
		uint64_t m_nextCachedCommandBuildSerial = 1u;
		uint64_t m_cachedCommandBuildCount = 0u;
		RenderSceneSyncStats m_lastSyncStats{};
		mutable DrawCallOptimizationStats m_lastDrawCallOptimizationStats{};
	};
}
