#include "VulkanShader.h"
// #include "glslangtoSpv.h"
#include "Assets.h"

#include <fstream>
#include <iostream>
#include <string>
#include <iosfwd>
#include <filesystem>

using std::ifstream;

using namespace NLS;
using namespace Rendering;

// These have both been ordered to match the ShaderStages enum for easy lookup!
vk::ShaderStageFlagBits stageTypes[] = {
    vk::ShaderStageFlagBits::eVertex,
    vk::ShaderStageFlagBits::eFragment,
    vk::ShaderStageFlagBits::eGeometry,
    vk::ShaderStageFlagBits::eTessellationControl,
    vk::ShaderStageFlagBits::eTessellationEvaluation};

VulkanShader::VulkanShader()
{
    stageCount = 0;
    infos = nullptr;
}

VulkanShader::~VulkanShader()
{
    for (int i = 0; i < stageCount; ++i)
    {
        sourceDevice.destroyShaderModule(infos[i].module);
    }
    delete[] infos;
}

void VulkanShader::ReloadShader()
{
}

void VulkanShader::SetSourceDevice(vk::Device d)
{
    sourceDevice = d;
}

const char* ComplerCommand = "glslc.exe -fshader-stage=%s \"%s\" -o \"%s\" ";

void CompileShader(const std::string& fileName, ShaderStages stage)
{
    auto srcPath = std::filesystem::absolute(Assets::SHADERDIR + fileName);
    auto targetSpvPath = std::filesystem::absolute(Assets::SHADERDIR + "VK/" + fileName);

    auto command = std::string(ComplerCommand);

    const char* shaderstage = "";
    switch (stage)
    {
        case ShaderStages::SHADER_VERTEX:
            shaderstage = "vertex";
            break;
        case ShaderStages::SHADER_FRAGMENT:
            shaderstage = "fragment";
            break;
        case ShaderStages::SHADER_GEOMETRY:
            shaderstage = "geometry";
            break;
        default: // Should never happen
            break;
    }

    if (!std::filesystem::exists(Assets::SHADERDIR + "VK/"))
    {
        std::filesystem::create_directory(Assets::SHADERDIR + "VK/");
    }

    std::vector<char> buf(1 + std::snprintf(nullptr, 0, ComplerCommand, shaderstage, srcPath.string().c_str(), targetSpvPath.string().c_str()));
    std::snprintf(buf.data(), buf.size(), ComplerCommand, shaderstage, srcPath.string().c_str(), targetSpvPath.string().c_str());

    std::system(buf.data());
}

void VulkanShader::AddBinaryShaderModule(const string& fromFile, ShaderStages stage)
{
    CompileShader(fromFile, stage);

    char* data = nullptr;
    size_t dataSize = 0;
    Assets::ReadBinaryFile(Assets::SHADERDIR + "VK/" + fromFile, &data, dataSize);

    if (data)
    {
        CreateShaderModule(data, dataSize, shaderModules[(int)stage], sourceDevice);
    }
    else
    {
        std::cout << __FUNCTION__ << " Problem loading shader file " << fromFile << "!\n";
    }
}

void VulkanShader::Init()
{
    stageCount = 0;
    for (int i = 0; i < (int)ShaderStages::SHADER_MAX; ++i)
    {
        if (shaderModules[i])
        {
            stageCount++;
        }
    }
    infos = new vk::PipelineShaderStageCreateInfo[stageCount];

    int doneCount = 0;
    for (int i = 0; i < (int)ShaderStages::SHADER_MAX; ++i)
    {
        if (shaderModules[i])
        {
            infos[doneCount].stage = stageTypes[i];
            infos[doneCount].module = shaderModules[i];
            infos[doneCount].pName = "main";

            doneCount++;
            if (doneCount >= stageCount)
            {
                break;
            }
        }
    }
}

vk::ShaderModule VulkanShader::GetShaderModule(ShaderStages stage) const
{
    return shaderModules[(int)stage];
}

bool VulkanShader::CreateShaderModule(char* data, size_t size, vk::ShaderModule& into, vk::Device& device)
{
    into = device.createShaderModule(vk::ShaderModuleCreateInfo{
        .codeSize = size,
        .pCode = reinterpret_cast<const uint32_t*>(data),
    });
    return true;
}

void VulkanShader::FillShaderStageCreateInfo(vk::GraphicsPipelineCreateInfo& info) const
{
    info.setStageCount(stageCount);
    info.setPStages(infos);
}