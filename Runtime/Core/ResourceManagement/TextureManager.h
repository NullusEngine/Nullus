#pragma once

#include "CoreDef.h"

#include <Rendering/Resources/Texture2D.h>
#include <Rendering/Resources/TextureCube.h>

#include "Core/ResourceManagement/AResourceManager.h"
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NLS::Core::ResourceManagement
{
	/**
	* ResourceManager of textures
	*/
	class NLS_RESOURCE_MANAGEMENT_API TextureManager : public AResourceManager<Render::Resources::Texture2D>
	{
	public:
        using Texture2D = Render::Resources::Texture2D;
        using TextureCube = Render::Resources::TextureCube;

			~TextureManager();

			const char* GetResourceTypeName() const override { return "Texture"; }

		/**
		* Create the resource identified by the given path
		* @param p_path
		*/
		virtual Texture2D* CreateResource(const std::string & p_path) override;

		/**
		* Destroy the given resource
		* @param p_resource
		*/
		virtual void DestroyResource(Texture2D* p_resource) override;

		/**
		* Reload the given resource
		* @param p_resource
		* @param p_path
		*/
		virtual void ReloadResource(Texture2D* p_resource, const std::string& p_path) override;

		Texture2D* RegisterResource(const std::string& p_path, Texture2D* p_instance);
		void UnloadResource(const std::string& p_path);
		bool MoveResource(const std::string& p_previousPath, const std::string& p_newPath);
		void UnloadResources();
		void UnregisterResource(const std::string& p_path);

		Texture2D* GetArtifactResource(const std::string& p_path, bool p_tryToLoadIfNotFound = true);
		Texture2D* RequestAsyncArtifact(const std::string& p_path, bool p_cancelableInterest = false);
		ResourceHandle<Texture2D> AcquireTextureHandle(
			ResourceLifetimeRegistry& registry,
			const std::string& ownerToken,
			const std::string& path,
			ResourceLifetimeOwnerKind ownerKind = ResourceLifetimeOwnerKind::SceneInstance,
			size_t estimatedBytes = 0u)
		{
			return AcquireResourceHandle(
				registry,
				ResourceLifetimeAcquireRequest {
					ownerToken,
					ResourceLifetimeResourceType::Texture,
					path,
					estimatedBytes,
					ownerKind });
		}

		size_t TrimUnusedTextureResources(
			ResourceLifetimeRegistry& registry,
			const ResourceLifetimeTrimOptions& options = {})
		{
			InvalidateArtifactLookupIndex();
			const auto trimmedCount = TrimUnusedResources(
				registry,
				ResourceLifetimeResourceType::Texture,
				options);
			if (trimmedCount > 0u)
				InvalidateArtifactLookupIndex();
			return trimmedCount;
		}

		void CancelAsyncArtifact(const std::string& p_path, bool p_cancelableInterest = true);
		bool IsAsyncArtifactLoadPending(const std::string& p_path) const;
		bool IsAsyncArtifactLoadFailed(const std::string& p_path) const;
			void PumpAsyncLoads(size_t p_maxCompletions = 1u);
			void PumpAsyncLoadsForPaths(
				const std::unordered_set<std::string>& p_paths,
				size_t p_maxCompletions = 1u,
				const std::function<bool()>& p_shouldStop = {});
		static AsyncArtifactRequestDiagnostics GetAsyncArtifactRequestDiagnostics();

        static std::string ResolveResourcePath(const std::string& p_path);

		static TextureCube* CreateCubeMap(const std::vector<std::string>& filePaths);

#if defined(NLS_ENABLE_TEST_HOOKS)
				static void ClearAsyncArtifactRequestStateForTesting();
				static bool WaitForAsyncArtifactWorkersForTesting(uint32_t p_timeoutMilliseconds = 5000u);
				static size_t GetMaxPendingAsyncArtifactRequestCountForTesting();
				static size_t GetPendingAsyncArtifactRequestCountForTesting();
					static size_t GetTotalAsyncArtifactRequestCountForTesting();
					static size_t GetFailedAsyncArtifactRequestCountForTesting();
					static void SetBeforeAsyncArtifactCompletionForTesting(std::function<void()> p_callback);
				void ClearArtifactLookupIndexForTesting() const;
			size_t GetArtifactLookupIndexRebuildCountForTesting() const;
#endif

	private:
		Texture2D* FindCachedArtifactResourceByResolvedPath(const std::string& p_realPath) const;
		void InvalidateArtifactLookupIndex() const;
		void EnsureArtifactLookupIndex() const;
		void IndexTextureArtifactPath(const std::string& p_path, Texture2D* p_texture) const;

		mutable std::mutex m_artifactLookupIndexMutex;
		mutable bool m_artifactLookupIndexDirty = true;
		mutable size_t m_artifactLookupIndexedResourceCount = 0u;
		mutable uint64_t m_artifactLookupGeneration = 0u;
		mutable uint64_t m_artifactLookupIndexedGeneration = 0u;
		mutable std::unordered_map<std::string, Texture2D*> m_texturesByNormalizedArtifactPath;
#if defined(NLS_ENABLE_TEST_HOOKS)
		mutable size_t m_artifactLookupIndexRebuildCountForTesting = 0u;
#endif
	};
}
