#define NLS_REGISTER_LONG_RUNNING_EDITOR_ASSET_DATABASE_TESTS 1
// The moved startup cases intentionally compile real shaders from a cold Library:
// - the empty-project case verifies built-in StandardPBR import end to end;
// - the project-override case verifies compilation without overwriting project ShaderLab sources.
// The legacy-material case performs a full model and external-texture migration through startup reimport.
#include "../Unit/EditorAssetDatabaseTests.cpp"
