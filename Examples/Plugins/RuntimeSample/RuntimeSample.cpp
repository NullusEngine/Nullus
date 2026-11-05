#include <NullusPlugin.h>
#include <cstring>
#include <new>

namespace
{
NlsPluginHeader Header(uint32_t size) { return {size, NLS_PLUGIN_ABI_MAJOR, NLS_PLUGIN_ABI_MINOR, 0}; }
NlsPluginString Text(const char* value) { return {value, static_cast<uint64_t>(std::strlen(value))}; }
struct RuntimeSample
{
    NlsPluginHostApi host{};
    NlsPluginHandle token = 0;
    uint64_t ticks = 0;
};
NlsPluginResult NLS_PLUGIN_CALL Invoke(void* context, NlsPluginCall* call)
{
    auto& sample = *static_cast<RuntimeSample*>(context);
    if (!call || call->header.struct_size < sizeof(*call)) return NLS_PLUGIN_INVALID;
    if (call->event == NLS_PLUGIN_EVENT_TICK) ++sample.ticks;
    if (call->event == NLS_PLUGIN_EVENT_BEGIN) sample.host.log(sample.host.host_context,sample.host.owner,Text("RuntimeSample runtime session begin"));
    if (call->event == NLS_PLUGIN_EVENT_TICK && sample.ticks == 3) sample.host.log(sample.host.host_context,sample.host.owner,Text("RuntimeSample received third runtime session tick"));
    if (call->event == NLS_PLUGIN_EVENT_END) sample.host.log(sample.host.host_context,sample.host.owner,Text("RuntimeSample runtime session end"));
    call->value = sample.ticks;
    // The test invokes event 99 from inside this DLL callback. This is an
    // asynchronous request: the host must never wait on this very stack.
    if (call->event == 99) return sample.host.request_unload(sample.host.host_context, sample.host.owner);
    if (call->event == 98) call->value = sample.host.authority_id;
    if (call->event == 97)
    {
        char buffer[256]; uint64_t size = 0;
        const auto result = sample.host.read_resource(sample.host.host_context, sample.host.owner,
            Text("Assets/Plugins/org.nullus.sample.runtime/content/RuntimeMessage.txt"), buffer, sizeof(buffer), &size);
        if (result != NLS_PLUGIN_OK) return result;
        call->value = size;
    }
    return NLS_PLUGIN_OK;
}
NlsPluginResult NLS_PLUGIN_CALL Create(const NlsPluginHostApi* host, void** instance)
{
    if (!host || !instance || host->header.struct_size < sizeof(*host) || host->header.abi_major != NLS_PLUGIN_ABI_MAJOR) return NLS_PLUGIN_INVALID;
    auto* sample = new (std::nothrow) RuntimeSample;
    if (!sample) return NLS_PLUGIN_FAILED;
    sample->host = *host; *instance = sample; return NLS_PLUGIN_OK;
}
NlsPluginResult NLS_PLUGIN_CALL Startup(void* instance)
{
    auto& sample = *static_cast<RuntimeSample*>(instance);
    NlsPluginExtension extension{Header(sizeof(NlsPluginExtension)), Text("org.nullus.sample.runtime.counter"), Text("runtime.play"),
        0, 0, &sample, Invoke, nullptr};
    const auto result = sample.host.register_extension(sample.host.host_context, sample.host.owner, &extension, &sample.token);
    if (result == NLS_PLUGIN_OK) sample.host.log(sample.host.host_context, sample.host.owner, Text("RuntimeSample started"));
    return result;
}
void NLS_PLUGIN_CALL RequestStop(void*) {}
void NLS_PLUGIN_CALL Abort(void* instance)
{
    auto& sample = *static_cast<RuntimeSample*>(instance);
    if (sample.token) sample.host.unregister_extension(sample.host.host_context, sample.host.owner, sample.token);
    sample.token = 0;
}
void NLS_PLUGIN_CALL Shutdown(void* instance) { Abort(instance); }
void NLS_PLUGIN_CALL Destroy(void* instance) { delete static_cast<RuntimeSample*>(instance); }
NlsPluginResult NLS_PLUGIN_CALL IsStopped(void*) { return NLS_PLUGIN_OK; }
}
extern "C" NLS_PLUGIN_EXPORT NlsPluginResult NLS_PLUGIN_CALL NullusPlugin_QueryApi(uint32_t major, uint32_t, NlsPluginApi* api)
{
    if (major != NLS_PLUGIN_ABI_MAJOR || !api || api->header.struct_size < sizeof(*api)) return NLS_PLUGIN_INVALID;
    *api = {Header(sizeof(NlsPluginApi)), NLS_PLUGIN_CAP_SAFE_UNLOAD, NLS_PLUGIN_OS, NLS_PLUGIN_ARCH, 64, 1,
        Text("C++ runtime owned by plugin"), Create, Startup, RequestStop, Abort, Shutdown, Destroy, IsStopped};
    return NLS_PLUGIN_OK;
}
