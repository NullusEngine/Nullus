#pragma once

#include "Rendering/RHI/Core/RHIDevice.h"

#include <memory>
#include <string_view>
#include <utility>

namespace NLS::Tests
{
class DeterministicTextureRhiAdapter final : public NLS::Render::RHI::RHIAdapter
{
public:
    std::string_view GetDebugName() const override { return "DeterministicTextureRhiAdapter"; }
    NLS::Render::RHI::NativeBackendType GetBackendType() const override
    {
        return NLS::Render::RHI::NativeBackendType::None;
    }
    std::string_view GetVendor() const override { return "NullusTests"; }
    std::string_view GetHardware() const override { return "InMemoryTextureDevice"; }
};

class DeterministicRhiTexture final : public NLS::Render::RHI::RHITexture
{
public:
    explicit DeterministicRhiTexture(NLS::Render::RHI::RHITextureDesc desc)
        : m_desc(std::move(desc))
    {
    }

    std::string_view GetDebugName() const override { return m_desc.debugName; }
    const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
    NLS::Render::RHI::ResourceState GetState() const override
    {
        return NLS::Render::RHI::ResourceState::Unknown;
    }

private:
    NLS::Render::RHI::RHITextureDesc m_desc {};
};

class DeterministicRhiTextureView final : public NLS::Render::RHI::RHITextureView
{
public:
    DeterministicRhiTextureView(
        std::shared_ptr<NLS::Render::RHI::RHITexture> texture,
        NLS::Render::RHI::RHITextureViewDesc desc)
        : m_texture(std::move(texture))
        , m_desc(std::move(desc))
    {
    }

    std::string_view GetDebugName() const override { return m_desc.debugName; }
    const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
    const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override
    {
        return m_texture;
    }

private:
    std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
    NLS::Render::RHI::RHITextureViewDesc m_desc {};
};

class DeterministicTextureRhiDevice final : public NLS::Render::RHI::RHIDevice
{
public:
    using NLS::Render::RHI::RHIDevice::CreateBuffer;
    using NLS::Render::RHI::RHIDevice::CreateTexture;

    DeterministicTextureRhiDevice()
        : m_adapter(std::make_shared<DeterministicTextureRhiAdapter>())
    {
        m_nativeDeviceInfo.backend = NLS::Render::RHI::NativeBackendType::None;
        m_capabilities.backendReady = true;
        m_capabilities.supportsGraphics = true;
        m_capabilities.supportsCurrentSceneRenderer = true;
    }

    std::string_view GetDebugName() const override { return "DeterministicTextureRhiDevice"; }
    const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_adapter; }
    const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
    NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override { return m_nativeDeviceInfo; }
    bool IsBackendReady() const override { return true; }
    std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(
        const NLS::Render::RHI::SwapchainDesc&) override
    {
        return nullptr;
    }
    std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(
        const NLS::Render::RHI::RHIBufferDesc&,
        const NLS::Render::RHI::RHIBufferUploadDesc&) override
    {
        return nullptr;
    }
    std::shared_ptr<NLS::Render::RHI::RHITexture> CreateTexture(
        const NLS::Render::RHI::RHITextureDesc& desc,
        const NLS::Render::RHI::RHITextureUploadDesc& uploadDesc) override
    {
        ++textureCreateCalls;
        lastUploadHadData = uploadDesc.HasData();
        return std::make_shared<DeterministicRhiTexture>(desc);
    }
    std::shared_ptr<NLS::Render::RHI::RHITextureView> CreateTextureView(
        const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
        const NLS::Render::RHI::RHITextureViewDesc& desc) override
    {
        return std::make_shared<DeterministicRhiTextureView>(texture, desc);
    }
    std::shared_ptr<NLS::Render::RHI::RHISampler> CreateSampler(
        const NLS::Render::RHI::SamplerDesc&,
        std::string = {}) override
    {
        return nullptr;
    }
    std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateBindingLayout(
        const NLS::Render::RHI::RHIBindingLayoutDesc&) override
    {
        return nullptr;
    }
    std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateBindingSet(
        const NLS::Render::RHI::RHIBindingSetDesc&) override
    {
        return nullptr;
    }
    std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> CreatePipelineLayout(
        const NLS::Render::RHI::RHIPipelineLayoutDesc&) override
    {
        return nullptr;
    }
    std::shared_ptr<NLS::Render::RHI::RHIShaderModule> CreateShaderModule(
        const NLS::Render::RHI::RHIShaderModuleDesc&) override
    {
        return nullptr;
    }
    std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> CreateGraphicsPipeline(
        const NLS::Render::RHI::RHIGraphicsPipelineDesc&) override
    {
        return nullptr;
    }
    std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> CreateComputePipeline(
        const NLS::Render::RHI::RHIComputePipelineDesc&) override
    {
        return nullptr;
    }
    std::shared_ptr<NLS::Render::RHI::RHICommandPool> CreateCommandPool(
        NLS::Render::RHI::QueueType,
        std::string = {}) override
    {
        return nullptr;
    }
    std::shared_ptr<NLS::Render::RHI::RHIFence> CreateFence(std::string = {}) override { return nullptr; }
    std::shared_ptr<NLS::Render::RHI::RHISemaphore> CreateSemaphore(std::string = {}) override
    {
        return nullptr;
    }
    void ReadPixels(
        const std::shared_ptr<NLS::Render::RHI::RHITexture>&,
        uint32_t,
        uint32_t,
        uint32_t,
        uint32_t,
        NLS::Render::Settings::EPixelDataFormat,
        NLS::Render::Settings::EPixelDataType,
        void*) override
    {
    }

    size_t textureCreateCalls = 0u;
    bool lastUploadHadData = false;

private:
    std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
    NLS::Render::RHI::NativeRenderDeviceInfo m_nativeDeviceInfo {};
    NLS::Render::RHI::RHIDeviceCapabilities m_capabilities {};
};
} // namespace NLS::Tests
