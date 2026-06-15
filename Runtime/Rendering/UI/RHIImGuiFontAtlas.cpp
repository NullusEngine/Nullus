#include "Rendering/UI/RHIImGuiFontAtlas.h"

#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIResource.h"

#include "ImGui/imgui.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace NLS::Render::UI
{
    namespace
    {
        constexpr uint32_t kDx12TextureRowPitchAlignment = 256u;
        constexpr uint32_t kDx12TexturePlacementAlignment = 512u;

        uint32_t AlignUp(const uint32_t value, const uint32_t alignment)
        {
            if (alignment == 0u)
                return value;

            return ((value + alignment - 1u) / alignment) * alignment;
        }
    }

    bool RHIImGuiFontAtlas::EnsureUploaded(
        RHI::RHIDevice& device,
        RHI::RHICommandBuffer& commandBuffer,
        const std::shared_ptr<RHI::RHIBindingLayout>& bindingLayout,
        std::string& errorMessage)
    {
        const uint64_t deviceCacheIdentity = device.GetCacheIdentity();
        if (m_deviceCacheIdentity != 0u && m_deviceCacheIdentity != deviceCacheIdentity)
            ClearCurrentResources();

        if (m_uploaded &&
            m_texture != nullptr &&
            m_textureView != nullptr &&
            m_sampler != nullptr &&
            m_bindingSet != nullptr &&
            m_deviceCacheIdentity == deviceCacheIdentity)
        {
            return true;
        }

        if (bindingLayout == nullptr)
        {
            errorMessage = "UI font atlas binding layout is not prepared";
            return false;
        }

        auto& io = ImGui::GetIO();
        if (io.Fonts == nullptr)
        {
            errorMessage = "ImGui font atlas is unavailable";
            return false;
        }

        if (!io.Fonts->IsBuilt() && !io.Fonts->Build())
        {
            errorMessage = "failed to build ImGui font atlas";
            return false;
        }

        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        if (pixels == nullptr || width <= 0 || height <= 0)
        {
            errorMessage = "ImGui font atlas did not produce RGBA pixels";
            return false;
        }

        RHI::RHITextureDesc textureDesc;
        textureDesc.extent.width = static_cast<uint32_t>(width);
        textureDesc.extent.height = static_cast<uint32_t>(height);
        textureDesc.extent.depth = 1u;
        textureDesc.format = RHI::TextureFormat::RGBA8;
        textureDesc.usage = RHI::TextureUsageFlags::Sampled | RHI::TextureUsageFlags::CopyDst;
        textureDesc.memoryUsage = RHI::MemoryUsage::GPUOnly;
        textureDesc.debugName = "RHIImGuiFontAtlas";

        auto texture = device.CreateTexture(textureDesc);
        if (texture == nullptr)
        {
            errorMessage = "failed to create UI font atlas texture";
            return false;
        }

        const uint32_t sourceRowPitch = static_cast<uint32_t>(width) * 4u;
        const uint32_t uploadRowPitch = AlignUp(sourceRowPitch, kDx12TextureRowPitchAlignment);
        const size_t uploadSize = static_cast<size_t>(uploadRowPitch) * static_cast<size_t>(height);
        RHI::RHIBufferDesc stagingDesc;
        stagingDesc.size = AlignUp(static_cast<uint32_t>(uploadSize), kDx12TexturePlacementAlignment);
        stagingDesc.usage = RHI::BufferUsageFlags::CopySrc;
        stagingDesc.memoryUsage = RHI::MemoryUsage::CPUToGPU;
        stagingDesc.debugName = "RHIImGuiFontAtlasUploadStaging";

        std::vector<uint8_t> uploadPixels(stagingDesc.size, 0u);
        for (int row = 0; row < height; ++row)
        {
            std::memcpy(
                uploadPixels.data() + static_cast<size_t>(row) * uploadRowPitch,
                pixels + static_cast<size_t>(row) * sourceRowPitch,
                sourceRowPitch);
        }

        RHI::RHIBufferUploadDesc stagingUploadDesc;
        stagingUploadDesc.data = uploadPixels.data();
        stagingUploadDesc.dataSize = uploadSize;
        stagingUploadDesc.destinationOffset = 0u;
        stagingUploadDesc.debugName = "RHIImGuiFontAtlasUpload";
        auto stagingBuffer = device.CreateBuffer(stagingDesc, stagingUploadDesc);
        if (stagingBuffer == nullptr)
        {
            errorMessage = "failed to create UI font atlas upload staging buffer";
            return false;
        }

        RHI::RHIBarrierDesc uploadBarrier;
        uploadBarrier.textureBarriers.push_back({
            texture,
            texture->GetState(),
            RHI::ResourceState::CopyDst,
            { 0u, 1u, 0u, 1u },
            RHI::PipelineStageMask::AllCommands,
            RHI::PipelineStageMask::Copy,
            RHI::AccessMask::MemoryRead | RHI::AccessMask::MemoryWrite,
            RHI::AccessMask::CopyWrite
        });
        commandBuffer.Barrier(uploadBarrier);

        RHI::RHIBufferToTextureCopyDesc copyDesc;
        copyDesc.source = stagingBuffer;
        copyDesc.destination = texture;
        copyDesc.bufferOffset = 0u;
        copyDesc.mipLevel = 0u;
        copyDesc.arrayLayer = 0u;
        copyDesc.textureOffset = {};
        copyDesc.extent = textureDesc.extent;
        copyDesc.rowPitch = uploadRowPitch;
        copyDesc.slicePitch = uploadRowPitch * static_cast<uint32_t>(height);
        commandBuffer.CopyBufferToTexture(copyDesc);

        RHI::RHITextureViewDesc viewDesc;
        viewDesc.viewType = RHI::TextureViewType::Texture2D;
        viewDesc.format = RHI::TextureFormat::RGBA8;
        viewDesc.colorSpace = RHI::TextureColorSpace::Linear;
        viewDesc.subresourceRange = { 0u, 1u, 0u, 1u };
        viewDesc.debugName = "RHIImGuiFontAtlasView";
        auto textureView = device.CreateTextureView(texture, viewDesc);
        if (textureView == nullptr)
        {
            errorMessage = "failed to create UI font atlas texture view";
            return false;
        }

        RHI::SamplerDesc samplerDesc;
        samplerDesc.minFilter = RHI::TextureFilter::Linear;
        samplerDesc.magFilter = RHI::TextureFilter::Linear;
        samplerDesc.mipFilter = RHI::TextureMipFilter::Linear;
        samplerDesc.wrapU = RHI::TextureWrap::ClampToEdge;
        samplerDesc.wrapV = RHI::TextureWrap::ClampToEdge;
        samplerDesc.wrapW = RHI::TextureWrap::ClampToEdge;
        samplerDesc.maxAnisotropy = 1u;
        auto sampler = device.CreateSampler(samplerDesc, "RHIImGuiFontAtlasSampler");
        if (sampler == nullptr)
        {
            errorMessage = "failed to create UI font atlas sampler";
            return false;
        }

        RHI::RHIBindingSetDesc bindingSetDesc;
        bindingSetDesc.layout = bindingLayout;
        bindingSetDesc.debugName = "RHIImGuiFontAtlasBindingSet";
        bindingSetDesc.entries.push_back({
            0u,
            RHI::BindingType::Texture,
            nullptr,
            0u,
            0u,
            0u,
            textureView,
            nullptr
        });
        bindingSetDesc.entries.push_back({
            1u,
            RHI::BindingType::Sampler,
            nullptr,
            0u,
            0u,
            0u,
            nullptr,
            sampler
        });
        auto bindingSet = device.CreateBindingSet(bindingSetDesc);
        if (bindingSet == nullptr)
        {
            errorMessage = "failed to create UI font atlas binding set";
            return false;
        }

        m_texture = std::move(texture);
        m_textureView = std::move(textureView);
        m_sampler = std::move(sampler);
        m_bindingSet = std::move(bindingSet);
        m_uploadStagingBuffer = std::move(stagingBuffer);
        m_deviceCacheIdentity = deviceCacheIdentity;
        m_uploaded = true;
        return true;
    }

    void RHIImGuiFontAtlas::Invalidate(const uint64_t retireFrameId)
    {
        ++m_generation;
        m_uploaded = false;
        RetireCurrentResources(retireFrameId);
    }

    void RHIImGuiFontAtlas::ReleaseRetiredResourcesUpTo(const uint64_t completedFrameId)
    {
        m_retiredResources.erase(
            std::remove_if(
                m_retiredResources.begin(),
                m_retiredResources.end(),
                [completedFrameId](const RHIImGuiFontAtlasRetiredResources& resources)
                {
                    return resources.retireFrameId <= completedFrameId;
                }),
            m_retiredResources.end());
    }

    void RHIImGuiFontAtlas::RetireCurrentResources(const uint64_t retireFrameId)
    {
        if (m_texture != nullptr ||
            m_textureView != nullptr ||
            m_sampler != nullptr ||
            m_bindingSet != nullptr)
        {
            m_retiredResources.push_back({
                retireFrameId,
                std::move(m_texture),
                std::move(m_textureView),
                std::move(m_sampler),
                std::move(m_bindingSet),
                std::move(m_uploadStagingBuffer)
            });
        }

        m_texture.reset();
        m_textureView.reset();
        m_sampler.reset();
        m_bindingSet.reset();
        m_uploadStagingBuffer.reset();
        m_deviceCacheIdentity = 0u;
    }

    void RHIImGuiFontAtlas::ClearCurrentResources()
    {
        m_uploaded = false;
        m_texture.reset();
        m_textureView.reset();
        m_sampler.reset();
        m_bindingSet.reset();
        m_uploadStagingBuffer.reset();
        m_deviceCacheIdentity = 0u;
    }

#if defined(NLS_ENABLE_TEST_HOOKS)
    void RHIImGuiFontAtlas::SetUploadedResourcesForTesting(
        std::shared_ptr<RHI::RHITexture> texture,
        std::shared_ptr<RHI::RHITextureView> textureView,
        std::shared_ptr<RHI::RHISampler> sampler,
        std::shared_ptr<RHI::RHIBindingSet> bindingSet)
    {
        m_texture = std::move(texture);
        m_textureView = std::move(textureView);
        m_sampler = std::move(sampler);
        m_bindingSet = std::move(bindingSet);
        m_uploadStagingBuffer.reset();
        m_deviceCacheIdentity = 0u;
        m_uploaded = m_texture != nullptr ||
            m_textureView != nullptr ||
            m_sampler != nullptr ||
            m_bindingSet != nullptr;
    }
#endif
}
