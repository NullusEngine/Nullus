#include "NullusPlugin.h"
#if NLS_FIXTURE_MODE == 4
extern "C" NLS_PLUGIN_EXPORT int NullusPlugin_DifferentSymbol() { return 0; }
#else
extern "C" NLS_PLUGIN_EXPORT NlsPluginResult NLS_PLUGIN_CALL NullusPlugin_QueryApi(uint32_t,uint32_t,NlsPluginApi* output)
{
    if (!output || output->header.struct_size < sizeof(*output)) return NLS_PLUGIN_INVALID;
    *output = {};
    output->header = {sizeof(*output),NLS_PLUGIN_ABI_MAJOR,NLS_PLUGIN_ABI_MINOR,0};
    output->os = NLS_PLUGIN_OS; output->architecture = NLS_PLUGIN_ARCH;
    output->pointer_bits = sizeof(void*) * 8; output->little_endian = 1;
#if NLS_FIXTURE_MODE == 1
    output->header.abi_major += 1;
#elif NLS_FIXTURE_MODE == 2
    output->architecture = NLS_PLUGIN_ARCH == 1 ? 2 : 1;
#endif
    return NLS_PLUGIN_OK;
}
#endif
