#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "Rendering/Resources/UniformType.h"
#include "RenderDef.h"

namespace NLS::Render::Resources
{
using MaterialParameterId = uint64_t;
using MaterialParameterValue = std::variant<std::monostate, bool, int32_t, float, std::array<float, 4>, std::string, std::array<float,16>>;

enum class MaterialBlendMode : uint8_t {Opaque,Masked,Alpha,Premultiplied,Additive};
struct MaterialShaderInterface
{
    std::string domain = "Surface";
    bool unlit = false;
    bool forwardOpaque = false;
    bool usesWorldPositionOffset = false;
    float maximumWorldDisplacement = 0;
    MaterialBlendMode blendMode = MaterialBlendMode::Opaque;
};

struct MaterialParameterDesc
{
    MaterialParameterId id = 0;
    std::string name;
    std::string displayName;
    std::string group;
    UniformType type = UniformType::UNIFORM_FLOAT;
    MaterialParameterValue defaultValue;
    bool color = false;
    bool isStatic = false;
    std::optional<std::array<float, 2>> range;
};

// Logical parameter identity and defaults are independent of target/pass layout.
struct NLS_RENDER_API MaterialParameterSchema
{
    uint32_t version = 1;
    std::vector<MaterialParameterDesc> parameters;
    const MaterialParameterDesc* Find(std::string_view name) const;
    const MaterialParameterDesc* Find(MaterialParameterId id) const;
    bool Validate(std::string& error) const;
};

NLS_RENDER_API MaterialParameterId MakeMaterialParameterId(std::string_view owner, std::string_view name);
NLS_RENDER_API bool ValidateMaterialParameterValue(UniformType type,const MaterialParameterValue& value);
NLS_RENDER_API std::string SerializeMaterialParameterSchema(const MaterialParameterSchema& schema);
NLS_RENDER_API std::optional<MaterialParameterSchema> DeserializeMaterialParameterSchema(std::string_view text);
NLS_RENDER_API std::string SerializeMaterialShaderInterface(const MaterialShaderInterface& shaderInterface);
NLS_RENDER_API std::optional<MaterialShaderInterface> DeserializeMaterialShaderInterface(std::string_view text);
}
