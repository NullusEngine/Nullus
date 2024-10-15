#include "AssemblyCore.h"
#include "Serialize/Serializer.h"
#include "Serialize/CommonSerialize.h"
namespace NLS
{
void AssemblyCore::Initialize()
{
    Serializer::Instance()->AddHandler<CommonValueHandler>();
}
}