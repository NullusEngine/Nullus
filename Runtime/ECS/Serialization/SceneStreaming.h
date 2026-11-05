#pragma once
#include "WorldDocumentSource.h"

namespace NLS::ECS
{
struct SceneLoadHandle
{
    uint64_t streaming = 0;
    uint64_t id = 0;
    bool operator==(const SceneLoadHandle&) const = default;
};
enum class SceneLoadStatus : uint8_t { Loading, Loaded, Cancelled, Failed };
struct SceneLoadInfo
{
    SceneLoadHandle handle;
    uint64_t chunk = 0;
    SceneLoadStatus status = SceneLoadStatus::Loading;
    size_t entityCount = 0;
    double parseMilliseconds = 0;
    bool parsedOnBackgroundWorker = false;
    std::string error;
};

// Chunk keys are application-defined nonzero IDs. Each file is a self-contained
// Scene document; local references are remapped independently in every chunk.
// All methods run on the World owner. Background jobs own only immutable input
// and parse output, so cancellation/destruction cannot leave a borrowed World.
class NLS_ECS_API SceneStreaming final
{
public:
    explicit SceneStreaming(World& world, size_t maxRememberedRequests = 256);
    ~SceneStreaming();
    SceneStreaming(const SceneStreaming&) = delete;
    SceneStreaming& operator=(const SceneStreaming&) = delete;
    SceneLoadHandle RequestLoad(uint64_t chunk, std::filesystem::path path, ArchiveLoadOptions options = {});
    SceneLoadHandle RequestLoadSource(uint64_t chunk, std::shared_ptr<const IWorldDocumentSource> source, ArchiveLoadOptions options = {});
    bool Cancel(SceneLoadHandle handle);
    void CancelAll();
    // Poll only; no waits. Commit at most maxCommits complete chunks (0 = all).
    // A chunk commits as one transaction; this is not a per-entity frame budget.
    size_t Pump(size_t maxCommits = 1);
    // Explicit synchronous tool/test path. Ordinary frames should use Pump.
    SceneLoadInfo Wait(SceneLoadHandle handle);
    SceneLoadInfo GetInfo(SceneLoadHandle handle) const;
    bool Forget(SceneLoadHandle handle); // Terminal request diagnostics only.
    Entity Find(uint64_t chunk, std::string_view localId) const;
    std::vector<Entity> GetEntities(uint64_t chunk) const;
    size_t Unload(uint64_t chunk);
    size_t UnloadAll();
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
