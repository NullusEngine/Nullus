#include "Baking.h"
#include "TransformHierarchy.h"
#include <Engine/GameObject.h>
#include <Engine/Components/TransformComponent.h>
#include <Engine/SceneSystem/Scene.h>
#include <Engine/Serialize/PPtr.h>
#include <algorithm>
#include <set>

namespace NLS::Engine::ECSIntegration
{
std::vector<BakingSource> CaptureSceneBakingSources(const SceneSystem::Scene& scene)
{
    if (scene.IsSerializationReadOnly())
        throw std::invalid_argument("Baking is blocked while authoring types are unresolved");
    if (scene.IsPlaying())
        throw std::invalid_argument("Capture an Edit Scene for authoring Baking, not a running Play Scene");
    auto& identities = Serialize::PersistentManager::Instance();
    identities.PruneObjectGraphIds(scene);
    std::vector<BakingSource> sources;
    sources.reserve(scene.GetGameObjects().size());
    for (const auto* object : scene.GetGameObjects())
    {
        if (!object || !object->IsAlive() || object->IsEditorTransient()) continue;
        if (object->GetScene() != &scene)
            throw std::invalid_argument("Baking source belongs to another Scene");
        sources.push_back({identities.GetOrCreateObjectGraphId(scene, *object), object->GetInstanceID()});
    }
    return sources;
}
namespace
{
GameObject* Resolve(InstanceID id)
{
    auto* object = dynamic_cast<GameObject*>(NLS::Object::IDToPointerNoThreadCheck(id));
    return object && object->IsAlive() && !object->IsEditorTransient() ? object : nullptr;
}
void Definition(const BakerDefinition& value)
{
    if (value.id.empty() || value.id.size() > 256 || value.version == 0 || !value.capture || !value.bake)
        throw std::invalid_argument("Baker needs a stable ID, nonzero version and Capture/Bake callbacks");
}
void ValidateCandidate(const ECS::WorldDocument& document, const std::shared_ptr<ECS::ComponentRegistry>& registry, const BakingOptions& options)
{
    // A real independent baking world validates codecs, references and native
    // hierarchy data before a successful artifact or cache is published.
    ECS::World validation({.name = "ECS Baking validation"}, registry);
    const auto* transform = registry->Find(ECS::StableComponentId("Nullus.ECS.LocalTransform"));
    if (transform && transform->nativeType == typeid(LocalTransform)) RegisterTransformSystem(validation);
    ECS::ArchiveLoadOptions load; load.limits = options.limits;
    load.resolveResource = [&](const ECS::ResourceDependency& dependency)
    {
        const auto guid = NLS::Guid::TryParse(dependency.guid);
        if (!guid || !options.resourceRevision) return false;
        const auto version = options.resourceRevision(*guid); return version && *version == dependency.hashOrVersion;
    };
    ECS::WorldArchive::Instantiate(validation, document, load);
    if (transform) validation.Systems().RunPhase(ECS::SystemPhase::LateUpdate, {});
}
}
std::string BakingCaptureContext::PrimaryId(const GameObject& object) const
{
    const auto found = m_ids.find(object.GetInstanceID()); return found == m_ids.end() ? std::string{} : found->second;
}
std::string BakingCaptureContext::DependsOnResource(const NLS::Guid& guid) const
{
    if (!guid.IsValid() || !m_options.resourceRevision) throw std::invalid_argument("Baker resource dependency needs a valid GUID and revision resolver");
    const auto version = m_options.resourceRevision(guid);
    if (!version) throw std::invalid_argument("Missing baker resource dependency: " + guid.ToString());
    const auto text = guid.ToString();
    for (const auto& dependency : m_dependencies) if (dependency.guid == text)
    {
        if (dependency.hashOrVersion != *version) throw std::invalid_argument("Resource changed while capturing baker inputs");
        return text;
    }
    m_dependencies.push_back({text, *version}); return text;
}
BakingOutput::BakingOutput(const ECS::ComponentRegistry& registry, std::string primary, std::string baker)
    : m_registry(registry), m_primary(std::move(primary)), m_baker(std::move(baker))
{ m_entities.emplace(m_primary, ECS::ArchivedEntity{m_primary, m_primary, {}}); }
std::string BakingOutput::CreateAdditionalEntity(std::string_view key)
{
    if (key.empty() || key.size() > 4096) throw std::invalid_argument("Additional baked entities need a nonempty stable local key");
    const auto scope = "Nullus.ECS.Extra:" + m_primary + ":" + std::to_string(m_baker.size()) + ":" + m_baker + ":" + std::string(key);
    const auto id = NLS::Guid::NewDeterministic(scope).ToString();
    if (id == m_primary) throw std::invalid_argument("Baked additional entity identity collision");
    m_entities.try_emplace(id, ECS::ArchivedEntity{id, {}, {}}); return id;
}
void BakingOutput::AddComponent(std::string_view entityId, ECS::ComponentId typeId, ECS::ComponentJson value, bool enabled)
{
    const auto entity = m_entities.find(std::string(entityId));
    if (entity == m_entities.end()) throw std::invalid_argument("Baker may only write its primary or owned additional entities");
    const auto& type = m_registry.Get(typeId);
    if (type.transient || !type.serializable) throw std::invalid_argument("Baker component needs an explicit serializable schema: " + type.name);
    for (const auto& existing : entity->second.components) if (existing.type == typeId) throw std::invalid_argument("Baker writes the same component twice");
    entity->second.components.push_back({typeId, type.name, type.version, type.kind, enabled, std::move(value)});
}
struct BakingPipeline::Impl
{
    using Key = std::pair<std::string, std::string>;
    struct Contribution { std::map<std::string, ECS::ArchivedEntity> entities; std::vector<ECS::ResourceDependency> resources; };
    struct CacheEntry { std::string input; std::shared_ptr<const Contribution> contribution; };
    struct Captured { Key key; const BakerDefinition* definition; ECS::ComponentJson data; std::string fingerprint; std::vector<ECS::ResourceDependency> resources; };
    std::shared_ptr<ECS::ComponentRegistry> registry;
    std::map<std::string, BakerDefinition> bakers;
    std::map<Key, CacheEntry> cache;
    std::shared_ptr<const ECS::WorldDocument> last;
    std::string lastEncoded;
    std::thread::id owner = std::this_thread::get_id();
    bool baking = false;
};
BakingPipeline::BakingPipeline(std::shared_ptr<ECS::ComponentRegistry> registry) : m_impl(std::make_unique<Impl>())
{
    if (!registry) throw std::invalid_argument("Baking requires an owned component registry");
    m_impl->registry = std::move(registry);
}
BakingPipeline::~BakingPipeline() = default;
void BakingPipeline::CheckThread() const
{
    if (std::this_thread::get_id() != m_impl->owner) throw std::logic_error("GameObject baking runs on its owning main thread");
    if (m_impl->baking) throw std::logic_error("Cannot re-enter or edit a running BakingPipeline");
}
std::shared_ptr<ECS::ComponentRegistry> BakingPipeline::GetRegistry() const { return m_impl->registry; }
void BakingPipeline::RegisterBaker(BakerDefinition definition)
{
    CheckThread(); Definition(definition); const auto id = definition.id;
    if (!m_impl->bakers.emplace(id, std::move(definition)).second) throw std::invalid_argument("Duplicate baker ID: " + id);
}
void BakingPipeline::ReplaceBaker(BakerDefinition definition)
{
    CheckThread(); Definition(definition); const auto found = m_impl->bakers.find(definition.id);
    if (found == m_impl->bakers.end() || definition.version <= found->second.version) throw std::invalid_argument("Replacing a baker must advance its version");
    found->second = std::move(definition);
}
bool BakingPipeline::RemoveBaker(std::string_view id) { CheckThread(); return m_impl->bakers.erase(std::string(id)) != 0; }
void BakingPipeline::InvalidateCache() { CheckThread(); m_impl->cache.clear(); }
BakingResult BakingPipeline::Bake(std::span<const BakingSource> sources, const BakingOptions& options)
{
    CheckThread(); auto& state = *m_impl;
    struct Running { bool& value; Running(bool& flag) : value(flag) { value = true; } ~Running() { value = false; } } running(state.baking);
    BakingResult result; result.artifact = state.last;
    try
    {
        if (sources.size() > options.limits.maxEntities) throw std::length_error("Baking source limit exceeded");
        std::map<std::string, InstanceID> ordered; std::unordered_map<InstanceID, std::string> ids;
        for (const auto& source : sources)
        {
            if (!source.id.IsValid()) throw std::invalid_argument("Baking source needs a persistent ObjectGraph ObjectId");
            if (!Resolve(source.object)) continue;
            const auto id = source.id.GetGuid().ToString();
            if (!ordered.emplace(id, source.object).second || !ids.emplace(source.object, id).second) throw std::invalid_argument("Duplicate baking source object or persistent identity");
        }
        std::vector<Impl::Captured> captured;
        for (const auto& [id, instanceId] : ordered) for (const auto& [bakerId, definition] : state.bakers)
        {
            auto* object = Resolve(instanceId); if (!object) throw std::invalid_argument("Authoring object disappeared during baking");
            std::vector<ECS::ResourceDependency> dependencies; BakingCaptureContext context(ids, options, dependencies);
            auto snapshot = definition.capture(*object, context); ++result.statistics.captured;
            if (!Resolve(instanceId)) throw std::invalid_argument("Capture callbacks must not destroy authoring objects");
            if (!snapshot) continue;
            ECS::ValidateComponentJson(*snapshot, options.limits.maxDepth);
            std::sort(dependencies.begin(), dependencies.end(), [](const auto& a, const auto& b) { return a.guid < b.guid; });
            ECS::ComponentJson resourceInputs = ECS::ComponentJson::array();
            for (const auto& dependency : dependencies) resourceInputs.push_back({dependency.guid, dependency.hashOrVersion});
            auto fingerprint = ECS::ComponentJson{{"version", definition.version}, {"data", *snapshot}, {"resources", std::move(resourceInputs)}}.dump();
            captured.push_back({{id, bakerId}, &definition, std::move(*snapshot), std::move(fingerprint), std::move(dependencies)});
        }
        std::map<Impl::Key, Impl::CacheEntry> candidateCache;
        const auto build = [&](bool full)
        {
            candidateCache.clear(); result.statistics.rebuilt = result.statistics.reused = result.statistics.removed = 0;
            for (const auto& input : captured)
            {
                const auto old = state.cache.find(input.key);
                if (!full && old != state.cache.end() && old->second.input == input.fingerprint)
                { candidateCache.emplace(input.key, old->second); ++result.statistics.reused; continue; }
                BakingOutput output(*state.registry, input.key.first, input.key.second);
                input.definition->bake(input.data, output);
                auto contribution = std::make_shared<Impl::Contribution>(); contribution->entities = std::move(output.m_entities); contribution->resources = input.resources;
                candidateCache.emplace(input.key, Impl::CacheEntry{input.fingerprint, std::move(contribution)}); ++result.statistics.rebuilt;
            }
            for (const auto& [key, entry] : state.cache) if (!candidateCache.contains(key)) ++result.statistics.removed;
            std::map<std::string, ECS::ArchivedEntity> entities; std::map<std::string, std::string> dependencies;
            for (const auto& [key, entry] : candidateCache)
            {
                for (const auto& [id, contribution] : entry.contribution->entities)
                {
                    auto [found, inserted] = entities.try_emplace(id, ECS::ArchivedEntity{id, contribution.authoringId, {}});
                    if (!inserted && found->second.authoringId != contribution.authoringId) throw std::invalid_argument("Baked entity ownership/identity collision");
                    for (const auto& component : contribution.components)
                    {
                        for (const auto& existing : found->second.components) if (existing.type == component.type)
                            throw std::invalid_argument("Multiple bakers own the same entity component: " + component.name);
                        found->second.components.push_back(component);
                    }
                }
                for (const auto& dependency : entry.contribution->resources)
                {
                    const auto [found, inserted] = dependencies.emplace(dependency.guid, dependency.hashOrVersion);
                    if (!inserted && found->second != dependency.hashOrVersion) throw std::invalid_argument("Baking resource versions disagree");
                }
            }
            ECS::WorldDocument document; document.kind = options.kind;
            for (auto& [id, entity] : entities)
            {
                std::sort(entity.components.begin(), entity.components.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
                document.entities.push_back(std::move(entity));
            }
            for (auto& [guid, version] : dependencies) document.resources.push_back({guid, version});
            return document;
        };
        auto document = build(options.mode == BakingMode::Full);
        std::string encoded;
        try { encoded = ECS::WorldArchive::Encode(document, options.limits); ValidateCandidate(document, state.registry, options); }
        catch (...)
        {
            if (options.mode != BakingMode::Incremental || !options.allowFullFallback || !result.statistics.reused) throw;
            result.statistics.fullFallback = true; document = build(true);
            encoded = ECS::WorldArchive::Encode(document, options.limits); ValidateCandidate(document, state.registry, options);
        }
        auto artifact = state.last && state.lastEncoded == encoded ? state.last : std::make_shared<const ECS::WorldDocument>(std::move(document));
        if (!options.outputPath.empty()) ECS::WorldArchive::WriteFile(options.outputPath, *artifact, options.limits);
        state.cache.swap(candidateCache); state.last = std::move(artifact); state.lastEncoded = std::move(encoded);
        result.succeeded = true; result.artifact = state.last; result.statistics.entities = state.last->entities.size();
    }
    catch (const std::exception& error) { result.diagnostics.push_back(error.what()); }
    return result;
}
void RegisterTransformBaker(BakingPipeline& pipeline)
{
    const auto localId = pipeline.GetRegistry()->Get<LocalTransform>().id, parentId = pipeline.GetRegistry()->Get<Parent>().id;
    pipeline.RegisterBaker({"Nullus.TransformBaker", 1,
        [](const GameObject& object, const BakingCaptureContext& context) -> std::optional<ECS::ComponentJson>
        {
            const auto* transform = object.GetTransform(); if (!transform) return std::nullopt;
            const auto* parent = object.GetParent(); const auto parentId = parent ? context.PrimaryId(*parent) : std::string{};
            const bool local = !parentId.empty();
            const auto& position = local ? transform->GetLocalPosition() : transform->GetWorldPosition();
            const auto& rotation = local ? transform->GetLocalRotation() : transform->GetWorldRotation();
            const auto& scale = local ? transform->GetLocalScale() : transform->GetWorldScale();
            return ECS::ComponentJson{{"position", {position.x, position.y, position.z}}, {"rotation", {rotation.x, rotation.y, rotation.z, rotation.w}},
                {"scale", {scale.x, scale.y, scale.z}}, {"parent", parentId}};
        },
        [localId, parentId](const ECS::ComponentJson& input, BakingOutput& output)
        {
            output.AddComponent(output.Primary(), localId, {{"position", input.at("position")}, {"rotation", input.at("rotation")}, {"scale", input.at("scale")}});
            if (!input.at("parent").get<std::string>().empty()) output.AddComponent(output.Primary(), parentId, {{"entity", input.at("parent")}});
        }});
}
BakingPreview::BakingPreview(std::shared_ptr<ECS::ComponentRegistry> registry) : m_world({.name = "ECS Baking preview"}, std::move(registry)) {}
bool BakingPreview::Apply(const BakingResult& result, const ECS::ArchiveLoadOptions& options)
{
    (void)m_world.GetChangeToken();
    if (!result.succeeded || !result.artifact || result.artifact == m_artifact) return false;
    auto candidate = ECS::WorldArchive::Instantiate(m_world, *result.artifact, options);
    if (m_instance.world) m_instance.Destroy(m_world);
    m_instance = std::move(candidate); m_artifact = result.artifact; return true;
}
ECS::Entity BakingPreview::FindAuthoring(const Serialize::ObjectId& id) const
{
    (void)m_world.GetChangeToken();
    const auto found = m_instance.byAuthoringId.find(id.GetGuid().ToString());
    return found != m_instance.byAuthoringId.end() && m_world.IsAlive(found->second) ? found->second : ECS::Entity{};
}
}
