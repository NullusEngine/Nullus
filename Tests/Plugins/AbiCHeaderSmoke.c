#include <NullusPlugin.h>
_Static_assert(sizeof(NlsPluginHeader) == 16, "ABI header size");
_Static_assert(sizeof(NlsPluginCall) == 40, "ABI callback size");
_Static_assert(offsetof(NlsPluginCall,value) == 32, "ABI value offset");
_Static_assert(offsetof(NlsPluginHostApi,host_context) == 24, "ABI context offset");
void NullusPlugin_C_Header_Compiles(void) {}
