#include <gtest/gtest.h>

#include "Rendering/RHI/Backends/ExplicitDeviceFactory.h"
#include "Rendering/RHI/Backends/RenderDeviceFactory.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/IRenderDevice.h"
#include "Rendering/Settings/EGraphicsBackend.h"

TEST(RHIBackendFactoryTests, CreateRenderDeviceForDX11ReportsDX11Backend)
{
    const auto device = NLS::Render::Backend::CreateRenderDevice(NLS::Render::Settings::EGraphicsBackend::DX11);

    ASSERT_NE(device, nullptr);
    EXPECT_EQ(
        device->GetNativeDeviceInfo().backend,
        NLS::Render::RHI::NativeBackendType::DX11);
    EXPECT_FALSE(device->IsBackendReady());
    EXPECT_FALSE(device->GetCapabilities().supportsSwapchain);
    EXPECT_FALSE(device->GetCapabilities().supportsCurrentSceneRenderer);
}

TEST(RHIBackendFactoryTests, CreateExplicitDeviceForDX11UsesCompatibilityIdentity)
{
    const auto device = NLS::Render::Backend::CreateRenderDevice(NLS::Render::Settings::EGraphicsBackend::DX11);

    ASSERT_NE(device, nullptr);

    const auto explicitDevice = NLS::Render::Backend::CreateExplicitDevice(*device);

    ASSERT_NE(explicitDevice, nullptr);
    EXPECT_EQ(explicitDevice->GetNativeDeviceInfo().backend, NLS::Render::RHI::NativeBackendType::None);
    EXPECT_EQ(explicitDevice->GetAdapter()->GetBackendType(), NLS::Render::RHI::NativeBackendType::None);
    EXPECT_FALSE(explicitDevice->IsBackendReady());
}
