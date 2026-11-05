#include <gtest/gtest.h>
#include "Engine/AssemblyEngine.h"
#include "Engine/GameObject.h"
#include "Engine/SceneSystem/Scene.h"
#include "Engine/SceneSystem/SceneManager.h"
#include "Engine/Serialize/ObjectGraphInstantiator.h"
#include "Engine/Serialize/ObjectGraphSerializer.h"
#include "Engine/Serialize/ObjectGraphWriter.h"
#include "Guid.h"
#include <fstream>
TEST(PluginSerialization, MissingTypeBlocksInstantiationAndAllCentralSavePaths)
{
    using namespace NLS::Engine::Serialize;
    NLS::Engine::AssemblyEngine::Initialize(); ObjectGraphDocument document;
    { NLS::Engine::SceneSystem::Scene scene; scene.CreateGameObject("PluginOwner"); document=ObjectGraphSerializer::SerializeScene(scene); }
    ObjectRecord missing; missing.id=ObjectId(NLS::Guid::New()); missing.localIdentifierInFile=MakeLocalIdentifierInFile(missing.id);
    missing.typeName="Vendor::UnavailablePlugin::Component"; missing.debugPath="/PluginOwner/Missing";
    missing.properties.push_back({"opaquePayload",PropertyValue::String("original data must survive")});
    for (auto& object : document.objects) if (object.typeName == "NLS::Engine::GameObject")
        for (auto& property : object.properties) if (property.name == "components")
        { auto values=property.value.GetArray(); values.push_back(PropertyValue::OwnedReference(missing.id)); property.value=PropertyValue::Array(std::move(values)); }
    document.objects.push_back(std::move(missing)); const auto original=ObjectGraphWriter::Write(document);
    EXPECT_EQ(ObjectGraphInstantiator::InstantiateScene(document),nullptr);
    LoadPolicy policy; policy.unknownTypePolicy=UnknownTypePolicy::Preserve;
    auto recovery=ObjectGraphInstantiator::InstantiateScene(document,policy);
    ASSERT_TRUE(recovery.scene); EXPECT_TRUE(recovery.scene->IsSerializationReadOnly());
    EXPECT_EQ(recovery.scene->GetSerializationRecoveryDocument(),original);
    EXPECT_THROW(ObjectGraphSerializer::SerializeScene(*recovery.scene),std::exception);
    auto* owner=recovery.scene->FindGameObjectByName("PluginOwner"); ASSERT_NE(owner,nullptr);
    EXPECT_THROW(ObjectGraphSerializer::SerializePrefab(*owner),std::exception);
    const auto root=std::filesystem::temp_directory_path() / ("NullusSerialization-" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path root; ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(root,ignored); } } cleanup{root};
    { std::ofstream file(root / "original.scene"); file << original; }
    for (const auto* name : {"original.scene","save-as.scene","autosave.scene","prefab.prefab"})
        EXPECT_FALSE(NLS::Engine::SceneSystem::SceneManager::SaveSceneToPath(*recovery.scene,(root / name).generic_string()));
    std::ifstream file(root / "original.scene"); const std::string after((std::istreambuf_iterator<char>(file)),{});
    EXPECT_EQ(after,original); EXPECT_FALSE(std::filesystem::exists(root / "save-as.scene")); EXPECT_FALSE(std::filesystem::exists(root / "autosave.scene"));
}
