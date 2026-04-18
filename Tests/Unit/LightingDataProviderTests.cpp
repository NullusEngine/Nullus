#include <gtest/gtest.h>

#include <memory>

#include "Core/ServiceLocator.h"
#include "Components/LightComponent.h"
#include "Rendering/BaseSceneRenderer.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Data/LightingDescriptor.h"
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
