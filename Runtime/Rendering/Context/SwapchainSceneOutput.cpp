#include "Rendering/Context/SwapchainSceneOutput.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ServiceLocator.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"

namespace NLS::Render::Context
{
    void SwapchainSceneOutput::EnsurePipeline(
        const std::shared_ptr<RHI::RHIDevice>& device, const RHI::TextureFormat format)
    {
        if (m_pipeline != nullptr && m_deviceIdentity == device->GetCacheIdentity() && m_outputFormat == format)
            return;

        Resources::Shader* shader = nullptr;
        if (::NLS::Core::ServiceLocator::Contains<::NLS::Core::ResourceManagement::ShaderManager>())
            shader = ::NLS::Core::ServiceLocator::Get<::NLS::Core::ResourceManagement::ShaderManager>().GetResource(
                ":Shaders/SwapchainOutput.hlsl", true);
        else
        {
            if (m_ownedShader == nullptr)
                m_ownedShader = std::shared_ptr<Resources::Shader>(Resources::Loaders::ShaderLoader::CreateBuiltInHlsl(
                    "App/Assets/Engine/Shaders/SwapchainOutput.hlsl"), [](Resources::Shader* owned)
                    { if (owned != nullptr) (void)Resources::Loaders::ShaderLoader::Destroy(owned); });
            shader = m_ownedShader.get();
        }
        if (shader == nullptr)
            throw std::runtime_error("Failed to load SwapchainOutput.hlsl");

        RHI::RHIBindingLayoutDesc bindingLayoutDesc;
        bindingLayoutDesc.debugName = "SwapchainOutputLayout";
        bindingLayoutDesc.entries = {
            { "SceneColorTexture", RHI::BindingType::Texture, 0u, 0u, 1u, RHI::ShaderStageMask::Fragment, 0u, 0u },
            { "SceneColorSampler", RHI::BindingType::Sampler, 0u, 1u, 1u, RHI::ShaderStageMask::Fragment, 0u, 0u }
        };
        auto bindingLayout = device->CreateBindingLayout(bindingLayoutDesc);
        RHI::RHIPipelineLayoutDesc pipelineLayoutDesc;
        pipelineLayoutDesc.debugName = "SwapchainOutputPipelineLayout";
        pipelineLayoutDesc.bindingLayouts.push_back(bindingLayout);
        auto layout = bindingLayout != nullptr ? device->CreatePipelineLayout(pipelineLayoutDesc) : nullptr;

        RHI::RHIGraphicsPipelineDesc pipelineDesc;
        pipelineDesc.debugName = "SwapchainOutputPipeline";
        pipelineDesc.pipelineLayout = layout;
        pipelineDesc.vertexShader = shader->GetOrCreateExplicitShaderModule(device, ShaderCompiler::ShaderStage::Vertex);
        pipelineDesc.fragmentShader = shader->GetOrCreateExplicitShaderModule(device, ShaderCompiler::ShaderStage::Pixel);
        pipelineDesc.rasterState.cullEnabled = false;
        pipelineDesc.depthStencilState.depthTest = false;
        pipelineDesc.depthStencilState.depthWrite = false;
        pipelineDesc.renderTargetLayout.colorFormats = { format };
        auto pipeline = layout != nullptr && pipelineDesc.vertexShader != nullptr && pipelineDesc.fragmentShader != nullptr
            ? device->CreateGraphicsPipeline(pipelineDesc) : nullptr;

        RHI::SamplerDesc samplerDesc;
        samplerDesc.wrapU = samplerDesc.wrapV = samplerDesc.wrapW = RHI::TextureWrap::ClampToEdge;
        samplerDesc.minFilter = samplerDesc.magFilter = RHI::TextureFilter::Nearest;
        samplerDesc.maxLod = 0.0f;
        auto sampler = device->CreateSampler(samplerDesc, "SwapchainOutputSampler");
        if (pipeline == nullptr || sampler == nullptr)
            throw std::runtime_error("Failed to prepare swapchain output pipeline");

        m_bindingLayout = std::move(bindingLayout);
        m_sampler = std::move(sampler);
        m_pipeline = std::move(pipeline);
        m_deviceIdentity = device->GetCacheIdentity();
        m_outputFormat = format;
    }

    void SwapchainSceneOutput::Prepare(Driver& driver, RHI::RHIFrameContext& frame, RenderScenePackage& package,
        const RHI::TextureFormat outputFormat)
    {
        const auto device = DriverRendererAccess::GetExplicitDevice(driver);
        EnsurePipeline(device, outputFormat);

        const auto matches = [&](const std::shared_ptr<RHI::RHITexture>& texture)
        {
            return texture != nullptr && texture->GetDesc().extent.width == package.renderWidth &&
                texture->GetDesc().extent.height == package.renderHeight;
        };
        if (!matches(frame.sceneColorTexture) || !matches(frame.sceneDepthStencilTexture) ||
            frame.sceneOutputBindingSet == nullptr || frame.sceneOutputBindingSet->GetDesc().layout != m_bindingLayout)
        {
            RHI::RHITextureDesc colorDesc;
            colorDesc.extent = { package.renderWidth, package.renderHeight, 1u };
            colorDesc.format = RHI::TextureFormat::RGBA8;
            colorDesc.usage = RHI::TextureUsageFlags::ColorAttachment | RHI::TextureUsageFlags::Sampled |
                RHI::TextureUsageFlags::CopySrc;
            colorDesc.debugName = "FrameSceneColor";
            auto color = device->CreateTexture(colorDesc);
            auto depthDesc = colorDesc;
            depthDesc.format = RHI::TextureFormat::Depth24Stencil8;
            depthDesc.usage = RHI::TextureUsageFlags::DepthStencilAttachment;
            depthDesc.debugName = "FrameSceneDepth";
            auto depth = device->CreateTexture(depthDesc);
            RHI::RHITextureViewDesc viewDesc;
            viewDesc.viewType = RHI::TextureViewType::Texture2D;
            viewDesc.format = colorDesc.format;
            viewDesc.debugName = "FrameSceneColorView";
            auto colorView = color != nullptr ? device->CreateTextureView(color, viewDesc) : nullptr;
            viewDesc.format = depthDesc.format;
            viewDesc.debugName = "FrameSceneDepthView";
            auto depthView = depth != nullptr ? device->CreateTextureView(depth, viewDesc) : nullptr;
            if (colorView == nullptr || depthView == nullptr)
                throw std::runtime_error("Failed to create frame scene output attachments");

            RHI::RHIBindingSetDesc bindingDesc;
            bindingDesc.layout = m_bindingLayout;
            bindingDesc.debugName = "FrameSceneOutputBinding";
            RHI::RHIBindingSetEntry textureBinding;
            textureBinding.type = RHI::BindingType::Texture;
            textureBinding.textureView = colorView;
            RHI::RHIBindingSetEntry samplerBinding;
            samplerBinding.type = RHI::BindingType::Sampler;
            samplerBinding.binding = 1u;
            samplerBinding.sampler = m_sampler;
            bindingDesc.entries = { textureBinding, samplerBinding };
            auto binding = DriverRendererAccess::CreateExplicitBindingSet(
                driver, bindingDesc, RHI::DescriptorAllocationLifetime::Persistent);
            if (binding == nullptr)
                throw std::runtime_error("Failed to create frame scene output binding");
            frame.sceneOutputBindingSet = std::move(binding);
            frame.sceneColorTexture = std::move(color);
            frame.sceneColorView = std::move(colorView);
            frame.sceneDepthStencilTexture = std::move(depth);
            frame.sceneDepthStencilView = std::move(depthView);
        }

        // Only scene passes lose their backbuffer dependency. The frame still owns
        // acquisition/Present, and the appended output pass consumes its image.
        const auto useSceneAttachments = [&frame](RenderPassCommandInput& pass)
        {
            if (!pass.targetsSwapchain)
                return;
            pass.targetsSwapchain = false;
            if (pass.usesColorAttachment)
                pass.colorAttachmentViews = { frame.sceneColorView };
            if (pass.usesDepthStencilAttachment && pass.depthStencilAttachmentView == nullptr)
                pass.depthStencilAttachmentView = frame.sceneDepthStencilView;
        };
        for (auto& pass : package.passCommandInputs)
            useSceneAttachments(pass);
        for (auto& unit : package.parallelCommandWorkUnits)
            useSceneAttachments(unit.commandInput);

        if (std::none_of(package.passCommandInputs.begin(), package.passCommandInputs.end(),
            [](const auto& pass) { return pass.usesColorAttachment; }))
        {
            RenderPassCommandInput clear;
            clear.debugName = "ForwardSceneClear";
            clear.renderWidth = package.renderWidth;
            clear.renderHeight = package.renderHeight;
            clear.usesColorAttachment = clear.clearColor = true;
            clear.clearColorValue = package.clearColorValue;
            clear.colorAttachmentViews = { frame.sceneColorView };
            auto units = BuildRecordedDrawCommandWorkUnitsForPass(clear,
                package.passCommandInputs.size(), package.parallelCommandWorkUnits.size());
            package.passCommandInputs.push_back(std::move(clear));
            package.parallelCommandWorkUnits.insert(package.parallelCommandWorkUnits.end(),
                std::make_move_iterator(units.begin()), std::make_move_iterator(units.end()));
        }

        RenderPassCommandInput output;
        output.kind = RenderPassCommandKind::SwapchainOutput;
        output.debugName = kSwapchainOutputRenderPassDebugName;
        output.drawCount = 1u;
        output.targetsSwapchain = true;
        output.renderWidth = package.renderWidth;
        output.renderHeight = package.renderHeight;
        output.usesColorAttachment = true;
        output.outputPipeline = m_pipeline;
        output.outputBindingSet = frame.sceneOutputBindingSet;
        TextureResourceAccess backbuffer;
        backbuffer.mode = ResourceAccessMode::Write;
        backbuffer.state = RHI::ResourceState::RenderTarget;
        backbuffer.stages = RHI::PipelineStageMask::RenderTarget;
        backbuffer.access = RHI::AccessMask::ColorAttachmentWrite;
        output.textureResourceAccesses.push_back(backbuffer);
        TextureResourceAccess sampledColor;
        sampledColor.texture = frame.sceneColorTexture;
        sampledColor.subresourceRange = frame.sceneColorView->GetDesc().subresourceRange;
        sampledColor.state = RHI::ResourceState::ShaderRead;
        sampledColor.stages = RHI::PipelineStageMask::FragmentShader;
        sampledColor.access = RHI::AccessMask::ShaderRead;
        output.textureResourceAccesses.push_back(sampledColor);

        const auto passIndex = static_cast<uint64_t>(package.passCommandInputs.size());
        const auto unitIndex = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());
        auto units = BuildRecordedDrawCommandWorkUnitsForPass(output, passIndex, unitIndex);
        if (unitIndex != 0u)
        {
            WorkUnitDependencyEdge edge;
            edge.sourceWorkUnitIndex = package.parallelCommandWorkUnits.back().workUnitIndex;
            edge.targetWorkUnitIndex = unitIndex;
            edge.kind = ThreadedDependencyKind::ResourceVisibility;
            edge.resourceKind = ThreadedDependencyResourceKind::Texture;
            auto sourceColor = sampledColor;
            sourceColor.mode = ResourceAccessMode::Write;
            sourceColor.state = RHI::ResourceState::ShaderRead;
            sourceColor.stages = RHI::PipelineStageMask::RenderTarget;
            sourceColor.access = RHI::AccessMask::ColorAttachmentWrite;
            edge.sourceTextureAccess = sourceColor;
            edge.targetTextureAccess = sampledColor;
            package.workUnitDependencyEdges.push_back(edge);
            units.front().incomingDependencyEdges.push_back(std::move(edge));
        }
        package.passCommandInputs.push_back(std::move(output));
        package.parallelCommandWorkUnits.insert(package.parallelCommandWorkUnits.end(),
            std::make_move_iterator(units.begin()), std::make_move_iterator(units.end()));
        package.passPlanCount = package.passCommandInputs.size();
        package.swapchainOutputDrawCount = 1u;
        package.drawCommandCount += package.swapchainOutputDrawCount;
        package.materialBatchCount = package.drawCommandCount;
        package.renderTargetUseCount = 3u;
        package.parallelCommandWorkUnitCount = package.parallelCommandWorkUnits.size();
        package.containsCommandInputs = package.containsParallelCommandWorkUnits = true;
        package.parallelDrawCommandBatches = BuildParallelDrawCommandBatchMetadata(package.parallelCommandWorkUnits);
    }
}
