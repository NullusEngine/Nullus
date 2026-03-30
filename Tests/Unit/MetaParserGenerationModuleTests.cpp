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

    ExpectContains(sampleText, "AllocateType(\"NLS::meta::MetaParserFieldMethodSample\")");
    ExpectContains(sampleText, "AddField<NLS::meta::MetaParserFieldMethodSample, int>(\"value\"");
    ExpectContains(sampleText, "&NLS::meta::MetaParserFieldMethodSample::GetValue");
    ExpectContains(sampleText, "&NLS::meta::MetaParserFieldMethodSample::SetValue");
    ExpectContains(sampleText, "AddMethod(\"GetValue\", &NLS::meta::MetaParserFieldMethodSample::GetValue, {})");
    ExpectNotContains(sampleText, "AddMethod(\"OnSerialize\"");
    ExpectContains(metaText, "LinkReflectionTypes_NLS_Base");
    ExpectContains(metaText, "#include \"Reflection/MetaParserFieldMethodSample.generated.cpp\"");
}
