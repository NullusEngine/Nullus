#pragma once

#include "Macros.h"
#include "Reflection/PrivateReflectionExternalSample.generated.h"

namespace NLS::meta
{
class PrivateReflectionExternalSample
{
public:
    GENERATED_BODY()
    PrivateReflectionExternalSample() = default;

private:
    int m_hiddenValue = 42;

    int GetHiddenValue() const
    {
        return m_hiddenValue;
    }
};

MetaExternal(NLS::meta::PrivateReflectionExternalSample)

REFLECT_EXTERNAL(
    NLS::meta::PrivateReflectionExternalSample,
    Fields(
        REFLECT_PRIVATE_FIELD(int, m_hiddenValue)
    ),
    Methods(
        REFLECT_PRIVATE_METHOD(GetHiddenValue)
    )
)
} // namespace NLS::meta
