#pragma once

#include "Rendering/RHI/Core/RHIResource.h"

namespace NLS::Render::RHI
{
    // Two graphics-queue timestamps. Writing index 1 resolves both values into
    // Results(); callers read that buffer only after submitting the frame.
    class NLS_RENDER_API RHITimestampQuery : public RHIObject
    {
    public:
        virtual NativeHandle GetNativeQueryHandle() const = 0;
        virtual uint64_t Frequency() const = 0;
        virtual const std::shared_ptr<RHIBuffer>& Results() const = 0;
    };
}
