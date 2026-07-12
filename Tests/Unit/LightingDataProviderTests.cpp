#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Components/LightComponent.h"
#include "Debug/Logger.h"
#include "Math/Transform.h"
#include "Rendering/BaseSceneRenderer.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/Data/LightingDescriptor.h"
#include "Rendering/Entities/Light.h"
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

	    class ScopedLogListener final
	    {
	    public:
	        explicit ScopedLogListener(std::function<void(const NLS::Debug::LogData&)> callback)
	            : m_listener(NLS::Debug::Logger::LogEvent += std::move(callback))
	        {
	        }

	        ~ScopedLogListener()
	        {
	            NLS::Debug::Logger::LogEvent -= m_listener;
	        }

	        ScopedLogListener(const ScopedLogListener&) = delete;
	        ScopedLogListener& operator=(const ScopedLogListener&) = delete;

	    private:
	        NLS::ListenerID m_listener = NLS::InvalidListenerID;
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

TEST(LightingDataProviderTests, PointLightComponentDefaultsProduceUsableShortRangeContribution)
{
    NLS::Engine::SceneSystem::Scene scene;

    auto& lightActor = scene.CreateGameObject("PointLight");
    auto* light = lightActor.AddComponent<NLS::Engine::Components::LightComponent>();
    ASSERT_NE(light, nullptr);

    ASSERT_NE(light->GetData(), nullptr);
    const auto* lightData = light->GetData();
    ASSERT_EQ(lightData->type, NLS::Render::Settings::ELightType::POINT);
    const float sampleDistance = 5.0f;
    const float attenuationDenominator =
        lightData->constant +
        lightData->linear * sampleDistance +
        lightData->quadratic * sampleDistance * sampleDistance;
    const float contribution =
        attenuationDenominator > 0.0f
            ? lightData->intensity / attenuationDenominator
            : lightData->intensity;

    EXPECT_GT(lightData->GetEffectRange(), 20.0f);
    EXPECT_GT(contribution, 0.2f);
}

TEST(LightingDataProviderTests, PointLightRadiusSetterControlsPointLightEffectRange)
{
    NLS::Engine::SceneSystem::Scene scene;

    auto& lightActor = scene.CreateGameObject("PointLight");
    auto* light = lightActor.AddComponent<NLS::Engine::Components::LightComponent>();
    ASSERT_NE(light, nullptr);

    light->SetLightType(NLS::Render::Settings::ELightType::POINT);
    light->SetIntensity(1.0f);
    light->SetRadius(100.0f);

    ASSERT_NE(light->GetData(), nullptr);
    EXPECT_NEAR(light->GetRadius(), 100.0f, 1.5f);
    EXPECT_NEAR(light->GetData()->GetEffectRange(), 100.0f, 1.5f);
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

TEST(LightingDataProviderTests, LightGridHotPathFailureDiagnosticsAreControlledByRenderDrawTraceSwitch)
{
    NLS::Render::Settings::EngineDiagnosticsSettings diagnostics;
    EXPECT_FALSE(NLS::Engine::Rendering::LightGridPrepass::ShouldLogHotPathFailureDiagnostics(diagnostics));

    diagnostics.logMaterialBindings = true;
    diagnostics.dx12LogFrameFlow = true;
    EXPECT_FALSE(NLS::Engine::Rendering::LightGridPrepass::ShouldLogHotPathFailureDiagnostics(diagnostics));

    diagnostics.logRenderDrawPath = true;
    EXPECT_TRUE(NLS::Engine::Rendering::LightGridPrepass::ShouldLogHotPathFailureDiagnostics(diagnostics));
}

TEST(LightingDataProviderTests, LightGridDefaultsMatchUESourceReference)
{
    const NLS::Engine::Rendering::ClusteredShadingSettings settings;

    EXPECT_EQ(settings.lightGridPixelSize, 64u);
    EXPECT_EQ(settings.gridSizeZ, 32u);
    EXPECT_EQ(settings.maxLightsPerCluster, 32u);
    EXPECT_TRUE(settings.linkedListCulling);
}

TEST(LightingDataProviderTests, LightGridDimensionsDeriveFromRenderSizeAndPixelSize)
{
    const NLS::Engine::Rendering::ClusteredShadingSettings settings;

    const auto fullHdGrid = NLS::Engine::Rendering::CalculateLightGridDimensions(settings, 1920u, 1080u);
    EXPECT_EQ(fullHdGrid.x, 30u);
    EXPECT_EQ(fullHdGrid.y, 17u);
    EXPECT_EQ(fullHdGrid.z, 32u);

    const auto tinyGrid = NLS::Engine::Rendering::CalculateLightGridDimensions(settings, 1u, 1u);
    EXPECT_EQ(tinyGrid.x, 1u);
    EXPECT_EQ(tinyGrid.y, 1u);
    EXPECT_EQ(tinyGrid.z, 32u);
}

TEST(LightingDataProviderTests, LightGridBufferElementCountsRejectOverflowingStartupViewport)
{
    NLS::Engine::Rendering::ClusteredShadingSettings settings;
    settings.lightGridPixelSize = 1u;
    settings.gridSizeZ = 4096u;
    settings.maxLightsPerCluster = 4096u;

    const auto dimensions = NLS::Engine::Rendering::CalculateLightGridDimensions(
        settings,
        65535u,
        65535u);

    uint64_t clusterCount = 0u;
    uint64_t culledLightLinksCount = 0u;
    uint64_t numCulledLightsGridCount = 0u;
    uint64_t culledLightDataGridCount = 0u;

    EXPECT_FALSE(NLS::Engine::Rendering::TryCalculateLightGridBufferElementCounts(
        settings,
        dimensions,
        NLS::Engine::Rendering::GetLightLinkStride(),
        NLS::Engine::Rendering::GetNumCulledLightsGridStride(),
        16ull * 1024ull * 1024ull,
        clusterCount,
        culledLightLinksCount,
        numCulledLightsGridCount,
        culledLightDataGridCount));
    EXPECT_EQ(clusterCount, 0u);
    EXPECT_EQ(culledLightLinksCount, 0u);
    EXPECT_EQ(numCulledLightsGridCount, 0u);
	EXPECT_EQ(culledLightDataGridCount, 0u);
}

TEST(LightingDataProviderTests, LightGridPreparedComputeRejectsOversizedFrameBeforeRhiSetup)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableLightGrid = true;
    settings.diagnostics.logRenderDrawPath = true;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverRendererAccess::SetDiagnosticsSettings(driver, settings.diagnostics);
    auto lightGridPrepass = std::make_shared<NLS::Engine::Rendering::LightGridPrepass>(driver);

    NLS::Render::Entities::Camera camera;
    camera.CacheMatrices(65535u, 65535u);
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.camera = &camera;
    frameDescriptor.renderWidth = 65535u;
    frameDescriptor.renderHeight = 65535u;

    NLS::Engine::Rendering::LightGridPrepass::PreparedFrameInputs frameInputs;

    std::vector<std::string> messages;
    const ScopedLogListener listener(
        [&messages](const NLS::Debug::LogData& data)
        {
            messages.push_back(data.message);
        });

    const auto request = NLS::Engine::Rendering::LightGridPrepass::BuildPreparedComputeRequest(
        frameDescriptor,
        lightGridPrepass,
        frameInputs);
    const auto preparedSource =
        NLS::Engine::Rendering::LightGridPrepass::BuildPreparedComputeDispatchSource(request);

    EXPECT_TRUE(preparedSource.dispatchInputs.empty());
    EXPECT_TRUE(preparedSource.metadata.empty());
    EXPECT_TRUE(std::any_of(
        messages.begin(),
        messages.end(),
        [](const std::string& message)
        {
            return message.find("frame data could not be built") != std::string::npos;
        }));
    EXPECT_FALSE(std::any_of(
        messages.begin(),
        messages.end(),
        [](const std::string& message)
        {
            return message.find("explicit RHI device is unavailable") != std::string::npos;
        }));
}

TEST(LightingDataProviderTests, LightGridZParamsMatchUESourceFormula)
{
    const auto zParams = NLS::Engine::Rendering::CalculateLightGridZParams(10.0f, 1010.0f, 32u);

    const double nearOffset = 0.095 * 100.0;
    const double scale = 4.05;
    const double nearValue = 10.0 + nearOffset;
    const double farValue = 1010.0;
    const double expectedO = (farValue - nearValue * std::exp2((32.0 - 1.0) / scale)) / (farValue - nearValue);
    const double expectedB = (1.0 - expectedO) / nearValue;

    EXPECT_NEAR(zParams.x, static_cast<float>(expectedB), 1e-5f);
    EXPECT_NEAR(zParams.y, static_cast<float>(expectedO), 1e-4f);
    EXPECT_FLOAT_EQ(zParams.z, static_cast<float>(scale));
}

TEST(LightingDataProviderTests, LightGridDispatchShapeMatchesUESourceReference)
{
    const NLS::Engine::Rendering::ClusteredShadingSettings settings;
    const auto grid = NLS::Engine::Rendering::CalculateLightGridDimensions(settings, 1920u, 1080u);
    const auto dispatch = NLS::Engine::Rendering::CalculateLightGridDispatchGroups(grid);

    EXPECT_EQ(NLS::Engine::Rendering::GetLightGridInjectionGroupSize(), 4u);
    EXPECT_EQ(NLS::Engine::Rendering::GetNumCulledLightsGridStride(), 2u);
    EXPECT_EQ(NLS::Engine::Rendering::GetLightLinkStride(), 2u);
    EXPECT_EQ(dispatch.x, 8u);
    EXPECT_EQ(dispatch.y, 5u);
    EXPECT_EQ(dispatch.z, 8u);
}

TEST(LightingDataProviderTests, LightGridCpuBuildUsesDerivedGridDimensionsForCapacityClamp)
{
    NLS::Engine::Rendering::ClusteredShadingSettings settings;
    settings.lightGridPixelSize = 64u;
    settings.maxLightsPerCluster = 2u;

    NLS::Maths::Transform lightTransform;
    NLS::Render::Entities::Light firstLight(&lightTransform);
    firstLight.type = NLS::Render::Settings::ELightType::DIRECTIONAL;
    NLS::Render::Entities::Light secondLight(&lightTransform);
    secondLight.type = NLS::Render::Settings::ELightType::DIRECTIONAL;
    NLS::Render::Entities::Light thirdLight(&lightTransform);
    thirdLight.type = NLS::Render::Settings::ELightType::DIRECTIONAL;

    const std::vector<std::reference_wrapper<const NLS::Render::Entities::Light>> lights{
        firstLight,
        secondLight,
        thirdLight
    };
    NLS::Render::Entities::Camera camera;

    const auto grid = NLS::Engine::Rendering::BuildClusteredLightGrid(
        settings,
        lights,
        camera,
        NLS::Maths::Matrix4::Identity,
        NLS::Maths::Matrix4::Identity,
        1920u,
        1080u,
        0.1f,
        1000.0f);

    ASSERT_EQ(grid.settings.gridSizeX, 30u);
    ASSERT_EQ(grid.settings.gridSizeY, 17u);
    ASSERT_EQ(grid.settings.gridSizeZ, 32u);
    ASSERT_EQ(grid.records.size(), 30u * 17u * 32u);
    ASSERT_EQ(grid.lightIndices.size(), grid.records.size() * settings.maxLightsPerCluster);
    for (const auto& record : grid.records)
        EXPECT_EQ(record.count, settings.maxLightsPerCluster);
}

TEST(LightingDataProviderTests, LightGridCpuBuildAssignsVisiblePointLightToClusters)
{
    NLS::Engine::Rendering::ClusteredShadingSettings settings;
    settings.lightGridPixelSize = 64u;
    settings.maxLightsPerCluster = 4u;

    NLS::Maths::Transform lightTransform;
    lightTransform.SetWorldPosition({ 0.0f, 0.0f, 5.0f });

    NLS::Render::Entities::Light pointLight(&lightTransform);
    pointLight.type = NLS::Render::Settings::ELightType::POINT;
    pointLight.constant = 1.0f;
    pointLight.linear = 0.0f;
    pointLight.quadratic = 0.04f;
    pointLight.intensity = 8.0f;

    const std::vector<std::reference_wrapper<const NLS::Render::Entities::Light>> lights{
        pointLight
    };

    NLS::Render::Entities::Camera camera;
    camera.CacheMatrices(640u, 360u);

    const auto grid = NLS::Engine::Rendering::BuildClusteredLightGrid(
        settings,
        lights,
        camera,
        camera.GetViewMatrix(),
        camera.GetProjectionMatrix(),
        640u,
        360u,
        camera.GetNear(),
        camera.GetFar());

    const auto litCluster = std::find_if(
        grid.records.begin(),
        grid.records.end(),
        [](const NLS::Engine::Rendering::ClusterRecord& record)
        {
            return record.count > 0u;
        });

    ASSERT_NE(litCluster, grid.records.end());
    ASSERT_FALSE(grid.lightIndices.empty());
    EXPECT_EQ(grid.lightIndices.front(), 0u);
}
