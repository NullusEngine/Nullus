#include <gtest/gtest.h>

#include <fstream>
#include <tuple>

#define private public
#include "Rendering/DeferredSceneRenderer.h"
#undef private

#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/Resources/Loaders/TextureLoader.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/RHI/BindingPointMap.h"
#include "Core/ServiceLocator.h"
#include "Guid.h"

namespace
{
    NLS::Render::Resources::ShaderReflection MakeDeferredMaterialShaderReflection()
    {
        NLS::Render::Resources::ShaderReflection reflection;
        reflection.constantBuffers.push_back({
            "MaterialConstants",
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
            0u,
            64u,
            {
                {"u_Diffuse", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, 0u, 16u, 1u},
                {"u_DiffuseMap", NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D, 16u, 0u, 1u},
                {"u_Albedo", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, 32u, 16u, 1u},
                {"u_AlbedoMap", NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D, 48u, 0u, 1u}
            }
        });
        for (const auto& [name, type, kind, offset, size] : {
            std::tuple{"u_Diffuse", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, NLS::Render::Resources::ShaderResourceKind::Value, 0u, 16u},
            std::tuple{"u_DiffuseMap", NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D, NLS::Render::Resources::ShaderResourceKind::SampledTexture, 16u, 0u},
            std::tuple{"u_Albedo", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, NLS::Render::Resources::ShaderResourceKind::Value, 32u, 16u},
            std::tuple{"u_AlbedoMap", NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D, NLS::Render::Resources::ShaderResourceKind::SampledTexture, 48u, 0u}
        })
        {
            reflection.properties.push_back({
                name,
                type,
                kind,
                NLS::Render::ShaderCompiler::ShaderStage::Pixel,
                NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
                0u,
                -1,
                1,
                offset,
                size,
                kind == NLS::Render::Resources::ShaderResourceKind::Value ? "MaterialConstants" : ""
            });
        }
        return reflection;
    }

    NLS::Render::Resources::Shader* CreateTestShader(const std::string& sourcePath)
    {
        NLS::Render::Assets::ShaderArtifact artifact;
        artifact.sourcePath = sourcePath;
        artifact.subAssetKey = "shader:test";
        artifact.reflection = MakeDeferredMaterialShaderReflection();
        artifact.stages.push_back({
            NLS::Render::ShaderCompiler::ShaderStage::Vertex,
            NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL,
            "VSMain",
            "vs_6_0",
            {
                NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded,
                {1u, 2u, 3u, 4u},
                {},
                {},
                "test-vertex",
                "test.nshader"
            }
        });
        artifact.stages.push_back({
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL,
            "PSMain",
            "ps_6_0",
            {
                NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded,
                {5u, 6u, 7u, 8u},
                {},
                {},
                "test-pixel",
                "test.nshader"
            }
        });

        const auto root = std::filesystem::temp_directory_path() /
            ("nullus_deferred_shader_" + NLS::Guid::New().ToString());
        const auto path = root / "shader.nshader";
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        const auto bytes = NLS::Render::Assets::SerializeShaderArtifact(artifact);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close();
        auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(path.string());
        std::filesystem::remove_all(root);
        return shader;
    }

    NLS::Render::Resources::Material& SyncOneDeferredCacheMaterial(
        NLS::Engine::Rendering::DeferredSceneRenderer& renderer,
        NLS::Render::Resources::Material& sourceMaterial)
    {
        renderer.m_frameGBufferMaterialSyncCount = 0u;
        return NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetOrCreateGBufferMaterial(
            renderer,
            sourceMaterial);
    }
}

TEST(DeferredSceneRendererMaterialCacheTests, ReusesGBufferMaterialForStableMaterialAssetPath)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

    auto* shader = CreateTestShader("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material firstMaterial(shader);
    NLS::Render::Resources::Material secondMaterial(shader);
    const_cast<std::string&>(firstMaterial.path) = "App/Assets/Test/SharedDeferredMaterial.nmat";
    const_cast<std::string&>(secondMaterial.path) = "App/Assets/Test/SharedDeferredMaterial.nmat";

    SyncOneDeferredCacheMaterial(renderer, firstMaterial);
    SyncOneDeferredCacheMaterial(renderer, secondMaterial);

    EXPECT_EQ(renderer.m_gBufferMaterialCache.size(), 1u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(DeferredSceneRendererMaterialCacheTests, ProvidesVisibleDeferredGBufferFallbackInputsForLambertMaterials)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

    auto* lambertShader = CreateTestShader("App/Assets/Engine/Shaders/Lambert.hlsl");
    ASSERT_NE(lambertShader, nullptr);
    auto* gbufferShader = CreateTestShader("App/Assets/Engine/Shaders/DeferredGBuffer.hlsl");
    ASSERT_NE(gbufferShader, nullptr);
    renderer.m_gBufferShader = gbufferShader;

    auto* diffuseTexture = NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(128, 64, 32, 255);
    ASSERT_NE(diffuseTexture, nullptr);

    NLS::Render::Resources::Material source(lambertShader);
    source.Set<NLS::Maths::Vector4>("u_Diffuse", { 0.25f, 0.5f, 0.75f, 1.0f });
    source.Set<NLS::Render::Resources::Texture2D*>("u_DiffuseMap", diffuseTexture);

    SyncOneDeferredCacheMaterial(renderer, source);

    ASSERT_EQ(renderer.m_gBufferMaterialCache.size(), 1u);
    const auto& gbuffer = *renderer.m_gBufferMaterialCache.begin()->second.material;

    const auto* albedoValue = gbuffer.GetParameterBlock().TryGet("u_Albedo");
    ASSERT_NE(albedoValue, nullptr);
    ASSERT_EQ(albedoValue->type(), typeid(NLS::Maths::Vector4));
    const auto& albedo = std::any_cast<const NLS::Maths::Vector4&>(*albedoValue);
    EXPECT_FLOAT_EQ(albedo.x, 0.25f);
    EXPECT_FLOAT_EQ(albedo.y, 0.5f);
    EXPECT_FLOAT_EQ(albedo.z, 0.75f);
    EXPECT_FLOAT_EQ(albedo.w, 1.0f);

    const auto* albedoMapValue = gbuffer.GetParameterBlock().TryGet("u_AlbedoMap");
    ASSERT_NE(albedoMapValue, nullptr);
    ASSERT_EQ(albedoMapValue->type(), typeid(NLS::Render::Resources::Texture2D*));
    EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*albedoMapValue), diffuseTexture);

    for (const char* textureName : {
        "u_MetallicMap",
        "u_RoughnessMap",
        "u_AmbientOcclusionMap",
        "u_NormalMap",
        "u_OpacityMap",
        "u_EmissiveMap",
        "u_SpecularMap"
    })
    {
        const auto* textureValue = gbuffer.GetParameterBlock().TryGet(textureName);
        ASSERT_NE(textureValue, nullptr) << textureName;
        ASSERT_EQ(textureValue->type(), typeid(NLS::Render::Resources::Texture2D*)) << textureName;
    }

    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(diffuseTexture));
    renderer.m_gBufferShader = nullptr;
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(gbufferShader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(lambertShader));
}

TEST(DeferredSceneRendererMaterialCacheTests, SkipsGBufferMaterialSyncUntilSourceMaterialRevisionChanges)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

    auto* lambertShader = CreateTestShader("App/Assets/Engine/Shaders/Lambert.hlsl");
    ASSERT_NE(lambertShader, nullptr);
    auto* gbufferShader = CreateTestShader("App/Assets/Engine/Shaders/DeferredGBuffer.hlsl");
    ASSERT_NE(gbufferShader, nullptr);
    renderer.m_gBufferShader = gbufferShader;

    NLS::Render::Resources::Material source(lambertShader);
    source.Set<NLS::Maths::Vector4>("u_Diffuse", { 0.15f, 0.25f, 0.35f, 1.0f });

    SyncOneDeferredCacheMaterial(renderer, source);
    ASSERT_EQ(renderer.m_gBufferMaterialCache.size(), 1u);
    EXPECT_EQ(renderer.m_frameGBufferMaterialSyncCount, 1u);
    EXPECT_EQ(renderer.m_gBufferMaterialCache.begin()->second.syncCount, 1u);

    SyncOneDeferredCacheMaterial(renderer, source);
    EXPECT_EQ(renderer.m_frameGBufferMaterialSyncCount, 0u);
    EXPECT_EQ(renderer.m_gBufferMaterialCache.begin()->second.syncCount, 1u);

    source.Set<NLS::Maths::Vector4>("u_Diffuse", { 0.8f, 0.7f, 0.6f, 1.0f });
    SyncOneDeferredCacheMaterial(renderer, source);
    EXPECT_EQ(renderer.m_frameGBufferMaterialSyncCount, 1u);
    EXPECT_EQ(renderer.m_gBufferMaterialCache.begin()->second.syncCount, 2u);

    const auto& gbuffer = *renderer.m_gBufferMaterialCache.begin()->second.material;
    const auto* albedoValue = gbuffer.GetParameterBlock().TryGet("u_Albedo");
    ASSERT_NE(albedoValue, nullptr);
    ASSERT_EQ(albedoValue->type(), typeid(NLS::Maths::Vector4));
    const auto& albedo = std::any_cast<const NLS::Maths::Vector4&>(*albedoValue);
    EXPECT_FLOAT_EQ(albedo.x, 0.8f);
    EXPECT_FLOAT_EQ(albedo.y, 0.7f);
    EXPECT_FLOAT_EQ(albedo.z, 0.6f);
    EXPECT_FLOAT_EQ(albedo.w, 1.0f);

    renderer.m_gBufferShader = nullptr;
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(gbufferShader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(lambertShader));
}

TEST(DeferredSceneRendererMaterialCacheTests, ResyncsSharedRuntimeVariantWhenSourceMaterialIdentityChanges)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

    auto* lambertShader = CreateTestShader("App/Assets/Engine/Shaders/Lambert.hlsl");
    ASSERT_NE(lambertShader, nullptr);
    auto* gbufferShader = CreateTestShader("App/Assets/Engine/Shaders/DeferredGBuffer.hlsl");
    ASSERT_NE(gbufferShader, nullptr);
    renderer.m_gBufferShader = gbufferShader;

    NLS::Render::Resources::Material firstSource(lambertShader);
    NLS::Render::Resources::Material secondSource(lambertShader);
    firstSource.Set<NLS::Maths::Vector4>("u_Diffuse", { 0.1f, 0.2f, 0.3f, 1.0f });
    secondSource.Set<NLS::Maths::Vector4>("u_Diffuse", { 0.7f, 0.6f, 0.5f, 1.0f });

    SyncOneDeferredCacheMaterial(renderer, firstSource);

    ASSERT_EQ(renderer.m_gBufferMaterialCache.size(), 1u);
    EXPECT_EQ(renderer.m_frameGBufferMaterialSyncCount, 1u);
    EXPECT_EQ(renderer.m_gBufferMaterialCache.begin()->second.syncCount, 1u);

    SyncOneDeferredCacheMaterial(renderer, secondSource);

    ASSERT_EQ(renderer.m_gBufferMaterialCache.size(), 2u);
    EXPECT_EQ(renderer.m_frameGBufferMaterialSyncCount, 1u);

    SyncOneDeferredCacheMaterial(renderer, secondSource);
    EXPECT_EQ(renderer.m_frameGBufferMaterialSyncCount, 0u);
    uint64_t totalSyncCount = 0u;
    const NLS::Render::Resources::Material* secondGBufferMaterial = nullptr;
    for (const auto& [_, entry] : renderer.m_gBufferMaterialCache)
    {
        totalSyncCount += entry.syncCount;
        if (entry.syncedStamp.sourceMaterialInstanceId == secondSource.GetInstanceId())
            secondGBufferMaterial = entry.material.get();
    }
    EXPECT_EQ(totalSyncCount, 2u);

    ASSERT_NE(secondGBufferMaterial, nullptr);
    const auto& gbuffer = *secondGBufferMaterial;
    const auto* albedoValue = gbuffer.GetParameterBlock().TryGet("u_Albedo");
    ASSERT_NE(albedoValue, nullptr);
    ASSERT_EQ(albedoValue->type(), typeid(NLS::Maths::Vector4));
    const auto& albedo = std::any_cast<const NLS::Maths::Vector4&>(*albedoValue);
    EXPECT_FLOAT_EQ(albedo.x, 0.7f);
    EXPECT_FLOAT_EQ(albedo.y, 0.6f);
    EXPECT_FLOAT_EQ(albedo.z, 0.5f);
    EXPECT_FLOAT_EQ(albedo.w, 1.0f);

    renderer.m_gBufferShader = nullptr;
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(gbufferShader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(lambertShader));
}
