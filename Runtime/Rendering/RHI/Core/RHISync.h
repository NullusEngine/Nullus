#pragma once

#include "Rendering/RHI/Core/RHIEnums.h"

namespace NLS::Render::RHI
{
    enum class RHICompletionStatusCode : uint8_t
    {
        Pending,
        Success,
        Failed
    };

    struct NLS_RENDER_API RHICompletionStatus
    {
        RHICompletionStatusCode code = RHICompletionStatusCode::Pending;
        std::string message;

        bool IsComplete() const { return code != RHICompletionStatusCode::Pending; }
        bool Succeeded() const { return code == RHICompletionStatusCode::Success; }
    };

    class NLS_RENDER_API RHICompletionToken : public RHIObject
    {
    public:
        virtual bool IsComplete() const = 0;
        virtual RHICompletionStatus GetStatus() const = 0;
        virtual RHICompletionStatus Wait(uint64_t timeoutNanoseconds = 0) = 0;
    };

    class NLS_RENDER_API RHIFence : public RHIObject
    {
    public:
        virtual bool IsSignaled() const = 0;
        virtual void Reset() = 0;
        virtual bool Wait(uint64_t timeoutNanoseconds = 0) = 0;
        virtual NativeHandle GetNativeFenceHandle() { return {}; } // For backend-specific access
    };

    class NLS_RENDER_API RHISemaphore : public RHIObject
    {
    public:
        virtual bool IsSignaled() const = 0;
        virtual void Reset() = 0;
        virtual NativeHandle GetNativeSemaphoreHandle() { return {}; } // For backend-specific access
    };
}
