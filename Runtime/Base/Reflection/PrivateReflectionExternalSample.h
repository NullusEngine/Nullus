#pragma once

#include "ExternalReflectionRegistration.h"
#include "Reflection/PrivateReflectionExternalSample.generated.h"

namespace NLS::meta
{
class PrivateReflectionExternalSample
{
public:
    GENERATED_BODY()
    PrivateReflectionExternalSample() = default;

private:
    friend struct PrivateReflectionExternalSampleReflectionAccess;

    int m_hiddenValue = 42;

    int GetHiddenValue() const
    {
        return m_hiddenValue;
    }
};

struct PrivateReflectionExternalSampleReflectionAccess
{
    static auto HiddenValue()
    {
        return &PrivateReflectionExternalSample::m_hiddenValue;
    }

    static auto GetHiddenValue()
    {
        return &PrivateReflectionExternalSample::GetHiddenValue;
    }
};

NLS_META_EXTERNAL_TYPE_NAME(NLS::meta::PrivateReflectionExternalSample)

inline void RegisterPrivateReflectionExternalSample(
    ReflectionDatabase& db,
    ReflectionRegistrationPhase phase)
{
    NLS_META_EXTERNAL_BEGIN(NLS::meta::PrivateReflectionExternalSample)
        NLS_META_EXTERNAL_FIELD_NAMED(
            int,
            "m_hiddenValue",
            PrivateReflectionExternalSampleReflectionAccess::HiddenValue(),
            PrivateReflectionExternalSampleReflectionAccess::HiddenValue());
        NLS_META_EXTERNAL_METHOD(
            "GetHiddenValue",
            PrivateReflectionExternalSampleReflectionAccess::GetHiddenValue());
    NLS_META_EXTERNAL_END();
}
} // namespace NLS::meta

NLS_META_EXTERNAL_MODULE(NLS::meta::RegisterPrivateReflectionExternalSample)
