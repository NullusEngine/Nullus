#pragma once

#include <Math/ExternalReflection.h>
#include <VFX/Authoring/VfxAuthoring.h>
#include <Reflection/ExternalReflectionRegistration.h>
#include "Properties/PropertyPanel.h"
#include "VfxDocument.h"

namespace NLS::VFX
{
NLS_META_EXTERNAL_TYPE_NAME(NLS::VFX::SimulationTarget)
NLS_META_EXTERNAL_TYPE_NAME(NLS::VFX::SpriteBlendMode)
NLS_META_EXTERNAL_TYPE_NAME(NLS::VFX::ValueType)
}
namespace NLS::VFX::Authoring
{
NLS_META_EXTERNAL_TYPE_NAME(NLS::VFX::Authoring::InputMode)
}
namespace NLS::Editor::VFXTools
{
struct VfxSystemDetails
{
    bool fixedTick = false;
    double fixedTickDelta = 0.01667, warmupTime = 0;
    uint32_t warmupTickCount = 0;
    double warmupTickDelta = 1.0/15;
};
struct VfxEmitterDetails
{
    std::string name;
    bool enabled = true;
    VFX::SimulationTarget target = VFX::SimulationTarget::CPU;
    uint32_t capacity = 10000;
    bool sortByViewDepth = false, frustumCulling = true;
};
struct VfxStageDetails
{
    bool enabled = true;
    std::string name;
    uint32_t iterations = 1;
};
struct VfxRendererDetails
{
    Maths::Color color;
    VFX::SpriteBlendMode blend = VFX::SpriteBlendMode::Translucent;
};
NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::VFXTools::VfxSystemDetails)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::VFXTools::VfxEmitterDetails)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::VFXTools::VfxStageDetails)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::VFXTools::VfxRendererDetails)

void EnsureVfxInspectorReflection();
bool CommitInspectorEdit(VfxDocument& document,const VFX::Authoring::System& candidate,
    const Inspector::ReflectedPropertyEdit& edit,const char* label);
Inspector::ReflectedPropertyEdit DrawVfxValue(Inspector::ReflectedPropertyEditor& inspector,
                                              const char* label, VFX::ValueType type, VFX::Value& value,
                                              const Inspector::ReflectedPropertyOptions& options = {});
Inspector::ReflectedPropertyEdit DrawVfxValueType(Inspector::ReflectedPropertyEditor& inspector,
                                                  const char* label, VFX::ValueType& type, VFX::Value& value);
}
