#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "Rendering/ShaderLab/ShaderLabAsset.h"
#include "Rendering/ShaderLab/ShaderLabVariant.h"

namespace NLS::Render::ShaderLab
{
    using ShaderLabPropertyId = uint64_t;

    class ShaderLabMaterial;

    struct NLS_RENDER_API ShaderLabMaterialValue
    {
        ShaderLabValueVariant value;

        [[nodiscard]] std::optional<float> AsFloat() const;
        [[nodiscard]] std::optional<int32_t> AsInt() const;
        [[nodiscard]] std::optional<ShaderLabFloat4> AsFloat4() const;
        [[nodiscard]] std::string AsTextureName() const;
    };

    struct NLS_RENDER_API ShaderLabMaterialReloadDependency
    {
        std::weak_ptr<ShaderLabMaterial> material;
        std::weak_ptr<ShaderLabAsset> shader;
        NLS::Guid shaderGuid = NLS::Guid::Empty();

        [[nodiscard]] bool IsAlive() const { return !material.expired(); }
        [[nodiscard]] std::shared_ptr<ShaderLabMaterial> Resolve() const { return material.lock(); }
    };

    class NLS_RENDER_API ShaderLabMaterial : public std::enable_shared_from_this<ShaderLabMaterial>
    {
    public:
        explicit ShaderLabMaterial(std::shared_ptr<ShaderLabAsset> shader);

        [[nodiscard]] std::shared_ptr<ShaderLabAsset> GetShader() const;
        void ReloadShader(std::shared_ptr<ShaderLabAsset> shader);
        [[nodiscard]] ShaderLabMaterialReloadDependency AsReloadDependency();

        [[nodiscard]] bool SetFloat(std::string_view name, float value);
        [[nodiscard]] bool SetInt(std::string_view name, int32_t value);
        [[nodiscard]] bool SetColor(std::string_view name, ShaderLabFloat4 value);
        [[nodiscard]] bool SetVector(std::string_view name, ShaderLabFloat4 value);
        [[nodiscard]] bool SetTexture(std::string_view name, std::string value);

        [[nodiscard]] std::optional<float> GetFloat(std::string_view name) const;
        [[nodiscard]] std::optional<int32_t> GetInt(std::string_view name) const;
        [[nodiscard]] std::optional<ShaderLabFloat4> GetColor(std::string_view name) const;
        [[nodiscard]] std::optional<ShaderLabFloat4> GetVector(std::string_view name) const;
        [[nodiscard]] std::optional<std::string> GetTexture(std::string_view name) const;

        void EnableKeyword(std::string keyword);
        void DisableKeyword(std::string_view keyword);
        [[nodiscard]] bool IsKeywordEnabled(std::string_view keyword) const;
        [[nodiscard]] ShaderLabKeywordSet GetKeywords() const;

        [[nodiscard]] std::vector<std::string> GetOrphanPropertyNames() const;
        [[nodiscard]] std::optional<ShaderLabMaterialValue> GetOrphanValue(std::string_view name) const;
        [[nodiscard]] int32_t GetRenderQueueOverride() const;
        void SetRenderQueueOverride(int32_t value);

    private:
        friend class ShaderLabHotReloadService;

        struct PropertyRecord
        {
            ShaderLabPropertyDesc desc;
            ShaderLabPropertyId id = 0;
        };

        void ReloadShaderUnderBarrier(std::shared_ptr<ShaderLabAsset> shader);
        void ReloadShaderUnderBarrier(std::shared_ptr<ShaderLabAsset> shader, const ShaderLabAssetDesc& shaderDesc);
        [[nodiscard]] bool IsBoundToShaderUnderBarrier(const std::shared_ptr<ShaderLabAsset>& shader) const;
        void RebuildFromShaderDefaults();
        void RebuildFromShaderDefaults(const std::vector<ShaderLabPropertyDesc>& properties);
        void ReloadShaderProperties(
            std::shared_ptr<ShaderLabAsset> shader,
            const std::vector<ShaderLabPropertyDesc>& properties);
        [[nodiscard]] const PropertyRecord* FindProperty(std::string_view name) const;
        [[nodiscard]] static bool ValueMatchesType(const ShaderLabMaterialValue& value, ShaderLabPropertyType type);
        [[nodiscard]] bool AssignValue(std::string_view name, ShaderLabValueVariant value, ShaderLabPropertyType expectedType);
        [[nodiscard]] std::optional<ShaderLabMaterialValue> GetValue(std::string_view name, ShaderLabPropertyType expectedType) const;

        mutable std::shared_mutex m_mutex;
        std::shared_ptr<ShaderLabAsset> m_shader;
        std::vector<PropertyRecord> m_properties;
        std::unordered_map<ShaderLabPropertyId, ShaderLabMaterialValue> m_values;
        std::unordered_map<std::string, ShaderLabMaterialValue> m_orphans;
        ShaderLabKeywordSet m_keywords;
        int32_t m_renderQueueOverride = -1;
    };

    NLS_RENDER_API ShaderLabPropertyId MakeShaderLabPropertyId(std::string_view name);
}
