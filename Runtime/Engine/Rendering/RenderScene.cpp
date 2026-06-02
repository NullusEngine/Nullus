#include "Rendering/RenderScene.h"

#include <algorithm>
#include <atomic>
#include <limits>

#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Components/TransformComponent.h"
#include "GameObject.h"
#include "Jobs/JobSystem.h"
#include "Rendering/EngineDrawableDescriptor.h"
#include "Rendering/IndexedObjectDataShaderSupport.h"
#include "Rendering/Data/DrawableInstanceCount.h"
#include "Rendering/Data/ObjectDataLimits.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Resources/Shader.h"
#include "SceneSystem/Scene.h"

namespace NLS::Engine::Rendering
{
namespace
{
	uint32_t ResolveVisibleInstanceCount(const NLS::Render::Entities::Drawable& drawable)
	{
		return NLS::Render::Data::ResolveDrawableInstanceCount(drawable).count;
	}

	template <typename Compare>
	void SortVisibleQueue(RenderSceneVisibleQueues::SceneDrawables& drawables, Compare compare)
	{
		std::stable_sort(
			drawables.begin(),
			drawables.end(),
			[compare](const auto& lhs, const auto& rhs)
			{
				return compare(lhs.first, rhs.first);
			});
	}

	struct OpaqueDrawSortKey
	{
		struct SortToken
		{
			uint64_t stableId = 0u;
			const void* address = nullptr;
		};

		SortToken material{};
		SortToken mesh{};
		uint8_t stateMask = 0u;
		uint8_t primitiveMode = 0u;
		uint32_t vertexStart = 0u;
		uint32_t vertexCount = 0u;
	};

	bool operator<(const OpaqueDrawSortKey::SortToken& lhs, const OpaqueDrawSortKey::SortToken& rhs)
	{
		if (lhs.stableId != rhs.stableId)
			return lhs.stableId < rhs.stableId;
		return std::less<const void*>{}(lhs.address, rhs.address);
	}

	bool operator==(const OpaqueDrawSortKey::SortToken& lhs, const OpaqueDrawSortKey::SortToken& rhs)
	{
		return lhs.stableId == rhs.stableId && lhs.address == rhs.address;
	}

	uint64_t ResolveMaterialSortId(const NLS::Render::Resources::Material* material)
	{
		return material != nullptr ? material->GetInstanceId() : 0u;
	}

	OpaqueDrawSortKey::SortToken BuildMaterialSortToken(const NLS::Render::Resources::Material* material)
	{
		return { ResolveMaterialSortId(material), material };
	}

	OpaqueDrawSortKey::SortToken BuildResourceSortToken(const void* resource)
	{
		return { 0u, resource };
	}

	OpaqueDrawSortKey BuildOpaqueDrawSortKey(const NLS::Render::Entities::Drawable& drawable)
	{
		return {
			BuildMaterialSortToken(drawable.material),
			BuildResourceSortToken(drawable.mesh),
			drawable.stateMask.mask,
			static_cast<uint8_t>(drawable.primitiveMode),
			drawable.vertexStart,
			drawable.vertexCount
		};
	}

	bool operator<(const OpaqueDrawSortKey& lhs, const OpaqueDrawSortKey& rhs)
	{
		if (lhs.material != rhs.material)
			return lhs.material < rhs.material;
		if (lhs.mesh != rhs.mesh)
			return lhs.mesh < rhs.mesh;
		if (lhs.stateMask != rhs.stateMask)
			return lhs.stateMask < rhs.stateMask;
		if (lhs.primitiveMode != rhs.primitiveMode)
			return lhs.primitiveMode < rhs.primitiveMode;
		if (lhs.vertexStart != rhs.vertexStart)
			return lhs.vertexStart < rhs.vertexStart;
		return lhs.vertexCount < rhs.vertexCount;
	}

	bool operator==(const OpaqueDrawSortKey& lhs, const OpaqueDrawSortKey& rhs)
	{
		return lhs.material == rhs.material &&
			lhs.mesh == rhs.mesh &&
			lhs.stateMask == rhs.stateMask &&
			lhs.primitiveMode == rhs.primitiveMode &&
			lhs.vertexStart == rhs.vertexStart &&
			lhs.vertexCount == rhs.vertexCount;
	}

	bool DescriptorCanParticipateInDynamicInstancing(
		const NLS::Engine::Rendering::EngineDrawableDescriptor& descriptor,
		const uint32_t instanceCount)
	{
		if (descriptor.objectCount == 0u || descriptor.objectCount != instanceCount)
			return false;
		if (descriptor.instanceModelMatrices.empty())
			return instanceCount == 1u;
		return descriptor.instanceModelMatrices.size() == instanceCount;
	}

	bool DescriptorPerObjectStateMatchesForDynamicInstancing(
		const NLS::Engine::Rendering::EngineDrawableDescriptor& lhs,
		const NLS::Engine::Rendering::EngineDrawableDescriptor& rhs)
	{
		return NLS::Maths::Matrix4::AreEquals(lhs.userMatrix, rhs.userMatrix);
	}

	bool CanMergeOpaqueDrawables(
		const NLS::Render::Entities::Drawable& lhs,
		const NLS::Render::Entities::Drawable& rhs)
	{
		if (!(BuildOpaqueDrawSortKey(lhs) == BuildOpaqueDrawSortKey(rhs)))
			return false;
		if (lhs.material == nullptr || lhs.mesh == nullptr)
			return false;
		if (lhs.material->IsBlendable())
			return false;
		const auto* shader = lhs.material->GetShader();
		if (shader == nullptr || !ShaderSupportsIndexedObjectData(*shader))
			return false;
		if (ResolveVisibleInstanceCount(lhs) == 0u || ResolveVisibleInstanceCount(rhs) != 1u)
			return false;
		if (lhs.material->GetGPUInstances() != 1)
			return false;

		NLS::Engine::Rendering::EngineDrawableDescriptor lhsDescriptor;
		NLS::Engine::Rendering::EngineDrawableDescriptor rhsDescriptor;
		const auto lhsInstanceCount = ResolveVisibleInstanceCount(lhs);
		const auto rhsInstanceCount = ResolveVisibleInstanceCount(rhs);
		return lhs.TryGetDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(lhsDescriptor) &&
			rhs.TryGetDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(rhsDescriptor) &&
			DescriptorCanParticipateInDynamicInstancing(lhsDescriptor, lhsInstanceCount) &&
			DescriptorCanParticipateInDynamicInstancing(rhsDescriptor, rhsInstanceCount) &&
			DescriptorPerObjectStateMatchesForDynamicInstancing(lhsDescriptor, rhsDescriptor);
	}

	void ExpandDescriptorForObjectDataRange(
		NLS::Engine::Rendering::EngineDrawableDescriptor& descriptor,
		const uint32_t objectCount)
	{
		if (objectCount <= 1u)
		{
			descriptor.objectCount = 1u;
			return;
		}

		descriptor.objectCount = objectCount;
		if (descriptor.instanceModelMatrices.size() == objectCount)
			return;

		descriptor.instanceModelMatrices.assign(objectCount, descriptor.modelMatrix);
	}

	uint32_t ResolveMaxObjectsPerSubmittedDraw()
	{
		return NLS::Render::Data::GetMaxObjectDataCountPerDraw();
	}

	bool DrawableRequiresIndexedObjectDataRange(const NLS::Render::Entities::Drawable& drawable)
	{
		if (drawable.material == nullptr || drawable.material->GetShader() == nullptr)
			return false;

		return ShaderSupportsIndexedObjectData(*drawable.material->GetShader());
	}

	constexpr size_t kBitsPerWord = sizeof(uint64_t) * 8u;
	constexpr size_t kParallelVisibilityPrimitiveThreshold = 1024u;
	constexpr size_t kParallelVisibilityPrimitivesPerTask = 128u;

	size_t BitWordCount(const size_t bitCount)
	{
		return (bitCount + kBitsPerWord - 1u) / kBitsPerWord;
	}

	void SetBit(std::vector<uint64_t>& words, const size_t index)
	{
		if (index >= words.size() * kBitsPerWord)
			return;

		words[index / kBitsPerWord] |= 1ull << (index % kBitsPerWord);
	}

	bool IsBitSet(const std::vector<uint64_t>& words, const size_t index)
	{
		if (index >= words.size() * kBitsPerWord)
			return false;

		return (words[index / kBitsPerWord] & (1ull << (index % kBitsPerWord))) != 0u;
	}

	size_t CountTrailingZeroBits(uint64_t word)
	{
		size_t count = 0u;
		while ((word & 1ull) == 0u)
		{
			word >>= 1u;
			++count;
		}
		return count;
	}

	void MergeBitRange(
		std::vector<uint64_t>& target,
		const std::vector<uint64_t>& source,
		const size_t targetBitBegin)
	{
		for (size_t sourceWordIndex = 0u; sourceWordIndex < source.size(); ++sourceWordIndex)
		{
			auto sourceWord = source[sourceWordIndex];
			while (sourceWord != 0u)
			{
				const auto bitIndex = sourceWordIndex * kBitsPerWord + CountTrailingZeroBits(sourceWord);
				SetBit(target, targetBitBegin + bitIndex);
				sourceWord &= sourceWord - 1u;
			}
		}
	}
}

bool RenderScene::CachedCommandInputStamp::operator==(const CachedCommandInputStamp& other) const
{
	return mesh == other.mesh &&
		material == other.material &&
		materialInstanceId == other.materialInstanceId &&
		materialParameterRevision == other.materialParameterRevision &&
		materialRenderStateRevision == other.materialRenderStateRevision &&
		stateMask == other.stateMask &&
		primitiveMode == other.primitiveMode;
}

RenderSceneSyncStats RenderScene::Synchronize(
	SceneSystem::Scene& scene,
	const RenderSceneSyncOptions& options)
{
	RenderSceneSyncStats stats;
	const auto& fastAccess = scene.GetFastAccessComponents();

	std::unordered_set<Components::MeshRenderer*> liveMeshRenderers;
	liveMeshRenderers.reserve(fastAccess.modelRenderers.size());

	for (auto* meshRenderer : fastAccess.modelRenderers)
	{
		if (meshRenderer == nullptr)
			continue;

		liveMeshRenderers.insert(meshRenderer);
		auto& primitive = FindOrCreatePrimitive(*meshRenderer, stats);
		SynchronizePrimitive(primitive, options, stats);
	}

	RemoveMissingPrimitives(liveMeshRenderers, stats);
	m_lastSyncStats = stats;
	return stats;
}

RenderSceneVisibleQueues RenderScene::GatherVisibleCommands(
	const RenderSceneVisibilityOptions& options,
	const RenderSceneVisibilityMode mode) const
{
	RenderSceneVisibleQueues output;
	m_lastDrawCallOptimizationStats = {};
	const auto meshBaseIndices = BuildMeshBaseIndices();
	const auto visibility = EvaluateVisibility(options, mode, meshBaseIndices);
	const auto visibleMeshCount = static_cast<size_t>(visibility.visibleMeshCount);
	output.opaques.reserve(visibleMeshCount);
	output.transparents.reserve(visibleMeshCount);

	for (size_t primitiveIndex = 0u; primitiveIndex < m_primitives.size(); ++primitiveIndex)
	{
		if (!IsBitSet(visibility.primitiveBits, primitiveIndex))
			continue;

		const auto& primitive = m_primitives[primitiveIndex];
		const auto meshBaseIndex = primitiveIndex < meshBaseIndices.size()
			? meshBaseIndices[primitiveIndex]
			: 0u;

		for (size_t slotIndex = 0u; slotIndex < primitive.cachedCommands.size(); ++slotIndex)
		{
			const auto& slot = primitive.cachedCommands[slotIndex];
			if (!slot.valid || slot.command.mesh == nullptr || slot.command.material == nullptr)
				continue;

			const auto meshBitIndex = meshBaseIndex + slotIndex;
			if (!IsBitSet(visibility.meshBits, meshBitIndex))
				continue;

			AppendVisibleDrawable(output, primitive, slot.command, options);
			++m_lastDrawCallOptimizationStats.rawVisibleObjectCount;
		}
	}

	FinalizeOpaqueQueue(output.opaques);
	SortVisibleQueue(output.transparents, std::greater<float>{});
	AssignVisibleObjectIndices(output);
	m_lastDrawCallOptimizationStats.submittedSceneDrawCount =
		static_cast<uint64_t>(output.opaques.size() + output.transparents.size() + output.skyboxes.size());
	m_lastDrawCallOptimizationStats.dynamicInstanceGroupCount = 0u;
	m_lastDrawCallOptimizationStats.largestInstanceGroupSize = 0u;
	for (const auto& entry : output.opaques)
	{
		const auto instanceCount = entry.second.instanceCount;
		if (instanceCount <= 1u)
			continue;

		++m_lastDrawCallOptimizationStats.dynamicInstanceGroupCount;
		m_lastDrawCallOptimizationStats.largestInstanceGroupSize =
			std::max<uint64_t>(m_lastDrawCallOptimizationStats.largestInstanceGroupSize, instanceCount);
	}
	m_lastDrawCallOptimizationStats.cachedCommandRebuildCount = m_lastSyncStats.rebuiltCachedCommandCount;
	return output;
}

size_t RenderScene::GetPrimitiveCount() const
{
	return m_primitives.size();
}

uint64_t RenderScene::GetCachedCommandBuildCountForTesting() const
{
	return m_cachedCommandBuildCount;
}

const DrawCallOptimizationStats& RenderScene::GetLastDrawCallOptimizationStats() const
{
	return m_lastDrawCallOptimizationStats;
}

const DrawCallOptimizationStats& RenderScene::GetLastDrawCallOptimizationStatsForTesting() const
{
	return GetLastDrawCallOptimizationStats();
}

RenderSceneVisibilitySnapshot RenderScene::EvaluateVisibilityForTesting(
	const RenderSceneVisibilityOptions& options,
	const RenderSceneVisibilityMode mode) const
{
	return EvaluateVisibility(options, mode);
}

RenderScene::RenderPrimitive& RenderScene::FindOrCreatePrimitive(
	Components::MeshRenderer& meshRenderer,
	RenderSceneSyncStats& stats)
{
	const auto found = m_primitiveIndexByMeshRenderer.find(&meshRenderer);
	if (found != m_primitiveIndexByMeshRenderer.end())
	{
		++stats.reusedPrimitiveCount;
		return m_primitives[found->second];
	}

	RenderPrimitive primitive;
	primitive.meshRenderer = &meshRenderer;
	primitive.frustumBehaviour = meshRenderer.GetFrustumBehaviour();

	m_primitives.push_back(std::move(primitive));
	const auto newIndex = m_primitives.size() - 1u;
	m_primitiveIndexByMeshRenderer[&meshRenderer] = newIndex;
	++stats.addedPrimitiveCount;
	return m_primitives.back();
}

void RenderScene::SynchronizePrimitive(
	RenderPrimitive& primitive,
	const RenderSceneSyncOptions& options,
	RenderSceneSyncStats& stats)
{
	auto* meshRenderer = primitive.meshRenderer;
	if (meshRenderer == nullptr)
		return;

	primitive.owner = meshRenderer->gameobject();
	auto* meshFilter = primitive.owner != nullptr
		? primitive.owner->GetComponent<Components::MeshFilter>()
		: nullptr;
	primitive.mesh = meshFilter != nullptr ? meshFilter->ResolveMesh() : nullptr;
	primitive.frustumBehaviour = meshRenderer->GetFrustumBehaviour();

	if (primitive.mesh == nullptr)
	{
		primitive.cachedCommands.clear();
		return;
	}

	primitive.modelBoundingSphere =
		primitive.frustumBehaviour == Components::MeshRenderer::EFrustumBehaviour::CULL_CUSTOM
			? meshRenderer->GetCustomBoundingSphere()
			: primitive.mesh->GetBoundingSphere();

	primitive.cachedCommands.resize(1u);
	auto* material = ResolveMaterialForMesh(primitive, *primitive.mesh, options);
	if (material == nullptr || !material->IsValid())
	{
		primitive.cachedCommands[0].valid = false;
		return;
	}

	auto stamp = BuildCommandInputStamp(primitive, *primitive.mesh, *material);
	auto& slot = primitive.cachedCommands[0];
	if (!slot.valid || slot.stamp != stamp)
		RebuildCachedCommand(slot, stamp, stats);
}

void RenderScene::RemoveMissingPrimitives(
	const std::unordered_set<Components::MeshRenderer*>& liveMeshRenderers,
	RenderSceneSyncStats& stats)
{
	bool removedAny = false;
	for (auto it = m_primitives.begin(); it != m_primitives.end();)
	{
		if (it->meshRenderer == nullptr || liveMeshRenderers.find(it->meshRenderer) == liveMeshRenderers.end())
		{
			it = m_primitives.erase(it);
			++stats.removedPrimitiveCount;
			removedAny = true;
		}
		else
		{
			++it;
		}
	}

	if (!removedAny)
		return;

	m_primitiveIndexByMeshRenderer.clear();
	for (size_t index = 0u; index < m_primitives.size(); ++index)
	{
		if (m_primitives[index].meshRenderer != nullptr)
			m_primitiveIndexByMeshRenderer[m_primitives[index].meshRenderer] = index;
	}
}

NLS::Render::Resources::Material* RenderScene::ResolveMaterialForMesh(
	RenderPrimitive& primitive,
	NLS::Render::Resources::Mesh& mesh,
	const RenderSceneSyncOptions& options) const
{
	if (options.overrideMaterial != nullptr && options.overrideMaterial->IsValid())
		return options.overrideMaterial;

	if (primitive.meshRenderer != nullptr && mesh.GetMaterialIndex() < Components::MeshRenderer::kMaxMaterialCount)
	{
		if (auto* material = primitive.meshRenderer->ResolveMaterialAtIndex(static_cast<uint8_t>(mesh.GetMaterialIndex()));
			material != nullptr && material->IsValid())
		{
			return material;
		}
	}

	return options.defaultMaterial != nullptr && options.defaultMaterial->IsValid()
		? options.defaultMaterial
		: nullptr;
}

RenderScene::CachedCommandInputStamp RenderScene::BuildCommandInputStamp(
	const RenderPrimitive& primitive,
	NLS::Render::Resources::Mesh& mesh,
	NLS::Render::Resources::Material& material) const
{
	auto stateMask = material.GenerateStateMask();
	CachedCommandInputStamp stamp;
	stamp.mesh = &mesh;
	stamp.material = &material;
	stamp.materialInstanceId = material.GetInstanceId();
	stamp.materialParameterRevision = material.GetParameterRevision();
	stamp.materialRenderStateRevision = material.GetRenderStateRevision();
	stamp.stateMask = stateMask.mask;
	stamp.primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;
	return stamp;
}

void RenderScene::RebuildCachedCommand(
	CachedCommandSlot& slot,
	const CachedCommandInputStamp& stamp,
	RenderSceneSyncStats& stats)
{
	slot.stamp = stamp;
	slot.command.mesh = stamp.mesh;
	slot.command.material = stamp.material;
	slot.command.stateMask.mask = stamp.stateMask;
	slot.command.primitiveMode = stamp.primitiveMode;
	slot.command.buildSerial = m_nextCachedCommandBuildSerial++;
	slot.valid = true;
	++m_cachedCommandBuildCount;
	++stats.rebuiltCachedCommandCount;
}

bool RenderScene::IsPrimitiveVisible(
	const RenderPrimitive& primitive,
	const RenderSceneVisibilityOptions& options) const
{
	if (primitive.owner == nullptr || !primitive.owner->IsActive())
		return false;
	if (primitive.mesh == nullptr)
		return false;
	if (options.frustum == nullptr)
		return true;
	if (primitive.frustumBehaviour == Components::MeshRenderer::EFrustumBehaviour::DISABLED)
		return true;

	auto* transform = primitive.owner->GetTransform();
	if (transform == nullptr)
		return false;

	return options.frustum->BoundingSphereInFrustum(
		primitive.modelBoundingSphere,
		transform->GetTransform());
}

bool RenderScene::IsMeshVisible(
	const RenderPrimitive& primitive,
	const NLS::Render::Resources::Mesh& mesh,
	const RenderSceneVisibilityOptions& options) const
{
	if (options.frustum == nullptr ||
		primitive.frustumBehaviour != Components::MeshRenderer::EFrustumBehaviour::CULL_MESHES)
	{
		return true;
	}

	auto* transform = primitive.owner != nullptr ? primitive.owner->GetTransform() : nullptr;
	return transform != nullptr && options.frustum->IsMeshInFrustum(mesh, transform->GetTransform());
}

std::vector<size_t> RenderScene::BuildMeshBaseIndices() const
{
	std::vector<size_t> meshBaseIndices;
	meshBaseIndices.reserve(m_primitives.size() + 1u);

	size_t meshCount = 0u;
	for (const auto& primitive : m_primitives)
	{
		meshBaseIndices.push_back(meshCount);
		meshCount += primitive.cachedCommands.size();
	}
	meshBaseIndices.push_back(meshCount);

	return meshBaseIndices;
}

RenderSceneVisibilitySnapshot RenderScene::EvaluateVisibility(
	const RenderSceneVisibilityOptions& options,
	const RenderSceneVisibilityMode mode) const
{
	const auto meshBaseIndices = BuildMeshBaseIndices();
	return EvaluateVisibility(options, mode, meshBaseIndices);
}

RenderSceneVisibilitySnapshot RenderScene::EvaluateVisibility(
	const RenderSceneVisibilityOptions& options,
	const RenderSceneVisibilityMode mode,
	const std::vector<size_t>& meshBaseIndices) const
{
	if (mode == RenderSceneVisibilityMode::Parallel)
	{
		return EvaluateVisibilityParallel(options, meshBaseIndices);
	}

	return EvaluateVisibilitySerialRange(options, meshBaseIndices, 0u, m_primitives.size());
}

RenderSceneVisibilitySnapshot RenderScene::EvaluateVisibilitySerialRange(
	const RenderSceneVisibilityOptions& options,
	const std::vector<size_t>& meshBaseIndices,
	const size_t primitiveBegin,
	const size_t primitiveEnd) const
{
	RenderSceneVisibilitySnapshot snapshot;
	snapshot.primitiveCount = static_cast<uint64_t>(m_primitives.size());
	snapshot.meshCount = !meshBaseIndices.empty()
		? static_cast<uint64_t>(meshBaseIndices.back())
		: 0u;
	snapshot.primitiveBits.resize(BitWordCount(static_cast<size_t>(snapshot.primitiveCount)));
	snapshot.meshBits.resize(BitWordCount(static_cast<size_t>(snapshot.meshCount)));

	const auto clampedEnd = std::min(primitiveEnd, m_primitives.size());
	for (size_t primitiveIndex = primitiveBegin; primitiveIndex < clampedEnd; ++primitiveIndex)
	{
		const auto& primitive = m_primitives[primitiveIndex];
		if (!IsPrimitiveVisible(primitive, options))
			continue;

		SetBit(snapshot.primitiveBits, primitiveIndex);
		++snapshot.visiblePrimitiveCount;

		const auto meshBaseIndex = primitiveIndex < meshBaseIndices.size()
			? meshBaseIndices[primitiveIndex]
			: 0u;

		for (size_t slotIndex = 0u; slotIndex < primitive.cachedCommands.size(); ++slotIndex)
		{
			const auto& slot = primitive.cachedCommands[slotIndex];
			if (!slot.valid || slot.command.mesh == nullptr || slot.command.material == nullptr)
				continue;
			if (!IsMeshVisible(primitive, *slot.command.mesh, options))
				continue;

			SetBit(snapshot.meshBits, meshBaseIndex + slotIndex);
			++snapshot.visibleMeshCount;
		}
	}

	return snapshot;
}

RenderScene::RenderSceneVisibilityRangeSnapshot RenderScene::EvaluateVisibilityCompactRange(
	const RenderSceneVisibilityOptions& options,
	const std::vector<size_t>& meshBaseIndices,
	const size_t primitiveBegin,
	const size_t primitiveEnd) const
{
	RenderSceneVisibilityRangeSnapshot snapshot;
	snapshot.primitiveBegin = std::min(primitiveBegin, m_primitives.size());
	const auto clampedEnd = std::min(primitiveEnd, m_primitives.size());
	snapshot.meshBegin = snapshot.primitiveBegin < meshBaseIndices.size()
		? meshBaseIndices[snapshot.primitiveBegin]
		: 0u;
	const auto meshEnd = clampedEnd < meshBaseIndices.size()
		? meshBaseIndices[clampedEnd]
		: snapshot.meshBegin;
	snapshot.primitiveBits.resize(BitWordCount(clampedEnd - snapshot.primitiveBegin));
	snapshot.meshBits.resize(BitWordCount(meshEnd - snapshot.meshBegin));

	for (size_t primitiveIndex = snapshot.primitiveBegin; primitiveIndex < clampedEnd; ++primitiveIndex)
	{
		const auto& primitive = m_primitives[primitiveIndex];
		if (!IsPrimitiveVisible(primitive, options))
			continue;

		SetBit(snapshot.primitiveBits, primitiveIndex - snapshot.primitiveBegin);
		++snapshot.visiblePrimitiveCount;

		const auto meshBaseIndex = primitiveIndex < meshBaseIndices.size()
			? meshBaseIndices[primitiveIndex]
			: snapshot.meshBegin;

		for (size_t slotIndex = 0u; slotIndex < primitive.cachedCommands.size(); ++slotIndex)
		{
			const auto& slot = primitive.cachedCommands[slotIndex];
			if (!slot.valid || slot.command.mesh == nullptr || slot.command.material == nullptr)
				continue;
			if (!IsMeshVisible(primitive, *slot.command.mesh, options))
				continue;

			SetBit(snapshot.meshBits, meshBaseIndex + slotIndex - snapshot.meshBegin);
			++snapshot.visibleMeshCount;
		}
	}

	return snapshot;
}

RenderSceneVisibilitySnapshot RenderScene::EvaluateVisibilityParallel(
	const RenderSceneVisibilityOptions& options,
	const std::vector<size_t>& meshBaseIndices) const
{
	const auto primitiveCount = m_primitives.size();
	const auto meshCount = !meshBaseIndices.empty() ? meshBaseIndices.back() : 0u;

	RenderSceneVisibilitySnapshot snapshot;
	snapshot.primitiveCount = static_cast<uint64_t>(primitiveCount);
	snapshot.meshCount = static_cast<uint64_t>(meshCount);
	snapshot.primitiveBits.resize(BitWordCount(primitiveCount));
	snapshot.meshBits.resize(BitWordCount(meshCount));

	if (primitiveCount == 0u)
		return snapshot;

	if (!NLS::Base::Jobs::IsJobSystemInitialized())
		return EvaluateVisibilitySerialRange(options, meshBaseIndices, 0u, primitiveCount);

	const auto hardwareThreads = std::max<uint32_t>(1u, NLS::Base::Jobs::GetJobWorkerCount() + 1u);
	const auto desiredTaskCount =
		(primitiveCount + kParallelVisibilityPrimitivesPerTask - 1u) /
		kParallelVisibilityPrimitivesPerTask;
	const auto taskCount = std::min<size_t>(hardwareThreads, desiredTaskCount);
	if (taskCount <= 1u)
		return EvaluateVisibilitySerialRange(options, meshBaseIndices, 0u, primitiveCount);

	snapshot.usedParallelEvaluation = true;

	const auto rangeSize = (primitiveCount + taskCount - 1u) / taskCount;
	std::vector<RenderSceneVisibilityRangeSnapshot> rangeSnapshots(taskCount);
	std::vector<std::atomic_bool> rangeCompleted(taskCount);
	for (auto& completed : rangeCompleted)
		completed.store(false, std::memory_order_relaxed);

	struct VisibilityRangeJob final : public NLS::Base::Jobs::IJobParallelFor
	{
		const RenderScene* scene = nullptr;
		const RenderSceneVisibilityOptions* options = nullptr;
		const std::vector<size_t>* meshBaseIndices = nullptr;
		std::vector<RenderSceneVisibilityRangeSnapshot>* rangeSnapshots = nullptr;
		std::vector<std::atomic_bool>* rangeCompleted = nullptr;
		size_t primitiveCount = 0u;
		size_t rangeSize = 0u;

		void Execute(uint32_t taskIndex)
		{
			const auto begin = static_cast<size_t>(taskIndex) * rangeSize;
			if (begin >= primitiveCount)
				return;

			const auto end = std::min(begin + rangeSize, primitiveCount);
			(*rangeSnapshots)[static_cast<size_t>(taskIndex)] =
				scene->EvaluateVisibilityCompactRange(*options, *meshBaseIndices, begin, end);
			(*rangeCompleted)[static_cast<size_t>(taskIndex)].store(true, std::memory_order_release);
		}
	};

	VisibilityRangeJob job;
	job.scene = this;
	job.options = &options;
	job.meshBaseIndices = &meshBaseIndices;
	job.rangeSnapshots = &rangeSnapshots;
	job.rangeCompleted = &rangeCompleted;
	job.primitiveCount = primitiveCount;
	job.rangeSize = rangeSize;

	NLS::Base::Jobs::JobParallelForScheduleOptions scheduleOptions;
	scheduleOptions.batchSize = 1u;
	scheduleOptions.debugName = "RenderScene::EvaluateVisibilityParallel";
	auto handle = NLS::Base::Jobs::ScheduleParallelFor(
		job,
		static_cast<uint32_t>(taskCount),
		scheduleOptions);
	if (handle.id == 0u)
		return EvaluateVisibilitySerialRange(options, meshBaseIndices, 0u, primitiveCount);

	NLS::Base::Jobs::CompleteNoClear(handle);
	const auto completionStatus = NLS::Base::Jobs::Internal::GetJobCompletionStatus(handle);
	NLS::Base::Jobs::ClearWithoutSync(handle);
	const bool allRangesCompleted = std::all_of(
		rangeCompleted.begin(),
		rangeCompleted.end(),
		[](const std::atomic_bool& completed)
		{
			return completed.load(std::memory_order_acquire);
		});
	if (completionStatus != NLS::Base::Jobs::JobCompletionStatus::Succeeded ||
		!allRangesCompleted)
	{
		return EvaluateVisibilitySerialRange(options, meshBaseIndices, 0u, primitiveCount);
	}

	for (const auto& rangeSnapshot : rangeSnapshots)
		MergeVisibilityRangeSnapshot(snapshot, rangeSnapshot);

	return snapshot;
}

void RenderScene::MergeVisibilityRangeSnapshot(
	RenderSceneVisibilitySnapshot& target,
	const RenderSceneVisibilityRangeSnapshot& source) const
{
	MergeBitRange(target.primitiveBits, source.primitiveBits, source.primitiveBegin);
	MergeBitRange(target.meshBits, source.meshBits, source.meshBegin);
	target.visiblePrimitiveCount += source.visiblePrimitiveCount;
	target.visibleMeshCount += source.visibleMeshCount;
}

void RenderScene::AppendVisibleDrawable(
	RenderSceneVisibleQueues& output,
	const RenderPrimitive& primitive,
	const RenderCachedDrawCommand& command,
	const RenderSceneVisibilityOptions& options) const
{
	if (primitive.owner == nullptr || primitive.owner->GetTransform() == nullptr)
		return;

	const auto& transform = primitive.owner->GetTransform()->GetTransform();
	const auto userMatrix = primitive.meshRenderer != nullptr
		? primitive.meshRenderer->GetUserMatrix()
		: Maths::Matrix4::Identity;

	NLS::Render::Entities::Drawable drawable;
	drawable.mesh = command.mesh;
	drawable.material = command.material;
	drawable.stateMask = command.stateMask;
	drawable.primitiveMode = command.primitiveMode;
	drawable.AddDescriptor<EngineDrawableDescriptor>({
		transform.GetWorldMatrix(),
		userMatrix
	});

	const float distanceToActor = Maths::Vector3::Distance(
		transform.GetWorldPosition(),
		options.cameraPosition);

	if (command.material != nullptr && command.material->IsBlendable())
		output.transparents.emplace_back(distanceToActor, std::move(drawable));
	else
		output.opaques.emplace_back(distanceToActor, std::move(drawable));
}

void RenderScene::FinalizeOpaqueQueue(RenderSceneVisibleQueues::SceneDrawables& opaques) const
{
	std::stable_sort(
		opaques.begin(),
		opaques.end(),
		[](const auto& lhs, const auto& rhs)
		{
			const auto lhsKey = BuildOpaqueDrawSortKey(lhs.second);
			const auto rhsKey = BuildOpaqueDrawSortKey(rhs.second);
			if (!(lhsKey == rhsKey))
				return lhsKey < rhsKey;
			return lhs.first < rhs.first;
		});

	RenderSceneVisibleQueues::SceneDrawables merged;
	merged.reserve(opaques.size());

	size_t index = 0u;
	while (index < opaques.size())
	{
		auto runEnd = index + 1u;
		while (runEnd < opaques.size() && CanMergeOpaqueDrawables(opaques[index].second, opaques[runEnd].second))
			++runEnd;

		if (runEnd - index > 1u)
		{
			EngineDrawableDescriptor descriptor;
			if (opaques[index].second.TryGetDescriptor<EngineDrawableDescriptor>(descriptor))
			{
				descriptor.instanceModelMatrices.clear();
				descriptor.instanceModelMatrices.reserve(runEnd - index);
				float nearestDistance = opaques[index].first;

				for (size_t runIndex = index; runIndex < runEnd; ++runIndex)
				{
					EngineDrawableDescriptor sourceDescriptor;
					if (!opaques[runIndex].second.TryGetDescriptor<EngineDrawableDescriptor>(sourceDescriptor))
						continue;
					descriptor.instanceModelMatrices.push_back(sourceDescriptor.modelMatrix);
					nearestDistance = std::min(nearestDistance, opaques[runIndex].first);
				}

				descriptor.objectCount = static_cast<uint32_t>(descriptor.instanceModelMatrices.size());
				opaques[index].second.RemoveDescriptor<EngineDrawableDescriptor>();
				opaques[index].second.AddDescriptor<EngineDrawableDescriptor>(std::move(descriptor));
				opaques[index].second.instanceCount =
					static_cast<uint32_t>(std::max<size_t>(1u, runEnd - index));
				opaques[index].first = nearestDistance;
				merged.push_back(std::move(opaques[index]));
				index = runEnd;
				continue;
			}
		}

		merged.push_back(std::move(opaques[index]));
		++index;
	}

	opaques = std::move(merged);
}

void RenderScene::AssignVisibleObjectIndices(RenderSceneVisibleQueues& output) const
{
	uint32_t nextObjectIndex = 0u;
	const auto assignQueue =
		[this, &nextObjectIndex](RenderSceneVisibleQueues::SceneDrawables& queue)
		{
			RenderSceneVisibleQueues::SceneDrawables assigned;
			const auto maxObjectsPerSubmittedDraw = ResolveMaxObjectsPerSubmittedDraw();
			size_t estimatedSubmittedDraws = queue.size();
			for (const auto& entry : queue)
			{
				if (!entry.second.HasDescriptor<EngineDrawableDescriptor>() ||
					!DrawableRequiresIndexedObjectDataRange(entry.second))
				{
					continue;
				}

				const auto instanceCount = ResolveVisibleInstanceCount(entry.second);
				const auto objectCount = std::max(1u, instanceCount);
				if (objectCount > maxObjectsPerSubmittedDraw)
				{
					estimatedSubmittedDraws +=
						static_cast<size_t>((objectCount - 1u) / maxObjectsPerSubmittedDraw);
				}
			}
			assigned.reserve(estimatedSubmittedDraws);
			for (auto& entry : queue)
			{
				if (!entry.second.HasDescriptor<EngineDrawableDescriptor>())
				{
					assigned.push_back(std::move(entry));
					continue;
				}

				if (!DrawableRequiresIndexedObjectDataRange(entry.second))
				{
					assigned.push_back(std::move(entry));
					continue;
				}

				const auto& descriptor = entry.second.GetDescriptor<EngineDrawableDescriptor>();
				const auto instanceCount = ResolveVisibleInstanceCount(entry.second);
				const auto objectCount = std::max(1u, instanceCount);
				const bool preservesMaterialInstanceCount =
					entry.second.instanceCount == 0u && objectCount == instanceCount;
				auto remainingObjectCount = objectCount;
				size_t matrixOffset = 0u;
				const auto appendChunk =
					[&assigned, &descriptor, &entry, objectCount, preservesMaterialInstanceCount](
						const uint32_t chunkObjectCount,
						const uint32_t objectIndex,
						const size_t sourceMatrixOffset)
					{
						NLS::Render::Entities::Drawable chunkDrawable;
						chunkDrawable.mesh = entry.second.mesh;
						chunkDrawable.material = entry.second.material;
						chunkDrawable.stateMask = entry.second.stateMask;
						chunkDrawable.primitiveMode = entry.second.primitiveMode;
						chunkDrawable.vertexStart = entry.second.vertexStart;
						chunkDrawable.vertexCount = entry.second.vertexCount;

						EngineDrawableDescriptor chunkDescriptor;
						chunkDescriptor.modelMatrix = descriptor.modelMatrix;
						chunkDescriptor.userMatrix = descriptor.userMatrix;
						chunkDescriptor.objectIndex = objectIndex;
						chunkDescriptor.objectCount = chunkObjectCount;
						if (!descriptor.instanceModelMatrices.empty())
						{
							const auto sliceBegin = std::min(
								descriptor.instanceModelMatrices.size(),
								sourceMatrixOffset);
							const auto sliceEnd = std::min(
								descriptor.instanceModelMatrices.size(),
								sliceBegin + static_cast<size_t>(chunkObjectCount));
							chunkDescriptor.instanceModelMatrices.assign(
								descriptor.instanceModelMatrices.begin() + static_cast<std::ptrdiff_t>(sliceBegin),
								descriptor.instanceModelMatrices.begin() + static_cast<std::ptrdiff_t>(sliceEnd));
						}
						else
						{
							chunkDescriptor.instanceModelMatrices.clear();
							ExpandDescriptorForObjectDataRange(chunkDescriptor, chunkObjectCount);
						}
						chunkDrawable.instanceCount =
							preservesMaterialInstanceCount && chunkObjectCount == objectCount
								? 0u
								: chunkObjectCount;
						chunkDrawable.AddDescriptor<EngineDrawableDescriptor>(std::move(chunkDescriptor));
						assigned.emplace_back(entry.first, std::move(chunkDrawable));
					};

				if (objectCount <= maxObjectsPerSubmittedDraw &&
					nextObjectIndex < NLS::Render::Data::GetMaxObjectDataCount())
				{
					uint32_t lastObjectIndex = 0u;
					if (NLS::Render::Data::TryResolveObjectDataRangeEnd(
							nextObjectIndex,
							objectCount,
							lastObjectIndex))
					{
						EngineDrawableDescriptor updatedDescriptor = descriptor;
						updatedDescriptor.objectIndex = nextObjectIndex;
						updatedDescriptor.objectCount = objectCount;
						if (updatedDescriptor.instanceModelMatrices.empty())
							ExpandDescriptorForObjectDataRange(updatedDescriptor, objectCount);
						entry.second.RemoveDescriptor<EngineDrawableDescriptor>();
						entry.second.AddDescriptor<EngineDrawableDescriptor>(std::move(updatedDescriptor));
						if (!preservesMaterialInstanceCount || objectCount != instanceCount)
							entry.second.instanceCount = objectCount;
						assigned.push_back(std::move(entry));
						nextObjectIndex = lastObjectIndex + 1u;
						continue;
					}
				}

				while (remainingObjectCount > 0u)
				{
					const auto remainingGlobalCapacity =
						nextObjectIndex < NLS::Render::Data::GetMaxObjectDataCount()
							? NLS::Render::Data::GetMaxObjectDataCount() - nextObjectIndex
							: 0u;
					if (remainingGlobalCapacity == 0u)
					{
						m_lastDrawCallOptimizationStats.objectDataOverflowDroppedObjectCount += remainingObjectCount;
						break;
					}

					const auto chunkObjectCount = std::min<uint32_t>(
						remainingObjectCount,
						std::min<uint32_t>(
							remainingGlobalCapacity,
							ResolveMaxObjectsPerSubmittedDraw()));
					uint32_t lastObjectIndex = 0u;
					if (!NLS::Render::Data::TryResolveObjectDataRangeEnd(
						nextObjectIndex,
						chunkObjectCount,
						lastObjectIndex))
					{
						m_lastDrawCallOptimizationStats.objectDataOverflowDroppedObjectCount += remainingObjectCount;
						break;
					}

					appendChunk(chunkObjectCount, nextObjectIndex, matrixOffset);

					nextObjectIndex = lastObjectIndex + 1u;
					remainingObjectCount -= chunkObjectCount;
					matrixOffset += chunkObjectCount;
				}
			}
			queue = std::move(assigned);
		};

	assignQueue(output.opaques);
	assignQueue(output.skyboxes);
	assignQueue(output.transparents);
}
}
