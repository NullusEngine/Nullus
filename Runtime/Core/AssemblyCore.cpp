#include "AssemblyCore.h"
#include "Serialize/Serializer.h"
#include "Serialize/CommonSerialize.h"
#include "MetaGenerated.h"
namespace NLS
{
void AssemblyCore::Initialize()
{
    Serializer::Instance()->AddHandler<CommonValueHandler>();
    // Register all reflection types
    meta_generated::RegisterReflectionTypes();
}
}