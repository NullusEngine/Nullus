#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHIEnums.h"
#include "Rendering/RHI/Core/RHISync.h"
#include "Rendering/Settings/EPixelDataFormat.h"
#include "Rendering/Settings/EPixelDataType.h"

#if defined(_WIN32)
#include <dxgiformat.h>
#include <winerror.h>
#endif

namespace NLS::Render::RHI
{
    class RHITexture;
}

struct ID3D12CommandQueue;
struct ID3D12Device;

namespace NLS::Render::RHI::DX12
{
#if defined(_WIN32)
    struct NLS_RENDER_API DX12ReadbackLayout
    {
        uint32_t bytesPerPixel = 0;
        uint32_t rowPitch = 0;
        uint64_t readbackSize = 0;
    };

    struct NLS_RENDER_API DX12ReadbackScratchResourcePlan
    {
        bool needsNewResource = false;
        uint64_t committedCapacity = 0u;
    };

    struct NLS_RENDER_API DX12ReadbackBarrierStates
    {
        ResourceState beforeCopy = ResourceState::Unknown;
        ResourceState afterCopy = ResourceState::Unknown;
    };

    struct NLS_RENDER_API DX12ReadbackRequestValidation
    {
        bool supported = false;
        uint32_t destinationBytesPerPixel = 0u;
        std::string reason;
    };

    enum class DX12ReadbackStatusCode : uint8_t
    {
        Success,
        InvalidArgument,
        UnsupportedFormat,
        DeviceLost,
        BackendFailure
    };

    struct NLS_RENDER_API DX12ReadbackResult
    {
        DX12ReadbackStatusCode code = DX12ReadbackStatusCode::InvalidArgument;
        std::string message;
        std::shared_ptr<RHICompletionToken> completion;

        bool Succeeded() const { return code == DX12ReadbackStatusCode::Success; }
    };

    NLS_RENDER_API uint32_t GetDX12ReadbackBytesPerPixel(DXGI_FORMAT format);
    NLS_RENDER_API uint32_t ConvertDX12WaitTimeoutNanosecondsToMilliseconds(uint64_t timeoutNanoseconds);
    NLS_RENDER_API DX12ReadbackLayout BuildDX12ReadbackLayout(DXGI_FORMAT format, uint32_t width, uint32_t height);
    NLS_RENDER_API DX12ReadbackScratchResourcePlan BuildDX12ReadbackScratchResourcePlan(
        uint64_t currentCapacity,
        uint64_t requiredSize);
    NLS_RENDER_API DX12ReadbackBarrierStates BuildDX12ReadbackBarrierStates(ResourceState sourceState);
    NLS_RENDER_API DX12ReadbackRequestValidation ValidateDX12ReadbackRequest(
        DXGI_FORMAT sourceFormat,
        NLS::Render::Settings::EPixelDataFormat requestedFormat,
        NLS::Render::Settings::EPixelDataType requestedType);
    NLS_RENDER_API DX12ReadbackResult ValidateDX12ReadPixelsInputs(
        const std::shared_ptr<RHITexture>& texture,
        uint32_t width,
        uint32_t height,
        NLS::Render::Settings::EPixelDataFormat format,
        NLS::Render::Settings::EPixelDataType type,
        void* data);
    NLS_RENDER_API DX12ReadbackResult BuildDX12DeviceRemovedReadbackFailure(
        HRESULT deviceRemovedReason,
        const std::string& context);

    class NLS_RENDER_API DX12ReadbackContext final
    {
    public:
        DX12ReadbackContext();
        ~DX12ReadbackContext();

        DX12ReadbackContext(const DX12ReadbackContext&) = delete;
        DX12ReadbackContext& operator=(const DX12ReadbackContext&) = delete;
        DX12ReadbackContext(DX12ReadbackContext&&) noexcept;
        DX12ReadbackContext& operator=(DX12ReadbackContext&&) noexcept;

        DX12ReadbackResult Execute(
            ID3D12Device* device,
            ID3D12CommandQueue* graphicsQueue,
            const std::shared_ptr<RHITexture>& texture,
            uint32_t x,
            uint32_t y,
            uint32_t width,
            uint32_t height,
            NLS::Render::Settings::EPixelDataFormat format,
            NLS::Render::Settings::EPixelDataType type,
            void* data);
        DX12ReadbackResult Begin(
            ID3D12Device* device,
            ID3D12CommandQueue* graphicsQueue,
            const std::shared_ptr<RHITexture>& texture,
            uint32_t x,
            uint32_t y,
            uint32_t width,
            uint32_t height,
            NLS::Render::Settings::EPixelDataFormat format,
            NLS::Render::Settings::EPixelDataType type,
            void* data);

    private:
        struct Impl;
        std::shared_ptr<Impl> m_impl;
    };

    NLS_RENDER_API DX12ReadbackResult ExecuteDX12ReadPixels(
        ID3D12Device* device,
        ID3D12CommandQueue* graphicsQueue,
        const std::shared_ptr<RHITexture>& texture,
        uint32_t x,
        uint32_t y,
        uint32_t width,
        uint32_t height,
        NLS::Render::Settings::EPixelDataFormat format,
        NLS::Render::Settings::EPixelDataType type,
        void* data);
#endif
}
