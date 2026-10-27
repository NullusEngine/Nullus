#pragma once

#include <set>
#include "VfxTypedBytecode.h"

namespace NLS::VFX
{
using Program = Typed::BytecodeProgram;
using Instruction = Typed::BytecodeInstruction;

// Runs an EmitterUpdate/EmitterSpawn program over a single emitter-state row.
// The scheduler tick is CPU-owned for both simulation targets.
class NLS_VFX_API EmitterScript
{
public:
    bool Initialize(std::vector<Attribute> attributes, const Program& update, const Program& spawn = {});
    bool Reset(const ExecutionContext& context = {});
    bool Tick(const ExecutionContext& context);
    [[nodiscard]] bool HasUpdate() const { return !m_update.instructions.empty() || !m_spawn.instructions.empty(); }
    [[nodiscard]] bool HasWrite(std::string_view name) const;
    [[nodiscard]] float Read(std::string_view name, float fallback = 0) const;
    [[nodiscard]] Value ReadValue(std::string_view name, const Value& fallback = {}) const;
    [[nodiscard]] const ParticleDataSet& Data() const { return m_data; }

private:
    ParticleDataSet m_data;
    Program m_update, m_spawn;
    std::vector<int32_t> m_bindings, m_spawnBindings;
    Typed::TypeRegistry m_types;
    std::vector<Attribute> m_attributes;
    std::set<std::string> m_writes;
};
}
