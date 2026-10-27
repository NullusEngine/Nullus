#pragma once
#include <VFX/VfxProgram.h>
#include <gtest/gtest.h>

namespace NLS::VFX
{
inline bool RunParticleProgram(const Program& program, ParticleDataSet& data, uint32_t first, uint32_t count,
                               const ExecutionContext& context, std::vector<Value>&)
{
    std::string error;
    const auto result = Typed::ExecuteParticleProgram(program, data, first, count, context, error);
    return result;
}
} // namespace NLS::VFX
