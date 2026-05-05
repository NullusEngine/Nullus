# Dear ImGui Extensions

Nullus keeps Dear ImGui extensions in `Runtime/UI/ImGuiExtensions` and builds
them into `NLS_UI`. Extensions placed here are source-level additions to the UI
module, not standalone third-party CMake targets.

## Add An Extension

1. Create `Runtime/UI/ImGuiExtensions/<Name>/`.
2. Copy only the source files needed by Nullus, plus upstream license text.
3. Add `UPSTREAM.md` with source URL, imported version or commit, license, and
   local compatibility notes.
4. Register the files in `Runtime/UI/ImGuiExtensions/CMakeLists.txt` with
   `nls_register_imgui_extension`.
5. Keep version compatibility shims inside the extension folder.
6. Prefer consuming the extension through `NLS_UI` include paths instead of
   adding a new target to `ThirdParty/CMakeLists.txt`.

This keeps Editor and Runtime code from accumulating one-off links for each
small ImGui helper library.
