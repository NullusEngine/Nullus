#ifndef SELECTION_OUTLINE_MASK_CHANNELS_HLSLI
#define SELECTION_OUTLINE_MASK_CHANNELS_HLSLI

#define NLS_SELECTION_OUTLINE_MASK_CHANNEL(name, swizzle, index) \
    static const int SelectionOutlineMask##name##Index = index; \
    float SelectionOutlineMaskGet##name(float4 value) { return value.swizzle; } \
    void SelectionOutlineMaskSet##name(inout float4 value, float channelValue) { value.swizzle = channelValue; }
#include "SelectionOutlineMaskChannels.def"
#undef NLS_SELECTION_OUTLINE_MASK_CHANNEL

#endif
