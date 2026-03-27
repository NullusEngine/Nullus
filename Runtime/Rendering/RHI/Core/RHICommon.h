#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "RenderDef.h"
#include "Rendering/RHI/RHITypes.h"

namespace NLS::Render::Resources
{
    struct ShaderReflection;
}

namespace NLS::Render::RHI
{
    inline constexpr uint32_t kRHIMaxBindingSets = 4u;
    inline constexpr uint32_t kRHIMaxPushConstantBytes = 128u;

    class NLS_RENDER_API RHIObject
    {
    public:
        virtual ~RHIObject() = default;
        virtual std::string_view GetDebugName() const = 0;
    };
}
