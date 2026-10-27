# ImGui Node Editor

Source: https://github.com/thedmd/imgui-node-editor

Imported commit: `1e0e494f1a86885eb332ca518543108ccd88d13e`.

License: MIT; see LICENSE. Only the core implementation is included.

The sources are maintained in the engine and compiled directly into NLS_UI.
There is no separate dependency target or fetched ImGui instance.

Local changes:

- Export the public node-editor and canvas APIs through NLS_UI_API.
- Use the engine's UIDef.h for shared and static build compatibility.

Keep additions and compatibility changes in this directory and record them here.
