# ImGui Node Editor

Source: https://github.com/thedmd/imgui-node-editor

Imported commit: `1e0e494f1a86885eb332ca518543108ccd88d13e`.

License: MIT; see LICENSE. Only the core implementation is included.

The sources are maintained in the engine and compiled directly into NLS_UI.
There is no separate dependency target or fetched ImGui instance.

Local changes:

- Export the public node-editor and canvas APIs through NLS_UI_API.
- Use the engine's UIDef.h for shared and static build compatibility.
- Separate keyboard accelerators from pointer gestures. An optional host observer
  routes focused graph commands through Input Actions and queues ordered requests
  into the existing navigation, deletion and clipboard actions. Clipboard ownership
  is granted only to clients using BeginShortcut. Alt+click deletion is retained.

Keep additions and compatibility changes in this directory and record them here.
