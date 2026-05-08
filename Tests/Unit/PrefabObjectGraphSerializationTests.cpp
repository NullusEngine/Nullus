#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "Components/LightComponent.h"
#include "Components/TransformComponent.h"
#include "GameObject.h"
#include "SceneSystem/Scene.h"
#include "Serialize/ObjectGraphInstantiator.h"
#include "Serialize/ObjectGraphReader.h"
#include "Serialize/ObjectGraphSerializer.h"
#include "Serialize/ObjectGraphWriter.h"
#include "Serialize/PrefabDocument.h"

namespace
{
std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream file(path);
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

const NLS::Engine::Serialize::PropertyRecord* FindProperty(
    const NLS::Engine::Serialize::ObjectRecord& record,
    const char* name)
{
    for (const auto& property : record.properties)
    {
        if (property.name == name)
            return &property;
    }
    return nullptr;
}

NLS::Engine::Serialize::ObjectRecord* FindRecord(
    NLS::Engine::Serialize::ObjectGraphDocument& document,
    NLS::Engine::Serialize::ObjectId id)
{
    for (auto& record : document.objects)
    {
        if (record.id == id)
            return &record;
    }
    return nullptr;
}
}

TEST(PrefabObjectGraphSerializationTests, PrefabDocumentSavesAndLoadsGameObjectRoot)
{
    using namespace NLS::Engine;
    using namespace NLS::Engine::Serialize;

    GameObject prefabRoot("Lamp", "Prop");
    prefabRoot.AddComponent<Components::LightComponent>()->SetIntensity(2.0f);

    const auto prefab = ObjectGraphSerializer::SerializePrefab(prefabRoot);
    const auto text = ObjectGraphWriter::Write(prefab.graph);
    const auto loaded = ObjectGraphReader::Read(text);

    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->format, "Nullus.ObjectGraph.Prefab");
    ASSERT_FALSE(loaded->Validate().HasErrors());

    const auto* root = FindRecord(const_cast<ObjectGraphDocument&>(*loaded), loaded->root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->typeName, "NLS::Engine::GameObject");
    ASSERT_NE(FindProperty(*root, "components"), nullptr);
}

TEST(PrefabObjectGraphSerializationTests, PrefabObjectGraphMatchesGoldenOutput)
{
    NLS::Engine::GameObject prefabRoot("Lamp", "Prop");
    prefabRoot.AddComponent<NLS::Engine::Components::LightComponent>()->SetIntensity(2.0f);

    const auto prefab = NLS::Engine::Serialize::ObjectGraphSerializer::SerializePrefab(prefabRoot);
    const auto output = NLS::Engine::Serialize::ObjectGraphWriter::Write(prefab.graph);
    const auto goldenPath =
        std::filesystem::path(NLS_ROOT_DIR) /
        "Tests/Unit/Fixtures/ObjectGraph/simple_prefab.objectgraph.json";

    EXPECT_EQ(output, ReadTextFile(goldenPath));
}

TEST(PrefabObjectGraphSerializationTests, PrefabInstantiatesWithNewObjectIdsAndSourceToInstanceMapping)
{
    using namespace NLS::Engine;
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    GameObject prefabRoot("Lamp", "Prop");
    prefabRoot.AddComponent<LightComponent>()->SetIntensity(4.0f);

    const auto prefab = ObjectGraphSerializer::SerializePrefab(prefabRoot);
    SceneSystem::Scene scene;
    const auto result = ObjectGraphInstantiator::InstantiatePrefab(prefab, scene);

    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->GetName(), "Lamp");
    EXPECT_EQ(result.root->GetTag(), "Prop");
    ASSERT_EQ(scene.GetActors().size(), 1u);
    EXPECT_EQ(scene.GetActors()[0], result.root);

    ASSERT_GE(result.sourceToInstance.size(), 2u);
    for (const auto& mapping : result.sourceToInstance)
    {
        EXPECT_TRUE(mapping.first.IsValid());
        EXPECT_TRUE(mapping.second.IsValid());
        EXPECT_NE(mapping.first, mapping.second);
    }

    auto* light = result.root->GetComponent<LightComponent>();
    ASSERT_NE(light, nullptr);
    EXPECT_EQ(light->gameobject(), result.root);
    EXPECT_FLOAT_EQ(light->GetIntensity(), 4.0f);
}

TEST(PrefabObjectGraphSerializationTests, PrefabOverridesReplaceInsertRemoveAndMoveOwnedObjects)
{
    using namespace NLS::Engine;
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    GameObject prefabRoot("Base", "Prop");
    prefabRoot.AddComponent<LightComponent>()->SetIntensity(1.0f);

    auto prefab = ObjectGraphSerializer::SerializePrefab(prefabRoot);
    auto* rootRecord = FindRecord(prefab.graph, prefab.graph.root);
    ASSERT_NE(rootRecord, nullptr);
    const auto* componentsProperty = FindProperty(*rootRecord, "components");
    ASSERT_NE(componentsProperty, nullptr);
    ASSERT_EQ(componentsProperty->value.GetKind(), PropertyValue::Kind::Array);
    ASSERT_EQ(componentsProperty->value.GetArray().size(), 2u);

    const auto transformSourceId = componentsProperty->value.GetArray()[0].GetObjectId();
    const auto lightSourceId = componentsProperty->value.GetArray()[1].GetObjectId();
    const auto insertedLightId = ObjectId(NLS::Guid::NewDeterministic("Prefab.Inserted.Light"));

    ObjectRecord insertedLight;
    insertedLight.id = insertedLightId;
    insertedLight.typeName = "NLS::Engine::Components::LightComponent";
    insertedLight.properties.push_back({"intensity", PropertyValue::Number(8.0)});
    prefab.graph.objects.push_back(std::move(insertedLight));

    prefab.graph.overrides.push_back(PatchOperation::ReplaceProperty(prefab.graph.root, "name", PropertyValue::String("Variant")));
    prefab.graph.overrides.push_back(PatchOperation::InsertOwned(prefab.graph.root, "components", insertedLightId, 1));
    prefab.graph.overrides.push_back(PatchOperation::RemoveOwned(prefab.graph.root, "components", lightSourceId));
    prefab.graph.overrides.push_back(PatchOperation::MoveOwned(prefab.graph.root, "components", transformSourceId, 1));

    SceneSystem::Scene scene;
    const auto result = ObjectGraphInstantiator::InstantiatePrefab(prefab, scene);

    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(result.root->GetName(), "Variant");

    const auto& components = result.root->GetComponents();
    ASSERT_EQ(components.size(), 2u);
    auto* loadedInsertedLight = dynamic_cast<LightComponent*>(components[0].get());
    ASSERT_NE(loadedInsertedLight, nullptr);
    EXPECT_FLOAT_EQ(loadedInsertedLight->GetIntensity(), 8.0f);
    EXPECT_NE(dynamic_cast<TransformComponent*>(components[1].get()), nullptr);
    EXPECT_EQ(result.root->GetComponent<LightComponent>(), loadedInsertedLight);
}

TEST(PrefabObjectGraphSerializationTests, PrefabVariantPreservesBasePrefabReferenceAndDiagnosesInvalidOverrides)
{
    using namespace NLS::Engine;
    using namespace NLS::Engine::Serialize;

    GameObject prefabRoot("Variant", "Prop");
    auto prefab = ObjectGraphSerializer::SerializePrefab(prefabRoot);
    prefab.graph.basePrefab = AssetReferenceValue {
        AssetId(NLS::Guid::Parse("33333333-3333-4333-8333-333333333333")),
        "Prefab",
        "Assets/Prefabs/Base.prefab"
    };
    prefab.graph.overrides.push_back(PatchOperation::ReplaceProperty(
        ObjectId(NLS::Guid::NewDeterministic("MissingOverrideTarget")),
        "name",
        PropertyValue::String("Broken")));

    const auto diagnostics = ObjectGraphInstantiator::ValidatePrefab(prefab);
    ASSERT_TRUE(diagnostics.HasErrors());
    ASSERT_FALSE(diagnostics.GetItems().empty());
    EXPECT_EQ(diagnostics.GetItems()[0].GetCode(), SerializationDiagnosticCode::InvalidPrefabOverride);

    const auto text = ObjectGraphWriter::Write(prefab.graph);
    const auto loaded = ObjectGraphReader::Read(text);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_TRUE(loaded->basePrefab.has_value());
    EXPECT_EQ(loaded->basePrefab->asset.GetGuid().ToString(), "33333333-3333-4333-8333-333333333333");
    EXPECT_EQ(loaded->basePrefab->expectedType, "Prefab");
    EXPECT_EQ(loaded->basePrefab->pathHint, "Assets/Prefabs/Base.prefab");
}
