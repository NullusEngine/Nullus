#include "SceneStreaming.h"
#include "ECS/Core/World.h"
#include <Base/Jobs/BackgroundJobQueue.h>
#include <Base/Jobs/JobSystem.h>
#include <Base/Jobs/JobSafety.h>
#include <atomic>
#include <chrono>
#include <map>
#include <optional>
#include <thread>

namespace NLS::ECS
{
namespace Jobs = Base::Jobs;
namespace
{
class FileWorldDocumentSource final : public IWorldDocumentSource
{
public:
    explicit FileWorldDocumentSource(std::filesystem::path path) : m_path(std::filesystem::absolute(path)) {}
    WorldDocument Read(ArchiveLimits limits) const override { return WorldArchive::ReadFile(m_path, limits); }
private:
    const std::filesystem::path m_path;
};
uint64_t AllocateStreamingId()
{
    static std::atomic<uint64_t> next{1};
    auto id = next.load(std::memory_order_relaxed);
    do { if (id == UINT64_MAX) throw std::overflow_error("ECS streaming service IDs exhausted"); }
    while (!next.compare_exchange_weak(id, id + 1, std::memory_order_relaxed));
    return id;
}
}
struct SceneStreaming::Impl
{
    struct ParseState
    {
        std::atomic<bool> cancelled{false}, finished{false};
        std::optional<WorldDocument> document;
        std::string error;
        double milliseconds = 0;
        bool onBackgroundWorker = false;
    };
    struct ParseJob
    {
        std::shared_ptr<ParseState> state;
        std::shared_ptr<const IWorldDocumentSource> source;
        ArchiveLimits limits;
        static void Run(void* pointer)
        {
            std::unique_ptr<ParseJob> job(static_cast<ParseJob*>(pointer));
            auto& state = *job->state;
            const auto start = std::chrono::steady_clock::now();
            state.onBackgroundWorker = Jobs::Internal::IsExecutingBackgroundJobWorkerThread();
            try
            {
                if (!state.cancelled.load(std::memory_order_acquire))
                {
                    auto document = job->source->Read(job->limits);
                    if (document.kind != DocumentKind::Scene) throw std::invalid_argument("Scene streaming requires a Scene document");
                    if (!state.cancelled.load(std::memory_order_acquire)) state.document = std::move(document);
                }
            }
            catch (const std::exception& error) { state.error = error.what(); }
            catch (...) { state.error = "Unknown ECS scene parsing failure"; }
            state.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            state.finished.store(true, std::memory_order_release);
        }
        static void Cancel(void* pointer)
        {
            std::unique_ptr<ParseJob> job(static_cast<ParseJob*>(pointer));
            job->state->cancelled.store(true, std::memory_order_release);
            job->state->finished.store(true, std::memory_order_release);
        }
    };
    struct Request
    {
        SceneLoadInfo info;
        std::shared_ptr<ParseState> parse;
        Jobs::JobHandle job;
        ArchiveLoadOptions options; // Owner-thread callbacks never enter ParseJob.
    };
    struct Chunk { SceneLoadHandle pending; std::optional<ArchiveInstance> instance; };
    World& world;
    std::weak_ptr<const void> lifetime;
    std::thread::id owner = std::this_thread::get_id();
    std::map<uint64_t, Request> requests;
    std::map<uint64_t, Chunk> chunks;
    uint64_t nextRequest = 1;
    const uint64_t identity = AllocateStreamingId();
    size_t maximum;
    bool pumping = false;
    Impl(World& target, size_t maxRequests) : world(target), lifetime(target.GetLifetime()), maximum(maxRequests) {}
    void CheckThread() const
    { if (owner != std::this_thread::get_id()) throw std::logic_error("ECS scene streaming requires the World owning thread"); }
    void CheckWorld(bool safePoint = true) const
    {
        CheckThread();
        if (lifetime.expired()) throw std::logic_error("ECS scene streaming World was closed or destroyed");
        if (safePoint) (void)world.GetChangeToken();
    }
    void CheckEditable() const
    {
        CheckWorld();
        if (pumping) throw std::logic_error("Reentrant ECS scene streaming mutation during commit");
    }
    Request& Require(SceneLoadHandle handle)
    {
        const auto found = requests.find(handle.id);
        if (found == requests.end() || found->second.info.handle != handle) throw std::invalid_argument("Foreign, unknown or forgotten ECS scene load request");
        return found->second;
    }
    bool Cancel(Request& request)
    {
        if (request.info.status != SceneLoadStatus::Loading) return false;
        request.parse->cancelled.store(true, std::memory_order_release);
        request.info.status = SceneLoadStatus::Cancelled;
        auto& chunk = chunks.at(request.info.chunk);
        if (chunk.pending == request.info.handle) chunk.pending = {};
        return true;
    }
    bool Commit(Request& request)
    {
        if (request.info.status != SceneLoadStatus::Loading)
        {
            if (request.parse->finished.load(std::memory_order_acquire)) request.parse->document.reset();
            request.options = {};
            return false;
        }
        if (!request.parse->finished.load(std::memory_order_acquire)) return false;
        auto& parse = *request.parse;
        auto& chunk = chunks.at(request.info.chunk);
        request.info.parseMilliseconds = parse.milliseconds;
        request.info.parsedOnBackgroundWorker = parse.onBackgroundWorker;
        if (chunk.pending != request.info.handle || parse.cancelled.load(std::memory_order_acquire))
            request.info.status = SceneLoadStatus::Cancelled;
        else if (!parse.error.empty() || !parse.document)
        {
            request.info.status = SceneLoadStatus::Failed;
            request.info.error = parse.error.empty() ? "ECS scene parse produced no document" : parse.error;
        }
        else
        {
            try
            {
                auto candidate = WorldArchive::Instantiate(world, *parse.document, request.options);
                // A resource/codec callback may cooperatively cancel this load.
                if (world.IsCancellationRequested() || parse.cancelled.load(std::memory_order_acquire) || chunk.pending != request.info.handle)
                {
                    candidate.Destroy(world); request.info.status = SceneLoadStatus::Cancelled;
                }
                else
                {
                    if (chunk.instance) chunk.instance->Destroy(world);
                    chunk.instance = std::move(candidate);
                    request.info.entityCount = chunk.instance->entities.size();
                    request.info.status = SceneLoadStatus::Loaded;
                }
            }
            catch (const std::exception& error)
            { request.info.status = SceneLoadStatus::Failed; request.info.error = error.what(); }
            catch (...)
            { request.info.status = SceneLoadStatus::Failed; request.info.error = "Unknown ECS scene commit failure"; }
        }
        if (chunk.pending == request.info.handle) chunk.pending = {};
        parse.document.reset(); request.options = {}; // Release callbacks on owner.
        return request.info.status == SceneLoadStatus::Loaded;
    }
    struct PumpScope
    {
        bool& value;
        explicit PumpScope(bool& flag) : value(flag) { value = true; }
        ~PumpScope() { value = false; }
    };
};
SceneStreaming::SceneStreaming(World& world, size_t maxRememberedRequests)
    : m_impl(std::make_unique<Impl>(world, maxRememberedRequests))
{
    m_impl->CheckWorld();
    if (!maxRememberedRequests) throw std::invalid_argument("ECS streaming request history must be nonzero");
}
SceneStreaming::~SceneStreaming()
{
    // Parse jobs contain no World, manager, resource resolver or engine object.
    // They can finish after this service and its World have been destroyed.
    for (auto& [id, request] : m_impl->requests)
        request.parse->cancelled.store(true, std::memory_order_release);
}
SceneLoadHandle SceneStreaming::RequestLoad(uint64_t chunkId, std::filesystem::path path, ArchiveLoadOptions options)
{
    if (path.empty()) throw std::invalid_argument("ECS scene load needs a file path");
    return RequestLoadSource(chunkId, std::make_shared<FileWorldDocumentSource>(std::move(path)), std::move(options));
}
SceneLoadHandle SceneStreaming::RequestLoadSource(uint64_t chunkId, std::shared_ptr<const IWorldDocumentSource> source, ArchiveLoadOptions options)
{
    auto& state = *m_impl; state.CheckEditable();
    if (!chunkId || !source) throw std::invalid_argument("ECS scene load needs a nonzero chunk key and an owned document source");
    if (!Jobs::IsJobSystemInitialized()) throw std::logic_error("Initialize the shared JobSystem before streaming ECS scenes");
    if (state.requests.size() >= state.maximum) throw std::length_error("Forget terminal ECS scene requests before loading more chunks");
    if (!state.nextRequest) throw std::overflow_error("ECS scene load request IDs exhausted");
    auto& chunk = state.chunks.try_emplace(chunkId).first->second;
    const SceneLoadHandle handle{state.identity, state.nextRequest++};
    auto parse = std::make_shared<Impl::ParseState>();
    auto payload = std::make_unique<Impl::ParseJob>(Impl::ParseJob{parse, std::move(source), options.limits});
    auto& request = state.requests.try_emplace(handle.id).first->second;
    request.info.handle = handle; request.info.chunk = chunkId; request.parse = std::move(parse); request.options = std::move(options);
    Jobs::BackgroundJobDesc desc; desc.function = &Impl::ParseJob::Run; desc.userData = payload.get();
    desc.cancelFunction = &Impl::ParseJob::Cancel; desc.cancelUserData = payload.get(); desc.debugName = "ECS scene file parse";
    try { request.job = Jobs::ScheduleBackgroundJob(desc); }
    catch (...) { state.requests.erase(handle.id); throw; }
    if (!request.job.id) { state.requests.erase(handle.id); throw std::runtime_error("JobSystem rejected an ECS background scene load"); }
    (void)payload.release();
    if (chunk.pending.id) state.Cancel(state.Require(chunk.pending));
    chunk.pending = handle;
    return handle;
}
bool SceneStreaming::Cancel(SceneLoadHandle handle)
{ m_impl->CheckThread(); return m_impl->Cancel(m_impl->Require(handle)); }
void SceneStreaming::CancelAll()
{ m_impl->CheckThread(); for (auto& [id, request] : m_impl->requests) m_impl->Cancel(request); }
size_t SceneStreaming::Pump(size_t maxCommits)
{
    auto& state = *m_impl; state.CheckEditable(); Impl::PumpScope scope(state.pumping);
    if (state.world.IsCancellationRequested())
        for (auto& [id, request] : state.requests) state.Cancel(request);
    size_t committed = 0;
    for (auto& [id, request] : state.requests)
    {
        if (maxCommits && committed >= maxCommits) break;
        committed += state.Commit(request) ? 1 : 0;
    }
    return committed;
}
SceneLoadInfo SceneStreaming::Wait(SceneLoadHandle handle)
{
    auto& state = *m_impl; state.CheckEditable(); auto& request = state.Require(handle);
    if (state.world.IsCancellationRequested()) state.Cancel(request);
    if (request.info.status == SceneLoadStatus::Loading)
    {
        if (Jobs::IsJobSyncWaitDisallowedForCurrentThread()) throw std::logic_error("Synchronous ECS scene loading is disallowed in this Jobs scope");
        if (!request.parse->finished.load(std::memory_order_acquire)) Jobs::CompleteNoClear(request.job);
        Impl::PumpScope scope(state.pumping); state.Commit(request);
    }
    return request.info;
}
SceneLoadInfo SceneStreaming::GetInfo(SceneLoadHandle handle) const
{ m_impl->CheckThread(); return m_impl->Require(handle).info; }
bool SceneStreaming::Forget(SceneLoadHandle handle)
{
    m_impl->CheckThread();
    if (m_impl->pumping) throw std::logic_error("Cannot forget an ECS load during a commit callback");
    const auto found = m_impl->requests.find(handle.id);
    if (found == m_impl->requests.end() || found->second.info.handle != handle || found->second.info.status == SceneLoadStatus::Loading) return false;
    m_impl->requests.erase(found); return true;
}
Entity SceneStreaming::Find(uint64_t chunk, std::string_view localId) const
{
    m_impl->CheckWorld(false); const auto found = m_impl->chunks.find(chunk);
    if (found == m_impl->chunks.end() || !found->second.instance) return {};
    const auto entity = found->second.instance->Find(localId); return m_impl->world.IsAlive(entity) ? entity : Entity{};
}
std::vector<Entity> SceneStreaming::GetEntities(uint64_t chunk) const
{
    m_impl->CheckWorld(false); const auto found = m_impl->chunks.find(chunk);
    std::vector<Entity> result;
    if (found != m_impl->chunks.end() && found->second.instance)
        for (const auto entity : found->second.instance->entities) if (m_impl->world.IsAlive(entity)) result.push_back(entity);
    return result;
}
size_t SceneStreaming::Unload(uint64_t chunkId)
{
    auto& state = *m_impl; state.CheckEditable(); const auto found = state.chunks.find(chunkId);
    if (found == state.chunks.end()) return 0;
    auto& chunk = found->second;
    if (chunk.pending.id) state.Cancel(state.Require(chunk.pending));
    const auto count = chunk.instance ? chunk.instance->Destroy(state.world) : 0;
    state.chunks.erase(found); return count;
}
size_t SceneStreaming::UnloadAll()
{
    m_impl->CheckEditable(); size_t count = 0;
    while (!m_impl->chunks.empty()) count += Unload(m_impl->chunks.begin()->first);
    return count;
}
}
