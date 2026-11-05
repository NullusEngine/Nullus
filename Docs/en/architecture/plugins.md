# Developing, Installing, and Publishing Nullus Plugins

Read the ABI, ownership, and unload contracts in `Runtime/Plugins/SDK/README.md` first. The full design, the verification that has actually been executed, and the known limits are recorded in `Docs/Plans/PluginSystemImplementation.md` and `Docs/Plans/PluginSystemValidation.md`.

## Running the Native Samples

In the repository PowerShell (an existing build directory named build is assumed):

```powershell
& Tools/BuildCleanEnvironment.ps1 cmake --build build --config Debug --target NullusRuntimeSamplePackage NullusPluginSDKPackage
& build/bin/Debug/NullusPlugins.exe D:/MyGame install-dir D:/Code/Nullus/build/plugin-examples/RuntimeSample/Debug
& build/bin/Debug/NullusPlugins.exe D:/MyGame enable org.nullus.sample.runtime
```

Installing and enabling never runs scripts from the package and never hot-swaps native code in the running editor. After the editor restarts, the Runtime sample registers a `runtime.play` extension, and the Play tick increments a counter. The Plugin Manager is under the Window menu and shows the source, version, dependencies, target and running state, and diagnostics. Disabling and updating are stored and take effect after the requested restart.

EditorTools is the source sample from the same build. Put it and RuntimeSample into the project under Plugins, enable both, inspect the CMake content, and then run `approve-build org.nullus.sample.editor`. Configure it like this:

```powershell
& Tools/BuildCleanEnvironment.ps1 cmake -S . -B build '-DNLS_PLUGIN_PROJECT_ROOT=D:/MyGame'
& Tools/BuildCleanEnvironment.ps1 cmake --build build --config Debug --target Editor Game
& App/Win64_Debug_Runtime_Shared/Editor.exe D:/MyGame
```

The generated factory source must take part in the host build; the editor refuses to start when it is missing from the target or when the source has changed. The sample adds a status panel under Window and a logging command under Tools. Source plugins do not use global static construction for registration.

## Manifests and Backends

A native module declares name, type, loadingPhase, and hosts, plus exactly one binary or sourceTarget, and it declares safeUnload explicitly when hot unload is required. Required and optional plugin dependencies go into dependencies, and per-module dependencies go into each module's dependencies. Runtime cannot depend on Editor or Developer; asset mount namespaces must not overlap, and dynamic reads need runtimeKeep. Required private dynamic dependencies are listed in runtimeDependencies.

The Lua sample is `Examples/Plugins/LuaRuntime`. It returns startup and shutdown tables and uses host.register_extension and host.start_coroutine. The plugin environment shares the existing VM and isolates game globals, and a resource that is used up belongs to the current module.

The C# sample is `Examples/Plugins/CSharpRuntime`. It is compiled with the repository .NET 8 SDK from a csproj, and the output DLL goes into Binaries. The module declares scriptBackend=CSharp, script, and entry; the entry point implements Nullus.Plugins.IPlugin. Use PluginContext to register extensions and to track unsubscriptions, objects, handles, tasks, and threads. Unload is a cooperative process; static events, GCHandle, or a thread stack that still holds a reference requires a restart. The intentionally leaking type in the sample is only compiled under the acceptance macro.

## Package Commands

```text
NullusPlugins <project> list
NullusPlugins <project> search <text>
NullusPlugins <project> install-dir <directory>
NullusPlugins <project> install-zip <archive.zip>
NullusPlugins <project> install-git <https-url> <full-commit>
NullusPlugins <project> search-registry <https-index-url> <text>
NullusPlugins <project> install-registry <https-index-url> <id> <range>
NullusPlugins <project> update-registry <https-index-url> <id> <range>
NullusPlugins <project> enable|disable|uninstall <id>
NullusPlugins <project> resolve|validate|restore|repair
NullusPlugins <project> approve-build <id>
```

A directory source can be installed again with install-dir or update-dir. restore only uses a valid Lockfile and the project cache, and repair can only re-acquire missing or damaged content when the lock file provides sufficient source and integrity information. The source build authorization covers the source and every file in the package, except the `.git` repository directory, including the Python, PowerShell, and tool binaries that CMake runs indirectly; executability is not decided by file extension. Any change to a file in the package requires a new `approve-build`, and the old script-file authorization policy no longer applies. Generated files and build output must go into a build directory outside the package, so that nothing is written back into a read-only cache and no reconfiguration loop forms. The authorization is not a CMake sandbox: once it is accepted, the build scripts may run the external steps that they declare.

A registry is versioned JSON and does not speak the npm or UPM server protocols: the top level is `schemaVersion:1, packages:[]`, and every package contains id and versions, where each version carries the version, the full manifest, and `archive:{url,sha256}`. The default archive and any redirect must have the same origin, and an embedder can name additional trusted sources, absolute tool paths, and a CA in RemotePackageOptions. Git and curl come from trusted tools found through the configuration, and a missing tool is an explicit error. TLS and hashes are not a publisher signature.

## Publishing and Troubleshooting

Regular publishing uses the existing BuildPipeline of NullusBuild and the editor. The Game target and source plugins must be built for the target platform, toolchain, and configuration, and the matching Player template has to be selected. A Build freezes the plugin configuration, the artifacts, and the content, deploys the Runtime modules, the explicit dependencies, and the retained assets into Plugins, and writes the plugin manifest hash into Player.info. The Game does not read plugins from the source project or from a global PackageCache.

The minimal native publishing check runs `NullusPlugins <project> stage-player <fresh-output>`, then copies the real Game and the dependencies listed by its Game deployment manifest into the output and runs `Game --plugin-self-test`. This is the graphics-free plugin diagnostic entry point of a real Game; it does not replace an ordinary Game scene or graphics run, and it does not pretend to initialize the script VM.

For an ABI or target mismatch, check the SDK major version, bit width, architecture, and the declared dependencies first. For a stale source build, reconfigure and compile again. For a restart requirement, keep the current running state and restart. For a missing scene type, restore the correct plugin and reload from the original file instead of saving a partially recovered scene. For an interrupted commit, run resolve or repair first, and never delete the transaction log or a cache that is in use by hand.
