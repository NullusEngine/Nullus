#include "ReflectionTestUtils.h"
#include "Rendering/ShaderCompiler/ShaderCompiler.h"

#include <gtest/gtest.h>
#include <Json/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <fstream>
#include <stdexcept>
#include <vector>

#define NLS_UNREGISTERED_TEST(suite, name) static void suite##_##name##_Unregistered()
#if defined(NLS_REGISTER_LONG_RUNNING_META_PARSER_MODULE_TESTS)
#undef TEST
#define TEST(suite, name) NLS_UNREGISTERED_TEST(suite, name)
#define NLS_LONG_RUNNING_TEST(performanceSuite, name) GTEST_TEST(performanceSuite, name)
#else
#define NLS_LONG_RUNNING_TEST(performanceSuite, name) NLS_UNREGISTERED_TEST(performanceSuite, name)
#endif

namespace
{
using NLS::Tests::Reflection::ExpectContains;
using NLS::Tests::Reflection::ExpectNotContains;
using NLS::Tests::Reflection::ReadAllText;

std::size_t CountOccurrences(const std::string& text, const std::string& pattern)
{
    std::size_t count = 0u;
    std::size_t position = 0u;
    while ((position = text.find(pattern, position)) != std::string::npos)
    {
        ++count;
        position += pattern.size();
    }
    return count;
}

struct ResourceReflectionFile
{
    const char* className;
    const char* qualifiedName;
    const char* headerPath;
    const char* generatedSourcePath;
};

constexpr std::array<ResourceReflectionFile, 6> kRenderingResourceReflectionFiles = {{
    {"Mesh", "NLS::Render::Resources::Mesh", "Runtime/Rendering/Resources/Mesh.h", "Runtime/Rendering/Gen/Resources/Mesh.generated.cpp"},
    {"Material", "NLS::Render::Resources::Material", "Runtime/Rendering/Resources/Material.h", "Runtime/Rendering/Gen/Resources/Material.generated.cpp"},
    {"Shader", "NLS::Render::Resources::Shader", "Runtime/Rendering/Resources/Shader.h", "Runtime/Rendering/Gen/Resources/Shader.generated.cpp"},
    {"Texture", "NLS::Render::Resources::Texture", "Runtime/Rendering/Resources/Texture.h", "Runtime/Rendering/Gen/Resources/Texture.generated.cpp"},
    {"Texture2D", "NLS::Render::Resources::Texture2D", "Runtime/Rendering/Resources/Texture2D.h", "Runtime/Rendering/Gen/Resources/Texture2D.generated.cpp"},
    {"TextureCube", "NLS::Render::Resources::TextureCube", "Runtime/Rendering/Resources/TextureCube.h", "Runtime/Rendering/Gen/Resources/TextureCube.generated.cpp"},
}};

void ResetTempDirectory(const std::filesystem::path& tempRoot)
{
    std::error_code error;
    std::filesystem::remove_all(tempRoot, error);
    ASSERT_FALSE(error) << error.message();
}

void WriteTextFile(const std::filesystem::path& path, const std::string& contents)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
}

std::filesystem::path ResolveMetaParserExecutable()
{
    const auto buildDir = std::filesystem::path(NLS_BUILD_DIR);
    const auto publishRoot = buildDir / "Tools" / "MetaParser" / "publish";
    const std::array<const char*, 4> configurations = {{
        "Debug",
        "RelWithDebInfo",
        "Release",
        "MinSizeRel"
    }};

    for (const auto* configuration : configurations)
    {
#if defined(_WIN32)
        const auto candidate = publishRoot / configuration / "MetaParser.exe";
#else
        const auto candidate = publishRoot / configuration / "MetaParser";
#endif
        if (std::filesystem::exists(candidate))
            return candidate;
    }

#if defined(_WIN32)
    const auto recursiveName = std::string("MetaParser.exe");
#else
    const auto recursiveName = std::string("MetaParser");
#endif
    if (std::filesystem::exists(publishRoot))
    {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(publishRoot))
        {
            if (entry.is_regular_file() && entry.path().filename() == recursiveName)
                return entry.path();
        }
    }

    return {};
}

bool RunMetaParser(
    const std::filesystem::path& paramsPath,
    const std::filesystem::path& outputPath)
{
    const auto metaParserExe = ResolveMetaParserExecutable();
    if (metaParserExe.empty())
        return false;

    NLS::Render::ShaderCompiler::ShaderProcessOptions options;
    options.timeoutMilliseconds = 180000u;

    // A loaded CI runner can starve the parser process long enough to hit the
    // wall-clock timeout even though the fixture is tiny and every neighbouring
    // case finishes in well under a second. Retry those launch-level failures
    // once; a real grammar or fixture error still reports Failed with diagnostics
    // and fails on the first attempt.
    NLS::Render::ShaderCompiler::ShaderProcessResult result;
    for (uint32_t attempt = 0u; attempt < 2u; ++attempt)
    {
        result = NLS::Render::ShaderCompiler::ExecuteShaderCompilerProcess(
            metaParserExe.string(),
            {paramsPath.string()},
            options);
        if (result.status == NLS::Render::ShaderCompiler::ShaderProcessStatus::Succeeded ||
            (result.status != NLS::Render::ShaderCompiler::ShaderProcessStatus::TimedOut &&
                result.status != NLS::Render::ShaderCompiler::ShaderProcessStatus::FailedToStart))
            break;
    }

    std::filesystem::create_directories(outputPath.parent_path());
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    output << result.output;
    if (!result.diagnostics.empty())
        output << "\n" << result.diagnostics;

    return result.status == NLS::Render::ShaderCompiler::ShaderProcessStatus::Succeeded;
}

std::string ReadOptionalText(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path))
        return {};
    return ReadAllText(path);
}

void ExpectMetaParserSuccess(
    const std::filesystem::path& paramsPath,
    const std::filesystem::path& outputPath)
{
    ASSERT_FALSE(ResolveMetaParserExecutable().empty());
    EXPECT_TRUE(RunMetaParser(paramsPath, outputPath)) << ReadOptionalText(outputPath);
}

void ExpectMetaParserFailure(
    const std::filesystem::path& paramsPath,
    const std::filesystem::path& outputPath)
{
    ASSERT_FALSE(ResolveMetaParserExecutable().empty());
    EXPECT_FALSE(RunMetaParser(paramsPath, outputPath)) << ReadOptionalText(outputPath);
}

std::filesystem::path ResolveBaseMetaParserConfigPath()
{
    return std::filesystem::path(NLS_BUILD_DIR) / "Runtime" / "Base" / "NLS_Base.precompile.json";
}

nlohmann::json LoadBaseMetaParserConfig()
{
    const auto configPath = ResolveBaseMetaParserConfigPath();
    if (!std::filesystem::exists(configPath))
        throw std::runtime_error(
            "Missing MetaParser base precompile config: " + configPath.string() +
            ". Build the NLS_Base target before running MetaParser fixture tests.");

    auto config = nlohmann::json::parse(ReadAllText(configPath), nullptr, false);
    if (config.is_discarded())
        throw std::runtime_error("Failed to parse " + configPath.string());
    return config;
}

void AddUniqueString(nlohmann::json& values, const std::string& value)
{
    if (value.empty())
        return;

    for (const auto& existing : values)
    {
        if (existing.is_string() && existing.get<std::string>() == value)
            return;
    }

    values.push_back(value);
}

nlohmann::json MakeBasicMetaParserConfig(
    const std::filesystem::path& tempRoot,
    const std::filesystem::path& runtimeDir,
    const std::filesystem::path& outputDir,
    const std::filesystem::path& headerPath,
    const std::vector<std::filesystem::path>& includeDirs)
{
    auto config = LoadBaseMetaParserConfig();
    config["RootDir"] = tempRoot.generic_string();
    config["SourceDir"] = runtimeDir.generic_string();
    config["TargetName"] = "Fixture";
    config["ModuleName"] = "Fixture";
    config["OutputDir"] = outputDir.generic_string();
    config["Headers"] = nlohmann::json::array({headerPath.generic_string()});

    auto mergedIncludeDirs = nlohmann::json::array();
    for (const auto& includeDir : includeDirs)
        AddUniqueString(mergedIncludeDirs, includeDir.generic_string());
    for (const auto& includeDir : config.value("IncludeDirs", nlohmann::json::array()))
    {
        if (includeDir.is_string())
            AddUniqueString(mergedIncludeDirs, includeDir.get<std::string>());
    }
    config["IncludeDirs"] = std::move(mergedIncludeDirs);

    auto defines = config.value("Defines", nlohmann::json::array());
    AddUniqueString(defines, "__REFLECTION_PARSER__");
    config["Defines"] = std::move(defines);

    return config;
}

void WriteMetaParserConfig(
    const std::filesystem::path& paramsPath,
    const std::filesystem::path& tempRoot,
    const std::filesystem::path& runtimeDir,
    const std::filesystem::path& outputDir,
    const std::filesystem::path& headerPath,
    const std::vector<std::filesystem::path>& includeDirs)
{
    std::ofstream config(paramsPath, std::ios::binary | std::ios::trunc);
    config << MakeBasicMetaParserConfig(tempRoot, runtimeDir, outputDir, headerPath, includeDirs).dump(2) << "\n";
}
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, FixturePrecompileConfigInheritsBuildCompilerEnvironment)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto tempRoot = std::filesystem::path(NLS_BUILD_DIR) / "MetaParserConfigInheritanceFixture";
    const auto runtimeDir = tempRoot / "Runtime" / "Fixture";
    const auto outputDir = tempRoot / "Gen";
    const auto headerPath = runtimeDir / "Fixture.h";

    const auto baseConfig = LoadBaseMetaParserConfig();
    const auto fixtureConfig = MakeBasicMetaParserConfig(
        tempRoot,
        runtimeDir,
        outputDir,
        headerPath,
        {
            runtimeDir,
            root / "Runtime" / "Base"
        });

    EXPECT_EQ(fixtureConfig["CompilerPath"], baseConfig["CompilerPath"]);
    EXPECT_EQ(fixtureConfig["CompilerId"], baseConfig["CompilerId"]);
    EXPECT_EQ(fixtureConfig["CompilerTarget"], baseConfig["CompilerTarget"]);
    EXPECT_EQ(fixtureConfig["ResourceDir"], baseConfig["ResourceDir"]);
    EXPECT_EQ(fixtureConfig["Sysroot"], baseConfig["Sysroot"]);
    EXPECT_EQ(fixtureConfig["SystemIncludeDirs"], baseConfig["SystemIncludeDirs"]);
    EXPECT_NE(
        std::find(
            fixtureConfig["IncludeDirs"].begin(),
            fixtureConfig["IncludeDirs"].end(),
            runtimeDir.generic_string()),
        fixtureConfig["IncludeDirs"].end());
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, GeneratesModuleSpecificRegistrationEntrypoints)
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

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, GeneratesEditorExtensionDescriptorsFromHeaderMetadata)
{
    const std::filesystem::path editorHeader =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Gen/EditorProject_MetaGenerated.h";
    const std::filesystem::path editorSource =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Gen/MetaGenerated.cpp";
    const std::filesystem::path descriptorSource =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Gen/EditorExtensionDescriptors.generated.cpp";

    const std::string headerText = ReadAllText(editorHeader);
    const std::string sourceText = ReadAllText(editorSource);
    const std::string descriptorText = ReadAllText(descriptorSource);

    ExpectContains(headerText, "RegisterEditorExtensions_EditorProject");
    ExpectContains(headerText, "NLS_META_GENERATED_REGISTER_EDITOR_EXTENSIONS");
    ExpectContains(sourceText, "EditorExtensionDescriptors.generated.cpp");
    ExpectContains(descriptorText, "NLS::Engine::Components::UICanvasComponent");
    ExpectContains(descriptorText, "NLS::Editor::Inspector::UICanvasComponentEditor");
    ExpectContains(descriptorText, "RegisterEditorExtensionDescriptors");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, GeneratesExpectedBaseReflectionBindings)
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
    ExpectContains(sampleText, "NLS::meta::DisplayName");
    ExpectContains(sampleText, "NLS::meta::Tooltip");
    ExpectContains(sampleText, "NLS::meta::Header");
    ExpectContains(sampleText, "NLS::meta::Space");
    ExpectContains(sampleText, "NLS::meta::Range");
    ExpectContains(sampleText, "NLS::meta::Min");
    ExpectContains(sampleText, "NLS::meta::Max");
    ExpectContains(sampleText, "NLS::meta::Multiline");
    ExpectContains(sampleText, "NLS::meta::ReadOnly");
    ExpectContains(sampleText, "NLS::meta::HideInInspector");
    ExpectContains(sampleText, "NLS::meta::Flags");
    ExpectContains(sampleText, "NLS::meta::ColorUsage");
    ExpectContains(sampleText, "NLS::meta::ReferenceTypeConstraint");
    ExpectContains(sampleText, "MetaPropertyInitializer<NLS::meta::Space>(4.0f)");
    ExpectContains(sampleText, "MetaPropertyInitializer<NLS::meta::Min>(-5.0f)");
    ExpectContains(sampleText, "MetaPropertyInitializer<NLS::meta::Max>(5.0f)");
    ExpectContains(metaText, "LinkReflectionTypes_NLS_Base");
    ExpectContains(metaText, "#include \"Reflection/MetaParserFieldMethodSample.generated.cpp\"");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, ExternalReflectionTypeDiscoveryIgnoresCommentsAndDisabledBlocks)
{
    const std::filesystem::path coreSource =
        std::filesystem::path(NLS_ROOT_DIR) / "Tools/MetaParser/src/MetaParserTool.Core.cs";
    const std::filesystem::path validationSource =
        std::filesystem::path(NLS_ROOT_DIR) / "Tools/MetaParser/src/MetaParserTool.Validation.cs";
    const std::string coreText = ReadAllText(coreSource);
    const std::string validationText = ReadAllText(validationSource);

    ExpectContains(coreText, "StripCommentsAndDisabledPreprocessorBlocks");
    ExpectContains(coreText, "StripCommentsAndDisabledPreprocessorBlocks(text)");
    ExpectContains(coreText, "StripDisabledIfZeroBlocks");
    ExpectContains(validationText, "ExtractExternalReflectionTypeNames(headerText)");
    ExpectNotContains(validationText, "Regex.Matches(\r\n                         headerText");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, HeaderSourceTemplateUsesGeneratedTypeTemplateModelShape)
{
    const std::filesystem::path modelSource =
        std::filesystem::path(NLS_ROOT_DIR) / "Tools/MetaParser/src/Generation/GenerationModels.cs";
    const std::filesystem::path sourceTemplate =
        std::filesystem::path(NLS_ROOT_DIR) / "Tools/MetaParser/src/Templates/HeaderGenerated.cpp.tt";

    const std::string modelText = ReadAllText(modelSource);
    const std::string templateText = ReadAllText(sourceTemplate);

    ExpectContains(modelText, "bool HasGeneratedBody");
    ExpectContains(templateText, "type.HasGeneratedBody");
    ExpectNotContains(templateText, "type.GeneratedBodyLine");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, CMakeTracksNestedMetaParserSourcesForIncrementalGeneration)
{
    const std::filesystem::path toolsCMakePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Tools/MetaParser/CMakeLists.txt";
    const std::filesystem::path runtimeCMakePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/CMakeLists.txt";
    const std::string toolsCMake = ReadAllText(toolsCMakePath);
    const std::string runtimeCMake = ReadAllText(runtimeCMakePath);

    ExpectContains(toolsCMake, "file(GLOB_RECURSE NLS_META_PARSER_SOURCE_FILES");
    ExpectContains(toolsCMake, "\"${NLS_META_PARSER_SOURCE_DIR}/*.cs\"");
    ExpectContains(toolsCMake, "list(FILTER NLS_META_PARSER_SOURCE_FILES EXCLUDE REGEX [[/bin/]])");
    ExpectContains(toolsCMake, "list(FILTER NLS_META_PARSER_SOURCE_FILES EXCLUDE REGEX [[/obj/]])");

    ExpectContains(runtimeCMake, "file(GLOB_RECURSE _meta_parser_tool_sources CONFIGURE_DEPENDS");
    ExpectContains(runtimeCMake, "\"${NLS_ROOT_DIR}/Tools/MetaParser/src/*.cs\"");
    ExpectContains(runtimeCMake, "list(FILTER _meta_parser_tool_sources EXCLUDE REGEX [[/bin/]])");
    ExpectContains(runtimeCMake, "list(FILTER _meta_parser_tool_sources EXCLUDE REGEX [[/obj/]])");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, RenderingResourceReflectionOwnedClassesUseGeneratedBodies)
{
    for (const auto& resource : kRenderingResourceReflectionFiles)
    {
        const std::filesystem::path headerPath = std::filesystem::path(NLS_ROOT_DIR) / resource.headerPath;
        const std::string headerText = ReadAllText(headerPath);

        ExpectContains(headerText, std::string("CLASS(NLS_RENDER_API ") + resource.className);
        ExpectContains(headerText, "GENERATED_BODY()");
        ExpectContains(headerText, std::string("Resources/") + resource.className + ".generated.h");
        ExpectNotContains(headerText, "StaticMetaTypeName");
        ExpectNotContains(headerText, "GetObjectTypeName");
    }

    const std::filesystem::path externalReflectionPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/ExternalReflection.h";
    const std::string externalReflectionText = ReadAllText(externalReflectionPath);

    ExpectContains(externalReflectionText, "NLS_META_EXTERNAL_TYPE_NAME(NLS::Render::Geometry::Bounds)");
    ExpectContains(externalReflectionText, "NLS_META_EXTERNAL_BEGIN(NLS::Render::Geometry::Bounds)");

    for (const auto& resource : kRenderingResourceReflectionFiles)
    {
        ExpectNotContains(
            externalReflectionText,
            std::string("NLS_META_EXTERNAL_TYPE_NAME(") + resource.qualifiedName + ")");
        ExpectNotContains(
            externalReflectionText,
            std::string("RegisterResourceReferenceType<") + resource.qualifiedName + ">");
    }
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, RenderingResourceReflectionGeneratesOwnedRegistrations)
{
    for (const auto& resource : kRenderingResourceReflectionFiles)
    {
        const std::filesystem::path generatedSourcePath =
            std::filesystem::path(NLS_ROOT_DIR) / resource.generatedSourcePath;
        const std::string generatedSourceText = ReadAllText(generatedSourcePath);

        ExpectContains(
            generatedSourceText,
            std::string("AllocateType(typeKey, \"") + resource.qualifiedName + "\", moduleKey)");
        ExpectContains(
            generatedSourceText,
            std::string("TypeInfo<") + resource.qualifiedName + ">::Register(id, type, true)");
        ExpectContains(
            generatedSourceText,
            std::string("TypeInfo<") + resource.qualifiedName + "*>::Register(pointerId");
        ExpectContains(
            generatedSourceText,
            std::string("TypeInfo<const ") + resource.qualifiedName + "*>::Register(constPointerId");
        ExpectNotContains(generatedSourceText, "RegisterResourceReferenceType");
    }
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, RenderingResourceReflectionSkipsNonReflectedRuntimeInterfacesAsBases)
{
    const std::filesystem::path generatedSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Gen/Resources/Mesh.generated.cpp";
    const std::string generatedSourceText = ReadAllText(generatedSourcePath);

    ExpectContains(
        generatedSourceText,
        "db.ResolveRegisteredType(moduleKey, \"NLS::NamedObject\", reportMissingType)");
    ExpectNotContains(
        generatedSourceText,
        "db.ResolveRegisteredType(moduleKey, \"NLS::Render::Resources::IMesh\", reportMissingType)");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, MetaParserCreatesHeaderStubsAndFiltersRuntimeInterfaces)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto tempRoot = std::filesystem::path(NLS_BUILD_DIR) / "MetaParserGeneratedHeaderStubFixture";
    const auto runtimeDir = tempRoot / "Runtime" / "Fixture";
    const auto outputDir = runtimeDir / "Gen";
    ResetTempDirectory(tempRoot);
    std::filesystem::create_directories(runtimeDir);
    std::filesystem::create_directories(outputDir);

    const auto baseHeaderPath = runtimeDir / "ReflectedBase.h";
    {
        std::ofstream header(baseHeaderPath);
        header
            << "#pragma once\n"
            << "#include \"Object/Object.h\"\n"
            << "#include \"Reflection/Macros.h\"\n"
            << "#include \"ReflectedBase.generated.h\"\n"
            << "namespace NLS::Fixture {\n"
            << "CLASS(ReflectedBase) : public NLS::Object\n"
            << "{\n"
            << "    GENERATED_BODY()\n"
            << "};\n"
            << "} // namespace NLS::Fixture\n";
    }

    const auto derivedHeaderPath = runtimeDir / "DerivedWithInterface.h";
    {
        std::ofstream header(derivedHeaderPath);
        header
            << "#pragma once\n"
            << "#include \"ReflectedBase.h\"\n"
            << "#include \"Reflection/Macros.h\"\n"
            << "#include \"DerivedWithInterface.generated.h\"\n"
            << "namespace NLS::Fixture {\n"
            << "class RuntimeOnlyInterface\n"
            << "{\n"
            << "public:\n"
            << "    virtual ~RuntimeOnlyInterface() = default;\n"
            << "};\n"
            << "CLASS(DerivedWithInterface) : public ReflectedBase, public RuntimeOnlyInterface\n"
            << "{\n"
            << "    GENERATED_BODY()\n"
            << "};\n"
            << "} // namespace NLS::Fixture\n";
    }

    const auto paramsPath = tempRoot / "DerivedWithInterface.precompile.json";
    WriteMetaParserConfig(
        paramsPath,
        tempRoot,
        runtimeDir,
        outputDir,
        derivedHeaderPath,
        {
            runtimeDir,
            outputDir,
            root / "Runtime" / "Base"
        });

    const auto outputPath = tempRoot / "metaparser.out.txt";
    ExpectMetaParserSuccess(paramsPath, outputPath);

    const auto baseGeneratedHeader = outputDir / "ReflectedBase.generated.h";
    const auto derivedGeneratedHeader = outputDir / "DerivedWithInterface.generated.h";
    const auto derivedGeneratedSource = outputDir / "DerivedWithInterface.generated.cpp";
    ASSERT_TRUE(std::filesystem::exists(baseGeneratedHeader));
    ASSERT_TRUE(std::filesystem::exists(derivedGeneratedHeader));
    ASSERT_TRUE(std::filesystem::exists(derivedGeneratedSource));

    const std::string headerText = ReadAllText(derivedGeneratedHeader);
    const std::string sourceText = ReadAllText(derivedGeneratedSource);
    ExpectContains(
        headerText,
        "GetObjectTypeName(void) const override { return \"NLS::Fixture::DerivedWithInterface\"; }");
    ExpectContains(
        sourceText,
        "db.ResolveRegisteredType(moduleKey, \"NLS::Fixture::ReflectedBase\", reportMissingType)");
    ExpectNotContains(
        sourceText,
        "db.ResolveRegisteredType(moduleKey, \"NLS::Fixture::RuntimeOnlyInterface\", reportMissingType)");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, HeaderSourceTemplateAvoidsDuplicatingRuntimeMetaPropertiesInclude)
{
    const std::filesystem::path generatedSource =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Base/Gen/Reflection/RuntimeMetaProperties.generated.cpp";

    const std::string generatedText = ReadAllText(generatedSource);
    EXPECT_EQ(CountOccurrences(generatedText, "#include \"Reflection/RuntimeMetaProperties.h\""), 1u);
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, ExternalReflectionTypeDiscoveryHandlesCommentsStringsAndElifBranches)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto tempRoot = std::filesystem::path(NLS_BUILD_DIR) / "MetaParserExternalDiscoveryFixture";
    const auto runtimeDir = tempRoot / "Runtime" / "Fixture";
    const auto outputDir = tempRoot / "Gen";
    ResetTempDirectory(tempRoot);
    std::filesystem::create_directories(runtimeDir);
    std::filesystem::create_directories(outputDir);

    const auto headerPath = runtimeDir / "FixtureExternalReflection.h";
    {
        std::ofstream header(headerPath);
        header
            << "#pragma once\n"
            << "#include \"Reflection/Macros.h\"\n"
            << "#define NLS_META_EXTERNAL_TYPE_NAME(type)\n"
            << "namespace NLS::Fixture {\n"
            << "struct ActiveExternal {};\n"
            << "struct ElifExternal {};\n"
            << "STRUCT(UsesExternal)\n"
            << "{\n"
            << "    GENERATED_BODY()\n"
            << "    PROPERTY()\n"
            << "    NLS::Fixture::ElifExternal value;\n"
            << "};\n"
            << "inline constexpr const char* Text = R\"(\n"
            << "#if 0\n"
            << "NLS_META_EXTERNAL_TYPE_NAME(NLS::Fixture::StringOnlyExternal)\n"
            << "#endif\n"
            << ")\";\n"
            << "/*\n"
            << "#if 0\n"
            << "NLS_META_EXTERNAL_TYPE_NAME(NLS::Fixture::CommentOnlyExternal)\n"
            << "#endif\n"
            << "*/\n"
            << "#if 0\n"
            << "NLS_META_EXTERNAL_TYPE_NAME(NLS::Fixture::DisabledExternal)\n"
            << "#elif 1\n"
            << "NLS_META_EXTERNAL_TYPE_NAME(NLS::Fixture::ElifExternal)\n"
            << "#endif\n"
            << "NLS_META_EXTERNAL_TYPE_NAME(NLS::Fixture::ActiveExternal)\n"
            << "} // namespace NLS::Fixture\n";
    }

    const auto paramsPath = tempRoot / "Fixture.precompile.json";
    WriteMetaParserConfig(
        paramsPath,
        tempRoot,
        runtimeDir,
        outputDir,
        headerPath,
        {
            root / "Runtime" / "Base"
        });

    const auto outputPath = tempRoot / "metaparser.out.txt";
    ExpectMetaParserSuccess(paramsPath, outputPath);

    const std::string processText = ReadAllText(outputPath);
    ExpectContains(processText, "Target=Fixture, Types=1");
    ExpectNotContains(processText, "StringOnlyExternal");
    ExpectNotContains(processText, "CommentOnlyExternal");
    ExpectNotContains(processText, "DisabledExternal");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, RejectsPPtrFieldsWhoseTargetsAreNotSupportedEngineObjects)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto tempRoot = std::filesystem::path(NLS_BUILD_DIR) / "MetaParserInvalidPPtrFixture";
    const auto runtimeDir = tempRoot / "Runtime" / "Fixture";
    const auto outputDir = tempRoot / "Gen";
    ResetTempDirectory(tempRoot);
    std::filesystem::create_directories(runtimeDir);
    std::filesystem::create_directories(outputDir);

    const auto headerPath = runtimeDir / "InvalidPPtrFixture.h";
    {
        std::ofstream header(headerPath);
        header
            << "#pragma once\n"
            << "#include \"Reflection/Macros.h\"\n"
            << "#include \"Object/Object.h\"\n"
            << "#include \"Serialize/PPtr.h\"\n"
            << "namespace NLS::Fixture {\n"
            << "class UnsupportedObject final : public NLS::Object\n"
            << "{\n"
            << "};\n"
            << "STRUCT(InvalidPPtrOwner)\n"
            << "{\n"
            << "    GENERATED_BODY()\n"
            << "    PROPERTY()\n"
            << "    NLS::Engine::Serialize::PPtr<NLS::Fixture::UnsupportedObject> target;\n"
            << "};\n"
            << "} // namespace NLS::Fixture\n";
    }

    const auto paramsPath = tempRoot / "InvalidPPtrFixture.precompile.json";
    WriteMetaParserConfig(
        paramsPath,
        tempRoot,
        runtimeDir,
        outputDir,
        headerPath,
        {
            root / "Runtime",
            root / "Runtime" / "Base",
            root / "Runtime" / "Engine"
        });

    const auto outputPath = tempRoot / "metaparser.out.txt";
    ExpectMetaParserFailure(paramsPath, outputPath);

    const std::string processText = ReadAllText(outputPath);
    ExpectContains(processText, "registered PPtr target set");
    ExpectContains(processText, "NLS::Fixture::UnsupportedObject");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, PPtrTargetMacroParsingFailsLoudlyAndAcceptsWrappedEntries)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto tempRoot = std::filesystem::path(NLS_BUILD_DIR) / "MetaParserWrappedPPtrTargetsFixture";
    const auto runtimeDir = tempRoot / "Runtime" / "Fixture";
    const auto serializeDir = tempRoot / "Runtime" / "Engine" / "Serialize";
    const auto outputDir = tempRoot / "Gen";
    ResetTempDirectory(tempRoot);
    std::filesystem::create_directories(runtimeDir);
    std::filesystem::create_directories(serializeDir);
    std::filesystem::create_directories(outputDir);

    const auto pptrTargetsPath = serializeDir / "PPtrResourceTypes.h";
    WriteTextFile(
        pptrTargetsPath,
        "#pragma once\n"
        "#define NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(APPLY) \\\n"
        "    APPLY( \\\n"
        "        NLS::Fixture::WrappedTarget, \\\n"
        "        \"WrappedTarget\", \\\n"
        "        NLS::Core::Assets::ArtifactType::Material, \\\n"
        "        \"material\")\n");

    const auto headerPath = runtimeDir / "WrappedPPtrFixture.h";
    {
        std::ofstream header(headerPath);
        header
            << "#pragma once\n"
            << "#include \"Reflection/Macros.h\"\n"
            << "#include \"Object/Object.h\"\n"
            << "#include \"Serialize/PPtr.h\"\n"
            << "namespace NLS::Fixture {\n"
            << "CLASS(WrappedTarget) : public NLS::Object\n"
            << "{\n"
            << "    GENERATED_BODY()\n"
            << "};\n"
            << "STRUCT(WrappedPPtrOwner)\n"
            << "{\n"
            << "    GENERATED_BODY()\n"
            << "    PROPERTY()\n"
            << "    NLS::Engine::Serialize::PPtr<NLS::Fixture::WrappedTarget> target;\n"
            << "};\n"
            << "} // namespace NLS::Fixture\n";
    }

    const auto paramsPath = tempRoot / "WrappedPPtrFixture.precompile.json";
    WriteMetaParserConfig(
        paramsPath,
        tempRoot,
        runtimeDir,
        outputDir,
        headerPath,
        {
            root / "Runtime",
            root / "Runtime" / "Base",
            root / "Runtime" / "Engine"
        });

    const auto outputPath = tempRoot / "metaparser.out.txt";
    ExpectMetaParserSuccess(paramsPath, outputPath);

    WriteTextFile(
        pptrTargetsPath,
        "#pragma once\n"
        "#define NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(APPLY) \\\n"
        "    NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGET(APPLY)\n");
    ResetTempDirectory(outputDir);
    std::filesystem::create_directories(outputDir);

    ExpectMetaParserFailure(paramsPath, outputPath);
    const std::string processText = ReadAllText(outputPath);
    ExpectContains(processText, "NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, GeneratesStdVectorAndArrayReflectedValueFields)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto tempRoot = std::filesystem::path(NLS_BUILD_DIR) / "MetaParserVectorArrayFixture";
    const auto runtimeDir = tempRoot / "Runtime" / "Fixture";
    const auto outputDir = tempRoot / "Gen";
    ResetTempDirectory(tempRoot);
    std::filesystem::create_directories(runtimeDir);
    std::filesystem::create_directories(outputDir);

    const auto headerPath = runtimeDir / "VectorArrayFixture.h";
    {
        std::ofstream header(headerPath);
        header
            << "#pragma once\n"
            << "#include <vector>\n"
            << "#include \"Reflection/Array.h\"\n"
            << "#include \"Reflection/Macros.h\"\n"
            << "using std::vector;\n"
            << "namespace NLS::Fixture {\n"
            << "STRUCT(VectorArrayElement)\n"
            << "{\n"
            << "    GENERATED_BODY()\n"
            << "    PROPERTY()\n"
            << "    int count = 0;\n"
            << "};\n"
            << "STRUCT(VectorArrayOwner)\n"
            << "{\n"
            << "    GENERATED_BODY()\n"
            << "    PROPERTY()\n"
            << "    std::vector<NLS::Fixture::VectorArrayElement> vectorEntries;\n"
            << "    PROPERTY()\n"
            << "    vector<NLS::Fixture::VectorArrayElement> shorthandVectorEntries;\n"
            << "    PROPERTY()\n"
            << "    NLS::Array<NLS::Fixture::VectorArrayElement> arrayEntries;\n"
            << "};\n"
            << "} // namespace NLS::Fixture\n";
    }

    const auto paramsPath = tempRoot / "VectorArrayFixture.precompile.json";
    WriteMetaParserConfig(
        paramsPath,
        tempRoot,
        runtimeDir,
        outputDir,
        headerPath,
        {
            root / "Runtime" / "Base"
        });

    const auto outputPath = tempRoot / "metaparser.out.txt";
    ExpectMetaParserSuccess(paramsPath, outputPath);

    const auto generatedSource = outputDir / "VectorArrayFixture.generated.cpp";
    const std::string sourceText = ReadAllText(generatedSource);

    ExpectContains(sourceText, "AddField<NLS::Fixture::VectorArrayOwner, std::vector<NLS::Fixture::VectorArrayElement>>(\"vectorEntries\"");
    ExpectContains(sourceText, "AddField<NLS::Fixture::VectorArrayOwner, std::vector<NLS::Fixture::VectorArrayElement>>(\"shorthandVectorEntries\"");
    ExpectContains(sourceText, "AddField<NLS::Fixture::VectorArrayOwner, NLS::Array<NLS::Fixture::VectorArrayElement>>(\"arrayEntries\"");
    ExpectContains(sourceText, "db.ResolveRegisteredArrayFieldType(moduleKey, \"NLS::Fixture::VectorArrayElement\", reportMissingType)");
    ExpectNotContains(sourceText, "db.ResolveRegisteredFieldType(moduleKey, \"std::vector<NLS::Fixture::VectorArrayElement>\", reportMissingType)");
    ExpectNotContains(sourceText, "db.ResolveRegisteredFieldType(moduleKey, \"NLS::Array<NLS::Fixture::VectorArrayElement>\", reportMissingType)");
    ExpectNotContains(sourceText, "resolveRegisteredFieldType");
    ExpectNotContains(sourceText, "resolveRegisteredType");
    ExpectNotContains(sourceText, "arrayPrefix");
    ExpectNotContains(sourceText, "vectorPrefix");

    const std::filesystem::path sourceTemplate =
        std::filesystem::path(NLS_ROOT_DIR) / "Tools/MetaParser/src/Templates/HeaderGenerated.cpp.tt";
    const std::string templateText = ReadAllText(sourceTemplate);
    ExpectNotContains(templateText, "pptrPrefix");
    ExpectNotContains(templateText, "arrayPPtrPrefix");
    ExpectNotContains(templateText, "vectorPPtrPrefix");
    ExpectContains(templateText, "PPtrFieldTypeRegistrations");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, GeneratesRangeMetadataAndDefaultPrivateFieldAccessors)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto tempRoot = std::filesystem::path(NLS_BUILD_DIR) / "MetaParserRangePrivateFixture";
    const auto runtimeDir = tempRoot / "Runtime" / "Fixture";
    const auto outputDir = tempRoot / "Gen";
    ResetTempDirectory(tempRoot);
    std::filesystem::create_directories(runtimeDir);
    std::filesystem::create_directories(outputDir);

    const auto headerPath = runtimeDir / "RangePrivateFixture.h";
    {
        std::ofstream header(headerPath);
        header
            << "#pragma once\n"
            << "#include \"Reflection/Macros.h\"\n"
            << "namespace NLS::Fixture {\n"
            << "CLASS(RangePrivateFixture)\n"
            << "{\n"
            << "    GENERATED_BODY()\n"
            << "    PROPERTY(Range(-2.5f, 3.25))\n"
            << "    float defaultPrivateValue = 0.0f;\n"
            << "};\n"
            << "} // namespace NLS::Fixture\n";
    }

    const auto paramsPath = tempRoot / "RangePrivateFixture.precompile.json";
    WriteMetaParserConfig(
        paramsPath,
        tempRoot,
        runtimeDir,
        outputDir,
        headerPath,
        {
            root / "Runtime" / "Base"
        });

    const auto outputPath = tempRoot / "metaparser.out.txt";
    ExpectMetaParserSuccess(paramsPath, outputPath);

    const auto generatedSource = outputDir / "RangePrivateFixture.generated.cpp";
    const auto generatedHeader = outputDir / "RangePrivateFixture.generated.h";
    const std::string sourceText = ReadAllText(generatedSource);
    const std::string headerText = ReadAllText(generatedHeader);

    ExpectContains(sourceText, "MetaPropertyInitializer<NLS::meta::Range>(-2.5f, 3.25f)");
    ExpectContains(sourceText, "PrivateAccess_NLS__Fixture__RangePrivateFixture::Field_defaultPrivateValue_0()");
    ExpectNotContains(sourceText, "&NLS::Fixture::RangePrivateFixture::defaultPrivateValue, &NLS::Fixture::RangePrivateFixture::defaultPrivateValue");
    ExpectContains(headerText, "friend struct ::NLS::meta_generated::PrivateAccess_NLS__Fixture__RangePrivateFixture;");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, RejectsPrivateAccessorPropertiesBeforeGeneratingInvalidPrivateFieldShim)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto tempRoot = std::filesystem::path(NLS_BUILD_DIR) / "MetaParserPrivateAccessorFixture";
    const auto runtimeDir = tempRoot / "Runtime" / "Fixture";
    const auto outputDir = tempRoot / "Gen";
    ResetTempDirectory(tempRoot);
    std::filesystem::create_directories(runtimeDir);
    std::filesystem::create_directories(outputDir);

    const auto headerPath = runtimeDir / "PrivateAccessorFixture.h";
    {
        std::ofstream header(headerPath);
        header
            << "#pragma once\n"
            << "#include \"Reflection/Macros.h\"\n"
            << "namespace NLS::Fixture {\n"
            << "CLASS(PrivateAccessorFixture)\n"
            << "{\n"
            << "    GENERATED_BODY()\n"
            << "private:\n"
            << "    PROPERTY(Getter(GetValue), Setter(SetValue))\n"
            << "    int value = 0;\n"
            << "    FUNCTION()\n"
            << "    int GetValue() const { return value; }\n"
            << "    FUNCTION()\n"
            << "    void SetValue(int p_value) { value = p_value; }\n"
            << "};\n"
            << "} // namespace NLS::Fixture\n";
    }

    const auto paramsPath = tempRoot / "PrivateAccessorFixture.precompile.json";
    WriteMetaParserConfig(
        paramsPath,
        tempRoot,
        runtimeDir,
        outputDir,
        headerPath,
        {
            root / "Runtime" / "Base"
        });

    const auto outputPath = tempRoot / "metaparser.out.txt";
    ExpectMetaParserFailure(paramsPath, outputPath);

    const std::string processText = ReadAllText(outputPath);
    ExpectContains(processText, "Private reflected accessor methods are not supported");
    ExpectContains(processText, "NLS::Fixture::PrivateAccessorFixture::GetValue");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, RejectsPropertyAnnotationsOnMethods)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto tempRoot = std::filesystem::path(NLS_BUILD_DIR) / "MetaParserMethodPropertyFixture";
    const auto runtimeDir = tempRoot / "Runtime" / "Fixture";
    const auto outputDir = tempRoot / "Gen";
    ResetTempDirectory(tempRoot);
    std::filesystem::create_directories(runtimeDir);
    std::filesystem::create_directories(outputDir);

    const auto headerPath = runtimeDir / "MethodPropertyFixture.h";
    {
        std::ofstream header(headerPath);
        header
            << "#pragma once\n"
            << "#include \"Reflection/Macros.h\"\n"
            << "namespace NLS::Fixture {\n"
            << "CLASS(MethodPropertyFixture)\n"
            << "{\n"
            << "    GENERATED_BODY()\n"
            << "public:\n"
            << "    PROPERTY(DisplayName(\"Value\"))\n"
            << "    FUNCTION()\n"
            << "    int GetValue() const { return 0; }\n"
            << "};\n"
            << "} // namespace NLS::Fixture\n";
    }

    const auto paramsPath = tempRoot / "MethodPropertyFixture.precompile.json";
    WriteMetaParserConfig(
        paramsPath,
        tempRoot,
        runtimeDir,
        outputDir,
        headerPath,
        {
            root / "Runtime" / "Base"
        });

    const auto outputPath = tempRoot / "metaparser.out.txt";
    ExpectMetaParserFailure(paramsPath, outputPath);

    const std::string processText = ReadAllText(outputPath);
    ExpectContains(processText, "PROPERTY(...) may only annotate a field");
    ExpectContains(processText, "PROPERTY(Getter(Name), Setter(Name))");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, BindsFieldsDirectlyWhenNoAccessorsAreDeclared)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto tempRoot = std::filesystem::path(NLS_BUILD_DIR) / "MetaParserUnmarkedAccessorFixture";
    const auto runtimeDir = tempRoot / "Runtime" / "Fixture";
    const auto outputDir = tempRoot / "Gen";
    ResetTempDirectory(tempRoot);
    std::filesystem::create_directories(runtimeDir);
    std::filesystem::create_directories(outputDir);

    const auto headerPath = runtimeDir / "UnmarkedAccessorFixture.h";
    {
        std::ofstream header(headerPath);
        header
            << "#pragma once\n"
            << "#include \"Reflection/Macros.h\"\n"
            << "namespace NLS::Fixture {\n"
            << "CLASS(UnmarkedAccessorFixture)\n"
            << "{\n"
            << "    GENERATED_BODY()\n"
            << "public:\n"
            << "    PROPERTY(DisplayName(\"Value\"))\n"
            << "    int value = 0;\n"
            << "    FUNCTION()\n"
            << "    int GetValue() const { return value; }\n"
            << "    FUNCTION()\n"
            << "    void SetValue(int p_value) { value = p_value; }\n"
            << "};\n"
            << "} // namespace NLS::Fixture\n";
    }

    const auto paramsPath = tempRoot / "UnmarkedAccessorFixture.precompile.json";
    WriteMetaParserConfig(
        paramsPath,
        tempRoot,
        runtimeDir,
        outputDir,
        headerPath,
        {
            root / "Runtime" / "Base"
        });

    const auto outputPath = tempRoot / "metaparser.out.txt";
    ExpectMetaParserSuccess(paramsPath, outputPath);

    const auto generatedSource = outputDir / "UnmarkedAccessorFixture.generated.cpp";
    const std::string sourceText = ReadAllText(generatedSource);

    ExpectContains(
        sourceText,
        "AddField<NLS::Fixture::UnmarkedAccessorFixture, int>(\"value\", &NLS::Fixture::UnmarkedAccessorFixture::value, &NLS::Fixture::UnmarkedAccessorFixture::value");
    ExpectContains(sourceText, "AddMethod(\"GetValue\"");
    ExpectContains(sourceText, "AddMethod(\"SetValue\"");
    ExpectNotContains(
        sourceText,
        "AddField<NLS::Fixture::UnmarkedAccessorFixture, int>(\"value\", static_cast<int (NLS::Fixture::UnmarkedAccessorFixture::*)() const>(&NLS::Fixture::UnmarkedAccessorFixture::GetValue)");
}

namespace
{
struct MetaParserFixtureResult
{
    bool succeeded = false;
    std::string generatedSource;
    std::string processOutput;
};

MetaParserFixtureResult RunAccessorGrammarFixture(
    const std::filesystem::path& root,
    const std::string& fixtureName,
    const std::string& className,
    const std::string& classBody)
{
    const auto tempRoot = std::filesystem::path(NLS_BUILD_DIR) / ("MetaParser" + fixtureName + "Fixture");
    const auto runtimeDir = tempRoot / "Runtime" / "Fixture";
    const auto outputDir = tempRoot / "Gen";
    std::error_code cleanupError;
    std::filesystem::remove_all(tempRoot, cleanupError);
    std::filesystem::create_directories(runtimeDir);
    std::filesystem::create_directories(outputDir);

    const auto headerPath = runtimeDir / (fixtureName + ".h");
    std::string headerText =
        "#pragma once\n"
        "#include \"Reflection/Macros.h\"\n"
        "namespace NLS::Fixture {\n"
        "CLASS(" + className + ")\n"
        "{\n"
        "    GENERATED_BODY()\n" + classBody + "};\n"
        "} // namespace NLS::Fixture\n";
    WriteTextFile(headerPath, headerText);

    const auto paramsPath = tempRoot / (fixtureName + ".precompile.json");
    WriteMetaParserConfig(
        paramsPath,
        tempRoot,
        runtimeDir,
        outputDir,
        headerPath,
        {
            root / "Runtime" / "Base"
        });

    MetaParserFixtureResult result;
    const auto outputPath = tempRoot / "metaparser.out.txt";
    result.succeeded = RunMetaParser(paramsPath, outputPath);
    result.processOutput = ReadOptionalText(outputPath);

    const auto generatedSource = outputDir / (fixtureName + ".generated.cpp");
    if (std::filesystem::exists(generatedSource))
        result.generatedSource = ReadAllText(generatedSource);
    return result;
}
} // namespace

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, BindsAccessorsDeclaredInsideTheFieldPropertyAnnotation)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto fixture = RunAccessorGrammarFixture(
        root,
        "AccessorBinding",
        "AccessorBindingFixture",
        "public:\n"
        "    PROPERTY(DisplayName(\"Value\"), Getter(GetValue), Setter(SetValue))\n"
        "    int value = 0;\n"
        "    FUNCTION()\n"
        "    int GetValue() const { return value; }\n"
        "    FUNCTION()\n"
        "    void SetValue(int p_value) { value = p_value; }\n");

    ASSERT_TRUE(fixture.succeeded) << fixture.processOutput;
    ExpectContains(fixture.generatedSource, "AddField<NLS::Fixture::AccessorBindingFixture, int>(\"value\"");
    ExpectContains(fixture.generatedSource, "&NLS::Fixture::AccessorBindingFixture::GetValue");
    ExpectContains(fixture.generatedSource, "&NLS::Fixture::AccessorBindingFixture::SetValue");
    ExpectNotContains(fixture.generatedSource, "AddField<NLS::Fixture::AccessorBindingFixture, int>(\"value\", &NLS::Fixture::AccessorBindingFixture::value");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, BindsReadOnlyPropertyWhenOnlyAGetterIsDeclared)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto fixture = RunAccessorGrammarFixture(
        root,
        "ReadOnlyAccessor",
        "ReadOnlyAccessorFixture",
        "public:\n"
        "    PROPERTY(DisplayName(\"Value\"), Getter(GetValue))\n"
        "    int value = 0;\n"
        "    FUNCTION()\n"
        "    int GetValue() const { return value; }\n");

    ASSERT_TRUE(fixture.succeeded) << fixture.processOutput;
    ExpectContains(fixture.generatedSource, "AddField<NLS::Fixture::ReadOnlyAccessorFixture, int>(\"value\"");
    ExpectContains(fixture.generatedSource, "&NLS::Fixture::ReadOnlyAccessorFixture::GetValue");
    ExpectContains(fixture.generatedSource, "static_cast<void (NLS::Fixture::ReadOnlyAccessorFixture::*)(int)>(nullptr)");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, EmitsFormerlySerializedAsMetadataWithoutRenamingTheField)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto fixture = RunAccessorGrammarFixture(
        root,
        "RenamedField",
        "RenamedFieldFixture",
        "public:\n"
        "    PROPERTY(DisplayName(\"Local Position\"), FormerlySerializedAs(\"localPosition\"))\n"
        "    int value = 0;\n");

    ASSERT_TRUE(fixture.succeeded) << fixture.processOutput;
    ExpectContains(fixture.generatedSource, "MetaPropertyInitializer<NLS::meta::FormerlySerializedAs>(\"localPosition\")");
    ExpectContains(fixture.generatedSource, "AddField<NLS::Fixture::RenamedFieldFixture, int>(\"value\"");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, RejectsBarePropertyArguments)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto fixture = RunAccessorGrammarFixture(
        root,
        "BarePropertyArgument",
        "BarePropertyArgumentFixture",
        "public:\n"
        "    PROPERTY(value)\n"
        "    int value = 0;\n");

    EXPECT_FALSE(fixture.succeeded) << fixture.processOutput;
    ExpectContains(fixture.processOutput, "Unsupported PROPERTY argument `value`");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, RejectsSetterWithoutGetter)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto fixture = RunAccessorGrammarFixture(
        root,
        "SetterOnly",
        "SetterOnlyFixture",
        "public:\n"
        "    PROPERTY(Setter(SetValue))\n"
        "    int value = 0;\n"
        "    FUNCTION()\n"
        "    void SetValue(int p_value) { value = p_value; }\n");

    EXPECT_FALSE(fixture.succeeded) << fixture.processOutput;
    ExpectContains(fixture.processOutput, "without Getter(...)");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, RejectsMissingAccessorMethod)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto fixture = RunAccessorGrammarFixture(
        root,
        "MissingAccessor",
        "MissingAccessorFixture",
        "public:\n"
        "    PROPERTY(Getter(GetMissing))\n"
        "    int value = 0;\n");

    EXPECT_FALSE(fixture.succeeded) << fixture.processOutput;
    ExpectContains(fixture.processOutput, "does not exist");
    ExpectContains(fixture.processOutput, "NLS::Fixture::MissingAccessorFixture::GetMissing");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, RejectsAccessorWithMismatchedSignature)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto fixture = RunAccessorGrammarFixture(
        root,
        "MismatchedGetter",
        "MismatchedGetterFixture",
        "public:\n"
        "    PROPERTY(Getter(GetValue))\n"
        "    int value = 0;\n"
        "    FUNCTION()\n"
        "    int GetValue(int p_index) const { return value + p_index; }\n");

    EXPECT_FALSE(fixture.succeeded) << fixture.processOutput;
    ExpectContains(fixture.processOutput, "no public overload matches");
    ExpectContains(fixture.processOutput, "A getter must take no arguments and return a value.");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, RejectsAccessorWithIncompatibleTypes)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto fixture = RunAccessorGrammarFixture(
        root,
        "IncompatibleAccessors",
        "IncompatibleAccessorsFixture",
        "public:\n"
        "    PROPERTY(Getter(GetValue), Setter(SetValue))\n"
        "    int value = 0;\n"
        "    FUNCTION()\n"
        "    int GetValue() const { return value; }\n"
        "    FUNCTION()\n"
        "    void SetValue(float p_value) { value = static_cast<int>(p_value); }\n");

    EXPECT_FALSE(fixture.succeeded) << fixture.processOutput;
    ExpectContains(fixture.processOutput, "has incompatible accessor types");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, RejectsAmbiguousAccessorOverloads)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto fixture = RunAccessorGrammarFixture(
        root,
        "AmbiguousAccessor",
        "AmbiguousAccessorFixture",
        "public:\n"
        "    PROPERTY(Getter(GetValue), Setter(SetValue))\n"
        "    int value = 0;\n"
        "    FUNCTION()\n"
        "    int GetValue() const { return value; }\n"
        "    FUNCTION()\n"
        "    void SetValue(int p_value) { value = p_value; }\n"
        "    FUNCTION()\n"
        "    void SetValue(const int& p_value) { value = p_value; }\n");

    EXPECT_FALSE(fixture.succeeded) << fixture.processOutput;
    ExpectContains(fixture.processOutput, "ambiguously");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, RejectsDuplicateAccessorBindings)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto fixture = RunAccessorGrammarFixture(
        root,
        "DuplicateGetter",
        "DuplicateGetterFixture",
        "public:\n"
        "    PROPERTY(Getter(GetValue), Getter(GetValue))\n"
        "    int value = 0;\n"
        "    FUNCTION()\n"
        "    int GetValue() const { return value; }\n");

    EXPECT_FALSE(fixture.succeeded) << fixture.processOutput;
    ExpectContains(fixture.processOutput, "declares Getter(...) more than once");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, RejectsGetterAndSetterArgumentsOnFunctions)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto fixture = RunAccessorGrammarFixture(
        root,
        "AccessorOnMethod",
        "AccessorOnMethodFixture",
        "public:\n"
        "    int value = 0;\n"
        "    FUNCTION(Getter(GetValue))\n"
        "    int GetValue() const { return value; }\n");

    EXPECT_FALSE(fixture.succeeded) << fixture.processOutput;
    ExpectContains(fixture.processOutput, "Getter(...)/Setter(...) belong on the PROPERTY annotation of the backing field.");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, RejectsMalformedRangeMetadataBeforeGeneratingCpp)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto tempRoot = std::filesystem::path(NLS_BUILD_DIR) / "MetaParserMalformedRangeFixture";
    const auto runtimeDir = tempRoot / "Runtime" / "Fixture";
    const auto outputDir = tempRoot / "Gen";
    ResetTempDirectory(tempRoot);
    std::filesystem::create_directories(runtimeDir);
    std::filesystem::create_directories(outputDir);

    const auto headerPath = runtimeDir / "MalformedRangeFixture.h";
    {
        std::ofstream header(headerPath);
        header
            << "#pragma once\n"
            << "#include \"Reflection/Macros.h\"\n"
            << "namespace NLS::Fixture {\n"
            << "STRUCT(MalformedRangeFixture)\n"
            << "{\n"
            << "    GENERATED_BODY()\n"
            << "    PROPERTY(Range(0, MakeMax()))\n"
            << "    float value = 0.0f;\n"
            << "};\n"
            << "} // namespace NLS::Fixture\n";
    }

    const auto paramsPath = tempRoot / "MalformedRangeFixture.precompile.json";
    WriteMetaParserConfig(
        paramsPath,
        tempRoot,
        runtimeDir,
        outputDir,
        headerPath,
        {
            root / "Runtime" / "Base"
        });

    const auto outputPath = tempRoot / "metaparser.out.txt";
    ExpectMetaParserFailure(paramsPath, outputPath);

    const std::string processText = ReadAllText(outputPath);
    ExpectContains(processText, "Range metadata must be Range(<finite-number>, <finite-number>)");
    ExpectContains(processText, "Range(0, MakeMax())");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, RejectsUnknownPropertyMetadataBeforeGeneratingCpp)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto tempRoot = std::filesystem::path(NLS_BUILD_DIR) / "MetaParserUnknownPropertyMetaFixture";
    const auto runtimeDir = tempRoot / "Runtime" / "Fixture";
    const auto outputDir = tempRoot / "Gen";
    ResetTempDirectory(tempRoot);
    std::filesystem::create_directories(runtimeDir);
    std::filesystem::create_directories(outputDir);

    const auto headerPath = runtimeDir / "UnknownPropertyMetaFixture.h";
    {
        std::ofstream header(headerPath);
        header
            << "#pragma once\n"
            << "#include \"Reflection/Macros.h\"\n"
            << "namespace NLS::Fixture {\n"
            << "STRUCT(UnknownPropertyMetaFixture)\n"
            << "{\n"
            << "    GENERATED_BODY()\n"
            << "    PROPERTY(Rnage(0, 1))\n"
            << "    float value = 0.0f;\n"
            << "};\n"
            << "} // namespace NLS::Fixture\n";
    }

    const auto paramsPath = tempRoot / "UnknownPropertyMetaFixture.precompile.json";
    WriteMetaParserConfig(
        paramsPath,
        tempRoot,
        runtimeDir,
        outputDir,
        headerPath,
        {
            root / "Runtime" / "Base"
        });

    const auto outputPath = tempRoot / "metaparser.out.txt";
    ExpectMetaParserFailure(paramsPath, outputPath);

    const std::string processText = ReadAllText(outputPath);
    ExpectContains(processText, "Unsupported PROPERTY argument");
    ExpectContains(processText, "Rnage(0, 1)");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, RejectsInvertedRangeMetadataBeforeGeneratingCpp)
{
    const auto root = std::filesystem::path(NLS_ROOT_DIR);
    const auto tempRoot = std::filesystem::path(NLS_BUILD_DIR) / "MetaParserInvertedRangeFixture";
    const auto runtimeDir = tempRoot / "Runtime" / "Fixture";
    const auto outputDir = tempRoot / "Gen";
    ResetTempDirectory(tempRoot);
    std::filesystem::create_directories(runtimeDir);
    std::filesystem::create_directories(outputDir);

    const auto headerPath = runtimeDir / "InvertedRangeFixture.h";
    {
        std::ofstream header(headerPath);
        header
            << "#pragma once\n"
            << "#include \"Reflection/Macros.h\"\n"
            << "namespace NLS::Fixture {\n"
            << "STRUCT(InvertedRangeFixture)\n"
            << "{\n"
            << "    GENERATED_BODY()\n"
            << "    PROPERTY(Range(10, 0))\n"
            << "    float value = 0.0f;\n"
            << "};\n"
            << "} // namespace NLS::Fixture\n";
    }

    const auto paramsPath = tempRoot / "InvertedRangeFixture.precompile.json";
    WriteMetaParserConfig(
        paramsPath,
        tempRoot,
        runtimeDir,
        outputDir,
        headerPath,
        {
            root / "Runtime" / "Base"
        });

    const auto outputPath = tempRoot / "metaparser.out.txt";
    ExpectMetaParserFailure(paramsPath, outputPath);

    const std::string processText = ReadAllText(outputPath);
    ExpectContains(processText, "Range metadata min must be less than or equal to max");
    ExpectContains(processText, "Range(10, 0)");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, GeneratedSourcesRegisterEnumTypesWithoutGeneratedBody)
{
    const std::filesystem::path lightTypeSource =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Gen/Settings/ELightType.generated.cpp";
    const std::string lightTypeText = ReadAllText(lightTypeSource);

    ExpectContains(lightTypeText, "StaticTypeRegister_NLS__Render__Settings__ELightType::StaticTypeRegister_NLS__Render__Settings__ELightType()");
    ExpectContains(lightTypeText, "ReflectionModuleRegistry::Add(NLS::meta::HashTypeKey(\"Settings/ELightType.h\"), \"Settings/ELightType.h\", &RegisterType_NLS__Render__Settings__ELightType)");
    ExpectContains(lightTypeText, "static StaticTypeRegister_NLS__Render__Settings__ELightType g_StaticTypeRegister_NLS__Render__Settings__ELightType");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, GeneratesComponentMenuTypeMetadataBindings)
{
    const std::filesystem::path meshRendererSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Gen/Components/MeshRenderer.generated.cpp";
    const std::filesystem::path cameraSource = std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Gen/Components/CameraComponent.generated.cpp";
    const std::string meshRendererText = ReadAllText(meshRendererSource);
    const std::string cameraText = ReadAllText(cameraSource);

    ExpectContains(meshRendererText, "ComponentMenu");
    ExpectContains(meshRendererText, "Rendering/Mesh Renderer");
    ExpectContains(meshRendererText, "NLS::meta::DisallowMultipleComponent");
    ExpectContains(meshRendererText, "NLS::meta::RequireComponent");
    ExpectContains(meshRendererText, "NLS::Engine::Components::MeshFilter");
    ExpectContains(cameraText, "NLS::meta::DisallowMultipleComponent");
    ExpectContains(meshRendererText, "type.meta");
}

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, MapsDenseRuntimeMetaPropertyGeneratedBodiesToTheirOwnTypes)
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
        "NLS::meta::DisallowMultipleComponent",
        "NLS::meta::RequireComponent",
        "NLS::meta::RequiresRestart",
        "NLS::meta::Range",
        "NLS::meta::DisplayName",
        "NLS::meta::Tooltip",
        "NLS::meta::Header",
        "NLS::meta::Space",
        "NLS::meta::Min",
        "NLS::meta::Max",
        "NLS::meta::Multiline",
        "NLS::meta::ReadOnly",
        "NLS::meta::HideInInspector",
        "NLS::meta::Flags",
        "NLS::meta::ColorUsage",
        "NLS::meta::ReferenceTypeConstraint",
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

NLS_LONG_RUNNING_TEST(MetaParserGenerationModuleIntegrationPerformanceTests, GeneratesExpectedEditorSettingsReflectionBindings)
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
