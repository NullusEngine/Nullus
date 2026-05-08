#pragma once

#include "Guid.h"
#include "Reflection/ExternalReflectionRegistration.h"

namespace NLS
{
NLS_META_EXTERNAL_TYPE_NAME(NLS::Guid)

inline void RegisterGuidExternalReflection(
    NLS::meta::ReflectionDatabase& db,
    NLS::meta::ReflectionRegistrationPhase phase)
{
    NLS_META_EXTERNAL_BEGIN(NLS::Guid)
    NLS_META_EXTERNAL_END();
}
} // namespace NLS

NLS_META_EXTERNAL_MODULE(NLS::RegisterGuidExternalReflection)
