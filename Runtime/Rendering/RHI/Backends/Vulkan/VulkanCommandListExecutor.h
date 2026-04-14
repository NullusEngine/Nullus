// Runtime/Rendering/RHI/Backends/Vulkan/VulkanCommandListExecutor.h
#pragma once
#include "Rendering/RHI/Core/RHICommandListExecutor.h"
#include "Rendering/RHI/Core/RHICommandList.h"
#include <vulkan/vulkan.h>

namespace NLS::Render::RHI::Vulkan {

class VulkanCommandListExecutor : public IRHICommandListExecutor {
public:
    VulkanCommandListExecutor(VkDevice device, VkCommandPool commandPool, VkQueue graphicsQueue);
    virtual ~VulkanCommandListExecutor() override;

    // IRHICommandListExecutor
    void Reset(RHICommandList* cmdList) override;
    void BeginRecording(RHICommandList* cmdList) override;
    void EndRecording(RHICommandList* cmdList) override;
    void Execute(RHICommandList* cmdList) override;
    const char* GetBackendName() const override { return "Vulkan"; }
    bool SupportsExplicitBarriers() const override { return true; }

private:
    void ExecuteDrawCommands(RHICommandList* cmdList);
    void ExecuteComputeCommands(RHICommandList* cmdList);
    void ExecuteCopyCommands(RHICommandList* cmdList);
    void ExecuteBarriers(RHICommandList* cmdList);
    void ExecuteRenderPass(RHICommandList* cmdList);
    void TranslateBarrier(const BarrierDesc& barrier);

    VkDevice device_;
    VkCommandPool commandPool_;
    VkQueue graphicsQueue_;
    VkCommandBuffer currentCmdBuffer_;
    std::vector<VkBufferMemoryBarrier> bufferBarriers_;
    std::vector<VkImageMemoryBarrier> imageBarriers_;
    RHICommandList* currentCommandList_ = nullptr;
};

} // namespace NLS::Render::RHI::Vulkan