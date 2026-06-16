# Quickstart: Editor Resource Management Refactor

1. Build the editor and unit tests.
2. Run the new resource-path and catalog unit tests.
3. Launch the editor from a non-repository working directory and verify startup resources load.
4. Open the asset browser and confirm:
   - fallback icons use Nullus-style IDs,
   - previews still come from thumbnail cache,
   - Unity-named icon resources are gone from the retained tree.
5. Verify the launcher/editor brand icon resolves from executable-relative resources.

