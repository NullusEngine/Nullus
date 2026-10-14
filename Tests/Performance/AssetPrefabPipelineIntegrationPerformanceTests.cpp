#define NLS_REGISTER_LONG_RUNNING_ASSET_PREFAB_PIPELINE_TESTS 1
// The moved cases build, invalidate, and reload prefab artifacts through the
// real import pipeline, so they stay out of the default unit binary and run
// from NullusPerformanceTests instead.
#include "../Unit/AssetPrefabPipelineTests.cpp"
