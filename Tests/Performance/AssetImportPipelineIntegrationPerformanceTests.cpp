#define NLS_REGISTER_LONG_RUNNING_ASSET_IMPORT_PIPELINE_TESTS 1
// The moved cases drive the model and texture import pipeline against real
// project files and artifact databases, so they stay out of the default unit
// binary and run from NullusPerformanceTests instead.
#include "../Unit/AssetImportPipelineTests.cpp"
