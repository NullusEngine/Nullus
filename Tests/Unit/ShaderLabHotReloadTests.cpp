#include <gtest/gtest.h>

#include "Guid.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/Resources/ShaderReflection.h"
#include "Rendering/ShaderLab/ShaderLabAsset.h"
#include "Rendering/ShaderLab/ShaderLabHotReload.h"
#include "Rendering/ShaderLab/ShaderLabMaterial.h"

namespace
{
    NLS::Render::ShaderLab::ShaderLabAssetDesc MakeReloadDesc(std::string shaderName, std::string lightMode)
    {
        using namespace NLS::Render::ShaderLab;

        ShaderLabAssetDesc desc;
        desc.shaderName = std::move(shaderName);
        desc.properties.push_back({
            "_BaseColor",
            "Base Color",
            ShaderLabPropertyType::Color,
            ShaderLabFloat4{1.0f, 1.0f, 1.0f, 1.0f}
        });

        ShaderLabSubShaderDesc subShader;
        ShaderLabPassDesc pass;
        pass.name = lightMode;
        pass.tags.values["LightMode"] = lightMode;
        pass.vertexEntry = "VSMain";
        pass.fragmentEntry = "PSMain";
        subShader.passes.push_back(std::move(pass));
        desc.subShaders.push_back(std::move(subShader));
        return desc;
    }

    class TestReloadValidator final : public NLS::Render::ShaderLab::IShaderLabReloadValidator
    {
    public:
        bool shouldSucceed = true;
        int validationCalls = 0;

        NLS::Render::ShaderLab::ShaderLabReloadValidationResult Validate(
            const NLS::Render::ShaderLab::ShaderLabAssetDesc& candidate,
            const std::vector<NLS::Render::ShaderLab::ShaderLabVariantKey>& requestedVariants) override
        {
            ++validationCalls;
            lastShaderName = candidate.shaderName;
            lastVariantCount = requestedVariants.size();
            NLS::Render::ShaderLab::ShaderLabReloadValidationResult result;
            result.succeeded = shouldSucceed;
            if (!shouldSucceed)
                result.diagnostics.push_back({"compile failed", {"Reload.shader", 9u, 13u, 64u}});
            return result;
        }

        std::string lastShaderName;
        size_t lastVariantCount = 0u;
    };
}

TEST(ShaderLabHotReloadTests, SuccessfulReloadAtomicallyReplacesShaderAndMigratesMaterials)
{
    using namespace NLS::Render::ShaderLab;

    auto shader = std::make_shared<ShaderLabAsset>(
        NLS::Guid::NewDeterministic("reload-success"),
        MakeReloadDesc("Nullus/Old", "Forward"));
    auto material = std::make_shared<ShaderLabMaterial>(shader);
    ASSERT_TRUE(material->SetColor("_BaseColor", {0.2f, 0.3f, 0.4f, 1.0f}));

    TestReloadValidator validator;
    ShaderLabHotReloadService reloader(validator);
    ShaderLabReloadRequest request;
    request.shader = shader;
    request.dependentMaterials.push_back(material->AsReloadDependency());
    request.candidate = MakeReloadDesc("Nullus/New", "DepthOnly");

    const auto result = reloader.Reload(request);

    ASSERT_TRUE(result.succeeded) << result.DiagnosticsToString();
    EXPECT_EQ(shader->GetName(), "Nullus/New");
    EXPECT_FALSE(shader->FindPass(0, "Forward"));
    EXPECT_TRUE(shader->FindPass(0, "DepthOnly"));
    EXPECT_FLOAT_EQ(material->GetColor("_BaseColor").value()[0], 0.2f);
    EXPECT_EQ(result.invalidatedPipelineGeneration, shader->GetGeneration());
    EXPECT_EQ(validator.validationCalls, 1);
}

TEST(ShaderLabHotReloadTests, ExpiredMaterialDependenciesAreIgnoredDuringReload)
{
    using namespace NLS::Render::ShaderLab;

    auto shader = std::make_shared<ShaderLabAsset>(
        NLS::Guid::NewDeterministic("reload-expired-material"),
        MakeReloadDesc("Nullus/Old", "Forward"));
    ShaderLabMaterialReloadDependency expiredDependency;
    {
        auto material = std::make_shared<ShaderLabMaterial>(shader);
        expiredDependency = material->AsReloadDependency();
        EXPECT_TRUE(expiredDependency.IsAlive());
        EXPECT_NE(expiredDependency.Resolve(), nullptr);
    }
    EXPECT_FALSE(expiredDependency.IsAlive());
    EXPECT_EQ(expiredDependency.Resolve(), nullptr);

    TestReloadValidator validator;
    ShaderLabHotReloadService reloader(validator);
    ShaderLabReloadRequest request;
    request.shader = shader;
    request.dependentMaterials.push_back(expiredDependency);
    request.candidate = MakeReloadDesc("Nullus/New", "DepthOnly");

    const auto result = reloader.Reload(request);

    ASSERT_TRUE(result.succeeded) << result.DiagnosticsToString();
    EXPECT_EQ(shader->GetName(), "Nullus/New");
}

TEST(ShaderLabHotReloadTests, MaterialDependenciesFromOtherShadersAreSkipped)
{
    using namespace NLS::Render::ShaderLab;

    auto shaderA = std::make_shared<ShaderLabAsset>(
        NLS::Guid::NewDeterministic("reload-shader-a"),
        MakeReloadDesc("Nullus/A", "Forward"));
    auto shaderB = std::make_shared<ShaderLabAsset>(
        NLS::Guid::NewDeterministic("reload-shader-b"),
        MakeReloadDesc("Nullus/B", "Forward"));
    auto materialB = std::make_shared<ShaderLabMaterial>(shaderB);

    TestReloadValidator validator;
    ShaderLabHotReloadService reloader(validator);
    ShaderLabReloadRequest request;
    request.shader = shaderA;
    request.dependentMaterials.push_back(materialB->AsReloadDependency());
    request.candidate = MakeReloadDesc("Nullus/A_Reloaded", "DepthOnly");

    const auto result = reloader.Reload(request);

    ASSERT_TRUE(result.succeeded) << result.DiagnosticsToString();
    EXPECT_EQ(shaderA->GetName(), "Nullus/A_Reloaded");
    EXPECT_EQ(materialB->GetShader(), shaderB);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_NE(result.DiagnosticsToString().find("different shader"), std::string::npos);
}

TEST(ShaderLabHotReloadTests, StackAllocatedMaterialProducesExpiredReloadDependency)
{
    using namespace NLS::Render::ShaderLab;

    auto shader = std::make_shared<ShaderLabAsset>(
        NLS::Guid::NewDeterministic("reload-stack-material"),
        MakeReloadDesc("Nullus/Stack", "Forward"));
    ShaderLabMaterial material(shader);

    const auto dependency = material.AsReloadDependency();

    EXPECT_FALSE(dependency.IsAlive());
    EXPECT_EQ(dependency.Resolve(), nullptr);
}

TEST(ShaderLabHotReloadTests, FailedReloadKeepsOldShaderAndStoresDiagnostics)
{
    using namespace NLS::Render::ShaderLab;

    auto shader = std::make_shared<ShaderLabAsset>(
        NLS::Guid::NewDeterministic("reload-failure"),
        MakeReloadDesc("Nullus/Old", "Forward"));

    TestReloadValidator validator;
    validator.shouldSucceed = false;
    ShaderLabHotReloadService reloader(validator);
    ShaderLabReloadRequest request;
    request.shader = shader;
    request.candidate = MakeReloadDesc("Nullus/New", "DepthOnly");

    const auto result = reloader.Reload(request);

    ASSERT_FALSE(result.succeeded);
    EXPECT_EQ(shader->GetName(), "Nullus/Old");
    EXPECT_TRUE(shader->FindPass(0, "Forward"));
    EXPECT_FALSE(shader->FindPass(0, "DepthOnly"));
    ASSERT_FALSE(shader->GetLastDiagnostics().empty());
    EXPECT_NE(shader->GetLastDiagnostics().front().message.find("compile failed"), std::string::npos);
}

TEST(ShaderLabHotReloadTests, OldPassHandleDoesNotResolveAfterSuccessfulReload)
{
    using namespace NLS::Render::ShaderLab;

    auto shader = std::make_shared<ShaderLabAsset>(
        NLS::Guid::NewDeterministic("reload-handle"),
        MakeReloadDesc("Nullus/Old", "Forward"));
    const auto oldHandle = shader->GetPassHandle(0, "Forward");
    ASSERT_TRUE(oldHandle.IsValid());

    TestReloadValidator validator;
    ShaderLabHotReloadService reloader(validator);
    ShaderLabReloadRequest request;
    request.shader = shader;
    request.candidate = MakeReloadDesc("Nullus/New", "DepthOnly");

    ASSERT_TRUE(reloader.Reload(request).succeeded);
    EXPECT_FALSE(shader->ResolvePass(oldHandle));
}

TEST(ShaderLabHotReloadTests, RuntimeShaderOldReflectionReferenceSurvivesAtomicReplacement)
{
    using namespace NLS::Render::Resources;
    using namespace NLS::Render::ShaderCompiler;

    auto* shader = Shader::CreateForTesting("RuntimeReload.shader");
    ShaderReflection oldReflection;
    ShaderPropertyDesc oldProperty;
    oldProperty.name = "OldColor";
    oldProperty.type = UniformType::UNIFORM_FLOAT_VEC4;
    oldProperty.kind = ShaderResourceKind::Value;
    oldProperty.byteSize = 16u;
    oldReflection.properties.push_back(oldProperty);
    shader->SetReflectionForTesting(oldReflection);
    const auto oldReflectionSnapshot = shader->GetReflectionSnapshot();
    ASSERT_EQ(oldReflectionSnapshot->properties.size(), 1u);
    EXPECT_EQ(oldReflectionSnapshot->properties.front().name, "OldColor");

    auto* refreshed = Shader::CreateForTesting("RuntimeReload.shader");
    ShaderReflection newReflection;
    ShaderPropertyDesc newProperty;
    newProperty.name = "NewColor";
    newProperty.type = UniformType::UNIFORM_FLOAT_VEC4;
    newProperty.kind = ShaderResourceKind::Value;
    newProperty.byteSize = 16u;
    newReflection.properties.push_back(newProperty);
    refreshed->SetReflectionForTesting(newReflection);
    shader->ReplaceRuntimeDataForTesting(*refreshed);

    ASSERT_EQ(oldReflectionSnapshot->properties.size(), 1u);
    EXPECT_EQ(oldReflectionSnapshot->properties.front().name, "OldColor");
    ASSERT_EQ(shader->GetReflection().properties.size(), 1u);
    EXPECT_EQ(shader->GetReflection().properties.front().name, "NewColor");

    Shader::DestroyForTesting(refreshed);
    Shader::DestroyForTesting(shader);
}

TEST(ShaderLabHotReloadTests, RuntimeShaderDoesNotRetainUnboundedOldRuntimeData)
{
    using namespace NLS::Render::Resources;

    auto* shader = Shader::CreateForTesting("RuntimeReload.shader");
    for (int index = 0; index < 8; ++index)
    {
        auto* refreshed = Shader::CreateForTesting("RuntimeReload.shader");
        ShaderReflection reflection;
        ShaderPropertyDesc property;
        property.name = "Color" + std::to_string(index);
        property.type = UniformType::UNIFORM_FLOAT_VEC4;
        property.kind = ShaderResourceKind::Value;
        property.byteSize = 16u;
        reflection.properties.push_back(property);
        refreshed->SetReflectionForTesting(std::move(reflection));

        shader->ReplaceRuntimeDataForTesting(*refreshed);
        EXPECT_EQ(shader->GetRetiredRuntimeDataCountForTesting(), 0u);
        Shader::DestroyForTesting(refreshed);
    }

    Shader::DestroyForTesting(shader);
}
