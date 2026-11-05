#pragma once

#include <Math/Quaternion.h>
#include <Math/Vector3.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace NLS::Physics
{
struct PhysicsPose
{
    Maths::Vector3 position;
    Maths::Quaternion rotation = Maths::Quaternion::Identity;
};

// Game-thread simulation samples; rendering receives copied poses/matrices.
// No authoritative Transform or backend object is changed by interpolation.
class PhysicsPoseHistory
{
public:
    void Clear() { m_valid = false; }
    void Reset(const PhysicsPose& pose, uint64_t stepIndex, uint64_t poseRevision)
    {
        m_previous = m_current = pose; m_stepIndex = stepIndex; m_poseRevision = poseRevision; m_valid = true;
    }
    void Publish(const PhysicsPose& pose, uint64_t stepIndex, uint64_t poseRevision)
    {
        if (!m_valid || poseRevision != m_poseRevision ||
            (stepIndex != m_stepIndex && stepIndex - m_stepIndex != 1))
        { Reset(pose, stepIndex, poseRevision); return; }
        if (stepIndex == m_stepIndex) return;
        m_previous = m_current; m_current = pose; m_stepIndex = stepIndex;
    }
    bool Tracks(uint64_t stepIndex, uint64_t poseRevision) const
    { return m_valid && m_stepIndex == stepIndex && m_poseRevision == poseRevision; }
    bool Interpolate(double alpha, PhysicsPose& pose) const
    {
        if (!m_valid) return false;
        const float factor = std::isfinite(alpha) ? static_cast<float>((std::clamp)(alpha, 0.0, 1.0)) : 1.0f;
        pose.position = m_previous.position + (m_current.position - m_previous.position) * factor;
        pose.rotation = Maths::Quaternion::Normalize(Maths::Quaternion::Slerp(m_previous.rotation, m_current.rotation, factor));
        return true;
    }
private:
    PhysicsPose m_previous, m_current;
    uint64_t m_stepIndex = 0, m_poseRevision = 0;
    bool m_valid = false;
};
}
