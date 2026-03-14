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

    ExpectContains(baseText, "RegisterReflectionTypes_NLS_Base");
    ExpectContains(baseText, "NLS_META_GENERATED_REGISTER_FUNCTION");
    ExpectContains(engineText, "RegisterReflectionTypes_NLS_Engine");
    ExpectContains(engineText, "NLS_META_GENERATED_REGISTER_FUNCTION");
}

TEST(MetaParserGenerationTests, GeneratesExpectedBaseReflectionBindings)
{
    const std::filesystem::path baseSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Base/Gen/MetaGenerated.cpp";
    const std::string text = ReadAllText(baseSource);

    ExpectContains(text, "AllocateType(\"NLS::meta::MetaParserFieldMethodSample\")");
    ExpectContains(text, "AllocateType(\"NLS::meta::MetaProperty\")");
    ExpectContains(text, "AddField<NLS::meta::MetaParserFieldMethodSample, int>(\"Value\"");
    ExpectContains(text, "RegisterReflectionTypes_NLS_Base");
}

TEST(MetaParserGenerationTests, GeneratesExpectedEngineReflectionBindings)
{
    const std::filesystem::path engineSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Gen/MetaGenerated.cpp";
    const std::string text = ReadAllText(engineSource);

    ExpectContains(text, "AllocateType(\"NLS::Engine::Components::Component\")");
    ExpectContains(text, "AllocateType(\"NLS::Engine::GameObject\")");
    ExpectContains(text, "AllocateType(\"NLS::Engine::SceneSystem::Scene\")");
    ExpectContains(text, "AddMethod(\"CreateBy\", &NLS::Engine::Components::Component::CreateBy, {})");
    ExpectContains(text, "AddMethod(\"Play\", &NLS::Engine::SceneSystem::Scene::Play, {})");
    ExpectContains(text, "RegisterReflectionTypes_NLS_Engine");
}
