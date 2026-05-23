#include <gtest/gtest.h>

#include <vector>
#include <unordered_map>

#include <fg/FrameGraph.hpp>

#include "Rendering/BaseSceneRenderer.h"
#include "Rendering/Buffers/MultiFramebuffer.h"

#define private public
#include "Rendering/DeferredSceneRenderer.h"
#undef private

#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/Resources/Loaders/TextureLoader.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Entities/Camera.h"
#include "Rendering/Data/FrameDescriptor.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Core/ServiceLocator.h"
#include "SceneSystem/Scene.h"

namespace
{
    NLS::Render::Geometry::Vertex MakeVertex(const float x, const float y, const float z)
    {
        NLS::Render::Geometry::Vertex vertex{};
        vertex.position[0] = x;
        vertex.position[1] = y;
        vertex.position[2] = z;
        vertex.normals[2] = 1.0f;
        vertex.tangent[0] = 1.0f;
        vertex.bitangent[1] = 1.0f;
        return vertex;
    }

    NLS::Render::Resources::Mesh* CreateSingleTriangleMesh()
    {
        return new NLS::Render::Resources::Mesh(
            std::vector<NLS::Render::Geometry::Vertex>{
                MakeVertex(-0.5f, -0.5f, 0.0f),
                MakeVertex(0.5f, -0.5f, 0.0f),
                MakeVertex(0.0f, 0.5f, 0.0f)
            },
            std::vector<uint32_t>{ 0u, 1u, 2u },
            0u);
    }

    void AttachRenderable(
        NLS::Engine::SceneSystem::Scene& scene,
        NLS::Render::Resources::Mesh& mesh,
        NLS::Render::Resources::Material& material)
    {
        auto& actor = scene.CreateGameObject("DeferredCacheActor");
        actor.AddComponent<NLS::Engine::Components::MeshFilter>()->SetMesh(&mesh);
        actor.AddComponent<NLS::Engine::Components::MeshRenderer>()->FillWithMaterial(material);
    }

    void RenderOneDeferredCacheFrame(
        NLS::Engine::Rendering::DeferredSceneRenderer& renderer,
        NLS::Engine::SceneSystem::Scene& scene)
    {
        renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
            scene,
            std::nullopt,
            nullptr
        });

        NLS::Render::Entities::Camera camera;
        NLS::Render::Data::FrameDescriptor frameDescriptor;
        frameDescriptor.renderWidth = 128u;
        frameDescriptor.renderHeight = 96u;
        frameDescriptor.camera = &camera;

        renderer.BeginFrame(frameDescriptor);
        renderer.EndFrame();
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
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    auto* mesh = CreateSingleTriangleMesh();
    ASSERT_NE(mesh, nullptr);

    NLS::Render::Resources::Material firstMaterial(shader);
    NLS::Render::Resources::Material secondMaterial(shader);
    const_cast<std::string&>(firstMaterial.path) = "App/Assets/Test/SharedDeferredMaterial.nmat";
    const_cast<std::string&>(secondMaterial.path) = "App/Assets/Test/SharedDeferredMaterial.nmat";

    {
        NLS::Engine::SceneSystem::Scene scene;
        AttachRenderable(scene, *mesh, firstMaterial);
        RenderOneDeferredCacheFrame(renderer, scene);
    }
    {
        NLS::Engine::SceneSystem::Scene scene;
        AttachRenderable(scene, *mesh, secondMaterial);
        RenderOneDeferredCacheFrame(renderer, scene);
    }

    EXPECT_EQ(renderer.m_gBufferMaterialCache.size(), 1u);

    delete mesh;
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
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver);

    auto* lambertShader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Lambert.hlsl");
    ASSERT_NE(lambertShader, nullptr);
    auto* gbufferShader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/DeferredGBuffer.hlsl");
    ASSERT_NE(gbufferShader, nullptr);

    auto* diffuseTexture = NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(128, 64, 32, 255);
    ASSERT_NE(diffuseTexture, nullptr);

    NLS::Render::Resources::Material source(lambertShader);
    source.Set<NLS::Maths::Vector4>("u_Diffuse", { 0.25f, 0.5f, 0.75f, 1.0f });
    source.Set<NLS::Render::Resources::Texture2D*>("u_DiffuseMap", diffuseTexture);

    auto* mesh = CreateSingleTriangleMesh();
    ASSERT_NE(mesh, nullptr);

    NLS::Engine::SceneSystem::Scene scene;
    AttachRenderable(scene, *mesh, source);
    RenderOneDeferredCacheFrame(renderer, scene);

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

    delete mesh;
    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(diffuseTexture));
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
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver);

    auto* lambertShader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Lambert.hlsl");
    ASSERT_NE(lambertShader, nullptr);

    NLS::Render::Resources::Material source(lambertShader);
    source.Set<NLS::Maths::Vector4>("u_Diffuse", { 0.15f, 0.25f, 0.35f, 1.0f });

    auto* mesh = CreateSingleTriangleMesh();
    ASSERT_NE(mesh, nullptr);

    NLS::Engine::SceneSystem::Scene scene;
    AttachRenderable(scene, *mesh, source);

    RenderOneDeferredCacheFrame(renderer, scene);
    ASSERT_EQ(renderer.m_gBufferMaterialCache.size(), 1u);
    EXPECT_EQ(renderer.m_frameGBufferMaterialSyncCount, 1u);
    EXPECT_EQ(renderer.m_gBufferMaterialCache.begin()->second.syncCount, 1u);

    RenderOneDeferredCacheFrame(renderer, scene);
    EXPECT_EQ(renderer.m_frameGBufferMaterialSyncCount, 0u);
    EXPECT_EQ(renderer.m_gBufferMaterialCache.begin()->second.syncCount, 1u);

    source.Set<NLS::Maths::Vector4>("u_Diffuse", { 0.8f, 0.7f, 0.6f, 1.0f });
    RenderOneDeferredCacheFrame(renderer, scene);
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

    delete mesh;
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
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver);

    auto* lambertShader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Lambert.hlsl");
    ASSERT_NE(lambertShader, nullptr);

    NLS::Render::Resources::Material firstSource(lambertShader);
    NLS::Render::Resources::Material secondSource(lambertShader);
    firstSource.Set<NLS::Maths::Vector4>("u_Diffuse", { 0.1f, 0.2f, 0.3f, 1.0f });
    secondSource.Set<NLS::Maths::Vector4>("u_Diffuse", { 0.7f, 0.6f, 0.5f, 1.0f });

    auto* mesh = CreateSingleTriangleMesh();
    ASSERT_NE(mesh, nullptr);

    {
        NLS::Engine::SceneSystem::Scene scene;
        AttachRenderable(scene, *mesh, firstSource);
        RenderOneDeferredCacheFrame(renderer, scene);
    }

    ASSERT_EQ(renderer.m_gBufferMaterialCache.size(), 1u);
    EXPECT_EQ(renderer.m_frameGBufferMaterialSyncCount, 1u);
    EXPECT_EQ(renderer.m_gBufferMaterialCache.begin()->second.syncCount, 1u);

    NLS::Engine::SceneSystem::Scene secondScene;
    AttachRenderable(secondScene, *mesh, secondSource);
    RenderOneDeferredCacheFrame(renderer, secondScene);

    ASSERT_EQ(renderer.m_gBufferMaterialCache.size(), 2u);
    EXPECT_EQ(renderer.m_frameGBufferMaterialSyncCount, 1u);

    RenderOneDeferredCacheFrame(renderer, secondScene);
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

    delete mesh;
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(lambertShader));
}
