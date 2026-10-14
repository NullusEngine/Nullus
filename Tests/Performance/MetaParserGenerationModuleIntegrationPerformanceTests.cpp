#define NLS_REGISTER_LONG_RUNNING_META_PARSER_MODULE_TESTS 1
// The moved cases launch the published MetaParser executable against real
// headers, so they stay out of the default unit binary and run from
// NullusPerformanceTests instead. In-process generation behavior remains
// covered by MetaParserGenerationEngineTests and DataTests in the unit suite.
#include "../Unit/MetaParserGenerationModuleTests.cpp"
