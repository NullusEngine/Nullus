#define NLS_REGISTER_LONG_RUNNING_GAME_OBJECT_ASSET_IMPORT_TESTS 1
// The moved cases import real model and scene artifacts and restore prefab
// instances through the editor asset database, so they stay out of the default
// unit binary and run from NullusPerformanceTests instead.
#include "../Unit/GameObjectAssetImportTests.cpp"
