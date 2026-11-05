#pragma once
#include "CoreClrScriptBackend.h"
namespace NLS::Engine { class GameObject; namespace Components { class Component; } }
namespace NLS::Scripting
{
// Optional v2 ABI tail. Arguments and result use the existing portable value layout.
NLS_SCRIPTING_API ScriptAbiResult InvokePlayMember(uint64_t owner, uint64_t member,
    const ScriptAbiValue* arguments, uint32_t count, ScriptAbiValue* result);
bool ResolvePlayComponent(Engine::GameObject& owner,uint64_t type,bool create,Engine::Components::Component*& result);
}
