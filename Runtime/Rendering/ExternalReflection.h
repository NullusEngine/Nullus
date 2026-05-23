#pragma once

#include "Reflection/ExternalReflectionRegistration.h"
#include "Rendering/Geometry/Bounds.h"

namespace NLS::Render::Geometry
{
NLS_META_EXTERNAL_TYPE_NAME(NLS::Render::Geometry::Bounds)
}

namespace NLS::Render::Resources
{
inline void RegisterRenderingExternalReflection(
    NLS::meta::ReflectionDatabase& db,
    NLS::meta::ReflectionRegistrationPhase phase)
{
    NLS_META_EXTERNAL_BEGIN(NLS::Render::Geometry::Bounds)
        NLS_META_EXTERNAL_FIELD(NLS::Maths::Vector3, center);
        NLS_META_EXTERNAL_FIELD(NLS::Maths::Vector3, size);
    NLS_META_EXTERNAL_END();
}
} // namespace NLS::Render::Resources

NLS_META_EXTERNAL_MODULE(NLS::Render::Resources::RegisterRenderingExternalReflection)
