# Nullus Script Debugger extension

This is the offline VS Code command bridge used by the generated
`Nullus: C# Scripts` launch configuration. It reads the project-local
`Library/IDE/Nullus.Debug.json`, starts or reuses the exact project Editor via
the absolute `NullusDebugBroker` path, waits for `PrepareManagedDebug`, and
returns the prepared Editor PID to the `coreclr` adapter.

The extension never calls a shell, looks up `PATH`, presents a process picker,
or starts Play mode. Install the packaged VSIX once with:

```text
code --install-extension nullus-script-debugger-1.0.0.vsix
```

The generated project workspace recommends this extension as
`nullus.nullus-script-debugger`.
