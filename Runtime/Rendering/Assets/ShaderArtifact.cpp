#include "Rendering/Assets/ShaderArtifact.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>

#include <spirv_cross_util.hpp>
#include <spirv_glsl.hpp>

#include "Assets/ArtifactManifest.h"
#include "Assets/NativeArtifactContainer.h"
#include "Rendering/ShaderCompiler/ShaderCompiler.h"

namespace NLS::Render::Assets
{
namespace
{
std::string Trim(std::string value)
{
    auto begin = value.begin();
    while (begin != value.end() && std::isspace(static_cast<unsigned char>(*begin)))
        ++begin;

    auto end = value.end();
    while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1))))
        --end;

    return std::string(begin, end);
}

std::string ToLower(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

std::string EscapeValue(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const auto character : value)
    {
        switch (character)
        {
        case '\\': escaped += "\\\\"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        default: escaped.push_back(character); break;
        }
    }
    return escaped;
}

std::string UnescapeValue(const std::string& value)
{
    std::string unescaped;
    unescaped.reserve(value.size());
    for (size_t index = 0u; index < value.size(); ++index)
    {
        if (value[index] != '\\' || index + 1u >= value.size())
        {
            unescaped.push_back(value[index]);
            continue;
        }

        const auto escaped = value[++index];
        switch (escaped)
        {
        case 'n': unescaped.push_back('\n'); break;
        case 'r': unescaped.push_back('\r'); break;
        case '\\': unescaped.push_back('\\'); break;
        default:
            unescaped.push_back('\\');
            unescaped.push_back(escaped);
            break;
        }
    }
    return unescaped;
}

const char* ToString(const ShaderCompiler::ShaderStage stage)
{
    switch (stage)
    {
    case ShaderCompiler::ShaderStage::Vertex: return "Vertex";
    case ShaderCompiler::ShaderStage::Pixel: return "Pixel";
    case ShaderCompiler::ShaderStage::Compute: return "Compute";
    default: return "Vertex";
    }
}

std::optional<ShaderCompiler::ShaderStage> ParseStage(const std::string& value)
{
    const auto lowered = ToLower(value);
    if (lowered == "vertex" || lowered == "vs")
        return ShaderCompiler::ShaderStage::Vertex;
    if (lowered == "pixel" || lowered == "fragment" || lowered == "ps")
        return ShaderCompiler::ShaderStage::Pixel;
    if (lowered == "compute" || lowered == "cs")
        return ShaderCompiler::ShaderStage::Compute;
    return std::nullopt;
}

const char* ToString(const ShaderCompiler::ShaderTargetPlatform target)
{
    switch (target)
    {
    case ShaderCompiler::ShaderTargetPlatform::DXIL: return "DXIL";
    case ShaderCompiler::ShaderTargetPlatform::SPIRV: return "SPIRV";
    case ShaderCompiler::ShaderTargetPlatform::GLSL: return "GLSL";
    case ShaderCompiler::ShaderTargetPlatform::Unknown:
    default: return "Unknown";
    }
}

std::optional<ShaderCompiler::ShaderTargetPlatform> ParseTarget(const std::string& value)
{
    const auto lowered = ToLower(value);
    if (lowered == "dxil")
        return ShaderCompiler::ShaderTargetPlatform::DXIL;
    if (lowered == "spirv" || lowered == "spir-v")
        return ShaderCompiler::ShaderTargetPlatform::SPIRV;
    if (lowered == "glsl")
        return ShaderCompiler::ShaderTargetPlatform::GLSL;
    if (lowered == "unknown")
        return ShaderCompiler::ShaderTargetPlatform::Unknown;
    return std::nullopt;
}

uint32_t ToStageMaskValue(const ShaderCompiler::ShaderStage stage)
{
    switch (stage)
    {
    case ShaderCompiler::ShaderStage::Vertex:
        return static_cast<uint32_t>(RHI::ShaderStageMask::Vertex);
    case ShaderCompiler::ShaderStage::Compute:
        return static_cast<uint32_t>(RHI::ShaderStageMask::Compute);
    case ShaderCompiler::ShaderStage::Pixel:
    default:
        return static_cast<uint32_t>(RHI::ShaderStageMask::Fragment);
    }
}

std::optional<ShaderLab::ShaderLabCullMode> ParseShaderLabCullMode(const std::string& value)
{
    const auto lowered = ToLower(value);
    if (lowered == "off")
        return ShaderLab::ShaderLabCullMode::Off;
    if (lowered == "front")
        return ShaderLab::ShaderLabCullMode::Front;
    if (lowered == "back")
        return ShaderLab::ShaderLabCullMode::Back;
    return std::nullopt;
}

const char* ToString(const Settings::EComparaisonAlgorithm compare)
{
    switch (compare)
    {
    case Settings::EComparaisonAlgorithm::NEVER: return "Never";
    case Settings::EComparaisonAlgorithm::LESS: return "Less";
    case Settings::EComparaisonAlgorithm::EQUAL: return "Equal";
    case Settings::EComparaisonAlgorithm::LESS_EQUAL: return "LessEqual";
    case Settings::EComparaisonAlgorithm::GREATER: return "Greater";
    case Settings::EComparaisonAlgorithm::NOTEQUAL: return "NotEqual";
    case Settings::EComparaisonAlgorithm::GREATER_EQUAL: return "GreaterEqual";
    case Settings::EComparaisonAlgorithm::ALWAYS: return "Always";
    default: return "Less";
    }
}

std::optional<Settings::EComparaisonAlgorithm> ParseCompareOp(const std::string& value)
{
    const auto lowered = ToLower(value);
    if (lowered == "never")
        return Settings::EComparaisonAlgorithm::NEVER;
    if (lowered == "less")
        return Settings::EComparaisonAlgorithm::LESS;
    if (lowered == "equal")
        return Settings::EComparaisonAlgorithm::EQUAL;
    if (lowered == "lessequal" || lowered == "less-equal")
        return Settings::EComparaisonAlgorithm::LESS_EQUAL;
    if (lowered == "greater")
        return Settings::EComparaisonAlgorithm::GREATER;
    if (lowered == "notequal" || lowered == "not-equal")
        return Settings::EComparaisonAlgorithm::NOTEQUAL;
    if (lowered == "greaterequal" || lowered == "greater-equal")
        return Settings::EComparaisonAlgorithm::GREATER_EQUAL;
    if (lowered == "always")
        return Settings::EComparaisonAlgorithm::ALWAYS;
    return std::nullopt;
}

const char* ToString(const RHI::RHIBlendFactor factor)
{
    switch (factor)
    {
    case RHI::RHIBlendFactor::Zero: return "Zero";
    case RHI::RHIBlendFactor::One: return "One";
    case RHI::RHIBlendFactor::SrcColor: return "SrcColor";
    case RHI::RHIBlendFactor::InvSrcColor: return "InvSrcColor";
    case RHI::RHIBlendFactor::SrcAlpha: return "SrcAlpha";
    case RHI::RHIBlendFactor::InvSrcAlpha: return "InvSrcAlpha";
    case RHI::RHIBlendFactor::DstAlpha: return "DstAlpha";
    case RHI::RHIBlendFactor::InvDstAlpha: return "InvDstAlpha";
    case RHI::RHIBlendFactor::DstColor: return "DstColor";
    case RHI::RHIBlendFactor::InvDstColor: return "InvDstColor";
    default: return "One";
    }
}

std::optional<RHI::RHIBlendFactor> ParseBlendFactor(const std::string& value)
{
    const auto lowered = ToLower(value);
    if (lowered == "zero")
        return RHI::RHIBlendFactor::Zero;
    if (lowered == "one")
        return RHI::RHIBlendFactor::One;
    if (lowered == "srccolor" || lowered == "src-color")
        return RHI::RHIBlendFactor::SrcColor;
    if (lowered == "invsrccolor" || lowered == "inv-src-color")
        return RHI::RHIBlendFactor::InvSrcColor;
    if (lowered == "srcalpha" || lowered == "src-alpha")
        return RHI::RHIBlendFactor::SrcAlpha;
    if (lowered == "invsrcalpha" || lowered == "inv-src-alpha")
        return RHI::RHIBlendFactor::InvSrcAlpha;
    if (lowered == "dstalpha" || lowered == "dst-alpha")
        return RHI::RHIBlendFactor::DstAlpha;
    if (lowered == "invdstalpha" || lowered == "inv-dst-alpha")
        return RHI::RHIBlendFactor::InvDstAlpha;
    if (lowered == "dstcolor" || lowered == "dst-color")
        return RHI::RHIBlendFactor::DstColor;
    if (lowered == "invdstcolor" || lowered == "inv-dst-color")
        return RHI::RHIBlendFactor::InvDstColor;
    return std::nullopt;
}

const char* ToString(const RHI::RHIBlendOp op)
{
    switch (op)
    {
    case RHI::RHIBlendOp::Add: return "Add";
    case RHI::RHIBlendOp::Subtract: return "Subtract";
    case RHI::RHIBlendOp::ReverseSubtract: return "ReverseSubtract";
    case RHI::RHIBlendOp::Min: return "Min";
    case RHI::RHIBlendOp::Max: return "Max";
    default: return "Add";
    }
}

std::optional<RHI::RHIBlendOp> ParseBlendOp(const std::string& value)
{
    const auto lowered = ToLower(value);
    if (lowered == "add")
        return RHI::RHIBlendOp::Add;
    if (lowered == "subtract")
        return RHI::RHIBlendOp::Subtract;
    if (lowered == "reversesubtract" || lowered == "reverse-subtract")
        return RHI::RHIBlendOp::ReverseSubtract;
    if (lowered == "min")
        return RHI::RHIBlendOp::Min;
    if (lowered == "max")
        return RHI::RHIBlendOp::Max;
    return std::nullopt;
}

const char* ToString(const RHI::RHIColorWriteMask mask)
{
    switch (mask)
    {
    case RHI::RHIColorWriteMask::None: return "None";
    case RHI::RHIColorWriteMask::Red: return "Red";
    case RHI::RHIColorWriteMask::Green: return "Green";
    case RHI::RHIColorWriteMask::Blue: return "Blue";
    case RHI::RHIColorWriteMask::Alpha: return "Alpha";
    case RHI::RHIColorWriteMask::All: return "All";
    default: return "All";
    }
}

std::optional<RHI::RHIColorWriteMask> ParseColorWriteMask(const std::string& value)
{
    const auto lowered = ToLower(value);
    if (lowered == "none")
        return RHI::RHIColorWriteMask::None;
    if (lowered == "red")
        return RHI::RHIColorWriteMask::Red;
    if (lowered == "green")
        return RHI::RHIColorWriteMask::Green;
    if (lowered == "blue")
        return RHI::RHIColorWriteMask::Blue;
    if (lowered == "alpha")
        return RHI::RHIColorWriteMask::Alpha;
    if (lowered == "all")
        return RHI::RHIColorWriteMask::All;
    return std::nullopt;
}

const char* ToString(const ShaderCompiler::ShaderCompilationStatus status)
{
    switch (status)
    {
    case ShaderCompiler::ShaderCompilationStatus::Succeeded: return "Succeeded";
    case ShaderCompiler::ShaderCompilationStatus::Failed: return "Failed";
    case ShaderCompiler::ShaderCompilationStatus::NotCompiled:
    default: return "NotCompiled";
    }
}

std::optional<ShaderCompiler::ShaderCompilationStatus> ParseStatus(const std::string& value)
{
    const auto lowered = ToLower(value);
    if (lowered == "succeeded" || lowered == "success")
        return ShaderCompiler::ShaderCompilationStatus::Succeeded;
    if (lowered == "failed" || lowered == "failure")
        return ShaderCompiler::ShaderCompilationStatus::Failed;
    if (lowered == "notcompiled" || lowered == "not-compiled")
        return ShaderCompiler::ShaderCompilationStatus::NotCompiled;
    return std::nullopt;
}

const char* ToString(const Resources::ShaderResourceKind kind)
{
    switch (kind)
    {
    case Resources::ShaderResourceKind::Value: return "Value";
    case Resources::ShaderResourceKind::SampledTexture: return "SampledTexture";
    case Resources::ShaderResourceKind::Sampler: return "Sampler";
    case Resources::ShaderResourceKind::UniformBuffer: return "UniformBuffer";
    case Resources::ShaderResourceKind::StructuredBuffer: return "StructuredBuffer";
    case Resources::ShaderResourceKind::StorageBuffer: return "StorageBuffer";
    default: return "Value";
    }
}

std::optional<Resources::ShaderResourceKind> ParseResourceKind(const std::string& value)
{
    const auto lowered = ToLower(value);
    if (lowered == "value")
        return Resources::ShaderResourceKind::Value;
    if (lowered == "sampledtexture" || lowered == "sampled-texture")
        return Resources::ShaderResourceKind::SampledTexture;
    if (lowered == "sampler")
        return Resources::ShaderResourceKind::Sampler;
    if (lowered == "uniformbuffer" || lowered == "uniform-buffer")
        return Resources::ShaderResourceKind::UniformBuffer;
    if (lowered == "structuredbuffer" || lowered == "structured-buffer")
        return Resources::ShaderResourceKind::StructuredBuffer;
    if (lowered == "storagebuffer" || lowered == "storage-buffer")
        return Resources::ShaderResourceKind::StorageBuffer;
    return std::nullopt;
}

const char* ToString(const Resources::UniformType type)
{
    switch (type)
    {
    case Resources::UniformType::UNIFORM_BOOL: return "bool";
    case Resources::UniformType::UNIFORM_INT: return "int";
    case Resources::UniformType::UNIFORM_FLOAT: return "float";
    case Resources::UniformType::UNIFORM_FLOAT_VEC2: return "vec2";
    case Resources::UniformType::UNIFORM_FLOAT_VEC3: return "vec3";
    case Resources::UniformType::UNIFORM_FLOAT_VEC4: return "vec4";
    case Resources::UniformType::UNIFORM_FLOAT_MAT4: return "mat4";
    case Resources::UniformType::UNIFORM_DOUBLE_MAT4: return "dmat4";
    case Resources::UniformType::UNIFORM_SAMPLER_2D: return "sampler2D";
    case Resources::UniformType::UNIFORM_SAMPLER_CUBE: return "samplerCube";
    default: return "float";
    }
}

std::optional<Resources::UniformType> ParseUniformType(const std::string& value)
{
    const auto lowered = ToLower(value);
    if (lowered == "bool")
        return Resources::UniformType::UNIFORM_BOOL;
    if (lowered == "int")
        return Resources::UniformType::UNIFORM_INT;
    if (lowered == "float")
        return Resources::UniformType::UNIFORM_FLOAT;
    if (lowered == "vec2" || lowered == "float2")
        return Resources::UniformType::UNIFORM_FLOAT_VEC2;
    if (lowered == "vec3" || lowered == "float3")
        return Resources::UniformType::UNIFORM_FLOAT_VEC3;
    if (lowered == "vec4" || lowered == "float4")
        return Resources::UniformType::UNIFORM_FLOAT_VEC4;
    if (lowered == "mat4" || lowered == "float4x4")
        return Resources::UniformType::UNIFORM_FLOAT_MAT4;
    if (lowered == "dmat4" || lowered == "double4x4")
        return Resources::UniformType::UNIFORM_DOUBLE_MAT4;
    if (lowered == "sampler2d" || lowered == "texture2d")
        return Resources::UniformType::UNIFORM_SAMPLER_2D;
    if (lowered == "samplercube" || lowered == "texturecube")
        return Resources::UniformType::UNIFORM_SAMPLER_CUBE;
    return std::nullopt;
}

template<typename T>
std::optional<T> ParseNumber(const std::string& value)
{
    T result {};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size())
        return std::nullopt;
    return result;
}

char ToHexDigit(const uint8_t value)
{
    return value < 10u
        ? static_cast<char>('0' + value)
        : static_cast<char>('A' + (value - 10u));
}

std::string ToHex(const std::vector<uint8_t>& bytes)
{
    std::string hex;
    hex.reserve(bytes.size() * 2u);
    for (const auto byte : bytes)
    {
        hex.push_back(ToHexDigit((byte >> 4u) & 0x0Fu));
        hex.push_back(ToHexDigit(byte & 0x0Fu));
    }
    return hex;
}

std::optional<uint8_t> FromHexDigit(const char character)
{
    if (character >= '0' && character <= '9')
        return static_cast<uint8_t>(character - '0');
    if (character >= 'a' && character <= 'f')
        return static_cast<uint8_t>(10 + character - 'a');
    if (character >= 'A' && character <= 'F')
        return static_cast<uint8_t>(10 + character - 'A');
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> FromHex(const std::string& hex)
{
    if (hex.size() % 2u != 0u)
        return std::nullopt;

    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2u);
    for (size_t index = 0u; index < hex.size(); index += 2u)
    {
        const auto high = FromHexDigit(hex[index]);
        const auto low = FromHexDigit(hex[index + 1u]);
        if (!high.has_value() || !low.has_value())
            return std::nullopt;

        bytes.push_back(static_cast<uint8_t>((*high << 4u) | *low));
    }
    return bytes;
}

std::vector<std::string> SplitDependencyPaths(const std::string& value)
{
    std::vector<std::string> paths;
    std::stringstream stream(value);
    std::string path;
    while (std::getline(stream, path, ';'))
    {
        path = Trim(UnescapeValue(path));
        if (!path.empty())
            paths.push_back(std::move(path));
    }
    return paths;
}

std::vector<uint32_t> ToSpirvWords(const std::vector<uint8_t>& bytecode)
{
    if (bytecode.empty() || bytecode.size() % sizeof(uint32_t) != 0u)
        return {};

    std::vector<uint32_t> words(bytecode.size() / sizeof(uint32_t));
    std::memcpy(words.data(), bytecode.data(), bytecode.size());
    return words;
}

uint32_t GetOpenGLTextureBindingPoint(uint32_t bindingSpace, uint32_t bindingIndex)
{
    return bindingSpace * 16u + bindingIndex;
}

uint32_t GetOpenGLUniformBufferBindingPoint(uint32_t bindingSpace, uint32_t bindingIndex)
{
    return 8u + bindingSpace * 4u + bindingIndex;
}

std::filesystem::path GetGlslArtifactPath(const std::string& spirvArtifactPath, ShaderCompiler::ShaderStage stage)
{
    auto path = std::filesystem::path(spirvArtifactPath);
    switch (stage)
    {
    case ShaderCompiler::ShaderStage::Vertex:
        path.replace_extension(".vert.glsl");
        break;
    case ShaderCompiler::ShaderStage::Compute:
        path.replace_extension(".comp.glsl");
        break;
    case ShaderCompiler::ShaderStage::Pixel:
    default:
        path.replace_extension(".frag.glsl");
        break;
    }
    return path;
}

std::string CrossCompileSpirvToGlsl(const ShaderCompiler::ShaderCompilationOutput& spirvOutput)
{
    const auto spirv = ToSpirvWords(spirvOutput.bytecode);
    if (spirv.empty())
        return {};

    try
    {
        spirv_cross::CompilerGLSL compiler(spirv);
        auto options = compiler.get_common_options();
        options.version = 430;
        options.es = false;
        options.vulkan_semantics = true;
        options.separate_shader_objects = false;
        options.enable_420pack_extension = true;
        options.vertex.flip_vert_y = false;
        options.vertex.fixup_clipspace = true;
        compiler.set_common_options(options);

        const auto resources = compiler.get_shader_resources();
        compiler.build_combined_image_samplers();
        spirv_cross_util::inherit_combined_sampler_bindings(compiler);

        for (const auto& resource : resources.uniform_buffers)
        {
            const auto descriptorSet = compiler.has_decoration(resource.id, spv::DecorationDescriptorSet)
                ? compiler.get_decoration(resource.id, spv::DecorationDescriptorSet)
                : 0u;
            const auto binding = compiler.has_decoration(resource.id, spv::DecorationBinding)
                ? compiler.get_decoration(resource.id, spv::DecorationBinding)
                : 0u;
            compiler.unset_decoration(resource.id, spv::DecorationDescriptorSet);
            compiler.set_decoration(
                resource.id,
                spv::DecorationBinding,
                GetOpenGLUniformBufferBindingPoint(descriptorSet, binding));
        }

        for (const auto& resource : resources.sampled_images)
        {
            const auto descriptorSet = compiler.has_decoration(resource.id, spv::DecorationDescriptorSet)
                ? compiler.get_decoration(resource.id, spv::DecorationDescriptorSet)
                : 0u;
            const auto binding = compiler.has_decoration(resource.id, spv::DecorationBinding)
                ? compiler.get_decoration(resource.id, spv::DecorationBinding)
                : 0u;
            compiler.unset_decoration(resource.id, spv::DecorationDescriptorSet);
            compiler.set_decoration(
                resource.id,
                spv::DecorationBinding,
                GetOpenGLTextureBindingPoint(descriptorSet, binding));
        }

        for (const auto& combined : compiler.get_combined_image_samplers())
        {
            const auto descriptorSet = compiler.has_decoration(combined.combined_id, spv::DecorationDescriptorSet)
                ? compiler.get_decoration(combined.combined_id, spv::DecorationDescriptorSet)
                : (compiler.has_decoration(combined.image_id, spv::DecorationDescriptorSet)
                    ? compiler.get_decoration(combined.image_id, spv::DecorationDescriptorSet)
                    : 0u);
            const auto binding = compiler.has_decoration(combined.combined_id, spv::DecorationBinding)
                ? compiler.get_decoration(combined.combined_id, spv::DecorationBinding)
                : (compiler.has_decoration(combined.image_id, spv::DecorationBinding)
                    ? compiler.get_decoration(combined.image_id, spv::DecorationBinding)
                    : 0u);

            compiler.unset_decoration(combined.combined_id, spv::DecorationDescriptorSet);
            compiler.set_decoration(
                combined.combined_id,
                spv::DecorationBinding,
                GetOpenGLTextureBindingPoint(descriptorSet, binding));
            compiler.set_name(combined.combined_id, compiler.get_name(combined.image_id));
        }

        return compiler.compile();
    }
    catch (const spirv_cross::CompilerError&)
    {
        return {};
    }
    catch (const std::exception&)
    {
        return {};
    }
    catch (...)
    {
        return {};
    }
}

void AppendLine(std::string& text, const char* key, const std::string& value)
{
    text += key;
    text += '=';
    text += EscapeValue(value);
    text += '\n';
}

void SerializeReflection(std::string& text, const Resources::ShaderReflection& reflection)
{
    for (const auto& buffer : reflection.constantBuffers)
    {
        text += "CBUFFER_BEGIN\n";
        AppendLine(text, "NAME", buffer.name);
        AppendLine(text, "STAGE", ToString(buffer.stage));
        AppendLine(text, "STAGE_MASK", std::to_string(static_cast<uint32_t>(buffer.stageMask)));
        AppendLine(text, "SPACE", std::to_string(buffer.bindingSpace));
        AppendLine(text, "BINDING", std::to_string(buffer.bindingIndex));
        AppendLine(text, "BYTE_SIZE", std::to_string(buffer.byteSize));
        for (const auto& member : buffer.members)
        {
            text += "MEMBER_BEGIN\n";
            AppendLine(text, "NAME", member.name);
            AppendLine(text, "TYPE", ToString(member.type));
            AppendLine(text, "BYTE_OFFSET", std::to_string(member.byteOffset));
            AppendLine(text, "BYTE_SIZE", std::to_string(member.byteSize));
            AppendLine(text, "ARRAY_SIZE", std::to_string(member.arraySize));
            text += "MEMBER_END\n";
        }
        text += "CBUFFER_END\n";
    }

    for (const auto& property : reflection.properties)
    {
        text += "PROPERTY_BEGIN\n";
        AppendLine(text, "NAME", property.name);
        AppendLine(text, "TYPE", ToString(property.type));
        AppendLine(text, "KIND", ToString(property.kind));
        AppendLine(text, "STAGE", ToString(property.stage));
        AppendLine(text, "STAGE_MASK", std::to_string(static_cast<uint32_t>(property.stageMask)));
        AppendLine(text, "SPACE", std::to_string(property.bindingSpace));
        AppendLine(text, "BINDING", std::to_string(property.bindingIndex));
        AppendLine(text, "LOCATION", std::to_string(property.location));
        AppendLine(text, "ARRAY_SIZE", std::to_string(property.arraySize));
        AppendLine(text, "BYTE_OFFSET", std::to_string(property.byteOffset));
        AppendLine(text, "BYTE_SIZE", std::to_string(property.byteSize));
        AppendLine(text, "PARENT_CBUFFER", property.parentConstantBuffer);
        text += "PROPERTY_END\n";
    }
}

void SerializeShaderLabPassState(std::string& text, const ShaderLab::ShaderLabPassState& state)
{
    text += "SHADERLAB_PASS_STATE_BEGIN\n";
    AppendLine(text, "CULL", ShaderLab::ToString(state.cullMode));
    AppendLine(text, "DEPTH_WRITE", state.depthWrite ? "true" : "false");
    AppendLine(text, "DEPTH_COMPARE", ToString(state.depthCompare));
    AppendLine(text, "BLEND_ENABLED", state.blend.enabled ? "true" : "false");
    AppendLine(text, "BLEND_COLOR_WRITE", state.blend.colorWrite ? "true" : "false");
    AppendLine(text, "ALPHA_TO_COVERAGE", state.blend.alphaToCoverageEnable ? "true" : "false");
    AppendLine(text, "INDEPENDENT_BLEND", state.blend.independentBlendEnable ? "true" : "false");
    for (const auto& target : state.blend.renderTargets)
    {
        text += "BLEND_TARGET_BEGIN\n";
        AppendLine(text, "BLEND_ENABLE", target.blendEnable ? "true" : "false");
        AppendLine(text, "SRC_COLOR", ToString(target.srcColor));
        AppendLine(text, "DST_COLOR", ToString(target.dstColor));
        AppendLine(text, "COLOR_OP", ToString(target.colorOp));
        AppendLine(text, "SRC_ALPHA", ToString(target.srcAlpha));
        AppendLine(text, "DST_ALPHA", ToString(target.dstAlpha));
        AppendLine(text, "ALPHA_OP", ToString(target.alphaOp));
        AppendLine(text, "COLOR_WRITE_MASK", ToString(target.colorWriteMask));
        text += "BLEND_TARGET_END\n";
    }
    text += "SHADERLAB_PASS_STATE_END\n";
}

bool ApplyStageField(ShaderArtifactStage& stage, const std::string& key, const std::string& value)
{
    if (key == "STAGE")
    {
        const auto parsed = ParseStage(value);
        if (!parsed.has_value())
            return false;
        stage.stage = *parsed;
    }
    else if (key == "TARGET")
    {
        const auto parsed = ParseTarget(value);
        if (!parsed.has_value())
            return false;
        stage.targetPlatform = *parsed;
    }
    else if (key == "ENTRY")
    {
        stage.entryPoint = value;
    }
    else if (key == "PROFILE")
    {
        stage.targetProfile = value;
    }
    else if (key == "KEYWORD_HASH")
    {
        const auto parsed = ParseNumber<uint64_t>(value);
        if (!parsed.has_value())
            return false;
        stage.keywordHash = *parsed;
    }
    else if (key == "STATUS")
    {
        const auto parsed = ParseStatus(value);
        if (!parsed.has_value())
            return false;
        stage.output.status = *parsed;
    }
    else if (key == "CACHE_KEY")
    {
        stage.output.cacheKey = value;
    }
    else if (key == "ARTIFACT_PATH")
    {
        stage.output.artifactPath = value;
    }
    else if (key == "DIAGNOSTICS")
    {
        stage.output.diagnostics = value;
    }
    else if (key == "DEPENDENCY")
    {
        if (!value.empty())
            stage.output.dependencyPaths.push_back(value);
    }
    else if (key == "DEPENDENCIES")
    {
        stage.output.dependencyPaths = SplitDependencyPaths(value);
    }
    else if (key == "BYTECODE_HEX")
    {
        const auto parsed = FromHex(value);
        if (!parsed.has_value())
            return false;
        stage.output.bytecode = *parsed;
    }
    return true;
}

bool ApplyConstantBufferField(Resources::ShaderConstantBufferDesc& buffer, const std::string& key, const std::string& value)
{
    if (key == "NAME")
        buffer.name = value;
    else if (key == "STAGE")
    {
        const auto parsed = ParseStage(value);
        if (!parsed.has_value())
            return false;
        buffer.stage = *parsed;
        if (buffer.stageMask == RHI::ShaderStageMask::Vertex)
            buffer.stageMask = static_cast<RHI::ShaderStageMask>(ToStageMaskValue(buffer.stage));
    }
    else if (key == "STAGE_MASK")
    {
        const auto parsed = ParseNumber<uint32_t>(value);
        if (!parsed.has_value())
            return false;
        buffer.stageMask = static_cast<RHI::ShaderStageMask>(*parsed);
    }
    else if (key == "SPACE")
    {
        const auto parsed = ParseNumber<uint32_t>(value);
        if (!parsed.has_value())
            return false;
        buffer.bindingSpace = *parsed;
    }
    else if (key == "BINDING")
    {
        const auto parsed = ParseNumber<uint32_t>(value);
        if (!parsed.has_value())
            return false;
        buffer.bindingIndex = *parsed;
    }
    else if (key == "BYTE_SIZE")
    {
        const auto parsed = ParseNumber<uint32_t>(value);
        if (!parsed.has_value())
            return false;
        buffer.byteSize = *parsed;
    }
    return true;
}

bool ApplyConstantBufferMemberField(Resources::ShaderCBufferMemberDesc& member, const std::string& key, const std::string& value)
{
    if (key == "NAME")
        member.name = value;
    else if (key == "TYPE")
    {
        const auto parsed = ParseUniformType(value);
        if (!parsed.has_value())
            return false;
        member.type = *parsed;
    }
    else if (key == "BYTE_OFFSET")
    {
        const auto parsed = ParseNumber<uint32_t>(value);
        if (!parsed.has_value())
            return false;
        member.byteOffset = *parsed;
    }
    else if (key == "BYTE_SIZE")
    {
        const auto parsed = ParseNumber<uint32_t>(value);
        if (!parsed.has_value())
            return false;
        member.byteSize = *parsed;
    }
    else if (key == "ARRAY_SIZE")
    {
        const auto parsed = ParseNumber<uint32_t>(value);
        if (!parsed.has_value())
            return false;
        member.arraySize = *parsed;
    }
    return true;
}

bool ApplyPropertyField(Resources::ShaderPropertyDesc& property, const std::string& key, const std::string& value)
{
    if (key == "NAME")
        property.name = value;
    else if (key == "TYPE")
    {
        const auto parsed = ParseUniformType(value);
        if (!parsed.has_value())
            return false;
        property.type = *parsed;
    }
    else if (key == "KIND")
    {
        const auto parsed = ParseResourceKind(value);
        if (!parsed.has_value())
            return false;
        property.kind = *parsed;
    }
    else if (key == "STAGE")
    {
        const auto parsed = ParseStage(value);
        if (!parsed.has_value())
            return false;
        property.stage = *parsed;
        if (property.stageMask == RHI::ShaderStageMask::Vertex)
            property.stageMask = static_cast<RHI::ShaderStageMask>(ToStageMaskValue(property.stage));
    }
    else if (key == "STAGE_MASK")
    {
        const auto parsed = ParseNumber<uint32_t>(value);
        if (!parsed.has_value())
            return false;
        property.stageMask = static_cast<RHI::ShaderStageMask>(*parsed);
    }
    else if (key == "SPACE")
    {
        const auto parsed = ParseNumber<uint32_t>(value);
        if (!parsed.has_value())
            return false;
        property.bindingSpace = *parsed;
    }
    else if (key == "BINDING")
    {
        const auto parsed = ParseNumber<uint32_t>(value);
        if (!parsed.has_value())
            return false;
        property.bindingIndex = *parsed;
    }
    else if (key == "LOCATION")
    {
        const auto parsed = ParseNumber<int32_t>(value);
        if (!parsed.has_value())
            return false;
        property.location = *parsed;
    }
    else if (key == "ARRAY_SIZE")
    {
        const auto parsed = ParseNumber<int32_t>(value);
        if (!parsed.has_value())
            return false;
        property.arraySize = *parsed;
    }
    else if (key == "BYTE_OFFSET")
    {
        const auto parsed = ParseNumber<uint32_t>(value);
        if (!parsed.has_value())
            return false;
        property.byteOffset = *parsed;
    }
    else if (key == "BYTE_SIZE")
    {
        const auto parsed = ParseNumber<uint32_t>(value);
        if (!parsed.has_value())
            return false;
        property.byteSize = *parsed;
    }
    else if (key == "PARENT_CBUFFER")
        property.parentConstantBuffer = value;
    return true;
}

bool ApplyShaderLabPassStateField(ShaderLab::ShaderLabPassState& state, const std::string& key, const std::string& value)
{
    if (key == "CULL")
    {
        const auto parsed = ParseShaderLabCullMode(value);
        if (!parsed.has_value())
            return false;
        state.cullMode = *parsed;
    }
    else if (key == "DEPTH_WRITE")
    {
        state.depthWrite = value == "true" || value == "1";
    }
    else if (key == "DEPTH_COMPARE")
    {
        const auto parsed = ParseCompareOp(value);
        if (!parsed.has_value())
            return false;
        state.depthCompare = *parsed;
    }
    else if (key == "BLEND_ENABLED")
    {
        state.blend.enabled = value == "true" || value == "1";
    }
    else if (key == "BLEND_COLOR_WRITE")
    {
        state.blend.colorWrite = value == "true" || value == "1";
    }
    else if (key == "ALPHA_TO_COVERAGE")
    {
        state.blend.alphaToCoverageEnable = value == "true" || value == "1";
    }
    else if (key == "INDEPENDENT_BLEND")
    {
        state.blend.independentBlendEnable = value == "true" || value == "1";
    }
    return true;
}

bool ApplyBlendTargetField(RHI::RHIRenderTargetBlendStateDesc& target, const std::string& key, const std::string& value)
{
    if (key == "BLEND_ENABLE")
    {
        target.blendEnable = value == "true" || value == "1";
    }
    else if (key == "SRC_COLOR")
    {
        const auto parsed = ParseBlendFactor(value);
        if (!parsed.has_value())
            return false;
        target.srcColor = *parsed;
    }
    else if (key == "DST_COLOR")
    {
        const auto parsed = ParseBlendFactor(value);
        if (!parsed.has_value())
            return false;
        target.dstColor = *parsed;
    }
    else if (key == "COLOR_OP")
    {
        const auto parsed = ParseBlendOp(value);
        if (!parsed.has_value())
            return false;
        target.colorOp = *parsed;
    }
    else if (key == "SRC_ALPHA")
    {
        const auto parsed = ParseBlendFactor(value);
        if (!parsed.has_value())
            return false;
        target.srcAlpha = *parsed;
    }
    else if (key == "DST_ALPHA")
    {
        const auto parsed = ParseBlendFactor(value);
        if (!parsed.has_value())
            return false;
        target.dstAlpha = *parsed;
    }
    else if (key == "ALPHA_OP")
    {
        const auto parsed = ParseBlendOp(value);
        if (!parsed.has_value())
            return false;
        target.alphaOp = *parsed;
    }
    else if (key == "COLOR_WRITE_MASK")
    {
        const auto parsed = ParseColorWriteMask(value);
        if (!parsed.has_value())
            return false;
        target.colorWriteMask = *parsed;
    }
    return true;
}
}

std::vector<uint8_t> SerializeShaderArtifact(const ShaderArtifact& artifact)
{
    std::string text;
    text += "NULLUS_IMPORTED_SHADER_ARTIFACT=1\n";
    AppendLine(text, "SOURCE", artifact.sourcePath);
    AppendLine(text, "SUB_ASSET", artifact.subAssetKey);
    AppendLine(text, "TARGET_PLATFORM", artifact.targetPlatform);
    if (!artifact.shaderLabLightMode.empty())
        AppendLine(text, "SHADERLAB_LIGHT_MODE", artifact.shaderLabLightMode);
    if (artifact.shaderLabPassState.has_value())
        SerializeShaderLabPassState(text, *artifact.shaderLabPassState);
    SerializeReflection(text, artifact.reflection);

    for (const auto& stage : artifact.stages)
    {
        text += "STAGE_BEGIN\n";
        AppendLine(text, "STAGE", ToString(stage.stage));
        AppendLine(text, "TARGET", ToString(stage.targetPlatform));
        AppendLine(text, "ENTRY", stage.entryPoint);
        AppendLine(text, "PROFILE", stage.targetProfile);
        AppendLine(text, "KEYWORD_HASH", std::to_string(stage.keywordHash));
        AppendLine(text, "STATUS", ToString(stage.output.status));
        AppendLine(text, "CACHE_KEY", stage.output.cacheKey);
        AppendLine(text, "ARTIFACT_PATH", stage.output.artifactPath);
        if (!stage.output.diagnostics.empty())
            AppendLine(text, "DIAGNOSTICS", stage.output.diagnostics);
        for (const auto& dependencyPath : stage.output.dependencyPaths)
        {
            if (!dependencyPath.empty())
                AppendLine(text, "DEPENDENCY", dependencyPath);
        }
        AppendLine(text, "BYTECODE_HEX", ToHex(stage.output.bytecode));
        text += "STAGE_END\n";
    }

    std::vector<uint8_t> payload(text.begin(), text.end());
    NLS::Core::Assets::NativeArtifactMetadata metadata;
    metadata.artifactType = NLS::Core::Assets::ArtifactType::Shader;
    metadata.schemaName = "shader";
    metadata.schemaVersion = 1u;
    metadata.subAssetKey = artifact.subAssetKey;
    metadata.targetPlatform = artifact.targetPlatform;
    return NLS::Core::Assets::WriteNativeArtifactContainer(std::move(metadata), payload);
}

std::optional<ShaderArtifact> DeserializeShaderArtifact(std::string_view text)
{
    ShaderArtifact artifact;
    bool sawHeader = false;
    bool inStage = false;
    bool inConstantBuffer = false;
    bool inConstantBufferMember = false;
    bool inProperty = false;
    bool inShaderLabPassState = false;
    bool inBlendTarget = false;
    ShaderArtifactStage currentStage;
    Resources::ShaderConstantBufferDesc currentConstantBuffer;
    Resources::ShaderCBufferMemberDesc currentConstantBufferMember;
    Resources::ShaderPropertyDesc currentProperty;
    ShaderLab::ShaderLabPassState currentShaderLabPassState;
    RHI::RHIRenderTargetBlendStateDesc currentBlendTarget;

    std::stringstream stream {std::string(text)};
    std::string line;
    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        line = Trim(std::move(line));
        if (line.empty())
            continue;

        if (line == "STAGE_BEGIN")
        {
            if (inStage || inConstantBuffer || inConstantBufferMember || inProperty || inShaderLabPassState || inBlendTarget)
                return std::nullopt;
            inStage = true;
            currentStage = {};
            continue;
        }

        if (line == "STAGE_END")
        {
            if (!inStage)
                return std::nullopt;
            artifact.stages.push_back(std::move(currentStage));
            currentStage = {};
            inStage = false;
            continue;
        }

        if (line == "CBUFFER_BEGIN")
        {
            if (inStage || inConstantBuffer || inConstantBufferMember || inProperty || inShaderLabPassState || inBlendTarget)
                return std::nullopt;
            inConstantBuffer = true;
            currentConstantBuffer = {};
            continue;
        }

        if (line == "CBUFFER_END")
        {
            if (!inConstantBuffer || inConstantBufferMember)
                return std::nullopt;
            artifact.reflection.constantBuffers.push_back(std::move(currentConstantBuffer));
            currentConstantBuffer = {};
            inConstantBuffer = false;
            continue;
        }

        if (line == "MEMBER_BEGIN")
        {
            if (!inConstantBuffer || inConstantBufferMember)
                return std::nullopt;
            inConstantBufferMember = true;
            currentConstantBufferMember = {};
            continue;
        }

        if (line == "MEMBER_END")
        {
            if (!inConstantBufferMember)
                return std::nullopt;
            currentConstantBuffer.members.push_back(std::move(currentConstantBufferMember));
            currentConstantBufferMember = {};
            inConstantBufferMember = false;
            continue;
        }

        if (line == "PROPERTY_BEGIN")
        {
            if (inStage || inConstantBuffer || inConstantBufferMember || inProperty || inShaderLabPassState || inBlendTarget)
                return std::nullopt;
            inProperty = true;
            currentProperty = {};
            continue;
        }

        if (line == "PROPERTY_END")
        {
            if (!inProperty)
                return std::nullopt;
            artifact.reflection.properties.push_back(std::move(currentProperty));
            currentProperty = {};
            inProperty = false;
            continue;
        }

        if (line == "SHADERLAB_PASS_STATE_BEGIN")
        {
            if (inStage || inConstantBuffer || inConstantBufferMember || inProperty || inShaderLabPassState || inBlendTarget)
                return std::nullopt;
            inShaderLabPassState = true;
            currentShaderLabPassState = {};
            continue;
        }

        if (line == "SHADERLAB_PASS_STATE_END")
        {
            if (!inShaderLabPassState || inBlendTarget)
                return std::nullopt;
            artifact.shaderLabPassState = std::move(currentShaderLabPassState);
            currentShaderLabPassState = {};
            inShaderLabPassState = false;
            continue;
        }

        if (line == "BLEND_TARGET_BEGIN")
        {
            if (!inShaderLabPassState || inBlendTarget)
                return std::nullopt;
            inBlendTarget = true;
            currentBlendTarget = {};
            continue;
        }

        if (line == "BLEND_TARGET_END")
        {
            if (!inBlendTarget)
                return std::nullopt;
            currentShaderLabPassState.blend.renderTargets.push_back(currentBlendTarget);
            currentBlendTarget = {};
            inBlendTarget = false;
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string::npos)
            return std::nullopt;

        const auto key = line.substr(0u, equals);
        const auto value = UnescapeValue(line.substr(equals + 1u));
        if (key == "NULLUS_IMPORTED_SHADER_ARTIFACT")
        {
            sawHeader = value == "1";
            continue;
        }

        if (inStage)
        {
            if (!ApplyStageField(currentStage, key, value))
                return std::nullopt;
            continue;
        }

        if (inConstantBufferMember)
        {
            if (!ApplyConstantBufferMemberField(currentConstantBufferMember, key, value))
                return std::nullopt;
            continue;
        }

        if (inConstantBuffer)
        {
            if (!ApplyConstantBufferField(currentConstantBuffer, key, value))
                return std::nullopt;
            continue;
        }

        if (inProperty)
        {
            if (!ApplyPropertyField(currentProperty, key, value))
                return std::nullopt;
            continue;
        }

        if (inBlendTarget)
        {
            if (!ApplyBlendTargetField(currentBlendTarget, key, value))
                return std::nullopt;
            continue;
        }

        if (inShaderLabPassState)
        {
            if (!ApplyShaderLabPassStateField(currentShaderLabPassState, key, value))
                return std::nullopt;
            continue;
        }

        if (key == "SOURCE")
            artifact.sourcePath = value;
        else if (key == "SUB_ASSET")
            artifact.subAssetKey = value;
        else if (key == "TARGET_PLATFORM")
            artifact.targetPlatform = value;
        else if (key == "SHADERLAB_LIGHT_MODE")
            artifact.shaderLabLightMode = value;
    }

    if (!sawHeader ||
        inStage ||
        inConstantBuffer ||
        inConstantBufferMember ||
        inProperty ||
        inShaderLabPassState ||
        inBlendTarget ||
        artifact.sourcePath.empty() ||
        artifact.subAssetKey.empty())
    {
        return std::nullopt;
    }
    return artifact;
}

std::optional<ShaderArtifact> DeserializeShaderArtifact(const std::vector<uint8_t>& bytes)
{
    const auto container = NLS::Core::Assets::ReadNativeArtifactContainer(
        bytes,
        NLS::Core::Assets::ArtifactType::Shader,
        1u);
    if (!container.has_value())
        return std::nullopt;

    return DeserializeShaderArtifact(std::string_view(
        reinterpret_cast<const char*>(container->payload.data()),
        container->payload.size()));
}

std::optional<ShaderArtifact> LoadShaderArtifact(const std::filesystem::path& path)
{
    const auto portableArtifactPath =
        NLS::Core::Assets::TryMakePortableContentArtifactPath(path.generic_string());
    if (!portableArtifactPath.empty() &&
        !NLS::Core::Assets::IsRuntimeArtifactPathAuthorized(portableArtifactPath))
    {
        return std::nullopt;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;

    const std::vector<uint8_t> bytes{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    return DeserializeShaderArtifact(bytes);
}

void AppendGlslShaderArtifactStages(ShaderArtifact& artifact)
{
    std::vector<ShaderArtifactStage> glslStages;
    for (const auto& stage : artifact.stages)
    {
        if (stage.targetPlatform != ShaderCompiler::ShaderTargetPlatform::SPIRV ||
            stage.output.status != ShaderCompiler::ShaderCompilationStatus::Succeeded ||
            stage.output.artifactPath.empty())
        {
            continue;
        }

        auto glslPath = GetGlslArtifactPath(stage.output.artifactPath, stage.stage);

        std::string glslText;
        if (std::ifstream input(glslPath, std::ios::binary); input)
        {
            glslText = {
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()};
        }
        if (glslText.empty())
        {
            glslText = CrossCompileSpirvToGlsl(stage.output);
            if (!glslText.empty())
            {
                std::string diagnostics;
                ShaderCompiler::WriteShaderArtifactTextAtomic(glslPath.string(), glslText, &diagnostics);
            }
        }
        if (glslText.empty())
            continue;

        auto glslStage = stage;
        glslStage.targetPlatform = ShaderCompiler::ShaderTargetPlatform::GLSL;
        glslStage.entryPoint = "main";
        glslStage.targetProfile = "glsl_430";
        glslStage.output.status = ShaderCompiler::ShaderCompilationStatus::Succeeded;
        glslStage.output.bytecode.assign(glslText.begin(), glslText.end());
        glslStage.output.diagnostics = "Generated from SPIR-V.";
        glslStage.output.artifactPath = glslPath.string();
        glslStages.push_back(std::move(glslStage));
    }

    for (auto& glslStage : glslStages)
    {
        const auto exists = std::any_of(
            artifact.stages.begin(),
            artifact.stages.end(),
            [&glslStage](const ShaderArtifactStage& existing)
            {
                return existing.stage == glslStage.stage &&
                    existing.targetPlatform == glslStage.targetPlatform &&
                    existing.keywordHash == glslStage.keywordHash;
            });
        if (!exists)
            artifact.stages.push_back(std::move(glslStage));
    }
}

bool HasUsableShaderArtifactStage(const ShaderArtifact& artifact)
{
    return std::any_of(
        artifact.stages.begin(),
        artifact.stages.end(),
        [](const ShaderArtifactStage& stage)
        {
            return stage.output.status == ShaderCompiler::ShaderCompilationStatus::Succeeded &&
                stage.targetPlatform != ShaderCompiler::ShaderTargetPlatform::Unknown &&
                !stage.output.bytecode.empty();
        });
}

bool HasUsableShaderArtifactStage(
    const ShaderArtifact& artifact,
    ShaderCompiler::ShaderTargetPlatform targetPlatform)
{
    return std::any_of(
        artifact.stages.begin(),
        artifact.stages.end(),
        [targetPlatform](const ShaderArtifactStage& stage)
        {
            return stage.targetPlatform == targetPlatform &&
                stage.output.status == ShaderCompiler::ShaderCompilationStatus::Succeeded &&
                !stage.output.bytecode.empty();
        });
}
}
