#include "Rendering/RenderScene.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <any>
#include <chrono>
#include <filesystem>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include <Json/json.hpp>

#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Core/ServiceLocator.h"
#include "Components/TransformComponent.h"
#include "GameObject.h"
#include "Engine/Assets/ModelPrefabBuilder.h"
#include "Jobs/JobSystem.h"
#include "Rendering/EngineDrawableDescriptor.h"
#include "Rendering/Geometry/BoundingSphereUtils.h"
#include "Rendering/LargeSceneSettings.h"
#include "Rendering/IndexedObjectDataShaderSupport.h"
#include "Rendering/Data/DrawableInstanceCount.h"
#include "Rendering/Data/ObjectDataLimits.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/SceneHLOD.h"
#include "Rendering/SceneLOD.h"
#include "Rendering/SceneStreamingResidency.h"
#include "Rendering/SceneSpatialIndex.h"
#include "Rendering/SceneVisibilityPipeline.h"
#include "SceneSystem/Scene.h"
#include "Profiling/Profiler.h"

namespace NLS::Engine::Rendering
{
	struct RenderSceneDeclaredTextureLookupCache
	{
		std::unordered_map<std::string, NLS::Render::Resources::Texture2D*> declaredTexturesByNormalizedPath;
		std::unordered_map<std::string, NLS::Render::Resources::Texture2D*> resourcesByNormalizedPath;
		bool resourceIndexBuilt = false;
	};

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
		return lhs.objectFlags == rhs.objectFlags &&
			NLS::Maths::Matrix4::AreEquals(lhs.userMatrix, rhs.userMatrix);
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

	std::string NormalizeResourcePathKey(const std::string& path)
	{
		if (path.empty())
			return {};

		try
		{
			auto normalized = NLS::Core::ResourceManagement::TextureManager::ResolveResourcePath(path);
			std::replace(normalized.begin(), normalized.end(), '\\', '/');
			return std::filesystem::path(normalized).lexically_normal().generic_string();
		}
		catch (const std::filesystem::filesystem_error&)
		{
			return {};
		}
		catch (const std::system_error&)
		{
			return {};
		}
	}

		void BuildDeclaredTextureResourceIndex(
		NLS::Core::ResourceManagement::TextureManager& textureManager,
		RenderSceneDeclaredTextureLookupCache& cache,
		RenderSceneSyncStats& stats)
	{
		if (cache.resourceIndexBuilt)
			return;

		cache.resourceIndexBuilt = true;
		++stats.declaredTextureResourceScanCount;
		const auto resources = textureManager.GetResources();
		cache.resourcesByNormalizedPath.reserve(resources.size() * 2u);
		for (const auto& [resourcePath, texture] : resources)
		{
			if (texture == nullptr)
				continue;

			const auto resourceKey = NormalizeResourcePathKey(resourcePath);
			if (!resourceKey.empty())
				cache.resourcesByNormalizedPath.try_emplace(resourceKey, texture);

			const auto textureKey = NormalizeResourcePathKey(texture->path);
			if (!textureKey.empty())
				cache.resourcesByNormalizedPath.try_emplace(textureKey, texture);
		}
	}

	NLS::Render::Resources::Texture2D* FindCachedDeclaredTexture(
		NLS::Core::ResourceManagement::TextureManager& textureManager,
		const std::string& texturePath,
		RenderSceneDeclaredTextureLookupCache& cache,
		RenderSceneSyncStats& stats)
	{
		++stats.declaredTextureLookupCount;

		const auto declaredKey = NormalizeResourcePathKey(texturePath);
		const auto cacheKey = declaredKey.empty() ? texturePath : declaredKey;
		if (cacheKey.empty())
			return nullptr;

		if (const auto cached = cache.declaredTexturesByNormalizedPath.find(cacheKey);
			cached != cache.declaredTexturesByNormalizedPath.end())
		{
			++stats.declaredTextureCacheHitCount;
			return cached->second;
		}

		++stats.declaredTextureCacheMissCount;
		if (auto* texture = textureManager.GetResource(texturePath, false))
		{
			cache.declaredTexturesByNormalizedPath.emplace(cacheKey, texture);
			return texture;
		}

		if (!declaredKey.empty())
		{
			BuildDeclaredTextureResourceIndex(textureManager, cache, stats);
			if (const auto found = cache.resourcesByNormalizedPath.find(declaredKey);
				found != cache.resourcesByNormalizedPath.end())
			{
				cache.declaredTexturesByNormalizedPath.emplace(cacheKey, found->second);
				return found->second;
			}
		}

		cache.declaredTexturesByNormalizedPath.emplace(cacheKey, nullptr);
		return nullptr;
	}

	bool TexturePathMatchesDeclaredPath(
		const NLS::Render::Resources::Texture2D& texture,
		const std::string& declaredPath)
	{
		return declaredPath.empty() ||
			texture.path == declaredPath ||
			NormalizeResourcePathKey(texture.path) == NormalizeResourcePathKey(declaredPath);
	}

	bool HasResolvedDeclaredMaterialTextures(const NLS::Render::Resources::Material& material)
	{
		NLS::Core::ResourceManagement::TextureManager* textureManager = nullptr;
		if (NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::TextureManager>())
			textureManager = &NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager);

		for (const auto& [uniformName, texturePath] : material.GetTextureResourcePaths())
		{
			if (texturePath.empty())
				continue;

			const auto* parameter = material.GetParameterBlock().TryGet(uniformName);
			if (parameter == nullptr || parameter->type() != typeid(NLS::Render::Resources::Texture2D*))
				return false;

			const auto* boundTexture = std::any_cast<NLS::Render::Resources::Texture2D*>(*parameter);
			if (boundTexture == nullptr || boundTexture->GetTextureHandle() == nullptr)
				return false;

			if (textureManager != nullptr)
			{
				const auto* currentTexture = textureManager->GetArtifactResource(texturePath, false);
				if (currentTexture == nullptr || boundTexture != currentTexture)
					return false;
			}
			else if (!TexturePathMatchesDeclaredPath(*boundTexture, texturePath))
			{
				return false;
			}
		}
		return true;
	}

	bool TryBindDeclaredMaterialTexturesFromCache(
		NLS::Render::Resources::Material& material,
		RenderSceneDeclaredTextureLookupCache& cache,
		RenderSceneSyncStats& stats)
	{
		if (!NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::TextureManager>())
			return false;

		bool boundAny = false;
		auto& textureManager = NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager);
		for (const auto& [uniformName, texturePath] : material.GetTextureResourcePaths())
		{
			if (texturePath.empty())
				continue;

				auto* texture = FindCachedDeclaredTexture(textureManager, texturePath, cache, stats);
				if (texture == nullptr || texture->GetTextureHandle() == nullptr)
					continue;

			const auto* parameter = material.GetParameterBlock().TryGet(uniformName);
			if (parameter != nullptr &&
				parameter->type() == typeid(NLS::Render::Resources::Texture2D*) &&
				std::any_cast<NLS::Render::Resources::Texture2D*>(*parameter) == texture)
			{
				continue;
			}

			material.Set<NLS::Render::Resources::Texture2D*>(uniformName, texture);
			boundAny = true;
		}
		return boundAny;
	}

	constexpr size_t kBitsPerWord = sizeof(uint64_t) * 8u;

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

	const LargeSceneSettings& ResolveLargeSceneSettings(const RenderSceneVisibilityOptions& options)
	{
		if (options.largeSceneSettings != nullptr)
			return *options.largeSceneSettings;

		static const LargeSceneSettings kDefaultSettings = LargeSceneSettings::Defaults();
		return kDefaultSettings;
	}

	const LargeSceneSettings& ResolveLargeSceneSettings(const RenderSceneSyncOptions& options)
	{
		if (options.largeSceneSettings != nullptr)
			return *options.largeSceneSettings;

		static const LargeSceneSettings kDefaultSettings = LargeSceneSettings::Defaults();
		return kDefaultSettings;
	}

	struct ScenePrimitiveHandleHash
	{
		size_t operator()(const ScenePrimitiveHandle& handle) const noexcept
		{
			auto hash = static_cast<size_t>(handle.sceneId);
			hash ^= static_cast<size_t>(handle.index) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
			hash ^= static_cast<size_t>(handle.generation) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
			return hash;
		}
	};

	void OverlayRepresentationResidencySnapshot(
		RepresentationResidencySnapshot& residency,
		const RepresentationResidencySnapshot& overlay)
	{
		for (const auto handle : overlay.fallbackPrimitiveResources)
			residency.MarkFallback(handle);
		for (const auto handle : overlay.readyPrimitiveResources)
			residency.MarkReady(handle);
		for (const auto handle : overlay.readyHLODProxyResources)
			residency.MarkHLODProxyReady(handle);
		for (const auto handle : overlay.notResidentResources)
			residency.MarkNotResident(handle);
	}

	RepresentationResidencySnapshot BuildRepresentationResidencySnapshot(
		const ScenePrimitiveSnapshot& snapshot,
		const RepresentationResidencySnapshot* modeledResidency)
	{
		RepresentationResidencySnapshot residency;
		for (const auto& record : snapshot.primitiveRecords)
		{
			if (!record.ownerAlive || !record.ownerActive)
				continue;
			if (modeledResidency != nullptr)
				residency.MarkNotResident(record.handle);
			else if (record.mesh != nullptr)
				residency.MarkReady(record.handle);
			else
				residency.MarkNotResident(record.handle);
		}
		if (modeledResidency != nullptr)
			OverlayRepresentationResidencySnapshot(residency, *modeledResidency);
		return residency;
	}

	void MarkHLODProxyResidency(
		RepresentationResidencySnapshot& residency,
		const ScenePrimitiveSnapshot& snapshot,
		const std::vector<HLODClusterRecord>& clusters)
	{
	for (const auto& cluster : clusters)
	{
		if (!cluster.proxyPrimitive.has_value())
			continue;

		const auto found = std::find_if(
			snapshot.primitiveRecords.begin(),
			snapshot.primitiveRecords.end(),
			[&](const ScenePrimitiveSnapshotRecord& record)
			{
				return record.handle == *cluster.proxyPrimitive && record.mesh != nullptr;
			});
		if (found != snapshot.primitiveRecords.end())
			residency.MarkHLODProxyReady(*cluster.proxyPrimitive);
	}
}

	uint64_t AllocateRenderSceneId()
	{
		static std::atomic_uint64_t nextSceneId { 1u };
		return nextSceneId.fetch_add(1u, std::memory_order_relaxed);
	}

	uint32_t IncrementPrimitiveGeneration(const uint32_t generation)
	{
		const auto nextGeneration = generation + 1u;
		return nextGeneration != 0u ? nextGeneration : 1u;
	}

	bool AreSameBounds(
		const NLS::Render::Geometry::BoundingSphere& lhs,
		const NLS::Render::Geometry::BoundingSphere& rhs)
	{
		return lhs.position == rhs.position && lhs.radius == rhs.radius;
	}

	struct ImportedHierarchyHLODMetadata
	{
		std::string clusterKey;
		std::vector<std::string> children;
		std::string proxySubAssetKey;
	};

	std::optional<ImportedHierarchyHLODMetadata> ParseImportedHierarchyHLODMetadata(const std::string& metadataJson)
	{
		try
		{
			const auto metadata = nlohmann::json::parse(metadataJson);
			if (!metadata.is_object())
				return std::nullopt;

			const auto source = metadata.find(NLS::Engine::Assets::GeneratedModelPrefabHLODSchema::SourceField);
			if (source == metadata.end() ||
				!source->is_string() ||
				source->get<std::string>() != NLS::Engine::Assets::GeneratedModelPrefabHLODSchema::ImportedHierarchySource)
			{
				return std::nullopt;
			}

			ImportedHierarchyHLODMetadata parsed;
			const auto clusterKey = metadata.find(NLS::Engine::Assets::GeneratedModelPrefabHLODSchema::ClusterKeyField);
			const auto children = metadata.find(NLS::Engine::Assets::GeneratedModelPrefabHLODSchema::ChildrenField);
			const auto proxy = metadata.find(NLS::Engine::Assets::GeneratedModelPrefabHLODSchema::ProxySubAssetKeyField);
			if (clusterKey == metadata.end() || !clusterKey->is_string() ||
				children == metadata.end() || !children->is_array() ||
				proxy == metadata.end() || !proxy->is_string())
			{
				return std::nullopt;
			}

			parsed.clusterKey = clusterKey->get<std::string>();
			parsed.proxySubAssetKey = proxy->get<std::string>();
			for (const auto& child : *children)
			{
				if (child.is_string())
					parsed.children.push_back(child.get<std::string>());
			}
			if (parsed.clusterKey.empty() || parsed.proxySubAssetKey.empty() || parsed.children.empty())
				return std::nullopt;
			return parsed;
		}
		catch (...)
		{
			return std::nullopt;
		}
	}

	bool AreSameBounds(
		const NLS::Render::Geometry::Bounds& lhs,
		const NLS::Render::Geometry::Bounds& rhs)
	{
		return lhs.center == rhs.center && lhs.size == rhs.size;
	}

	bool AreSameMatrix(const Maths::Matrix4& lhs, const Maths::Matrix4& rhs)
	{
		for (size_t index = 0u; index < 16u; ++index)
		{
			if (lhs.data[index] != rhs.data[index])
				return false;
		}
		return true;
	}

	uint64_t ElapsedNanoseconds(const std::chrono::steady_clock::time_point start)
	{
		const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - start).count();
		return static_cast<uint64_t>(std::max<int64_t>(elapsed, 1));
	}

	float ResolveSpatialQueryRadius(const RenderSceneVisibilityOptions& options)
	{
		if (options.frustum == nullptr)
			return 0.0f;

		const auto planeNormal = [](const std::array<float, 4>& plane)
		{
			return Maths::Vector3(plane[0], plane[1], plane[2]);
		};
		const auto intersectPlanes = [&](const std::array<float, 4>& first,
			const std::array<float, 4>& second,
			const std::array<float, 4>& third,
			Maths::Vector3& point)
		{
			const auto n1 = planeNormal(first);
			const auto n2 = planeNormal(second);
			const auto n3 = planeNormal(third);
			const auto n2CrossN3 = Maths::Vector3::Cross(n2, n3);
			const auto denominator = Maths::Vector3::Dot(n1, n2CrossN3);
			if (std::abs(denominator) <= std::numeric_limits<float>::epsilon())
				return false;

			point =
				(n2CrossN3 * -first[3] -
				 Maths::Vector3::Cross(n3, n1) * second[3] -
				 Maths::Vector3::Cross(n1, n2) * third[3]) /
				denominator;
			return true;
		};

		const auto farPlane = options.frustum->GetFarPlane();
		const auto farSignedDistance =
			farPlane[0] * options.cameraPosition.x +
			farPlane[1] * options.cameraPosition.y +
			farPlane[2] * options.cameraPosition.z +
			farPlane[3];
		const auto farDistance = std::abs(farSignedDistance);
		float queryRadius = farDistance;

		const std::array<std::array<float, 4>, 2u> horizontalPlanes = {
			options.frustum->GetLeftPlane(),
			options.frustum->GetRightPlane()
		};
		const std::array<std::array<float, 4>, 2u> verticalPlanes = {
			options.frustum->GetBottomPlane(),
			options.frustum->GetTopPlane()
		};
		for (const auto& horizontalPlane : horizontalPlanes)
		{
			for (const auto& verticalPlane : verticalPlanes)
			{
				Maths::Vector3 corner;
				if (!intersectPlanes(farPlane, horizontalPlane, verticalPlane, corner))
					continue;
				queryRadius = std::max(queryRadius, Maths::Vector3::Distance(options.cameraPosition, corner));
			}
		}

		return queryRadius > 0.0f ? queryRadius : 0.0f;
	}

}

#if defined(NLS_ENABLE_TEST_HOOKS)
bool HasResolvedDeclaredMaterialTexturesForTesting(
	const NLS::Render::Resources::Material& material)
{
	return HasResolvedDeclaredMaterialTextures(material);
}
#endif

struct RenderScene::RepresentationRegistry
{
	std::vector<LODGroupRecord> lodGroups;
	std::vector<HLODClusterRecord> hlodClusters;
	mutable std::unordered_map<uint64_t, std::vector<LODSelectionHistory>> lodSelectionHistoryByView;
	std::unordered_map<ScenePrimitiveHandle, std::vector<uint32_t>, ScenePrimitiveHandleStableHash> lodGroupsByPrimitive;
	std::unordered_map<ScenePrimitiveHandle, std::vector<uint32_t>, ScenePrimitiveHandleStableHash> hlodClustersByPrimitive;

	[[nodiscard]] bool HasLOD() const
	{
		return !lodGroups.empty();
	}

	[[nodiscard]] bool HasHLOD() const
	{
		return !hlodClusters.empty();
	}

	[[nodiscard]] std::vector<LODSelectionHistory>& LODHistoryForView(const uint64_t viewKey) const
	{
		auto& history = lodSelectionHistoryByView[viewKey];
		if (history.size() < lodGroups.size())
			history.resize(lodGroups.size());
		return history;
	}

	void ResizeLODHistories()
	{
		for (auto& [viewKey, history] : lodSelectionHistoryByView)
		{
			(void)viewKey;
			if (history.size() < lodGroups.size())
				history.resize(lodGroups.size());
		}
	}

	void RebuildLookup()
	{
		lodGroupsByPrimitive.clear();
		hlodClustersByPrimitive.clear();
		for (const auto& group : lodGroups)
		{
			if (!group.groupHandle.IsValid())
				continue;
			for (const auto& level : group.levels)
			{
				for (const auto handle : level.primitiveHandles)
					lodGroupsByPrimitive[handle].push_back(group.groupHandle.index);
			}
		}
		for (const auto& cluster : hlodClusters)
		{
			if (!cluster.clusterHandle.IsValid())
				continue;
			for (const auto child : cluster.childPrimitives)
				hlodClustersByPrimitive[child].push_back(cluster.clusterHandle.index);
			if (cluster.proxyPrimitive.has_value())
				hlodClustersByPrimitive[*cluster.proxyPrimitive].push_back(cluster.clusterHandle.index);
		}
	}
};

RenderScene::RenderScene()
	: m_sceneId(AllocateRenderSceneId()),
	  m_spatialIndex(std::make_unique<SceneSpatialIndex>()),
	  m_representationRegistry(std::make_unique<RepresentationRegistry>()),
	  m_importedHierarchyHLODClusterHandles()
{
}

RenderScene::~RenderScene() = default;

RenderScene::RenderScene(RenderScene&& other) noexcept
	: m_sceneId(other.m_sceneId),
	  m_primitives(std::move(other.m_primitives)),
	  m_primitiveIndexByMeshRenderer(std::move(other.m_primitiveIndexByMeshRenderer)),
	  m_firstFreePrimitiveSlot(other.m_firstFreePrimitiveSlot),
	  m_livePrimitiveCount(other.m_livePrimitiveCount),
	  m_lastSceneFastAccessRevision(other.m_lastSceneFastAccessRevision),
	  m_nextCachedCommandBuildSerial(other.m_nextCachedCommandBuildSerial),
	  m_cachedCommandBuildCount(other.m_cachedCommandBuildCount),
	  m_nextPrimitiveSnapshotSerial(other.m_nextPrimitiveSnapshotSerial),
	  m_cachedMeshBaseIndices(std::move(other.m_cachedMeshBaseIndices)),
	  m_commandOffsetTableDirty(other.m_commandOffsetTableDirty),
	  m_lastDirtySyncHandles(std::move(other.m_lastDirtySyncHandles)),
	  m_lastRemovedHandles(std::move(other.m_lastRemovedHandles)),
	  m_lastSyncStats(other.m_lastSyncStats),
	  m_lastDrawCallOptimizationStats(other.m_lastDrawCallOptimizationStats),
	  m_lastLargeSceneTelemetry(other.m_lastLargeSceneTelemetry),
	  m_lastVisiblePrimitiveHandles(std::move(other.m_lastVisiblePrimitiveHandles)),
	  m_lastRepresentationStreamingInterest(std::move(other.m_lastRepresentationStreamingInterest)),
	  m_spatialIndex(std::move(other.m_spatialIndex)),
	  m_representationRegistry(std::move(other.m_representationRegistry)),
	  m_importedHierarchyHLODClusterHandles(std::move(other.m_importedHierarchyHLODClusterHandles))
{
	other.ResetMovedFromState();
}

RenderScene& RenderScene::operator=(RenderScene&& other) noexcept
{
	if (this == &other)
		return *this;

	m_sceneId = other.m_sceneId;
	m_primitives = std::move(other.m_primitives);
	m_primitiveIndexByMeshRenderer = std::move(other.m_primitiveIndexByMeshRenderer);
	m_firstFreePrimitiveSlot = other.m_firstFreePrimitiveSlot;
	m_livePrimitiveCount = other.m_livePrimitiveCount;
	m_lastSceneFastAccessRevision = other.m_lastSceneFastAccessRevision;
	m_nextCachedCommandBuildSerial = other.m_nextCachedCommandBuildSerial;
	m_cachedCommandBuildCount = other.m_cachedCommandBuildCount;
	m_nextPrimitiveSnapshotSerial = other.m_nextPrimitiveSnapshotSerial;
	m_cachedMeshBaseIndices = std::move(other.m_cachedMeshBaseIndices);
	m_commandOffsetTableDirty = other.m_commandOffsetTableDirty;
	m_lastDirtySyncHandles = std::move(other.m_lastDirtySyncHandles);
	m_lastRemovedHandles = std::move(other.m_lastRemovedHandles);
	m_lastSyncStats = other.m_lastSyncStats;
	m_lastDrawCallOptimizationStats = other.m_lastDrawCallOptimizationStats;
	m_lastLargeSceneTelemetry = other.m_lastLargeSceneTelemetry;
	m_lastVisiblePrimitiveHandles = std::move(other.m_lastVisiblePrimitiveHandles);
	m_lastRepresentationStreamingInterest = std::move(other.m_lastRepresentationStreamingInterest);
	m_spatialIndex = std::move(other.m_spatialIndex);
	m_representationRegistry = std::move(other.m_representationRegistry);
	m_importedHierarchyHLODClusterHandles = std::move(other.m_importedHierarchyHLODClusterHandles);

	other.ResetMovedFromState();
	return *this;
}

void RenderScene::ResetMovedFromState() noexcept
{
	m_sceneId = AllocateRenderSceneId();
	m_primitives.clear();
	m_primitiveIndexByMeshRenderer.clear();
	m_firstFreePrimitiveSlot.reset();
	m_livePrimitiveCount = 0u;
	m_lastSceneFastAccessRevision = 0u;
	m_nextCachedCommandBuildSerial = 1u;
	m_cachedCommandBuildCount = 0u;
	m_nextPrimitiveSnapshotSerial = 1u;
	m_cachedMeshBaseIndices.clear();
	m_commandOffsetTableDirty = true;
	m_lastDirtySyncHandles.clear();
	m_lastRemovedHandles.clear();
	m_lastSyncStats = {};
	m_lastDrawCallOptimizationStats = {};
	m_lastLargeSceneTelemetry = {};
	m_lastVisiblePrimitiveHandles.clear();
	m_lastRepresentationStreamingInterest.clear();
	m_spatialIndex = std::make_unique<SceneSpatialIndex>();
	m_representationRegistry = std::make_unique<RepresentationRegistry>();
	m_importedHierarchyHLODClusterHandles.clear();
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

bool RenderScene::PrimitiveInputStamp::operator==(const PrimitiveInputStamp& other) const
{
	return owner == other.owner &&
		meshFilter == other.meshFilter &&
		mesh == other.mesh &&
		AreSameMatrix(worldMatrix, other.worldMatrix) &&
		layer == other.layer &&
			ownerRenderRevision == other.ownerRenderRevision &&
			transformRenderRevision == other.transformRenderRevision &&
			meshFilterRenderRevision == other.meshFilterRenderRevision &&
			meshRendererRenderRevision == other.meshRendererRenderRevision &&
			meshContentRevision == other.meshContentRevision &&
			materialInstanceId == other.materialInstanceId &&
			materialParameterRevision == other.materialParameterRevision &&
				materialRenderStateRevision == other.materialRenderStateRevision &&
				explicitMaterialTexturesResolved == other.explicitMaterialTexturesResolved &&
				requireExplicitMaterialTextures == other.requireExplicitMaterialTextures &&
				allowDefaultMaterialForUnresolvedExplicitMaterials == other.allowDefaultMaterialForUnresolvedExplicitMaterials &&
				ownerAlive == other.ownerAlive &&
				ownerActive == other.ownerActive;
}

RenderSceneSyncStats RenderScene::Synchronize(
	SceneSystem::Scene& scene,
	const RenderSceneSyncOptions& options)
{
	NLS_PROFILE_NAMED_SCOPE("RenderScene::Synchronize");
	const auto syncStart = std::chrono::steady_clock::now();
	RenderSceneSyncStats stats;
	RenderSceneDeclaredTextureLookupCache declaredTextureCache;
	m_lastDirtySyncHandles.clear();
	m_lastRemovedHandles.clear();
	const auto& fastAccess = scene.GetFastAccessComponents();
	const auto fastAccessRevision = scene.GetFastAccessComponentsRevision();
	const bool fastAccessMembershipChanged =
		fastAccessRevision != m_lastSceneFastAccessRevision;

	std::unordered_map<Components::MeshRenderer*, NLS::InstanceID> liveMeshRenderers;
	if (fastAccessMembershipChanged)
	{
		NLS_PROFILE_NAMED_SCOPE("RenderScene::Synchronize::BuildLiveMeshRendererSetReserve");
		liveMeshRenderers.reserve(fastAccess.modelRenderers.size());
	}

	{
		NLS_PROFILE_NAMED_SCOPE("RenderScene::Synchronize::SynchronizePrimitiveLoop");
		for (auto* meshRenderer : fastAccess.modelRenderers)
		{
			if (meshRenderer == nullptr)
				continue;
			auto* owner = meshRenderer->gameobject();
			if (owner == nullptr || !owner->IsAlive())
			{
				const auto found = m_primitiveIndexByMeshRenderer.find(meshRenderer);
				if (found != m_primitiveIndexByMeshRenderer.end())
					TombstonePrimitive(found->second, stats);
				continue;
			}

			if (meshRenderer->IsTransientRenderingSuppressed())
			{
				const auto found = m_primitiveIndexByMeshRenderer.find(meshRenderer);
				if (found != m_primitiveIndexByMeshRenderer.end())
					TombstonePrimitive(found->second, stats);
				continue;
			}

			if (fastAccessMembershipChanged)
				liveMeshRenderers.emplace(meshRenderer, meshRenderer->GetInstanceID());
			auto& primitive = FindOrCreatePrimitive(*meshRenderer, stats);
			const auto inputStamp = BuildPrimitiveInputStamp(*meshRenderer, primitive, options);
			if (CanReuseSynchronizedPrimitive(primitive, inputStamp))
				continue;
			SynchronizePrimitive(primitive, options, stats, declaredTextureCache);
			primitive.lastInputStamp = BuildPrimitiveInputStamp(*meshRenderer, primitive, options);
		}
	}

	if (fastAccessMembershipChanged)
	{
		NLS_PROFILE_NAMED_SCOPE("RenderScene::Synchronize::RemoveMissingPrimitives");
		RemoveMissingPrimitives(liveMeshRenderers, stats);
	}
	m_lastSceneFastAccessRevision = fastAccessRevision;
	{
		NLS_PROFILE_NAMED_SCOPE("RenderScene::Synchronize::RefreshSpatialIndex");
		RefreshSpatialIndex(options);
	}
	{
		NLS_PROFILE_NAMED_SCOPE("RenderScene::Synchronize::RebuildImportedHierarchyHLODRecords");
		RebuildImportedHierarchyHLODRecords(scene);
	}
	m_lastSyncStats = stats;
	stats.syncTimeNs = ElapsedNanoseconds(syncStart);
	m_lastSyncStats.syncTimeNs = stats.syncTimeNs;
	RefreshSyncTelemetry(stats);
	return stats;
}

RenderSceneVisibleQueues RenderScene::GatherVisibleCommands(
	const RenderSceneVisibilityOptions& options,
	const RenderSceneVisibilityMode mode) const
{
	RenderSceneVisibleQueues output;
	m_lastDrawCallOptimizationStats = {};
	m_lastCullReasonDebugSnapshot.reset();
	m_lastVisiblePrimitiveHandles.clear();
	m_lastRepresentationStreamingInterest.clear();
	const auto commandOffsetStart = std::chrono::steady_clock::now();
	uint64_t commandOffsetTouchedPrimitiveCount = 0u;
	const auto& meshBaseIndices = GetMeshBaseIndices(&commandOffsetTouchedPrimitiveCount);
	const auto commandOffsetTimeNs = ElapsedNanoseconds(commandOffsetStart);
	const auto visibilityStart = std::chrono::steady_clock::now();
	const auto visibility = EvaluateVisibility(options, mode, meshBaseIndices, false);
	const auto visibilityTimeNs = ElapsedNanoseconds(visibilityStart);
	m_lastLargeSceneTelemetry.culledByReason = visibility.culledByReason;
	m_lastLargeSceneTelemetry.spatialCandidateCount = visibility.spatialCandidateCount;
	m_lastLargeSceneTelemetry.fullScanCandidateCount = visibility.fullScanCandidateCount;
	m_lastLargeSceneTelemetry.staticPrimitiveCount = 0u;
	m_lastLargeSceneTelemetry.dynamicPrimitiveCount = 0u;
	m_lastLargeSceneTelemetry.unclassifiedPrimitiveCount = static_cast<uint64_t>(m_livePrimitiveCount);
	if (ResolveLargeSceneSettings(options).enableSpatialIndex && m_spatialIndex != nullptr)
	{
		m_lastLargeSceneTelemetry.staticPrimitiveCount =
			static_cast<uint64_t>(m_spatialIndex->GetStaticPrimitiveCount());
		m_lastLargeSceneTelemetry.dynamicPrimitiveCount =
			static_cast<uint64_t>(m_spatialIndex->GetDynamicPrimitiveCount());
		m_lastLargeSceneTelemetry.unclassifiedPrimitiveCount = 0u;
	}
	m_lastLargeSceneTelemetry.dynamicCandidateCount = visibility.dynamicCandidateCount;
	m_lastLargeSceneTelemetry.dynamicRecordsTouched = visibility.dynamicRecordsTouched;
	m_lastLargeSceneTelemetry.staticIndexRefitCount = visibility.staticIndexRefitCount;
	m_lastLargeSceneTelemetry.staticIndexRebuildCount = visibility.staticIndexRebuildCount;
	m_lastLargeSceneTelemetry.staticIndexLastGoodQueryCount = visibility.staticIndexLastGoodQueryCount;
	m_lastLargeSceneTelemetry.staticIndexDirtyOverlayCount = visibility.staticIndexDirtyOverlayCount;
	m_lastLargeSceneTelemetry.spatialRebuildFallbackCount = visibility.spatialRebuildFallbackCount;
	m_lastLargeSceneTelemetry.dynamicIndexUpdateCount = visibility.dynamicIndexUpdateCount;
	m_lastLargeSceneTelemetry.primitiveRecordsTouched =
		commandOffsetTouchedPrimitiveCount + visibility.primitiveRecordsTouched;
	m_lastLargeSceneTelemetry.visibilityTestedPrimitiveCount = visibility.visibilityTestedPrimitiveCount;
	m_lastLargeSceneTelemetry.visibilityBitsetWordCount =
		static_cast<uint64_t>(visibility.primitiveBits.size() + visibility.meshBits.size());
	m_lastLargeSceneTelemetry.visiblePrimitiveCount = visibility.visiblePrimitiveCount;
	m_lastLargeSceneTelemetry.visibleMeshCount = visibility.visibleMeshCount;
	m_lastLargeSceneTelemetry.occlusionTestCount = visibility.occlusionTestCount;
	m_lastLargeSceneTelemetry.occlusionCulledCount = visibility.occlusionCulledCount;
	m_lastLargeSceneTelemetry.lodSelectionCount = visibility.lodSelectionCount;
	m_lastLargeSceneTelemetry.activeHLODClusterCount = visibility.activeHLODClusterCount;
	m_lastLargeSceneTelemetry.commandOffsetRebuildCount = commandOffsetTouchedPrimitiveCount > 0u ? 1u : 0u;
	m_lastLargeSceneTelemetry.serialVisibilityTimeNs = 0u;
	m_lastLargeSceneTelemetry.parallelVisibilityTimeNs = 0u;
	if (visibility.usedParallelEvaluation)
		m_lastLargeSceneTelemetry.parallelVisibilityTimeNs = visibilityTimeNs;
	else
		m_lastLargeSceneTelemetry.serialVisibilityTimeNs = visibilityTimeNs;
	m_lastLargeSceneTelemetry.rawVisibleDrawCount = 0u;
	m_lastLargeSceneTelemetry.submittedDrawCount = 0u;
	m_lastLargeSceneTelemetry.dynamicInstanceGroupCount = 0u;
	m_lastLargeSceneTelemetry.finalizationTouchedPrimitiveCount = 0u;
	m_lastLargeSceneTelemetry.finalizationTouchedCommandCount = 0u;
	const auto visibleMeshCount = static_cast<size_t>(visibility.visibleMeshCount);
	output.opaques.reserve(visibleMeshCount);
	output.transparents.reserve(visibleMeshCount);

	const auto finalizationStart = std::chrono::steady_clock::now();
	const auto finalizePrimitive = [&](const size_t primitiveIndex)
	{
		if (primitiveIndex >= m_primitives.size())
			return;

		const auto& primitive = m_primitives[primitiveIndex];
		if (!primitive.occupied || primitive.tombstoned)
			return;
		++m_lastLargeSceneTelemetry.finalizationTouchedPrimitiveCount;

		if (!visibility.usesSparseVisiblePrimitiveIndices &&
			!IsBitSet(visibility.primitiveBits, primitiveIndex))
			return;

		const auto meshBaseIndex = primitiveIndex < meshBaseIndices.size()
			? meshBaseIndices[primitiveIndex]
			: 0u;

		for (size_t slotIndex = 0u; slotIndex < primitive.cachedCommands.size(); ++slotIndex)
		{
			++m_lastLargeSceneTelemetry.finalizationTouchedCommandCount;
			const auto& slot = primitive.cachedCommands[slotIndex];
			if (!slot.valid || slot.command.mesh == nullptr || slot.command.material == nullptr)
				continue;

			const auto meshBitIndex = meshBaseIndex + slotIndex;
			if (visibility.meshBits.empty())
			{
				if (!IsMeshVisible(primitive, *slot.command.mesh, options))
					continue;
			}
			else if (!IsBitSet(visibility.meshBits, meshBitIndex))
			{
				continue;
			}

			AppendVisibleDrawable(output, primitive, slot.command, options);
			++m_lastDrawCallOptimizationStats.rawVisibleObjectCount;
			++m_lastLargeSceneTelemetry.rawVisibleDrawCount;
		}
	};

	if (visibility.usesSparseVisiblePrimitiveIndices)
	{
		for (const auto primitiveIndex : visibility.visiblePrimitiveIndices)
			finalizePrimitive(primitiveIndex);
	}
	else
	{
		for (size_t primitiveIndex = 0u; primitiveIndex < m_primitives.size(); ++primitiveIndex)
			finalizePrimitive(primitiveIndex);
	}

	FinalizeOpaqueQueue(output.opaques);
	SortVisibleQueue(output.decals, std::greater<float>{});
	SortVisibleQueue(output.transparents, std::greater<float>{});
	AssignVisibleObjectIndices(output);
	m_lastVisiblePrimitiveHandles = visibility.visiblePrimitiveHandles;
	m_lastRepresentationStreamingInterest = visibility.representationStreamingInterest;
	m_lastDrawCallOptimizationStats.submittedSceneDrawCount =
		static_cast<uint64_t>(
			output.opaques.size() +
			output.decals.size() +
			output.transparents.size() +
			output.skyboxes.size());
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
	m_lastLargeSceneTelemetry.submittedDrawCount = m_lastDrawCallOptimizationStats.submittedSceneDrawCount;
	m_lastLargeSceneTelemetry.dynamicInstanceGroupCount = m_lastDrawCallOptimizationStats.dynamicInstanceGroupCount;
	m_lastLargeSceneTelemetry.queueFinalizationTimeNs =
		commandOffsetTimeNs + ElapsedNanoseconds(finalizationStart);
	return output;
}

size_t RenderScene::GetPrimitiveCount() const
{
	return m_livePrimitiveCount;
}

uint64_t RenderScene::GetSceneId() const
{
	return m_sceneId;
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

const NLS::Render::Data::LargeSceneTelemetry& RenderScene::GetLastLargeSceneTelemetry() const
{
	return m_lastLargeSceneTelemetry;
}

const NLS::Render::Data::LargeSceneTelemetry& RenderScene::GetLastLargeSceneTelemetryForTesting() const
{
	return GetLastLargeSceneTelemetry();
}

std::shared_ptr<const SceneCullReasonDebugSnapshot> RenderScene::GetLastCullReasonDebugSnapshot() const
{
	return m_lastCullReasonDebugSnapshot;
}

const std::vector<ScenePrimitiveHandle>& RenderScene::GetLastVisiblePrimitiveHandles() const
{
	return m_lastVisiblePrimitiveHandles;
}

const std::vector<ScenePrimitiveHandle>& RenderScene::GetLastRemovedPrimitiveHandles() const
{
	return m_lastRemovedHandles;
}

std::vector<ScenePrimitiveHandle> RenderScene::GetLivePrimitiveHandles() const
{
	std::vector<ScenePrimitiveHandle> handles;
	handles.reserve(m_livePrimitiveCount);
	for (const auto& primitive : m_primitives)
	{
		if (primitive.occupied && !primitive.tombstoned)
			handles.push_back(primitive.handle);
	}
	return handles;
}

std::vector<ScenePickablePrimitiveDrawSource> RenderScene::CreatePickablePrimitiveDrawSourcesForHandles(
	const std::vector<ScenePrimitiveHandle>& handles) const
{
	std::vector<ScenePickablePrimitiveDrawSource> sources;
	sources.reserve(handles.size());
	AppendPickablePrimitiveDrawSourcesForHandles(handles, sources);
	return sources;
}

void RenderScene::AppendPickablePrimitiveDrawSourcesForHandles(
	const std::vector<ScenePrimitiveHandle>& handles,
	std::vector<ScenePickablePrimitiveDrawSource>& outSources) const
{
	outSources.reserve(outSources.size() + handles.size());
	for (const auto handle : handles)
	{
		if (handle.sceneId != m_sceneId || handle.index >= m_primitives.size())
			continue;

		const auto& primitive = m_primitives[handle.index];
		if (primitive.handle != handle ||
			!primitive.occupied ||
			primitive.tombstoned ||
			primitive.owner == nullptr ||
			!primitive.owner->IsAlive() ||
			!primitive.owner->IsActive() ||
			primitive.mesh == nullptr)
		{
			continue;
		}

		for (const auto& slot : primitive.cachedCommands)
		{
			if (!slot.valid || slot.command.mesh == nullptr || slot.command.material == nullptr)
				continue;

			outSources.push_back({
				primitive.owner,
				slot.command.mesh,
				primitive.worldMatrix,
				slot.command.stateMask
			});
		}
	}
}

const std::vector<ScenePrimitiveHandle>& RenderScene::GetLastRepresentationStreamingInterest() const
{
	return m_lastRepresentationStreamingInterest;
}

RenderSceneVisibilitySnapshot RenderScene::EvaluateVisibilityForTesting(
	const RenderSceneVisibilityOptions& options,
	const RenderSceneVisibilityMode mode) const
{
	return EvaluateVisibility(options, mode);
}

ScenePrimitiveSnapshot RenderScene::CreatePrimitiveSnapshotForTesting(const uint64_t frameSerial) const
{
	return CreatePrimitiveSnapshot(frameSerial);
}

bool RenderScene::IsPrimitiveHandleLiveForTesting(const ScenePrimitiveHandle handle) const
{
	return IsPrimitiveHandleLive(handle);
}

void RenderScene::ClearRepresentationRecords()
{
	m_representationRegistry = std::make_unique<RepresentationRegistry>();
	m_importedHierarchyHLODClusterHandles.clear();
	for (auto& primitive : m_primitives)
	{
		primitive.lodGroup.reset();
		primitive.hlodCluster.reset();
	}
}

void RenderScene::RebuildImportedHierarchyHLODRecords(const SceneSystem::Scene& scene)
{
	if (m_representationRegistry == nullptr)
		m_representationRegistry = std::make_unique<RepresentationRegistry>();

	for (auto& primitive : m_primitives)
	{
		if (!primitive.hlodCluster.has_value())
			continue;
		const auto found = std::find(
			m_importedHierarchyHLODClusterHandles.begin(),
			m_importedHierarchyHLODClusterHandles.end(),
			*primitive.hlodCluster);
		if (found != m_importedHierarchyHLODClusterHandles.end())
			primitive.hlodCluster.reset();
	}
	for (const auto handle : m_importedHierarchyHLODClusterHandles)
	{
		if (handle.index < m_representationRegistry->hlodClusters.size())
			m_representationRegistry->hlodClusters[handle.index] = {};
	}
	m_importedHierarchyHLODClusterHandles.clear();

	struct ImportedHierarchyHLODScope
	{
		const Engine::GameObject* root = nullptr;
		ImportedHierarchyHLODMetadata metadata;
		std::unordered_map<std::string, std::vector<ScenePrimitiveHandle>> primitivesBySourceKey;
	};

	std::vector<ImportedHierarchyHLODScope> scopes;
	for (const auto* gameObject : scene.GetGameObjects())
	{
		if (gameObject == nullptr || gameObject->GetLargeSceneHLODMetadata().empty())
			continue;

		const auto metadata = ParseImportedHierarchyHLODMetadata(
			gameObject->GetLargeSceneHLODMetadata());
		if (!metadata.has_value())
			continue;

		scopes.push_back({ gameObject, *metadata, {} });
	}

	std::unordered_map<const Engine::GameObject*, size_t> scopeIndicesByRoot;
	scopeIndicesByRoot.reserve(scopes.size());
	for (size_t index = 0u; index < scopes.size(); ++index)
	{
		scopeIndicesByRoot[scopes[index].root] = index;
		scopes[index].primitivesBySourceKey.reserve(m_livePrimitiveCount);
	}

	for (const auto& primitive : m_primitives)
	{
		if (!primitive.occupied || primitive.tombstoned || primitive.owner == nullptr)
			continue;
		const auto& sourceKey = primitive.owner->GetSourceObjectKey();
		if (sourceKey.empty())
			continue;

		for (const Engine::GameObject* ownerOrAncestor = primitive.owner;
			ownerOrAncestor != nullptr;
			ownerOrAncestor = ownerOrAncestor->GetParent())
		{
			const auto foundScope = scopeIndicesByRoot.find(ownerOrAncestor);
			if (foundScope != scopeIndicesByRoot.end())
				scopes[foundScope->second].primitivesBySourceKey[sourceKey].push_back(primitive.handle);
		}
	}

	for (const auto& scope : scopes)
	{
		const auto& metadata = scope.metadata;
		const auto& primitivesBySourceKey = scope.primitivesBySourceKey;
		HLODClusterRecord cluster;
		cluster.clusterHandle = {};
		cluster.activationScreenRelativeSize = 0.03f;
		cluster.compatibilityFlags = HLODCompatibilityFlags::OpaqueOnly | HLODCompatibilityFlags::ProxySafe;

		for (const auto& childKey : metadata.children)
		{
			const auto foundChildren = primitivesBySourceKey.find(childKey);
			if (foundChildren == primitivesBySourceKey.end())
				continue;
			for (const auto handle : foundChildren->second)
				cluster.childPrimitives.push_back(handle);
		}

		const auto foundProxy = primitivesBySourceKey.find(metadata.proxySubAssetKey);
		if (foundProxy != primitivesBySourceKey.end() && !foundProxy->second.empty())
			cluster.proxyPrimitive = foundProxy->second.front();

		if (cluster.childPrimitives.empty() || !cluster.proxyPrimitive.has_value())
			continue;

		std::optional<NLS::Render::Geometry::Bounds> clusterWorldBounds;
		for (const auto child : cluster.childPrimitives)
		{
			if (!IsPrimitiveHandleLive(child))
				continue;
			const auto& primitive = m_primitives[child.index];
			const auto childWorldBounds =
				NLS::Render::Geometry::TransformBounds(primitive.modelBounds, primitive.worldMatrix);
			if (clusterWorldBounds.has_value())
				clusterWorldBounds = NLS::Render::Geometry::UnionBounds(*clusterWorldBounds, childWorldBounds);
			else
				clusterWorldBounds = childWorldBounds;
		}
		if (!clusterWorldBounds.has_value())
			continue;
		cluster.worldReferencePoint = clusterWorldBounds->center;
		cluster.worldSize = std::max(
			std::max(clusterWorldBounds->size.x, clusterWorldBounds->size.y),
			std::max(clusterWorldBounds->size.z, 1.0f));

		const auto handle = RegisterHLODCluster(cluster);
		m_importedHierarchyHLODClusterHandles.push_back(handle);
	}

	m_representationRegistry->RebuildLookup();
}

SceneLODGroupHandle RenderScene::RegisterLODGroup(const LODGroupRecord& group)
{
	if (m_representationRegistry == nullptr)
		m_representationRegistry = std::make_unique<RepresentationRegistry>();

	auto record = group;
	if (!record.groupHandle.IsValid())
		record.groupHandle = { static_cast<uint32_t>(m_representationRegistry->lodGroups.size()) };

	const auto handle = record.groupHandle;
	if (handle.index >= m_representationRegistry->lodGroups.size())
		m_representationRegistry->lodGroups.resize(static_cast<size_t>(handle.index) + 1u);
	m_representationRegistry->lodGroups[handle.index] = record;
	m_representationRegistry->ResizeLODHistories();

	for (const auto& level : record.levels)
	{
		for (const auto primitiveHandle : level.primitiveHandles)
		{
			if (!IsPrimitiveHandleLive(primitiveHandle))
				continue;
			m_primitives[primitiveHandle.index].lodGroup = handle;
		}
	}
	m_representationRegistry->RebuildLookup();

	return handle;
}

SceneHLODClusterHandle RenderScene::RegisterHLODCluster(const HLODClusterRecord& cluster)
{
	if (m_representationRegistry == nullptr)
		m_representationRegistry = std::make_unique<RepresentationRegistry>();

	auto record = cluster;
	if (!record.clusterHandle.IsValid())
		record.clusterHandle = { static_cast<uint32_t>(m_representationRegistry->hlodClusters.size()) };

	const auto handle = record.clusterHandle;
	if (handle.index >= m_representationRegistry->hlodClusters.size())
		m_representationRegistry->hlodClusters.resize(static_cast<size_t>(handle.index) + 1u);
	m_representationRegistry->hlodClusters[handle.index] = record;

	for (const auto primitiveHandle : record.childPrimitives)
	{
		if (!IsPrimitiveHandleLive(primitiveHandle))
			continue;
		m_primitives[primitiveHandle.index].hlodCluster = handle;
	}
	if (record.proxyPrimitive.has_value() && IsPrimitiveHandleLive(*record.proxyPrimitive))
		m_primitives[record.proxyPrimitive->index].hlodCluster = handle;
	m_representationRegistry->RebuildLookup();

	return handle;
}

void RenderScene::ClearRepresentationRecordsForTesting()
{
	ClearRepresentationRecords();
}

SceneLODGroupHandle RenderScene::RegisterLODGroupForTesting(const LODGroupRecord& group)
{
	return RegisterLODGroup(group);
}

SceneHLODClusterHandle RenderScene::RegisterHLODClusterForTesting(const HLODClusterRecord& cluster)
{
	return RegisterHLODCluster(cluster);
}

ScenePrimitiveSnapshot RenderScene::CreatePrimitiveSnapshot(const uint64_t frameSerial) const
{
	ScenePrimitiveSnapshot snapshot;
	snapshot.snapshotSerial = m_nextPrimitiveSnapshotSerial++;
	snapshot.sceneId = m_sceneId;
	snapshot.frameSerial = frameSerial;
	snapshot.memoryArenaSerial = snapshot.snapshotSerial;
	snapshot.primitiveRecords.reserve(m_livePrimitiveCount);
	snapshot.handleToDenseIndex.reserve(m_livePrimitiveCount);
	snapshot.denseIndexToHandle.reserve(m_livePrimitiveCount);
	snapshot.dirtySyncHandles = m_lastDirtySyncHandles;
	snapshot.removedHandles = m_lastRemovedHandles;
	snapshot.liveHandleBits.resize(BitWordCount(m_primitives.size()));

	uint64_t commandOffset = 0u;
	for (const auto& primitive : m_primitives)
	{
		if (!primitive.occupied || primitive.tombstoned)
			continue;

		const auto denseIndex = static_cast<uint64_t>(snapshot.primitiveRecords.size());
		const auto commandOffsetBegin = commandOffset;
		commandOffset += static_cast<uint64_t>(primitive.cachedCommands.size());

		ScenePrimitiveSnapshotRecord record;
		record.handle = primitive.handle;
		record.mesh = primitive.mesh;
		record.modelBoundingSphere = primitive.modelBoundingSphere;
		record.modelBounds = primitive.modelBounds;
		record.worldMatrix = primitive.worldMatrix;
		record.ownerAlive = primitive.ownerAlive;
		record.ownerActive = primitive.ownerActive;
		if (primitive.meshRenderer != nullptr)
			record.userMatrix = primitive.meshRenderer->GetUserMatrix();
		record.frustumBehaviour = primitive.frustumBehaviour;
		record.visibilitySettings = primitive.visibilitySettings;
		record.lodGroup = primitive.lodGroup;
		record.hlodCluster = primitive.hlodCluster;
		record.commandOffsetBegin = commandOffsetBegin;
		record.commandOffsetEnd = commandOffset;
		record.hasMeshBinding = primitive.meshRenderer != nullptr;
		record.hasValidMaterial = std::any_of(
			primitive.cachedCommands.begin(),
			primitive.cachedCommands.end(),
			[](const CachedCommandSlot& slot)
			{
				return slot.valid && slot.command.material != nullptr;
			});
		record.depthWriteEligibleForOcclusion = std::any_of(
			primitive.cachedCommands.begin(),
			primitive.cachedCommands.end(),
			[](const CachedCommandSlot& slot)
			{
				return slot.valid &&
					slot.command.material != nullptr &&
					!slot.command.material->IsBlendable() &&
					slot.command.stateMask.depthWriting &&
					slot.command.stateMask.depthTest;
			});
		record.occupied = primitive.occupied;
		record.tombstoned = primitive.tombstoned;

		snapshot.primitiveRecords.push_back(record);
		snapshot.handleToDenseIndex.push_back({ primitive.handle, denseIndex });
		snapshot.denseIndexToHandle.push_back(primitive.handle);
		snapshot.commandOffsetTable.push_back({ primitive.handle, commandOffsetBegin, commandOffset });
		SetBit(snapshot.liveHandleBits, primitive.handle.index);
	}

	return snapshot;
}

ScenePrimitiveSnapshot RenderScene::CreatePrimitiveSnapshotForHandles(
	const std::vector<ScenePrimitiveHandle>& handles,
	const std::vector<ScenePrimitiveHandle>& removedHandles,
	const uint64_t frameSerial) const
{
	ScenePrimitiveSnapshot snapshot;
	snapshot.snapshotSerial = m_nextPrimitiveSnapshotSerial++;
	snapshot.sceneId = m_sceneId;
	snapshot.frameSerial = frameSerial;
	snapshot.memoryArenaSerial = snapshot.snapshotSerial;
	snapshot.primitiveRecords.reserve(handles.size());
	snapshot.handleToDenseIndex.reserve(handles.size());
	snapshot.denseIndexToHandle.reserve(handles.size());
	snapshot.dirtySyncHandles = handles;
	snapshot.removedHandles = removedHandles;
	snapshot.liveHandleBits.resize(BitWordCount(m_primitives.size()));

	std::vector<size_t> localMeshBaseIndices;
	const auto* meshBaseIndices = &m_cachedMeshBaseIndices;
	if (m_commandOffsetTableDirty)
	{
		localMeshBaseIndices.reserve(m_primitives.size() + 1u);
		size_t meshCount = 0u;
		for (const auto& primitive : m_primitives)
		{
			localMeshBaseIndices.push_back(meshCount);
			if (primitive.occupied && !primitive.tombstoned)
				meshCount += primitive.cachedCommands.size();
		}
		localMeshBaseIndices.push_back(meshCount);
		meshBaseIndices = &localMeshBaseIndices;
	}

	for (const auto& handle : handles)
	{
		if (handle.sceneId != m_sceneId || handle.index >= m_primitives.size())
			continue;

		const auto& primitive = m_primitives[handle.index];
		if (primitive.handle != handle || !primitive.occupied || primitive.tombstoned)
			continue;

		ScenePrimitiveSnapshotRecord record;
		record.handle = primitive.handle;
		record.mesh = primitive.mesh;
		record.modelBoundingSphere = primitive.modelBoundingSphere;
		record.modelBounds = primitive.modelBounds;
		record.worldMatrix = primitive.worldMatrix;
		record.ownerAlive = primitive.ownerAlive;
		record.ownerActive = primitive.ownerActive;
		if (primitive.meshRenderer != nullptr)
			record.userMatrix = primitive.meshRenderer->GetUserMatrix();
		record.frustumBehaviour = primitive.frustumBehaviour;
		record.visibilitySettings = primitive.visibilitySettings;
		record.lodGroup = primitive.lodGroup;
		record.hlodCluster = primitive.hlodCluster;
		record.commandOffsetBegin = handle.index < meshBaseIndices->size()
			? static_cast<uint64_t>((*meshBaseIndices)[handle.index])
			: 0u;
		record.commandOffsetEnd = record.commandOffsetBegin + static_cast<uint64_t>(primitive.cachedCommands.size());
		record.hasMeshBinding = primitive.meshRenderer != nullptr;
		record.hasValidMaterial = std::any_of(
			primitive.cachedCommands.begin(),
			primitive.cachedCommands.end(),
			[](const CachedCommandSlot& slot)
			{
				return slot.valid && slot.command.material != nullptr;
			});
		record.depthWriteEligibleForOcclusion = std::any_of(
			primitive.cachedCommands.begin(),
			primitive.cachedCommands.end(),
			[](const CachedCommandSlot& slot)
			{
				return slot.valid &&
					slot.command.material != nullptr &&
					!slot.command.material->IsBlendable() &&
					slot.command.stateMask.depthWriting &&
					slot.command.stateMask.depthTest;
			});
		record.occupied = primitive.occupied;
		record.tombstoned = primitive.tombstoned;

		const auto denseIndex = static_cast<uint64_t>(snapshot.primitiveRecords.size());
		snapshot.primitiveRecords.push_back(record);
		snapshot.handleToDenseIndex.push_back({ primitive.handle, denseIndex });
		snapshot.denseIndexToHandle.push_back(primitive.handle);
		snapshot.commandOffsetTable.push_back({ primitive.handle, record.commandOffsetBegin, record.commandOffsetEnd });
		SetBit(snapshot.liveHandleBits, primitive.handle.index);
	}

	return snapshot;
}

bool RenderScene::IsPrimitiveHandleLive(const ScenePrimitiveHandle handle) const
{
	if (handle.sceneId != m_sceneId || handle.index >= m_primitives.size())
		return false;

	const auto& primitive = m_primitives[handle.index];
	return primitive.occupied &&
		!primitive.tombstoned &&
		primitive.handle == handle;
}

RenderScene::RenderPrimitive& RenderScene::AllocatePrimitiveSlot(
	Components::MeshRenderer& meshRenderer,
	RenderSceneSyncStats& stats)
{
	if (m_firstFreePrimitiveSlot.has_value())
	{
		const auto primitiveIndex = m_firstFreePrimitiveSlot.value();
		auto& primitive = m_primitives[primitiveIndex];
		m_firstFreePrimitiveSlot = primitive.nextFreePrimitiveSlot;
		const auto generation = IncrementPrimitiveGeneration(primitive.handle.generation);
		primitive = {};
		primitive.handle = {
			m_sceneId,
			static_cast<uint32_t>(primitiveIndex),
			generation
		};
		primitive.meshRenderer = &meshRenderer;
		primitive.meshRendererInstanceId = meshRenderer.GetInstanceID();
		primitive.frustumBehaviour = meshRenderer.GetFrustumBehaviour();
		primitive.occupied = true;
		primitive.tombstoned = false;
		++m_livePrimitiveCount;
		++stats.primitiveSlotReuseCount;
		MarkCommandOffsetTableDirty();
		return primitive;
	}

	const auto primitiveIndex = m_primitives.size();
	RenderPrimitive primitive;
	primitive.handle = {
		m_sceneId,
		static_cast<uint32_t>(primitiveIndex),
		1u
	};
	primitive.meshRenderer = &meshRenderer;
	primitive.meshRendererInstanceId = meshRenderer.GetInstanceID();
	primitive.frustumBehaviour = meshRenderer.GetFrustumBehaviour();
	primitive.occupied = true;
	primitive.tombstoned = false;
	m_primitives.push_back(std::move(primitive));
	++m_livePrimitiveCount;
	MarkCommandOffsetTableDirty();
	return m_primitives.back();
}

RenderScene::RenderPrimitive& RenderScene::FindOrCreatePrimitive(
	Components::MeshRenderer& meshRenderer,
	RenderSceneSyncStats& stats)
{
	const auto found = m_primitiveIndexByMeshRenderer.find(&meshRenderer);
	if (found != m_primitiveIndexByMeshRenderer.end())
	{
		const auto primitiveIndex = found->second;
		if (primitiveIndex < m_primitives.size())
		{
			auto& primitive = m_primitives[primitiveIndex];
			if (primitive.occupied &&
				!primitive.tombstoned &&
				primitive.meshRenderer == &meshRenderer &&
				primitive.meshRendererInstanceId == meshRenderer.GetInstanceID())
			{
				++stats.reusedPrimitiveCount;
				return primitive;
			}
			if (primitive.occupied &&
				!primitive.tombstoned &&
				primitive.meshRenderer == &meshRenderer)
			{
				TombstonePrimitive(primitiveIndex, stats);
				m_primitiveIndexByMeshRenderer.erase(&meshRenderer);
			}
			else
			{
				m_primitiveIndexByMeshRenderer.erase(found);
			}
		}
		else
		{
			m_primitiveIndexByMeshRenderer.erase(found);
		}
	}

	auto& primitive = AllocatePrimitiveSlot(meshRenderer, stats);
	m_primitiveIndexByMeshRenderer[&meshRenderer] = primitive.handle.index;
	m_lastDirtySyncHandles.push_back(primitive.handle);
	++stats.addedPrimitiveCount;
	return primitive;
}

void RenderScene::TombstonePrimitive(const size_t primitiveIndex, RenderSceneSyncStats& stats)
{
	if (primitiveIndex >= m_primitives.size())
		return;

	auto& primitive = m_primitives[primitiveIndex];
	if (!primitive.occupied || primitive.tombstoned)
		return;

	if (primitive.meshRenderer != nullptr)
	{
		const auto found = m_primitiveIndexByMeshRenderer.find(primitive.meshRenderer);
		if (found != m_primitiveIndexByMeshRenderer.end() && found->second == primitiveIndex)
			m_primitiveIndexByMeshRenderer.erase(found);
	}

	m_lastRemovedHandles.push_back(primitive.handle);
	primitive.meshRenderer = nullptr;
	primitive.meshRendererInstanceId = NLS::InstanceID_None;
	primitive.owner = nullptr;
	primitive.mesh = nullptr;
	if (!primitive.cachedCommands.empty())
		MarkCommandOffsetTableDirty();
	primitive.cachedCommands.clear();
	primitive.occupied = false;
	primitive.tombstoned = true;
	primitive.nextFreePrimitiveSlot = m_firstFreePrimitiveSlot;
	m_firstFreePrimitiveSlot = primitiveIndex;
	--m_livePrimitiveCount;
	++stats.removedPrimitiveCount;
}

void RenderScene::MarkPrimitiveDirtyForSnapshot(const RenderPrimitive& primitive)
{
	if (!primitive.occupied || primitive.tombstoned)
		return;
	if (std::find(m_lastDirtySyncHandles.begin(), m_lastDirtySyncHandles.end(), primitive.handle) ==
		m_lastDirtySyncHandles.end())
	{
		m_lastDirtySyncHandles.push_back(primitive.handle);
	}
}

void RenderScene::SynchronizePrimitive(
	RenderPrimitive& primitive,
	const RenderSceneSyncOptions& options,
	RenderSceneSyncStats& stats,
	RenderSceneDeclaredTextureLookupCache& declaredTextureCache)
{
	if (!primitive.occupied || primitive.tombstoned)
		return;
	++stats.syncTouchedPrimitiveCount;

	auto* meshRenderer = primitive.meshRenderer;
	if (meshRenderer == nullptr)
		return;

	const auto previousCommandCount = primitive.cachedCommands.size();
	const auto* previousOwner = primitive.owner;
	const auto* previousMesh = primitive.mesh;
	const auto previousBounds = primitive.modelBoundingSphere;
	const auto previousModelBounds = primitive.modelBounds;
	const auto previousWorldMatrix = primitive.worldMatrix;
	const auto previousFrustumBehaviour = primitive.frustumBehaviour;
	const auto previousVisibilitySettings = primitive.visibilitySettings;
	const bool previousOwnerAlive = primitive.ownerAlive;
	const bool previousOwnerActive = primitive.ownerActive;
	primitive.owner = meshRenderer->gameobject();
	primitive.ownerAlive = primitive.owner != nullptr && primitive.owner->IsAlive();
	primitive.ownerActive = primitive.owner != nullptr && primitive.owner->IsActive();
	if (auto* transform = primitive.owner != nullptr ? primitive.owner->GetTransform() : nullptr)
		primitive.worldMatrix = transform->GetWorldMatrix();
	else
		primitive.worldMatrix = Maths::Matrix4::Identity;
	auto* meshFilter = primitive.owner != nullptr
		? primitive.owner->GetComponent<Components::MeshFilter>()
		: nullptr;
	primitive.meshFilter = meshFilter;
	auto* resolvedMesh = meshFilter != nullptr ? meshFilter->ResolveMesh() : nullptr;
	primitive.mesh = resolvedMesh;
	primitive.frustumBehaviour = meshRenderer->GetFrustumBehaviour();
	primitive.transientRenderingSuppressed = meshRenderer->IsTransientRenderingSuppressed();
	const auto layer = primitive.owner != nullptr ? primitive.owner->GetLayer() : 0;
	primitive.visibilitySettings.layer = static_cast<uint32_t>(std::clamp(layer, 0, 31));
	primitive.visibilitySettings.distanceCullingEnabled = false;
	primitive.visibilitySettings.minDrawDistance = 0.0f;
	primitive.visibilitySettings.maxDrawDistance = 0.0f;

	if (primitive.mesh == nullptr)
	{
		const bool becameMissing = previousMesh != nullptr || !primitive.cachedCommands.empty();
		if (!primitive.cachedCommands.empty())
			MarkCommandOffsetTableDirty();
		primitive.cachedCommands.clear();
		if (becameMissing ||
			previousOwner != primitive.owner ||
			previousOwnerAlive != primitive.ownerAlive ||
			previousOwnerActive != primitive.ownerActive ||
			previousVisibilitySettings.layer != primitive.visibilitySettings.layer ||
			!AreSameMatrix(previousWorldMatrix, primitive.worldMatrix))
		{
			MarkPrimitiveDirtyForSnapshot(primitive);
		}
		return;
	}

	primitive.modelBoundingSphere =
		primitive.frustumBehaviour == Components::MeshRenderer::EFrustumBehaviour::CULL_CUSTOM
			? meshRenderer->GetCustomBoundingSphere()
			: primitive.mesh->GetBoundingSphere();
	primitive.modelBounds = primitive.mesh->GetBounds();
	if (!AreSameBounds(previousBounds, primitive.modelBoundingSphere) ||
		!AreSameBounds(previousModelBounds, primitive.modelBounds) ||
		previousFrustumBehaviour != primitive.frustumBehaviour ||
		previousOwnerAlive != primitive.ownerAlive ||
		previousOwnerActive != primitive.ownerActive ||
		!AreSameMatrix(previousWorldMatrix, primitive.worldMatrix))
	{
		++stats.boundsDirtyPrimitiveCount;
	}

	if (previousCommandCount != 1u)
		MarkCommandOffsetTableDirty();
	primitive.cachedCommands.resize(1u);
	auto* material = ResolveMaterialForMesh(primitive, *primitive.mesh, options, stats, declaredTextureCache);
	if (material == nullptr || !material->IsValid())
	{
		const bool wasValid = primitive.cachedCommands[0].valid;
		primitive.cachedCommands[0].valid = false;
		if (wasValid ||
			previousOwner != primitive.owner ||
			previousMesh != primitive.mesh ||
			previousFrustumBehaviour != primitive.frustumBehaviour ||
			previousOwnerAlive != primitive.ownerAlive ||
			previousOwnerActive != primitive.ownerActive ||
			previousVisibilitySettings.layer != primitive.visibilitySettings.layer ||
			!AreSameBounds(previousBounds, primitive.modelBoundingSphere) ||
			!AreSameBounds(previousModelBounds, primitive.modelBounds) ||
			!AreSameMatrix(previousWorldMatrix, primitive.worldMatrix))
		{
			MarkPrimitiveDirtyForSnapshot(primitive);
		}
		return;
	}

	auto stamp = BuildCommandInputStamp(primitive, *primitive.mesh, *material);
	auto& slot = primitive.cachedCommands[0];
	const bool commandInputChanged = !slot.valid || slot.stamp != stamp;
	if (!slot.valid || slot.stamp != stamp)
		RebuildCachedCommand(slot, stamp, stats);
	if (previousOwner != primitive.owner ||
		previousMesh != primitive.mesh ||
		previousFrustumBehaviour != primitive.frustumBehaviour ||
		previousOwnerAlive != primitive.ownerAlive ||
		previousOwnerActive != primitive.ownerActive ||
		previousVisibilitySettings.layer != primitive.visibilitySettings.layer ||
		previousVisibilitySettings.distanceCullingEnabled != primitive.visibilitySettings.distanceCullingEnabled ||
		previousVisibilitySettings.minDrawDistance != primitive.visibilitySettings.minDrawDistance ||
		previousVisibilitySettings.maxDrawDistance != primitive.visibilitySettings.maxDrawDistance ||
		!AreSameBounds(previousBounds, primitive.modelBoundingSphere) ||
		!AreSameBounds(previousModelBounds, primitive.modelBounds) ||
		!AreSameMatrix(previousWorldMatrix, primitive.worldMatrix) ||
		commandInputChanged)
	{
		MarkPrimitiveDirtyForSnapshot(primitive);
	}
}

RenderScene::PrimitiveInputStamp RenderScene::BuildPrimitiveInputStamp(
	Components::MeshRenderer& meshRenderer,
	const RenderPrimitive& primitive,
	const RenderSceneSyncOptions& options) const
{
	PrimitiveInputStamp stamp;
	stamp.owner = meshRenderer.gameobject();
	stamp.ownerAlive = stamp.owner != nullptr && stamp.owner->IsAlive();
	stamp.ownerActive = stamp.owner != nullptr && stamp.owner->IsActive();
	const auto layer = stamp.owner != nullptr ? stamp.owner->GetLayer() : 0;
	stamp.layer = static_cast<uint32_t>(std::clamp(layer, 0, 31));
	stamp.ownerRenderRevision = stamp.owner != nullptr ? stamp.owner->GetRenderStateRevision() : 0u;
	if (auto* transform = stamp.owner != nullptr ? stamp.owner->GetTransform() : nullptr)
	{
		stamp.transformRenderRevision = transform->GetRenderRevision();
		stamp.worldMatrix = transform->GetWorldMatrix();
	}
	stamp.meshFilter = stamp.owner != nullptr ? stamp.owner->GetComponent<Components::MeshFilter>() : nullptr;
	stamp.meshFilterRenderRevision = stamp.meshFilter != nullptr ? stamp.meshFilter->GetRenderRevision() : 0u;
	stamp.meshRendererRenderRevision = meshRenderer.GetRenderRevision();
		stamp.mesh = stamp.meshFilter != nullptr ? stamp.meshFilter->ResolveMesh() : primitive.mesh;
		stamp.meshContentRevision = stamp.mesh != nullptr ? stamp.mesh->GetContentRevision() : 0u;
		stamp.requireExplicitMaterialTextures = options.requireExplicitMaterialTextures;
		stamp.allowDefaultMaterialForUnresolvedExplicitMaterials =
			options.allowDefaultMaterialForUnresolvedExplicitMaterials;

	NLS::Render::Resources::Material* material = nullptr;
	if (options.overrideMaterial != nullptr && options.overrideMaterial->IsValid())
	{
		material = options.overrideMaterial;
	}
		else if (stamp.mesh != nullptr)
		{
			material = meshRenderer.ResolveMaterialAtIndex(stamp.mesh->GetMaterialIndex());
		}
	if (material == nullptr && options.defaultMaterial != nullptr && options.defaultMaterial->IsValid())
		material = options.defaultMaterial;

	if (material != nullptr)
	{
		stamp.materialInstanceId = material->GetInstanceId();
		stamp.materialParameterRevision = material->GetParameterRevision();
		stamp.materialRenderStateRevision = material->GetRenderStateRevision();
			if (!material->GetTextureResourcePaths().empty())
				stamp.explicitMaterialTexturesResolved = HasResolvedDeclaredMaterialTextures(*material);
		}

	return stamp;
}

bool RenderScene::CanReuseSynchronizedPrimitive(
	const RenderPrimitive& primitive,
	const PrimitiveInputStamp& stamp) const
{
	if (!primitive.occupied || primitive.tombstoned)
		return false;
	if (primitive.owner == nullptr ||
		primitive.meshRenderer == nullptr ||
		primitive.mesh == nullptr ||
		primitive.cachedCommands.empty())
	{
		return false;
	}
	for (const auto& slot : primitive.cachedCommands)
	{
		if (!slot.valid ||
			slot.stamp.mesh != stamp.mesh ||
			slot.stamp.material == nullptr ||
			slot.stamp.material != slot.command.material ||
			slot.stamp.materialInstanceId != stamp.materialInstanceId ||
			slot.stamp.materialParameterRevision != stamp.materialParameterRevision ||
			slot.stamp.materialRenderStateRevision != stamp.materialRenderStateRevision)
		{
			return false;
		}
	}
	for (const auto& slot : primitive.cachedCommands)
	{
		auto* material = slot.stamp.material;
		if (material != nullptr &&
			!material->GetTextureResourcePaths().empty() &&
			!HasResolvedDeclaredMaterialTextures(*material))
		{
			return false;
		}
	}
	if (stamp.requireExplicitMaterialTextures || !stamp.explicitMaterialTexturesResolved)
		return stamp.explicitMaterialTexturesResolved && primitive.lastInputStamp == stamp;
	return primitive.lastInputStamp == stamp;
}

void RenderScene::RemoveMissingPrimitives(
	const std::unordered_map<Components::MeshRenderer*, NLS::InstanceID>& liveMeshRenderers,
	RenderSceneSyncStats& stats)
{
	++stats.syncFullSweepCount;
	for (size_t primitiveIndex = 0u; primitiveIndex < m_primitives.size(); ++primitiveIndex)
	{
		++stats.syncSweepTouchedSlotCount;
		const auto& primitive = m_primitives[primitiveIndex];
		if (!primitive.occupied || primitive.tombstoned)
			continue;
		if (primitive.meshRenderer == nullptr)
		{
			TombstonePrimitive(primitiveIndex, stats);
			continue;
		}

		const auto live = liveMeshRenderers.find(primitive.meshRenderer);
		if (live == liveMeshRenderers.end() || live->second != primitive.meshRendererInstanceId)
			TombstonePrimitive(primitiveIndex, stats);
	}
}

NLS::Render::Resources::Material* RenderScene::ResolveMaterialForMesh(
	RenderPrimitive& primitive,
	NLS::Render::Resources::Mesh& mesh,
	const RenderSceneSyncOptions& options,
	RenderSceneSyncStats& stats,
	RenderSceneDeclaredTextureLookupCache& declaredTextureCache) const
{
	if (options.overrideMaterial != nullptr && options.overrideMaterial->IsValid())
		return options.overrideMaterial;

	if (primitive.meshRenderer != nullptr)
	{
		const auto materialPaths = primitive.meshRenderer->GetMaterialPaths();
		const bool hasExplicitMaterialPath = mesh.GetMaterialIndex() < materialPaths.size() &&
			!materialPaths[mesh.GetMaterialIndex()].empty();

		if (auto* material = primitive.meshRenderer->ResolveMaterialAtIndex(mesh.GetMaterialIndex());
			material != nullptr && material->IsValid())
		{
			if (hasExplicitMaterialPath)
				TryBindDeclaredMaterialTexturesFromCache(*material, declaredTextureCache, stats);
			if (options.requireExplicitMaterialTextures &&
				hasExplicitMaterialPath &&
				!HasResolvedDeclaredMaterialTextures(*material))
			{
				return nullptr;
			}
			return material;
		}

			if (hasExplicitMaterialPath && !options.allowDefaultMaterialForUnresolvedExplicitMaterials)
				return nullptr;
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
	if (!primitive.occupied || primitive.tombstoned)
		return false;
	if (primitive.owner == nullptr || !primitive.owner->IsAlive() || !primitive.owner->IsActive())
		return false;
	if (primitive.transientRenderingSuppressed)
		return false;
	if (primitive.mesh == nullptr)
		return false;
	if ((options.visibleLayerMask & (1u << primitive.visibilitySettings.layer)) == 0u)
		return false;
	if (options.frustum == nullptr)
		return true;
	if (primitive.frustumBehaviour == Components::MeshRenderer::EFrustumBehaviour::DISABLED)
		return true;

	auto* transform = primitive.owner->GetTransform();
	if (transform == nullptr)
		return false;

	return options.frustum->BoundsInFrustum(
		primitive.modelBounds,
		primitive.worldMatrix);
}

bool RenderScene::IsMeshVisible(
	const RenderPrimitive& primitive,
	const NLS::Render::Resources::Mesh& mesh,
	const RenderSceneVisibilityOptions& options) const
{
	if (!primitive.occupied || primitive.tombstoned)
		return false;
	if (options.frustum == nullptr ||
		primitive.frustumBehaviour != Components::MeshRenderer::EFrustumBehaviour::CULL_MESHES)
	{
		return true;
	}

	auto* transform = primitive.owner != nullptr ? primitive.owner->GetTransform() : nullptr;
	return transform != nullptr && options.frustum->IsMeshInFrustum(mesh, transform->GetTransform());
}

const std::vector<size_t>& RenderScene::GetMeshBaseIndices(uint64_t* touchedPrimitiveCount) const
{
	if (!m_commandOffsetTableDirty)
	{
		if (touchedPrimitiveCount != nullptr)
			*touchedPrimitiveCount = 0u;
		return m_cachedMeshBaseIndices;
	}

	m_cachedMeshBaseIndices.clear();
	m_cachedMeshBaseIndices.reserve(m_primitives.size() + 1u);

	size_t meshCount = 0u;
	for (const auto& primitive : m_primitives)
	{
		m_cachedMeshBaseIndices.push_back(meshCount);
		if (primitive.occupied && !primitive.tombstoned)
		{
			if (touchedPrimitiveCount != nullptr)
				++*touchedPrimitiveCount;
			meshCount += primitive.cachedCommands.size();
		}
	}
	m_cachedMeshBaseIndices.push_back(meshCount);
	m_commandOffsetTableDirty = false;

	return m_cachedMeshBaseIndices;
}

void RenderScene::MarkCommandOffsetTableDirty() noexcept
{
	m_commandOffsetTableDirty = true;
}

void RenderScene::RefreshSpatialIndex(const RenderSceneSyncOptions& options)
{
	const auto& settings = ResolveLargeSceneSettings(options);
	if (!settings.enableSpatialIndex)
		return;

	if (m_spatialIndex == nullptr)
		m_spatialIndex = std::make_unique<SceneSpatialIndex>();

	SceneSpatialIndexUpdateOptions updateOptions;
	updateOptions.staticRebuildDirtyRatio = settings.staticRebuildDirtyRatio;
	updateOptions.rebuildBudgetUs = settings.staticRebuildBudgetUs;
	const auto buildDirtyMetadata = [this]()
	{
		std::vector<SceneSpatialIndexPrimitiveMetadata> metadata;
		metadata.reserve(m_lastDirtySyncHandles.size());
		for (const auto& handle : m_lastDirtySyncHandles)
		{
			SceneSpatialIndexPrimitiveMetadata entry;
			entry.handle = handle;
			entry.primitiveClass = SceneSpatialIndexPrimitiveClass::Dynamic;
			metadata.push_back(entry);
		}
		return metadata;
	};
	if (!m_spatialIndex->IsInitialized())
	{
		m_spatialIndex->Update(CreatePrimitiveSnapshot(), {}, updateOptions);
		return;
	}

	m_spatialIndex->UpdateChanged(
		CreatePrimitiveSnapshotForHandles(m_lastDirtySyncHandles, m_lastRemovedHandles),
		buildDirtyMetadata(),
		updateOptions);
}

namespace
{
	void ApplySpatialIndexUpdateTelemetry(
		NLS::Render::Data::LargeSceneTelemetry& target,
		const SceneSpatialIndex& spatialIndex)
	{
		const auto updateTelemetry = spatialIndex.GetLastUpdateTelemetry();
		target.staticIndexRefitCount = updateTelemetry.staticIndexRefitCount;
		target.staticIndexRebuildCount = updateTelemetry.staticIndexRebuildCount;
		target.staticIndexLastGoodQueryCount = updateTelemetry.staticIndexLastGoodQueryCount;
		target.staticIndexDirtyOverlayCount = updateTelemetry.staticIndexDirtyOverlayCount;
		target.spatialRebuildFallbackCount = updateTelemetry.spatialRebuildFallbackCount;
		target.dynamicIndexUpdateCount = updateTelemetry.dynamicIndexUpdateCount;
	}

	void CopyCullReasonCounts(
		RenderSceneVisibilitySnapshot& target,
		const SceneVisibilityPipelineResult& source)
	{
		for (const auto reason : source.cullReasons)
		{
			const auto reasonIndex = static_cast<size_t>(reason);
			if (reasonIndex < target.culledByReason.size())
				++target.culledByReason[reasonIndex];
		}
		for (const auto selectedLOD : source.selectedLOD)
		{
			if (selectedLOD < target.lodSelectionCount.size())
				++target.lodSelectionCount[selectedLOD];
		}
		target.activeHLODClusterCount = static_cast<uint64_t>(source.activeHLODClusters.size());
	}
}

RenderSceneVisibilitySnapshot RenderScene::EvaluateVisibility(
	const RenderSceneVisibilityOptions& options,
	const RenderSceneVisibilityMode mode) const
{
	const auto& meshBaseIndices = GetMeshBaseIndices();
	return EvaluateVisibility(options, mode, meshBaseIndices);
}

RenderSceneVisibilitySnapshot RenderScene::EvaluateVisibility(
	const RenderSceneVisibilityOptions& options,
	const RenderSceneVisibilityMode mode,
	const std::vector<size_t>& meshBaseIndices,
	const bool buildVisibilityBitsets) const
{
	const auto& settings = ResolveLargeSceneSettings(options);
	const bool occlusionEnabled =
		settings.enableHZBOcclusion && options.occlusion != nullptr;
	const bool representationEnabled =
		(settings.enableLOD && m_representationRegistry != nullptr && m_representationRegistry->HasLOD()) ||
		(settings.enableHLOD && m_representationRegistry != nullptr && m_representationRegistry->HasHLOD());
	if (settings.enableSpatialIndex && m_spatialIndex != nullptr)
		return EvaluateVisibilitySpatial(options, meshBaseIndices, mode, buildVisibilityBitsets);
	if (representationEnabled || occlusionEnabled || options.enableCullReasonDebugSnapshot)
	{
		return EvaluateVisibilityThroughPipeline(
			options,
			meshBaseIndices,
			mode == RenderSceneVisibilityMode::Parallel
				? SceneVisibilityPipelineMode::Parallel
				: SceneVisibilityPipelineMode::Serial,
			buildVisibilityBitsets);
	}

	if (mode == RenderSceneVisibilityMode::Parallel)
	{
		return EvaluateVisibilityParallel(options, meshBaseIndices);
	}
	if (mode == RenderSceneVisibilityMode::Auto &&
		settings.enableParallelVisibility &&
		m_livePrimitiveCount >= settings.parallelVisibilityPrimitiveThreshold &&
		NLS::Base::Jobs::IsJobSystemInitialized())
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
		if (!primitive.occupied || primitive.tombstoned)
			continue;
		++snapshot.fullScanCandidateCount;
		++snapshot.primitiveRecordsTouched;
		++snapshot.visibilityTestedPrimitiveCount;
		if (!IsPrimitiveVisible(primitive, options))
			continue;

		SetBit(snapshot.primitiveBits, primitiveIndex);
		snapshot.visiblePrimitiveHandles.push_back(primitive.handle);
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
		if (!primitive.occupied || primitive.tombstoned)
			continue;
		++snapshot.fullScanCandidateCount;
		++snapshot.primitiveRecordsTouched;
		++snapshot.visibilityTestedPrimitiveCount;
		if (!IsPrimitiveVisible(primitive, options))
			continue;

		SetBit(snapshot.primitiveBits, primitiveIndex - snapshot.primitiveBegin);
		snapshot.visiblePrimitiveHandles.push_back(primitive.handle);
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

RenderSceneVisibilitySnapshot RenderScene::EvaluateVisibilityThroughPipeline(
	const RenderSceneVisibilityOptions& options,
	const std::vector<size_t>& meshBaseIndices,
	const SceneVisibilityPipelineMode mode,
	const bool buildVisibilityBitsets) const
{
	RenderSceneVisibilitySnapshot snapshot;
	snapshot.usesSparseVisiblePrimitiveIndices = true;
	snapshot.primitiveCount = static_cast<uint64_t>(m_primitives.size());
	snapshot.meshCount = !meshBaseIndices.empty()
		? static_cast<uint64_t>(meshBaseIndices.back())
		: 0u;
	if (buildVisibilityBitsets)
	{
		snapshot.primitiveBits.resize(BitWordCount(static_cast<size_t>(snapshot.primitiveCount)));
		snapshot.meshBits.resize(BitWordCount(static_cast<size_t>(snapshot.meshCount)));
	}

	auto primitiveSnapshot = CreatePrimitiveSnapshot();
	auto residency = BuildRepresentationResidencySnapshot(
		primitiveSnapshot,
		options.representationResidency);

	const auto& settings = ResolveLargeSceneSettings(options);
	SceneVisibilityPipelineOptions pipelineOptions;
	pipelineOptions.frustum = options.frustum;
	pipelineOptions.cameraPosition = options.cameraPosition;
	pipelineOptions.visibleLayerMask = options.visibleLayerMask;
	pipelineOptions.enableSpatialIndex = false;
	pipelineOptions.enableLOD = settings.enableLOD;
	pipelineOptions.enableHLOD = settings.enableHLOD;
	pipelineOptions.enableOcclusion = settings.enableHZBOcclusion && options.occlusion != nullptr;
	pipelineOptions.enableParallelVisibility = settings.enableParallelVisibility;
	pipelineOptions.parallelVisibilityPrimitiveThreshold = settings.parallelVisibilityPrimitiveThreshold;
	pipelineOptions.parallelVisibilityPrimitivesPerTask = settings.parallelVisibilityPrimitivesPerTask;
	pipelineOptions.maxVisibilityJobs = settings.maxVisibilityJobs;
	pipelineOptions.lodBias = options.lodBias;
	pipelineOptions.lodHistoryViewKey = options.lodHistoryViewKey;
	pipelineOptions.allowHLOD = options.allowHLOD;
	pipelineOptions.editorInspectionView = options.editorInspectionView;
	pipelineOptions.selectedPrimitiveHandles = options.selectedPrimitiveHandles;
	pipelineOptions.forceInspectableHLODClusters = ResolveInspectableHLODClusters(options);

	SceneRepresentationState representation;
	if (m_representationRegistry != nullptr)
	{
		representation.lodGroups = &m_representationRegistry->lodGroups;
		representation.hlodClusters = &m_representationRegistry->hlodClusters;
		representation.lodGroupsByPrimitive = &m_representationRegistry->lodGroupsByPrimitive;
		representation.hlodClustersByPrimitive = &m_representationRegistry->hlodClustersByPrimitive;
		representation.lodSelectionHistory =
			&m_representationRegistry->LODHistoryForView(options.lodHistoryViewKey);
		MarkHLODProxyResidency(
			residency,
			primitiveSnapshot,
			m_representationRegistry->hlodClusters);
	}
	representation.residency = &residency;
	representation.occlusion = options.occlusion;

	const auto pipelineResult = SceneVisibilityPipeline::Evaluate(
		pipelineOptions,
		primitiveSnapshot,
		*m_spatialIndex,
		representation,
		mode);
	if (options.enableCullReasonDebugSnapshot)
	{
		m_lastCullReasonDebugSnapshot = std::make_shared<SceneCullReasonDebugSnapshot>(
			SceneVisibilityPipeline::BuildCullReasonDebugSnapshot(
				primitiveSnapshot,
				pipelineResult,
				options.maxCullReasonDebugSnapshotEntries));
	}

	snapshot.fullScanCandidateCount = pipelineResult.fullScanCandidateCount;
	snapshot.primitiveRecordsTouched = pipelineResult.primitiveRecordsTouched;
	snapshot.visibilityTestedPrimitiveCount = pipelineResult.visibilityTestedPrimitiveCount;
	snapshot.occlusionTestCount = pipelineResult.occlusionTestCount;
	snapshot.occlusionCulledCount = pipelineResult.occlusionCulledCount;
	snapshot.usedParallelEvaluation = pipelineResult.usedParallelEvaluation;
	snapshot.representationStreamingInterest = pipelineResult.representationStreamingInterest;
	CopyCullReasonCounts(snapshot, pipelineResult);

	for (const auto handle : pipelineResult.visiblePrimitiveHandles)
	{
		if (!IsPrimitiveHandleLive(handle))
			continue;
		const auto primitiveIndex = static_cast<size_t>(handle.index);
		const auto& primitive = m_primitives[primitiveIndex];
		if (buildVisibilityBitsets)
			SetBit(snapshot.primitiveBits, primitiveIndex);
		snapshot.visiblePrimitiveIndices.push_back(primitiveIndex);
		snapshot.visiblePrimitiveHandles.push_back(handle);
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

			if (buildVisibilityBitsets)
				SetBit(snapshot.meshBits, meshBaseIndex + slotIndex);
			++snapshot.visibleMeshCount;
		}
	}

	return snapshot;
}

std::vector<SceneHLODClusterHandle> RenderScene::ResolveInspectableHLODClusters(
	const RenderSceneVisibilityOptions& options) const
{
	std::vector<SceneHLODClusterHandle> handles;
	if (!options.editorInspectionView ||
		options.inspectionRootObjects.empty() ||
		m_representationRegistry == nullptr)
	{
		return handles;
	}

	const auto isInsideInspectionRoot = [&options](const Engine::GameObject* owner)
	{
		for (const auto* root : options.inspectionRootObjects)
		{
			if (root == nullptr || owner == nullptr)
				continue;
			if (owner == root || owner->IsDescendantOf(root))
				return true;
		}
		return false;
	};

	for (const auto& cluster : m_representationRegistry->hlodClusters)
	{
		if (!cluster.clusterHandle.IsValid())
			continue;
		for (const auto child : cluster.childPrimitives)
		{
			if (!IsPrimitiveHandleLive(child))
				continue;
			const auto& primitive = m_primitives[child.index];
			if (!isInsideInspectionRoot(primitive.owner))
				continue;
			if (std::find(handles.begin(), handles.end(), cluster.clusterHandle) == handles.end())
				handles.push_back(cluster.clusterHandle);
			break;
		}
	}
	return handles;
}

RenderSceneVisibilitySnapshot RenderScene::EvaluateVisibilitySpatial(
	const RenderSceneVisibilityOptions& options,
	const std::vector<size_t>& meshBaseIndices,
	const RenderSceneVisibilityMode mode,
	const bool buildVisibilityBitsets) const
{
	const auto& settings = ResolveLargeSceneSettings(options);
	const bool occlusionEnabled =
		settings.enableHZBOcclusion && options.occlusion != nullptr;
	const bool representationEnabled =
		(settings.enableLOD && m_representationRegistry != nullptr && m_representationRegistry->HasLOD()) ||
		(settings.enableHLOD && m_representationRegistry != nullptr && m_representationRegistry->HasHLOD());
	const auto queryRadius = ResolveSpatialQueryRadius(options);
	if (queryRadius <= 0.0f)
	{
		if (representationEnabled || occlusionEnabled || options.enableCullReasonDebugSnapshot)
		{
			return EvaluateVisibilityThroughPipeline(
				options,
				meshBaseIndices,
				SceneVisibilityPipelineMode::Serial,
				buildVisibilityBitsets);
		}
		return EvaluateVisibilitySerialRange(options, meshBaseIndices, 0u, m_primitives.size());
	}

	RenderSceneVisibilitySnapshot snapshot;
	snapshot.usesSparseVisiblePrimitiveIndices = true;
	snapshot.primitiveCount = static_cast<uint64_t>(m_primitives.size());
	snapshot.meshCount = !meshBaseIndices.empty()
		? static_cast<uint64_t>(meshBaseIndices.back())
		: 0u;
	if (buildVisibilityBitsets)
	{
		snapshot.primitiveBits.resize(BitWordCount(static_cast<size_t>(snapshot.primitiveCount)));
		snapshot.meshBits.resize(BitWordCount(static_cast<size_t>(snapshot.meshCount)));
	}

	SceneVisibilityPipelineOptions pipelineOptions;
	pipelineOptions.frustum = options.frustum;
	pipelineOptions.cameraPosition = options.cameraPosition;
	pipelineOptions.visibleLayerMask = options.visibleLayerMask;
	pipelineOptions.enableSpatialIndex = false;
	pipelineOptions.spatialQueryRadius = queryRadius;
	pipelineOptions.enableLOD = settings.enableLOD;
	pipelineOptions.enableHLOD = settings.enableHLOD;
	pipelineOptions.enableOcclusion = settings.enableHZBOcclusion && options.occlusion != nullptr;
	pipelineOptions.enableParallelVisibility = settings.enableParallelVisibility;
	pipelineOptions.parallelVisibilityPrimitiveThreshold = settings.parallelVisibilityPrimitiveThreshold;
	pipelineOptions.parallelVisibilityPrimitivesPerTask = settings.parallelVisibilityPrimitivesPerTask;
	pipelineOptions.maxVisibilityJobs = settings.maxVisibilityJobs;

	SceneSpatialIndexQuery query;
	query.center = options.cameraPosition;
	query.radius = queryRadius;
	query.visibleLayerMask = options.visibleLayerMask;

	const auto candidates = m_spatialIndex->Query(query);
	auto initialCandidateSnapshot = CreatePrimitiveSnapshotForHandles(
		candidates.candidatePrimitiveHandles,
		{});
	std::vector<LODGroupRecord> candidateLODGroups;
	std::vector<HLODClusterRecord> candidateHLODClusters;
	SceneRepresentationState representation;
	if (m_representationRegistry != nullptr)
	{
		SceneRepresentationState registryRepresentation;
		registryRepresentation.lodGroups = &m_representationRegistry->lodGroups;
		registryRepresentation.hlodClusters = &m_representationRegistry->hlodClusters;
		registryRepresentation.lodGroupsByPrimitive = &m_representationRegistry->lodGroupsByPrimitive;
		registryRepresentation.hlodClustersByPrimitive = &m_representationRegistry->hlodClustersByPrimitive;
		const auto expansion = SceneVisibilityPipeline::ExpandRepresentationCandidates(
			candidates.candidatePrimitiveHandles,
			initialCandidateSnapshot,
			registryRepresentation);
		initialCandidateSnapshot = CreatePrimitiveSnapshotForHandles(
			expansion.primitiveHandles,
			{});

		candidateLODGroups.reserve(expansion.lodGroupIndices.size());
		for (const auto groupIndex : expansion.lodGroupIndices)
		{
			if (groupIndex < m_representationRegistry->lodGroups.size())
				candidateLODGroups.push_back(m_representationRegistry->lodGroups[groupIndex]);
		}
		candidateHLODClusters.reserve(expansion.hlodClusterIndices.size());
		for (const auto clusterIndex : expansion.hlodClusterIndices)
		{
			if (clusterIndex < m_representationRegistry->hlodClusters.size())
				candidateHLODClusters.push_back(m_representationRegistry->hlodClusters[clusterIndex]);
		}

		representation.lodGroups = &candidateLODGroups;
		representation.hlodClusters = &candidateHLODClusters;
		representation.lodSelectionHistory =
			&m_representationRegistry->LODHistoryForView(options.lodHistoryViewKey);
	}
	auto residency = BuildRepresentationResidencySnapshot(
		initialCandidateSnapshot,
		options.representationResidency);
	if (m_representationRegistry != nullptr)
	{
		MarkHLODProxyResidency(
			residency,
			initialCandidateSnapshot,
			candidateHLODClusters);
	}
	representation.residency = &residency;
	representation.occlusion = options.occlusion;
	pipelineOptions.lodBias = options.lodBias;
	pipelineOptions.lodHistoryViewKey = options.lodHistoryViewKey;
	pipelineOptions.allowHLOD = options.allowHLOD;
	pipelineOptions.editorInspectionView = options.editorInspectionView;
	pipelineOptions.selectedPrimitiveHandles = options.selectedPrimitiveHandles;
	pipelineOptions.forceInspectableHLODClusters = ResolveInspectableHLODClusters(options);
	const auto pipelineResult = SceneVisibilityPipeline::Evaluate(
		pipelineOptions,
		initialCandidateSnapshot,
		*m_spatialIndex,
		representation,
		mode == RenderSceneVisibilityMode::Parallel
			? SceneVisibilityPipelineMode::Parallel
			: (mode == RenderSceneVisibilityMode::Auto
				? SceneVisibilityPipelineMode::Auto
				: SceneVisibilityPipelineMode::Serial));
	if (options.enableCullReasonDebugSnapshot)
	{
		m_lastCullReasonDebugSnapshot = std::make_shared<SceneCullReasonDebugSnapshot>(
			SceneVisibilityPipeline::BuildCullReasonDebugSnapshot(
				initialCandidateSnapshot,
				pipelineResult,
				options.maxCullReasonDebugSnapshotEntries));
	}
	snapshot.spatialCandidateCount = candidates.candidateCount;
	snapshot.fullScanCandidateCount = candidates.fullScanCandidateCount;
	snapshot.primitiveRecordsTouched =
		candidates.primitiveRecordsTouched + pipelineResult.primitiveRecordsTouched;
	snapshot.visibilityTestedPrimitiveCount = pipelineResult.visibilityTestedPrimitiveCount;
	snapshot.occlusionTestCount = pipelineResult.occlusionTestCount;
	snapshot.occlusionCulledCount = pipelineResult.occlusionCulledCount;
	snapshot.dynamicCandidateCount = candidates.dynamicCandidateCount;
	snapshot.dynamicRecordsTouched = candidates.dynamicRecordsTouched;
	snapshot.staticIndexRefitCount = candidates.telemetry.staticIndexRefitCount;
	snapshot.staticIndexRebuildCount = candidates.telemetry.staticIndexRebuildCount;
	snapshot.staticIndexLastGoodQueryCount = candidates.telemetry.staticIndexLastGoodQueryCount;
	snapshot.staticIndexDirtyOverlayCount = candidates.telemetry.staticIndexDirtyOverlayCount;
	snapshot.spatialRebuildFallbackCount = candidates.telemetry.spatialRebuildFallbackCount;
	snapshot.dynamicIndexUpdateCount = candidates.telemetry.dynamicIndexUpdateCount;
	snapshot.representationStreamingInterest = pipelineResult.representationStreamingInterest;
	CopyCullReasonCounts(snapshot, pipelineResult);

	for (const auto& handle : pipelineResult.visiblePrimitiveHandles)
	{
		if (handle.sceneId != m_sceneId || handle.index >= m_primitives.size())
			continue;

		const auto primitiveIndex = static_cast<size_t>(handle.index);
		const auto& primitive = m_primitives[primitiveIndex];
		if (primitive.handle != handle || !primitive.occupied || primitive.tombstoned)
			continue;

		if (buildVisibilityBitsets)
			SetBit(snapshot.primitiveBits, primitiveIndex);
		snapshot.visiblePrimitiveIndices.push_back(primitiveIndex);
		snapshot.visiblePrimitiveHandles.push_back(handle);
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

			if (buildVisibilityBitsets)
				SetBit(snapshot.meshBits, meshBaseIndex + slotIndex);
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

	const auto& settings = ResolveLargeSceneSettings(options);
	const auto primitivesPerTask = std::max<size_t>(1u, settings.parallelVisibilityPrimitivesPerTask);
	const auto hardwareThreads = std::max<uint32_t>(1u, NLS::Base::Jobs::GetJobWorkerCount() + 1u);
	const auto desiredTaskCount =
		(primitiveCount + primitivesPerTask - 1u) /
		primitivesPerTask;
	const auto maxVisibilityJobs = settings.maxVisibilityJobs != 0u
		? settings.maxVisibilityJobs
		: hardwareThreads;
	const auto taskLimit = std::max<uint32_t>(1u, std::min(hardwareThreads, maxVisibilityJobs));
	const auto taskCount = std::min<size_t>(taskLimit, desiredTaskCount);
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
	target.visiblePrimitiveHandles.insert(
		target.visiblePrimitiveHandles.end(),
		source.visiblePrimitiveHandles.begin(),
		source.visiblePrimitiveHandles.end());
	target.spatialCandidateCount += source.spatialCandidateCount;
	target.fullScanCandidateCount += source.fullScanCandidateCount;
	target.primitiveRecordsTouched += source.primitiveRecordsTouched;
	target.visibilityTestedPrimitiveCount += source.visibilityTestedPrimitiveCount;
}

void RenderScene::RefreshSyncTelemetry(const RenderSceneSyncStats& stats)
{
	m_lastLargeSceneTelemetry = {};
	m_lastLargeSceneTelemetry.registeredPrimitiveCount = static_cast<uint64_t>(m_livePrimitiveCount);
	m_lastLargeSceneTelemetry.staticPrimitiveCount = 0u;
	m_lastLargeSceneTelemetry.dynamicPrimitiveCount = 0u;
	m_lastLargeSceneTelemetry.unclassifiedPrimitiveCount = static_cast<uint64_t>(m_livePrimitiveCount);
	m_lastLargeSceneTelemetry.allocatedPrimitiveSlotCount = static_cast<uint64_t>(m_primitives.size());
	m_lastLargeSceneTelemetry.tombstonedPrimitiveSlotCount =
		static_cast<uint64_t>(m_primitives.size() - m_livePrimitiveCount);
	m_lastLargeSceneTelemetry.syncSweepTouchedSlotCount = stats.syncSweepTouchedSlotCount;
	m_lastLargeSceneTelemetry.syncTouchedPrimitiveCount = stats.syncTouchedPrimitiveCount;
	m_lastLargeSceneTelemetry.syncFullSweepCount = stats.syncFullSweepCount;
	m_lastLargeSceneTelemetry.boundsDirtyPrimitiveCount = stats.boundsDirtyPrimitiveCount;
	m_lastLargeSceneTelemetry.primitiveSlotReuseCount = stats.primitiveSlotReuseCount;
	m_lastLargeSceneTelemetry.commandOffsetRebuildCount = 0u;
	m_lastLargeSceneTelemetry.syncTimeNs = stats.syncTimeNs;
	if (m_spatialIndex != nullptr)
	{
		ApplySpatialIndexUpdateTelemetry(m_lastLargeSceneTelemetry, *m_spatialIndex);
	}
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

	if (command.material != nullptr && command.material->IsDecal())
		output.decals.emplace_back(distanceToActor, std::move(drawable));
	else if (command.material != nullptr && command.material->IsBlendable())
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

#if defined(NLS_ENABLE_TEST_HOOKS)
void RenderScene::FinalizeOpaqueQueueForTesting(RenderSceneVisibleQueues::SceneDrawables& opaques) const
{
	FinalizeOpaqueQueue(opaques);
}
#endif

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
						chunkDescriptor.objectFlags = descriptor.objectFlags;
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
	assignQueue(output.decals);
	assignQueue(output.skyboxes);
	assignQueue(output.transparents);
}
}
