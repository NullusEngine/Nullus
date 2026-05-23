#include "Assembly.h"
#include "GuidReflection.h"
#include "Reflection/PrivateReflectionExternalSample.h"

namespace NLS
{
Assembly& Assembly::Instance()
{
    static Assembly sInstance;
    return sInstance;
}
}
