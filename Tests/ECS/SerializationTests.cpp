#include <ECS/Serialization/WorldArchive.h>
#include <ECS/Core/World.h>
#include <ECS/Commands/EntityCommandBuffer.h>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <iomanip>
#include <sstream>

namespace
{
using namespace NLS::ECS;
struct Saved
{
    std::string text;
    std::vector<int> values;
    Entity target;
    bool operator==(const Saved&) const = default;
};
struct Group { std::string name; bool operator==(const Group&) const = default; };
struct Flag {};
struct Active { int value = 1; };
struct Derived { int value = 99; };
struct Asset { std::string guid; };
void Register(World& world, uint32_t savedVersion = 1,
    std::function<ComponentJson(uint32_t, const ComponentJson&)> migrate = {})
{
    world.RegisterComponent<Saved>("save.Saved", ComponentKind::Data, savedVersion);
    world.RegisterComponent<Group>("save.Group", ComponentKind::Shared);
    world.RegisterComponent<Flag>("save.Flag", ComponentKind::Tag);
    world.RegisterComponent<Active>("save.Active", ComponentKind::Enableable);
    world.RegisterComponent<DynamicBuffer<int>>("save.Buffer", ComponentKind::Buffer);
    world.RegisterComponent<Derived>("save.Derived");
    world.GetRegistry().RegisterTransient(world.GetComponentType<Derived>().id);
    RegisterComponentCodec<Saved>(world.GetRegistry(),
        [](const Saved& value, const EntityWriteContext& context)
        { return ComponentJson{{"text", value.text}, {"values", value.values}, {"target", context.EntityId(value.target)}}; },
        [](Saved& value, const ComponentJson& json, const EntityReadContext& context)
        { value = {json.at("text").get<std::string>(), json.at("values").get<std::vector<int>>(), context.Resolve(json.at("target").get<std::string>())}; }, std::move(migrate));
    RegisterComponentCodec<Group>(world.GetRegistry(),
        [](const Group& value, const auto&) { return ComponentJson{{"name", value.name}}; },
        [](Group& value, const ComponentJson& json, const auto&) { value.name = json.at("name").get<std::string>(); });
    RegisterComponentCodec<Active>(world.GetRegistry(),
        [](const Active& value, const auto&) { return ComponentJson{{"value", value.value}}; },
        [](Active& value, const ComponentJson& json, const auto&) { value.value = json.at("value").get<int>(); });
    RegisterComponentCodec<DynamicBuffer<int>>(world.GetRegistry(),
        [](const DynamicBuffer<int>& value, const auto&) { return ComponentJson(value.values); },
        [](DynamicBuffer<int>& value, const ComponentJson& json, const auto&) { value.values = json.get<std::vector<int>>(); });
}
WorldDocument Prefab(World& world)
{
    const auto root = world.CreateEntity(), child = world.CreateEntity();
    world.AddComponent<Saved>(root, {"root", {1, 2, 3}, child});
    world.AddComponent<Saved>(child, {"child", {}, root});
    world.AddComponent<Group>(root, {"shared"});
    world.AddComponent<Group>(child, {"shared"});
    world.AddComponent<Flag>(root); world.AddComponent<Active>(root, {7}); world.SetEnabled<Active>(root, false);
    world.AddComponent<DynamicBuffer<int>>(child, {{4, 5, 6}}); world.AddComponent<Derived>(root);
    const std::array records{PersistentEntity{root, "root", "author-root"}, PersistentEntity{child, "child", "author-child"}};
    return WorldArchive::Capture(world, records, DocumentKind::Prefab);
}
template<class T> auto& SavedRecord(T& entity)
{
    const auto found = std::find_if(entity.components.begin(), entity.components.end(), [](const auto& component) { return component.name == "save.Saved"; });
    if (found == entity.components.end()) throw std::logic_error("Missing test fixture component");
    return *found;
}
TEST(ECSSerialization, RoundTripOwnsValuesRemapsReferencesAndPreservesStorageKinds)
{
    World source; Register(source); const auto document = Prefab(source);
    const auto encoded = WorldArchive::Encode(document);
    EXPECT_NE(encoded.find("archetypes"), std::string::npos);
    World target; Register(target);
    const auto instance = WorldArchive::Instantiate(target, WorldArchive::Parse(encoded), {.sceneOwner = 42});
    ASSERT_EQ(instance.entities.size(), 2u);
    const auto root = instance.Find("root"), child = instance.Find("child");
    EXPECT_EQ(target.ReadComponent<Saved>(root).target, child);
    EXPECT_EQ(target.ReadComponent<Saved>(child).target, root);
    EXPECT_EQ(target.ReadComponent<Saved>(root).values, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(target.ReadComponent<DynamicBuffer<int>>(child).values, (std::vector<int>{4, 5, 6}));
    EXPECT_EQ(target.ReadComponent<Group>(root), target.ReadComponent<Group>(child));
    EXPECT_TRUE(target.HasComponent<Flag>(root)); EXPECT_FALSE(target.IsEnabled<Active>(root));
    EXPECT_FALSE(target.HasComponent<Derived>(root)); EXPECT_EQ(instance.byAuthoringId.at("author-root"), root);
    EXPECT_EQ(target.GetScene(child), 42u); EXPECT_TRUE(target.GetComponentType<Saved>().serializable);
    EXPECT_EQ(target.UnloadScene(42), 2u); EXPECT_EQ(source.GetEntityCount(), 2u);
}
TEST(ECSSerialization, PrefabInstancesDoNotShareEntityReferencesAndSupportOverrides)
{
    World source; Register(source); const auto document = Prefab(source);
    World target; Register(target);
    const auto type = target.GetComponentType<Saved>().id;
    const std::array changes{PrefabOverride{"root", type, PrefabOverrideKind::SetField, "/text", "overridden"}};
    const auto first = WorldArchive::InstantiatePrefab(target, document, changes);
    const auto second = WorldArchive::InstantiatePrefab(target, document);
    EXPECT_EQ(target.ReadComponent<Saved>(first.Find("root")).text, "overridden");
    EXPECT_EQ(target.ReadComponent<Saved>(second.Find("root")).text, "root");
    EXPECT_EQ(target.ReadComponent<Saved>(first.Find("root")).target, first.Find("child"));
    EXPECT_NE(first.Find("child"), second.Find("child"));
    auto batch = WorldArchive::InstantiatePrefabs(target, document, 3);
    EXPECT_EQ(batch.size(), 3u); EXPECT_EQ(target.GetEntityCount(), 10u);
    EXPECT_EQ(batch[1].Destroy(target), 2u); EXPECT_EQ(target.GetEntityCount(), 8u);
    EXPECT_THROW(batch[0].Destroy(source), std::invalid_argument);
}
TEST(ECSSerialization, BadReferenceAndDecodeFailureRollBackOnlyNewEntities)
{
    World source; Register(source); auto document = Prefab(source);
    World target; Register(target); const auto retained = target.CreateEntity();
    SavedRecord(document.entities[0]).value["target"] = "missing";
    EXPECT_THROW(WorldArchive::Instantiate(target, document), std::invalid_argument);
    EXPECT_EQ(target.GetEntityCount(), 1u); EXPECT_TRUE(target.IsAlive(retained));
    document = Prefab(source); SavedRecord(document.entities[1]).value["values"] = "bad array";
    EXPECT_THROW(WorldArchive::Instantiate(target, document), std::invalid_argument);
    EXPECT_EQ(target.GetEntityCount(), 1u);
}
TEST(ECSSerialization, RequiresExplicitCodecAndRejectsExternalRuntimeReferences)
{
    World world; world.RegisterComponent<Saved>("save.Saved"); const auto root = world.CreateEntity(); world.AddComponent<Saved>(root);
    EXPECT_THROW(WorldArchive::CaptureAll(world), std::invalid_argument);
    World source; Register(source); const auto document = Prefab(source);
    const auto entities = source.GetEntities(); const std::array subset{PersistentEntity{entities[0], "only", {}}};
    EXPECT_THROW(WorldArchive::Capture(source, subset), std::invalid_argument);
    auto wrong = std::make_shared<ComponentCodec>(); wrong->nativeType = typeid(Group);
    wrong->encode = [](const void*, const auto&) { return ComponentJson::object(); };
    wrong->decode = [](void*, const auto&, const auto&) {};
    EXPECT_THROW(source.GetRegistry().RegisterCodec(source.GetComponentType<Saved>().id, wrong), std::invalid_argument);
    EXPECT_THROW(source.GetRegistry().RegisterTransient(source.GetComponentType<Saved>().id), std::invalid_argument);
}
TEST(ECSSerialization, MigratesOlderValuesAndRejectsFutureOrUnknownTypes)
{
    World old; Register(old); auto document = Prefab(old);
    for (auto& entity : document.entities) for (auto& component : entity.components)
        if (component.name == "save.Saved") { component.value["old_text"] = component.value["text"]; component.value.erase("text"); }
    World target; Register(target, 2, [](uint32_t version, const ComponentJson& json)
    {
        if (version != 1) throw std::invalid_argument("bad migration source");
        auto migrated = json; migrated["text"] = migrated.at("old_text"); migrated.erase("old_text"); return migrated;
    });
    const auto instance = WorldArchive::Instantiate(target, document);
    EXPECT_EQ(target.ReadComponent<Saved>(instance.Find("root")).text, "root");
    SavedRecord(document.entities[0]).version = 3;
    EXPECT_THROW(WorldArchive::Instantiate(target, document), std::invalid_argument);
    SavedRecord(document.entities[0]).type = StableComponentId("unknown");
    EXPECT_THROW(WorldArchive::Instantiate(target, document), std::invalid_argument);
    EXPECT_EQ(target.GetEntityCount(), 2u);
}
TEST(ECSSerialization, DetectsCorruptionDuplicatesLayoutAndLimitsBeforeMutation)
{
    World source; Register(source); auto document = Prefab(source);
    auto envelope = ComponentJson::parse(WorldArchive::Encode(document)); envelope["data"]["entities"][0]["id"] = "tampered";
    EXPECT_THROW(WorldArchive::Parse(envelope.dump()), std::invalid_argument);
    EXPECT_THROW(WorldArchive::Parse("{\"format\":\"a\",\"format\":\"b\"}"), std::invalid_argument);
    ArchiveLimits limits; limits.maxEntities = 1;
    EXPECT_THROW(WorldArchive::Parse(WorldArchive::Encode(document), limits), std::invalid_argument);
    limits = {}; limits.maxBytes = 8;
    EXPECT_THROW(WorldArchive::Encode(document, limits), std::length_error);
    EXPECT_THROW(WorldArchive::Parse(WorldArchive::Encode(document), limits), std::length_error);
    document.entities[1].id = document.entities[0].id;
    EXPECT_THROW(WorldArchive::Encode(document), std::invalid_argument);
    document = Prefab(source); SavedRecord(document.entities[0]).value["bad"] = std::numeric_limits<double>::infinity();
    EXPECT_THROW(WorldArchive::Encode(document), std::invalid_argument);
}
TEST(ECSSerialization, ResourceDependenciesAreTrackedAndValidatedBeforeCreation)
{
    World source; source.RegisterComponent<Asset>("save.Asset");
    RegisterComponentCodec<Asset>(source.GetRegistry(),
        [](const Asset& value, const auto& context) { return ComponentJson{{"guid", context.Resource(value.guid, "revision-7")}}; },
        [](Asset& value, const ComponentJson& json, const auto& context) { value.guid = context.Resource(json.at("guid").template get<std::string>()); });
    const auto entity = source.CreateEntity(); source.AddComponent<Asset>(entity, {"asset-guid"});
    const auto document = WorldArchive::CaptureAll(source);
    ASSERT_EQ(document.resources.size(), 1u);
    EXPECT_THROW(WorldArchive::Instantiate(source, document), std::invalid_argument);
    EXPECT_EQ(source.GetEntityCount(), 1u);
    const auto instance = WorldArchive::Instantiate(source, document, {.resolveResource = [](const ResourceDependency& dependency)
        { return dependency.guid == "asset-guid" && dependency.hashOrVersion == "revision-7"; }});
    EXPECT_EQ(source.ReadComponent<Asset>(instance.entities[0]).guid, "asset-guid");
}
TEST(ECSSerialization, CallbacksCannotMutateWorldAndParserCanRunOnAWorker)
{
    World source; Register(source); const auto text = WorldArchive::Encode(Prefab(source));
    auto pending = std::async(std::launch::async, [&] { return WorldArchive::Parse(text); });
    const auto document = pending.get();
    World target; target.RegisterComponent<Saved>("save.Saved");
    RegisterComponentCodec<Saved>(target.GetRegistry(),
        [&](const Saved&, const auto&) { target.CreateEntity(); return ComponentJson::object(); },
        [&](Saved&, const auto&, const auto&) { target.CreateEntity(); });
    const auto entity = target.CreateEntity(); target.AddComponent<Saved>(entity);
    EXPECT_THROW(WorldArchive::CaptureAll(target), std::logic_error);
    WorldDocument one; one.entities.push_back(document.entities[0]); one.entities[0].components = {SavedRecord(document.entities[0])};
    EXPECT_THROW(WorldArchive::Instantiate(target, one), std::invalid_argument);
    EXPECT_EQ(target.GetEntityCount(), 1u);
    auto worker = std::async(std::launch::async, [&] { EXPECT_THROW(WorldArchive::Instantiate(target, one), std::logic_error); }); worker.get();
}
TEST(ECSSerialization, AtomicSaveReplacesOldDataAndFailedEncodePreservesFile)
{
    World source; Register(source); auto document = Prefab(source);
    const auto directory = std::filesystem::current_path() / "tmp" / ("NullusECSArchive-" + std::to_string(source.GetId()) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory); const auto path = directory / "scene.json";
    struct Cleanup { std::filesystem::path path, directory; ~Cleanup() { std::error_code e; std::filesystem::remove(path, e); std::filesystem::remove(directory, e); } } cleanup{path, directory};
    WorldArchive::WriteFile(path, document);
    EXPECT_EQ(WorldArchive::ReadFile(path).entities.size(), 2u);
    document.kind = DocumentKind::Scene; WorldArchive::WriteFile(path, document);
    EXPECT_EQ(WorldArchive::ReadFile(path).kind, DocumentKind::Scene);
    document.entities[1].id = document.entities[0].id;
    EXPECT_THROW(WorldArchive::WriteFile(path, document), std::invalid_argument);
    EXPECT_EQ(WorldArchive::ReadFile(path).entities.size(), 2u);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator{}), 1);
}
TEST(ECSSerialization, DeferredPrefabOwnsDocumentAndResolvesTargetsForLaterCommands)
{
    World source; Register(source); auto document = Prefab(source);
    World target; Register(target); EntityCommandBuffer commands(target); auto writer = commands.GetWriter();
    const auto deferred = writer.InstantiatePrefab(document, 71);
    writer.SetEnabled<Active>(deferred.Find("root"), true);
    writer.SetComponent<Group>(deferred.Find("child"), {"replacement"});
    document.entities.clear(); source.Close();
    const auto result = commands.Playback(); ASSERT_TRUE(result.Succeeded()); ASSERT_EQ(result.created.size(), 2u);
    const auto root = result.Resolve(deferred.Find("root")), child = result.Resolve(deferred.Find("child"));
    ASSERT_TRUE(root.has_value()); ASSERT_TRUE(child.has_value());
    EXPECT_EQ(target.ReadComponent<Saved>(*root).target, *child);
    EXPECT_EQ(target.ReadComponent<Saved>(*child).target, *root);
    EXPECT_TRUE(target.IsEnabled<Active>(*root)); EXPECT_EQ(target.ReadComponent<Group>(*child).name, "replacement");
    EXPECT_EQ(target.GetScene(*root), 71u);
}
TEST(ECSSerialization, FailedDeferredPrefabRollsBackAndLaterCommandsStillExecute)
{
    World source; Register(source); auto document = Prefab(source);
    SavedRecord(document.entities[1]).value["target"] = "broken";
    World target; Register(target); EntityCommandBuffer commands(target); auto writer = commands.GetWriter();
    const auto failed = writer.InstantiatePrefab(document); const auto surviving = writer.CreateEntity();
    const auto result = commands.Playback(); EXPECT_EQ(result.errors.size(), 1u); EXPECT_EQ(result.applied, 1u);
    EXPECT_FALSE(result.Resolve(failed.Find("root")).has_value()); EXPECT_TRUE(result.Resolve(surviving).has_value());
    EXPECT_EQ(target.GetEntityCount(), 1u);
}
TEST(ECSSerialization, ParallelWritersInstantiateIndependentPrefabGraphs)
{
    World source; Register(source); const auto document = Prefab(source);
    World target; Register(target); EntityCommandBuffer commands(target);
    std::array<RecordedPrefab, 8> recorded;
    std::vector<std::future<void>> jobs;
    for (size_t i = 0; i < recorded.size(); ++i)
        jobs.push_back(std::async(std::launch::async, [&, i] { recorded[i] = commands.GetWriter(i).InstantiatePrefab(document); }));
    for (auto& job : jobs) job.get();
    const auto result = commands.Playback(); ASSERT_TRUE(result.Succeeded()); EXPECT_EQ(result.created.size(), 16u);
    for (const auto& prefab : recorded)
    {
        const auto root = result.Resolve(prefab.Find("root")), child = result.Resolve(prefab.Find("child"));
        ASSERT_TRUE(root.has_value()); ASSERT_TRUE(child.has_value()); EXPECT_EQ(target.ReadComponent<Saved>(*root).target, *child);
    }
}
TEST(ECSSerialization, ResignedMalformedLayoutsAndNonintegralVersionsAreRejected)
{
    World source; Register(source); const auto document = Prefab(source);
    auto envelope = ComponentJson::parse(WorldArchive::Encode(document));
    envelope["data"]["archetypes"][0] = ComponentJson::array();
    std::ostringstream checksum; checksum << std::hex << std::setfill('0') << std::setw(16) << StableComponentId(envelope["data"].dump());
    envelope["checksum"] = checksum.str();
    EXPECT_THROW(WorldArchive::Parse(envelope.dump()), std::invalid_argument);
    envelope = ComponentJson::parse(WorldArchive::Encode(document)); envelope["version"] = 1.0;
    EXPECT_THROW(WorldArchive::Parse(envelope.dump()), std::invalid_argument);
    ArchiveLimits limits; limits.maxDepth = 3;
    EXPECT_THROW(WorldArchive::Parse(WorldArchive::Encode(document), limits), std::length_error);
    EXPECT_THROW(WorldArchive::Encode(document, limits), std::length_error);
}
TEST(ECSSerialization, StructuralPrefabOverridesAddRemoveReplaceAndEnableComponents)
{
    World source; Register(source); const auto document = Prefab(source);
    World target; Register(target);
    PrefabOverride add; add.entityId = "child"; add.type = target.GetComponentType<Flag>().id; add.operation = PrefabOverrideKind::AddComponent;
    add.component = {add.type, "save.Flag", 1, ComponentKind::Tag};
    PrefabOverride remove; remove.entityId = "root"; remove.type = target.GetComponentType<Saved>().id; remove.operation = PrefabOverrideKind::RemoveComponent;
    PrefabOverride replace; replace.entityId = "root"; replace.type = target.GetComponentType<Active>().id; replace.operation = PrefabOverrideKind::SetComponent;
    replace.component = {replace.type, "save.Active", 1, ComponentKind::Enableable, true, {{"value", 42}}};
    PrefabOverride enable; enable.entityId = "root"; enable.type = replace.type; enable.operation = PrefabOverrideKind::SetEnabled; enable.enabled = false;
    const std::array changes{add, remove, replace, enable};
    const auto instance = WorldArchive::InstantiatePrefab(target, document, changes);
    EXPECT_TRUE(target.HasComponent<Flag>(instance.Find("child"))); EXPECT_FALSE(target.HasComponent<Saved>(instance.Find("root")));
    EXPECT_EQ(target.ReadComponent<Active>(instance.Find("root")).value, 42); EXPECT_FALSE(target.IsEnabled<Active>(instance.Find("root")));
    EXPECT_EQ(target.ReadComponent<Saved>(instance.Find("child")).target, instance.Find("root"));
}
TEST(ECSSerialization, FailedBatchInstantiationRollsBackEarlierInstances)
{
    struct Value { int count = 0; };
    World world; const auto id = world.RegisterComponent<Value>("batch.Value").id; int calls = 0;
    RegisterComponentCodec<Value>(world.GetRegistry(), [](const Value& value, const auto&) { return ComponentJson{{"count", value.count}}; },
        [&](Value& value, const ComponentJson& json, const auto&)
        { if (++calls == 3) throw std::runtime_error("batch decode failure"); value.count = json.at("count").get<int>(); });
    const auto retained = world.CreateEntity();
    WorldDocument prefab; prefab.kind = DocumentKind::Prefab;
    prefab.entities.push_back({"one", {}, {{id, "batch.Value", 1, ComponentKind::Data, true, {{"count", 7}}}}});
    EXPECT_THROW(WorldArchive::InstantiatePrefabs(world, prefab, 4), std::invalid_argument);
    EXPECT_EQ(world.GetEntityCount(), 1u); EXPECT_TRUE(world.IsAlive(retained));
}
TEST(ECSSerialization, SaxValidationHandlesLargeArraysAndRejectsNestedDuplicateKeys)
{
    World source; Register(source); const std::array signature{source.GetComponentType<Active>().id};
    source.CreateEntities(10000, signature); const auto document = WorldArchive::CaptureAll(source);
    const auto parsed = WorldArchive::Parse(WorldArchive::Encode(document));
    ASSERT_EQ(parsed.entities.size(), 10000u); EXPECT_EQ(parsed.entities.back().id, "e10000");
    World target({}, source.GetRegistryLifetime()); const auto loaded = WorldArchive::Instantiate(target, parsed);
    EXPECT_EQ(loaded.entities.size(), 10000u); EXPECT_EQ(target.ReadComponent<Active>(loaded.entities.back()).value, 1);
    EXPECT_THROW(WorldArchive::Parse("{\"nested\":{\"key\":1,\"key\":2}}"), std::invalid_argument);
}
}
