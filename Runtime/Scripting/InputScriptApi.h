#pragma once
#include "ScriptingDef.h"
#include <cstdint>
#include <string>

namespace NLS::Scripting
{
// Fixed-width ABI data shared by C# and Lua. No native callback or user pointer
// survives a script reload. Event iteration uses each subscriber's sequence.
struct ScriptInputValue
{
    float x = 0, y = 0, z = 0;
    uint32_t flags = 0;
    uint64_t sequence = 0, tick = 0, epoch = 0;
    double timestamp = 0;
    uint32_t phase = 0, reason = 0;
};
class NLS_SCRIPTING_API InputScriptApi final
{
public:
    // operation: 0 Dynamic snapshot, 1 Fixed snapshot, 2/3 next ordered event.
    static bool Read(uint64_t user, const char* id, uint32_t operation, uint64_t after, ScriptInputValue& result);
    // 0 Push, 1 Pop, 2 Enable, 3 Disable, 4 Default, 5 Unbind, 6 Pair,
    // 7 Unpair, 8 BeginRebind, 9 PollRebind, 10 ApplyRebind, 11 CancelRebind,
    // 12 SaveProfile, 13 LoadProfile, 14..18 users/devices, 19 rebind conflicts,
    // 20/21 resolve canonical ActionId halves, 22 PlayerInput's user.
    // Rebind operations return opaque ids/status.
    static bool Command(uint64_t user, uint32_t operation, const char* id, uint64_t argument, uint64_t& result, std::string& error);
    static std::string BindingDisplayName(uint64_t user, const char* binding);
    static void Reset();
    static void UpdateRebindings();
};
}
