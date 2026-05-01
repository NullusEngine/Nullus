#include <gtest/gtest.h>

#include <memory>

#include "Core/ServiceLocator.h"
#include "Components/LightComponent.h"
#include "Rendering/BaseSceneRenderer.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/Data/LightingDescriptor.h"
#include "Rendering/LightGridPrepass.h"
#include "Rendering/SceneLightingProvider.h"
#include "Rendering/Settings/DriverSettings.h"
#include "SceneSystem/Scene.h"

namespace
{
    class LightingProviderTestSceneRenderer final : public NLS::Engine::Rendering::BaseSceneRenderer
    {
    public:
        explicit LightingProviderTestSceneRenderer(NLS::Render::Context::Driver& driver)
            : BaseSceneRenderer(driver)
        {
        }

        void PublishLightingForScene(NLS::Engine::SceneSystem::Scene& scene)
        {
            RefreshSceneLightingDescriptor(scene);
        }

        NLS::Render::Context::RenderScenePackage CaptureRenderScenePackage(
            const NLS::Render::Context::FrameSnapshot& snapshot) const
        {
            return BuildRenderScenePackage(snapshot);
        }
    };
}

TEST(LightingDataProviderTests, SceneLightingProviderCollectsOnlyActiveLightsAndRefreshesEachFrame)
{
    NLS::Engine::SceneSystem::Scene scene;

    auto& activeLightActor = scene.CreateGameObject("ActiveLight");
    auto* activeLight = activeLightActor.AddComponent<NLS::Engine::Components::LightComponent>();
    ASSERT_NE(activeLight, nullptr);
    activeLight->SetIntensity(3.5f);

    auto& inactiveLightActor = scene.CreateGameObject("InactiveLight");
    auto* inactiveLight = inactiveLightActor.AddComponent<NLS::Engine::Components::LightComponent>();
    ASSERT_NE(inactiveLight, nullptr);
    inactiveLight->SetIntensity(9.0f);
    inactiveLightActor.SetActive(false);

    NLS::Engine::Rendering::SceneLightingProvider provider;
    provider.Collect(scene);

    const auto& firstCollection = provider.GetLightingDescriptor();
    ASSERT_EQ(firstCollection.lights.size(), 1u);
    EXPECT_FLOAT_EQ(firstCollection.lights.front().get().intensity, 3.5f);

    activeLightActor.SetActive(false);
    provider.Collect(scene);

    EXPECT_TRUE(provider.GetLightingDescriptor().lights.empty());
}

TEST(LightingDataProviderTests, BaseSceneRendererPublishesSharedLightingDescriptorThroughSharedProviderPath)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    LightingProviderTestSceneRenderer renderer(*driver);

    NLS::Engine::SceneSystem::Scene scene;
    auto& lightActor = scene.CreateGameObject("SceneLight");
    auto* light = lightActor.AddComponent<NLS::Engine::Components::LightComponent>();
    ASSERT_NE(light, nullptr);
    light->SetIntensity(4.25f);

    renderer.PublishLightingForScene(scene);
    ASSERT_TRUE(renderer.HasDescriptor<NLS::Render::Data::LightingDescriptor>());
    const auto& lightingDescriptor = renderer.GetDescriptor<NLS::Render::Data::LightingDescriptor>();
    ASSERT_EQ(lightingDescriptor.lights.size(), 1u);
    EXPECT_EQ(&lightingDescriptor.lights.front().get(), light->GetData());

    const auto& providerDescriptor = renderer.GetSceneLightingProvider().GetLightingDescriptor();
    ASSERT_EQ(providerDescriptor.lights.size(), 1u);
    EXPECT_EQ(&providerDescriptor.lights.front().get(), &lightingDescriptor.lights.front().get());
}

TEST(LightingDataProviderTests, RenderScenePackageCarriesLightingReadinessFromSnapshot)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    LightingProviderTestSceneRenderer renderer(driver);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 42u;
    snapshot.hasSceneInput = true;
    snapshot.sceneLightCount = 2u;

    const auto package = renderer.CaptureRenderScenePackage(snapshot);

    EXPECT_EQ(package.frameId, 42u);
    EXPECT_TRUE(package.hasLightingData);
    EXPECT_TRUE(package.lightingDataReady);
    EXPECT_TRUE(package.frameDataReady);
    EXPECT_FALSE(package.containsCommandInputs);
    EXPECT_EQ(package.drawCommandCount, 0u);
    EXPECT_EQ(package.materialBatchCount, 0u);
    EXPECT_TRUE(package.passCommandInputs.empty());
}

TEST(LightingDataProviderTests, SceneLightingProviderPreparesPackageLightingFromSnapshotOnly)
{
    NLS::Engine::Rendering::SceneLightingProvider provider;

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.hasSceneInput = true;
    snapshot.sceneLightCount = 2u;

    NLS::Render::Context::RenderScenePackage package;
    EXPECT_FALSE(package.hasLightingData);
    EXPECT_FALSE(package.lightingDataReady);

    provider.PrepareRenderScenePackage(snapshot, package);

    EXPECT_TRUE(package.hasLightingData);
    EXPECT_TRUE(package.lightingDataReady);
}

TEST(LightingDataProviderTests, LightGridCapturedFrameInputsOwnLightValuesInsteadOfReferencingSceneLights)
{
    NLS::Engine::SceneSystem::Scene scene;

    auto& lightActor = scene.CreateGameObject("SceneLight");
    auto* light = lightActor.AddComponent<NLS::Engine::Components::LightComponent>();
    ASSERT_NE(light, nullptr);

    lightActor.GetTransform()->SetWorldPosition({ 1.0f, 2.0f, 3.0f });
    light->SetColor({ 0.25f, 0.5f, 0.75f });
    light->SetIntensity(4.0f);
    light->SetConstant(0.2f);
    light->SetLinear(0.3f);
    light->SetQuadratic(0.4f);
    light->SetOuterCutoff(22.0f);

    NLS::Engine::Rendering::SceneLightingProvider provider;
    provider.Collect(scene);

    const auto capturedInputs =
        NLS::Engine::Rendering::LightGridPrepass::CaptureFrameInputs(
            provider.GetLightingDescriptor(),
            true);

    ASSERT_EQ(capturedInputs.lights.size(), 1u);
    const auto& capturedLight = capturedInputs.lights.front();

    lightActor.GetTransform()->SetWorldPosition({ 10.0f, 20.0f, 30.0f });
    light->SetColor({ 1.0f, 0.0f, 0.0f });
    light->SetIntensity(9.0f);
    light->SetConstant(1.0f);
    light->SetLinear(2.0f);
    light->SetQuadratic(3.0f);
    light->SetOuterCutoff(45.0f);

    EXPECT_TRUE(capturedInputs.hasSkyboxTexture);
    EXPECT_FLOAT_EQ(capturedLight.position.x, 1.0f);
    EXPECT_FLOAT_EQ(capturedLight.position.y, 2.0f);
    EXPECT_FLOAT_EQ(capturedLight.position.z, 3.0f);
    EXPECT_FLOAT_EQ(capturedLight.color.x, 0.25f);
    EXPECT_FLOAT_EQ(capturedLight.color.y, 0.5f);
    EXPECT_FLOAT_EQ(capturedLight.color.z, 0.75f);
    EXPECT_FLOAT_EQ(capturedLight.intensity, 4.0f);
    EXPECT_FLOAT_EQ(capturedLight.constant, 0.2f);
    EXPECT_FLOAT_EQ(capturedLight.linear, 0.3f);
    EXPECT_FLOAT_EQ(capturedLight.quadratic, 0.4f);
    EXPECT_FLOAT_EQ(capturedLight.outerCutoff, 22.0f);
}

TEST(LightingDataProviderTests, LightGridBuildPreparedComputeRequestWithoutPreparedInputsProducesEmptyPreparedSource)
{
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;

    const auto request =
        NLS::Engine::Rendering::LightGridPrepass::BuildPreparedComputeRequest(
            frameDescriptor,
            nullptr,
            std::nullopt);

    EXPECT_EQ(request.frameDescriptor.renderWidth, 320u);
    EXPECT_EQ(request.frameDescriptor.renderHeight, 180u);
    EXPECT_EQ(request.lightGridPrepass, nullptr);
    EXPECT_FALSE(request.preparedFrameInputs.has_value());

    const auto preparedSource =
        NLS::Engine::Rendering::LightGridPrepass::BuildPreparedComputeDispatchSource(request);
    EXPECT_TRUE(preparedSource.dispatchInputs.empty());
    EXPECT_TRUE(preparedSource.metadata.empty());
}
