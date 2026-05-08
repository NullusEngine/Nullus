#include "Assembly.h"
#include "Reflection/PrivateReflectionExternalSample.h"

namespace NLS
{
Assembly& Assembly::Instance()
{
    static Assembly sInstance;
    return sInstance;
}
}
