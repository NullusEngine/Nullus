#include "Rendering/RenderScene.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <any>
#include <chrono>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <Rendering/Context/RenderingThreadSystem.h>
#include <unordered_map>
#include <unordered_set>

#include <Json/json.hpp>

#include <Debug/Logger.h>

#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Core/ServiceLocator.h"
#include "Components/TransformComponent.h"
#include "GameObject.h"
#include "Engine/Assets/ModelPrefabBuilder.h"
#include "TaskGraph/TaskGraphSystem.h"
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
#include "Rendering/SceneProxySourceRegistry.h"
#include "Rendering/SceneRenderStoreRegistry.h"
#include "Rendering/SceneSpatialIndex.h"
#include "Rendering/SceneVisibilityPipeline.h"
#include "SceneSystem/Scene.h"
#include "Profiling/Profiler.h"

namespace NLS::Engine::Rendering
{
	namespace
	{
		uint64_t MakeSceneProxyKey(const SceneProxyHandle handle)
		{
			return (static_cast<uint64_t>(handle.index) << 32u) |
				static_cast<uint64_t>(handle.generation);
		}
	}

	struct RenderSceneDeclaredTextureLookupCache
	{
		std::unordered_map<std::string, NLS::Render::Resources::Texture2D*> declaredTexturesByNormalizedPath;
		std::unordered_map<std::string, NLS::Render::Resources::Texture2D*> resourcesByNormalizedPath;
		std::unordered_map<const NLS::Render::Resources::Material*, bool> declaredTexturesResolvedByMaterial;
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

	uint64_t HashOpaqueSortTokenComponent(uint64_t token, const uint64_t value)
	{
		constexpr uint64_t kFnvPrime = 1099511628211ull;
		token ^= value;
		token *= kFnvPrime;
		return token;
	}

	bool PopulateTrustedStaticDrawRevision(
		NLS::Engine::Rendering::EngineDrawableDescriptor& descriptor,
		const ScenePrimitiveHandle& handle,
		const uint64_t transformRevision,
		const RenderCachedDrawCommand& command)
	{
		auto* mesh = command.mesh;
		auto* material = command.material;
		auto* shader = material != nullptr ? material->GetShader() : nullptr;
		if (!handle.IsValid() || command.buildSerial == 0u || transformRevision == 0u ||
			mesh == nullptr || material == nullptr || shader == nullptr)
		{
			return false;
		}

		descriptor.cachedCommandBuildSerial = command.buildSerial;
		descriptor.meshInstanceId = mesh->GetInstanceId();
		descriptor.meshContentRevision = mesh->GetContentRevision();
		descriptor.materialInstanceId = material->GetInstanceId();
		descriptor.materialParameterRevision = material->GetParameterRevision();
		descriptor.materialRenderStateRevision = material->GetRenderStateRevision();
		descriptor.materialBindingRevision = material->GetBindingRevision();
		descriptor.shaderInstanceId = shader->GetInstanceId();
		descriptor.shaderGeneration = shader->GetGeneration();
		descriptor.transformRevision = transformRevision;
		descriptor.stableSceneIdentity = { handle.sceneId, handle.index, handle.generation };
		descriptor.groupIdentity =
			NLS::Engine::Rendering::EngineDrawableDescriptor::kInvalidStaticDrawGroupIdentity;
		descriptor.allowsSingleObjectDataReuse = true;
		descriptor.hasTrustedStaticDrawRevision =
			descriptor.meshInstanceId != 0u && descriptor.meshContentRevision != 0u &&
			descriptor.materialInstanceId != 0u && descriptor.materialParameterRevision != 0u &&
			descriptor.materialRenderStateRevision != 0u && descriptor.materialBindingRevision != 0u &&
			descriptor.shaderInstanceId != 0u && descriptor.shaderGeneration != 0u;
		return descriptor.hasTrustedStaticDrawRevision;
	}

	bool TrustedStaticDrawResourcesMatch(
		const NLS::Engine::Rendering::EngineDrawableDescriptor& lhs,
		const NLS::Engine::Rendering::EngineDrawableDescriptor& rhs)
	{
		return lhs.meshInstanceId == rhs.meshInstanceId &&
			lhs.meshContentRevision == rhs.meshContentRevision &&
			lhs.materialInstanceId == rhs.materialInstanceId &&
			lhs.materialParameterRevision == rhs.materialParameterRevision &&
			lhs.materialRenderStateRevision == rhs.materialRenderStateRevision &&
			lhs.materialBindingRevision == rhs.materialBindingRevision &&
			lhs.shaderInstanceId == rhs.shaderInstanceId &&
			lhs.shaderGeneration == rhs.shaderGeneration;
	}

	uint64_t BuildOpaqueDrawSortToken(
		const NLS::Render::Resources::Mesh* mesh,
		const NLS::Render::Resources::Material* material,
		const NLS::Render::Data::StateMask stateMask,
		const NLS::Render::Settings::EPrimitiveMode primitiveMode,
		const uint32_t vertexStart,
		const uint32_t vertexCount)
	{
		constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
		auto token = HashOpaqueSortTokenComponent(
			kFnvOffsetBasis,
			material != nullptr ? material->GetInstanceId() : 0u);
		token = HashOpaqueSortTokenComponent(token, reinterpret_cast<uintptr_t>(material));
		token = HashOpaqueSortTokenComponent(
			token,
			mesh != nullptr ? mesh->GetInstanceId() : 0u);
		token = HashOpaqueSortTokenComponent(token, reinterpret_cast<uintptr_t>(mesh));
		token = HashOpaqueSortTokenComponent(token, stateMask.mask);
		token = HashOpaqueSortTokenComponent(token, static_cast<uint8_t>(primitiveMode));
		token = HashOpaqueSortTokenComponent(token, vertexStart);
		token = HashOpaqueSortTokenComponent(token, vertexCount);
		if (token == EngineDrawableDescriptor::kInvalidOpaqueSortToken)
			--token;
		return token;
	}

	uint64_t BuildOpaqueDrawSortToken(const NLS::Render::Entities::Drawable& drawable)
	{
		return BuildOpaqueDrawSortToken(
			drawable.mesh,
			drawable.material,
			drawable.stateMask,
			drawable.primitiveMode,
			drawable.vertexStart,
			drawable.vertexCount);
	}

	uint64_t BuildMeshLODRevisionDigest(const NLS::Render::Resources::Mesh& mesh)
	{
		constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
		auto digest = HashOpaqueSortTokenComponent(kFnvOffsetBasis, mesh.GetLODCount());
		for (uint32_t lodIndex = 0u; lodIndex < mesh.GetLODCount(); ++lodIndex)
		{
			const auto* lodMesh = mesh.GetLODMesh(lodIndex);
			digest = HashOpaqueSortTokenComponent(digest, reinterpret_cast<uintptr_t>(lodMesh));
			digest = HashOpaqueSortTokenComponent(
				digest,
				lodMesh != nullptr ? lodMesh->GetInstanceId() : 0u);
			digest = HashOpaqueSortTokenComponent(
				digest,
				lodMesh != nullptr ? lodMesh->GetContentRevision() : 0u);
		}
		return digest;
	}

	bool OpaqueDrawStateMatches(
		const NLS::Render::Entities::Drawable& lhs,
		const NLS::Render::Entities::Drawable& rhs)
	{
		return lhs.material == rhs.material &&
			lhs.mesh == rhs.mesh &&
			lhs.stateMask.mask == rhs.stateMask.mask &&
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
		if (!OpaqueDrawStateMatches(lhs, rhs))
			return false;
		if (lhs.material == nullptr || lhs.mesh == nullptr)
			return false;
		if (lhs.material->IsBlendable())
			return false;
		const auto* shader = lhs.material->GetShader();
		if (shader == nullptr || !ShaderSupportsIndexedObjectData(*shader))
			return false;
		const auto lhsInstanceCount = ResolveVisibleInstanceCount(lhs);
		const auto rhsInstanceCount = ResolveVisibleInstanceCount(rhs);
		if (lhsInstanceCount == 0u || rhsInstanceCount != 1u)
			return false;
		if (lhs.material->GetGPUInstances() != 1)
			return false;

		const auto* lhsDescriptor =
			lhs.TryGetDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>();
		const auto* rhsDescriptor =
			rhs.TryGetDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>();
		return lhsDescriptor != nullptr &&
			rhsDescriptor != nullptr &&
			DescriptorCanParticipateInDynamicInstancing(*lhsDescriptor, lhsInstanceCount) &&
			DescriptorCanParticipateInDynamicInstancing(*rhsDescriptor, rhsInstanceCount) &&
			DescriptorPerObjectStateMatchesForDynamicInstancing(*lhsDescriptor, *rhsDescriptor);
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

	bool HasResolvedDeclaredMaterialTexturesCached(
		const NLS::Render::Resources::Material& material,
		RenderSceneDeclaredTextureLookupCache& cache)
	{
		const auto found = cache.declaredTexturesResolvedByMaterial.find(&material);
		if (found != cache.declaredTexturesResolvedByMaterial.end())
			return found->second;

		const bool resolved = HasResolvedDeclaredMaterialTextures(material);
		cache.declaredTexturesResolvedByMaterial.emplace(&material, resolved);
		return resolved;
	}

	bool TryBindDeclaredMaterialTexturesFromCache(
		NLS::Render::Resources::Material& material,
		RenderSceneDeclaredTextureLookupCache& cache,
		RenderSceneSyncStats& stats)
	{
		// Binding may change the resolved-texture state, so the per-frame memo must be dropped.
		cache.declaredTexturesResolvedByMaterial.erase(&material);
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

struct RenderScene::SpatialCandidateSnapshotCache
{
	std::vector<ScenePrimitiveHandle> candidatePrimitiveHandles;
	ScenePrimitiveSnapshot primitiveSnapshot;
	std::vector<LODGroupRecord> lodGroups;
	std::vector<HLODClusterRecord> hlodClusters;
};

RenderScene::RenderScene()
	: m_sceneId(AllocateRenderSceneId()),
	  m_spatialIndex(std::make_unique<SceneSpatialIndex>()),
	  m_representationRegistry(std::make_unique<RepresentationRegistry>()),
	  m_importedHierarchyHLODClusterHandles()
{
}

RenderScene::~RenderScene() = default;

SceneProxyUpdateBatch RenderScene::CollectProxyUpdates(
	SceneSystem::Scene& scene,
	const RenderSceneSyncOptions& options,
	const uint64_t frameId)
{
	const auto entry = SceneRenderStoreRegistry::Acquire(
		static_cast<uint64_t>(scene.GetInstanceID()),
		&scene);

	// Collect through a handle taken before the call: the registry stays alive for
	// the duration even if another thread republishes the handles meanwhile.
	const std::shared_ptr<SceneProxySourceRegistry> sourceRegistry = entry.sourceRegistry;
	if (sourceRegistry == nullptr)
		return {};

	SceneProxyExtractionOptions extractionOptions;
	extractionOptions.defaultMaterial = options.defaultMaterial;
	extractionOptions.overrideMaterial = options.overrideMaterial;
	extractionOptions.requireExplicitMaterialTextures = options.requireExplicitMaterialTextures;
	extractionOptions.allowDefaultMaterialForUnresolvedExplicitMaterials =
		options.allowDefaultMaterialForUnresolvedExplicitMaterials;
	extractionOptions.largeSceneSettings = options.largeSceneSettings;
	return sourceRegistry->Collect(scene, extractionOptions, frameId);
}

SceneRenderStoreApplyStats RenderScene::ApplyProxyUpdates(const SceneProxyUpdateBatch& batch)
{
	if (!NLS::Render::Context::RenderingThreadSystem::IsInRenderingThread())
		throw std::logic_error("RenderScene proxy updates require the render thread");
	std::shared_ptr<SceneRenderStore> store = GetProxyStoreHandle();
	if (store == nullptr || store->GetSceneId() != batch.sceneId)
	{
		const void* const proxySceneIdentity = GetProxySceneIdentity();
		if (proxySceneIdentity == nullptr)
			return {};

		store = SceneRenderStoreRegistry::Acquire(
			batch.sceneId,
			proxySceneIdentity).store;
		{
			std::lock_guard lock(m_proxyHandleMutex);
			m_proxyStore = store;
		}
		PublishResolvedSynchronizationState({});
	}
	if (store == nullptr)
		return {};
	const auto stats = store->Apply(batch);
    store->RetireResources();
	return stats;
}

std::shared_ptr<const SceneRenderStore> RenderScene::GetProxyStoreHandle() const
{
	std::lock_guard lock(m_proxyHandleMutex);
	return m_proxyStore;
}

std::shared_ptr<SceneRenderStore> RenderScene::GetProxyStoreHandle()
{
	std::lock_guard lock(m_proxyHandleMutex);
	return m_proxyStore;
}

const void* RenderScene::GetProxySceneIdentity() const
{
	std::lock_guard lock(m_proxyHandleMutex);
	return m_proxySceneIdentity;
}

void RenderScene::SetProxySceneIdentity(const void* const sceneIdentity)
{
	std::lock_guard lock(m_proxyHandleMutex);
	m_proxySceneIdentity = sceneIdentity;
}

RenderScene::RenderScene(RenderScene&& other) noexcept
	: m_sceneId(other.m_sceneId),
	  m_primitives(std::move(other.m_primitives)),
	  m_nextStableObjectIndex(other.m_nextStableObjectIndex),
	  m_freeStableObjectIndices(std::move(other.m_freeStableObjectIndices)),
	  m_primitiveIndexByMeshRenderer(std::move(other.m_primitiveIndexByMeshRenderer)),
	  m_primitiveIndexByProxy(std::move(other.m_primitiveIndexByProxy)),
	  m_parentByOwnerInstanceId(std::move(other.m_parentByOwnerInstanceId)),
	  m_trackedMaterialInstanceIds(std::move(other.m_trackedMaterialInstanceIds)),
	  m_pendingDeclaredTextureProxyKeys(std::move(other.m_pendingDeclaredTextureProxyKeys)),
	  m_firstFreePrimitiveSlot(other.m_firstFreePrimitiveSlot),
	  m_livePrimitiveCount(other.m_livePrimitiveCount),
	  m_lastSceneFastAccessRevision(other.m_lastSceneFastAccessRevision),
	  m_resolvedSynchronization(other.ReadResolvedSynchronizationState()),
	  m_lastMeshMutationEpoch(other.m_lastMeshMutationEpoch),
	  m_lastObservedMaterialMutationEpoch(other.m_lastObservedMaterialMutationEpoch),
	  m_nextCachedCommandBuildSerial(other.m_nextCachedCommandBuildSerial),
	  m_cachedCommandBuildCount(other.m_cachedCommandBuildCount),
	  m_opaqueSortTokenBuildCount(other.m_opaqueSortTokenBuildCount),
	  m_nextPrimitiveSnapshotSerial(other.m_nextPrimitiveSnapshotSerial),
	  m_cachedMeshBaseIndices(std::move(other.m_cachedMeshBaseIndices)),
	  m_commandOffsetTableDirty(other.m_commandOffsetTableDirty),
	  m_lastDirtySyncHandles(std::move(other.m_lastDirtySyncHandles)),
	  m_lastRemovedHandles(std::move(other.m_lastRemovedHandles)),
	  m_lastSyncStats(other.m_lastSyncStats),
	  m_lastDrawCallOptimizationStats(other.m_lastDrawCallOptimizationStats),
	  m_lastLargeSceneTelemetry(other.m_lastLargeSceneTelemetry),
	  m_lastCullReasonDebugSnapshot(std::move(other.m_lastCullReasonDebugSnapshot)),
	  m_lastVisiblePrimitiveHandles(std::move(other.m_lastVisiblePrimitiveHandles)),
	  m_lastRepresentationStreamingInterest(std::move(other.m_lastRepresentationStreamingInterest)),
	  m_spatialIndex(std::move(other.m_spatialIndex)),
	  m_representationRegistry(std::move(other.m_representationRegistry)),
	  m_spatialCandidateSnapshotCache(std::move(other.m_spatialCandidateSnapshotCache)),
	  m_spatialCandidateSnapshotCacheHitCount(other.m_spatialCandidateSnapshotCacheHitCount),
	  m_importedHierarchyHLODClusterHandles(std::move(other.m_importedHierarchyHLODClusterHandles)),
	  m_trackedMaterialStates(std::move(other.m_trackedMaterialStates)),
	  m_proxySourceRegistry(std::move(other.m_proxySourceRegistry)),
	  m_proxyStore(std::move(other.m_proxyStore)),
	  m_proxySceneIdentity(other.m_proxySceneIdentity),
	  m_proxyFrameSerial(other.m_proxyFrameSerial)
{
	other.ResetMovedFromState();
}

RenderScene& RenderScene::operator=(RenderScene&& other) noexcept
{
	if (this == &other)
		return *this;

	m_sceneId = other.m_sceneId;
	m_primitives = std::move(other.m_primitives);
	m_nextStableObjectIndex = other.m_nextStableObjectIndex;
	m_freeStableObjectIndices = std::move(other.m_freeStableObjectIndices);
	m_primitiveIndexByMeshRenderer = std::move(other.m_primitiveIndexByMeshRenderer);
	m_primitiveIndexByProxy = std::move(other.m_primitiveIndexByProxy);
	m_parentByOwnerInstanceId = std::move(other.m_parentByOwnerInstanceId);
	m_trackedMaterialInstanceIds = std::move(other.m_trackedMaterialInstanceIds);
	m_pendingDeclaredTextureProxyKeys = std::move(other.m_pendingDeclaredTextureProxyKeys);
	m_firstFreePrimitiveSlot = other.m_firstFreePrimitiveSlot;
	m_livePrimitiveCount = other.m_livePrimitiveCount;
	m_lastSceneFastAccessRevision = other.m_lastSceneFastAccessRevision;
	PublishResolvedSynchronizationState(other.ReadResolvedSynchronizationState());
	m_lastMeshMutationEpoch = other.m_lastMeshMutationEpoch;
	m_lastObservedMaterialMutationEpoch = other.m_lastObservedMaterialMutationEpoch;
	m_syncMeshLODRevisionDigestCache.clear();
	m_trackedMaterialStates = std::move(other.m_trackedMaterialStates);
	m_nextCachedCommandBuildSerial = other.m_nextCachedCommandBuildSerial;
	m_cachedCommandBuildCount = other.m_cachedCommandBuildCount;
	m_opaqueSortTokenBuildCount = other.m_opaqueSortTokenBuildCount;
	m_nextPrimitiveSnapshotSerial = other.m_nextPrimitiveSnapshotSerial;
	m_cachedMeshBaseIndices = std::move(other.m_cachedMeshBaseIndices);
	m_commandOffsetTableDirty = other.m_commandOffsetTableDirty;
	m_lastDirtySyncHandles = std::move(other.m_lastDirtySyncHandles);
	m_lastRemovedHandles = std::move(other.m_lastRemovedHandles);
	m_lastSyncStats = other.m_lastSyncStats;
	m_lastDrawCallOptimizationStats = other.m_lastDrawCallOptimizationStats;
	m_lastLargeSceneTelemetry = other.m_lastLargeSceneTelemetry;
	m_lastCullReasonDebugSnapshot = std::move(other.m_lastCullReasonDebugSnapshot);
	m_lastVisiblePrimitiveHandles = std::move(other.m_lastVisiblePrimitiveHandles);
	m_lastRepresentationStreamingInterest = std::move(other.m_lastRepresentationStreamingInterest);
	m_spatialIndex = std::move(other.m_spatialIndex);
	m_representationRegistry = std::move(other.m_representationRegistry);
	m_spatialCandidateSnapshotCache = std::move(other.m_spatialCandidateSnapshotCache);
	m_spatialCandidateSnapshotCacheHitCount = other.m_spatialCandidateSnapshotCacheHitCount;
	m_importedHierarchyHLODClusterHandles = std::move(other.m_importedHierarchyHLODClusterHandles);
	m_proxySourceRegistry = std::move(other.m_proxySourceRegistry);
	m_proxyStore = std::move(other.m_proxyStore);
	m_proxySceneIdentity = other.m_proxySceneIdentity;
	m_proxyFrameSerial = other.m_proxyFrameSerial;

	other.ResetMovedFromState();
	return *this;
}

void RenderScene::ResetMovedFromState() noexcept
{
	m_sceneId = AllocateRenderSceneId();
	m_primitives.clear();
	m_nextStableObjectIndex = 0u;
	m_freeStableObjectIndices.clear();
	m_primitiveIndexByMeshRenderer.clear();
	m_primitiveIndexByProxy.clear();
	m_parentByOwnerInstanceId.clear();
	m_pendingDeclaredTextureProxyKeys.clear();
	m_firstFreePrimitiveSlot.reset();
	m_livePrimitiveCount = 0u;
	m_lastSceneFastAccessRevision = 0u;
	m_lastMeshMutationEpoch = 0u;
	m_lastObservedMaterialMutationEpoch = 0u;
	m_syncMeshLODRevisionDigestCache.clear();
	m_trackedMaterialStates.clear();
	m_trackedMaterialInstanceIds.clear();
	m_nextCachedCommandBuildSerial = 1u;
	m_cachedCommandBuildCount = 0u;
	m_opaqueSortTokenBuildCount = 0u;
	m_nextPrimitiveSnapshotSerial = 1u;
	m_cachedMeshBaseIndices.clear();
	m_commandOffsetTableDirty = true;
	m_lastDirtySyncHandles.clear();
	m_lastRemovedHandles.clear();
	m_lastSyncStats = {};
	m_lastDrawCallOptimizationStats = {};
	m_lastLargeSceneTelemetry = {};
	m_lastCullReasonDebugSnapshot.reset();
	m_lastVisiblePrimitiveHandles.clear();
	m_lastRepresentationStreamingInterest.clear();
	m_spatialIndex = std::make_unique<SceneSpatialIndex>();
	m_representationRegistry = std::make_unique<RepresentationRegistry>();
	m_spatialCandidateSnapshotCache.reset();
	m_spatialCandidateSnapshotCacheHitCount = 0u;
	m_importedHierarchyHLODClusterHandles.clear();
	{
		std::lock_guard lock(m_proxyHandleMutex);
		m_proxySourceRegistry.reset();
		m_proxyStore.reset();
	}
	m_proxySceneIdentity = nullptr;
	m_proxyFrameSerial = 0u;
	PublishResolvedSynchronizationState({});
}

RenderScene::ResolvedSceneSynchronizationState
RenderScene::ReadResolvedSynchronizationState() const
{
	std::lock_guard lock(m_resolvedSynchronizationMutex);
	return m_resolvedSynchronization;
}

void RenderScene::PublishResolvedSynchronizationState(
	const ResolvedSceneSynchronizationState& state)
{
	std::lock_guard lock(m_resolvedSynchronizationMutex);
	m_resolvedSynchronization = state;
}

std::shared_ptr<const RenderScene::TrackedMaterialKeys>
RenderScene::ReadTrackedMaterialKeys() const
{
	std::lock_guard lock(m_resolvedSynchronizationMutex);
	return m_trackedMaterialKeys;
}

void RenderScene::PublishTrackedMaterialKeys(
	std::shared_ptr<const TrackedMaterialKeys> keys)
{
	std::lock_guard lock(m_resolvedSynchronizationMutex);
	m_trackedMaterialKeys = std::move(keys);
}

bool RenderScene::CachedCommandInputStamp::operator==(const CachedCommandInputStamp& other) const
{
	return mesh == other.mesh &&
		material == other.material &&
		meshContentRevision == other.meshContentRevision &&
		meshLODRevisionDigest == other.meshLODRevisionDigest &&
		materialInstanceId == other.materialInstanceId &&
		materialParameterRevision == other.materialParameterRevision &&
		materialRenderStateRevision == other.materialRenderStateRevision &&
		materialBindingRevision == other.materialBindingRevision &&
		shaderInstanceId == other.shaderInstanceId &&
		shaderGeneration == other.shaderGeneration &&
		stateMask == other.stateMask &&
		primitiveMode == other.primitiveMode;
}

bool RenderScene::PrimitiveInputStamp::operator==(const PrimitiveInputStamp& other) const
{
	return proxyHandle == other.proxyHandle &&
		ownerInstanceId == other.ownerInstanceId &&
		owner == other.owner &&
		meshFilter == other.meshFilter &&
		mesh == other.mesh &&
		AreSameMatrix(worldMatrix, other.worldMatrix) &&
		layer == other.layer &&
			ownerRenderRevision == other.ownerRenderRevision &&
			transformRenderRevision == other.transformRenderRevision &&
			meshFilterRenderRevision == other.meshFilterRenderRevision &&
			meshRendererRenderRevision == other.meshRendererRenderRevision &&
			meshContentRevision == other.meshContentRevision &&
			meshLODRevisionDigest == other.meshLODRevisionDigest &&
			material == other.material &&
			materialInstanceId == other.materialInstanceId &&
			requestedMaterialInstanceId == other.requestedMaterialInstanceId &&
			materialParameterRevision == other.materialParameterRevision &&
				materialRenderStateRevision == other.materialRenderStateRevision &&
				explicitMaterialTexturesResolved == other.explicitMaterialTexturesResolved &&
				requireExplicitMaterialTextures == other.requireExplicitMaterialTextures &&
				allowDefaultMaterialForUnresolvedExplicitMaterials == other.allowDefaultMaterialForUnresolvedExplicitMaterials &&
				ownerAlive == other.ownerAlive &&
				ownerActive == other.ownerActive;
}

bool RenderScene::IntrinsicLODGroupRecordMatches(
	const LODGroupRecord& record,
	const RenderPrimitive& primitive,
	const NLS::Render::Resources::Mesh& mesh) const
{
	// This mirrors exactly what the intrinsic registration path writes, so a match means the
	// record can be left untouched and the per-frame lookup rebuild can be skipped.
	if (!record.selectsMeshLOD ||
		record.hysteresis != 0.0f ||
		record.forcedLOD.has_value() ||
		record.maxLOD != (std::numeric_limits<uint32_t>::max)() ||
		record.minLOD != mesh.GetMinLOD() ||
		record.worldReferencePoint != primitive.worldPosition ||
		record.worldSize != std::max(primitive.modelBoundingSphere.radius * 2.0f, 1.0f) ||
		record.boundsSphereRadius != primitive.modelBoundingSphere.radius ||
		record.levels.size() != mesh.GetLODCount())
	{
		return false;
	}

	for (uint32_t lodIndex = 0u; lodIndex < mesh.GetLODCount(); ++lodIndex)
	{
		const auto& level = record.levels[lodIndex];
		if (level.screenRelativeThreshold != mesh.GetLODScreenSize(lodIndex) ||
			level.fadeDurationSeconds != 0.0f ||
			!level.resident ||
			level.primitiveHandles.size() != 1u ||
			!(level.primitiveHandles.front() == primitive.handle))
		{
			return false;
		}
	}
	return true;
}

RenderSceneSyncRequest RenderScene::BeginSynchronize(
	SceneSystem::Scene& scene,
	const RenderSceneSyncOptions& options)
{
	NLS_PROFILE_NAMED_SCOPE("RenderScene::BeginSynchronize");

	// Bound game-thread capture without changing the render thread's current scene.
	uint64_t claimedCaptureSerial = 0u;
	{
		std::lock_guard lock(m_outstandingSynchronizationMutex);
		if (m_outstandingSynchronizations.size() >= m_maxOutstandingSynchronizations)
		{
			NLS_LOG_ERROR(
				"RenderScene::BeginSynchronize refused: " +
				std::to_string(m_outstandingSynchronizations.size()) +
				" capture(s) are still outstanding (limit " +
				std::to_string(m_maxOutstandingSynchronizations) +
				"). Resolve them before starting another.");
			return {};
		}

		claimedCaptureSerial = ++m_nextCaptureSerial;
		m_outstandingSynchronizations.push_back(claimedCaptureSerial);
	}

	RenderSceneSyncRequest request;
	request.captureSerial = claimedCaptureSerial;
	request.sceneIdentity = &scene;
	request.sceneInstanceId = static_cast<uint64_t>(scene.GetInstanceID());
		request.sceneGameObjectCount = static_cast<uint64_t>(scene.GetGameObjects().size());

	request.proxyStore = SceneRenderStoreRegistry::Acquire(request.sceneInstanceId, &scene).store;
	const auto resolved = ReadResolvedSynchronizationState();
	request.sceneIdentityChanged = !resolved.hasSynchronizationStamp ||
		resolved.synchronizationStamp.scene != &scene;

	request.sceneRenderContentRevision = scene.GetRenderContentRevision();
	request.meshMutationEpoch = NLS::Render::Resources::Mesh::GetGlobalMutationEpoch();
	request.trackedMaterialMutationObserved = HasTrackedMaterialMutation();
	// One consistent snapshot: a torn read here would let this frame believe it
	// had already accounted for changes the resolve half has not applied yet.
	request.meshMutationObserved = resolved.meshMutationEpoch != request.meshMutationEpoch;

	const auto synchronizationStamp =
		BuildSceneSynchronizationStamp(request.sceneIdentity, options);
	request.synchronizationOptionsChanged =
		!resolved.hasSynchronizationStamp ||
		!(resolved.synchronizationStamp == synchronizationStamp);
	const uint64_t storeChangeSerial =
		request.proxyStore != nullptr ? request.proxyStore->GetChangeSerial() : 0u;
	const bool sharedStoreChanged = storeChangeSerial != resolved.storeChangeSerial;
	const bool hasPendingDeclaredTextures = resolved.hasPendingDeclaredTextures;
	request.canReuseProxySynchronization =
		options.trustSceneRenderContentRevision &&
		request.proxyStore != nullptr &&
		!request.sceneIdentityChanged &&
		!request.synchronizationOptionsChanged &&
		resolved.sceneRenderContentRevision == request.sceneRenderContentRevision &&
		resolved.meshMutationEpoch == request.meshMutationEpoch &&
		!request.trackedMaterialMutationObserved &&
		!sharedStoreChanged &&
		!hasPendingDeclaredTextures;

	if (!request.canReuseProxySynchronization)
	{
		request.sourceNeedsCollection =
			request.sceneIdentityChanged ||
			request.synchronizationOptionsChanged ||
			!options.trustSceneRenderContentRevision ||
			resolved.sceneRenderContentRevision != request.sceneRenderContentRevision ||
			request.meshMutationObserved ||
			request.trackedMaterialMutationObserved;
	}

	if (request.sourceNeedsCollection)
	{
		NLS_PROFILE_NAMED_SCOPE("RenderScene::BeginSynchronize::CollectProxyUpdates");
		request.frameSerial = ++m_proxyFrameSerial;
		request.proxyBatch = CollectProxyUpdates(scene, options, request.frameSerial);
	}

	return request;
}

RenderSceneSyncStats RenderScene::CompleteSynchronize(
	RenderSceneSyncRequest request,
	const RenderSceneSyncOptions& options)
{
	if (!NLS::Render::Context::RenderingThreadSystem::IsInRenderingThread())
		throw std::logic_error("RenderScene synchronization must resolve on the render thread");
	NLS_PROFILE_NAMED_SCOPE("RenderScene::CompleteSynchronize");
	const auto syncStart = std::chrono::steady_clock::now();
	RenderSceneSyncStats stats;
	m_lastDirtySyncHandles.clear();
	m_lastRemovedHandles.clear();

	// Base state for the "did anything change?" decisions below. Read once so the
	// whole resolve decides against one consistent snapshot.
	const auto resolvedBefore = ReadResolvedSynchronizationState();
	// Carried out of the resolve block: the state published below must record the
	// serial the store was actually observed at, including when no sweep ran.
	uint64_t observedStoreChangeSerial = resolvedBefore.storeChangeSerial;

	if (!request.IsValid())
	{
		// Nothing was captured, so there is no safe state transition to perform.
		stats.syncTimeNs = ElapsedNanoseconds(syncStart);
		return stats;
	}

	// Re-derived from the same options the capture used, so it compares equal to
	// the stamp that drove the fast-path decision.
	const auto synchronizationStamp =
		BuildSceneSynchronizationStamp(request.sceneIdentity, options);
	const auto store = request.proxyStore != nullptr
		? request.proxyStore
		: SceneRenderStoreRegistry::Acquire(request.sceneInstanceId, request.sceneIdentity).store;
	const bool sceneIdentityChanged =
		!resolvedBefore.hasSynchronizationStamp ||
		resolvedBefore.synchronizationStamp.scene != request.sceneIdentity ||
		GetProxyStoreHandle() != store;
	const bool synchronizationOptionsChanged =
		!resolvedBefore.hasSynchronizationStamp ||
		!(resolvedBefore.synchronizationStamp == synchronizationStamp);
	{
		std::lock_guard lock(m_proxyHandleMutex);
		m_proxyStore = store;
		m_proxySceneIdentity = request.sceneIdentity;
	}
	const bool canReuseProxySynchronization =
		request.canReuseProxySynchronization &&
		!sceneIdentityChanged && !synchronizationOptionsChanged &&
		store != nullptr && store->GetChangeSerial() == resolvedBefore.storeChangeSerial;

	if (canReuseProxySynchronization)
	{
		stats.reusedPrimitiveCount = static_cast<uint64_t>(m_livePrimitiveCount);
		stats.usedSceneRenderContentRevisionFastPath = true;
	}
	else
	{
		SceneRenderStoreApplyStats applyStats;
		if (request.sourceNeedsCollection)
		{
			NLS_PROFILE_NAMED_SCOPE("RenderScene::CompleteSynchronize::ApplyProxyUpdates");
			applyStats = ApplyProxyUpdates(request.proxyBatch);
		}

		const bool forceFullResync = sceneIdentityChanged || synchronizationOptionsChanged;
		const bool hasPendingDeclaredTextures =
			!m_pendingDeclaredTextureProxyKeys.empty();
		const std::shared_ptr<SceneRenderStore> resolveStore = GetProxyStoreHandle();
		const bool proxySynchronizationNeeded =
			forceFullResync ||
			hasPendingDeclaredTextures ||
			(resolveStore != nullptr &&
			 resolveStore->GetChangeSerial() != resolvedBefore.storeChangeSerial);
		bool observedStoreChange = false;
		if (proxySynchronizationNeeded)
		{
			NLS_PROFILE_NAMED_SCOPE("RenderScene::CompleteSynchronize::SynchronizeProxyPrimitives");
			observedStoreChange = SynchronizeProxyPrimitives(
				options,
				stats,
				forceFullResync,
				resolvedBefore.storeChangeSerial,
				observedStoreChangeSerial);
		}
		else
		{
			stats.reusedPrimitiveCount = static_cast<uint64_t>(m_livePrimitiveCount);
		}

		const bool primitiveStateChanged =
			forceFullResync ||
			stats.addedPrimitiveCount != 0u ||
			stats.removedPrimitiveCount != 0u ||
			!m_lastDirtySyncHandles.empty() ||
			!m_lastRemovedHandles.empty();
		if (primitiveStateChanged)
		{
			RefreshTrackedMaterialStates();
			{
				NLS_PROFILE_NAMED_SCOPE("RenderScene::Synchronize::RefreshSpatialIndex");
				RefreshSpatialIndex(options);
			}
			const bool importedHierarchyHLODEnabled =
				ResolveLargeSceneSettings(options).enableHLOD;
			if (importedHierarchyHLODEnabled)
			{
				NLS_PROFILE_NAMED_SCOPE("RenderScene::Synchronize::RebuildImportedHierarchyHLODRecords");
				RebuildImportedHierarchyHLODRecords();
			}
			else
			{
				NLS_PROFILE_NAMED_SCOPE("RenderScene::Synchronize::ClearImportedHierarchyHLODRecords");
				ClearImportedHierarchyHLODRecords();
			}
			{
				NLS_PROFILE_NAMED_SCOPE("RenderScene::Synchronize::RegisterIntrinsicLODGroups");
				bool representationLookupDirty = false;
				for (auto& primitive : m_primitives)
				{
					if (!primitive.occupied || primitive.tombstoned)
						continue;
					const bool hasIntrinsicGroup = primitive.lodGroup.has_value() &&
						primitive.lodGroup->index < m_representationRegistry->lodGroups.size() &&
						m_representationRegistry->lodGroups[primitive.lodGroup->index].selectsMeshLOD;
					if (primitive.mesh == nullptr || primitive.mesh->GetLODCount() <= 1u)
					{
						if (hasIntrinsicGroup)
						{
							m_representationRegistry->lodGroups[primitive.lodGroup->index] = {};
							primitive.lodGroup.reset();
							representationLookupDirty = true;
						}
						continue;
					}
					if (primitive.lodGroup.has_value() && !hasIntrinsicGroup)
						continue;
					if (hasIntrinsicGroup &&
						IntrinsicLODGroupRecordMatches(
							m_representationRegistry->lodGroups[primitive.lodGroup->index],
							primitive,
							*primitive.mesh))
					{
						continue;
					}
					LODGroupRecord group;
					if (hasIntrinsicGroup)
						group.groupHandle = *primitive.lodGroup;
					group.selectsMeshLOD = true;
					group.worldReferencePoint = primitive.worldPosition;
					group.worldSize = std::max(primitive.modelBoundingSphere.radius * 2.0f, 1.0f);
					group.boundsSphereRadius = primitive.modelBoundingSphere.radius;
					group.minLOD = primitive.mesh->GetMinLOD();
					for (uint32_t lod = 0u; lod < primitive.mesh->GetLODCount(); ++lod)
						group.levels.push_back({primitive.mesh->GetLODScreenSize(lod), {primitive.handle}});
					bool membershipChanged = false;
					RegisterLODGroup(group, false, &membershipChanged);
					representationLookupDirty = representationLookupDirty || membershipChanged;
				}
				if (representationLookupDirty)
					m_representationRegistry->RebuildLookup();
			}
			m_spatialCandidateSnapshotCache.reset();
		}

		const bool appliedStoreChange =
			applyStats.addedProxyCount != 0u ||
			applyStats.updatedTransformCount != 0u ||
			applyStats.updatedPayloadCount != 0u ||
			applyStats.removedProxyCount != 0u;
		stats.detectedRenderContentChange =
			request.trackedMaterialMutationObserved ||
			request.meshMutationObserved ||
			synchronizationOptionsChanged ||
			appliedStoreChange ||
			observedStoreChange ||
			stats.addedPrimitiveCount != 0u ||
			stats.removedPrimitiveCount != 0u ||
			stats.rebuiltCachedCommandCount != 0u ||
			stats.rebuiltOpaqueSortTokenCount != 0u ||
			stats.boundsDirtyPrimitiveCount != 0u;
	}
	// Publish as one block: the capture half reads all of these together to decide
	// whether it may reuse this synchronization, so a partial update would let it
	// skip collecting changes this resolve had not applied.
	ResolvedSceneSynchronizationState resolvedAfter;
	resolvedAfter.sceneRenderContentRevision = request.sceneRenderContentRevision;
	resolvedAfter.meshMutationEpoch = request.meshMutationEpoch;
	resolvedAfter.storeChangeSerial = observedStoreChangeSerial;
	resolvedAfter.hasPendingDeclaredTextures = !m_pendingDeclaredTextureProxyKeys.empty();
	resolvedAfter.hasSynchronizationStamp = true;
	resolvedAfter.synchronizationStamp = synchronizationStamp;
	PublishResolvedSynchronizationState(resolvedAfter);
	m_lastSyncStats = stats;
	stats.syncTimeNs = ElapsedNanoseconds(syncStart);
	m_lastSyncStats.syncTimeNs = stats.syncTimeNs;
	RefreshSyncTelemetry(stats);

	// Mark the capture resolved only now that the sweep is finished: the claim
	// guard exists because the sweep mutates state the capture reads, so releasing
	// it earlier would re-open that window.
	ReleaseCaptureClaim(request.captureSerial);
	return stats;
}

SceneRenderStoreApplyStats RenderScene::ApplyDroppedSynchronizeCapture(
	const RenderSceneSyncRequest& request)
{
	if (!NLS::Render::Context::RenderingThreadSystem::IsInRenderingThread())
		throw std::logic_error("Dropped scene updates require the render thread");
	if (!request.IsValid())
		return {};

	// Apply whatever the capture collected. The registry cursor has already moved
	// past these updates, so dropping them here would lose them outright.
	SceneRenderStoreApplyStats applyStats;
	if (!request.proxyBatch.commands.empty() && request.proxyStore != nullptr)
	{
		applyStats = request.proxyStore->Apply(request.proxyBatch);
		request.proxyStore->RetireResources();
	}

	// Deliberately no primitive rebuild: this frame will not be gathered or drawn.
	// The next CompleteSynchronize sees the store change serial move and rebuilds.
	ReleaseCaptureClaim(request.captureSerial);
	return applyStats;
}

void RenderScene::ReleaseCaptureClaim(const uint64_t captureSerial) noexcept
{
	if (captureSerial == 0u)
		return;

	// Removes exactly this claim. Erasing by serial rather than popping the front
	// also releases a claim that resolves out of order, so resolving a newer
	// capture first cannot leave an older one permanently occupying a slot.
	std::lock_guard lock(m_outstandingSynchronizationMutex);
	const auto claim = std::find(
		m_outstandingSynchronizations.begin(),
		m_outstandingSynchronizations.end(),
		captureSerial);
	if (claim != m_outstandingSynchronizations.end())
	{
		m_outstandingSynchronizations.erase(claim);
	}
}

void RenderScene::SetMaxOutstandingSynchronizations(const size_t maxOutstanding)
{
	std::lock_guard lock(m_outstandingSynchronizationMutex);
	// At least one: a limit of zero would refuse every capture and the scene could
	// never render.
	m_maxOutstandingSynchronizations = (std::max)(size_t { 1u }, maxOutstanding);
}

size_t RenderScene::GetMaxOutstandingSynchronizations() const
{
	std::lock_guard lock(m_outstandingSynchronizationMutex);
	return m_maxOutstandingSynchronizations;
}

size_t RenderScene::GetOutstandingSynchronizationCount() const
{
	std::lock_guard lock(m_outstandingSynchronizationMutex);
	return m_outstandingSynchronizations.size();
}

bool RenderScene::HasOutstandingSynchronization(const uint64_t captureSerial) const
{
	if (captureSerial == 0u)
		return false;

	std::lock_guard lock(m_outstandingSynchronizationMutex);
	return std::find(
		m_outstandingSynchronizations.begin(),
		m_outstandingSynchronizations.end(),
		captureSerial) != m_outstandingSynchronizations.end();
}

bool RenderScene::SynchronizeProxyPrimitives(
	const RenderSceneSyncOptions& options,
	RenderSceneSyncStats& stats,
	const bool forceFullResync,
	const uint64_t baseStoreChangeSerial,
	uint64_t& outObservedStoreChangeSerial)
{
	const std::shared_ptr<SceneRenderStore> store = GetProxyStoreHandle();
	if (store == nullptr)
		return false;

	m_syncMeshLODRevisionDigestCache.clear();

	// Copy the proxies this pass needs under one short read view, then release it
	// before the expensive per-proxy work.
	//
	// The store's per-call read helpers release their lock before returning, so a
	// payload/transform pointer handed back by one of them is unprotected the
	// instant the call returns, and synchronizeProxy reads far more than one field.
	// Holding the view for the whole sweep would close that hole but would also
	// block every other render scene sharing this store from applying its updates
	// for the whole sweep, declared-texture resolution included.
	//
	// Copying first keeps the lock hold proportional to the copied proxy count and
	// lets the sweep work from one consistent snapshot, so the change serial this
	// pass records is the serial those copies came from.
	struct StoreProxySnapshot final
	{
		SceneProxyHandle handle;
		SceneProxyPayload payload;
		SceneProxyTransformState transform;
	};

	std::vector<StoreProxySnapshot> snapshots;
	std::vector<SceneProxyHandle> missingHandles;
	std::vector<SceneProxyHandle> removedHandles;
	uint64_t observedChangeSerial = baseStoreChangeSerial;
	bool changesRequiredFullResync = false;

	{
		const auto view = store->Read();
		const auto changes = view.GetChangesSince(baseStoreChangeSerial);
		observedChangeSerial = changes.changeSerial;
		changesRequiredFullResync = changes.requiresFullResync;

		if (forceFullResync || changes.requiresFullResync)
		{
			snapshots.reserve(view.GetProxyCount());
			view.ForEachProxy(
				[&snapshots](
					const SceneProxyHandle proxyHandle,
					const SceneProxyPayload& payload,
					const SceneProxyTransformState& transform)
				{
					if (payload.kind != SceneProxyKind::Primitive)
						return;
					snapshots.push_back(StoreProxySnapshot { proxyHandle, payload, transform });
				});
		}
		else
		{
			removedHandles = changes.removedHandles;

			std::vector<SceneProxyHandle> handlesToSynchronize = changes.changedHandles;
			handlesToSynchronize.reserve(
				handlesToSynchronize.size() + m_pendingDeclaredTextureProxyKeys.size());
			for (const uint64_t proxyKey : m_pendingDeclaredTextureProxyKeys)
			{
				const auto found = m_primitiveIndexByProxy.find(proxyKey);
				if (found == m_primitiveIndexByProxy.end())
					continue;
				const auto& primitive = m_primitives[found->second];
				if (primitive.occupied && !primitive.tombstoned)
					handlesToSynchronize.push_back(primitive.proxyHandle);
			}

			std::unordered_set<uint64_t> requestedProxyKeys;
			requestedProxyKeys.reserve(handlesToSynchronize.size());
			snapshots.reserve(handlesToSynchronize.size());
			for (const auto proxyHandle : handlesToSynchronize)
			{
				const uint64_t proxyKey = MakeSceneProxyKey(proxyHandle);
				if (!requestedProxyKeys.insert(proxyKey).second)
					continue;

				const SceneProxyPayload* payload = nullptr;
				const SceneProxyTransformState* transform = nullptr;
				if (!view.TryGetProxy(proxyHandle, payload, transform) ||
					payload == nullptr || transform == nullptr)
				{
					missingHandles.push_back(proxyHandle);
					continue;
				}

				snapshots.push_back(StoreProxySnapshot { proxyHandle, *payload, *transform });
			}
		}
	}

	const bool fullResync = forceFullResync || changesRequiredFullResync;
	const bool observedStoreChange =
		changesRequiredFullResync ||
		observedChangeSerial != baseStoreChangeSerial;
	RenderSceneDeclaredTextureLookupCache declaredTextureCache;

	const auto unlinkFreePrimitiveSlot = [this](const size_t primitiveIndex)
	{
		if (primitiveIndex >= m_primitives.size())
			return;
		if (m_firstFreePrimitiveSlot.has_value() && *m_firstFreePrimitiveSlot == primitiveIndex)
		{
			m_firstFreePrimitiveSlot = m_primitives[primitiveIndex].nextFreePrimitiveSlot;
			m_primitives[primitiveIndex].nextFreePrimitiveSlot.reset();
			return;
		}

		auto current = m_firstFreePrimitiveSlot;
		for (size_t visited = 0u; current.has_value() && visited < m_primitives.size(); ++visited)
		{
			auto& slot = m_primitives[*current];
			if (slot.nextFreePrimitiveSlot.has_value() &&
				*slot.nextFreePrimitiveSlot == primitiveIndex)
			{
				slot.nextFreePrimitiveSlot = m_primitives[primitiveIndex].nextFreePrimitiveSlot;
				m_primitives[primitiveIndex].nextFreePrimitiveSlot.reset();
				return;
			}
			current = slot.nextFreePrimitiveSlot;
		}
	};

	const auto synchronizeProxy = [this,
		&options,
		&stats,
		&declaredTextureCache,
		&unlinkFreePrimitiveSlot](
			const SceneProxyHandle proxyHandle,
			const SceneProxyPayload& payload,
			const SceneProxyTransformState& transform)
	{
		if (payload.kind != SceneProxyKind::Primitive)
			return;

		const uint64_t proxyKey = MakeSceneProxyKey(proxyHandle);
		m_pendingDeclaredTextureProxyKeys.erase(proxyKey);
		++stats.syncTouchedPrimitiveCount;

		const size_t primitiveIndex = proxyHandle.index;
		if (primitiveIndex >= m_primitives.size())
			m_primitives.resize(primitiveIndex + 1u);

		auto& primitive = m_primitives[primitiveIndex];
		if (primitive.occupied && !(primitive.proxyHandle == proxyHandle))
			TombstonePrimitive(primitiveIndex, stats);

		const bool newlyAdded = !primitive.occupied || primitive.tombstoned;
		if (newlyAdded)
		{
			unlinkFreePrimitiveSlot(primitiveIndex);
			const auto freeStableIndex = std::find(
				m_freeStableObjectIndices.begin(),
				m_freeStableObjectIndices.end(),
				proxyHandle.index);
			if (freeStableIndex != m_freeStableObjectIndices.end())
				m_freeStableObjectIndices.erase(freeStableIndex);

			primitive = {};
			primitive.proxyHandle = proxyHandle;
			primitive.handle = {
				m_sceneId,
				static_cast<uint32_t>(primitiveIndex),
				proxyHandle.generation
			};
			primitive.occupied = true;
			primitive.tombstoned = false;
			primitive.stableObjectIndex = proxyHandle.index <
				NLS::Render::Data::GetMaxObjectDataCount()
				? proxyHandle.index
				: NLS::Render::Data::DrawableObjectDescriptor::kInvalidStableObjectIndex;
			++m_livePrimitiveCount;
			++stats.addedPrimitiveCount;
		}
		else
		{
			++stats.reusedPrimitiveCount;
		}
		m_primitiveIndexByProxy[proxyKey] = primitiveIndex;

		const auto previousStamp = primitive.lastInputStamp;
		const auto previousBounds = primitive.modelBoundingSphere;
		const auto previousModelBounds = primitive.modelBounds;
		const auto previousWorldMatrix = primitive.worldMatrix;
		const auto previousUserMatrix = primitive.userMatrix;
		const auto previousOwnerInstanceId = primitive.ownerInstanceId;

		primitive.ownerInstanceId = static_cast<NLS::InstanceID>(payload.ownerInstanceId);
		primitive.parentInstanceId = static_cast<NLS::InstanceID>(payload.parentInstanceId);
		primitive.hlodScopeRootInstanceId = payload.hlodScopeRootInstanceId;
		primitive.sourceObjectKey = payload.sourceObjectKey;
		primitive.hlodScopeMetadata = payload.hlodScopeMetadata;
		primitive.mesh = payload.resources.mesh;
		primitive.meshRendererInstanceId = NLS::InstanceID_None;
		primitive.owner = nullptr;
		primitive.meshRenderer = nullptr;
		primitive.meshFilter = nullptr;
		primitive.worldMatrix = transform.worldMatrix;
		primitive.worldPosition = transform.worldPosition;
		primitive.userMatrix = payload.userMatrix;
		primitive.customBoundingSphere = payload.customBoundingSphere;
		primitive.frustumBehaviour = static_cast<Components::MeshRenderer::EFrustumBehaviour>(
			payload.frustumBehaviour);
		primitive.transientRenderingSuppressed = payload.transientRenderingSuppressed;
		primitive.visibilitySettings.layer = payload.layer;
		primitive.ownerAlive = payload.ownerAlive;
		primitive.ownerActive = payload.ownerActive;
		primitive.modelBoundingSphere = primitive.frustumBehaviour ==
			Components::MeshRenderer::EFrustumBehaviour::CULL_CUSTOM
			? payload.customBoundingSphere
			: payload.modelBoundingSphere;
		primitive.modelBounds = payload.modelBounds;
		primitive.hasMeshBinding = payload.resources.mesh != nullptr;

		if (previousOwnerInstanceId != NLS::InstanceID_None &&
			previousOwnerInstanceId != primitive.ownerInstanceId)
		{
			m_parentByOwnerInstanceId.erase(previousOwnerInstanceId);
			for (size_t index = 0u; index < m_primitives.size(); ++index)
			{
				if (index == primitiveIndex)
					continue;
				const auto& candidate = m_primitives[index];
				if (candidate.occupied && !candidate.tombstoned &&
					candidate.ownerInstanceId == previousOwnerInstanceId)
				{
					m_parentByOwnerInstanceId[previousOwnerInstanceId] =
						candidate.parentInstanceId;
					break;
				}
			}
		}
		if (primitive.ownerInstanceId != NLS::InstanceID_None)
		{
			m_parentByOwnerInstanceId[primitive.ownerInstanceId] =
				primitive.parentInstanceId;
		}

		PrimitiveInputStamp stamp;
		stamp.proxyHandle = proxyHandle;
		stamp.ownerInstanceId = primitive.ownerInstanceId;
		stamp.mesh = payload.resources.mesh;
		stamp.worldMatrix = transform.worldMatrix;
		stamp.layer = payload.layer;
		stamp.ownerRenderRevision = payload.ownerRenderRevision;
		stamp.transformRenderRevision = transform.transformRevision;
		stamp.meshRendererRenderRevision = payload.meshRendererRenderRevision;
		stamp.meshContentRevision = payload.resources.meshContentRevision;
		stamp.meshLODRevisionDigest = primitive.mesh != nullptr
			? GetMeshLODRevisionDigestForSynchronization(*primitive.mesh)
			: 0u;
		stamp.material = payload.resources.material;
		stamp.materialInstanceId = payload.resources.materialInstanceId;
		stamp.requestedMaterialInstanceId = payload.resources.requestedMaterialInstanceId;
		stamp.materialParameterRevision = payload.resources.materialParameterRevision;
		stamp.materialRenderStateRevision = payload.resources.materialRenderStateRevision;
		stamp.requireExplicitMaterialTextures = options.requireExplicitMaterialTextures;
		stamp.allowDefaultMaterialForUnresolvedExplicitMaterials =
			options.allowDefaultMaterialForUnresolvedExplicitMaterials;
		stamp.ownerAlive = payload.ownerAlive;
		stamp.ownerActive = payload.ownerActive;

		if (stamp.material != nullptr && !stamp.material->GetTextureResourcePaths().empty())
		{
			TryBindDeclaredMaterialTexturesFromCache(
				*stamp.material,
				declaredTextureCache,
				stats);
			stamp.explicitMaterialTexturesResolved =
				HasResolvedDeclaredMaterialTexturesCached(
					*stamp.material,
					declaredTextureCache);
			if (!stamp.explicitMaterialTexturesResolved)
				m_pendingDeclaredTextureProxyKeys.insert(proxyKey);
		}

		const bool explicitMaterialBlocked =
			payload.resources.hasExplicitMaterialPath &&
			!payload.resources.materialResolved &&
			!options.allowDefaultMaterialForUnresolvedExplicitMaterials;
		const bool explicitTexturesBlocked =
			options.requireExplicitMaterialTextures &&
			payload.resources.hasExplicitMaterialPath &&
			!stamp.explicitMaterialTexturesResolved;
		const bool validMaterial = stamp.material != nullptr &&
			stamp.material->IsValid() &&
			!explicitMaterialBlocked &&
			!explicitTexturesBlocked;
		primitive.hasValidMaterial = validMaterial;

		if (primitive.mesh == nullptr || !validMaterial)
		{
			if (!primitive.cachedCommands.empty())
			{
				MarkCommandOffsetTableDirty();
				primitive.cachedCommands.clear();
			}
		}
		else
		{
			if (primitive.cachedCommands.size() != 1u)
				MarkCommandOffsetTableDirty();
			primitive.cachedCommands.resize(1u);
			auto& slot = primitive.cachedCommands[0];
			const auto commandStamp = BuildCommandInputStamp(
				primitive,
				*primitive.mesh,
				*stamp.material);
			if (!slot.valid || slot.stamp != commandStamp)
				RebuildCachedCommand(slot, commandStamp, stats);
		}

		const bool boundsChanged =
			!AreSameBounds(previousBounds, primitive.modelBoundingSphere) ||
			!AreSameBounds(previousModelBounds, primitive.modelBounds) ||
			!AreSameMatrix(previousWorldMatrix, primitive.worldMatrix) ||
			!AreSameMatrix(previousUserMatrix, primitive.userMatrix);
		const bool changed = newlyAdded || previousStamp != stamp || boundsChanged;
		if (changed)
			MarkPrimitiveDirtyForSnapshot(primitive);
		if (boundsChanged)
			++stats.boundsDirtyPrimitiveCount;
		primitive.lastInputStamp = stamp;
	};

	if (fullResync)
	{
		++stats.syncFullSweepCount;
		m_parentByOwnerInstanceId.clear();
		m_pendingDeclaredTextureProxyKeys.clear();
		std::unordered_set<uint64_t> liveProxyKeys;
		liveProxyKeys.reserve(snapshots.size());
		for (const auto& snapshot : snapshots)
		{
			liveProxyKeys.insert(MakeSceneProxyKey(snapshot.handle));
			synchronizeProxy(snapshot.handle, snapshot.payload, snapshot.transform);
		}

		stats.syncSweepTouchedSlotCount += m_primitiveIndexByProxy.size();
		for (auto iterator = m_primitiveIndexByProxy.begin();
			 iterator != m_primitiveIndexByProxy.end();)
		{
			if (liveProxyKeys.find(iterator->first) == liveProxyKeys.end())
			{
				const size_t stalePrimitiveIndex = iterator->second;
				++iterator;
				TombstonePrimitive(stalePrimitiveIndex, stats);
			}
			else
			{
				++iterator;
			}
		}
	}
	else
	{
		for (const auto removedHandle : removedHandles)
		{
			const uint64_t proxyKey = MakeSceneProxyKey(removedHandle);
			m_pendingDeclaredTextureProxyKeys.erase(proxyKey);
			const auto found = m_primitiveIndexByProxy.find(proxyKey);
			if (found != m_primitiveIndexByProxy.end())
				TombstonePrimitive(found->second, stats);
		}

		for (const auto& snapshot : snapshots)
			synchronizeProxy(snapshot.handle, snapshot.payload, snapshot.transform);

		for (const auto handle : missingHandles)
		{
			const auto found = m_primitiveIndexByProxy.find(MakeSceneProxyKey(handle));
			if (found != m_primitiveIndexByProxy.end())
				TombstonePrimitive(found->second, stats);
		}
	}

	outObservedStoreChangeSerial = observedChangeSerial;
	if (stats.syncTouchedPrimitiveCount == 0u &&
		stats.addedPrimitiveCount == 0u &&
		stats.removedPrimitiveCount == 0u)
	{
		stats.reusedPrimitiveCount = static_cast<uint64_t>(m_livePrimitiveCount);
	}
	return observedStoreChange;
}

bool RenderScene::HasTrackedMaterialMutation()
{
	const auto mutationEpoch = NLS::Render::Resources::Material::GetGlobalMutationEpoch();
	if (mutationEpoch == m_lastObservedMaterialMutationEpoch)
		return false;
	const auto observedEpoch = m_lastObservedMaterialMutationEpoch;
	std::array<NLS::Render::Resources::MaterialMutationRecord, 512u> mutations;
	size_t mutationCount = 0u;
	const bool hasCompleteMutationJournal =
		NLS::Render::Resources::Material::GetMutationsSince(
			observedEpoch,
			mutations.data(),
			mutations.size(),
			mutationCount);
	m_lastObservedMaterialMutationEpoch = mutationEpoch;
	if (!hasCompleteMutationJournal)
		return true;

	// Membership is tested against the resolve half's published snapshot, never
	// against the live sets: those are rebuilt on the render thread, and this half
	// runs on the game thread. A null snapshot means no resolve has published yet,
	// so nothing is tracked and nothing can match.
	const auto trackedKeys = ReadTrackedMaterialKeys();
	if (trackedKeys == nullptr)
		return false;

	for (size_t mutationIndex = 0u; mutationIndex < mutationCount; ++mutationIndex)
	{
		const auto& mutation = mutations[mutationIndex];
		if (trackedKeys->materials.find(mutation.material) == trackedKeys->materials.end() &&
			trackedKeys->instanceIds.find(mutation.instanceId) ==
				trackedKeys->instanceIds.end())
			continue;

		return true;
	}

	return false;
}

void RenderScene::RefreshTrackedMaterialStates()
{
	m_trackedMaterialStates.clear();
	m_trackedMaterialInstanceIds.clear();
	const auto trackMaterial = [this](NLS::Render::Resources::Material* material)
	{
		if (material == nullptr || !NLS::Render::Resources::Material::IsLive(material))
			return;
		m_trackedMaterialStates[material] = {
			material->GetInstanceId(),
			material->GetParameterRevision(),
			material->GetRenderStateRevision(),
			material->GetBindingRevision()
		};
		m_trackedMaterialInstanceIds.insert(material->GetInstanceId());
	};

	for (const auto& primitive : m_primitives)
	{
		if (!primitive.occupied || primitive.tombstoned)
			continue;
		trackMaterial(primitive.lastInputStamp.material);
		if (primitive.lastInputStamp.requestedMaterialInstanceId != 0u)
		{
			m_trackedMaterialInstanceIds.insert(
				primitive.lastInputStamp.requestedMaterialInstanceId);
		}
		for (const auto& slot : primitive.cachedCommands)
		{
			if (slot.valid)
				trackMaterial(slot.stamp.material);
		}
	}

	// Publish an immutable copy so the capture half can test membership without
	// reading the sets this function is rewriting.
	auto keys = std::make_shared<TrackedMaterialKeys>();
	keys->materials.reserve(m_trackedMaterialStates.size());
	for (const auto& [material, state] : m_trackedMaterialStates)
	{
		(void)state;
		keys->materials.insert(material);
	}
	keys->instanceIds = m_trackedMaterialInstanceIds;
	PublishTrackedMaterialKeys(std::move(keys));
}

RenderScene::SceneSynchronizationStamp RenderScene::BuildSceneSynchronizationStamp(
	const SceneSystem::Scene* scene,
	const RenderSceneSyncOptions& options)
{
	SceneSynchronizationStamp stamp;
	stamp.scene = scene;
	stamp.defaultMaterial = options.defaultMaterial;
	stamp.overrideMaterial = options.overrideMaterial;
	stamp.requireExplicitMaterialTextures = options.requireExplicitMaterialTextures;
	stamp.allowDefaultMaterialForUnresolvedExplicitMaterials =
		options.allowDefaultMaterialForUnresolvedExplicitMaterials;
	const auto& largeSceneSettings = ResolveLargeSceneSettings(options);
	stamp.enableSpatialIndex = largeSceneSettings.enableSpatialIndex;
	stamp.enableHLOD = largeSceneSettings.enableHLOD;
	stamp.staticRebuildDirtyRatio = largeSceneSettings.staticRebuildDirtyRatio;
	stamp.staticRebuildBudgetUs = largeSceneSettings.staticRebuildBudgetUs;
	return stamp;
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
	const auto visibility = [&]()
	{
		NLS_PROFILE_NAMED_SCOPE("RenderScene::GatherVisibleCommands::EvaluateVisibility");
		return EvaluateVisibility(options, mode, meshBaseIndices, false);
	}();
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

			auto command = slot.command;
			const auto selectedLOD = primitiveIndex < visibility.selectedLODByPrimitive.size()
				? visibility.selectedLODByPrimitive[primitiveIndex]
				: 0u;
			command.mesh = command.mesh->GetLODMesh(selectedLOD);
			command.opaqueSortToken = EngineDrawableDescriptor::kInvalidOpaqueSortToken;
			if (selectedLOD < slot.opaqueSortTokensByLOD.size())
			{
				const auto& tokenEntry = slot.opaqueSortTokensByLOD[selectedLOD];
				if (tokenEntry.mesh == command.mesh &&
					command.mesh != nullptr &&
					tokenEntry.meshContentRevision == command.mesh->GetContentRevision())
				{
					command.opaqueSortToken = tokenEntry.token;
				}
			}
			AppendVisibleDrawable(output, primitive, command, options);
			++m_lastDrawCallOptimizationStats.rawVisibleObjectCount;
			++m_lastLargeSceneTelemetry.rawVisibleDrawCount;
		}
	};

	{
		NLS_PROFILE_NAMED_SCOPE("RenderScene::GatherVisibleCommands::BuildVisibleDrawables");
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
	}
	m_lastLargeSceneTelemetry.visibleDrawableBuildTimeNs = ElapsedNanoseconds(finalizationStart);

	{
		NLS_PROFILE_NAMED_SCOPE("RenderScene::GatherVisibleCommands::FinalizeOpaqueQueue");
		const auto opaqueQueueFinalizationStart = std::chrono::steady_clock::now();
		FinalizeOpaqueQueue(output.opaques);
		m_lastLargeSceneTelemetry.opaqueQueueFinalizationTimeNs =
			ElapsedNanoseconds(opaqueQueueFinalizationStart);
	}
	{
		NLS_PROFILE_NAMED_SCOPE("RenderScene::GatherVisibleCommands::SortTransparentQueues");
		SortVisibleQueue(output.decals, std::greater<float>{});
		SortVisibleQueue(output.transparents, std::greater<float>{});
	}
	{
		NLS_PROFILE_NAMED_SCOPE("RenderScene::GatherVisibleCommands::AssignVisibleObjectIndices");
		const auto visibleObjectIndexAssignmentStart = std::chrono::steady_clock::now();
		AssignVisibleObjectIndices(output);
		m_lastLargeSceneTelemetry.visibleObjectIndexAssignmentTimeNs =
			ElapsedNanoseconds(visibleObjectIndexAssignmentStart);
	}
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
	m_lastDrawCallOptimizationStats.opaqueSortTokenRebuildCount =
		m_lastSyncStats.rebuiltOpaqueSortTokenCount;
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

uint64_t RenderScene::GetOpaqueSortTokenBuildCountForTesting() const
{
	return m_opaqueSortTokenBuildCount;
}

#if defined(NLS_ENABLE_TEST_HOOKS)
uint64_t RenderScene::GetSpatialCandidateSnapshotCacheHitCountForTesting() const
{
	return m_spatialCandidateSnapshotCacheHitCount;
}
#endif

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
			!primitive.ownerAlive ||
			!primitive.ownerActive ||
			primitive.proxyHandle.sceneId == 0u ||
			primitive.ownerInstanceId == NLS::InstanceID_None ||
			primitive.mesh == nullptr)
		{
			continue;
		}

		for (const auto& slot : primitive.cachedCommands)
		{
			if (!slot.valid || slot.command.mesh == nullptr || slot.command.material == nullptr)
				continue;

			outSources.push_back({
				primitive.ownerInstanceId,
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
	m_spatialCandidateSnapshotCache.reset();
	m_representationRegistry = std::make_unique<RepresentationRegistry>();
	m_importedHierarchyHLODClusterHandles.clear();
	for (auto& primitive : m_primitives)
	{
		primitive.lodGroup.reset();
		primitive.hlodCluster.reset();
	}
}

void RenderScene::ClearImportedHierarchyHLODRecords(const bool rebuildLookup)
{
	if (m_importedHierarchyHLODClusterHandles.empty())
		return;

	m_spatialCandidateSnapshotCache.reset();
	if (m_representationRegistry != nullptr)
	{
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
	}

	m_importedHierarchyHLODClusterHandles.clear();
	if (rebuildLookup && m_representationRegistry != nullptr)
		m_representationRegistry->RebuildLookup();
}

void RenderScene::RebuildImportedHierarchyHLODRecords()
{
	m_spatialCandidateSnapshotCache.reset();
	ClearImportedHierarchyHLODRecords(false);
	if (m_representationRegistry == nullptr)
		m_representationRegistry = std::make_unique<RepresentationRegistry>();

	struct ImportedHierarchyHLODScope
	{
		uint64_t rootInstanceId = 0u;
		ImportedHierarchyHLODMetadata metadata;
		std::unordered_map<std::string, std::vector<ScenePrimitiveHandle>> primitivesBySourceKey;
	};

	std::vector<ImportedHierarchyHLODScope> scopes;
	std::unordered_map<uint64_t, size_t> scopeIndicesByRoot;
	for (const auto& primitive : m_primitives)
	{
		if (!primitive.occupied || primitive.tombstoned ||
			primitive.hlodScopeRootInstanceId == 0u ||
			primitive.hlodScopeMetadata.empty())
		{
			continue;
		}

		if (scopeIndicesByRoot.find(primitive.hlodScopeRootInstanceId) != scopeIndicesByRoot.end())
			continue;

		const auto metadata = ParseImportedHierarchyHLODMetadata(primitive.hlodScopeMetadata);
		if (!metadata.has_value())
			continue;

		scopeIndicesByRoot[primitive.hlodScopeRootInstanceId] = scopes.size();
		scopes.push_back({ primitive.hlodScopeRootInstanceId, *metadata, {} });
	}

	scopeIndicesByRoot.reserve(scopes.size());
	for (size_t index = 0u; index < scopes.size(); ++index)
	{
		scopeIndicesByRoot[scopes[index].rootInstanceId] = index;
		scopes[index].primitivesBySourceKey.reserve(m_livePrimitiveCount);
	}

	for (const auto& primitive : m_primitives)
	{
		if (!primitive.occupied || primitive.tombstoned ||
			primitive.hlodScopeRootInstanceId == 0u ||
			primitive.sourceObjectKey.empty())
			continue;

		const auto foundScope = scopeIndicesByRoot.find(primitive.hlodScopeRootInstanceId);
		if (foundScope != scopeIndicesByRoot.end())
		{
			scopes[foundScope->second]
				.primitivesBySourceKey[primitive.sourceObjectKey]
				.push_back(primitive.handle);
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

		const auto handle = RegisterHLODCluster(cluster, false);
		m_importedHierarchyHLODClusterHandles.push_back(handle);
	}

	m_representationRegistry->RebuildLookup();
}

namespace
{
	bool LODGroupMembershipMatches(const LODGroupRecord& lhs, const LODGroupRecord& rhs)
	{
		if (lhs.levels.size() != rhs.levels.size())
			return false;
		for (size_t levelIndex = 0u; levelIndex < lhs.levels.size(); ++levelIndex)
		{
			const auto& lhsHandles = lhs.levels[levelIndex].primitiveHandles;
			const auto& rhsHandles = rhs.levels[levelIndex].primitiveHandles;
			if (lhsHandles.size() != rhsHandles.size())
				return false;
			for (size_t handleIndex = 0u; handleIndex < lhsHandles.size(); ++handleIndex)
			{
				if (!(lhsHandles[handleIndex] == rhsHandles[handleIndex]))
					return false;
			}
		}
		return true;
	}
}

SceneLODGroupHandle RenderScene::RegisterLODGroup(
	const LODGroupRecord& group,
	const bool rebuildLookup,
	bool* const outMembershipChanged)
{
	m_spatialCandidateSnapshotCache.reset();
	if (m_representationRegistry == nullptr)
		m_representationRegistry = std::make_unique<RepresentationRegistry>();

	const bool creatingRecord = !group.groupHandle.IsValid();
	const auto handle = creatingRecord
		? SceneLODGroupHandle { static_cast<uint32_t>(m_representationRegistry->lodGroups.size()) }
		: group.groupHandle;
	if (handle.index >= m_representationRegistry->lodGroups.size())
		m_representationRegistry->lodGroups.resize(static_cast<size_t>(handle.index) + 1u);

	// Registration runs every frame for intrinsic LOD groups. Only the volatile fields change
	// while membership stays stable, so updating in place avoids copying the level vectors.
	auto& stored = m_representationRegistry->lodGroups[handle.index];
	const bool membershipChanged = creatingRecord || !LODGroupMembershipMatches(stored, group);
	if (membershipChanged)
	{
		stored = group;
		stored.groupHandle = handle;
	}
	else
	{
		stored.hysteresis = group.hysteresis;
		stored.worldReferencePoint = group.worldReferencePoint;
		stored.worldSize = group.worldSize;
		stored.boundsSphereRadius = group.boundsSphereRadius;
		stored.minLOD = group.minLOD;
		stored.maxLOD = group.maxLOD;
		stored.forcedLOD = group.forcedLOD;
		stored.selectsMeshLOD = group.selectsMeshLOD;
		for (size_t levelIndex = 0u; levelIndex < stored.levels.size(); ++levelIndex)
		{
			stored.levels[levelIndex].screenRelativeThreshold =
				group.levels[levelIndex].screenRelativeThreshold;
			stored.levels[levelIndex].fadeDurationSeconds =
				group.levels[levelIndex].fadeDurationSeconds;
			stored.levels[levelIndex].resident = group.levels[levelIndex].resident;
		}
	}
	m_representationRegistry->ResizeLODHistories();

	for (const auto& level : stored.levels)
	{
		for (const auto primitiveHandle : level.primitiveHandles)
		{
			if (!IsPrimitiveHandleLive(primitiveHandle))
				continue;
			m_primitives[primitiveHandle.index].lodGroup = handle;
		}
	}
	if (rebuildLookup)
		m_representationRegistry->RebuildLookup();
	if (outMembershipChanged != nullptr)
		*outMembershipChanged = membershipChanged;

	return handle;
}

SceneHLODClusterHandle RenderScene::RegisterHLODCluster(const HLODClusterRecord& cluster, const bool rebuildLookup)
{
	m_spatialCandidateSnapshotCache.reset();
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
	if (rebuildLookup)
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
		record.userMatrix = primitive.userMatrix;
		record.frustumBehaviour = primitive.frustumBehaviour;
		record.visibilitySettings = primitive.visibilitySettings;
		record.lodGroup = primitive.lodGroup;
		record.hlodCluster = primitive.hlodCluster;
		record.commandOffsetBegin = commandOffsetBegin;
		record.commandOffsetEnd = commandOffset;
		record.hasMeshBinding = primitive.hasMeshBinding;
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
		record.userMatrix = primitive.userMatrix;
		record.frustumBehaviour = primitive.frustumBehaviour;
		record.visibilitySettings = primitive.visibilitySettings;
		record.lodGroup = primitive.lodGroup;
		record.hlodCluster = primitive.hlodCluster;
		record.commandOffsetBegin = handle.index < meshBaseIndices->size()
			? static_cast<uint64_t>((*meshBaseIndices)[handle.index])
			: 0u;
		record.commandOffsetEnd = record.commandOffsetBegin + static_cast<uint64_t>(primitive.cachedCommands.size());
		record.hasMeshBinding = primitive.hasMeshBinding;
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
	const auto allocateStableObjectIndex = [this]()
	{
		if (!m_freeStableObjectIndices.empty())
		{
			const auto stableObjectIndex = m_freeStableObjectIndices.back();
			m_freeStableObjectIndices.pop_back();
			return stableObjectIndex;
		}
		if (m_nextStableObjectIndex < NLS::Render::Data::GetMaxObjectDataCount())
			return m_nextStableObjectIndex++;
		return NLS::Render::Data::DrawableObjectDescriptor::kInvalidStableObjectIndex;
	};
	if (m_firstFreePrimitiveSlot.has_value())
	{
		const auto primitiveIndex = m_firstFreePrimitiveSlot.value();
		auto& primitive = m_primitives[primitiveIndex];
		m_firstFreePrimitiveSlot = primitive.nextFreePrimitiveSlot;
		const auto generation = IncrementPrimitiveGeneration(primitive.handle.generation);
		primitive = {};
		primitive.stableObjectIndex = allocateStableObjectIndex();
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
	primitive.stableObjectIndex = allocateStableObjectIndex();
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
	if (primitive.proxyHandle.IsValid())
	{
		const uint64_t proxyKey = MakeSceneProxyKey(primitive.proxyHandle);
		m_primitiveIndexByProxy.erase(proxyKey);
		m_pendingDeclaredTextureProxyKeys.erase(proxyKey);
	}
	if (primitive.ownerInstanceId != NLS::InstanceID_None)
	{
		const auto removedOwnerInstanceId = primitive.ownerInstanceId;
		m_parentByOwnerInstanceId.erase(removedOwnerInstanceId);
		for (size_t index = 0u; index < m_primitives.size(); ++index)
		{
			if (index == primitiveIndex)
				continue;
			const auto& candidate = m_primitives[index];
			if (candidate.occupied && !candidate.tombstoned &&
				candidate.ownerInstanceId == removedOwnerInstanceId)
			{
				m_parentByOwnerInstanceId[removedOwnerInstanceId] =
					candidate.parentInstanceId;
				break;
			}
		}
	}

	m_lastRemovedHandles.push_back(primitive.handle);
	primitive.proxyHandle = {};
	primitive.ownerInstanceId = NLS::InstanceID_None;
	primitive.parentInstanceId = NLS::InstanceID_None;
	primitive.hlodScopeRootInstanceId = 0u;
	primitive.sourceObjectKey.clear();
	primitive.hlodScopeMetadata.clear();
	primitive.meshRenderer = nullptr;
	primitive.meshRendererInstanceId = NLS::InstanceID_None;
	primitive.owner = nullptr;
	primitive.mesh = nullptr;
	if (!primitive.cachedCommands.empty())
		MarkCommandOffsetTableDirty();
	primitive.cachedCommands.clear();
	if (primitive.stableObjectIndex != NLS::Render::Data::DrawableObjectDescriptor::kInvalidStableObjectIndex)
		m_freeStableObjectIndices.push_back(primitive.stableObjectIndex);
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
	const auto previousUserMatrix = primitive.userMatrix;
	const auto previousFrustumBehaviour = primitive.frustumBehaviour;
	const auto previousVisibilitySettings = primitive.visibilitySettings;
	const bool previousOwnerAlive = primitive.ownerAlive;
	const bool previousOwnerActive = primitive.ownerActive;
	primitive.owner = meshRenderer->gameobject();
	primitive.ownerAlive = primitive.owner != nullptr && primitive.owner->IsAlive();
	primitive.ownerActive = primitive.owner != nullptr && primitive.owner->IsActive();
	if (auto* transform = primitive.owner != nullptr ? primitive.owner->GetTransform() : nullptr)
	{
		primitive.worldMatrix = transform->GetWorldMatrix();
		primitive.worldPosition = transform->GetWorldPosition();
	}
	else
	{
		primitive.worldMatrix = Maths::Matrix4::Identity;
		primitive.worldPosition = {};
	}
	primitive.userMatrix = meshRenderer->GetUserMatrix();
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
		!AreSameMatrix(previousUserMatrix, primitive.userMatrix) ||
		commandInputChanged)
	{
		MarkPrimitiveDirtyForSnapshot(primitive);
	}
}

RenderScene::PrimitiveInputStamp RenderScene::BuildPrimitiveInputStamp(
	Components::MeshRenderer& meshRenderer,
	const RenderPrimitive& primitive,
	const RenderSceneSyncOptions& options,
	RenderSceneDeclaredTextureLookupCache& declaredTextureCache) const
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
		stamp.meshLODRevisionDigest = stamp.mesh != nullptr
			? GetMeshLODRevisionDigestForSynchronization(*stamp.mesh)
			: 0u;
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
		stamp.material = material;
		stamp.materialInstanceId = material->GetInstanceId();
		stamp.materialParameterRevision = material->GetParameterRevision();
		stamp.materialRenderStateRevision = material->GetRenderStateRevision();
			if (!material->GetTextureResourcePaths().empty())
				stamp.explicitMaterialTexturesResolved =
					HasResolvedDeclaredMaterialTexturesCached(*material, declaredTextureCache);
		}

	return stamp;
}

bool RenderScene::CanReuseSynchronizedPrimitive(
	const RenderPrimitive& primitive,
	const PrimitiveInputStamp& stamp,
	RenderSceneDeclaredTextureLookupCache& declaredTextureCache) const
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
			!HasResolvedDeclaredMaterialTexturesCached(*material, declaredTextureCache))
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
				!HasResolvedDeclaredMaterialTexturesCached(*material, declaredTextureCache))
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
	stamp.meshContentRevision = mesh.GetContentRevision();
	stamp.meshLODRevisionDigest = GetMeshLODRevisionDigestForSynchronization(mesh);
	stamp.materialInstanceId = material.GetInstanceId();
	stamp.materialParameterRevision = material.GetParameterRevision();
	stamp.materialRenderStateRevision = material.GetRenderStateRevision();
	stamp.materialBindingRevision = material.GetBindingRevision();
	if (const auto* shader = material.GetShader(); shader != nullptr)
	{
		stamp.shaderInstanceId = shader->GetInstanceId();
		stamp.shaderGeneration = shader->GetGeneration();
	}
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
	slot.command.opaqueSortToken = EngineDrawableDescriptor::kInvalidOpaqueSortToken;
	slot.opaqueSortTokensByLOD.clear();
	if (slot.command.material != nullptr &&
		!slot.command.material->IsBlendable() &&
		!slot.command.material->IsDecal())
	{
		const auto lodCount = slot.command.mesh != nullptr
			? slot.command.mesh->GetLODCount()
			: 0u;
		slot.opaqueSortTokensByLOD.reserve(lodCount);
		for (uint32_t lodIndex = 0u; lodIndex < lodCount; ++lodIndex)
		{
			auto* lodMesh = slot.command.mesh->GetLODMesh(lodIndex);
			const auto token = BuildOpaqueDrawSortToken(
				lodMesh,
				slot.command.material,
				slot.command.stateMask,
				slot.command.primitiveMode,
				0u,
				0u);
			slot.opaqueSortTokensByLOD.push_back({
				lodMesh,
				lodMesh != nullptr ? lodMesh->GetContentRevision() : 0u,
				token
			});
			++m_opaqueSortTokenBuildCount;
			++stats.rebuiltOpaqueSortTokenCount;
		}
		if (!slot.opaqueSortTokensByLOD.empty())
			slot.command.opaqueSortToken = slot.opaqueSortTokensByLOD.front().token;
	}
	slot.valid = true;
	++m_cachedCommandBuildCount;
	++stats.rebuiltCachedCommandCount;
}

uint64_t RenderScene::GetMeshLODRevisionDigestForSynchronization(
	const NLS::Render::Resources::Mesh& mesh) const
{
	const auto found = m_syncMeshLODRevisionDigestCache.find(&mesh);
	if (found != m_syncMeshLODRevisionDigestCache.end())
		return found->second;

	const auto digest = BuildMeshLODRevisionDigest(mesh);
	m_syncMeshLODRevisionDigestCache.emplace(&mesh, digest);
	return digest;
}

bool RenderScene::IsPrimitiveVisible(
	const RenderPrimitive& primitive,
	const RenderSceneVisibilityOptions& options) const
{
	if (!primitive.occupied || primitive.tombstoned)
		return false;
	if (!primitive.ownerAlive || !primitive.ownerActive)
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

	return options.frustum->BoundsInFrustum(mesh.GetBounds(), primitive.worldMatrix);
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
		// The pipeline decides for itself between the parallel and serial full
		// scan, but only when it is told the caller has no opinion. Forcing
		// Serial here disabled `enableParallelVisibility` for every scene that
		// uses LOD, HLOD or occlusion - i.e. all of them - and silently turned a
		// worker-parallel cull into a serial one on the render thread.
		return EvaluateVisibilityThroughPipeline(
			options,
			meshBaseIndices,
			mode == RenderSceneVisibilityMode::Parallel
				? SceneVisibilityPipelineMode::Parallel
				: mode == RenderSceneVisibilityMode::Serial
					? SceneVisibilityPipelineMode::Serial
					: SceneVisibilityPipelineMode::Auto,
			buildVisibilityBitsets);
	}

	if (mode == RenderSceneVisibilityMode::Parallel)
	{
		return EvaluateVisibilityParallel(options, meshBaseIndices);
	}
	if (mode == RenderSceneVisibilityMode::Auto &&
		settings.enableParallelVisibility &&
		m_livePrimitiveCount >= settings.parallelVisibilityPrimitiveThreshold &&
		NLS::Base::TaskGraph::IsGlobalTaskGraphStarted())
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
	pipelineOptions.verticalFovRadians = options.verticalFovRadians;
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
	snapshot.selectedLODByPrimitive.assign(m_primitives.size(), 0u);
	for (size_t denseIndex = 0u; denseIndex < primitiveSnapshot.primitiveRecords.size(); ++denseIndex)
	{
		const auto primitiveIndex = primitiveSnapshot.primitiveRecords[denseIndex].handle.index;
		if (primitiveIndex < snapshot.selectedLODByPrimitive.size() && denseIndex < pipelineResult.selectedLOD.size() &&
			pipelineResult.selectedLOD[denseIndex] != ~0u)
			snapshot.selectedLODByPrimitive[primitiveIndex] = pipelineResult.selectedLOD[denseIndex];
	}

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
		options.inspectionRootInstanceIds.empty() ||
		m_representationRegistry == nullptr)
	{
		return handles;
	}

	const auto isInsideInspectionRoot = [this, &options](const NLS::InstanceID ownerInstanceId)
	{
		for (const NLS::InstanceID root : options.inspectionRootInstanceIds)
		{
			if (root == NLS::InstanceID_None || ownerInstanceId == NLS::InstanceID_None)
				continue;
			NLS::InstanceID current = ownerInstanceId;
			for (size_t depth = 0u; current != NLS::InstanceID_None && depth < m_livePrimitiveCount + 1u; ++depth)
			{
				if (current == root)
					return true;
				const auto parent = m_parentByOwnerInstanceId.find(current);
				if (parent == m_parentByOwnerInstanceId.end())
					break;
				current = parent->second;
			}
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
			if (!isInsideInspectionRoot(primitive.ownerInstanceId))
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
				mode == RenderSceneVisibilityMode::Parallel
					? SceneVisibilityPipelineMode::Parallel
					: mode == RenderSceneVisibilityMode::Serial
						? SceneVisibilityPipelineMode::Serial
						: SceneVisibilityPipelineMode::Auto,
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

	const auto candidates = [&]()
	{
		NLS_PROFILE_NAMED_SCOPE("RenderScene::EvaluateVisibilitySpatial::QuerySpatialIndex");
		return m_spatialIndex->Query(query);
	}();
	const bool canReuseCandidateSnapshot =
		m_spatialCandidateSnapshotCache != nullptr &&
		m_spatialCandidateSnapshotCache->candidatePrimitiveHandles == candidates.candidatePrimitiveHandles;
	if (!canReuseCandidateSnapshot)
	{
		NLS_PROFILE_NAMED_SCOPE("RenderScene::EvaluateVisibilitySpatial::BuildCandidateSnapshot");
		auto cache = std::make_unique<SpatialCandidateSnapshotCache>();
		cache->candidatePrimitiveHandles = candidates.candidatePrimitiveHandles;
		cache->primitiveSnapshot = CreatePrimitiveSnapshotForHandles(
			candidates.candidatePrimitiveHandles,
			{});
		if (m_representationRegistry != nullptr)
		{
			SceneRepresentationState registryRepresentation;
			registryRepresentation.lodGroups = &m_representationRegistry->lodGroups;
			registryRepresentation.hlodClusters = &m_representationRegistry->hlodClusters;
			registryRepresentation.lodGroupsByPrimitive = &m_representationRegistry->lodGroupsByPrimitive;
			registryRepresentation.hlodClustersByPrimitive = &m_representationRegistry->hlodClustersByPrimitive;
			const auto expansion = SceneVisibilityPipeline::ExpandRepresentationCandidates(
				candidates.candidatePrimitiveHandles,
				cache->primitiveSnapshot,
				registryRepresentation);
			cache->primitiveSnapshot = CreatePrimitiveSnapshotForHandles(
				expansion.primitiveHandles,
				{});

			cache->lodGroups.reserve(expansion.lodGroupIndices.size());
			for (const auto groupIndex : expansion.lodGroupIndices)
			{
				if (groupIndex < m_representationRegistry->lodGroups.size())
					cache->lodGroups.push_back(m_representationRegistry->lodGroups[groupIndex]);
			}
			cache->hlodClusters.reserve(expansion.hlodClusterIndices.size());
			for (const auto clusterIndex : expansion.hlodClusterIndices)
			{
				if (clusterIndex < m_representationRegistry->hlodClusters.size())
					cache->hlodClusters.push_back(m_representationRegistry->hlodClusters[clusterIndex]);
			}
		}
		m_spatialCandidateSnapshotCache = std::move(cache);
	}
	else
	{
		++m_spatialCandidateSnapshotCacheHitCount;
	}

	const auto& initialCandidateSnapshot = m_spatialCandidateSnapshotCache->primitiveSnapshot;
	SceneRepresentationState representation;
	if (m_representationRegistry != nullptr)
	{
		representation.lodGroups = &m_spatialCandidateSnapshotCache->lodGroups;
		representation.hlodClusters = &m_spatialCandidateSnapshotCache->hlodClusters;
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
			m_spatialCandidateSnapshotCache->hlodClusters);
	}
	representation.residency = &residency;
	representation.occlusion = options.occlusion;
	pipelineOptions.lodBias = options.lodBias;
	pipelineOptions.verticalFovRadians = options.verticalFovRadians;
	pipelineOptions.lodHistoryViewKey = options.lodHistoryViewKey;
	pipelineOptions.allowHLOD = options.allowHLOD;
	pipelineOptions.editorInspectionView = options.editorInspectionView;
	pipelineOptions.selectedPrimitiveHandles = options.selectedPrimitiveHandles;
	pipelineOptions.forceInspectableHLODClusters = ResolveInspectableHLODClusters(options);
	const auto pipelineResult = [&]()
	{
		NLS_PROFILE_NAMED_SCOPE("RenderScene::EvaluateVisibilitySpatial::EvaluatePipeline");
		return SceneVisibilityPipeline::Evaluate(
			pipelineOptions,
			initialCandidateSnapshot,
			*m_spatialIndex,
			representation,
			mode == RenderSceneVisibilityMode::Parallel
				? SceneVisibilityPipelineMode::Parallel
				: (mode == RenderSceneVisibilityMode::Auto
					? SceneVisibilityPipelineMode::Auto
					: SceneVisibilityPipelineMode::Serial));
	}();
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
	snapshot.usedParallelEvaluation = pipelineResult.usedParallelEvaluation;
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
	snapshot.selectedLODByPrimitive.assign(m_primitives.size(), 0u);
	for (size_t denseIndex = 0u; denseIndex < initialCandidateSnapshot.primitiveRecords.size(); ++denseIndex)
	{
		const auto primitiveIndex = initialCandidateSnapshot.primitiveRecords[denseIndex].handle.index;
		if (primitiveIndex < snapshot.selectedLODByPrimitive.size() && denseIndex < pipelineResult.selectedLOD.size() &&
			pipelineResult.selectedLOD[denseIndex] != ~0u)
			snapshot.selectedLODByPrimitive[primitiveIndex] = pipelineResult.selectedLOD[denseIndex];
	}

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

	if (!NLS::Base::TaskGraph::IsGlobalTaskGraphStarted())
		return EvaluateVisibilitySerialRange(options, meshBaseIndices, 0u, primitiveCount);

	const auto& settings = ResolveLargeSceneSettings(options);
	const auto primitivesPerTask = std::max<size_t>(1u, settings.parallelVisibilityPrimitivesPerTask);
	const auto hardwareThreads = std::max<uint32_t>(
		1u,
		NLS::Base::TaskGraph::GetGlobalTaskGraph().GetWorkerCount());
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

	auto& taskGraph = NLS::Base::TaskGraph::GetGlobalTaskGraph();
	std::vector<NLS::Base::TaskGraph::TaskHandle> taskHandles;
	taskHandles.reserve(taskCount);
	for (size_t taskIndex = 0u; taskIndex < taskCount; ++taskIndex)
	{
		NLS::Base::TaskGraph::TaskDesc desc;
		desc.targetThread = NLS::Base::TaskGraph::NamedThread::Any;
		desc.priority = NLS::Base::TaskGraph::TaskPriority::High;
		desc.debugName = "RenderScene::EvaluateVisibilityParallel";
		desc.function = [this,
						 &options,
						 &meshBaseIndices,
						 &rangeSnapshots,
						 &rangeCompleted,
						 primitiveCount,
						 rangeSize,
						 taskIndex]
		{
			const auto begin = taskIndex * rangeSize;
			if (begin >= primitiveCount)
				return;

			const auto end = std::min(begin + rangeSize, primitiveCount);
			rangeSnapshots[taskIndex] =
				EvaluateVisibilityCompactRange(options, meshBaseIndices, begin, end);
			rangeCompleted[taskIndex].store(true, std::memory_order_release);
		};
		taskHandles.push_back(taskGraph.Submit(std::move(desc)));
		if (!taskHandles.back().IsValid())
		{
			for (const auto pendingHandle : taskHandles)
			{
				if (pendingHandle.IsValid())
					(void)taskGraph.Wait(pendingHandle);
			}
			return EvaluateVisibilitySerialRange(options, meshBaseIndices, 0u, primitiveCount);
		}
	}

	bool allTasksSucceeded = true;
	for (const auto handle : taskHandles)
	{
		allTasksSucceeded = allTasksSucceeded &&
			taskGraph.Wait(handle) == NLS::Base::TaskGraph::TaskStatus::Succeeded;
	}
	const bool allRangesCompleted = std::all_of(
		rangeCompleted.begin(),
		rangeCompleted.end(),
		[](const std::atomic_bool& completed)
		{
			return completed.load(std::memory_order_acquire);
		});
	if (!allTasksSucceeded || !allRangesCompleted)
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
	m_lastLargeSceneTelemetry.sceneRenderContentRevisionFastPathCount =
		stats.usedSceneRenderContentRevisionFastPath ? 1u : 0u;
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
	if (primitive.ownerInstanceId == NLS::InstanceID_None)
		return;

	NLS::Render::Entities::Drawable drawable;
	drawable.mesh = command.mesh;
	drawable.material = command.material;
	drawable.stateMask = command.stateMask;
	drawable.primitiveMode = command.primitiveMode;
	EngineDrawableDescriptor descriptor{
		primitive.worldMatrix,
		primitive.userMatrix
	};
	descriptor.stableSortKey = primitive.handle.index;
	descriptor.stableObjectIndex = primitive.stableObjectIndex;
	descriptor.opaqueSortToken = command.opaqueSortToken;
	const bool isDecal = command.material != nullptr && command.material->IsDecal();
	const bool isTransparent = command.material != nullptr && command.material->IsBlendable();
	if (!isDecal && !isTransparent)
	{
		PopulateTrustedStaticDrawRevision(
			descriptor,
			primitive.handle,
			primitive.lastInputStamp.transformRenderRevision,
			command);
	}
	drawable.AddDescriptor<EngineDrawableDescriptor>(std::move(descriptor));

	const float distanceToActor = Maths::Vector3::Distance(
		primitive.worldPosition,
		options.cameraPosition);

	if (isDecal)
		output.decals.emplace_back(distanceToActor, std::move(drawable));
	else if (isTransparent)
		output.transparents.emplace_back(distanceToActor, std::move(drawable));
	else
	{
		if (command.opaqueSortToken != EngineDrawableDescriptor::kInvalidOpaqueSortToken)
			++m_lastDrawCallOptimizationStats.opaqueSortTokenHitCount;
		output.opaques.emplace_back(distanceToActor, std::move(drawable));
	}
}

void RenderScene::FinalizeOpaqueQueue(RenderSceneVisibleQueues::SceneDrawables& opaques) const
{
	struct KeyedOpaqueDrawable
	{
		float distance = 0.0f;
		NLS::Render::Entities::Drawable drawable;
		uint64_t opaqueSortToken = EngineDrawableDescriptor::kInvalidOpaqueSortToken;
		uint64_t stablePrimitiveIdentity = EngineDrawableDescriptor::kInvalidStableSortKey;
	};

	std::vector<KeyedOpaqueDrawable> keyedDrawables;
	keyedDrawables.reserve(opaques.size());
	for (auto& entry : opaques)
	{
		const auto* descriptor = entry.second.TryGetDescriptor<EngineDrawableDescriptor>();
		const auto opaqueSortToken = descriptor != nullptr &&
			descriptor->opaqueSortToken != EngineDrawableDescriptor::kInvalidOpaqueSortToken
				? descriptor->opaqueSortToken
				: BuildOpaqueDrawSortToken(entry.second);
		const auto stablePrimitiveIdentity = descriptor != nullptr
			? descriptor->stableSortKey
			: EngineDrawableDescriptor::kInvalidStableSortKey;
		keyedDrawables.push_back({
			entry.first,
			std::move(entry.second),
			opaqueSortToken,
			stablePrimitiveIdentity
		});
	}
	opaques.clear();

	std::stable_sort(
		keyedDrawables.begin(),
		keyedDrawables.end(),
		[](const auto& lhs, const auto& rhs)
		{
			if (lhs.opaqueSortToken != rhs.opaqueSortToken)
				return lhs.opaqueSortToken < rhs.opaqueSortToken;

			const bool lhsHasStableSortKey =
				lhs.stablePrimitiveIdentity != EngineDrawableDescriptor::kInvalidStableSortKey;
			const bool rhsHasStableSortKey =
				rhs.stablePrimitiveIdentity != EngineDrawableDescriptor::kInvalidStableSortKey;
			if (lhsHasStableSortKey != rhsHasStableSortKey)
				return lhsHasStableSortKey;
			if (lhsHasStableSortKey &&
				lhs.stablePrimitiveIdentity != rhs.stablePrimitiveIdentity)
			{
				return lhs.stablePrimitiveIdentity < rhs.stablePrimitiveIdentity;
			}
			return lhs.distance < rhs.distance;
		});

	opaques.reserve(keyedDrawables.size());

	size_t index = 0u;
	while (index < keyedDrawables.size())
	{
		auto runEnd = index + 1u;
		while (runEnd < keyedDrawables.size() &&
			CanMergeOpaqueDrawables(
				keyedDrawables[index].drawable,
				keyedDrawables[runEnd].drawable))
		{
			++runEnd;
		}

		if (runEnd - index > 1u)
		{
			auto* descriptor =
				keyedDrawables[index].drawable.TryGetDescriptor<EngineDrawableDescriptor>();
			if (descriptor != nullptr)
			{
				descriptor->instanceModelMatrices.clear();
				descriptor->instanceModelMatrices.reserve(runEnd - index);
				descriptor->instanceObjectIndices.clear();
				descriptor->instanceObjectIndices.reserve(runEnd - index);
				float nearestDistance = keyedDrawables[index].distance;
				constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
				auto groupIdentity = HashOpaqueSortTokenComponent(
					kFnvOffsetBasis,
					static_cast<uint64_t>(runEnd - index));
				auto groupCommandBuildSerial = groupIdentity;
				auto groupTransformRevision = groupIdentity;
				bool hasTrustedGroupRevision = descriptor->hasTrustedStaticDrawRevision;

				for (size_t runIndex = index; runIndex < runEnd; ++runIndex)
				{
					const auto* sourceDescriptor =
						keyedDrawables[runIndex].drawable.TryGetDescriptor<EngineDrawableDescriptor>();
					if (sourceDescriptor == nullptr)
					{
						hasTrustedGroupRevision = false;
						continue;
					}
					descriptor->instanceModelMatrices.push_back(sourceDescriptor->modelMatrix);
					descriptor->instanceObjectIndices.push_back(sourceDescriptor->stableObjectIndex);
					nearestDistance = std::min(nearestDistance, keyedDrawables[runIndex].distance);
					hasTrustedGroupRevision = hasTrustedGroupRevision &&
						sourceDescriptor->hasTrustedStaticDrawRevision &&
						sourceDescriptor->stableSceneIdentity.IsValid() &&
						TrustedStaticDrawResourcesMatch(*descriptor, *sourceDescriptor);
					groupIdentity = HashOpaqueSortTokenComponent(
						groupIdentity,
						sourceDescriptor->stableSceneIdentity.sceneId);
					groupIdentity = HashOpaqueSortTokenComponent(
						groupIdentity,
						sourceDescriptor->stableSceneIdentity.primitiveIndex);
					groupIdentity = HashOpaqueSortTokenComponent(
						groupIdentity,
						sourceDescriptor->stableSceneIdentity.primitiveGeneration);
					groupCommandBuildSerial = HashOpaqueSortTokenComponent(
						groupCommandBuildSerial,
						sourceDescriptor->cachedCommandBuildSerial);
					groupTransformRevision = HashOpaqueSortTokenComponent(
						groupTransformRevision,
						sourceDescriptor->transformRevision);
				}

				if (hasTrustedGroupRevision)
				{
					if (groupIdentity == EngineDrawableDescriptor::kInvalidStaticDrawGroupIdentity)
						--groupIdentity;
					if (groupCommandBuildSerial == 0u)
						groupCommandBuildSerial = 1u;
					if (groupTransformRevision == 0u)
						groupTransformRevision = 1u;
					descriptor->groupIdentity = groupIdentity;
					descriptor->cachedCommandBuildSerial = groupCommandBuildSerial;
					descriptor->transformRevision = groupTransformRevision;
					descriptor->allowsSingleObjectDataReuse = false;
				}
				else
				{
					descriptor->hasTrustedStaticDrawRevision = false;
					descriptor->allowsSingleObjectDataReuse = false;
					descriptor->groupIdentity =
						EngineDrawableDescriptor::kInvalidStaticDrawGroupIdentity;
				}

				descriptor->objectCount = static_cast<uint32_t>(descriptor->instanceModelMatrices.size());
				keyedDrawables[index].drawable.instanceCount =
					static_cast<uint32_t>(std::max<size_t>(1u, runEnd - index));
				opaques.emplace_back(nearestDistance, std::move(keyedDrawables[index].drawable));
				index = runEnd;
				continue;
			}
		}

		opaques.emplace_back(
			keyedDrawables[index].distance,
			std::move(keyedDrawables[index].drawable));
		++index;
	}
}

#if defined(NLS_ENABLE_TEST_HOOKS)
void RenderScene::FinalizeOpaqueQueueForTesting(RenderSceneVisibleQueues::SceneDrawables& opaques) const
{
	FinalizeOpaqueQueue(opaques);
}
#endif

void RenderScene::AssignVisibleObjectIndices(RenderSceneVisibleQueues& output) const
{
	const std::array<RenderSceneVisibleQueues::SceneDrawables*, 4u> queues = {
		&output.opaques,
		&output.decals,
		&output.skyboxes,
		&output.transparents
	};
	const auto maxObjectDataCount = NLS::Render::Data::GetMaxObjectDataCount();
	const auto maxObjectsPerSubmittedDraw = ResolveMaxObjectsPerSubmittedDraw();
	const auto isTrustedSingleObject = [](const NLS::Render::Entities::Drawable& drawable,
		const EngineDrawableDescriptor& descriptor)
	{
		return ResolveVisibleInstanceCount(drawable) == 1u &&
			descriptor.hasTrustedStaticDrawRevision &&
			descriptor.allowsSingleObjectDataReuse &&
			descriptor.stableSceneIdentity.IsValid() &&
			descriptor.transformRevision != 0u &&
			descriptor.groupIdentity == EngineDrawableDescriptor::kInvalidStaticDrawGroupIdentity;
	};

	std::vector<uint32_t> reservedStableObjectIndices;
	for (const auto* queue : queues)
	{
		for (const auto& entry : *queue)
		{
			const auto* descriptor = entry.second.TryGetDescriptor<EngineDrawableDescriptor>();
			if (descriptor == nullptr ||
				!DrawableRequiresIndexedObjectDataRange(entry.second) ||
				!isTrustedSingleObject(entry.second, *descriptor))
			{
				continue;
			}

			const auto stableObjectIndex = descriptor->stableObjectIndex !=
				EngineDrawableDescriptor::kInvalidStableObjectIndex
				? descriptor->stableObjectIndex
				: descriptor->stableSceneIdentity.primitiveIndex;
			if (stableObjectIndex < maxObjectDataCount)
				reservedStableObjectIndices.push_back(stableObjectIndex);
		}
	}
	std::sort(reservedStableObjectIndices.begin(), reservedStableObjectIndices.end());
	reservedStableObjectIndices.erase(
		std::unique(reservedStableObjectIndices.begin(), reservedStableObjectIndices.end()),
		reservedStableObjectIndices.end());

	const auto tryResolveStableObjectIndex =
		[&reservedStableObjectIndices, &isTrustedSingleObject](
			const NLS::Render::Entities::Drawable& drawable,
			const EngineDrawableDescriptor& descriptor,
			std::unordered_set<uint32_t>& claimedStableObjectIndices,
			uint32_t& outObjectIndex)
		{
			if (!isTrustedSingleObject(drawable, descriptor))
				return false;

			const auto stableObjectIndex = descriptor.stableObjectIndex !=
				EngineDrawableDescriptor::kInvalidStableObjectIndex
				? descriptor.stableObjectIndex
				: descriptor.stableSceneIdentity.primitiveIndex;
			if (!std::binary_search(
				reservedStableObjectIndices.begin(),
				reservedStableObjectIndices.end(),
				stableObjectIndex))
			{
				return false;
			}
			if (!claimedStableObjectIndices.insert(stableObjectIndex).second)
				return false;

			outObjectIndex = stableObjectIndex;
			return true;
		};

	const auto tryAllocateDynamicRange =
		[&reservedStableObjectIndices, maxObjectDataCount](
			uint32_t& nextObjectIndex,
			const uint32_t objectCount,
			uint32_t& outObjectIndex)
		{
			if (objectCount == 0u)
				return false;

			while (true)
			{
				uint32_t lastObjectIndex = 0u;
				if (!NLS::Render::Data::TryResolveObjectDataRangeEnd(
					nextObjectIndex,
					objectCount,
					lastObjectIndex))
				{
					return false;
				}

				const auto conflictingStableIndex = std::lower_bound(
					reservedStableObjectIndices.begin(),
					reservedStableObjectIndices.end(),
					nextObjectIndex);
				if (conflictingStableIndex == reservedStableObjectIndices.end() ||
					*conflictingStableIndex > lastObjectIndex)
				{
					outObjectIndex = nextObjectIndex;
					nextObjectIndex = lastObjectIndex + 1u;
					return true;
				}

				if (*conflictingStableIndex == (std::numeric_limits<uint32_t>::max)())
					return false;
				nextObjectIndex = *conflictingStableIndex + 1u;
				if (nextObjectIndex >= maxObjectDataCount)
					return false;
			}
		};

	uint32_t nextObjectIndex = 0u;
	// The planning pass decides every stable/dynamic object-data range and the
	// replay applies it. Running the same lookup and allocation search twice --
	// once to prove that in-place assignment cannot fail after partially mutating
	// the queues, once to perform it -- was pure duplicated work.
	struct PlannedObjectIndexAssignment
	{
		RenderSceneVisibleQueues::SceneDrawables::value_type* entry = nullptr;
		uint32_t objectIndex = 0u;
		uint32_t objectCount = 0u;
		uint32_t instanceCount = 0u;
		bool usesStableObjectIndex = false;
		bool preservesMaterialInstanceCount = false;
	};
	std::vector<PlannedObjectIndexAssignment> plannedObjectIndexAssignments;
	const auto tryAssignInPlace = [&]()
	{
		plannedObjectIndexAssignments.clear();
		uint32_t simulatedNextObjectIndex = 0u;
		std::unordered_set<uint32_t> simulatedStableObjectIndices;
		simulatedStableObjectIndices.reserve(reservedStableObjectIndices.size());
		for (auto* queue : queues)
		{
			for (auto& entry : *queue)
			{
				const auto* descriptor = entry.second.TryGetDescriptor<EngineDrawableDescriptor>();
				if (descriptor == nullptr ||
					!DrawableRequiresIndexedObjectDataRange(entry.second))
				{
					continue;
				}

				const auto instanceCount = ResolveVisibleInstanceCount(entry.second);
				const auto objectCount = std::max(1u, instanceCount);
				if (objectCount > maxObjectsPerSubmittedDraw)
					return false;

				uint32_t objectIndex = 0u;
				const bool usesStableObjectIndex = tryResolveStableObjectIndex(
					entry.second,
					*descriptor,
					simulatedStableObjectIndices,
					objectIndex);
				if (!usesStableObjectIndex &&
					!tryAllocateDynamicRange(simulatedNextObjectIndex, objectCount, objectIndex))
				{
					return false;
				}

				// The queues are not resized while planning, so this element pointer stays
				// valid for the replay below.
				plannedObjectIndexAssignments.push_back({
					&entry,
					objectIndex,
					objectCount,
					instanceCount,
					usesStableObjectIndex,
					entry.second.instanceCount == 0u && objectCount == instanceCount
				});
			}
		}

		for (auto& plan : plannedObjectIndexAssignments)
		{
			auto& entry = *plan.entry;
			auto* descriptor = entry.second.TryGetDescriptor<EngineDrawableDescriptor>();
			if (descriptor == nullptr)
				continue;

			descriptor->objectIndex = plan.objectIndex;
			descriptor->objectCount = plan.objectCount;
			descriptor->allowsSingleObjectDataReuse = plan.usesStableObjectIndex;
			if (descriptor->instanceModelMatrices.empty())
				ExpandDescriptorForObjectDataRange(*descriptor, plan.objectCount);
			if (!plan.preservesMaterialInstanceCount || plan.objectCount != plan.instanceCount)
				entry.second.instanceCount = plan.objectCount;
		}

		return true;
	};
	if (tryAssignInPlace())
		return;

	nextObjectIndex = 0u;
	std::unordered_set<uint32_t> claimedStableObjectIndices;
	claimedStableObjectIndices.reserve(reservedStableObjectIndices.size());
	const auto assignQueue =
		[this,
		 &maxObjectsPerSubmittedDraw,
		 &nextObjectIndex,
		 &claimedStableObjectIndices,
		 &tryResolveStableObjectIndex,
		 &tryAllocateDynamicRange](RenderSceneVisibleQueues::SceneDrawables& queue)
		{
			RenderSceneVisibleQueues::SceneDrawables assigned;
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

						EngineDrawableDescriptor chunkDescriptor = descriptor;
						chunkDescriptor.modelMatrix = descriptor.modelMatrix;
						chunkDescriptor.userMatrix = descriptor.userMatrix;
						chunkDescriptor.objectIndex = objectIndex;
						chunkDescriptor.objectCount = chunkObjectCount;
						chunkDescriptor.objectFlags = descriptor.objectFlags;
						chunkDescriptor.stableSortKey = descriptor.stableSortKey;
						chunkDescriptor.opaqueSortToken = descriptor.opaqueSortToken;
						chunkDescriptor.allowsSingleObjectDataReuse = false;
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

				uint32_t objectIndex = 0u;
				const bool usesStableObjectIndex = tryResolveStableObjectIndex(
					entry.second,
					descriptor,
					claimedStableObjectIndices,
					objectIndex);
				if (usesStableObjectIndex ||
					(objectCount <= maxObjectsPerSubmittedDraw &&
						tryAllocateDynamicRange(nextObjectIndex, objectCount, objectIndex)))
				{
					EngineDrawableDescriptor updatedDescriptor = descriptor;
					updatedDescriptor.objectIndex = objectIndex;
					updatedDescriptor.objectCount = objectCount;
					updatedDescriptor.allowsSingleObjectDataReuse = usesStableObjectIndex;
					if (updatedDescriptor.instanceModelMatrices.empty())
						ExpandDescriptorForObjectDataRange(updatedDescriptor, objectCount);
					entry.second.RemoveDescriptor<EngineDrawableDescriptor>();
					entry.second.AddDescriptor<EngineDrawableDescriptor>(std::move(updatedDescriptor));
					if (!preservesMaterialInstanceCount || objectCount != instanceCount)
						entry.second.instanceCount = objectCount;
					assigned.push_back(std::move(entry));
					continue;
				}

				while (remainingObjectCount > 0u)
				{
					if (maxObjectsPerSubmittedDraw == 0u)
					{
						m_lastDrawCallOptimizationStats.objectDataOverflowDroppedObjectCount += remainingObjectCount;
						break;
					}

					uint32_t chunkObjectCount = std::min<uint32_t>(
						remainingObjectCount,
						maxObjectsPerSubmittedDraw);
					uint32_t chunkObjectIndex = 0u;
					while (chunkObjectCount > 0u &&
						!tryAllocateDynamicRange(nextObjectIndex, chunkObjectCount, chunkObjectIndex))
					{
						--chunkObjectCount;
					}
					if (chunkObjectCount == 0u)
					{
						m_lastDrawCallOptimizationStats.objectDataOverflowDroppedObjectCount += remainingObjectCount;
						break;
					}

					appendChunk(chunkObjectCount, chunkObjectIndex, matrixOffset);

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
