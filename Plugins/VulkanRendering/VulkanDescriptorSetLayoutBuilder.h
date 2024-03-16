#pragma once
#include "VulkanRenderer.h"

namespace NLS
{
namespace Rendering
{
class VulkanDescriptorSetLayoutBuilder
{
public:
    VulkanDescriptorSetLayoutBuilder(){};
    ~VulkanDescriptorSetLayoutBuilder(){};

    VulkanDescriptorSetLayoutBuilder& WithSamplers(unsigned int count, vk::ShaderStageFlags inShaders);
    VulkanDescriptorSetLayoutBuilder& WithUniformBuffers(unsigned int count, vk::ShaderStageFlags inShaders);

    VulkanDescriptorSetLayoutBuilder& WithDebugName(const string& name);

    vk::DescriptorSetLayout Build(VulkanRenderer& renderer);

protected:
    string debugName;
    vector<vk::DescriptorSetLayoutBinding> addedBindings;
};
} // namespace Rendering
} // namespace NLS
