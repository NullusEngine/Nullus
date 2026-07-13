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
    return fallback;
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
