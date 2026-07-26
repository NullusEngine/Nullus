#include "Rendering/MetalViewportRenderer.h"

#import <Metal/Metal.h>

#include <chrono>
#include <string>
#include <utility>

#include "Debug/Logger.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Entities/Camera.h"
#include "Rendering/RHI/Core/RHIDevice.h"

namespace NLS::Editor::Rendering
{
    namespace
    {
        struct MetalViewportUniforms
        {
            float viewportWidth = 1.0f;
            float viewportHeight = 1.0f;
            float cameraX = 0.0f;
            float cameraZ = 0.0f;
            float elapsedSeconds = 0.0f;
            float viewKind = 0.0f;
        };

        constexpr const char* kMetalViewportShaderSource = R"METAL(
            #include <metal_stdlib>
            using namespace metal;

            struct VertexOutput
            {
                float4 position [[position]];
                float2 uv;
            };

            struct ViewportUniforms
            {
                float viewportWidth;
                float viewportHeight;
                float cameraX;
                float cameraZ;
                float elapsedSeconds;
                float viewKind;
            };

            vertex VertexOutput viewport_vertex(uint vertexId [[vertex_id]])
            {
                constexpr float2 positions[3] = {
                    float2(-1.0, -1.0),
                    float2( 3.0, -1.0),
                    float2(-1.0,  3.0)
                };

                VertexOutput output;
                output.position = float4(positions[vertexId], 0.0, 1.0);
                output.uv = positions[vertexId] * 0.5 + 0.5;
                return output;
            }

            fragment float4 viewport_fragment(
                VertexOutput input [[stage_in]],
                constant ViewportUniforms& uniforms [[buffer(0)]])
            {
                const float aspect = max(uniforms.viewportWidth / max(uniforms.viewportHeight, 1.0), 0.001);
                float2 plane = float2((input.uv.x - 0.5) * aspect, input.uv.y - 0.5);
                plane += float2(uniforms.cameraX, uniforms.cameraZ) * 0.025;

                const float2 cell = abs(fract(plane * 12.0) - 0.5);
                const float grid = 1.0 - smoothstep(0.47, 0.5, max(cell.x, cell.y));
                const float axis = 1.0 - smoothstep(0.008, 0.018, min(abs(plane.x), abs(plane.y)));
                const float vignette = smoothstep(1.05, 0.15, length(plane));

                const float3 sceneBase = float3(0.055, 0.095, 0.135);
                const float3 gameBase = float3(0.045, 0.105, 0.075);
                float3 color = mix(sceneBase, gameBase, uniforms.viewKind);
                color += grid * float3(0.12, 0.18, 0.22);
                color = mix(color, float3(0.82, 0.28, 0.14), axis * (1.0 - uniforms.viewKind));
                color += 0.02 * sin(uniforms.elapsedSeconds + plane.xyx * float3(1.0, 1.7, 2.3));
                return float4(color * vignette, 1.0);
            }
        )METAL";

        bool IsGameViewport(const std::string& debugName)
        {
            return debugName.find("Game") != std::string::npos;
        }
    }

    struct MetalViewportRenderer::Impl
    {
        explicit Impl(std::string name)
            : debugName(std::move(name))
            , gameViewport(IsGameViewport(debugName))
        {
        }

        ~Impl()
        {
            [pipeline release];
            [queue release];
            [device release];
        }

        id<MTLDevice> device = nil;
        id<MTLCommandQueue> queue = nil;
        id<MTLRenderPipelineState> pipeline = nil;
        std::shared_ptr<NLS::Render::RHI::RHIDevice> rhiDevice;
        std::shared_ptr<NLS::Render::RHI::RHITexture> outputTexture;
        std::shared_ptr<NLS::Render::RHI::RHITextureView> outputView;
        std::string debugName;
        uint16_t width = 0u;
        uint16_t height = 0u;
        bool gameViewport = false;
        bool loggedFirstFrame = false;
        std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    };

    MetalViewportRenderer::MetalViewportRenderer(std::unique_ptr<Impl> impl)
        : m_impl(std::move(impl))
    {
    }

    MetalViewportRenderer::~MetalViewportRenderer() = default;

    std::unique_ptr<MetalViewportRenderer> CreateMetalViewportRenderer(
        NLS::Render::Context::Driver& driver,
        std::string debugName)
    {
        const auto nativeInfo = NLS::Render::Context::DriverUIAccess::GetNativeDeviceInfo(driver);
        const auto rhiDevice = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver);
        if (nativeInfo.backend != NLS::Render::RHI::NativeBackendType::Metal ||
            nativeInfo.device == nullptr ||
            nativeInfo.graphicsQueue == nullptr ||
            rhiDevice == nullptr)
        {
            return nullptr;
        }

        auto impl = std::make_unique<MetalViewportRenderer::Impl>(std::move(debugName));
        impl->device = [(__bridge id<MTLDevice>)nativeInfo.device retain];
        impl->queue = [(__bridge id<MTLCommandQueue>)nativeInfo.graphicsQueue retain];
        impl->rhiDevice = rhiDevice;

        NSError* error = nil;
        id<MTLLibrary> library = [impl->device newLibraryWithSource:@(kMetalViewportShaderSource)
            options:nil
            error:&error];
        if (library == nil)
        {
            NLS_LOG_ERROR(
                "MetalViewportRenderer: failed to compile viewport shader: " +
                std::string(error != nil ? error.localizedDescription.UTF8String : "unknown error"));
            return nullptr;
        }

        id<MTLFunction> vertexFunction = [library newFunctionWithName:@"viewport_vertex"];
        id<MTLFunction> fragmentFunction = [library newFunctionWithName:@"viewport_fragment"];
        MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = vertexFunction;
        descriptor.fragmentFunction = fragmentFunction;
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        impl->pipeline = [impl->device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        [descriptor release];
        [fragmentFunction release];
        [vertexFunction release];
        [library release];

        if (impl->pipeline == nil)
        {
            NLS_LOG_ERROR(
                "MetalViewportRenderer: failed to create viewport pipeline: " +
                std::string(error != nil ? error.localizedDescription.UTF8String : "unknown error"));
            return nullptr;
        }

        NLS_LOG_INFO("MetalViewportRenderer: created native preview renderer for " + impl->debugName);
        return std::unique_ptr<MetalViewportRenderer>(new MetalViewportRenderer(std::move(impl)));
    }

    bool MetalViewportRenderer::Render(
        const uint16_t width,
        const uint16_t height,
        const NLS::Render::Entities::Camera* camera)
    {
        if (m_impl == nullptr || width == 0u || height == 0u ||
            m_impl->device == nil || m_impl->queue == nil || m_impl->pipeline == nil)
        {
            return false;
        }

        if (m_impl->outputTexture == nullptr || m_impl->width != width || m_impl->height != height)
        {
            NLS::Render::RHI::RHITextureDesc textureDesc{};
            textureDesc.extent = { width, height, 1u };
            textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
            textureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
            textureDesc.usage = NLS::Render::RHI::TextureUsageFlags::Sampled |
                NLS::Render::RHI::TextureUsageFlags::ColorAttachment;
            textureDesc.debugName = m_impl->debugName + ".MetalPreview";
            m_impl->outputTexture = m_impl->rhiDevice->CreateTexture(textureDesc);
            if (m_impl->outputTexture == nullptr)
                return false;

            NLS::Render::RHI::RHITextureViewDesc viewDesc{};
            viewDesc.viewType = NLS::Render::RHI::TextureViewType::Texture2D;
            viewDesc.format = textureDesc.format;
            viewDesc.debugName = textureDesc.debugName + ".View";
            m_impl->outputView = m_impl->rhiDevice->CreateTextureView(m_impl->outputTexture, viewDesc);
            if (m_impl->outputView == nullptr)
                return false;

            m_impl->width = width;
            m_impl->height = height;
        }

        const auto nativeTexture = m_impl->outputTexture->GetNativeImageHandle();
        if (nativeTexture.backend != NLS::Render::RHI::BackendType::Metal || nativeTexture.handle == nullptr)
            return false;

        id<MTLTexture> texture = (__bridge id<MTLTexture>)nativeTexture.handle;
        id<MTLCommandBuffer> commandBuffer = [m_impl->queue commandBuffer];
        MTLRenderPassDescriptor* renderPass = [MTLRenderPassDescriptor renderPassDescriptor];
        auto* colorAttachment = renderPass.colorAttachments[0];
        colorAttachment.texture = texture;
        colorAttachment.loadAction = MTLLoadActionClear;
        colorAttachment.storeAction = MTLStoreActionStore;
        colorAttachment.clearColor = MTLClearColorMake(0.02, 0.03, 0.04, 1.0);

        id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPass];
        [encoder setRenderPipelineState:m_impl->pipeline];
        float cameraX = 0.0f;
        float cameraZ = 0.0f;
        if (camera != nullptr)
        {
            const auto cameraPosition = camera->GetPosition();
            cameraX = cameraPosition.x;
            cameraZ = cameraPosition.z;
        }
        const MetalViewportUniforms uniforms {
            static_cast<float>(width),
            static_cast<float>(height),
            cameraX,
            cameraZ,
            std::chrono::duration<float>(std::chrono::steady_clock::now() - m_impl->startTime).count(),
            m_impl->gameViewport ? 1.0f : 0.0f
        };
        [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        [commandBuffer commit];

        if (!m_impl->loggedFirstFrame)
        {
            NLS_LOG_INFO("MetalViewportRenderer: encoded first native Metal preview frame for " + m_impl->debugName);
            m_impl->loggedFirstFrame = true;
        }
        return true;
    }

    const std::shared_ptr<NLS::Render::RHI::RHITextureView>& MetalViewportRenderer::GetOutputTextureView() const
    {
        static const std::shared_ptr<NLS::Render::RHI::RHITextureView> empty;
        return m_impl != nullptr ? m_impl->outputView : empty;
    }
}
