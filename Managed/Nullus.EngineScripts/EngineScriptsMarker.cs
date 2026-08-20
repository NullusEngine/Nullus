using System;
using Nullus;

namespace Nullus.EngineScripts;

// EngineScripts is loaded as a dependency of GameScripts.  This explicit
// registration entry point is called by GameScripts before the shared ABI
// table is returned, so built-in and project Behaviours share one registry.
internal static class EngineScriptsMarker
{
}

public static class EngineScriptsExports
{
    public static void Register()
    {
        NullusScriptAssemblyRegistration.Register();
    }
}
