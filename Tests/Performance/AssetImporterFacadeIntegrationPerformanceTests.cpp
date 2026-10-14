#define NLS_REGISTER_LONG_RUNNING_ASSET_IMPORTER_FACADE_TESTS 1
// The moved cases run real save/reimport round trips through the asset importer
// facade against a temporary project on disk, so they stay out of the default
// unit binary and run from NullusPerformanceTests instead.
#include "../Unit/AssetImporterFacadeTests.cpp"
