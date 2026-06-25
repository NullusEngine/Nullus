#include "Rendering/ShaderLab/ShaderLabMaterial.h"
#include "Rendering/ShaderLab/ShaderLabReloadBarrier.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace NLS::Render::ShaderLab
{
    namespace
    {
        bool TypeMatches(const ShaderLabPropertyType actual, const ShaderLabPropertyType expected)
        {
            if (actual == expected)
                return true;
            return expected == ShaderLabPropertyType::Float && actual == ShaderLabPropertyType::Range;
        }

        bool ValueMatchesPropertyType(const ShaderLabMaterialValue& value, const ShaderLabPropertyType type)
        {
            switch (type)
            {
            case ShaderLabPropertyType::Float:
            case ShaderLabPropertyType::Range:
                return std::holds_alternative<float>(value.value);
            case ShaderLabPropertyType::Int:
                return std::holds_alternative<int32_t>(value.value);
            case ShaderLabPropertyType::Vector:
            case ShaderLabPropertyType::Color:
                return std::holds_alternative<ShaderLabFloat4>(value.value);
            case ShaderLabPropertyType::Texture2D:
            case ShaderLabPropertyType::TextureCube:
                return std::holds_alternative<std::string>(value.value);
            default:
                return false;
            }
        }
    }

    ShaderLabPropertyId MakeShaderLabPropertyId(const std::string_view name)
    {
        return HashShaderLabString(name);
    }

    std::optional<float> ShaderLabMaterialValue::AsFloat() const
    {
        if (const auto* v = std::get_if<float>(&value))
            return *v;
        return std::nullopt;
    }

    std::optional<int32_t> ShaderLabMaterialValue::AsInt() const
    {
        if (const auto* v = std::get_if<int32_t>(&value))
            return *v;
        return std::nullopt;
    }

    std::optional<ShaderLabFloat4> ShaderLabMaterialValue::AsFloat4() const
    {
        if (const auto* v = std::get_if<ShaderLabFloat4>(&value))
            return *v;
        return std::nullopt;
    }

    std::string ShaderLabMaterialValue::AsTextureName() const
    {
        if (const auto* v = std::get_if<std::string>(&value))
            return *v;
        return {};
    }

    ShaderLabMaterial::ShaderLabMaterial(std::shared_ptr<ShaderLabAsset> shader)
        : m_shader(std::move(shader))
    {
        const std::unique_lock lock(m_mutex);
        RebuildFromShaderDefaults();
    }

    ShaderLabMaterialReloadDependency ShaderLabMaterial::AsReloadDependency()
    {
        std::shared_ptr<ShaderLabAsset> shader;
        {
            const std::shared_lock lock(m_mutex);
            shader = m_shader;
        }

        return {
            weak_from_this(),
            shader,
            shader != nullptr ? shader->GetGuid() : NLS::Guid::Empty()
        };
    }

    std::shared_ptr<ShaderLabAsset> ShaderLabMaterial::GetShader() const
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::shared_lock lock(m_mutex);
        return m_shader;
    }

    void ShaderLabMaterial::ReloadShader(std::shared_ptr<ShaderLabAsset> shader)
    {
        const std::unique_lock barrier(GetShaderLabReloadBarrier());
        ReloadShaderUnderBarrier(std::move(shader));
    }

    void ShaderLabMaterial::ReloadShaderUnderBarrier(std::shared_ptr<ShaderLabAsset> shader)
    {
        const auto properties = shader != nullptr
            ? shader->CopyPropertiesForMaterialReload()
            : std::vector<ShaderLabPropertyDesc>{};
        const std::unique_lock lock(m_mutex);
        ReloadShaderProperties(std::move(shader), properties);
    }

    void ShaderLabMaterial::ReloadShaderUnderBarrier(std::shared_ptr<ShaderLabAsset> shader, const ShaderLabAssetDesc& shaderDesc)
    {
        const std::unique_lock lock(m_mutex);
        ReloadShaderProperties(std::move(shader), shaderDesc.properties);
    }

    bool ShaderLabMaterial::IsBoundToShaderUnderBarrier(const std::shared_ptr<ShaderLabAsset>& shader) const
    {
        const std::shared_lock lock(m_mutex);
        return m_shader == shader;
    }

    void ShaderLabMaterial::ReloadShaderProperties(
        std::shared_ptr<ShaderLabAsset> shader,
        const std::vector<ShaderLabPropertyDesc>& properties)
    {
        std::unordered_map<std::string, ShaderLabMaterialValue> oldValuesByName;
        for (const auto& property : m_properties)
        {
            if (const auto it = m_values.find(property.id); it != m_values.end())
                oldValuesByName[property.desc.name] = it->second;
        }

        m_shader = std::move(shader);
        m_properties.clear();
        m_values.clear();

        if (m_shader != nullptr)
        {
            for (const auto& desc : properties)
            {
                PropertyRecord record;
                record.desc = desc;
                record.id = MakeShaderLabPropertyId(desc.name);
                m_properties.push_back(record);

                if (const auto old = oldValuesByName.find(desc.name); old != oldValuesByName.end())
                {
                    m_values[record.id] = ValueMatchesPropertyType(old->second, desc.type)
                        ? old->second
                        : ShaderLabMaterialValue{ desc.defaultValue };
                    oldValuesByName.erase(old);
                }
                else if (const auto orphan = m_orphans.find(desc.name); orphan != m_orphans.end())
                {
                    if (ValueMatchesPropertyType(orphan->second, desc.type))
                    {
                        m_values[record.id] = orphan->second;
                    }
                    else
                    {
                        m_values[record.id] = ShaderLabMaterialValue{ desc.defaultValue };
                    }
                    m_orphans.erase(orphan);
                }
                else
                {
                    m_values[record.id] = ShaderLabMaterialValue{ desc.defaultValue };
                }
            }
        }

        for (auto& [name, value] : oldValuesByName)
            m_orphans[name] = std::move(value);
    }

    bool ShaderLabMaterial::SetFloat(const std::string_view name, const float value)
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::unique_lock lock(m_mutex);
        return AssignValue(name, value, ShaderLabPropertyType::Float);
    }

    bool ShaderLabMaterial::SetInt(const std::string_view name, const int32_t value)
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::unique_lock lock(m_mutex);
        return AssignValue(name, value, ShaderLabPropertyType::Int);
    }

    bool ShaderLabMaterial::SetColor(const std::string_view name, ShaderLabFloat4 value)
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::unique_lock lock(m_mutex);
        return AssignValue(name, value, ShaderLabPropertyType::Color);
    }

    bool ShaderLabMaterial::SetVector(const std::string_view name, ShaderLabFloat4 value)
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::unique_lock lock(m_mutex);
        return AssignValue(name, value, ShaderLabPropertyType::Vector);
    }

    bool ShaderLabMaterial::SetTexture(const std::string_view name, std::string value)
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::unique_lock lock(m_mutex);
        const auto* property = FindProperty(name);
        if (property == nullptr ||
            (property->desc.type != ShaderLabPropertyType::Texture2D &&
                property->desc.type != ShaderLabPropertyType::TextureCube))
            return false;
        m_values[property->id] = ShaderLabMaterialValue{ std::move(value) };
        return true;
    }

    std::optional<float> ShaderLabMaterial::GetFloat(const std::string_view name) const
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::shared_lock lock(m_mutex);
        if (const auto value = GetValue(name, ShaderLabPropertyType::Float))
            return value->AsFloat();
        return std::nullopt;
    }

    std::optional<int32_t> ShaderLabMaterial::GetInt(const std::string_view name) const
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::shared_lock lock(m_mutex);
        if (const auto value = GetValue(name, ShaderLabPropertyType::Int))
            return value->AsInt();
        return std::nullopt;
    }

    std::optional<ShaderLabFloat4> ShaderLabMaterial::GetColor(const std::string_view name) const
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::shared_lock lock(m_mutex);
        if (const auto value = GetValue(name, ShaderLabPropertyType::Color))
            return value->AsFloat4();
        return std::nullopt;
    }

    std::optional<ShaderLabFloat4> ShaderLabMaterial::GetVector(const std::string_view name) const
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::shared_lock lock(m_mutex);
        if (const auto value = GetValue(name, ShaderLabPropertyType::Vector))
            return value->AsFloat4();
        return std::nullopt;
    }

    std::optional<std::string> ShaderLabMaterial::GetTexture(const std::string_view name) const
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::shared_lock lock(m_mutex);
        const auto* property = FindProperty(name);
        if (property == nullptr ||
            (property->desc.type != ShaderLabPropertyType::Texture2D &&
                property->desc.type != ShaderLabPropertyType::TextureCube))
            return std::nullopt;

        if (const auto value = m_values.find(property->id); value != m_values.end())
            return value->second.AsTextureName();
        return std::nullopt;
    }

    void ShaderLabMaterial::EnableKeyword(std::string keyword)
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::unique_lock lock(m_mutex);
        m_keywords.Enable(std::move(keyword));
    }

    void ShaderLabMaterial::DisableKeyword(const std::string_view keyword)
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::unique_lock lock(m_mutex);
        m_keywords.Disable(keyword);
    }

    bool ShaderLabMaterial::IsKeywordEnabled(const std::string_view keyword) const
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::shared_lock lock(m_mutex);
        return m_keywords.Contains(keyword);
    }

    ShaderLabKeywordSet ShaderLabMaterial::GetKeywords() const
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::shared_lock lock(m_mutex);
        return m_keywords;
    }

    std::vector<std::string> ShaderLabMaterial::GetOrphanPropertyNames() const
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::shared_lock lock(m_mutex);
        std::vector<std::string> names;
        names.reserve(m_orphans.size());
        for (const auto& [name, value] : m_orphans)
            names.push_back(name);
        std::sort(names.begin(), names.end());
        return names;
    }

    std::optional<ShaderLabMaterialValue> ShaderLabMaterial::GetOrphanValue(const std::string_view name) const
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::shared_lock lock(m_mutex);
        const auto found = m_orphans.find(std::string(name));
        return found != m_orphans.end() ? std::optional<ShaderLabMaterialValue>(found->second) : std::nullopt;
    }

    int32_t ShaderLabMaterial::GetRenderQueueOverride() const
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::shared_lock lock(m_mutex);
        return m_renderQueueOverride;
    }

    void ShaderLabMaterial::SetRenderQueueOverride(const int32_t value)
    {
        const std::shared_lock barrier(GetShaderLabReloadBarrier());
        const std::unique_lock lock(m_mutex);
        m_renderQueueOverride = value;
    }

    void ShaderLabMaterial::RebuildFromShaderDefaults()
    {
        RebuildFromShaderDefaults(m_shader != nullptr ? m_shader->GetProperties() : std::vector<ShaderLabPropertyDesc>{});
    }

    void ShaderLabMaterial::RebuildFromShaderDefaults(const std::vector<ShaderLabPropertyDesc>& properties)
    {
        m_properties.clear();
        m_values.clear();
        if (m_shader == nullptr)
            return;

        for (const auto& desc : properties)
        {
            PropertyRecord record;
            record.desc = desc;
            record.id = MakeShaderLabPropertyId(desc.name);
            m_properties.push_back(record);
            m_values[record.id] = ShaderLabMaterialValue{ desc.defaultValue };
        }
    }

    const ShaderLabMaterial::PropertyRecord* ShaderLabMaterial::FindProperty(const std::string_view name) const
    {
        const auto found = std::find_if(
            m_properties.begin(),
            m_properties.end(),
            [name](const PropertyRecord& property)
            {
                return property.desc.name == name;
            });
        return found != m_properties.end() ? &*found : nullptr;
    }

    bool ShaderLabMaterial::ValueMatchesType(const ShaderLabMaterialValue& value, const ShaderLabPropertyType type)
    {
        return ValueMatchesPropertyType(value, type);
    }

    bool ShaderLabMaterial::AssignValue(
        const std::string_view name,
        ShaderLabValueVariant value,
        const ShaderLabPropertyType expectedType)
    {
        const auto* property = FindProperty(name);
        if (property == nullptr || !TypeMatches(property->desc.type, expectedType))
            return false;

        m_values[property->id] = ShaderLabMaterialValue{ std::move(value) };
        return true;
    }

    std::optional<ShaderLabMaterialValue> ShaderLabMaterial::GetValue(
        const std::string_view name,
        const ShaderLabPropertyType expectedType) const
    {
        const auto* property = FindProperty(name);
        if (property == nullptr || !TypeMatches(property->desc.type, expectedType))
            return std::nullopt;

        const auto found = m_values.find(property->id);
        return found != m_values.end() ? std::optional<ShaderLabMaterialValue>(found->second) : std::nullopt;
    }
}
