#pragma once
#include <algorithm>
#include <cstring>
#include <imgui.h>
#include <VFX/Authoring/VfxAuthoring.h>
#include "Properties/PropertyPanel.h"

namespace NLS::Editor::VFXTools
{
template<class Commit>
void DrawStageControlBindings(Inspector::ReflectedPropertyEditor& inspector,NLS::VFX::SimulationStageBindings& bindings,
    std::span<const NLS::VFX::Authoring::Parameter> parameters,bool manual,Commit commit)
{
    const auto draw = [&](const char* label,std::string& name)
    {
        Inspector::ReflectedPropertyOptions options;
        if (!manual)
        {
            options.choices.push_back({"","None"});
            for (const auto& parameter : parameters) if (parameter.type == NLS::VFX::ValueType::Float || parameter.type == (std::string_view(label) == "Enabled Binding" ? NLS::VFX::ValueType::Bool : NLS::VFX::ValueType::Int32))
                options.choices.push_back({parameter.name,parameter.name});
        }
        const auto edit = inspector.Draw(std::to_string(ImGui::GetID(label)),label,name,options);
        commit(edit,"Set stage control binding");
    };
    draw("Enabled Binding",bindings.enabled);
    draw("Num Iterations Binding",bindings.iterations);
}
}
