#include "ParticleBounds.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace NLS::VFX
{
ParticleBounds CalculateParticleBounds(const ParticleDataSet& data)
{
    ParticleBounds result; result.particleCount = data.Count();
    const auto position = data.FindAttribute("Particles.Position"), size = data.FindAttribute("Particles.Size");
    if (position < 0 || data.Attributes()[position].type != ValueType::Float3 ||
        (size >= 0 && data.Attributes()[size].type != ValueType::Float && data.Attributes()[size].type != ValueType::Float2))
    { result.invalidCount = result.particleCount; return result; }
    for (uint32_t index = 0; index < data.Count(); ++index)
    {
        const auto center = data.Read(position,index);
        const auto extent = size < 0 ? Value{0.05f,0.05f,0,0} : data.Read(size,index);
        const float x = std::abs(extent[0]), y = size >= 0 && data.Attributes()[size].type == ValueType::Float ? x : std::abs(extent[1]);
        const float radius = std::hypot(x*0.5f,y*0.5f);
        if (!std::isfinite(center[0]) || !std::isfinite(center[1]) || !std::isfinite(center[2]) || !std::isfinite(radius))
        { ++result.invalidCount; continue; }
        if (index == result.invalidCount) result.minimum = result.maximum = center;
        else for (uint32_t component = 0; component < 3; ++component)
        {
            result.minimum[component] = (std::min)(result.minimum[component],center[component]);
            result.maximum[component] = (std::max)(result.maximum[component],center[component]);
        }
        result.spriteRadius = (std::max)(result.spriteRadius,radius);
    }
    result.minimum[3] = result.maximum[3] = 0;
    return result;
}

ParticleBounds TransformParticleBounds(const ParticleBounds& bounds, const std::array<float,16>& transform,
    const Value& cameraRight, const Value& cameraUp)
{
    ParticleBounds result; result.particleCount = bounds.particleCount; result.invalidCount = bounds.invalidCount;
    if (bounds.Empty()) return result;
    for (uint32_t row = 0; row < 3; ++row)
    {
        // Accumulate endpoints directly to avoid overflow in (minimum+maximum)/2.
        float lower = transform[row*4+3], upper = lower;
        for (uint32_t column = 0; column < 3; ++column)
        {
            const auto a = transform[row*4+column]*bounds.minimum[column], b = transform[row*4+column]*bounds.maximum[column];
            lower += (std::min)(a,b); upper += (std::max)(a,b);
        }
        const auto radius = bounds.spriteRadius*(std::abs(cameraRight[row])+std::abs(cameraUp[row]));
        result.minimum[row] = lower-radius; result.maximum[row] = upper+radius;
    }
    return result;
}

ParticleViewStatistics CalculateParticleVisibility(const ParticleDataSet& data, const SpriteView& view)
{
    ParticleViewStatistics result; result.particleCount = data.Count();
    const auto position = data.FindAttribute("Particles.Position"), size = data.FindAttribute("Particles.Size");
    if (position < 0 || data.Attributes()[position].type != ValueType::Float3 ||
        (size >= 0 && data.Attributes()[size].type != ValueType::Float && data.Attributes()[size].type != ValueType::Float2))
    { result.invalidCount = result.particleCount; return result; }
    for (uint32_t index = 0; index < data.Count(); ++index)
    {
        const auto point = data.Read(position,index);
        const auto dimensions = size < 0 ? Value{0.05f,0.05f,0,0} : data.Read(size,index);
        const float x = std::abs(dimensions[0])*0.5f;
        const float y = size >= 0 && data.Attributes()[size].type == ValueType::Float ? x : std::abs(dimensions[1])*0.5f;
        Value center{}, extent{};
        bool valid = std::isfinite(x) && std::isfinite(y);
        for (uint32_t row = 0; row < 3; ++row)
        {
            center[row] = view.localToWorld[row*4+3];
            for (uint32_t column = 0; column < 3; ++column) center[row] += view.localToWorld[row*4+column]*point[column];
            // Conservative for arbitrary in-plane rotation and velocity alignment.
            extent[row] = (std::abs(view.cameraRight[row])+std::abs(view.cameraUp[row]))*std::hypot(x,y);
            valid &= std::isfinite(center[row]) && std::isfinite(extent[row]);
        }
        if (!valid) { ++result.invalidCount; continue; }
        bool visible = true;
        if (view.frustumCulling)
            for (uint32_t plane = 0; plane < 6; ++plane)
            {
                Value equation{};
                for (uint32_t component = 0; component < 4; ++component)
                {
                    const auto& matrix = view.viewProjection;
                    equation[component] = plane == 4 ? matrix[8+component] : matrix[12+component] +
                        (plane%2 ? -1.0f : 1.0f)*matrix[(plane/2)*4+component];
                }
                float distance = equation[3], radius = 0;
                for (uint32_t component = 0; component < 3; ++component)
                { distance += equation[component]*center[component]; radius += std::abs(equation[component])*extent[component]; }
                if (distance+radius < -1e-5f*(std::abs(distance)+radius+1)) { visible = false; break; }
            }
        if (visible) ++result.visibleCount; else ++result.culledCount;
    }
    result.instanceCount = result.visibleCount;
    return result;
}
}
