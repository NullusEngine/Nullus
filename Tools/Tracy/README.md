# Tracy profiling tools

The `nullus-tracy-performance` skill installs the protocol-matched Tracy
command line tools here, one directory per vendor version:

```text
Tools/Tracy/<version>/tracy-profiler.exe
Tools/Tracy/<version>/tracy-capture.exe
Tools/Tracy/<version>/tracy-csvexport.exe
```

The version is read from `ThirdParty/Tracy/public/common/TracyVersion.hpp`, and
the build deploys the executables next to the editor in
`App/<platform>_<config>_Runtime_*/Tracy/<version>/`. The editor exposes them
through `Tools > Tracy Profiler`.

Install or refresh them with:

```powershell
./.agents/skills/nullus-tracy-performance/scripts/Prepare-NullusTracy.ps1
```

Only `Tools/Tracy/README.md` is tracked by Git; the downloaded or locally built
tool payloads are ignored.
