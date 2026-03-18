#include "Rendering/Resources/Loaders/MaterialLoader.h"

#include <fstream>
#include <array>
#include <sstream>
#include <string_view>

#include <Debug/Logger.h>

#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Core/ServiceLocator.h"
#include "Math/Vector2.h"
#include "Math/Vector3.h"
#include "Math/Vector4.h"
#include "Math/Matrix4.h"
#include "Rendering/Resources/TextureCube.h"

namespace
{
    std::string ReadAllText(const std::string& path)
    {
        std::ifstream stream(path);
        if (!stream)
            return {};

        std::ostringstream buffer;
        buffer << stream.rdbuf();
        return buffer.str();
    }

    std::string Trim(std::string value)
    {
        const auto begin = value.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos)
            return {};

        const auto end = value.find_last_not_of(" \t\r\n");
        return value.substr(begin, end - begin + 1);
    }

    std::string EscapeXml(const std::string& value)
    {
        std::string result;
        result.reserve(value.size());

        for (const char c : value)
        {
            switch (c)
            {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            default: result += c; break;
            }
        }

        return result;
    }

    std::string UnescapeXml(std::string value)
    {
        const struct Replacement
        {
            std::string_view from;
            std::string_view to;
        } replacements[] =
        {
            { "&quot;", "\"" },
            { "&gt;", ">" },
            { "&lt;", "<" },
            { "&amp;", "&" }
        };

        for (const auto& replacement : replacements)
        {
            size_t position = 0;
            while ((position = value.find(replacement.from.data(), position)) != std::string::npos)
            {
                value.replace(position, replacement.from.size(), replacement.to.data());
                position += replacement.to.size();
            }
        }

        return value;
    }

    std::string GetTagValue(const std::string& xml, const std::string& tag)
    {
        const std::string openTag = "<" + tag + ">";
        const std::string closeTag = "</" + tag + ">";

        const auto open = xml.find(openTag);
        if (open == std::string::npos)
            return {};

        const auto valueStart = open + openTag.size();
        const auto close = xml.find(closeTag, valueStart);
        if (close == std::string::npos)
            return {};

        return UnescapeXml(Trim(xml.substr(valueStart, close - valueStart)));
    }

    std::vector<std::string> GetBlocks(const std::string& xml, const std::string& blockName)
    {
        std::vector<std::string> blocks;
        const std::string openTag = "<" + blockName;
        const std::string closeTag = "/>";

        size_t searchFrom = 0;
        while (true)
        {
            const auto open = xml.find(openTag, searchFrom);
            if (open == std::string::npos)
                break;

            const auto close = xml.find(closeTag, open);
            if (close == std::string::npos)
                break;

            blocks.emplace_back(xml.substr(open, close - open + closeTag.size()));
            searchFrom = close + closeTag.size();
        }

        return blocks;
    }

    std::string GetAttributeValue(const std::string& block, const std::string& attribute)
    {
        const std::string marker = attribute + "=\"";
        const auto begin = block.find(marker);
        if (begin == std::string::npos)
            return {};

        const auto valueStart = begin + marker.size();
        const auto valueEnd = block.find('"', valueStart);
        if (valueEnd == std::string::npos)
            return {};

        return UnescapeXml(block.substr(valueStart, valueEnd - valueStart));
    }

    bool ParseBool(const std::string& value, bool fallback = false)
    {
        if (value == "true" || value == "1")
            return true;
        if (value == "false" || value == "0")
            return false;
        return fallback;
    }

    std::string UniformTypeToString(NLS::Render::Resources::UniformType type)
    {
        using UniformType = NLS::Render::Resources::UniformType;
        switch (type)
        {
        case UniformType::UNIFORM_BOOL: return "bool";
        case UniformType::UNIFORM_INT: return "int";
        case UniformType::UNIFORM_FLOAT: return "float";
        case UniformType::UNIFORM_FLOAT_VEC2: return "vec2";
        case UniformType::UNIFORM_FLOAT_VEC3: return "vec3";
        case UniformType::UNIFORM_FLOAT_VEC4: return "vec4";
        case UniformType::UNIFORM_FLOAT_MAT4: return "mat4";
        case UniformType::UNIFORM_SAMPLER_2D: return "sampler2D";
        case UniformType::UNIFORM_SAMPLER_CUBE: return "samplerCube";
        default: return {};
        }
    }

    std::string SerializeUniformValue(NLS::Render::Resources::UniformType type, const std::any& value)
    {
        using namespace NLS;
        using namespace NLS::Maths;
        using namespace NLS::Render::Resources;

        std::ostringstream stream;
        stream.setf(std::ios::fixed);
        stream.precision(6);

        switch (type)
        {
        case UniformType::UNIFORM_BOOL:
            return std::any_cast<bool>(value) ? "true" : "false";
        case UniformType::UNIFORM_INT:
            stream << std::any_cast<int>(value);
            return stream.str();
        case UniformType::UNIFORM_FLOAT:
            stream << std::any_cast<float>(value);
            return stream.str();
        case UniformType::UNIFORM_FLOAT_VEC2:
        {
            const auto vec = std::any_cast<Vector2>(value);
            stream << vec.x << ' ' << vec.y;
            return stream.str();
        }
        case UniformType::UNIFORM_FLOAT_VEC3:
        {
            const auto vec = std::any_cast<Vector3>(value);
            stream << vec.x << ' ' << vec.y << ' ' << vec.z;
            return stream.str();
        }
        case UniformType::UNIFORM_FLOAT_VEC4:
        {
            const auto vec = std::any_cast<Vector4>(value);
            stream << vec.x << ' ' << vec.y << ' ' << vec.z << ' ' << vec.w;
            return stream.str();
        }
        case UniformType::UNIFORM_FLOAT_MAT4:
        {
            const auto mat = std::any_cast<Matrix4>(value);
            for (size_t index = 0; index < 16; ++index)
            {
                if (index != 0)
                    stream << ' ';
                stream << mat.data[index];
            }
            return stream.str();
        }
        case UniformType::UNIFORM_SAMPLER_2D:
        {
            const auto texture = std::any_cast<Texture2D*>(value);
            return texture ? texture->path : "";
        }
        case UniformType::UNIFORM_SAMPLER_CUBE:
            return "";
        default:
            return {};
        }
    }

    template <size_t N>
    bool ParseFloatArray(const std::string& value, std::array<float, N>& output)
    {
        std::istringstream stream(value);
        for (size_t index = 0; index < N; ++index)
        {
            if (!(stream >> output[index]))
                return false;
        }

        return true;
    }

    void ApplyUniformValue(NLS::Render::Resources::Material& material, const NLS::Render::Resources::UniformInfo& uniform, const std::string& value)
    {
        using namespace NLS;
        using namespace NLS::Maths;
        using namespace NLS::Render::Resources;

        switch (uniform.type)
        {
        case UniformType::UNIFORM_BOOL:
            material.Set<bool>(uniform.name, ParseBool(value));
            break;
        case UniformType::UNIFORM_INT:
            material.Set<int>(uniform.name, std::stoi(value));
            break;
        case UniformType::UNIFORM_FLOAT:
            material.Set<float>(uniform.name, std::stof(value));
            break;
        case UniformType::UNIFORM_FLOAT_VEC2:
        {
            std::array<float, 2> parsed{};
            if (ParseFloatArray(value, parsed))
                material.Set<Vector2>(uniform.name, { parsed[0], parsed[1] });
            break;
        }
        case UniformType::UNIFORM_FLOAT_VEC3:
        {
            std::array<float, 3> parsed{};
            if (ParseFloatArray(value, parsed))
                material.Set<Vector3>(uniform.name, { parsed[0], parsed[1], parsed[2] });
            break;
        }
        case UniformType::UNIFORM_FLOAT_VEC4:
        {
            std::array<float, 4> parsed{};
            if (ParseFloatArray(value, parsed))
                material.Set<Vector4>(uniform.name, { parsed[0], parsed[1], parsed[2], parsed[3] });
            break;
        }
        case UniformType::UNIFORM_FLOAT_MAT4:
        {
            std::array<float, 16> parsed{};
            if (ParseFloatArray(value, parsed))
            {
                Matrix4 matrix;
                for (size_t index = 0; index < 16; ++index)
                    matrix.data[index] = parsed[index];
                material.Set<Matrix4>(uniform.name, matrix);
            }
            break;
        }
        case UniformType::UNIFORM_SAMPLER_2D:
        {
            auto* texture = value.empty() ? nullptr : NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager)[value];
            material.Set<Texture2D*>(uniform.name, texture);
            break;
        }
        case UniformType::UNIFORM_SAMPLER_CUBE:
            break;
        default:
            break;
        }
    }

    void ApplySerializedMaterial(NLS::Render::Resources::Material& material, const std::string& xml)
    {
        using namespace NLS;
        using namespace NLS::Render::Resources;

        const auto shaderPath = GetTagValue(xml, "shader");
        auto* shader = shaderPath.empty() || shaderPath == "?" ? nullptr : NLS_SERVICE(NLS::Core::ResourceManagement::ShaderManager)[shaderPath];
        material.SetShader(shader);

        if (!shader)
            return;

        const auto blendable = GetTagValue(xml, "blendable");
        if (!blendable.empty())
            material.SetBlendable(ParseBool(blendable, material.IsBlendable()));

        const auto backfaceCulling = GetTagValue(xml, "backfaceCulling");
        if (!backfaceCulling.empty())
            material.SetBackfaceCulling(ParseBool(backfaceCulling, material.HasBackfaceCulling()));

        const auto frontfaceCulling = GetTagValue(xml, "frontfaceCulling");
        if (!frontfaceCulling.empty())
            material.SetFrontfaceCulling(ParseBool(frontfaceCulling, material.HasFrontfaceCulling()));

        const auto depthTest = GetTagValue(xml, "depthTest");
        if (!depthTest.empty())
            material.SetDepthTest(ParseBool(depthTest, material.HasDepthTest()));

        const auto depthWriting = GetTagValue(xml, "depthWriting");
        if (!depthWriting.empty())
            material.SetDepthWriting(ParseBool(depthWriting, material.HasDepthWriting()));

        const auto colorWriting = GetTagValue(xml, "colorWriting");
        if (!colorWriting.empty())
            material.SetColorWriting(ParseBool(colorWriting, material.HasColorWriting()));

        const auto gpuInstances = GetTagValue(xml, "gpuInstances");
        if (!gpuInstances.empty())
            material.SetGPUInstances(std::stoi(gpuInstances));

        for (const auto& block : GetBlocks(xml, "uniform"))
        {
            const auto uniformName = GetAttributeValue(block, "name");
            const auto uniformValue = GetAttributeValue(block, "value");
            if (uniformName.empty())
                continue;

            if (const auto* uniformInfo = shader->GetUniformInfo(uniformName))
                ApplyUniformValue(material, *uniformInfo, uniformValue);
        }
    }
}

NLS::Render::Resources::Material* NLS::Render::Resources::Loaders::MaterialLoader::Create(const std::string& p_path)
{
    const auto xml = ReadAllText(p_path);
    if (xml.empty())
    {
        NLS_LOG_ERROR("Failed to load material: " + p_path);
        return nullptr;
    }

    auto* material = new Material();
    ApplySerializedMaterial(*material, xml);
    const_cast<std::string&>(material->path) = p_path;
    return material;
}

void NLS::Render::Resources::Loaders::MaterialLoader::Reload(Material& p_material, const std::string& p_path)
{
    const auto xml = ReadAllText(p_path);
    if (xml.empty())
    {
        NLS_LOG_ERROR("Failed to reload material: " + p_path);
        return;
    }

    ApplySerializedMaterial(p_material, xml);
}

void NLS::Render::Resources::Loaders::MaterialLoader::Save(Material& p_material, const std::string& p_path)
{
    std::ofstream output(p_path, std::ios::trunc);
    if (!output)
    {
        NLS_LOG_ERROR("Failed to save material: " + p_path);
        return;
    }

    output << "<root>\n";
    output << "  <shader>" << EscapeXml(p_material.GetShader() ? p_material.GetShader()->path : "?") << "</shader>\n";
    output << "  <blendable>" << (p_material.IsBlendable() ? "true" : "false") << "</blendable>\n";
    output << "  <backfaceCulling>" << (p_material.HasBackfaceCulling() ? "true" : "false") << "</backfaceCulling>\n";
    output << "  <frontfaceCulling>" << (p_material.HasFrontfaceCulling() ? "true" : "false") << "</frontfaceCulling>\n";
    output << "  <depthTest>" << (p_material.HasDepthTest() ? "true" : "false") << "</depthTest>\n";
    output << "  <depthWriting>" << (p_material.HasDepthWriting() ? "true" : "false") << "</depthWriting>\n";
    output << "  <colorWriting>" << (p_material.HasColorWriting() ? "true" : "false") << "</colorWriting>\n";
    output << "  <gpuInstances>" << p_material.GetGPUInstances() << "</gpuInstances>\n";

    if (auto* shader = p_material.GetShader())
    {
        for (const auto& [name, value] : p_material.GetUniformsData())
        {
            const auto* uniformInfo = shader->GetUniformInfo(name);
            if (!uniformInfo || !value.has_value())
                continue;

            const auto type = UniformTypeToString(uniformInfo->type);
            if (type.empty())
                continue;

            output << "  <uniform name=\"" << EscapeXml(name) << "\" type=\"" << type
                << "\" value=\"" << EscapeXml(SerializeUniformValue(uniformInfo->type, value)) << "\"/>\n";
        }
    }

    output << "</root>\n";
}

bool NLS::Render::Resources::Loaders::MaterialLoader::Destroy(Material*& p_material)
{
    if (!p_material)
        return false;

    delete p_material;
    p_material = nullptr;
    return true;
}
