using System;
using System.Runtime.InteropServices;
using Nullus;
using Nullus.EngineScripts;

namespace Nullus.GameScripts;

// Keeps the project buildable when a project has no user scripts yet. User
// scripts are compiled from the Scripts directory into this same unloadable
// assembly.
internal static class GameScriptsMarker
{
}

// Resolving this entry point through hostfxr loads GameScripts.dll and runs
// its generated module initializer before handing the shared ABI table back
// to Native.
public static unsafe class GameScriptsExports
{
    [UnmanagedCallersOnly]
    public static IntPtr GetApiTable()
    {
        // hostfxr resolves this method as a component entry point.  Register
        // explicitly as well as through the module initializer because some
        // hosts can resolve a method before running module initializers.
        EngineScriptsExports.Register();
        NullusScriptAssemblyRegistration.Register();
        return ManagedExports.GetApiTableManaged();
    }

    // Keep diagnostics on the same project assembly entry point that Native
    // uses for the ABI handshake.  Resolving the method from Nullus.Managed.dll
    // directly can create a separate hostfxr load context on some runtimes.
    [UnmanagedCallersOnly]
    public static int CollectAndGetLiveProjectLoadContextCount()
        => ManagedExports.CollectAndGetLiveProjectLoadContextCountManaged();
}
