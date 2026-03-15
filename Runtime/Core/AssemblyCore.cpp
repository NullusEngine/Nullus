#include "AssemblyCore.h"
#include "Serialize/Serializer.h"
#include "Serialize/CommonSerialize.h"
#include "MetaGenerated.h"

namespace
{
[[maybe_unused]] const auto* g_metaGeneratedCoreAnchor = &NLS_META_GENERATED_LINK_FUNCTION;
}

namespace NLS
{
void AssemblyCore::Initialize()
{
    Serializer::Instance()->AddHandler<CommonValueHandler>();
}
}
