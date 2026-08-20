#pragma once

#include "ScriptTypes.h"

#include <string>
#include <string_view>
#include <unordered_map>

namespace NLS::Scripting
{
using OrphanScriptFields = std::unordered_map<ScriptFieldId, std::string>;

class NLS_SCRIPTING_API ScriptFieldSerialization final
{
public:
    static ScriptStatus Serialize(
        const SerializedScriptFields& fields,
        const OrphanScriptFields& orphanFields,
        std::string& output);

    static ScriptStatus Deserialize(
        std::string_view input,
        const ScriptTypeDescriptor* descriptor,
        SerializedScriptFields& fields,
        OrphanScriptFields& orphanFields);
};
}
