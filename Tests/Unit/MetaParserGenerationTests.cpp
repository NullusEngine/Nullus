#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
std::string ReadAllText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::in | std::ios::binary);
    EXPECT_TRUE(input.is_open()) << "Failed to open " << path.string();
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void ExpectContains(const std::string& content, const std::string& needle)
{
    EXPECT_NE(content.find(needle), std::string::npos) << "Missing generated fragment: " << needle;
}
} // namespace

TEST(MetaParserGenerationTests, GeneratesModuleSpecificRegistrationEntrypoints)
{
    const std::filesystem::path baseHeader = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Base/Gen/MetaGenerated.h";
    const std::filesystem::path engineHeader = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Gen/MetaGenerated.h";

    const std::string baseText = ReadAllText(baseHeader);
    const std::string engineText = ReadAllText(engineHeader);

    ExpectContains(baseText, "LinkReflectionTypes_NLS_Base");
    ExpectContains(baseText, "NLS_META_GENERATED_LINK_FUNCTION");
    ExpectContains(engineText, "LinkReflectionTypes_NLS_Engine");
    ExpectContains(engineText, "NLS_META_GENERATED_LINK_FUNCTION");
}

TEST(MetaParserGenerationTests, GeneratesExpectedBaseReflectionBindings)
{
    const std::filesystem::path sampleSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Base/Gen/Reflection/MetaParserFieldMethodSample.generated.cpp";
    const std::filesystem::path metaSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Base/Gen/MetaGenerated.cpp";
    const std::string sampleText = ReadAllText(sampleSource);
    const std::string metaText = ReadAllText(metaSource);

    ExpectContains(sampleText, "AllocateType(\"NLS::meta::MetaParserFieldMethodSample\")");
    ExpectContains(sampleText, "AddField<NLS::meta::MetaParserFieldMethodSample, int>(\"Value\"");
    ExpectContains(sampleText, "AddMethod(\"GetValue\", &NLS::meta::MetaParserFieldMethodSample::GetValue, {})");
    ExpectContains(metaText, "LinkReflectionTypes_NLS_Base");
    ExpectContains(metaText, "#include \"Reflection/MetaParserFieldMethodSample.generated.cpp\"");
}

TEST(MetaParserGenerationTests, GeneratesExpectedEngineReflectionBindings)
{
    const std::filesystem::path componentSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Gen/Components/Component.generated.cpp";
    const std::filesystem::path sceneSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Gen/SceneSystem/Scene.generated.cpp";
    const std::filesystem::path gameObjectSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Gen/GameObject.generated.cpp";
    const std::filesystem::path engineMetaSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Gen/MetaGenerated.cpp";
    const std::string componentText = ReadAllText(componentSource);
    const std::string sceneText = ReadAllText(sceneSource);
    const std::string gameObjectText = ReadAllText(gameObjectSource);
    const std::string metaText = ReadAllText(engineMetaSource);

    ExpectContains(componentText, "AllocateType(\"NLS::Engine::Components::Component\")");
    ExpectContains(componentText, "AddMethod(\"CreateBy\", &NLS::Engine::Components::Component::CreateBy, {})");
    ExpectContains(gameObjectText, "AllocateType(\"NLS::Engine::GameObject\")");
    ExpectContains(sceneText, "AllocateType(\"NLS::Engine::SceneSystem::Scene\")");
    ExpectContains(sceneText, "AddMethod(\"Play\", &NLS::Engine::SceneSystem::Scene::Play, {})");
    ExpectContains(metaText, "LinkReflectionTypes_NLS_Engine");
    ExpectContains(metaText, "#include \"GameObject.generated.cpp\"");
}
