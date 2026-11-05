#pragma once
#include <Engine/EngineDef.h>
#include <Engine/Serialize/ObjectId.h>
#include <Base/Object/Object.h>
#include <ECS/Core/World.h>
#include <ECS/Serialization/WorldArchive.h>
#include <map>
#include <optional>
#include <thread>

namespace NLS::Engine { class GameObject; }
namespace NLS::Engine::SceneSystem { class Scene; }
namespace NLS::Engine::ECSIntegration
{
struct BakingSource
{
    // Supply the persistent ObjectGraph identity, never an InstanceID-derived ID.
    Serialize::ObjectId id;
    InstanceID object = InstanceID_None;
};
// Capture live authoring objects with their real serialized ObjectGraph IDs.
// Newly authored objects receive GUIDs which the Scene serializer persists.
NLS_ENGINE_API std::vector<BakingSource> CaptureSceneBakingSources(const SceneSystem::Scene& scene);
enum class BakingMode : uint8_t { Full, Incremental };
struct BakingOptions
{
    BakingMode mode = BakingMode::Incremental;
    ECS::DocumentKind kind = ECS::DocumentKind::Scene;
    std::function<std::optional<std::string>(const NLS::Guid&)> resourceRevision;
    std::filesystem::path outputPath;
    ECS::ArchiveLimits limits;
    bool allowFullFallback = true;
};
class NLS_ENGINE_API BakingCaptureContext final
{
public:
    std::string PrimaryId(const GameObject& object) const;
    std::string DependsOnResource(const NLS::Guid& guid) const;
private:
    friend class BakingPipeline;
    BakingCaptureContext(const std::unordered_map<InstanceID, std::string>& ids, const BakingOptions& options,
        std::vector<ECS::ResourceDependency>& dependencies) : m_ids(ids), m_options(options), m_dependencies(dependencies) {}
    const std::unordered_map<InstanceID, std::string>& m_ids;
    const BakingOptions& m_options;
    std::vector<ECS::ResourceDependency>& m_dependencies;
};
class NLS_ENGINE_API BakingOutput final
{
public:
    const std::string& Primary() const { return m_primary; }
    std::string CreateAdditionalEntity(std::string_view key);
    void AddComponent(std::string_view entity, ECS::ComponentId type, ECS::ComponentJson value, bool enabled = true);
    template<class T> void AddComponent(std::string_view entity, ECS::ComponentJson value, bool enabled = true)
    { AddComponent(entity, m_registry.Get<T>().id, std::move(value), enabled); }
private:
    friend class BakingPipeline;
    BakingOutput(const ECS::ComponentRegistry& registry, std::string primary, std::string baker);
    const ECS::ComponentRegistry& m_registry;
    std::string m_primary, m_baker;
    std::map<std::string, ECS::ArchivedEntity> m_entities;
};
struct BakerDefinition
{
    std::string id;
    uint32_t version = 1;
    // Capture every value used by Bake, including presence/absence and referenced
    // object fields. Bake consumes this immutable snapshot, not a GameObject.
    // nullopt removes this baker's previous contribution for this object.
    std::function<std::optional<ECS::ComponentJson>(const GameObject&, const BakingCaptureContext&)> capture;
    std::function<void(const ECS::ComponentJson&, BakingOutput&)> bake;
};
struct BakingStatistics
{
    size_t captured = 0, rebuilt = 0, reused = 0, removed = 0, entities = 0;
    bool fullFallback = false;
};
struct BakingResult
{
    bool succeeded = false;
    std::shared_ptr<const ECS::WorldDocument> artifact;
    BakingStatistics statistics;
    std::vector<std::string> diagnostics;
};
class NLS_ENGINE_API BakingPipeline final
{
public:
    explicit BakingPipeline(std::shared_ptr<ECS::ComponentRegistry> registry);
    ~BakingPipeline();
    BakingPipeline(const BakingPipeline&) = delete;
    BakingPipeline& operator=(const BakingPipeline&) = delete;
    void RegisterBaker(BakerDefinition definition);
    void ReplaceBaker(BakerDefinition definition); // Must advance the version.
    bool RemoveBaker(std::string_view id);
    BakingResult Bake(std::span<const BakingSource> sources, const BakingOptions& options = {});
    std::shared_ptr<ECS::ComponentRegistry> GetRegistry() const;
    void InvalidateCache();
private:
    void CheckThread() const;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
NLS_ENGINE_API void RegisterTransformBaker(BakingPipeline& pipeline);

// Explicit CPU preview application. Editor input/Undo hooks are separate.
// Each successful changed artifact replaces owned entities after loading the
// candidate; unchanged artifacts preserve handles. Play worlds are independent.
class NLS_ENGINE_API BakingPreview final
{
public:
    explicit BakingPreview(std::shared_ptr<ECS::ComponentRegistry> registry);
    ECS::World& GetWorld() { return m_world; }
    bool Apply(const BakingResult& result, const ECS::ArchiveLoadOptions& options = {});
    ECS::Entity FindAuthoring(const Serialize::ObjectId& id) const;
private:
    ECS::World m_world;
    ECS::ArchiveInstance m_instance;
    std::shared_ptr<const ECS::WorldDocument> m_artifact;
};
}
