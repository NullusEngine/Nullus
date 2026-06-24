#include <gtest/gtest.h>

#include "Rendering/ShaderLab/ShaderLabParser.h"

#include <string>

namespace
{
    const char* kMultiPassShader = R"(Shader "Nullus/Test"
{
    Properties
    {
        _BaseMap("Base Map", Texture2D) = "white"
        _BaseColor("Base Color", Color) = (1, 0.5, 0.25, 1)
        _Cutoff("Alpha Cutoff", Range(0, 1)) = 0.5
        _Mode("Mode", Int) = 2
    }

    SubShader
    {
        Tags
        {
            "RenderPipeline" = "Nullus"
            "Queue" = "Geometry"
        }

        Pass
        {
            Name "Forward"
            Tags { "LightMode" = "Forward" }
            Cull Back
            ZWrite On
            ZTest LessEqual
            Blend Off

            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            #pragma shader_feature _ALPHATEST_ON
            #pragma multi_compile _ MAIN_LIGHT_SHADOWS
            float4 PSMain() : SV_Target0 { return 1.xxxx; }
            ENDHLSL
        }

        Pass
        {
            Name "DepthOnly"
            Tags { "LightMode" = "DepthOnly" }
            Cull Off
            ZWrite On
            ZTest Less
            Blend SrcAlpha OneMinusSrcAlpha

            HLSLPROGRAM
            #pragma vertex DepthVS
            #pragma fragment DepthPS
            float4 DepthPS() : SV_Target0 { return 0.xxxx; }
            ENDHLSL
        }
    }

    Fallback "Hidden/Error"
})";
}

TEST(ShaderLabParserTests, ParsesShaderNamePropertiesTagsPassesStateAndPragmas)
{
    const auto result = NLS::Render::ShaderLab::ParseShaderLabSource(kMultiPassShader, "Assets/Test.shader");

    ASSERT_TRUE(result.Succeeded()) << result.DiagnosticsToString();
    EXPECT_EQ(result.asset.shaderName, "Nullus/Test");
    EXPECT_EQ(result.asset.fallbackShader, "Hidden/Error");
    ASSERT_EQ(result.asset.properties.size(), 4u);
    EXPECT_EQ(result.asset.properties[0].name, "_BaseMap");
    EXPECT_EQ(result.asset.properties[0].displayName, "Base Map");
    EXPECT_EQ(result.asset.properties[0].type, NLS::Render::ShaderLab::ShaderLabPropertyType::Texture2D);
    EXPECT_EQ(std::get<std::string>(result.asset.properties[0].defaultValue), "white");
    EXPECT_EQ(result.asset.properties[1].type, NLS::Render::ShaderLab::ShaderLabPropertyType::Color);
    EXPECT_FLOAT_EQ(std::get<NLS::Render::ShaderLab::ShaderLabFloat4>(result.asset.properties[1].defaultValue)[1], 0.5f);
    EXPECT_EQ(result.asset.properties[2].type, NLS::Render::ShaderLab::ShaderLabPropertyType::Range);
    EXPECT_FLOAT_EQ(result.asset.properties[2].rangeMin, 0.0f);
    EXPECT_FLOAT_EQ(result.asset.properties[2].rangeMax, 1.0f);
    EXPECT_EQ(std::get<int32_t>(result.asset.properties[3].defaultValue), 2);

    ASSERT_EQ(result.asset.subShaders.size(), 1u);
    EXPECT_EQ(result.asset.subShaders[0].tags.values.at("RenderPipeline"), "Nullus");
    EXPECT_EQ(result.asset.subShaders[0].tags.values.at("Queue"), "Geometry");
    ASSERT_EQ(result.asset.subShaders[0].passes.size(), 2u);

    const auto& forward = result.asset.subShaders[0].passes[0];
    EXPECT_EQ(forward.name, "Forward");
    EXPECT_EQ(forward.tags.values.at("LightMode"), "Forward");
    EXPECT_EQ(forward.state.cullMode, NLS::Render::ShaderLab::ShaderLabCullMode::Back);
    EXPECT_TRUE(forward.state.depthWrite);
    EXPECT_EQ(forward.state.depthCompare, NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL);
    EXPECT_FALSE(forward.state.blend.enabled);
    EXPECT_EQ(forward.vertexEntry, "VSMain");
    EXPECT_EQ(forward.fragmentEntry, "PSMain");
    ASSERT_EQ(forward.shaderFeatures.size(), 1u);
    EXPECT_EQ(forward.shaderFeatures[0].keywords, std::vector<std::string>({"_ALPHATEST_ON"}));
    ASSERT_EQ(forward.multiCompiles.size(), 1u);
    EXPECT_EQ(forward.multiCompiles[0].keywords, std::vector<std::string>({"_", "MAIN_LIGHT_SHADOWS"}));
    EXPECT_NE(forward.hlslSource.find("float4 PSMain()"), std::string::npos);
    EXPECT_EQ(forward.hlslSource.find("HLSLPROGRAM"), std::string::npos);
    EXPECT_EQ(forward.hlslSource.find("ENDHLSL"), std::string::npos);

    const auto& depth = result.asset.subShaders[0].passes[1];
    EXPECT_EQ(depth.name, "DepthOnly");
    EXPECT_EQ(depth.tags.values.at("LightMode"), "DepthOnly");
    EXPECT_EQ(depth.state.cullMode, NLS::Render::ShaderLab::ShaderLabCullMode::Off);
    EXPECT_EQ(depth.state.depthCompare, NLS::Render::Settings::EComparaisonAlgorithm::LESS);
    ASSERT_EQ(depth.state.blend.renderTargets.size(), 1u);
    EXPECT_TRUE(depth.state.blend.enabled);
    EXPECT_TRUE(depth.state.blend.renderTargets[0].blendEnable);
}

TEST(ShaderLabParserTests, PrependsLineDirectiveToCompilerSource)
{
    const auto result = NLS::Render::ShaderLab::ParseShaderLabSource(kMultiPassShader, "Assets/Test.shader");

    ASSERT_TRUE(result.Succeeded()) << result.DiagnosticsToString();
    const auto compilerSource = NLS::Render::ShaderLab::BuildShaderLabHlslForCompile(
        result.asset.subShaders[0].passes[0]);

    EXPECT_NE(compilerSource.find("#line 29 \"Assets/Test.shader\""), std::string::npos);
    EXPECT_LT(compilerSource.find("#line"), compilerSource.find("#pragma vertex VSMain"));
}

TEST(ShaderLabParserTests, ReportsDuplicatePropertiesWithLocation)
{
    const auto result = NLS::Render::ShaderLab::ParseShaderLabSource(R"(Shader "Broken"
{
    Properties
    {
        _Color("Color", Color) = (1, 1, 1, 1)
        _Color("Duplicate", Color) = (0, 0, 0, 1)
    }
})", "Broken.shader");

    ASSERT_FALSE(result.Succeeded());
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_NE(result.diagnostics.front().message.find("duplicate property"), std::string::npos);
    EXPECT_EQ(result.diagnostics.front().location.line, 6u);
    EXPECT_EQ(result.diagnostics.front().location.column, 9u);
}

TEST(ShaderLabParserTests, ReportsUnsupportedPropertyType)
{
    const auto result = NLS::Render::ShaderLab::ParseShaderLabSource(R"(Shader "Broken"
{
    Properties
    {
        _Bad("Bad", Matrix) = (1, 0, 0, 1)
    }
})", "Broken.shader");

    ASSERT_FALSE(result.Succeeded());
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_NE(result.diagnostics.front().message.find("unsupported property type"), std::string::npos);
    EXPECT_EQ(result.diagnostics.front().location.line, 5u);
}

TEST(ShaderLabParserTests, ReportsMissingEndHlslAtRawBlockLocation)
{
    const auto result = NLS::Render::ShaderLab::ParseShaderLabSource(R"(Shader "Broken"
{
    SubShader
    {
        Pass
        {
            HLSLPROGRAM
            float4 PSMain() : SV_Target0 { return 1.xxxx; }
        }
    }
})", "Broken.shader");

    ASSERT_FALSE(result.Succeeded());
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_NE(result.diagnostics.front().message.find("ENDHLSL"), std::string::npos);
    EXPECT_EQ(result.diagnostics.front().location.line, 7u);
    EXPECT_EQ(result.diagnostics.front().location.column, 13u);
}

TEST(ShaderLabParserTests, ReportsUnmatchedBraceWithLineAndColumn)
{
    const auto result = NLS::Render::ShaderLab::ParseShaderLabSource(R"(Shader "Broken"
{
    SubShader
    {
        Pass
        {
            Name "Forward"
        }
)", "Broken.shader");

    ASSERT_FALSE(result.Succeeded());
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_NE(result.DiagnosticsToString().find("expected '}'"), std::string::npos);
    EXPECT_NE(result.DiagnosticsToString().find("Broken.shader"), std::string::npos);
}

TEST(ShaderLabParserTests, ReportsInvalidRenderStateValuesWithLineAndColumn)
{
    const auto result = NLS::Render::ShaderLab::ParseShaderLabSource(R"(Shader "Broken"
{
    SubShader
    {
        Pass
        {
            Cull Sideways
            ZWrite Maybe
            ZTest Nearish
            Blend SrcAlpha Mystery
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            float4 PSMain() : SV_Target0 { return 1.xxxx; }
            ENDHLSL
        }
    }
})", "Broken.shader");

    ASSERT_FALSE(result.Succeeded());
    const auto diagnostics = result.DiagnosticsToString();
    EXPECT_NE(diagnostics.find("unsupported Cull mode 'Sideways'"), std::string::npos);
    EXPECT_NE(diagnostics.find("unsupported ZWrite mode 'Maybe'"), std::string::npos);
    EXPECT_NE(diagnostics.find("unsupported ZTest mode 'Nearish'"), std::string::npos);
    EXPECT_NE(diagnostics.find("unsupported Blend factor 'Mystery'"), std::string::npos);
    EXPECT_NE(diagnostics.find("Broken.shader("), std::string::npos);
}

TEST(ShaderLabParserTests, ParsesNumericZWriteModes)
{
    const auto result = NLS::Render::ShaderLab::ParseShaderLabSource(R"(Shader "NumericZWrite"
{
    SubShader
    {
        Pass
        {
            Name "DepthOff"
            ZWrite 0
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            ENDHLSL
        }

        Pass
        {
            Name "DepthOn"
            ZWrite 1
            HLSLPROGRAM
            #pragma vertex VSMain
            #pragma fragment PSMain
            ENDHLSL
        }
    }
})", "NumericZWrite.shader");

    ASSERT_TRUE(result.Succeeded()) << result.DiagnosticsToString();
    ASSERT_EQ(result.asset.subShaders[0].passes.size(), 2u);
    EXPECT_FALSE(result.asset.subShaders[0].passes[0].state.depthWrite);
    EXPECT_TRUE(result.asset.subShaders[0].passes[1].state.depthWrite);
}

TEST(ShaderLabParserTests, ParsesNegativeNumbersAndRejectsMalformedNumbers)
{
    const auto negativeResult = NLS::Render::ShaderLab::ParseShaderLabSource(R"(Shader "Numbers"
{
    Properties
    {
        _Bias("Bias", Range(-1, 1)) = -0.25
        _Offset("Offset", Vector) = (-1, 0.5, -0.5, 1)
    }
})", "Numbers.shader");

    ASSERT_TRUE(negativeResult.Succeeded()) << negativeResult.DiagnosticsToString();
    EXPECT_FLOAT_EQ(negativeResult.asset.properties[0].rangeMin, -1.0f);
    EXPECT_FLOAT_EQ(std::get<float>(negativeResult.asset.properties[0].defaultValue), -0.25f);
    EXPECT_FLOAT_EQ(std::get<NLS::Render::ShaderLab::ShaderLabFloat4>(
        negativeResult.asset.properties[1].defaultValue)[0], -1.0f);

    const auto malformedResult = NLS::Render::ShaderLab::ParseShaderLabSource(R"(Shader "Broken"
{
    Properties
    {
        _Bad("Bad", Float) = 1e999
    }
})", "Broken.shader");

    ASSERT_FALSE(malformedResult.Succeeded());
    EXPECT_NE(malformedResult.DiagnosticsToString().find("invalid numeric literal '1e999'"), std::string::npos);
    EXPECT_NE(malformedResult.DiagnosticsToString().find("Broken.shader("), std::string::npos);
}

TEST(ShaderLabParserTests, EscapesLineDirectiveFilenameForCompilerSource)
{
    auto result = NLS::Render::ShaderLab::ParseShaderLabSource(
        kMultiPassShader,
        "D:\\VSProject\\Nullus\\Shaders\\Test \"Quoted\".shader");

    ASSERT_TRUE(result.Succeeded()) << result.DiagnosticsToString();
    const auto compilerSource = NLS::Render::ShaderLab::BuildShaderLabHlslForCompile(
        result.asset.subShaders[0].passes[0]);

    EXPECT_NE(
        compilerSource.find("#line 29 \"D:/VSProject/Nullus/Shaders/Test \\\"Quoted\\\".shader\""),
        std::string::npos);
}

TEST(ShaderLabParserTests, IgnoresCommentedAndStringContainedPragmas)
{
    const auto result = NLS::Render::ShaderLab::ParseShaderLabSource(R"(Shader "Pragmas"
{
    SubShader
    {
        Pass
        {
            HLSLPROGRAM
            // #pragma vertex WrongVS
            /* #pragma fragment WrongPS */
            static const char* text = "#pragma multi_compile BAD";
                #pragma vertex VSMain
                #pragma fragment PSMain
                #pragma shader_feature _USED
            ENDHLSL
        }
    }
})", "Pragmas.shader");

    ASSERT_TRUE(result.Succeeded()) << result.DiagnosticsToString();
    const auto& pass = result.asset.subShaders[0].passes[0];
    EXPECT_EQ(pass.vertexEntry, "VSMain");
    EXPECT_EQ(pass.fragmentEntry, "PSMain");
    ASSERT_EQ(pass.shaderFeatures.size(), 1u);
    EXPECT_EQ(pass.shaderFeatures[0].keywords, std::vector<std::string>({"_USED"}));
    EXPECT_TRUE(pass.multiCompiles.empty());
}

TEST(ShaderLabParserTests, ReportsUnterminatedStringLiteral)
{
    const auto result = NLS::Render::ShaderLab::ParseShaderLabSource(R"(Shader "Broken"
{
    Properties
    {
        _Name("Name, Float) = 1
    }
})", "Broken.shader");

    ASSERT_FALSE(result.Succeeded());
    EXPECT_NE(result.DiagnosticsToString().find("unterminated string literal"), std::string::npos);
}

TEST(ShaderLabParserTests, RejectsNonIntegralIntDefaults)
{
    const auto result = NLS::Render::ShaderLab::ParseShaderLabSource(R"(Shader "Broken"
{
    Properties
    {
        _Mode("Mode", Int) = 1.9
    }
})", "Broken.shader");

    ASSERT_FALSE(result.Succeeded());
    EXPECT_EQ(result.asset.properties[0].type, NLS::Render::ShaderLab::ShaderLabPropertyType::Int);
    EXPECT_EQ(std::get<int32_t>(result.asset.properties[0].defaultValue), 0);
    EXPECT_NE(result.DiagnosticsToString().find("expected integer literal for Int default"), std::string::npos);
}

TEST(ShaderLabParserTests, ParsesLargeIntDefaultsWithoutFloatRounding)
{
    const auto result = NLS::Render::ShaderLab::ParseShaderLabSource(R"(Shader "LargeInt"
{
    Properties
    {
        _Mask("Mask", Int) = 16777217
    }
})", "LargeInt.shader");

    ASSERT_TRUE(result.Succeeded()) << result.DiagnosticsToString();
    EXPECT_EQ(std::get<int32_t>(result.asset.properties[0].defaultValue), 16777217);
}

TEST(ShaderLabParserTests, RejectsOutOfRangeIntDefaults)
{
    const auto result = NLS::Render::ShaderLab::ParseShaderLabSource(R"(Shader "Broken"
{
    Properties
    {
        _Mode("Mode", Int) = 2147483648
    }
})", "Broken.shader");

    ASSERT_FALSE(result.Succeeded());
    EXPECT_NE(result.DiagnosticsToString().find("integer literal out of Int range"), std::string::npos);
}

TEST(ShaderLabParserTests, ReportsDuplicateTagKeys)
{
    const auto result = NLS::Render::ShaderLab::ParseShaderLabSource(R"(Shader "Broken"
{
    SubShader
    {
        Pass
        {
            Tags
            {
                "LightMode" = "Forward"
                "LightMode" = "DepthOnly"
            }
        }
    }
})", "Broken.shader");

    ASSERT_FALSE(result.Succeeded());
    EXPECT_NE(result.DiagnosticsToString().find("duplicate tag 'LightMode'"), std::string::npos);
}
