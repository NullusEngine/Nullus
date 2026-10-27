#include "SpirvShaderReflection.h"

#include <cstring>
#include <spirv_cross.hpp>

namespace NLS::Render::Resources
{
namespace
{
UniformType ValueType(const spirv_cross::SPIRType& type)
{
    if (type.basetype == spirv_cross::SPIRType::Boolean) return UniformType::UNIFORM_BOOL;
    if (type.basetype == spirv_cross::SPIRType::Int || type.basetype == spirv_cross::SPIRType::UInt)
        return UniformType::UNIFORM_INT;
    if (type.columns == 4 && type.vecsize == 4) return UniformType::UNIFORM_FLOAT_MAT4;
    switch (type.vecsize)
    {
    case 2: return UniformType::UNIFORM_FLOAT_VEC2;
    case 3: return UniformType::UNIFORM_FLOAT_VEC3;
    case 4: return UniformType::UNIFORM_FLOAT_VEC4;
    default: return UniformType::UNIFORM_FLOAT;
    }
}
uint32_t ArraySize(const spirv_cross::SPIRType& type)
{
    uint32_t count = 1;
    for (const auto size : type.array) count *= size;
    return count;
}
}

bool ReflectSpirvShader(std::span<const uint8_t> bytecode, ShaderCompiler::ShaderStage stage,
    ShaderReflection& reflection, std::string& error)
{
    try
    {
        if (bytecode.empty() || bytecode.size() % sizeof(uint32_t))
        { error = "Invalid SPIR-V bytecode size"; return false; }
        std::vector<uint32_t> words(bytecode.size()/sizeof(uint32_t));
        std::memcpy(words.data(), bytecode.data(), bytecode.size());
        spirv_cross::Compiler compiler(std::move(words));
        const auto resources = compiler.get_shader_resources();
        ShaderReflection candidate;
        const auto mask = stage == ShaderCompiler::ShaderStage::Vertex ? RHI::ShaderStageMask::Vertex :
            stage == ShaderCompiler::ShaderStage::Compute ? RHI::ShaderStageMask::Compute : RHI::ShaderStageMask::Fragment;
        auto addBuffer = [&](const spirv_cross::Resource& resource, bool pushConstant)
        {
            const auto& type = compiler.get_type(resource.base_type_id);
            ShaderConstantBufferDesc buffer;
            buffer.name = compiler.get_name(resource.id);
            if(buffer.name.empty())buffer.name=resource.name;
            if(buffer.name.starts_with("type."))buffer.name.erase(0,5);
            const bool objectDrawConstants=pushConstant&&buffer.name=="g_NLSObjectDrawConstants";
            if(objectDrawConstants)buffer.name="ObjectIndexConstants";
            buffer.stage = stage;
            buffer.stageMask = mask;
            buffer.bindingSpace = pushConstant ? 3u : compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
            const uint32_t binding = pushConstant ? 1u : compiler.get_decoration(resource.id, spv::DecorationBinding);
            const uint32_t shift = 8u + buffer.bindingSpace*4u;
            if (!pushConstant && binding < shift) throw std::runtime_error("Invalid SPIR-V cbuffer register shift");
            buffer.bindingIndex = pushConstant ? 1u : binding-shift;
            buffer.byteSize = static_cast<uint32_t>(compiler.get_declared_struct_size(type));
            for (uint32_t index = 0; index < type.member_types.size(); ++index)
            {
                const auto& memberType = compiler.get_type(type.member_types[index]);
                ShaderCBufferMemberDesc member;
                member.name = compiler.get_member_name(resource.base_type_id, index);
                if(objectDrawConstants)
                {
                    if(member.name=="objectIndex")member.name="u_ObjectIndex";
                    else if(member.name=="objectFlags")member.name="u_ObjectFlags";
                    else if(member.name=="objectPadding0")member.name="u_ObjectPadding0";
                    else if(member.name=="objectPadding1")member.name="u_ObjectPadding1";
                }
                member.type = ValueType(memberType);
                member.byteOffset = compiler.type_struct_member_offset(type, index);
                member.byteSize = static_cast<uint32_t>(compiler.get_declared_struct_member_size(type, index));
                member.arraySize = ArraySize(memberType);
                buffer.members.push_back(member);
                candidate.properties.push_back({member.name, member.type, ShaderResourceKind::Value, stage,
                    buffer.bindingSpace, buffer.bindingIndex, -1, static_cast<int32_t>(member.arraySize),
                    member.byteOffset, member.byteSize, buffer.name, mask});
            }
            candidate.constantBuffers.push_back(std::move(buffer));
        };
        for (const auto& resource : resources.uniform_buffers) addBuffer(resource, false);
        for (const auto& resource : resources.push_constant_buffers) addBuffer(resource, true);
        auto addResource = [&](const spirv_cross::Resource& resource, ShaderResourceKind kind)
        {
            ShaderPropertyDesc property;
            property.name = compiler.get_name(resource.id);
            if(property.name.empty())property.name=resource.name;
            if(property.name.starts_with("type."))property.name.erase(0,5);
            property.kind = kind;
            property.stage = stage;
            property.stageMask = mask;
            property.bindingSpace = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
            const auto binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
            const auto space = property.bindingSpace;
            uint32_t shift = space*16u;
            if (kind == ShaderResourceKind::Sampler) shift = space == 0u ? 0u : 64u+space*16u;
            if (kind == ShaderResourceKind::StorageBuffer || kind == ShaderResourceKind::StorageTexture) shift = 128u+space*16u;
            if (binding < shift) throw std::runtime_error("Invalid SPIR-V resource register shift");
            property.bindingIndex = binding-shift;
            const auto& type = compiler.get_type(resource.type_id);
            property.arraySize = static_cast<int32_t>(ArraySize(type));
            property.type = type.basetype == spirv_cross::SPIRType::Image && type.image.dim == spv::DimCube
                ? UniformType::UNIFORM_SAMPLER_CUBE : UniformType::UNIFORM_SAMPLER_2D;
            if (type.basetype == spirv_cross::SPIRType::Image)
            {
                if (type.image.dim == spv::Dim3D) property.textureDimension = RHI::TextureDimension::Texture3D;
                else if (type.image.dim == spv::DimCube) property.textureDimension = RHI::TextureDimension::TextureCube;
                else if (type.image.arrayed) property.textureDimension = RHI::TextureDimension::Texture2DArray;
            }
            if (kind == ShaderResourceKind::StructuredBuffer || kind == ShaderResourceKind::StorageBuffer)
            {
                const auto& structure = compiler.get_type(resource.base_type_id);
                if (!structure.member_types.empty())
                    property.byteSize = compiler.type_struct_member_array_stride(structure,
                        static_cast<uint32_t>(structure.member_types.size()-1));
            }
            candidate.properties.push_back(std::move(property));
        };
        for (const auto& resource : resources.separate_images) addResource(resource, ShaderResourceKind::SampledTexture);
        for (const auto& resource : resources.storage_images) addResource(resource, ShaderResourceKind::StorageTexture);
        for (const auto& resource : resources.separate_samplers) addResource(resource, ShaderResourceKind::Sampler);
        for (const auto& resource : resources.storage_buffers)
        {
            const auto readOnly = compiler.get_buffer_block_flags(resource.id).get(spv::DecorationNonWritable);
            addResource(resource, readOnly ? ShaderResourceKind::StructuredBuffer : ShaderResourceKind::StorageBuffer);
        }
        reflection = std::move(candidate);
        error.clear();
        return true;
    }
    catch (const std::exception& exception) { error = exception.what(); return false; }
}
}
