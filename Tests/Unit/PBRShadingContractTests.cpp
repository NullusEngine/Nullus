#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "Guid.h"
#include "Math/Vector2.h"
#include "Math/Vector3.h"
#include "Rendering/ShaderCompiler/ShaderCompiler.h"
#include "Rendering/ShaderLab/ShaderLabParser.h"
#include "Rendering/ShaderLab/ShaderLabTypes.h"

namespace
{
using NLS::Maths::Vector2;
using NLS::Maths::Vector3;

constexpr float kNormalTolerance = 1.0e-5f;

std::filesystem::path PBRNormalsPath()
{
    return std::filesystem::path(NLS_ROOT_DIR) /
        "App/Assets/Engine/Shaders/NullusShaderLibrary/PBRNormals.hlsl";
}

std::filesystem::path ShaderRootPath()
{
    return std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders";
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

std::string_view TrimWhitespace(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1u);
}

std::string ExtractFunctionDefinition(const std::string& source, std::string_view functionName)
{
    size_t nameOffset = 0u;
    while ((nameOffset = source.find(functionName, nameOffset)) != std::string::npos)
    {
        size_t parameterStart = nameOffset + functionName.size();
        while (parameterStart < source.size() &&
            std::string_view(" \t\r\n").find(source[parameterStart]) != std::string_view::npos)
        {
            ++parameterStart;
        }
        if (parameterStart < source.size() && source[parameterStart] == '(')
            break;
        nameOffset += functionName.size();
    }
    if (nameOffset == std::string::npos)
        return {};

    const auto lineStart = source.rfind('\n', nameOffset);
    const auto bodyStart = source.find('{', nameOffset + functionName.size());
    if (bodyStart == std::string::npos)
        return {};

    size_t depth = 0u;
    for (size_t offset = bodyStart; offset < source.size(); ++offset)
    {
        if (source[offset] == '{')
            ++depth;
        else if (source[offset] == '}' && --depth == 0u)
            return source.substr(
                lineStart == std::string::npos ? 0u : lineStart + 1u,
                offset - (lineStart == std::string::npos ? 0u : lineStart + 1u) + 1u);
    }
    return {};
}

std::vector<std::string> ExtractCallArguments(
    const std::string& source,
    std::string_view functionName,
    size_t occurrence = 0u)
{
    size_t nameOffset = 0u;
    for (size_t index = 0u; index <= occurrence; ++index)
    {
        nameOffset = source.find(functionName, nameOffset);
        if (nameOffset == std::string::npos)
            return {};
        if (index != occurrence)
            nameOffset += functionName.size();
    }

    const auto argumentsStart = source.find('(', nameOffset + functionName.size());
    if (argumentsStart == std::string::npos)
        return {};

    std::vector<std::string> arguments;
    size_t argumentStart = argumentsStart + 1u;
    size_t depth = 1u;
    for (size_t offset = argumentStart; offset < source.size(); ++offset)
    {
        if (source[offset] == '(')
        {
            ++depth;
        }
        else if (source[offset] == ')')
        {
            if (--depth == 0u)
            {
                arguments.emplace_back(TrimWhitespace(
                    std::string_view(source).substr(argumentStart, offset - argumentStart)));
                return arguments;
            }
        }
        else if (source[offset] == ',' && depth == 1u)
        {
            arguments.emplace_back(TrimWhitespace(
                std::string_view(source).substr(argumentStart, offset - argumentStart)));
            argumentStart = offset + 1u;
        }
    }
    return {};
}

std::vector<std::string> ExtractFunctionParameterNames(
    const std::string& source,
    std::string_view functionName)
{
    const auto definition = ExtractFunctionDefinition(source, functionName);
    auto parameters = ExtractCallArguments(definition, functionName);
    for (auto& parameter : parameters)
    {
        const auto nameStart = parameter.find_last_of(" \t\r\n");
        parameter = std::string(TrimWhitespace(
            nameStart == std::string::npos
                ? std::string_view(parameter)
                : std::string_view(parameter).substr(nameStart + 1u)));
    }
    return parameters;
}

std::filesystem::path NormalizeComparablePath(const std::filesystem::path& path)
{
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : normalized;
}

bool HasDependency(
    const std::vector<std::string>& dependencyPaths,
    const std::filesystem::path& expectedPath)
{
    const auto normalizedExpected = NormalizeComparablePath(expectedPath);
    return std::any_of(
        dependencyPaths.begin(),
        dependencyPaths.end(),
        [&normalizedExpected](const std::string& dependencyPath)
        {
            return NormalizeComparablePath(dependencyPath) == normalizedExpected;
        });
}

size_t CountOccurrences(const std::string& source, std::string_view needle)
{
    size_t count = 0u;
    for (size_t offset = 0u; (offset = source.find(needle, offset)) != std::string::npos; offset += needle.size())
        ++count;
    return count;
}

std::multiset<std::string> FindNLSFunctionDefinitions(const std::string& source)
{
    const std::regex definitionPattern(
        R"((?:^|\n)\s*(?:[A-Za-z_][A-Za-z0-9_]*(?:\s*<[^>\n]+>)?\s+)+(NLS[A-Za-z0-9_]+)\s*\([^;{}]*\)\s*\{)");
    std::multiset<std::string> definitions;
    for (std::sregex_iterator match(source.begin(), source.end(), definitionPattern), end;
         match != end;
         ++match)
    {
        definitions.insert((*match)[1].str());
    }
    return definitions;
}

Vector3 Normalize(const Vector3& value)
{
    return Vector3::Normalize(value);
}

float Dot(const Vector3& left, const Vector3& right)
{
    return Vector3::Dot(left, right);
}

bool IsFinite(const Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

Vector3 SafeNormalize(const Vector3& value, const Vector3& fallback)
{
    const float lengthSquared = Dot(value, value);
    if (IsFinite(value) && lengthSquared > 1.0e-8f && lengthSquared < 1.0e+20f)
        return value * (1.0f / std::sqrt(lengthSquared));

    const float fallbackLengthSquared = Dot(fallback, fallback);
    if (IsFinite(fallback) && fallbackLengthSquared > 1.0e-8f && fallbackLengthSquared < 1.0e+20f)
        return fallback * (1.0f / std::sqrt(fallbackLengthSquared));
    return Vector3::Forward;
}

Vector3 OrientGeometryNormal(const Vector3& normal, bool isFrontFace)
{
    const Vector3 geometryNormal = SafeNormalize(normal, Vector3::Forward);
    return isFrontFace ? geometryNormal : -geometryNormal;
}

Vector3 ConstrainShadingNormal(const Vector3& shadingNormal, const Vector3& geometryNormal)
{
    const Vector3 orientedGeometryNormal = SafeNormalize(geometryNormal, Vector3::Forward);
    Vector3 constrained = SafeNormalize(shadingNormal, orientedGeometryNormal);
    constrained -= orientedGeometryNormal * std::min(0.0f, Dot(constrained, orientedGeometryNormal));
    return SafeNormalize(constrained, orientedGeometryNormal);
}

float SignNotZero(float value)
{
    return value >= 0.0f ? 1.0f : -1.0f;
}

Vector2 OctEncode(const Vector3& normal)
{
    const float inverseL1Norm = 1.0f / (std::fabs(normal.x) + std::fabs(normal.y) + std::fabs(normal.z));
    Vector2 encoded{normal.x * inverseL1Norm, normal.y * inverseL1Norm};
    if (normal.z < 0.0f)
    {
        encoded = {
            (1.0f - std::fabs(encoded.y)) * SignNotZero(encoded.x),
            (1.0f - std::fabs(encoded.x)) * SignNotZero(encoded.y)};
    }
    return encoded;
}

Vector3 OctDecode(const Vector2& encoded)
{
    Vector3 normal{encoded.x, encoded.y, 1.0f - std::fabs(encoded.x) - std::fabs(encoded.y)};
    const float fold = std::clamp(-normal.z, 0.0f, 1.0f);
    normal.x += normal.x >= 0.0f ? -fold : fold;
    normal.y += normal.y >= 0.0f ? -fold : fold;
    return Normalize(normal);
}

float GeometryFade(float ndotDirection)
{
    if (ndotDirection <= 0.0f)
        return 0.0f;

    const float t = std::clamp(ndotDirection / 0.10f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

bool IsDxcUnavailableDiagnostic(const std::string& diagnostics)
{
    return diagnostics.find("Unable to locate dxc.exe.") != std::string::npos ||
        diagnostics.find("Unable to locate an executable native dxc.") != std::string::npos ||
        diagnostics.find("Failed to spawn shader compiler process (") != std::string::npos ||
        diagnostics.find("[dxc-exit-code] 126") != std::string::npos ||
        diagnostics.find("[dxc-exit-code] 127") != std::string::npos;
}

NLS::Render::ShaderCompiler::ShaderCompilationInput MakeNativeShaderCompileInput(
    const std::filesystem::path& sourcePath,
    NLS::Render::ShaderCompiler::ShaderStage stage,
    NLS::Render::ShaderCompiler::ShaderTargetPlatform target,
    const std::filesystem::path& artifactDirectory)
{
    NLS::Render::ShaderCompiler::ShaderCompilationInput input;
    input.assetPath = sourcePath.string();
    input.stage = stage;
    input.options.targetPlatform = target;
    input.options.targetProfile =
        stage == NLS::Render::ShaderCompiler::ShaderStage::Vertex ? "vs_6_0" : "ps_6_0";
    input.options.entryPoint =
        stage == NLS::Render::ShaderCompiler::ShaderStage::Vertex ? "VSMain" : "PSMain";
    input.options.treatWarningsAsErrors = true;
    input.options.artifactDirectory = artifactDirectory.string();
    input.options.includeDirectories.push_back(ShaderRootPath().string());
    return input;
}

void ExpectSpirvReturnsFiniteUnitPositiveZ(
    const std::string& dxcPath,
    const std::filesystem::path& wrapperPath,
    const std::filesystem::path& artifactDirectory)
{
    std::filesystem::create_directories(artifactDirectory);
    const auto artifactPath = artifactDirectory / "ExpectedOutput.spv";
    const auto assemblyPath = artifactDirectory / "ExpectedOutput.spv.txt";
    const auto compile = NLS::Render::ShaderCompiler::ExecuteShaderCompilerProcess(
        dxcPath,
        {
            "-nologo",
            "-spirv",
            "-E", "PSMain",
            "-T", "ps_6_0",
            "-O3",
            "-Fo", artifactPath.string(),
            "-Fc", assemblyPath.string(),
            "-I", ShaderRootPath().string(),
            wrapperPath.string()});
    ASSERT_EQ(compile.status, NLS::Render::ShaderCompiler::ShaderProcessStatus::Succeeded)
        << compile.diagnostics;

    const auto assembly = ReadTextFile(assemblyPath);
    const std::regex expectedOutputPattern(
        R"((%[A-Za-z0-9_]+)\s*=\s*OpConstantComposite\s+%v4float\s+%float_0\s+%float_0\s+%float_1\s+%float_1)");
    std::smatch expectedOutput;
    ASSERT_TRUE(std::regex_search(assembly, expectedOutput, expectedOutputPattern))
        << assembly;
    EXPECT_NE(
        assembly.find("OpStore %out_var_SV_Target0 " + expectedOutput[1].str()),
        std::string::npos)
        << assembly;
}

void ExpectGuardedDxilReturnsFiniteUnitPositiveZ(const std::string& assembly)
{
    const std::regex fallbackXPattern(
        R"((%[0-9]+)\s*=\s*phi float \[ %[0-9]+, %[0-9]+ \], \[ 0\.000000e\+00, %0 \])");
    const std::regex fallbackZPattern(
        R"((%[0-9]+)\s*=\s*phi float \[ 0\.000000e\+00, %[0-9]+ \], \[ 1\.000000e\+00, %0 \])");
    std::smatch fallbackX;
    std::smatch fallbackZ;
    ASSERT_TRUE(std::regex_search(assembly, fallbackX, fallbackXPattern)) << assembly;
    ASSERT_TRUE(std::regex_search(assembly, fallbackZ, fallbackZPattern)) << assembly;
    EXPECT_NE(
        assembly.find("i8 0, float " + fallbackX[1].str()),
        std::string::npos)
        << assembly;
    EXPECT_NE(
        assembly.find("i8 1, float 0.000000e+00"),
        std::string::npos)
        << assembly;
    EXPECT_NE(
        assembly.find("i8 2, float " + fallbackZ[1].str()),
        std::string::npos)
        << assembly;
    EXPECT_NE(
        assembly.find("i8 3, float 1.000000e+00"),
        std::string::npos)
        << assembly;
}

class ScopedTemporaryDirectory final
{
public:
    ScopedTemporaryDirectory()
        : m_path(
              std::filesystem::temp_directory_path() /
              ("nullus_pbr_normals_contract_" + NLS::Guid::New().ToString()))
    {
        std::filesystem::create_directories(m_path);
    }

    ~ScopedTemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    const std::filesystem::path& GetPath() const { return m_path; }

private:
    std::filesystem::path m_path;
};
} // namespace

TEST(PBRShadingContractTests, OctGeometryNormalRoundTripsBothHemispheres)
{
    for (const auto normal : std::array{
             Normalize(Vector3{1.0f, 2.0f, 3.0f}),
             Normalize(Vector3{1.0f, 2.0f, -3.0f}),
             Normalize(Vector3{-1.0f, 2.0f, -3.0f}),
             Normalize(Vector3{-1.0f, -2.0f, -3.0f}),
             Normalize(Vector3{1.0f, -2.0f, -3.0f}),
             Normalize(Vector3{1.0f, 0.0f, -1.0e-5f}),
             Normalize(Vector3{-1.0f, 0.0f, -1.0e-5f}),
             Normalize(Vector3{0.0f, 1.0f, -1.0e-5f}),
             Normalize(Vector3{0.0f, -1.0f, -1.0e-5f}),
             Vector3{0.0f, 0.0f, -1.0f}})
    {
        EXPECT_GT(Dot(normal, OctDecode(OctEncode(normal))), 1.0f - kNormalTolerance);
    }
}

TEST(PBRShadingContractTests, GeometryNormalOrientationHandlesFrontAndBackFaces)
{
    const Vector3 sourceNormal{0.0f, 0.0f, 2.0f};
    EXPECT_GT(Dot(OrientGeometryNormal(sourceNormal, true), Vector3::Forward), 1.0f - kNormalTolerance);
    EXPECT_GT(Dot(OrientGeometryNormal(sourceNormal, false), -Vector3::Forward), 1.0f - kNormalTolerance);
}

TEST(PBRShadingContractTests, InvalidGeometryNormalsUseFiniteUnitFallbacks)
{
    struct GeometryCase
    {
        std::string_view name;
        Vector3 geometryNormal;
    };
    const std::array cases{
        GeometryCase{"zero", Vector3::Zero},
        GeometryCase{
            "NaN",
            Vector3{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}},
        GeometryCase{
            "+Inf",
            Vector3{std::numeric_limits<float>::infinity(), 0.0f, 0.0f}},
        GeometryCase{
            "-Inf",
            Vector3{-std::numeric_limits<float>::infinity(), 0.0f, 0.0f}}};

    for (const auto& testCase : cases)
    {
        SCOPED_TRACE(testCase.name);
        for (const bool isFrontFace : {true, false})
        {
            const Vector3 expected = isFrontFace ? Vector3::Forward : -Vector3::Forward;
            const Vector3 oriented = OrientGeometryNormal(testCase.geometryNormal, isFrontFace);
            EXPECT_TRUE(IsFinite(oriented));
            EXPECT_NEAR(Dot(oriented, oriented), 1.0f, kNormalTolerance);
            EXPECT_GT(Dot(oriented, expected), 1.0f - kNormalTolerance);
        }

        const Vector3 constrained = ConstrainShadingNormal(Vector3::Zero, testCase.geometryNormal);
        EXPECT_TRUE(IsFinite(constrained));
        EXPECT_NEAR(Dot(constrained, constrained), 1.0f, kNormalTolerance);
        EXPECT_GT(Dot(constrained, Vector3::Forward), 1.0f - kNormalTolerance);
    }
}

TEST(PBRShadingContractTests, ShadingNormalConstraintCoversHemispheresAndInvalidInputs)
{
    struct ShadingCase
    {
        std::string_view name;
        Vector3 shadingNormal;
        bool expectsGeometryNormal;
    };

    for (const bool isFrontFace : {true, false})
    {
        const Vector3 geometryNormal = OrientGeometryNormal(Vector3::Forward, isFrontFace);
        const Vector3 tangent = Vector3::Right;
        const std::array cases{
            ShadingCase{"same hemisphere", Normalize(tangent + geometryNormal), false},
            ShadingCase{"wrong hemisphere", Normalize(tangent - geometryNormal), false},
            ShadingCase{"positive collinear", geometryNormal, true},
            ShadingCase{"negative collinear", -geometryNormal, true},
            ShadingCase{"near reverse", Normalize(tangent * 1.0e-5f - geometryNormal), true},
            ShadingCase{"tangent", tangent, false},
            ShadingCase{"zero", Vector3::Zero, true},
            ShadingCase{
                "NaN",
                Vector3{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f},
                true},
            ShadingCase{
                "+Inf",
                Vector3{std::numeric_limits<float>::infinity(), 0.0f, 0.0f},
                true},
            ShadingCase{
                "-Inf",
                Vector3{-std::numeric_limits<float>::infinity(), 0.0f, 0.0f},
                true}};

        for (const auto& testCase : cases)
        {
            SCOPED_TRACE(testCase.name);
            SCOPED_TRACE(isFrontFace ? "front face" : "back face");
            const Vector3 constrained = ConstrainShadingNormal(testCase.shadingNormal, geometryNormal);
            EXPECT_TRUE(IsFinite(constrained));
            EXPECT_NEAR(Dot(constrained, constrained), 1.0f, kNormalTolerance);
            EXPECT_GE(Dot(constrained, geometryNormal), -kNormalTolerance);
            if (testCase.expectsGeometryNormal)
                EXPECT_GT(Dot(constrained, geometryNormal), 1.0f - kNormalTolerance);
        }
    }
}

TEST(PBRShadingContractTests, GeometryHorizonRejectsWrongHemisphereAndFadesContinuously)
{
    EXPECT_FLOAT_EQ(GeometryFade(-0.01f), 0.0f);
    EXPECT_FLOAT_EQ(GeometryFade(0.0f), 0.0f);
    EXPECT_NEAR(GeometryFade(0.05f), 0.5f, 1.0e-5f);
    EXPECT_FLOAT_EQ(GeometryFade(0.10f), 1.0f);
}

TEST(PBRShadingContractTests, ForwardDirectBrdfUsesGeometryGateAndShadingNormal)
{
    const auto source = ReadTextFile(ShaderRootPath() / "LightGridCommon.hlsli");
    ASSERT_FALSE(source.empty());
    EXPECT_NE(
        source.find("#include \"NullusShaderLibrary/PBRNormals.hlsl\""),
        std::string::npos);

    const auto direct = ExtractFunctionDefinition(source, "NLSEvaluateCookTorranceDirect");
    ASSERT_FALSE(direct.empty());
    const std::vector<std::string> expectedParameters{
        "geometryNormalWS",
        "shadingNormalWS",
        "viewDir",
        "lightDir",
        "safeAlbedo",
        "safeMetallic",
        "safeRoughness",
        "lightColor",
        "lightIntensity",
        "attenuation"};
    EXPECT_EQ(
        ExtractFunctionParameterNames(source, "NLSEvaluateCookTorranceDirect"),
        expectedParameters);

    const auto geometryNdotL = direct.find(
        "const float geometryNdotL = dot(geometryNormalWS, lightDir);");
    const auto geometryNdotV = direct.find(
        "const float geometryNdotV = dot(geometryNormalWS, viewDir);");
    const auto geometryGate = direct.find(
        "if (geometryNdotL <= 0.0f || geometryNdotV <= 0.0f)");
    const auto geometryFade = direct.find(
        "const float geometryFade = NLSGeometryHorizonFade(geometryNdotL) *\n"
        "        NLSGeometryHorizonFade(geometryNdotV);");
    ASSERT_NE(geometryNdotL, std::string::npos);
    ASSERT_NE(geometryNdotV, std::string::npos);
    ASSERT_NE(geometryGate, std::string::npos);
    ASSERT_NE(geometryFade, std::string::npos);
    EXPECT_LT(geometryNdotL, geometryGate);
    EXPECT_LT(geometryNdotV, geometryGate);
    EXPECT_LT(geometryGate, geometryFade);
    EXPECT_EQ(direct.find("saturate(dot(geometryNormalWS"), std::string::npos)
        << "Geometry hemisphere rejection must observe the signed dot product.";

    const auto shadingDirect = ExtractFunctionDefinition(
        source,
        "NLSEvaluateCookTorranceShadingDirect");
    ASSERT_FALSE(shadingDirect.empty());
    const std::string shadingNormalTokens[] = {
        "const float ndotv = saturate(dot(shadingNormalWS, viewDir));",
        "const float ndotl = saturate(dot(shadingNormalWS, lightDir));",
        "NLSSafeLightingNormalize(lightDir + viewDir, shadingNormalWS)",
        "saturate(dot(shadingNormalWS, halfVector))",
        "NLSGeometrySmith(ndotv, ndotl, safeRoughness)"};
    for (const auto& token : shadingNormalTokens)
        EXPECT_NE(shadingDirect.find(token), std::string::npos) << token;

    EXPECT_NE(
        shadingDirect.find("return brdf * radiance * ndotl * visibility;"),
        std::string::npos);
    const auto shadingArguments = ExtractCallArguments(
        direct,
        "NLSEvaluateCookTorranceShadingDirect");
    ASSERT_EQ(shadingArguments.size(), 9u);
    EXPECT_EQ(shadingArguments[0], "shadingNormalWS");
    EXPECT_EQ(shadingArguments[1], "viewDir");
    EXPECT_EQ(shadingArguments[2], "lightDir");
    EXPECT_NE(direct.find("return shadingDirect * geometryFade;"), std::string::npos);
}

TEST(PBRShadingContractTests, ShadingDirectSkipsBrdfOutsideLightHemisphereOnly)
{
    const auto source = ReadTextFile(ShaderRootPath() / "LightGridCommon.hlsli");
    ASSERT_FALSE(source.empty());

    const auto shadingDirect = ExtractFunctionDefinition(
        source,
        "NLSEvaluateCookTorranceShadingDirect");
    ASSERT_FALSE(shadingDirect.empty());

    const auto ndotv = shadingDirect.find(
        "const float ndotv = saturate(dot(shadingNormalWS, viewDir));");
    const auto ndotl = shadingDirect.find(
        "const float ndotl = saturate(dot(shadingNormalWS, lightDir));");
    const auto lightHemisphereGate = shadingDirect.find(
        "if (ndotl <= 0.0f)\n"
        "        return 0.0f.xxx;");
    const auto brdfWork = shadingDirect.find(
        "const float3 dielectricF0 = NLS_PBR_DIELECTRIC_F0.xxx;");
    ASSERT_NE(ndotv, std::string::npos);
    ASSERT_NE(ndotl, std::string::npos);
    ASSERT_NE(lightHemisphereGate, std::string::npos);
    ASSERT_NE(brdfWork, std::string::npos);
    EXPECT_LT(ndotv, lightHemisphereGate);
    EXPECT_LT(ndotl, lightHemisphereGate);
    EXPECT_LT(lightHemisphereGate, brdfWork)
        << "A non-positive shading NdotL contributes zero and must skip all BRDF work.";
    EXPECT_EQ(shadingDirect.find("if (ndotv <= 0.0f"), std::string::npos)
        << "Shading NdotV must only gate specular so back-facing-view diffuse remains visible.";
}

TEST(PBRShadingContractTests, ForwardAccumulatorsKeepAmbientOutsideGeometryGatedDirectLoop)
{
    const auto source = ReadTextFile(ShaderRootPath() / "LightGridCommon.hlsli");
    ASSERT_FALSE(source.empty());

    const auto clustered = ExtractFunctionDefinition(source, "NLSAccumulateClusteredLightingPBR");
    ASSERT_FALSE(clustered.empty());
    const auto clusteredParameters = ExtractFunctionParameterNames(
        source,
        "NLSAccumulateClusteredLightingPBR");
    ASSERT_GE(clusteredParameters.size(), 6u);
    EXPECT_EQ(clusteredParameters[4], "geometryNormalWS");
    EXPECT_EQ(clusteredParameters[5], "shadingNormalWS");
    EXPECT_NE(
        clustered.find("NLSFilterPerceptualRoughness(safeShadingNormalWS, roughness)"),
        std::string::npos);

    const auto ambientFloor = clustered.find(
        "float3 lighting = safeAlbedo * (NLSGetVisibleAmbientFloor() * safeAo);");
    const auto directLoop = clustered.find("[loop]");
    const auto directCall = clustered.find("NLSEvaluateCookTorranceDirect(");
    ASSERT_NE(ambientFloor, std::string::npos);
    ASSERT_NE(directLoop, std::string::npos);
    ASSERT_NE(directCall, std::string::npos);
    EXPECT_LT(ambientFloor, directLoop);
    EXPECT_LT(directLoop, directCall);

    const auto directArguments = ExtractCallArguments(
        clustered,
        "NLSEvaluateCookTorranceDirect");
    ASSERT_EQ(directArguments.size(), 10u);
    EXPECT_EQ(directArguments[0], "safeGeometryNormalWS");
    EXPECT_EQ(directArguments[1], "safeShadingNormalWS");
    EXPECT_EQ(directArguments[2], "viewDir");
    EXPECT_EQ(directArguments[3], "lightDir");
    EXPECT_EQ(
        std::find(directArguments.begin(), directArguments.end(), "safeAo"),
        directArguments.end());

    const auto scene = ExtractFunctionDefinition(source, "NLSAccumulateSceneLightingPBR");
    const auto sceneDirectArguments = ExtractCallArguments(
        scene,
        "NLSEvaluateCookTorranceDirect");
    ASSERT_EQ(sceneDirectArguments.size(), 9u);
    EXPECT_EQ(sceneDirectArguments[0], "safeNormalWS");
    EXPECT_EQ(sceneDirectArguments[1], "viewDir")
        << "Deferred must not apply a geometry gate until it stores a separate geometry normal.";
}

TEST(PBRShadingContractTests, ForwardPixelShadersOrientConstrainAndPassGeometryThenShadingNormals)
{
    struct ForwardShaderContract
    {
        std::filesystem::path path;
        std::string_view positionName;
        std::string_view interpolatedNormalName;
        std::string_view normalMapCondition;
    };
    const std::array shaders{
        ForwardShaderContract{
            ShaderRootPath() / "StandardPBR.hlsl",
            "input.PositionWS",
            "input.NormalWS",
            "u_EnableNormalMapping > 0.5f"},
        ForwardShaderContract{
            ShaderRootPath() / "ShaderLab/StandardPBR.shader",
            "input.positionWS",
            "input.normalWS",
            "#if defined(_NORMALMAP)"}};

    for (const auto& shader : shaders)
    {
        SCOPED_TRACE(shader.path.string());
        const auto source = ReadTextFile(shader.path);
        ASSERT_FALSE(source.empty());
        const auto pixel = ExtractFunctionDefinition(source, "PSMain");
        ASSERT_FALSE(pixel.empty());
        EXPECT_NE(pixel.find("bool isFrontFace : SV_IsFrontFace"), std::string::npos);

        const std::string safeGeometryDefinition =
            "const float3 interpolatedGeometryNormalWS = NLSSafeNormalize(" +
            std::string(shader.interpolatedNormalName) +
            ", float3(0.0f, 0.0f, 1.0f));";
        const auto safeGeometryOffset = pixel.find(safeGeometryDefinition);
        const auto geometryOffset = pixel.find(
            "const float3 geometryNormalWS = NLSOrientGeometryNormal("
            "interpolatedGeometryNormalWS, isFrontFace);");
        const auto normalMapCondition = pixel.find(shader.normalMapCondition);
        const auto constrainOffset = pixel.find(
            "NLSConstrainShadingNormalToGeometryHemisphere(");
        ASSERT_NE(safeGeometryOffset, std::string::npos);
        ASSERT_NE(geometryOffset, std::string::npos);
        ASSERT_NE(normalMapCondition, std::string::npos);
        ASSERT_NE(constrainOffset, std::string::npos);
        EXPECT_LT(safeGeometryOffset, geometryOffset);
        EXPECT_LT(geometryOffset, normalMapCondition);
        EXPECT_LT(normalMapCondition, constrainOffset);
        EXPECT_NE(pixel.find("shadingNormalWS = geometryNormalWS;"), std::string::npos)
            << "The no-normal-map path must use the exact geometry normal value.";

        const auto lightingArguments = ExtractCallArguments(
            pixel,
            "NLSAccumulateClusteredLightingPBR");
        ASSERT_GE(lightingArguments.size(), 6u);
        EXPECT_EQ(lightingArguments[3], shader.positionName);
        EXPECT_EQ(lightingArguments[4], "geometryNormalWS");
        EXPECT_EQ(lightingArguments[5], "shadingNormalWS");
        EXPECT_NE(pixel.find("lighting + emissive"), std::string::npos);
        EXPECT_EQ(pixel.find("geometryFade"), std::string::npos);

        const auto orientFrame = source.find("NLSOrientTangentFrameForFace(");
        const auto applyNormalMap = source.find("NLSApplyTangentNormal(");
        ASSERT_NE(orientFrame, std::string::npos);
        ASSERT_NE(applyNormalMap, std::string::npos);
        EXPECT_LT(orientFrame, applyNormalMap);
    }
}

TEST(PBRShadingContractTests, SharedNormalShaderDefinesPublicMathAndFallbackContracts)
{
    const auto shaderPath = PBRNormalsPath();
    std::ifstream input(shaderPath, std::ios::binary);
    ASSERT_TRUE(input.is_open()) << "Failed to open source file: " << shaderPath.string();

    const std::string source{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    const std::multiset<std::string> expectedEntryPoints{
        "NLSOrientGeometryNormal",
        "NLSConstrainShadingNormalToGeometryHemisphere",
        "NLSOctEncodeNormal",
        "NLSOctDecodeNormal",
        "NLSGeometryHorizonFade"};
    EXPECT_EQ(FindNLSFunctionDefinitions(source), expectedEntryPoints);

    EXPECT_NE(source.find("#include \"Common.hlsl\""), std::string::npos);
    EXPECT_NE(
        source.find("return isFrontFace ? geometryNormalWS : -geometryNormalWS;"),
        std::string::npos);
    EXPECT_EQ(
        CountOccurrences(
            source,
            "NLSSafeNormalize(normalWS, float3(0.0f, 0.0f, 1.0f))"),
        2u);
    EXPECT_EQ(
        CountOccurrences(
            source,
            "NLSSafeNormalize(geometryNormalWS, float3(0.0f, 0.0f, 1.0f))"),
        1u);
    EXPECT_NE(
        source.find("shadingNormalWS = NLSSafeNormalize(shadingNormalWS, orientedGeometryNormalWS);"),
        std::string::npos);
    EXPECT_NE(
        source.find(
            "min(0.0f, dot(shadingNormalWS, orientedGeometryNormalWS)) * "
            "orientedGeometryNormalWS"),
        std::string::npos);
    EXPECT_NE(
        source.find("return NLSSafeNormalize(shadingNormalWS, orientedGeometryNormalWS);"),
        std::string::npos);
    EXPECT_EQ(
        CountOccurrences(
            source,
            "NLSSafeNormalize(shadingNormalWS, orientedGeometryNormalWS)"),
        2u);
    EXPECT_EQ(
        CountOccurrences(source, "NLSSafeNormalize(normal, float3(0.0f, 0.0f, 1.0f))"),
        1u);
    EXPECT_EQ(CountOccurrences(source, "NLSSafeNormalize("), 6u);

    EXPECT_NE(
        source.find(
            "float2 encoded = normal.xy / (abs(normal.x) + abs(normal.y) + abs(normal.z));"),
        std::string::npos);
    EXPECT_NE(source.find("if (normal.z < 0.0f)"), std::string::npos);
    EXPECT_NE(
        source.find(
            "encoded.x >= 0.0f ? 1.0f : -1.0f,\n"
            "            encoded.y >= 0.0f ? 1.0f : -1.0f"),
        std::string::npos);
    EXPECT_NE(
        source.find("encoded = (1.0f - abs(encoded.yx)) * signs;"),
        std::string::npos);
    EXPECT_NE(
        source.find(
            "float3 normal = float3(encoded, 1.0f - abs(encoded.x) - abs(encoded.y));"),
        std::string::npos);
    EXPECT_NE(source.find("const float fold = saturate(-normal.z);"), std::string::npos);
    EXPECT_NE(
        source.find(
            "normal.xy += float2(\n"
            "        normal.x >= 0.0f ? -fold : fold,\n"
            "        normal.y >= 0.0f ? -fold : fold);"),
        std::string::npos);
    EXPECT_NE(source.find("smoothstep(0.0f, 0.10f, ndotDirection)"), std::string::npos);
}

TEST(PBRShadingContractTests, NLSFunctionDefinitionMatcherRecognizesAllHlslReturnTypes)
{
    const std::string source =
        "static bool NLSBool(float value) { return value > 0.0f; }\n"
        "inline float4 NLSFloat4() { return 0.0f; }\n"
        "void NLSVoid() { }\n"
        "CustomResult NLSCustomType() { return (CustomResult)0; }\n";
    const std::multiset<std::string> expected{
        "NLSBool",
        "NLSFloat4",
        "NLSVoid",
        "NLSCustomType"};
    EXPECT_EQ(FindNLSFunctionDefinitions(source), expected);
}

TEST(PBRShadingContractTests, SharedSafeNormalizeHasOneImplementationAndNormalizesFallback)
{
    const auto commonTypesSource = ReadTextFile(ShaderRootPath() / "CommonTypes.hlsli");
    const auto shaderLibraryCommonSource = ReadTextFile(
        ShaderRootPath() / "NullusShaderLibrary/Common.hlsl");

    EXPECT_NE(
        commonTypesSource.find("#include \"NullusShaderLibrary/Common.hlsl\""),
        std::string::npos);
    const auto commonTypesDefinitions = FindNLSFunctionDefinitions(commonTypesSource);
    EXPECT_EQ(commonTypesDefinitions.count("NLSIsFinite3"), 0u);
    EXPECT_EQ(commonTypesDefinitions.count("NLSNormalizeFallback"), 0u);
    EXPECT_EQ(commonTypesDefinitions.count("NLSSafeNormalize"), 0u);

    const auto shaderLibraryDefinitions = FindNLSFunctionDefinitions(shaderLibraryCommonSource);
    EXPECT_EQ(shaderLibraryDefinitions.count("NLSIsFinite3"), 1u);
    EXPECT_EQ(shaderLibraryDefinitions.count("NLSNormalizeFallback"), 1u);
    EXPECT_EQ(shaderLibraryDefinitions.count("NLSSafeNormalize"), 1u);
    const auto safeNormalizeDefinition = ExtractFunctionDefinition(
        shaderLibraryCommonSource,
        "NLSSafeNormalize");
    EXPECT_NE(
        safeNormalizeDefinition.find("return NLSNormalizeFallback(fallback);"),
        std::string::npos);
}

TEST(PBRShadingContractTests, SharedNormalShaderCompilesThroughNativeDxcForDxilAndSpirv)
{
    ScopedTemporaryDirectory temporaryDirectory;
    const auto wrapperPath = temporaryDirectory.GetPath() / "PBRNormalsContract.hlsl";
    std::ofstream wrapper(wrapperPath, std::ios::binary);
    ASSERT_TRUE(wrapper.is_open()) << wrapperPath.string();
    wrapper
        << "#include \"PBRNormals.hlsl\"\n"
        << "float4 PSMain(float4 position : SV_Position) : SV_Target0\n"
        << "{\n"
        << "    const float3 geometryNormal = NLSOrientGeometryNormal(float3(0.0f, 0.0f, 1.0f), true);\n"
        << "    const float3 shadingNormal = NLSConstrainShadingNormalToGeometryHemisphere(float3(1.0f, 0.0f, 1.0f), geometryNormal);\n"
        << "    const float2 encoded = NLSOctEncodeNormal(shadingNormal);\n"
        << "    const float3 decoded = NLSOctDecodeNormal(encoded);\n"
        << "    const float fade = NLSGeometryHorizonFade(dot(decoded, geometryNormal));\n"
        << "    return float4(decoded * fade, 1.0f);\n"
        << "}\n";
    wrapper.close();

    struct CompileTarget
    {
        NLS::Render::ShaderCompiler::ShaderTargetPlatform platform;
        std::string_view extension;
    };
    const std::array targets{
        CompileTarget{NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL, ".dxil"},
        CompileTarget{NLS::Render::ShaderCompiler::ShaderTargetPlatform::SPIRV, ".spv"}};

    NLS::Render::ShaderCompiler::ShaderCompiler compiler;
    for (const auto& target : targets)
    {
        NLS::Render::ShaderCompiler::ShaderCompilationInput input;
        input.assetPath = wrapperPath.string();
        input.stage = NLS::Render::ShaderCompiler::ShaderStage::Pixel;
        input.options.targetPlatform = target.platform;
        input.options.targetProfile = "ps_6_0";
        input.options.entryPoint = "PSMain";
        input.options.treatWarningsAsErrors = true;
        input.options.artifactDirectory = (temporaryDirectory.GetPath() / "Artifacts").string();
        input.options.includeDirectories.push_back(PBRNormalsPath().parent_path().string());

        const auto output = compiler.Compile(input);
        if (output.status != NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded &&
            IsDxcUnavailableDiagnostic(output.diagnostics))
        {
            GTEST_SKIP() << "Native DXC is unavailable for PBR normal contract compilation: "
                         << output.diagnostics;
        }

        ASSERT_EQ(output.status, NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded)
            << output.diagnostics;
        EXPECT_FALSE(output.bytecode.empty());
        EXPECT_EQ(std::filesystem::path(output.artifactPath).extension(), target.extension);
        EXPECT_TRUE(HasDependency(output.dependencyPaths, PBRNormalsPath()));
        EXPECT_TRUE(HasDependency(output.dependencyPaths, PBRNormalsPath().parent_path() / "Common.hlsl"));
    }
}

TEST(PBRShadingContractTests, SafeNormalizeSemanticsAreIndependentOfPbrIncludeOrder)
{
    ScopedTemporaryDirectory temporaryDirectory;
    const auto wrapperPath = temporaryDirectory.GetPath() / "PBRSafeNormalizeSemantics.hlsl";

    struct NormalCase
    {
        std::string_view name;
        std::string_view value;
        std::string_view fallback;
        Vector3 cpuValue;
        Vector3 cpuFallback;
        bool expectConstantDxilOutput;
        bool expectCanonicalSpirvOutput;
        bool expectGuardedDxilOutput;
    };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    const std::array cases{
        NormalCase{"NonUnitFallback", "0.0f.xxx", "float3(0.0f, 0.0f, 2.0f)", Vector3::Zero, {0.0f, 0.0f, 2.0f}, true, false, false},
        NormalCase{"ZeroFallback", "0.0f.xxx", "0.0f.xxx", Vector3::Zero, Vector3::Zero, true, false, false},
        NormalCase{"NanInput", "float3(asfloat(0x7fc00000u), 0.0f, 0.0f)", "float3(0.0f, 0.0f, 2.0f)", {nan, 0.0f, 0.0f}, {0.0f, 0.0f, 2.0f}, true, false, false},
        NormalCase{"InfinityInput", "float3(asfloat(0x7f800000u), 0.0f, 0.0f)", "float3(0.0f, 0.0f, 2.0f)", {infinity, 0.0f, 0.0f}, {0.0f, 0.0f, 2.0f}, false, false, false},
        NormalCase{"NearZeroInput", "float3(1.0e-12f, 0.0f, 0.0f)", "float3(0.0f, 0.0f, 2.0f)", {1.0e-12f, 0.0f, 0.0f}, {0.0f, 0.0f, 2.0f}, true, false, false},
        NormalCase{"NanFallback", "0.0f.xxx", "float3(asfloat(0x7fc00000u), 0.0f, 0.0f)", Vector3::Zero, {nan, 0.0f, 0.0f}, true, true, false},
        NormalCase{"InfinityFallback", "0.0f.xxx", "float3(asfloat(0x7f800000u), 0.0f, 0.0f)", Vector3::Zero, {infinity, 0.0f, 0.0f}, false, true, true},
        NormalCase{"NearZeroFallback", "0.0f.xxx", "float3(1.0e-12f, 0.0f, 0.0f)", Vector3::Zero, {1.0e-12f, 0.0f, 0.0f}, true, true, false}};

    struct IncludeOrder
    {
        std::string_view name;
        std::string_view first;
        std::string_view second;
    };
    const std::array includeOrders{
        IncludeOrder{"CommonTypesFirst", "CommonTypes.hlsli", "LightGridCommon.hlsli"},
        IncludeOrder{"LightGridFirst", "LightGridCommon.hlsli", "CommonTypes.hlsli"}};

    struct CompileTarget
    {
        NLS::Render::ShaderCompiler::ShaderTargetPlatform platform;
        std::string_view name;
        std::string_view extension;
    };
    const std::array targets{
        CompileTarget{NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL, "DXIL", ".dxil"},
        CompileTarget{NLS::Render::ShaderCompiler::ShaderTargetPlatform::SPIRV, "SPIRV", ".spv"}};

    NLS::Render::ShaderCompiler::ShaderCompiler compiler;
    const auto dxcPath = NLS::Render::ShaderCompiler::GetCurrentShaderCompilerToolchainIdentity().compilerPath;
    ASSERT_FALSE(dxcPath.empty());
    for (const auto& normalCase : cases)
    {
        const auto expected = SafeNormalize(normalCase.cpuValue, normalCase.cpuFallback);
        EXPECT_TRUE(IsFinite(expected)) << normalCase.name;
        EXPECT_NEAR(Dot(expected, expected), 1.0f, kNormalTolerance) << normalCase.name;
        EXPECT_NEAR(expected.x, 0.0f, kNormalTolerance) << normalCase.name;
        EXPECT_NEAR(expected.y, 0.0f, kNormalTolerance) << normalCase.name;
        EXPECT_NEAR(expected.z, 1.0f, kNormalTolerance) << normalCase.name;

        for (const auto& target : targets)
        {
            std::array<NLS::Render::ShaderCompiler::ShaderCompilationOutput, 2u> outputs;
            for (size_t orderIndex = 0u; orderIndex < includeOrders.size(); ++orderIndex)
            {
                const auto& includeOrder = includeOrders[orderIndex];
                SCOPED_TRACE(std::string(normalCase.name) + "/" + std::string(target.name) + "/" + std::string(includeOrder.name));

                std::ofstream wrapper(wrapperPath, std::ios::binary | std::ios::trunc);
                ASSERT_TRUE(wrapper.is_open()) << wrapperPath.string();
                wrapper
                    << "#include \"" << includeOrder.first << "\"\n"
                    << "#include \"" << includeOrder.second << "\"\n"
                    << "float4 PSMain(float4 position : SV_Position) : SV_Target0\n"
                    << "{\n"
                    << "    return float4(NLSSafeNormalize(" << normalCase.value << ", " << normalCase.fallback << "), 1.0f);\n"
                    << "}\n";
                wrapper.close();

                const auto input = MakeNativeShaderCompileInput(
                    wrapperPath,
                    NLS::Render::ShaderCompiler::ShaderStage::Pixel,
                    target.platform,
                    temporaryDirectory.GetPath() / "SafeNormalizeArtifacts" / std::string(normalCase.name) / std::string(includeOrder.name));
                outputs[orderIndex] = compiler.Compile(input);
                const auto& output = outputs[orderIndex];
                ASSERT_EQ(output.status, NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded)
                    << output.diagnostics;
                EXPECT_FALSE(output.bytecode.empty());
                EXPECT_EQ(std::filesystem::path(output.artifactPath).extension(), target.extension);
                EXPECT_TRUE(HasDependency(output.dependencyPaths, ShaderRootPath() / "LightGridCommon.hlsli"));
                EXPECT_TRUE(HasDependency(output.dependencyPaths, ShaderRootPath() / "CommonTypes.hlsli"));
                EXPECT_TRUE(HasDependency(output.dependencyPaths, PBRNormalsPath()));

                if (target.platform == NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL &&
                    (normalCase.expectConstantDxilOutput || normalCase.expectGuardedDxilOutput))
                {
                    const auto dump = NLS::Render::ShaderCompiler::ExecuteShaderCompilerProcess(
                        dxcPath,
                        {"-dumpbin", output.artifactPath});
                    ASSERT_EQ(dump.status, NLS::Render::ShaderCompiler::ShaderProcessStatus::Succeeded)
                        << dump.diagnostics;
                    if (normalCase.expectConstantDxilOutput)
                    {
                        EXPECT_NE(dump.output.find("i8 0, float 0.000000e+00"), std::string::npos);
                        EXPECT_NE(dump.output.find("i8 1, float 0.000000e+00"), std::string::npos);
                        EXPECT_NE(dump.output.find("i8 2, float 1.000000e+00"), std::string::npos);
                        EXPECT_NE(dump.output.find("i8 3, float 1.000000e+00"), std::string::npos);
                    }
                    else
                    {
                        ExpectGuardedDxilReturnsFiniteUnitPositiveZ(dump.output);
                    }
                }
                else if (target.platform == NLS::Render::ShaderCompiler::ShaderTargetPlatform::SPIRV &&
                    normalCase.expectCanonicalSpirvOutput)
                {
                    ExpectSpirvReturnsFiniteUnitPositiveZ(
                        dxcPath,
                        wrapperPath,
                        temporaryDirectory.GetPath() / "SafeNormalizeAssembly" /
                            std::string(normalCase.name) / std::string(includeOrder.name));
                }
            }

            EXPECT_EQ(outputs[0].bytecode, outputs[1].bytecode)
                << normalCase.name << "/" << target.name;
        }
    }
}

TEST(PBRShadingContractTests, ForwardShadersCompileThroughNativeDxcForDxilSpirvAndShaderLabVariants)
{
    struct CompileTarget
    {
        NLS::Render::ShaderCompiler::ShaderTargetPlatform platform;
        std::string_view extension;
    };
    const std::array targets{
        CompileTarget{NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL, ".dxil"},
        CompileTarget{NLS::Render::ShaderCompiler::ShaderTargetPlatform::SPIRV, ".spv"}};

    ScopedTemporaryDirectory temporaryDirectory;
    NLS::Render::ShaderCompiler::ShaderCompiler compiler;
    const auto builtInPath = ShaderRootPath() / "StandardPBR.hlsl";
    for (const auto& target : targets)
    {
        for (const auto stage : {
                 NLS::Render::ShaderCompiler::ShaderStage::Vertex,
                 NLS::Render::ShaderCompiler::ShaderStage::Pixel})
        {
            const auto input = MakeNativeShaderCompileInput(
                builtInPath,
                stage,
                target.platform,
                temporaryDirectory.GetPath() / "BuiltInArtifacts");

            const auto output = compiler.Compile(input);
            if (output.status != NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded &&
                IsDxcUnavailableDiagnostic(output.diagnostics))
            {
                GTEST_SKIP() << "Native DXC is unavailable for Forward PBR compilation: "
                             << output.diagnostics;
            }
            ASSERT_EQ(output.status, NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded)
                << output.diagnostics;
            EXPECT_FALSE(output.bytecode.empty());
            EXPECT_EQ(std::filesystem::path(output.artifactPath).extension(), target.extension);
            EXPECT_TRUE(HasDependency(output.dependencyPaths, builtInPath));
            EXPECT_TRUE(HasDependency(output.dependencyPaths, PBRNormalsPath()));
        }
    }

    const auto shaderLabPath = ShaderRootPath() / "ShaderLab/StandardPBR.shader";
    const auto shaderLabSource = ReadTextFile(shaderLabPath);
    ASSERT_FALSE(shaderLabSource.empty());
    const auto parsed = NLS::Render::ShaderLab::ParseShaderLabSource(
        shaderLabSource,
        shaderLabPath.generic_string());
    ASSERT_TRUE(parsed.Succeeded()) << parsed.DiagnosticsToString();
    ASSERT_FALSE(parsed.asset.subShaders.empty());
    ASSERT_FALSE(parsed.asset.subShaders.front().passes.empty());
    const auto& parsedForward = parsed.asset.subShaders.front().passes.front();
    ASSERT_EQ(parsedForward.state.cullMode, NLS::Render::ShaderLab::ShaderLabCullMode::Back);

    struct ShaderLabVariant
    {
        std::string_view name;
        bool normalMap;
        NLS::Render::ShaderLab::ShaderLabCullMode cullMode;
    };
    const std::array variants{
        ShaderLabVariant{"Default", false, NLS::Render::ShaderLab::ShaderLabCullMode::Back},
        ShaderLabVariant{"NormalMap", true, NLS::Render::ShaderLab::ShaderLabCullMode::Back},
        ShaderLabVariant{"CullOff", false, NLS::Render::ShaderLab::ShaderLabCullMode::Off}};

    for (const auto& variant : variants)
    {
        auto forward = parsedForward;
        forward.state.cullMode = variant.cullMode;
        ASSERT_EQ(forward.state.cullMode, variant.cullMode);

        const auto compileSourcePath =
            temporaryDirectory.GetPath() /
            ("StandardPBRForward" + std::string(variant.name) + ".hlsl");
        std::ofstream compileSource(compileSourcePath, std::ios::binary);
        ASSERT_TRUE(compileSource.is_open()) << compileSourcePath.string();
        compileSource << NLS::Render::ShaderLab::BuildShaderLabHlslForCompile(forward);
        compileSource.close();

        for (const auto& target : targets)
        {
            for (const auto stage : {
                     NLS::Render::ShaderCompiler::ShaderStage::Vertex,
                     NLS::Render::ShaderCompiler::ShaderStage::Pixel})
            {
                if (stage == NLS::Render::ShaderCompiler::ShaderStage::Vertex &&
                    variant.name != "Default")
                {
                    continue;
                }

                auto input = MakeNativeShaderCompileInput(
                    compileSourcePath,
                    stage,
                    target.platform,
                    temporaryDirectory.GetPath() / "ShaderLabArtifacts");
                if (variant.normalMap)
                    input.options.macros.push_back({"_NORMALMAP", "1"});

                const auto output = compiler.Compile(input);
                if (output.status != NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded &&
                    IsDxcUnavailableDiagnostic(output.diagnostics))
                {
                    GTEST_SKIP() << "Native DXC is unavailable for ShaderLab Forward PBR compilation: "
                                 << output.diagnostics;
                }
                ASSERT_EQ(output.status, NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded)
                    << variant.name << " " << output.diagnostics;
                EXPECT_FALSE(output.bytecode.empty()) << variant.name;
                EXPECT_EQ(std::filesystem::path(output.artifactPath).extension(), target.extension);
                EXPECT_TRUE(HasDependency(output.dependencyPaths, PBRNormalsPath()));
            }
        }
    }
}
