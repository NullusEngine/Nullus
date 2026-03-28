#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <optional>
#include <unordered_map>

#include "Rendering/RHI/ExplicitRHICompat.h"
#include "Rendering/RHI/GraphicsPipelineDesc.h"
#include "Rendering/RHI/IRenderDevice.h"
#include "Rendering/Resources/BindingSetInstance.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/ShaderReflection.h"
#include "Rendering/ShaderCompiler/ShaderCompilationTypes.h"

namespace
{
    class RecordingBufferResource final : public NLS::Render::RHI::IRHIBuffer
    {
    public:
        RecordingBufferResource(uint32_t resourceId, NLS::Render::RHI::BufferType type)
            : m_resourceId(resourceId)
            , m_type(type)
        {
        }

        NLS::Render::RHI::RHIResourceType GetResourceType() const override { return NLS::Render::RHI::RHIResourceType::Buffer; }
        uint32_t GetResourceId() const override { return m_resourceId; }
        NLS::Render::RHI::BufferType GetBufferType() const override { return m_type; }
        size_t GetSize() const override { return m_size; }
        void SetSize(size_t size) override { m_size = size; }

    private:
        uint32_t m_resourceId = 0;
        NLS::Render::RHI::BufferType m_type = NLS::Render::RHI::BufferType::ShaderStorage;
        size_t m_size = 0;
    };

    class RecordingTextureResource final : public NLS::Render::RHI::IRHITexture
    {
    public:
        RecordingTextureResource(uint32_t resourceId, NLS::Render::RHI::TextureDimension dimension)
            : m_resourceId(resourceId)
        {
            m_desc.dimension = dimension;
        }

        NLS::Render::RHI::RHIResourceType GetResourceType() const override { return NLS::Render::RHI::RHIResourceType::Texture; }
        uint32_t GetResourceId() const override { return m_resourceId; }
        NLS::Render::RHI::TextureDimension GetDimension() const override { return m_desc.dimension; }
        const NLS::Render::RHI::TextureDesc& GetDesc() const override { return m_desc; }
        void SetDesc(const NLS::Render::RHI::TextureDesc& desc) override { m_desc = desc; }

    private:
        uint32_t m_resourceId = 0;
        NLS::Render::RHI::TextureDesc m_desc{};
    };

    class RecordingRenderDevice final : public NLS::Render::RHI::IRenderDevice
    {
    public:
        std::optional<NLS::Render::Data::PipelineState> Init(const NLS::Render::Settings::DriverSettings&) override
        {
            return NLS::Render::Data::PipelineState{};
        }

        void Clear(bool, bool, bool) override {}
        void ReadPixels(uint32_t, uint32_t, uint32_t, uint32_t, NLS::Render::Settings::EPixelDataFormat, NLS::Render::Settings::EPixelDataType, void*) override {}

        void DrawElements(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t indexCount) override
        {
            lastDrawMode = primitiveMode;
            lastIndexCount = indexCount;
            ++drawElementsCallCount;
        }

        void DrawElementsInstanced(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t indexCount, uint32_t instances) override
        {
            lastDrawMode = primitiveMode;
            lastIndexCount = indexCount;
            lastInstanceCount = instances;
            ++drawElementsInstancedCallCount;
        }

        void DrawArrays(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t vertexCount) override
        {
            lastDrawMode = primitiveMode;
            lastVertexCount = vertexCount;
            ++drawArraysCallCount;
        }

        void DrawArraysInstanced(NLS::Render::Settings::EPrimitiveMode primitiveMode, uint32_t vertexCount, uint32_t instances) override
        {
            lastDrawMode = primitiveMode;
            lastVertexCount = vertexCount;
            lastInstanceCount = instances;
            ++drawArraysInstancedCallCount;
        }

        void SetClearColor(float, float, float, float) override {}
        void SetRasterizationLinesWidth(float) override {}
        void SetRasterizationMode(NLS::Render::Settings::ERasterizationMode) override {}

        void SetCapability(NLS::Render::Settings::ERenderingCapability capability, bool value) override
        {
            capabilityValues[static_cast<size_t>(capability)] = value;
        }

        bool GetCapability(NLS::Render::Settings::ERenderingCapability capability) override
        {
            return capabilityValues[static_cast<size_t>(capability)];
        }

        void SetStencilAlgorithm(NLS::Render::Settings::EComparaisonAlgorithm, int32_t, uint32_t) override {}
        void SetDepthAlgorithm(NLS::Render::Settings::EComparaisonAlgorithm) override {}
        void SetStencilMask(uint32_t) override {}
        void SetStencilOperations(NLS::Render::Settings::EOperation, NLS::Render::Settings::EOperation, NLS::Render::Settings::EOperation) override {}

        void SetCullFace(NLS::Render::Settings::ECullFace cullFace) override
        {
            lastCullFace = cullFace;
        }

        void SetDepthWriting(bool enable) override
        {
            depthWritingEnabled = enable;
        }

        void SetColorWriting(bool enableRed, bool enableGreen, bool enableBlue, bool enableAlpha) override
        {
            colorWritingMask = { enableRed, enableGreen, enableBlue, enableAlpha };
        }

        void SetViewport(uint32_t, uint32_t, uint32_t, uint32_t) override {}

        void BindGraphicsPipeline(const NLS::Render::RHI::GraphicsPipelineDesc& pipelineDesc, const NLS::Render::Resources::BindingSetInstance* bindingSet) override
        {
            boundPipelineDesc = pipelineDesc;
            if (bindingSet != nullptr)
            {
                boundBindingSetStorage = *bindingSet;
                boundBindingSet = &boundBindingSetStorage;
            }
            else
            {
                boundBindingSet = nullptr;
            }
            ++bindGraphicsPipelineCallCount;
        }

        std::shared_ptr<NLS::Render::RHI::IRHITexture> CreateTextureResource(NLS::Render::RHI::TextureDimension dimension) override
        {
            const auto texture = std::make_shared<RecordingTextureResource>(++m_nextTextureId, dimension);
            m_textures[texture->GetResourceId()] = texture;
            return texture;
        }

        uint32_t CreateTexture() override { return ++m_nextTextureId; }
        void DestroyTexture(uint32_t) override {}
        void BindTexture(NLS::Render::RHI::TextureDimension, uint32_t) override {}
        void ActivateTexture(uint32_t) override {}
        void SetupTexture(const NLS::Render::RHI::TextureDesc&, const void*) override {}
        void GenerateTextureMipmap(NLS::Render::RHI::TextureDimension) override {}

        uint32_t CreateFramebuffer() override { return 0; }
        void DestroyFramebuffer(uint32_t) override {}
        void BindFramebuffer(uint32_t framebufferId) override { lastFramebufferId = framebufferId; }
        void AttachFramebufferColorTexture(uint32_t, uint32_t, uint32_t) override {}
        void AttachFramebufferDepthStencilTexture(uint32_t, uint32_t) override {}
        void SetFramebufferDrawBufferCount(uint32_t, uint32_t) override {}
        void BlitDepth(uint32_t, uint32_t, uint32_t, uint32_t) override {}

        std::shared_ptr<NLS::Render::RHI::IRHIBuffer> CreateBufferResource(NLS::Render::RHI::BufferType type) override
        {
            const auto buffer = std::make_shared<RecordingBufferResource>(++m_nextBufferId, type);
            m_buffers[buffer->GetResourceId()] = buffer;
            return buffer;
        }

        uint32_t CreateBuffer() override { return ++m_nextBufferId; }
        void DestroyBuffer(uint32_t) override {}

        void BindBuffer(NLS::Render::RHI::BufferType type, uint32_t bufferId) override
        {
            m_boundBuffers[static_cast<size_t>(type)] = m_buffers.contains(bufferId) ? m_buffers[bufferId] : nullptr;
        }

        void BindBufferBase(NLS::Render::RHI::BufferType, uint32_t, uint32_t) override {}

        void SetBufferData(NLS::Render::RHI::BufferType type, size_t size, const void*, NLS::Render::RHI::BufferUsage) override
        {
            if (const auto& buffer = m_boundBuffers[static_cast<size_t>(type)])
                buffer->SetSize(size);
        }

        void SetBufferSubData(NLS::Render::RHI::BufferType, size_t, size_t, const void*) override {}
        void* GetUITextureHandle(uint32_t) const override { return nullptr; }
        void ReleaseUITextureHandles() override {}
        bool PrepareUIRender() override { return false; }

        std::string GetVendor() override { return "TestVendor"; }
        std::string GetHardware() override { return "TestHardware"; }
        std::string GetVersion() override { return "TestVersion"; }
        std::string GetShadingLanguageVersion() override { return "TestSL"; }

        NLS::Render::RHI::RHIDeviceCapabilities GetCapabilities() const override
        {
            NLS::Render::RHI::RHIDeviceCapabilities capabilities;
            capabilities.backendReady = true;
            capabilities.supportsGraphics = true;
            return capabilities;
        }

        NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override
        {
            NLS::Render::RHI::NativeRenderDeviceInfo info;
            info.backend = nativeBackend;
            return info;
        }
        bool IsBackendReady() const override { return true; }
        bool CreateSwapchain(const NLS::Render::RHI::SwapchainDesc&) override { return false; }
        void DestroySwapchain() override {}
        void ResizeSwapchain(uint32_t, uint32_t) override {}
        void PresentSwapchain() override {}

        std::optional<NLS::Render::RHI::GraphicsPipelineDesc> boundPipelineDesc;
        const NLS::Render::Resources::BindingSetInstance* boundBindingSet = nullptr;
        NLS::Render::Resources::BindingSetInstance boundBindingSetStorage;
        NLS::Render::Settings::EPrimitiveMode lastDrawMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;
        NLS::Render::Settings::ECullFace lastCullFace = NLS::Render::Settings::ECullFace::BACK;
        std::array<bool, 4> colorWritingMask{ true, true, true, true };
        std::array<bool, 16> capabilityValues{};
        uint32_t lastVertexCount = 0;
        uint32_t lastIndexCount = 0;
        uint32_t lastInstanceCount = 0;
        uint32_t lastFramebufferId = 0;
        uint32_t drawArraysCallCount = 0;
        uint32_t drawArraysInstancedCallCount = 0;
        uint32_t drawElementsCallCount = 0;
        uint32_t drawElementsInstancedCallCount = 0;
        uint32_t bindGraphicsPipelineCallCount = 0;
        bool depthWritingEnabled = true;
        NLS::Render::RHI::NativeBackendType nativeBackend = NLS::Render::RHI::NativeBackendType::DX11;

    private:
        uint32_t m_nextBufferId = 0;
        uint32_t m_nextTextureId = 0;
        std::unordered_map<uint32_t, std::shared_ptr<RecordingBufferResource>> m_buffers;
        std::unordered_map<uint32_t, std::shared_ptr<RecordingTextureResource>> m_textures;
        std::array<std::shared_ptr<RecordingBufferResource>, 4> m_boundBuffers{};
    };
}

TEST(FormalRHICompatibilityTests, CompatibilityQueueSubmissionTranslatesFormalDX11PipelineAndBindingsToLegacyBindCall)
{
    RecordingRenderDevice renderDevice;
    const auto device = NLS::Render::RHI::CreateCompatibilityExplicitDevice(renderDevice);

    ASSERT_NE(device, nullptr);
    EXPECT_EQ(device->GetAdapter()->GetBackendType(), NLS::Render::RHI::NativeBackendType::DX11);

    NLS::Render::Resources::ShaderReflection reflection;
    reflection.constantBuffers.push_back({
        "Globals",
        NLS::Render::ShaderCompiler::ShaderStage::Vertex,
        0,
        0,
        64,
        {}
    });
    reflection.properties.push_back({
        "LinearSampler",
        NLS::Render::Resources::UniformType::UNIFORM_FLOAT,
        NLS::Render::Resources::ShaderResourceKind::Sampler,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        0,
        1,
        -1,
        1,
        0,
        0,
        {}
    });

    NLS::Render::RHI::RHIBindingLayoutDesc bindingLayoutDesc;
    bindingLayoutDesc.debugName = "TestLayout";
    bindingLayoutDesc.entries.push_back({ "Globals", NLS::Render::RHI::BindingType::UniformBuffer, 0, 0, 1, NLS::Render::RHI::ShaderStageMask::Vertex });
    bindingLayoutDesc.entries.push_back({ "LinearSampler", NLS::Render::RHI::BindingType::Sampler, 0, 1, 1, NLS::Render::RHI::ShaderStageMask::Fragment });

    const auto bindingLayout = device->CreateBindingLayout(bindingLayoutDesc);
    ASSERT_NE(bindingLayout, nullptr);

    NLS::Render::RHI::RHIBufferDesc bufferDesc;
    bufferDesc.size = 64;
    bufferDesc.usage = NLS::Render::RHI::BufferUsageFlags::Uniform;
    bufferDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
    bufferDesc.debugName = "GlobalsBuffer";

    const auto buffer = device->CreateBuffer(bufferDesc, nullptr);
    ASSERT_NE(buffer, nullptr);

    NLS::Render::RHI::SamplerDesc samplerDesc;
    samplerDesc.minFilter = NLS::Render::RHI::TextureFilter::Linear;
    samplerDesc.magFilter = NLS::Render::RHI::TextureFilter::Linear;
    const auto sampler = device->CreateSampler(samplerDesc, "LinearSampler");
    ASSERT_NE(sampler, nullptr);

    NLS::Render::RHI::RHIBindingSetDesc bindingSetDesc;
    bindingSetDesc.layout = bindingLayout;
    bindingSetDesc.debugName = "TestBindingSet";
    bindingSetDesc.entries.push_back({ 0, NLS::Render::RHI::BindingType::UniformBuffer, buffer, 0, 64, nullptr, nullptr });
    bindingSetDesc.entries.push_back({ 1, NLS::Render::RHI::BindingType::Sampler, nullptr, 0, 0, nullptr, sampler });

    const auto bindingSet = device->CreateBindingSet(bindingSetDesc);
    ASSERT_NE(bindingSet, nullptr);

    NLS::Render::RHI::RHIPipelineLayoutDesc pipelineLayoutDesc;
    pipelineLayoutDesc.debugName = "TestPipelineLayout";
    pipelineLayoutDesc.bindingLayouts.push_back(bindingLayout);

    const auto pipelineLayout = device->CreatePipelineLayout(pipelineLayoutDesc);
    ASSERT_NE(pipelineLayout, nullptr);

    NLS::Render::RHI::RHIShaderModuleDesc vertexShaderDesc;
    vertexShaderDesc.stage = NLS::Render::RHI::ShaderStage::Vertex;
    vertexShaderDesc.targetBackend = NLS::Render::RHI::NativeBackendType::DX11;
    vertexShaderDesc.entryPoint = "VSMain";
    vertexShaderDesc.bytecode = { 0x01, 0x02 };
    vertexShaderDesc.debugName = "VertexShader";

    NLS::Render::RHI::RHIShaderModuleDesc fragmentShaderDesc;
    fragmentShaderDesc.stage = NLS::Render::RHI::ShaderStage::Fragment;
    fragmentShaderDesc.targetBackend = NLS::Render::RHI::NativeBackendType::DX11;
    fragmentShaderDesc.entryPoint = "PSMain";
    fragmentShaderDesc.bytecode = { 0x03, 0x04 };
    fragmentShaderDesc.debugName = "PixelShader";

    const auto vertexShader = device->CreateShaderModule(vertexShaderDesc);
    const auto fragmentShader = device->CreateShaderModule(fragmentShaderDesc);
    ASSERT_NE(vertexShader, nullptr);
    ASSERT_NE(fragmentShader, nullptr);

    NLS::Render::RHI::RHIGraphicsPipelineDesc pipelineDesc;
    pipelineDesc.pipelineLayout = pipelineLayout;
    pipelineDesc.vertexShader = vertexShader;
    pipelineDesc.fragmentShader = fragmentShader;
    pipelineDesc.reflection = &reflection;
    pipelineDesc.rasterState.cullEnabled = false;
    pipelineDesc.blendState.enabled = true;
    pipelineDesc.blendState.colorWrite = true;
    pipelineDesc.depthStencilState.depthTest = true;
    pipelineDesc.depthStencilState.depthWrite = false;
    pipelineDesc.primitiveTopology = NLS::Render::RHI::PrimitiveTopology::TriangleList;
    pipelineDesc.renderTargetLayout.colorFormats = { NLS::Render::RHI::TextureFormat::RGBA8 };
    pipelineDesc.renderTargetLayout.depthFormat = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
    pipelineDesc.renderTargetLayout.hasDepth = true;
    pipelineDesc.renderTargetLayout.sampleCount = 1;
    pipelineDesc.debugName = "FormalPipeline";

    const auto pipeline = device->CreateGraphicsPipeline(pipelineDesc);
    ASSERT_NE(pipeline, nullptr);

    const auto commandPool = device->CreateCommandPool(NLS::Render::RHI::QueueType::Graphics, "TestPool");
    ASSERT_NE(commandPool, nullptr);

    const auto commandBuffer = commandPool->CreateCommandBuffer("TestCommandBuffer");
    ASSERT_NE(commandBuffer, nullptr);

    commandBuffer->Begin();
    commandBuffer->BindGraphicsPipeline(pipeline);
    commandBuffer->BindBindingSet(0, bindingSet);
    commandBuffer->DrawIndexed(36);
    commandBuffer->End();

    NLS::Render::RHI::RHISubmitDesc submitDesc;
    submitDesc.commandBuffers.push_back(commandBuffer);

    const auto queue = device->GetQueue(NLS::Render::RHI::QueueType::Graphics);
    ASSERT_NE(queue, nullptr);
    queue->Submit(submitDesc);

    ASSERT_TRUE(renderDevice.boundPipelineDesc.has_value());
    EXPECT_EQ(renderDevice.bindGraphicsPipelineCallCount, 1u);
    ASSERT_EQ(renderDevice.boundPipelineDesc->shaderStages.size(), 2u);
    EXPECT_EQ(renderDevice.boundPipelineDesc->shaderStages[0].targetPlatform, NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL);
    EXPECT_EQ(renderDevice.boundPipelineDesc->shaderStages[1].targetPlatform, NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL);
    EXPECT_EQ(renderDevice.boundPipelineDesc->layout.uniformBufferBindingCount, 1u);
    EXPECT_EQ(renderDevice.boundPipelineDesc->layout.samplerBindingCount, 1u);
    EXPECT_EQ(renderDevice.boundPipelineDesc->layout.storageBufferBindingCount, 0u);
    EXPECT_FALSE(renderDevice.boundPipelineDesc->rasterState.culling);
    EXPECT_FALSE(renderDevice.boundPipelineDesc->depthStencilState.depthWrite);
    EXPECT_TRUE(renderDevice.boundPipelineDesc->blendState.enabled);

    ASSERT_NE(renderDevice.boundBindingSet, nullptr);
    const auto* globalsBinding = renderDevice.boundBindingSet->Find("Globals");
    ASSERT_NE(globalsBinding, nullptr);
    EXPECT_EQ(globalsBinding->bindingSpace, 0u);
    EXPECT_EQ(globalsBinding->bindingIndex, 0u);
    EXPECT_NE(globalsBinding->bufferResource, nullptr);
    EXPECT_EQ(globalsBinding->bufferResource->GetSize(), 64u);

    const auto* samplerBinding = renderDevice.boundBindingSet->Find("LinearSampler");
    ASSERT_NE(samplerBinding, nullptr);
    EXPECT_TRUE(samplerBinding->hasSampler);
    EXPECT_EQ(samplerBinding->bindingSpace, 0u);
    EXPECT_EQ(samplerBinding->bindingIndex, 1u);
    EXPECT_EQ(samplerBinding->sampler.minFilter, NLS::Render::RHI::TextureFilter::Linear);

    EXPECT_EQ(renderDevice.lastDrawMode, NLS::Render::Settings::EPrimitiveMode::TRIANGLES);
    EXPECT_EQ(renderDevice.lastIndexCount, 36u);
    EXPECT_EQ(renderDevice.drawElementsCallCount, 1u);
}

TEST(FormalRHICompatibilityTests, MaterialBuildExplicitPipelineStateBundlesFormalPipelineInputs)
{
    RecordingRenderDevice renderDevice;
    renderDevice.nativeBackend = NLS::Render::RHI::NativeBackendType::DX12;

    const auto device = NLS::Render::RHI::CreateCompatibilityExplicitDevice(renderDevice);
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(device->GetNativeDeviceInfo().backend, NLS::Render::RHI::NativeBackendType::DX12);

    NLS::Render::RHI::RHIBindingLayoutDesc bindingLayoutDesc;
    bindingLayoutDesc.debugName = "MaterialPipelineLayout";
    const auto bindingLayout = device->CreateBindingLayout(bindingLayoutDesc);
    ASSERT_NE(bindingLayout, nullptr);

    NLS::Render::RHI::RHIPipelineLayoutDesc pipelineLayoutDesc;
    pipelineLayoutDesc.debugName = "MaterialPipelineLayout";
    pipelineLayoutDesc.bindingLayouts.push_back(bindingLayout);
    const auto pipelineLayout = device->CreatePipelineLayout(pipelineLayoutDesc);
    ASSERT_NE(pipelineLayout, nullptr);

    NLS::Render::RHI::RHIShaderModuleDesc vertexShaderDesc;
    vertexShaderDesc.stage = NLS::Render::RHI::ShaderStage::Vertex;
    vertexShaderDesc.targetBackend = NLS::Render::RHI::NativeBackendType::DX12;
    vertexShaderDesc.entryPoint = "VSMain";
    vertexShaderDesc.bytecode = { 0x01, 0x02, 0x03, 0x04 };
    vertexShaderDesc.debugName = "UnitVertex";
    const auto vertexShader = device->CreateShaderModule(vertexShaderDesc);
    ASSERT_NE(vertexShader, nullptr);

    NLS::Render::RHI::RHIShaderModuleDesc fragmentShaderDesc;
    fragmentShaderDesc.stage = NLS::Render::RHI::ShaderStage::Fragment;
    fragmentShaderDesc.targetBackend = NLS::Render::RHI::NativeBackendType::DX12;
    fragmentShaderDesc.entryPoint = "PSMain";
    fragmentShaderDesc.bytecode = { 0x05, 0x06, 0x07, 0x08 };
    fragmentShaderDesc.debugName = "UnitPixel";
    const auto fragmentShader = device->CreateShaderModule(fragmentShaderDesc);
    ASSERT_NE(fragmentShader, nullptr);

    NLS::Render::Resources::Material material(nullptr);
    material.SetBlendable(true);
    material.SetDepthWriting(false);
    material.SetBackfaceCulling(false);

    const auto explicitState = material.BuildExplicitPipelineState(
        pipelineLayout,
        vertexShader,
        fragmentShader,
        NLS::Render::Settings::EPrimitiveMode::LINES,
        NLS::Render::Settings::EComparaisonAlgorithm::GREATER);

    EXPECT_TRUE(explicitState.IsComplete());
    ASSERT_NE(explicitState.pipelineLayout, nullptr);
    ASSERT_NE(explicitState.vertexShader, nullptr);
    ASSERT_NE(explicitState.fragmentShader, nullptr);
    EXPECT_EQ(explicitState.pipelineDesc.pipelineLayout, explicitState.pipelineLayout);
    EXPECT_EQ(explicitState.pipelineDesc.vertexShader, explicitState.vertexShader);
    EXPECT_EQ(explicitState.pipelineDesc.fragmentShader, explicitState.fragmentShader);
    EXPECT_EQ(explicitState.pipelineDesc.primitiveTopology, NLS::Render::RHI::PrimitiveTopology::LineList);
    EXPECT_EQ(
        explicitState.pipelineDesc.depthStencilState.depthCompare,
        NLS::Render::Settings::EComparaisonAlgorithm::GREATER);
    EXPECT_TRUE(explicitState.pipelineDesc.blendState.enabled);
    EXPECT_FALSE(explicitState.pipelineDesc.depthStencilState.depthWrite);
    EXPECT_FALSE(explicitState.pipelineDesc.rasterState.cullEnabled);
    EXPECT_EQ(explicitState.pipelineDesc.reflection, nullptr);
}
