#include "Core/ResourceManagement/MeshManager.h"

#include <Debug/Logger.h>
#include <Jobs/BackgroundJobQueue.h>
#include <Jobs/JobSystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Guid.h>
#include "Assets/ArtifactManifest.h"
#include <Filesystem/IniFile.h>
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Resources/Parsers/AssimpParser.h"
#include "Rendering/Resources/Parsers/FbxSdkParser.h"

#ifndef NLS_HAS_ASSIMP_FBX_IMPORTER
#define NLS_HAS_ASSIMP_FBX_IMPORTER 0
#endif

#ifndef NLS_HAS_AUTODESK_FBX_SDK
#define NLS_HAS_AUTODESK_FBX_SDK 0
#endif

namespace
{
using EModelParserFlags = NLS::Render::Resources::Parsers::EModelParserFlags;

std::string ToLower(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

bool IsBuiltinPath(const std::string& path)
{
    return !path.empty() && path[0] == ':';
}

bool IsPrimitiveAliasPath(const std::string& path)
{
    return path.rfind("builtin:Primitive/", 0) == 0;
}

std::optional<std::string> PrimitiveAliasToBuiltinModelPath(const std::string& path)
{
    if (!IsPrimitiveAliasPath(path))
        return std::nullopt;

    auto primitiveName = path.substr(std::string("builtin:Primitive/").size());
    if (primitiveName.empty())
        return std::nullopt;

    primitiveName[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(primitiveName[0])));
    return std::string(":Models/") + primitiveName + ".fbx";
}

bool IsSourceModelExtension(const std::filesystem::path& path)
{
    const auto extension = ToLower(path.extension().string());
    return extension == ".fbx" || extension == ".obj" || extension == ".gltf" || extension == ".glb";
}

bool IsMeshArtifactPath(const std::filesystem::path& path)
{
    return NLS::Render::Assets::IsMeshArtifactFile(path);
}

std::string NormalizeResolvedArtifactPath(std::string path)
{
    if (path.empty())
        return {};

    std::replace(path.begin(), path.end(), '\\', '/');
    return std::filesystem::path(path).lexically_normal().generic_string();
}

bool IsAuthorizedContentArtifactPath(const std::string& path)
{
    const auto portableArtifactPath =
        NLS::Core::Assets::TryMakePortableContentArtifactPath(path);
    return portableArtifactPath.empty() ||
        NLS::Core::Assets::IsRuntimeArtifactPathAuthorized(portableArtifactPath);
}

std::filesystem::path NormalizeVirtualBuiltinPath(const std::string& path)
{
    std::string relativePath = path;
    if (IsBuiltinPath(relativePath))
        relativePath.erase(relativePath.begin());
    std::replace(relativePath.begin(), relativePath.end(), '\\', '/');
    return std::filesystem::path(relativePath);
}

std::filesystem::path NormalizeConfiguredRoot(std::string path)
{
    while (!path.empty() && (path.back() == '/' || path.back() == '\\'))
        path.pop_back();
    return std::filesystem::path(path).lexically_normal();
}

EModelParserFlags GetModelMetadata(const std::string& path)
{
    auto metaFile = NLS::Filesystem::IniFile(path + ".meta");
    EModelParserFlags flags = EModelParserFlags::NONE;

    if (metaFile.GetOrDefault("CALC_TANGENT_SPACE", true)) flags |= EModelParserFlags::CALC_TANGENT_SPACE;
    if (metaFile.GetOrDefault("JOIN_IDENTICAL_VERTICES", true)) flags |= EModelParserFlags::JOIN_IDENTICAL_VERTICES;
    if (metaFile.GetOrDefault("MAKE_LEFT_HANDED", false)) flags |= EModelParserFlags::MAKE_LEFT_HANDED;
    if (metaFile.GetOrDefault("TRIANGULATE", true)) flags |= EModelParserFlags::TRIANGULATE;
    if (metaFile.GetOrDefault("REMOVE_COMPONENT", false)) flags |= EModelParserFlags::REMOVE_COMPONENT;
    if (metaFile.GetOrDefault("GEN_NORMALS", false)) flags |= EModelParserFlags::GEN_NORMALS;
    if (metaFile.GetOrDefault("GEN_SMOOTH_NORMALS", true)) flags |= EModelParserFlags::GEN_SMOOTH_NORMALS;
    if (metaFile.GetOrDefault("SPLIT_LARGE_MESHES", false)) flags |= EModelParserFlags::SPLIT_LARGE_MESHES;
    if (metaFile.GetOrDefault("PRE_TRANSFORM_VERTICES", false)) flags |= EModelParserFlags::PRE_TRANSFORM_VERTICES;
    if (metaFile.GetOrDefault("LIMIT_BONE_WEIGHTS", false)) flags |= EModelParserFlags::LIMIT_BONE_WEIGHTS;
    if (metaFile.GetOrDefault("VALIDATE_DATA_STRUCTURE", false)) flags |= EModelParserFlags::VALIDATE_DATA_STRUCTURE;
    if (metaFile.GetOrDefault("IMPROVE_CACHE_LOCALITY", true)) flags |= EModelParserFlags::IMPROVE_CACHE_LOCALITY;
    if (metaFile.GetOrDefault("REMOVE_REDUNDANT_MATERIALS", false)) flags |= EModelParserFlags::REMOVE_REDUNDANT_MATERIALS;
    if (metaFile.GetOrDefault("FIX_INFACING_NORMALS", false)) flags |= EModelParserFlags::FIX_INFACING_NORMALS;
    if (metaFile.GetOrDefault("SORT_BY_PTYPE", false)) flags |= EModelParserFlags::SORT_BY_PTYPE;
    if (metaFile.GetOrDefault("FIND_DEGENERATES", false)) flags |= EModelParserFlags::FIND_DEGENERATES;
    if (metaFile.GetOrDefault("FIND_INVALID_DATA", true)) flags |= EModelParserFlags::FIND_INVALID_DATA;
    if (metaFile.GetOrDefault("GEN_UV_COORDS", true)) flags |= EModelParserFlags::GEN_UV_COORDS;
    if (metaFile.GetOrDefault("TRANSFORM_UV_COORDS", false)) flags |= EModelParserFlags::TRANSFORM_UV_COORDS;
    if (metaFile.GetOrDefault("FIND_INSTANCES", true)) flags |= EModelParserFlags::FIND_INSTANCES;
    if (metaFile.GetOrDefault("OPTIMIZE_MESHES", true)) flags |= EModelParserFlags::OPTIMIZE_MESHES;
    if (metaFile.GetOrDefault("OPTIMIZE_GRAPH", false)) flags |= EModelParserFlags::OPTIMIZE_GRAPH;
    if (metaFile.GetOrDefault("FLIP_UVS", false)) flags |= EModelParserFlags::FLIP_UVS;
    if (metaFile.GetOrDefault("FLIP_WINDING_ORDER", false)) flags |= EModelParserFlags::FLIP_WINDING_ORDER;
    if (metaFile.GetOrDefault("SPLIT_BY_BONE_COUNT", false)) flags |= EModelParserFlags::SPLIT_BY_BONE_COUNT;
    if (metaFile.GetOrDefault("DEBONE", true)) flags |= EModelParserFlags::DEBONE;
    if (metaFile.GetOrDefault("GLOBAL_SCALE", true)) flags |= EModelParserFlags::GLOBAL_SCALE;
    if (metaFile.GetOrDefault("EMBED_TEXTURES", false)) flags |= EModelParserFlags::EMBED_TEXTURES;
    if (metaFile.GetOrDefault("FORCE_GEN_NORMALS", false)) flags |= EModelParserFlags::FORCE_GEN_NORMALS;
    if (metaFile.GetOrDefault("DROP_NORMALS", false)) flags |= EModelParserFlags::DROP_NORMALS;
    if (metaFile.GetOrDefault("GEN_BOUNDING_BOXES", false)) flags |= EModelParserFlags::GEN_BOUNDING_BOXES;

    return flags;
}

bool WriteMeshArtifact(
    const std::filesystem::path& artifactPath,
    const NLS::Render::Resources::Parsers::ParsedMeshData& mesh)
{
    std::error_code error;
    std::filesystem::create_directories(artifactPath.parent_path(), error);
    if (error)
        return false;

    NLS::Render::Assets::MeshArtifactData artifact;
    artifact.vertices = mesh.vertices;
    artifact.indices = mesh.indices;
    artifact.materialIndex = mesh.materialIndex;
    const auto bytes = NLS::Render::Assets::SerializeMeshArtifact(artifact);
    if (bytes.empty())
        return false;

    std::ofstream output(artifactPath, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;

    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

bool GenerateBuiltinMeshArtifact(
    const std::string& sourcePath,
    const std::filesystem::path& artifactPath,
    const EModelParserFlags parserFlags)
{
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;
    const auto extension = ToLower(std::filesystem::path(sourcePath).extension().string());
    bool loaded = false;
    if (extension == ".fbx")
    {
#if NLS_HAS_ASSIMP_FBX_IMPORTER
        {
            NLS::Render::Resources::Parsers::AssimpParser parser;
            loaded = parser.LoadModelData(sourcePath, meshes, materials, parserFlags, nullptr, true);
        }
#endif
#if NLS_HAS_AUTODESK_FBX_SDK
        if (!loaded)
        {
            NLS::Render::Resources::Parsers::FbxSdkParser parser;
            loaded = parser.LoadModelData(sourcePath, meshes, materials, parserFlags, nullptr, true);
        }
#endif
    }
    else
    {
        NLS::Render::Resources::Parsers::AssimpParser parser;
        loaded = parser.LoadModelData(sourcePath, meshes, materials, parserFlags, nullptr, true);
    }
    if (!loaded || meshes.size() != 1u)
        return false;

    return WriteMeshArtifact(artifactPath, meshes.front());
}

std::string ResolveBuiltinMeshArtifactPath(const std::string& path)
{
    std::string sourcePathAlias = path;
    if (const auto primitiveSource = PrimitiveAliasToBuiltinModelPath(path))
        sourcePathAlias = *primitiveSource;

    if (!IsBuiltinPath(sourcePathAlias))
        return {};

    auto relativePath = NormalizeVirtualBuiltinPath(sourcePathAlias);
    if (!IsSourceModelExtension(relativePath))
        return {};

    const auto hashedFileName = NLS::Core::Assets::BuildArtifactStorageFileName(
        "BuiltinMeshArtifact:" + relativePath.generic_string());
    relativePath = relativePath.parent_path() / hashedFileName;
    std::error_code error;
    const auto projectAssetsPath = NLS::Core::ResourceManagement::MeshManager::ProjectAssetsRoot();
    std::filesystem::path projectCachePath;
    if (!projectAssetsPath.empty())
    {
        const auto projectAssetsRoot = NormalizeConfiguredRoot(projectAssetsPath);
        projectCachePath =
            (projectAssetsRoot.parent_path() /
             "Library" / "BuiltinArtifacts" / relativePath).lexically_normal();
        if (std::filesystem::is_regular_file(projectCachePath, error))
            return projectCachePath.string();
    }

    const auto bundledArtifactPath = std::filesystem::path("Library") / "BuiltinArtifacts" / relativePath;
    const auto realPath = NLS::Core::ResourceManagement::MeshManager::ResolveResourcePath(
        ":" + bundledArtifactPath.generic_string());
    if (std::filesystem::is_regular_file(realPath, error))
        return realPath;

    if (!projectCachePath.empty())
    {
        const auto sourcePath = NLS::Core::ResourceManagement::MeshManager::ResolveResourcePath(sourcePathAlias);
        if (std::filesystem::is_regular_file(sourcePath, error) &&
            GenerateBuiltinMeshArtifact(sourcePath, projectCachePath, GetModelMetadata(sourcePath)))
        {
            return projectCachePath.string();
        }
    }

    return {};
}
}

namespace NLS::Core::ResourceManagement
{
namespace
{
struct AsyncMeshArtifactRequest
{
    const MeshManager* owner = nullptr;
    std::string path;
    std::string realPath;
    std::optional<std::filesystem::file_time_type> writeTime;
    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
    size_t cancelableInterestCount = 0u;
    size_t sharedInterestCount = 0u;
    bool retryCancelledCompletion = false;
    NLS::Base::Jobs::JobHandle jobHandle;
    std::future<std::optional<NLS::Render::Assets::MeshArtifactData>> future;
    uint64_t runtimeUploadRequestId = 0u;
};

struct FailedMeshArtifactLoad
{
    const MeshManager* owner = nullptr;
    std::string realPath;
    std::optional<std::filesystem::file_time_type> writeTime;
};

struct AsyncMeshArtifactStateKey
{
    const MeshManager* owner = nullptr;
    std::string path;
    std::string realPath;

    [[nodiscard]] bool operator==(const AsyncMeshArtifactStateKey& other) const
    {
        return owner == other.owner && path == other.path;
    }
};

struct AsyncMeshArtifactStateKeyHash
{
    [[nodiscard]] size_t operator()(const AsyncMeshArtifactStateKey& key) const
    {
        return std::hash<const MeshManager*>{}(key.owner) ^ (std::hash<std::string>{}(key.path) << 1u);
    }
};

std::mutex g_asyncMeshMutex;
std::unordered_map<AsyncMeshArtifactStateKey, AsyncMeshArtifactRequest, AsyncMeshArtifactStateKeyHash> g_asyncMeshRequests;
std::unordered_map<AsyncMeshArtifactStateKey, FailedMeshArtifactLoad, AsyncMeshArtifactStateKeyHash> g_failedAsyncMeshArtifacts;
std::unordered_set<AsyncMeshArtifactStateKey, AsyncMeshArtifactStateKeyHash> g_cancelledAsyncMeshArtifacts;
std::atomic_size_t g_activeMeshArtifactWorkers {0u};
#if defined(NLS_ENABLE_TEST_HOOKS)
std::atomic_size_t g_artifactResourcePathResolutionCount {0u};
#endif
constexpr size_t kMaxPendingAsyncMeshArtifactRequests = 16u;
constexpr size_t kMaxQueuedAsyncMeshArtifactRequests = 256u;

struct TrackedMeshArtifactPaths
{
    std::unordered_set<std::string> sourcePaths;
    std::unordered_set<std::string> normalizedRealPaths;
};

std::optional<std::filesystem::file_time_type> TryGetLastWriteTime(const std::string& path)
{
    std::error_code error;
    auto writeTime = std::filesystem::last_write_time(path, error);
    if (error)
        return std::nullopt;
    return writeTime;
}

bool ArtifactPathMatchesResolvedPath(
    const std::string& candidateRealPath,
    const std::string& targetRealPath)
{
    return NormalizeResolvedArtifactPath(candidateRealPath) == NormalizeResolvedArtifactPath(targetRealPath);
}

MeshManager::Mesh* FindCachedMeshByEquivalentArtifactPath(
    MeshManager& manager,
    const std::string& realPath)
{
    const auto target = NormalizeResolvedArtifactPath(realPath);
    if (target.empty())
        return nullptr;

    for (const auto& [resourcePath, mesh] : manager.GetResources())
    {
        if (mesh == nullptr)
            continue;

        if (NormalizeResolvedArtifactPath(MeshManager::ResolveArtifactResourcePath(resourcePath)) == target)
            return mesh;
    }

    return nullptr;
}

auto FindAsyncMeshRequestByEquivalentArtifactPath(
    std::unordered_map<AsyncMeshArtifactStateKey, AsyncMeshArtifactRequest, AsyncMeshArtifactStateKeyHash>& requests,
    const MeshManager& manager,
    const std::string& path,
    const std::string& realPath)
{
    if (auto exact = requests.find({ &manager, path }); exact != requests.end())
        return exact;
    return std::find_if(
        requests.begin(),
        requests.end(),
        [&manager, &path, &realPath](const auto& entry)
        {
            return entry.second.owner == &manager &&
                (entry.first.path == path ||
                 ArtifactPathMatchesResolvedPath(entry.second.realPath, realPath));
        });
}

auto FindFailedMeshLoadByEquivalentArtifactPath(
    std::unordered_map<AsyncMeshArtifactStateKey, FailedMeshArtifactLoad, AsyncMeshArtifactStateKeyHash>& failures,
    const MeshManager& manager,
    const std::string& path,
    const std::string& realPath)
{
    if (auto exact = failures.find({ &manager, path }); exact != failures.end())
        return exact;
    return std::find_if(
        failures.begin(),
        failures.end(),
        [&manager, &path, &realPath](const auto& entry)
        {
            return entry.second.owner == &manager &&
                (entry.first.path == path ||
                 ArtifactPathMatchesResolvedPath(entry.second.realPath, realPath));
        });
}

void EraseCancelledMeshArtifactByEquivalentPath(
    std::unordered_set<AsyncMeshArtifactStateKey, AsyncMeshArtifactStateKeyHash>& cancelledArtifacts,
    const MeshManager& manager,
    const std::string& path,
    const std::string& realPath)
{
    for (auto it = cancelledArtifacts.begin(); it != cancelledArtifacts.end();)
    {
        if (it->owner == &manager &&
            (it->path == path ||
             ArtifactPathMatchesResolvedPath(it->realPath, realPath)))
        {
            it = cancelledArtifacts.erase(it);
        }
        else
            ++it;
    }
}

bool MeshRequestMatchesAnyTrackedPath(
    const AsyncMeshArtifactRequest& request,
    const TrackedMeshArtifactPaths& paths)
{
    if (paths.sourcePaths.find(request.path) != paths.sourcePaths.end())
        return true;
    const auto normalizedRequestPath = NormalizeResolvedArtifactPath(request.realPath);
    return !normalizedRequestPath.empty() &&
        paths.normalizedRealPaths.find(normalizedRequestPath) != paths.normalizedRealPaths.end();
}

TrackedMeshArtifactPaths BuildTrackedMeshArtifactPaths(const std::unordered_set<std::string>& paths)
{
    TrackedMeshArtifactPaths tracked;
    tracked.sourcePaths = paths;
    tracked.normalizedRealPaths.reserve(paths.size());
    for (const auto& path : paths)
    {
        const auto normalized = NormalizeResolvedArtifactPath(MeshManager::ResolveArtifactResourcePath(path));
        if (!normalized.empty())
            tracked.normalizedRealPaths.insert(normalized);
    }
    return tracked;
}

class MeshArtifactWorkerScope
{
public:
    MeshArtifactWorkerScope() { g_activeMeshArtifactWorkers.fetch_add(1u, std::memory_order_acq_rel); }
    ~MeshArtifactWorkerScope() { g_activeMeshArtifactWorkers.fetch_sub(1u, std::memory_order_acq_rel); }
};

struct MeshArtifactJobPayload
{
    std::string realPath;
    std::shared_ptr<std::atomic_bool> cancellationFlag;
    std::promise<std::optional<NLS::Render::Assets::MeshArtifactData>> promise;
};

struct MeshArtifactLoadSubmission
{
    NLS::Base::Jobs::JobHandle handle;
    std::future<std::optional<NLS::Render::Assets::MeshArtifactData>> future;
};

void RunMeshArtifactJob(void* userData)
{
    std::unique_ptr<MeshArtifactJobPayload> payload(static_cast<MeshArtifactJobPayload*>(userData));
    MeshArtifactWorkerScope workerScope;
    try
    {
        payload->promise.set_value(NLS::Render::Assets::LoadMeshArtifact(
            payload->realPath,
            payload->cancellationFlag.get()));
    }
    catch (...)
    {
        try
        {
            payload->promise.set_exception(std::current_exception());
        }
        catch (...)
        {
        }
    }
}

void CancelMeshArtifactJob(void* userData)
{
    std::unique_ptr<MeshArtifactJobPayload> payload(static_cast<MeshArtifactJobPayload*>(userData));
    if (payload->cancellationFlag)
        payload->cancellationFlag->store(true, std::memory_order_release);
    try
    {
        payload->promise.set_value(std::nullopt);
    }
    catch (...)
    {
    }
}

MeshArtifactLoadSubmission StartMeshArtifactLoad(
    const AsyncMeshArtifactRequest& request)
{
    auto payload = std::make_unique<MeshArtifactJobPayload>();
    payload->realPath = request.realPath;
    payload->cancellationFlag = request.cancelled;
    auto future = payload->promise.get_future();

    NLS::Base::Jobs::BackgroundJobDesc desc;
    desc.function = RunMeshArtifactJob;
    desc.userData = payload.get();
    desc.cancelFunction = CancelMeshArtifactJob;
    desc.cancelUserData = payload.get();
    desc.debugName = "MeshManager::AsyncArtifactLoad";

    const auto handle = NLS::Base::Jobs::ScheduleBackgroundJob(desc);
    if (handle.id == 0u)
        return {};

    (void)payload.release();
    return {handle, std::move(future)};
}

size_t CountActiveMeshRequests()
{
    return static_cast<size_t>(std::count_if(
        g_asyncMeshRequests.begin(),
        g_asyncMeshRequests.end(),
        [](const auto& entry)
        {
            if (!entry.second.future.valid())
                return false;
            if (entry.second.jobHandle.id != 0u &&
                NLS::Base::Jobs::IsCompleted(entry.second.jobHandle))
            {
                return false;
            }
            return entry.second.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
        }));
}

size_t CountQueuedMeshRequestsForOwner(const MeshManager& manager)
{
    return static_cast<size_t>(std::count_if(
        g_asyncMeshRequests.begin(),
        g_asyncMeshRequests.end(),
        [&manager](const auto& entry)
        {
            return entry.second.owner == &manager;
        }));
}

void PromoteQueuedMeshArtifactLoads(
    MeshManager& manager,
    const TrackedMeshArtifactPaths* paths)
{
    for (;;)
    {
        if (CountActiveMeshRequests() >= kMaxPendingAsyncMeshArtifactRequests)
            return;

        auto found = std::find_if(
            g_asyncMeshRequests.begin(),
            g_asyncMeshRequests.end(),
            [&manager, paths](auto& entry)
            {
                if (entry.second.owner != &manager ||
                    entry.second.future.valid() ||
                    entry.second.runtimeUploadRequestId != 0u)
                    return false;
                return paths == nullptr || MeshRequestMatchesAnyTrackedPath(entry.second, *paths);
            });
        if (found == g_asyncMeshRequests.end())
            return;

            try
            {
                auto load = StartMeshArtifactLoad(found->second);
                if (!load.future.valid())
                {
                    if (NLS::Base::Jobs::IsJobSystemInitialized())
                    {
                        g_failedAsyncMeshArtifacts[found->first] =
                            { found->second.owner, found->second.realPath, found->second.writeTime };
                        g_asyncMeshRequests.erase(found);
                        continue;
                    }
                    return;
                }
                found->second.jobHandle = load.handle;
                found->second.future = std::move(load.future);
            }
            catch (...)
        {
            g_failedAsyncMeshArtifacts[found->first] =
                { found->second.owner, found->second.realPath, found->second.writeTime };
            g_asyncMeshRequests.erase(found);
        }
    }
}

void PumpAsyncMeshArtifactLoads(
    MeshManager& manager,
    const size_t maxCompletions,
    const TrackedMeshArtifactPaths* paths)
{
    size_t completedCount = 0u;

    std::vector<std::pair<AsyncMeshArtifactStateKey, uint64_t>> runtimeUploads;
    {
        std::lock_guard lock(g_asyncMeshMutex);
        runtimeUploads.reserve(g_asyncMeshRequests.size());
        for (const auto& [key, request] : g_asyncMeshRequests)
        {
            if (request.owner != &manager || request.runtimeUploadRequestId == 0u)
                continue;
            if (paths != nullptr && !MeshRequestMatchesAnyTrackedPath(request, *paths))
                continue;
            runtimeUploads.emplace_back(key, request.runtimeUploadRequestId);
        }
    }

    if (auto* driver = NLS::Render::Context::TryGetLocatedDriver(); driver != nullptr)
    {
        for (const auto& [key, uploadRequestId] : runtimeUploads)
        {
            if (completedCount >= maxCompletions)
                break;

            auto upload = NLS::Render::Context::DriverResourceAccess::ConsumeMeshRuntimeUploadResult(
                *driver,
                uploadRequestId);
            if (!upload.ready)
                continue;

            AsyncMeshArtifactRequest request;
            {
                std::lock_guard lock(g_asyncMeshMutex);
                auto found = g_asyncMeshRequests.find(key);
                if (found == g_asyncMeshRequests.end() ||
                    found->second.runtimeUploadRequestId != uploadRequestId)
                {
                    continue;
                }
                request = std::move(found->second);
                g_asyncMeshRequests.erase(found);
            }

            if (upload.success && upload.mesh != nullptr)
            {
                if (FindCachedMeshByEquivalentArtifactPath(manager, request.realPath) == nullptr)
                    manager.RegisterResource(request.path, upload.mesh.release());
                std::lock_guard lock(g_asyncMeshMutex);
                g_failedAsyncMeshArtifacts.erase({ request.owner, request.path });
            }
            else
            {
                NLS_LOG_ERROR(
                    "Async mesh runtime upload failed: " + request.realPath +
                    " error=" + (upload.diagnostic.empty() ? "unknown" : upload.diagnostic));
                std::lock_guard lock(g_asyncMeshMutex);
                g_failedAsyncMeshArtifacts[{ request.owner, request.path }] =
                    { request.owner, request.realPath, request.writeTime };
            }
            ++completedCount;
        }
    }

    while (completedCount < maxCompletions)
    {
        AsyncMeshArtifactRequest request;
        {
            std::lock_guard lock(g_asyncMeshMutex);
            PromoteQueuedMeshArtifactLoads(manager, paths);
            auto found = std::find_if(
                g_asyncMeshRequests.begin(),
                g_asyncMeshRequests.end(),
                [&manager, paths](auto& entry)
                {
                    if (entry.second.owner != &manager)
                        return false;
                    if (paths != nullptr && !MeshRequestMatchesAnyTrackedPath(entry.second, *paths))
                        return false;
                    return entry.second.future.valid() &&
                        entry.second.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                });
            if (found == g_asyncMeshRequests.end())
                return;

            request = std::move(found->second);
            g_asyncMeshRequests.erase(found);
        }

        std::optional<NLS::Render::Assets::MeshArtifactData> artifact;
        try
        {
            artifact = request.future.get();
        }
        catch (const std::exception& exception)
        {
            NLS_LOG_ERROR(
                std::string("Async mesh artifact load failed: ") +
                request.realPath +
                " error=" +
                exception.what());
            std::lock_guard lock(g_asyncMeshMutex);
            g_failedAsyncMeshArtifacts[{ request.owner, request.path }] = { request.owner, request.realPath, request.writeTime };
            ++completedCount;
            continue;
        }
        catch (...)
        {
            NLS_LOG_ERROR("Async mesh artifact load failed: " + request.realPath + " error=unknown");
            std::lock_guard lock(g_asyncMeshMutex);
            g_failedAsyncMeshArtifacts[{ request.owner, request.path }] = { request.owner, request.realPath, request.writeTime };
            ++completedCount;
            continue;
        }

        {
            std::lock_guard lock(g_asyncMeshMutex);
            auto cancelled = std::find_if(
                g_cancelledAsyncMeshArtifacts.begin(),
                g_cancelledAsyncMeshArtifacts.end(),
                [&request](const auto& entry)
                {
                    return entry.owner == request.owner &&
                        (entry.path == request.path ||
                        ArtifactPathMatchesResolvedPath(entry.realPath, request.realPath));
                });
            if (cancelled != g_cancelledAsyncMeshArtifacts.end())
            {
                g_cancelledAsyncMeshArtifacts.erase(cancelled);
                ++completedCount;
                continue;
            }
        }

        if (artifact.has_value())
        {
            if (FindCachedMeshByEquivalentArtifactPath(manager, request.realPath) != nullptr)
            {
                std::lock_guard lock(g_asyncMeshMutex);
                g_failedAsyncMeshArtifacts.erase({ request.owner, request.path });
                ++completedCount;
                continue;
            }

            if (auto* driver = NLS::Render::Context::TryGetLocatedDriver();
                driver != nullptr &&
                NLS::Render::Context::DriverRendererAccess::HasExplicitRHI(*driver))
            {
                NLS::Render::Context::MeshRuntimeUploadRequest uploadRequest;
                uploadRequest.vertices = std::move(artifact->vertices);
                uploadRequest.indices = std::move(artifact->indices);
                uploadRequest.materialIndex = artifact->materialIndex;
                uploadRequest.boundingSphere = artifact->boundingSphere;
                uploadRequest.debugName = request.path;
                const uint64_t uploadRequestId =
                    NLS::Render::Context::DriverResourceAccess::RequestMeshRuntimeUpload(
                        *driver,
                        std::move(uploadRequest));
                if (uploadRequestId != 0u)
                {
                    request.jobHandle = {};
                    request.future = {};
                    request.runtimeUploadRequestId = uploadRequestId;
                    std::lock_guard lock(g_asyncMeshMutex);
                    g_asyncMeshRequests.emplace(
                        AsyncMeshArtifactStateKey{ request.owner, request.path },
                        std::move(request));
                    ++completedCount;
                    continue;
                }
            }

            MeshManager::Mesh* mesh = nullptr;
            try
            {
                mesh = new MeshManager::Mesh(
                    artifact->vertices,
                    artifact->indices,
                    artifact->materialIndex,
                    NLS::Render::Resources::MeshBufferUploadMode::GpuOnly,
                    artifact->boundingSphere);
                manager.RegisterResource(request.path, mesh);
                mesh = nullptr;
                std::lock_guard lock(g_asyncMeshMutex);
                g_failedAsyncMeshArtifacts.erase({ request.owner, request.path });
            }
            catch (const std::exception& exception)
            {
                if (mesh)
                    manager.DestroyResource(mesh);
                NLS_LOG_ERROR(
                    std::string("Async mesh runtime creation failed: ") +
                    request.realPath +
                    " error=" +
                    exception.what());
                std::lock_guard lock(g_asyncMeshMutex);
                g_failedAsyncMeshArtifacts[{ request.owner, request.path }] = { request.owner, request.realPath, request.writeTime };
            }
            catch (...)
            {
                if (mesh)
                    manager.DestroyResource(mesh);
                NLS_LOG_ERROR("Async mesh runtime creation failed: " + request.realPath + " error=unknown");
                std::lock_guard lock(g_asyncMeshMutex);
                g_failedAsyncMeshArtifacts[{ request.owner, request.path }] = { request.owner, request.realPath, request.writeTime };
            }
        }
        else
        {
            std::lock_guard lock(g_asyncMeshMutex);
            if (request.retryCancelledCompletion &&
                request.cancelableInterestCount + request.sharedInterestCount > 0u)
            {
                request.cancelled = std::make_shared<std::atomic_bool>(false);
                request.jobHandle = {};
                request.future = {};
                request.retryCancelledCompletion = false;
                g_failedAsyncMeshArtifacts.erase({ request.owner, request.path });
                g_asyncMeshRequests.emplace(
                    AsyncMeshArtifactStateKey{ request.owner, request.path },
                    std::move(request));
            }
            else
            {
                g_failedAsyncMeshArtifacts[{ request.owner, request.path }] =
                    { request.owner, request.realPath, request.writeTime };
            }
        }

        ++completedCount;
    }
}

void ClearAsyncMeshArtifactStateForOwner(const MeshManager& manager)
{
    std::vector<AsyncMeshArtifactRequest> removedRequests;
    {
        std::lock_guard lock(g_asyncMeshMutex);
        for (auto it = g_asyncMeshRequests.begin(); it != g_asyncMeshRequests.end();)
        {
            if (it->second.owner == &manager)
            {
                if (it->second.cancelled)
                    it->second.cancelled->store(true, std::memory_order_release);
                removedRequests.push_back(std::move(it->second));
                it = g_asyncMeshRequests.erase(it);
            }
            else
                ++it;
        }
        for (auto it = g_failedAsyncMeshArtifacts.begin(); it != g_failedAsyncMeshArtifacts.end();)
        {
            if (it->second.owner == &manager)
                it = g_failedAsyncMeshArtifacts.erase(it);
            else
                ++it;
        }
        for (auto it = g_cancelledAsyncMeshArtifacts.begin(); it != g_cancelledAsyncMeshArtifacts.end();)
        {
            if (it->owner == &manager)
                it = g_cancelledAsyncMeshArtifacts.erase(it);
            else
                ++it;
        }
    }
    for (auto& request : removedRequests)
    {
        if (request.runtimeUploadRequestId != 0u)
        {
            if (auto* driver = NLS::Render::Context::TryGetLocatedDriver(); driver != nullptr)
            {
                NLS::Render::Context::DriverResourceAccess::CancelMeshRuntimeUpload(
                    *driver,
                    request.runtimeUploadRequestId);
            }
        }
        if (request.jobHandle.id != 0u)
            NLS::Base::Jobs::Complete(request.jobHandle);
    }
}
}

std::string MeshManager::ResolveResourcePath(const std::string& path)
{
    return GetRealPath(path);
}

std::string MeshManager::ResolveArtifactResourcePath(const std::string& path)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    g_artifactResourcePathResolutionCount.fetch_add(1u, std::memory_order_relaxed);
#endif
    auto resolved = ResolveBuiltinMeshArtifactPath(path);
    if (!resolved.empty())
        return resolved;
    return ResolveResourcePath(path);
}

const std::string& MeshManager::ProjectAssetsRoot()
{
    return GetProjectAssetsPath();
}

MeshManager::~MeshManager()
{
    ClearAsyncMeshArtifactStateForOwner(*this);
}

MeshManager::Mesh* MeshManager::CreateResource(const std::string& path)
{
    const auto realPath = ResolveArtifactResourcePath(path);
    if (!IsAuthorizedContentArtifactPath(path) || !IsAuthorizedContentArtifactPath(realPath))
        return nullptr;

    if (!IsMeshArtifactPath(realPath))
        return nullptr;

    auto meshArtifact = NLS::Render::Assets::LoadMeshArtifact(realPath);
    if (!meshArtifact.has_value())
        return nullptr;

    return new Mesh(
        meshArtifact->vertices,
        meshArtifact->indices,
        meshArtifact->materialIndex,
        NLS::Render::Resources::MeshBufferUploadMode::GpuOnly,
        meshArtifact->boundingSphere);
}

MeshManager::Mesh* MeshManager::PrewarmArtifact(const std::string& path)
{
    if (auto* cached = GetResource(path, false))
        return cached;

    const auto realPath = ResolveArtifactResourcePath(path);
    if (!IsAuthorizedContentArtifactPath(path) || !IsAuthorizedContentArtifactPath(realPath))
        return nullptr;

    if (auto* cached = FindCachedMeshByEquivalentArtifactPath(*this, realPath))
        return cached;

    if (!IsMeshArtifactPath(realPath))
        return nullptr;

    std::error_code error;
    if (!std::filesystem::is_regular_file(realPath, error))
        return nullptr;

    const auto* resourceType = GetResourceTypeName();
    ResourceLoadProgressScope::Report({
        resourceType,
        path,
        std::string("Prewarming ") + resourceType + ": " + path,
        false,
        false
    });
    auto* prewarmed = GetResource(path, true);
    ResourceLoadProgressScope::Report({
        resourceType,
        path,
        std::string(prewarmed ? "Prewarmed " : "Failed to prewarm ") + resourceType + ": " + path,
        true,
        prewarmed != nullptr
    });
    return prewarmed;
}

MeshManager::Mesh* MeshManager::RequestAsyncArtifact(const std::string& path, const bool cancelableInterest)
{
    if (auto* cached = GetResource(path, false))
        return cached;

    const auto realPath = ResolveArtifactResourcePath(path);
    if (!std::filesystem::path(path).is_absolute())
    {
        if (auto* cached = FindCachedMeshByEquivalentArtifactPath(*this, realPath))
            return cached;
    }
    if (!IsMeshArtifactPath(realPath))
        return nullptr;

    std::error_code error;
    if (!std::filesystem::is_regular_file(realPath, error))
        return nullptr;

    const auto writeTime = TryGetLastWriteTime(realPath);
    AsyncMeshArtifactRequest request;
    request.owner = this;
    request.path = path;
    request.realPath = realPath;
    request.writeTime = writeTime;
    request.cancelableInterestCount = cancelableInterest ? 1u : 0u;
    request.sharedInterestCount = cancelableInterest ? 0u : 1u;
    {
        std::lock_guard lock(g_asyncMeshMutex);
        if (auto existing = FindAsyncMeshRequestByEquivalentArtifactPath(g_asyncMeshRequests, *this, path, realPath);
            existing != g_asyncMeshRequests.end())
        {
            if (cancelableInterest)
                ++existing->second.cancelableInterestCount;
            else
                ++existing->second.sharedInterestCount;
            EraseCancelledMeshArtifactByEquivalentPath(
                g_cancelledAsyncMeshArtifacts,
                *this,
                path,
                realPath);
            if (existing->second.cancelled)
                existing->second.cancelled->store(false, std::memory_order_release);
            existing->second.retryCancelledCompletion = true;
            return nullptr;
        }
        auto failed = FindFailedMeshLoadByEquivalentArtifactPath(g_failedAsyncMeshArtifacts, *this, path, realPath);
        if (failed != g_failedAsyncMeshArtifacts.end())
        {
            if (ArtifactPathMatchesResolvedPath(failed->second.realPath, realPath) &&
                failed->second.writeTime == writeTime)
            {
                return nullptr;
            }
            g_failedAsyncMeshArtifacts.erase(failed);
        }
        EraseCancelledMeshArtifactByEquivalentPath(g_cancelledAsyncMeshArtifacts, *this, path, realPath);
        if (CountQueuedMeshRequestsForOwner(*this) >= kMaxQueuedAsyncMeshArtifactRequests)
            return nullptr;
        if (CountActiveMeshRequests() < kMaxPendingAsyncMeshArtifactRequests)
        {
            try
            {
                auto load = StartMeshArtifactLoad(request);
                if (load.future.valid())
                {
                    request.jobHandle = load.handle;
                    request.future = std::move(load.future);
                }
                else if (NLS::Base::Jobs::IsJobSystemInitialized())
                {
                    g_failedAsyncMeshArtifacts[{ this, path }] = { this, realPath, writeTime };
                    return nullptr;
                }
            }
            catch (...)
            {
                g_failedAsyncMeshArtifacts[{ this, path }] = { this, realPath, writeTime };
                return nullptr;
            }
        }
        g_asyncMeshRequests.emplace(AsyncMeshArtifactStateKey{ this, path }, std::move(request));
    }

    return nullptr;
}

void MeshManager::CancelAsyncArtifact(const std::string& path)
{
    if (path.empty())
        return;

    const auto realPath = ResolveArtifactResourcePath(path);
    std::lock_guard lock(g_asyncMeshMutex);
    if (auto found = FindAsyncMeshRequestByEquivalentArtifactPath(g_asyncMeshRequests, *this, path, realPath);
        found != g_asyncMeshRequests.end())
    {
        if (found->second.cancelableInterestCount > 0u)
            --found->second.cancelableInterestCount;
        if (found->second.cancelableInterestCount == 0u && found->second.sharedInterestCount == 0u)
        {
            if (found->second.cancelled)
                found->second.cancelled->store(true, std::memory_order_release);
            if (!found->second.future.valid())
            {
                if (found->second.runtimeUploadRequestId != 0u)
                {
                    if (auto* driver = NLS::Render::Context::TryGetLocatedDriver(); driver != nullptr)
                    {
                        NLS::Render::Context::DriverResourceAccess::CancelMeshRuntimeUpload(
                            *driver,
                            found->second.runtimeUploadRequestId);
                    }
                }
                g_asyncMeshRequests.erase(found);
                return;
            }
            g_cancelledAsyncMeshArtifacts.insert({ found->second.owner, found->second.path, found->second.realPath });
        }
        return;
    }
    if (auto failed = FindFailedMeshLoadByEquivalentArtifactPath(g_failedAsyncMeshArtifacts, *this, path, realPath);
        failed != g_failedAsyncMeshArtifacts.end())
    {
        g_failedAsyncMeshArtifacts.erase(failed);
    }
}

bool MeshManager::IsAsyncArtifactLoadPending(const std::string& path) const
{
    {
        std::lock_guard lock(g_asyncMeshMutex);
        if (g_asyncMeshRequests.find({ this, path }) != g_asyncMeshRequests.end())
            return true;
        if (g_asyncMeshRequests.empty())
            return false;
    }

    const auto realPath = ResolveArtifactResourcePath(path);
    std::lock_guard lock(g_asyncMeshMutex);
    return FindAsyncMeshRequestByEquivalentArtifactPath(
        g_asyncMeshRequests,
        *this,
        path,
        realPath) != g_asyncMeshRequests.end();
}

bool MeshManager::IsAsyncArtifactLoadFailed(const std::string& path) const
{
    {
        std::lock_guard lock(g_asyncMeshMutex);
        if (g_failedAsyncMeshArtifacts.empty())
            return false;
    }

    const auto realPath = ResolveArtifactResourcePath(path);
    const auto writeTime = TryGetLastWriteTime(realPath);
    std::lock_guard lock(g_asyncMeshMutex);
    auto failed = FindFailedMeshLoadByEquivalentArtifactPath(g_failedAsyncMeshArtifacts, *this, path, realPath);
    return failed != g_failedAsyncMeshArtifacts.end() &&
        ArtifactPathMatchesResolvedPath(failed->second.realPath, realPath) &&
        failed->second.writeTime == writeTime;
}

void MeshManager::PumpAsyncLoads(const size_t maxCompletions)
{
    PumpAsyncMeshArtifactLoads(*this, maxCompletions, nullptr);
}

void MeshManager::PumpAsyncLoadsForPaths(
    const std::unordered_set<std::string>& paths,
    const size_t maxCompletions)
{
    if (paths.empty())
        return;
    const auto trackedPaths = BuildTrackedMeshArtifactPaths(paths);
    PumpAsyncMeshArtifactLoads(*this, maxCompletions, &trackedPaths);
}

#if defined(NLS_ENABLE_TEST_HOOKS)
void MeshManager::ClearAsyncArtifactRequestStateForTesting()
{
    std::vector<AsyncMeshArtifactRequest> removedRequests;
    {
        std::lock_guard lock(g_asyncMeshMutex);
        removedRequests.reserve(g_asyncMeshRequests.size());
        for (auto& [path, request] : g_asyncMeshRequests)
        {
            if (request.cancelled)
                request.cancelled->store(true, std::memory_order_release);
            removedRequests.push_back(std::move(request));
        }
        g_asyncMeshRequests.clear();
        g_failedAsyncMeshArtifacts.clear();
        g_cancelledAsyncMeshArtifacts.clear();
    }
    for (auto& request : removedRequests)
    {
        if (request.runtimeUploadRequestId != 0u)
        {
            if (auto* driver = NLS::Render::Context::TryGetLocatedDriver(); driver != nullptr)
            {
                NLS::Render::Context::DriverResourceAccess::CancelMeshRuntimeUpload(
                    *driver,
                    request.runtimeUploadRequestId);
            }
        }
        if (request.jobHandle.id != 0u)
            NLS::Base::Jobs::Complete(request.jobHandle);
    }
}

bool MeshManager::WaitForAsyncArtifactWorkersForTesting(const uint32_t timeoutMilliseconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    while (g_activeMeshArtifactWorkers.load(std::memory_order_acquire) != 0u)
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

size_t MeshManager::GetMaxPendingAsyncArtifactRequestCountForTesting()
{
    return kMaxPendingAsyncMeshArtifactRequests;
}

size_t MeshManager::GetPendingAsyncArtifactRequestCountForTesting()
{
    std::lock_guard lock(g_asyncMeshMutex);
    return static_cast<size_t>(std::count_if(
        g_asyncMeshRequests.begin(),
        g_asyncMeshRequests.end(),
        [](const auto& entry)
        {
            return entry.second.future.valid();
        }));
}

size_t MeshManager::GetTotalAsyncArtifactRequestCountForTesting()
{
    std::lock_guard lock(g_asyncMeshMutex);
    return g_asyncMeshRequests.size();
}

size_t MeshManager::GetFailedAsyncArtifactRequestCountForTesting()
{
    std::lock_guard lock(g_asyncMeshMutex);
    return g_failedAsyncMeshArtifacts.size();
}

void MeshManager::ResetArtifactResourcePathResolutionCountForTesting()
{
    g_artifactResourcePathResolutionCount.store(0u, std::memory_order_relaxed);
}

size_t MeshManager::GetArtifactResourcePathResolutionCountForTesting()
{
    return g_artifactResourcePathResolutionCount.load(std::memory_order_relaxed);
}
#endif

void MeshManager::DestroyResource(Mesh* resource)
{
    delete resource;
}

void MeshManager::ReloadResource(Mesh* resource, const std::string& path)
{
    if (resource == nullptr)
        return;

    const auto realPath = ResolveArtifactResourcePath(path);
    if (!IsAuthorizedContentArtifactPath(path) || !IsAuthorizedContentArtifactPath(realPath))
        return;

    if (!IsMeshArtifactPath(realPath))
        return;

    auto meshArtifact = NLS::Render::Assets::LoadMeshArtifact(realPath);
    if (!meshArtifact.has_value())
        return;

    resource->Reload(
        meshArtifact->vertices,
        meshArtifact->indices,
        meshArtifact->materialIndex,
        NLS::Render::Resources::MeshBufferUploadMode::GpuOnly,
        meshArtifact->boundingSphere);
}
}
