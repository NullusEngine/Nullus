#include "AssemblyCore.h"
#include "Serialize/Serializer.h"
#include "Serialize/CommonSerialize.h"
#include "MetaGenerated.h"

namespace NLS
{
void AssemblyCore::Initialize()
{
    NLS_META_GENERATED_LINK_FUNCTION();
    Serializer::Instance()->AddHandler<CommonValueHandler>();
}
}
