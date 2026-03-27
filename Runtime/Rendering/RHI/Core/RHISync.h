#pragma once

#include "Rendering/RHI/Core/RHICommon.h"

namespace NLS::Render::RHI
{
    class NLS_RENDER_API RHIFence : public RHIObject
    {
    public:
        virtual bool IsSignaled() const = 0;
        virtual void Reset() = 0;
        virtual bool Wait(uint64_t timeoutNanoseconds = 0) = 0;
    };

    class NLS_RENDER_API RHISemaphore : public RHIObject
    {
    public:
        virtual bool IsSignaled() const = 0;
        virtual void Reset() = 0;
    };
}
