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
#include "Rendering/Resources/Loaders/ModelLoader.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Resources/Model.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Entities/Camera.h"
#include "Rendering/Data/FrameDescriptor.h"
#include "Components/MaterialRenderer.h"
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

    NLS::Render::Resources::Model* CreateSingleTriangleModel()
    {
        auto* mesh = new NLS::Render::Resources::Mesh(
            std::vector<NLS::Render::Geometry::Vertex>{
                MakeVertex(-0.5f, -0.5f, 0.0f),
                MakeVertex(0.5f, -0.5f, 0.0f),
                MakeVertex(0.0f, 0.5f, 0.0f)
            },
            std::vector<uint32_t>{ 0u, 1u, 2u },
            0u);

        return NLS::Render::Resources::Loaders::ModelLoader::Create({ mesh });
    }

    void AttachRenderable(
        NLS::Engine::SceneSystem::Scene& scene,
        NLS::Render::Resources::Model& model,
        NLS::Render::Resources::Material& material)
    {
        auto& actor = scene.CreateGameObject("DeferredCacheActor");
        actor.AddComponent<NLS::Engine::Components::MeshRenderer>()->SetModel(&model);
        actor.AddComponent<NLS::Engine::Components::MaterialRenderer>()->FillWithMaterial(material);
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

    auto* model = CreateSingleTriangleModel();
    ASSERT_NE(model, nullptr);

    NLS::Render::Resources::Material firstMaterial(shader);
    NLS::Render::Resources::Material secondMaterial(shader);
    const_cast<std::string&>(firstMaterial.path) = "App/Assets/Test/SharedDeferredMaterial.nmat";
    const_cast<std::string&>(secondMaterial.path) = "App/Assets/Test/SharedDeferredMaterial.nmat";

    {
        NLS::Engine::SceneSystem::Scene scene;
        AttachRenderable(scene, *model, firstMaterial);
        RenderOneDeferredCacheFrame(renderer, scene);
    }
    {
        NLS::Engine::SceneSystem::Scene scene;
        AttachRenderable(scene, *model, secondMaterial);
        RenderOneDeferredCacheFrame(renderer, scene);
    }

    EXPECT_EQ(renderer.m_gBufferMaterialCache.size(), 1u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ModelLoader::Destroy(model));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}
