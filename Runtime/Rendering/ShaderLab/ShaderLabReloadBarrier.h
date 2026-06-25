#pragma once

#include <shared_mutex>

namespace NLS::Render::ShaderLab
{
    inline std::shared_mutex& GetShaderLabReloadBarrier()
    {
        static std::shared_mutex barrier;
        return barrier;
    }
}
