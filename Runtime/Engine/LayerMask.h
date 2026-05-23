#pragma once

#include <cstdint>

#include "EngineDef.h"

namespace NLS::Engine
{
class NLS_ENGINE_API LayerMask
{
public:
    constexpr LayerMask(uint32_t p_mask = 0u)
        : m_mask(p_mask)
    {
    }

    [[nodiscard]] constexpr uint32_t GetMask() const
    {
        return m_mask;
    }

    constexpr void SetMask(uint32_t p_mask)
    {
        m_mask = p_mask;
    }

    [[nodiscard]] constexpr bool ContainsLayer(int p_layer) const
    {
        return p_layer >= 0 && p_layer < 32 && (m_mask & (1u << static_cast<uint32_t>(p_layer))) != 0u;
    }

private:
    uint32_t m_mask = 0u;
};
} // namespace NLS::Engine
