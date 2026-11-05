#pragma once
#include <cstdint>
namespace NLS::Scripting
{
struct ScriptPluginHostApi
{
    uint32_t size = sizeof(ScriptPluginHostApi), version = 1;
    void* context = nullptr;
    int32_t (*registerExtension)(void*,const uint8_t*,uint32_t,const uint8_t*,uint32_t,int32_t,uint64_t) = nullptr;
    void (*log)(void*,const uint8_t*,uint32_t) = nullptr;
};
struct ScriptPluginApi
{
    uint32_t size = sizeof(ScriptPluginApi), version = 1;
    int32_t (*load)(const uint8_t*,const uint8_t*,const ScriptPluginHostApi*,uint64_t*) = nullptr;
    int32_t (*invoke)(uint64_t,uint64_t,uint32_t,double,uint64_t*) = nullptr;
    void (*stop)(uint64_t) = nullptr;
    int32_t (*poll)(uint64_t) = nullptr;
    void (*release)(uint64_t) = nullptr;
    uint32_t (*diagnostic)(uint64_t,uint8_t*,uint32_t) = nullptr;
};
}
