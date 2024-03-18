#include "Assembly.h"

namespace NLS
{
Assembly& Assembly::Instance()
{
    static Assembly sInstance;
    return sInstance;
}
}