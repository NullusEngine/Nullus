#include "ReflectionTestUtils.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace
{
using NLS::Tests::Reflection::ExpectContains;
using NLS::Tests::Reflection::ExpectNotContains;
using NLS::Tests::Reflection::ReadAllText;
}

TEST(MetaParserGenerationModuleTests, GeneratesModuleSpecificRegistrationEntrypoints)
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

TEST(MetaParserGenerationModuleTests, GeneratesExpectedBaseReflectionBindings)
{
    const std::filesystem::path sampleSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Base/Gen/Reflection/MetaParserFieldMethodSample.generated.cpp";
    const std::filesystem::path metaSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Base/Gen/MetaGenerated.cpp";
    const std::string sampleText = ReadAllText(sampleSource);
    const std::string metaText = ReadAllText(metaSource);

    ExpectContains(sampleText, "AllocateType(typeKey, \"NLS::meta::MetaParserFieldMethodSample\", moduleKey)");
    ExpectContains(sampleText, "AddField<NLS::meta::MetaParserFieldMethodSample, int>(\"value\"");
    ExpectContains(sampleText, "&NLS::meta::MetaParserFieldMethodSample::GetValue");
    ExpectContains(sampleText, "&NLS::meta::MetaParserFieldMethodSample::SetValue");
    ExpectContains(sampleText, "AddMethod(\"GetValue\", static_cast<int (NLS::meta::MetaParserFieldMethodSample::*)() const>(&NLS::meta::MetaParserFieldMethodSample::GetValue), {})");
    ExpectNotContains(sampleText, "AddMethod(\"OnSerialize\"");
    ExpectContains(metaText, "LinkReflectionTypes_NLS_Base");
    ExpectContains(metaText, "#include \"Reflection/MetaParserFieldMethodSample.generated.cpp\"");
}

TEST(MetaParserGenerationModuleTests, GeneratesComponentMenuTypeMetadataBindings)
{
    const std::filesystem::path meshRendererSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Gen/Components/MeshRenderer.generated.cpp";
    const std::string meshRendererText = ReadAllText(meshRendererSource);

    ExpectContains(meshRendererText, "ComponentMenu");
    ExpectContains(meshRendererText, "Rendering/Mesh Renderer");
    ExpectContains(meshRendererText, "type.meta");
}

TEST(MetaParserGenerationModuleTests, MapsDenseRuntimeMetaPropertyGeneratedBodiesToTheirOwnTypes)
{
    const std::filesystem::path generatedHeader =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Base/Gen/Reflection/RuntimeMetaProperties.generated.h";
    const std::string headerText = ReadAllText(generatedHeader);

    const std::vector<std::string> expectedTypes = {
        "NLS::meta::SerializationIntent",
        "NLS::meta::SerializeField",
        "NLS::meta::Transient",
        "NLS::meta::OwnedReference",
        "NLS::meta::ObjectReference",
        "NLS::meta::AssetReference",
        "NLS::meta::EditorOnly",
        "NLS::meta::RuntimeOnly",
        "NLS::meta::FormerlySerializedAs",
        "NLS::meta::StableTypeName",
        "NLS::meta::FormerlyTypeName",
        "NLS::meta::ComponentMenu",
    };

    std::size_t previousPosition = 0;
    for (const std::string& typeName : expectedTypes)
    {
        const std::string staticNameFragment = "StaticMetaTypeName() { return \"" + typeName + "\"; }";
        const std::size_t position = headerText.find(staticNameFragment);
        ASSERT_NE(position, std::string::npos) << "Missing generated body for " << typeName;
        EXPECT_GE(position, previousPosition) << typeName << " generated body is out of source order";
        previousPosition = position;
    }
}

TEST(MetaParserGenerationModuleTests, GeneratesExpectedEditorSettingsReflectionBindings)
{
    const std::filesystem::path sceneToolSource =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Gen/Project/Editor/Settings/EditorSettings.generated.cpp";
    const std::filesystem::path metaSource =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Gen/MetaGenerated.cpp";
    const std::filesystem::path handwrittenSource =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Settings/EditorSettings.cpp";

    const std::string sceneToolText = ReadAllText(sceneToolSource);
    const std::string metaText = ReadAllText(metaSource);
    const std::string handwrittenText = ReadAllText(handwrittenSource);

    ExpectContains(sceneToolText, "AllocateType(typeKey, \"NLS::Editor::Settings::EditorSceneToolSettingsObject\", moduleKey)");
    ExpectContains(sceneToolText, "AddField<NLS::Editor::Settings::EditorSceneToolSettingsObject, float>(\"translationSnapUnit\"");
    ExpectContains(sceneToolText, "AddField<NLS::Editor::Settings::EditorDebugDrawSettingsObject, bool>(\"debugDrawEnabled\"");
    ExpectContains(metaText, "#include \"Project/Editor/Settings/EditorSettings.generated.cpp\"");
    ExpectNotContains(handwrittenText, "AddField<EditorSceneToolSettingsObject");
    ExpectNotContains(handwrittenText, "AddField<EditorDebugDrawSettingsObject");
    ExpectNotContains(handwrittenText, "AutoReflectionModuleRegistrar");
}
