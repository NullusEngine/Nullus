#include "MeshRenderGeometry.h"

#include <algorithm>
#include <cmath>

namespace NLS::VFX
{
namespace
{
// Mirrors the shader slot selection: a missing binding selects slot 0, a
// nonfinite index is invalid and out-of-range indices clamp to the last slot.
int32_t SelectSlot(const ParticleDataSet& data, const MeshSettings& settings, uint32_t index, uint32_t slotCount)
{
    if (settings.meshIndexAttribute.empty() || slotCount == 0) return 0;
    const auto attribute = data.FindAttribute(settings.meshIndexAttribute);
    if (attribute < 0) return 0;
    if (data.Attributes()[attribute].type != ValueType::Float) return -1;
    const float value = data.Read(attribute,index)[0];
    if (!std::isfinite(value)) return -1;
    const float rounded = std::floor(value+0.5f);
    if (rounded <= 0) return 0;
    if (rounded >= static_cast<float>(slotCount-1)) return static_cast<int32_t>(slotCount-1);
    return static_cast<int32_t>(rounded);
}

bool Sphere(const ParticleDataSet& data, uint32_t index, const MeshSettings& settings, const Value& bounds, Value& center, float& radius)
{
    const auto position = data.FindAttribute("Particles.Position");
    const auto scaleIndex = settings.scaleAttribute.empty() ? -1 : data.FindAttribute(settings.scaleAttribute);
    const auto rotationIndex = settings.orientationAttribute.empty() ? -1 : data.FindAttribute(settings.orientationAttribute);
    if (position < 0 || data.Attributes()[position].type != ValueType::Float3 ||
        (scaleIndex >= 0 && data.Attributes()[scaleIndex].type != ValueType::Float && data.Attributes()[scaleIndex].type != ValueType::Float3) ||
        (rotationIndex >= 0 && data.Attributes()[rotationIndex].type != ValueType::Float4)) return false;
    const auto point = data.Read(position,index);
    auto scale = scaleIndex < 0 ? settings.defaultScale : data.Read(scaleIndex,index);
    if (scaleIndex >= 0 && data.Attributes()[scaleIndex].type == ValueType::Float) scale[1] = scale[2] = scale[0];
    auto q = rotationIndex < 0 ? settings.defaultOrientation : data.Read(rotationIndex,index);
    for (size_t c = 0; c < 4; ++c)
        if (!std::isfinite(q[c]) || !std::isfinite(bounds[c]) || (c < 3 && (!std::isfinite(point[c]) || !std::isfinite(scale[c])))) return false;
    if (bounds[3] < 0) return false;
    const auto largest = (std::max)({std::abs(q[0]),std::abs(q[1]),std::abs(q[2]),std::abs(q[3])});
    if (largest > 1e-20f)
    {
        float length = 0;
        for (auto& value : q) { value /= largest; length += value*value; }
        for (auto& value : q) value /= std::sqrt(length);
    }
    else q = {0,0,0,1};
    Value v{bounds[0]*scale[0],bounds[1]*scale[1],bounds[2]*scale[2],0};
    const Value t{2*(q[1]*v[2]-q[2]*v[1]),2*(q[2]*v[0]-q[0]*v[2]),2*(q[0]*v[1]-q[1]*v[0]),0};
    center = {point[0]+v[0]+q[3]*t[0]+q[1]*t[2]-q[2]*t[1],
        point[1]+v[1]+q[3]*t[1]+q[2]*t[0]-q[0]*t[2],point[2]+v[2]+q[3]*t[2]+q[0]*t[1]-q[1]*t[0],0};
    radius = bounds[3]*(std::max)({std::abs(scale[0]),std::abs(scale[1]),std::abs(scale[2])});
    return std::isfinite(radius) && std::all_of(center.begin(),center.end(),[](float value) { return std::isfinite(value); });
}
}

uint32_t MeshRenderGeometry::IndexCount(const uint32_t lod) const
{
    if (levels.empty()) return 0;
    const auto& level = levels[(std::min)(static_cast<std::size_t>(lod),levels.size()-1)].mesh;
    return level ? level->GetIndexCount() : 0;
}

uint32_t MeshRenderGeometry::ChooseLod(const std::array<float,16>& viewProjection, const std::array<float,16>& localToWorld,
    const Value& cameraUp, const int32_t requested) const
{
    if (levels.empty()) return 0;
    if (requested >= 0) return (std::min)(static_cast<uint32_t>(requested) > firstLod ? static_cast<uint32_t>(requested)-firstLod : 0u,
        static_cast<uint32_t>(levels.size()-1));
    Value center{};
    for (uint32_t row = 0; row < 3; ++row)
    {
        center[row] = localToWorld[row*4+3];
        for (uint32_t column = 0; column < 3; ++column) center[row] += localToWorld[row*4+column]*bounds[column];
    }
    // Enclose the transformed sphere using the matrix norm bound, including
    // nonuniform/negative scale and shear, without reducing quality under scale.
    float normOne = 0, normInfinity = 0;
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        float row = 0, column = 0;
        for (uint32_t component = 0; component < 3; ++component)
        { row += std::abs(localToWorld[axis*4+component]); column += std::abs(localToWorld[component*4+axis]); }
        normInfinity = (std::max)(normInfinity,row); normOne = (std::max)(normOne,column);
    }
    const float radius = bounds[3]*std::sqrt(normOne*normInfinity);
    const float upLength = std::hypot(cameraUp[0],cameraUp[1],cameraUp[2]);
    if (!std::isfinite(radius) || !std::isfinite(upLength) || upLength <= 1e-6f) return 0;
    Value top = center, bottom = center;
    for (uint32_t component = 0; component < 3; ++component)
    { top[component] += cameraUp[component]*radius/upLength; bottom[component] -= cameraUp[component]*radius/upLength; }
    const auto project = [&](const Value& point, float& ndc)
    {
        const auto& m = viewProjection;
        const float y = m[4]*point[0]+m[5]*point[1]+m[6]*point[2]+m[7];
        const float w = m[12]*point[0]+m[13]*point[1]+m[14]*point[2]+m[15];
        if (!std::isfinite(y) || !std::isfinite(w) || w <= 1e-6f) return false;
        ndc = y/w;
        return std::isfinite(ndc);
    };
    float lower = 0, upper = 0;
    if (!project(bottom,lower) || !project(top,upper)) return 0;
    const float screenSize = 0.5f*std::abs(upper-lower);
    for (uint32_t lod = 0; lod+1 < levels.size(); ++lod)
        if (screenSize >= levels[lod].screenSize) return lod;
    return static_cast<uint32_t>(levels.size()-1);
}

ParticleBounds CalculateMeshParticleBounds(const ParticleDataSet& data, const MeshSettings& settings, const std::vector<Value>& slotBounds)
{
    ParticleBounds result; result.particleCount = data.Count();
    for (uint32_t index = 0; index < data.Count(); ++index)
    {
        const int32_t slot = SelectSlot(data,settings,index,static_cast<uint32_t>(slotBounds.size()));
        Value center; float radius = 0;
        if (slot < 0 || static_cast<std::size_t>(slot) >= slotBounds.size() || !Sphere(data,index,settings,slotBounds[slot],center,radius))
        { ++result.invalidCount; continue; }
        for (size_t component = 0; component < 3; ++component)
        {
            const auto lower = center[component]-radius, upper = center[component]+radius;
            if (index == result.invalidCount) { result.minimum[component] = lower; result.maximum[component] = upper; }
            else { result.minimum[component] = (std::min)(result.minimum[component],lower); result.maximum[component] = (std::max)(result.maximum[component],upper); }
        }
    }
    return result;
}

ParticleViewStatistics CalculateMeshParticleVisibility(const ParticleDataSet& data, const MeshSettings& settings,
    const std::vector<Value>& slotBounds, const SpriteView& view, const std::vector<uint32_t>& indexCounts)
{
    ParticleViewStatistics result; result.particleCount = data.Count();
    result.vertexCount = indexCounts.empty() ? 0 : indexCounts.front();
    for (uint32_t index = 0; index < data.Count(); ++index)
    {
        const int32_t slot = SelectSlot(data,settings,index,static_cast<uint32_t>(slotBounds.size()));
        Value center; float radius = 0;
        if (slot < 0 || static_cast<std::size_t>(slot) >= slotBounds.size() || !Sphere(data,index,settings,slotBounds[slot],center,radius))
        { ++result.invalidCount; continue; }
        Value world{}, extent{};
        bool valid = true;
        for (size_t row = 0; row < 3; ++row)
        {
            world[row] = view.localToWorld[row*4+3];
            for (size_t column = 0; column < 3; ++column)
            {
                world[row] += view.localToWorld[row*4+column]*center[column];
                extent[row] += std::abs(view.localToWorld[row*4+column])*radius;
            }
            valid &= std::isfinite(world[row]) && std::isfinite(extent[row]);
        }
        if (!valid) { ++result.invalidCount; continue; }
        bool visible = true;
        if (view.frustumCulling)
            for (size_t plane = 0; plane < 6; ++plane)
            {
                Value equation{};
                for (size_t c = 0; c < 4; ++c)
                    equation[c] = plane == 4 ? view.viewProjection[8+c] : view.viewProjection[12+c]+
                        (plane%2 ? -1.0f : 1.0f)*view.viewProjection[(plane/2)*4+c];
                float distance = equation[3], projectedRadius = 0;
                for (size_t c = 0; c < 3; ++c) { distance += equation[c]*world[c]; projectedRadius += std::abs(equation[c])*extent[c]; }
                if (distance+projectedRadius < -1e-5f*(std::abs(distance)+projectedRadius+1)) { visible = false; break; }
            }
        if (visible) ++result.visibleCount; else ++result.culledCount;
    }
    result.instanceCount = result.visibleCount; return result;
}
}
