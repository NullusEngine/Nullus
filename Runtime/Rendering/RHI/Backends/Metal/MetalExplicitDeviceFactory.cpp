// Runtime/Rendering/RHI/Backends/Metal/MetalExplicitDeviceFactory.cpp
#include "Rendering/RHI/Backends/Metal/MetalExplicitDeviceFactory.h"

#include "Debug/Logger.h"

#if defined(__APPLE__)
#import <Metal/Metal.h>
#import <Metal/MTLDevice.hpp>
#import <Metal/MTLCommandQueue.hpp>
#endif

namespace NLS::Render::Backend
{
    std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateMetalRhiDevice(void* platformWindow)
    {
#if defined(__APPLE__)
        // Metal is only available on macOS/iOS
        (void)platformWindow;

        // Enumerate available Metal devices
        NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
        if (devices == nil || devices.count == 0)
        {
            NLS_LOG_ERROR("CreateMetalRhiDevice: No Metal devices found");
            return nullptr;
        }

        // Use the first available device (prefer discrete GPU)
        id<MTLDevice> metalDevice = devices[0];
        for (id<MTLDevice> device in devices)
        {
            // Check if this is a discrete GPU (not integrated)
            // Note: This is a simplified selection - real implementation might be more sophisticated
            if (device != nil)
            {
                metalDevice = device;
                break;
            }
        }

        if (metalDevice == nil)
        {
            NLS_LOG_ERROR("CreateMetalRhiDevice: Failed to select Metal device");
            return nullptr;
        }

        // Create command queue
        id<MTLCommandQueue> commandQueue = [metalDevice newCommandQueue];
        if (commandQueue == nil)
        {
            NLS_LOG_ERROR("CreateMetalRhiDevice: Failed to create Metal command queue");
            return nullptr;
        }

        // Build capabilities
        NLS::Render::RHI::RHIDeviceCapabilities capabilities{};
        capabilities.backendReady = true;
        capabilities.supportsGraphics = true;
        capabilities.supportsCompute = true;
        capabilities.supportsSwapchain = true;
        capabilities.supportsFramebufferReadback = true;
        capabilities.supportsExplicitBarriers = false; // Metal uses implicit synchronization
        capabilities.maxTextureDimension2D = 4096;
        capabilities.maxColorAttachments = 8;

        // Get device name
        NSString* deviceName = [[metalDevice name] retain];

        // Create native Metal device
        // Note: This is a placeholder - actual implementation would need NativeMetalExplicitDevice class
        (void)commandQueue;
        (void)capabilities;
        (void)deviceName;

        NLS_LOG_INFO("CreateMetalRhiDevice: Metal backend is not fully implemented yet");
        return nullptr;

#else
        NLS_LOG_WARNING("CreateMetalRhiDevice: Metal backend is only available on macOS/iOS");
        (void)platformWindow;
        return nullptr;
#endif
    }
}