#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Math/Vector4.h"
#include "Rendering/Core/RenderClearValues.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/Settings/EPixelDataFormat.h"
#include "Rendering/Settings/EPixelDataType.h"
#include "RenderDef.h"

namespace NLS::Render::FrameGraph
{
    struct FrozenFrameDescriptor;
}

namespace NLS::Render::Context
{
    struct NLS_RENDER_API PostSubmitTextureReadbackState
    {
        mutable std::mutex mutex;
        std::shared_ptr<RHI::RHICompletionToken> completion;
        RHI::RHIReadbackStatusCode resultCode = RHI::RHIReadbackStatusCode::InvalidArgument;
        std::string resultMessage;
        bool carriedIntoRenderScenePackage = false;
        uint64_t renderScenePackageFrameId = 0u;
        bool beginAttempted = false;
        bool beginInProgress = false;
        bool beginSucceeded = false;
    };

    enum class TextureReadbackSource : uint8_t
    {
        Texture,
        FrameSwapchain,
        /**
         * The frame's own offscreen scene color target.
         *
         * A swapchain image is owned by the presentation engine between
         * vkQueuePresentKHR and the next vkAcquireNextImageKHR, so a swapchain
         * readback can only run inside the frame, before Present. Comparing what
         * the frame rendered against what it presented therefore needs both
         * sources resolved in the same frame; FrameSceneColor does that without
         * the caller having to know which frame slot, and therefore which scene
         * color texture, this frame will use.
         */
        FrameSceneColor
    };

    struct NLS_RENDER_API PostSubmitTextureReadbackRequest
    {
        std::shared_ptr<RHI::RHITexture> texture;
        uint32_t x = 0u;
        uint32_t y = 0u;
        uint32_t width = 0u;
        uint32_t height = 0u;
        Settings::EPixelDataFormat format = Settings::EPixelDataFormat::RGBA;
        Settings::EPixelDataType type = Settings::EPixelDataType::UNSIGNED_BYTE;
        void* destination = nullptr;
        std::shared_ptr<PostSubmitTextureReadbackState> state;
        std::shared_ptr<void> destinationKeepAlive;
        TextureReadbackSource source = TextureReadbackSource::Texture;
    };

    // Scene contents, visibility and recorded commands belong to render output.
    // A queued frame carries only its immutable view, output and per-frame requests.
    struct NLS_RENDER_API RenderFramePacket
    {
        uint64_t frameId = 0u;
        uint32_t renderWidth = 0u;
        uint32_t renderHeight = 0u;
        bool targetsSwapchain = true;
        bool hasExternalOutput = false;
        Maths::Vector4 clearColor = Core::DefaultOpaqueClearColor();
        bool clearColorBuffer = true;
        bool clearDepthBuffer = true;
        bool clearStencilBuffer = true;
        bool hasGeometryFrustum = false;
        bool hasLightFrustum = false;
        std::shared_ptr<const FrameGraph::FrozenFrameDescriptor> view;
        uint64_t externalOutputIdentity = 0u;
        std::vector<uint64_t> externalOutputIdentities;
        uint32_t externalOutputTextureCount = 0u;
        std::shared_ptr<RHI::RHITextureView> externalOutputColorView;
        std::shared_ptr<RHI::RHITextureView> externalOutputDepthView;
        std::vector<PostSubmitTextureReadbackRequest> postSubmitTextureReadbacks;
    };
}
