#include "Rendering/RHI/Backends/DX12/DX12ReadbackUtils.h"

#if defined(_WIN32)
#include <atomic>
#include <cstring>
#include <memory>

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <Debug/Logger.h>
#include "Rendering/RHI/Backends/DX12/DX12Command.h"
#include "Rendering/RHI/Backends/DX12/DX12FormatUtils.h"
#include "Rendering/RHI/Core/RHIResource.h"
#endif

namespace NLS::Render::RHI::DX12
{
#if defined(_WIN32)
    namespace
    {
        uint32_t AlignUp(uint32_t value, uint32_t alignment)
        {
            return alignment == 0u
                ? value
                : ((value + alignment - 1u) / alignment) * alignment;
        }

        DXGI_FORMAT ToDX12ReadbackSourceFormat(TextureFormat format)
        {
            return ToDXGIFormat(format);
        }
    }

    uint32_t GetDX12ReadbackBytesPerPixel(DXGI_FORMAT format)
    {
        return GetDXGIFormatBytesPerPixel(format);
    }

    DX12ReadbackLayout BuildDX12ReadbackLayout(DXGI_FORMAT format, uint32_t width, uint32_t height)
    {
        DX12ReadbackLayout layout{};
        layout.bytesPerPixel = GetDX12ReadbackBytesPerPixel(format);

        if (layout.bytesPerPixel == 0u || width == 0u || height == 0u)
            return layout;

        const uint32_t packedRowSize = width * layout.bytesPerPixel;
        layout.rowPitch = AlignUp(packedRowSize, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        layout.readbackSize = static_cast<uint64_t>(layout.rowPitch) * static_cast<uint64_t>(height);
        return layout;
    }

    DX12ReadbackScratchResourcePlan BuildDX12ReadbackScratchResourcePlan(
        const uint64_t currentCapacity,
        const uint64_t requiredSize)
    {
        DX12ReadbackScratchResourcePlan plan{};
        plan.committedCapacity = currentCapacity;
        if (requiredSize == 0u)
            return plan;

        if (currentCapacity < requiredSize)
        {
            plan.needsNewResource = true;
            plan.committedCapacity = requiredSize;
        }
        return plan;
    }

    DX12ReadbackBarrierStates BuildDX12ReadbackBarrierStates(ResourceState sourceState)
    {
        DX12ReadbackBarrierStates states{};
        states.beforeCopy = sourceState;
        states.afterCopy = sourceState;
        return states;
    }

    DX12ReadbackRequestValidation ValidateDX12ReadbackRequest(
        DXGI_FORMAT sourceFormat,
        NLS::Render::Settings::EPixelDataFormat requestedFormat,
        NLS::Render::Settings::EPixelDataType requestedType)
    {
        DX12ReadbackRequestValidation validation{};

        const uint32_t sourceBytesPerPixel = GetDX12ReadbackBytesPerPixel(sourceFormat);
        if (sourceBytesPerPixel == 0u)
        {
            validation.reason = "Unsupported DX12 readback source format";
            return validation;
        }

        if (requestedType != NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE)
        {
            validation.reason = "DX12 readback currently supports only UNSIGNED_BYTE destination data";
            return validation;
        }

        switch (requestedFormat)
        {
        case NLS::Render::Settings::EPixelDataFormat::RGB:
        case NLS::Render::Settings::EPixelDataFormat::BGR:
            validation.destinationBytesPerPixel = 3u;
            break;
        case NLS::Render::Settings::EPixelDataFormat::RGBA:
        case NLS::Render::Settings::EPixelDataFormat::BGRA:
            validation.destinationBytesPerPixel = 4u;
            break;
        default:
            validation.reason = "Unsupported DX12 readback destination pixel format";
            return validation;
        }

        if (validation.destinationBytesPerPixel > sourceBytesPerPixel)
        {
            validation.destinationBytesPerPixel = 0u;
            validation.reason = "DX12 readback destination format requires more channels than the source format provides";
            return validation;
        }

        validation.supported = true;
        return validation;
    }

    DX12ReadbackResult ValidateDX12ReadPixelsInputs(
        const std::shared_ptr<RHITexture>& texture,
        uint32_t width,
        uint32_t height,
        NLS::Render::Settings::EPixelDataFormat format,
        NLS::Render::Settings::EPixelDataType type,
        void* data)
    {
        if (texture == nullptr)
            return { DX12ReadbackStatusCode::InvalidArgument, "ReadPixels texture is null" };
        if (data == nullptr)
            return { DX12ReadbackStatusCode::InvalidArgument, "ReadPixels destination data is null" };
        if (width == 0u || height == 0u)
            return { DX12ReadbackStatusCode::InvalidArgument, "ReadPixels region has zero width or height" };

        const auto validation = ValidateDX12ReadbackRequest(
            ToDX12ReadbackSourceFormat(texture->GetDesc().format),
            format,
            type);
        if (!validation.supported)
            return { DX12ReadbackStatusCode::UnsupportedFormat, validation.reason };

        return { DX12ReadbackStatusCode::Success, {} };
    }

    struct DX12ReadbackContext::Impl
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> readbackResource;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        HANDLE fenceEvent = nullptr;
        uint64_t readbackCapacity = 0u;
        uint64_t nextFenceValue = 0u;
        std::shared_ptr<std::atomic_bool> readbackInFlight = std::make_shared<std::atomic_bool>(false);

        ~Impl()
        {
            if (fenceEvent != nullptr)
            {
                CloseHandle(fenceEvent);
                fenceEvent = nullptr;
            }
        }
    };

    namespace
    {
        struct DX12ReadbackPendingCopy
        {
            Microsoft::WRL::ComPtr<ID3D12Resource> readbackResource;
            Microsoft::WRL::ComPtr<ID3D12Fence> fence;
            HANDLE fenceEvent = nullptr;
            std::shared_ptr<std::atomic_bool> inFlightFlag;
            uint64_t fenceValue = 0u;
            DX12ReadbackLayout layout{};
            DX12ReadbackRequestValidation validation{};
            NLS::Render::Settings::EPixelDataFormat format = NLS::Render::Settings::EPixelDataFormat::RGB;
            uint32_t width = 0u;
            uint32_t height = 0u;
            void* data = nullptr;
        };

        void CopyDX12ReadbackRowsToDestination(
            const DX12ReadbackPendingCopy& pendingCopy,
            const void* mappedData)
        {
            const auto* srcBytes = static_cast<const uint8_t*>(mappedData);
            auto* dstBytes = static_cast<uint8_t*>(pendingCopy.data);
            const size_t sourceRowPitch = static_cast<size_t>(pendingCopy.layout.rowPitch);
            const size_t packedRowSize =
                static_cast<size_t>(pendingCopy.width) *
                static_cast<size_t>(pendingCopy.validation.destinationBytesPerPixel);

            if ((pendingCopy.format == NLS::Render::Settings::EPixelDataFormat::RGB ||
                 pendingCopy.format == NLS::Render::Settings::EPixelDataFormat::BGR) &&
                pendingCopy.layout.bytesPerPixel >= 3u)
            {
                const bool swapRedBlue = pendingCopy.format == NLS::Render::Settings::EPixelDataFormat::BGR;
                for (uint32_t row = 0; row < pendingCopy.height; ++row)
                {
                    for (uint32_t col = 0; col < pendingCopy.width; ++col)
                    {
                        const size_t srcIdx =
                            row * sourceRowPitch + col * static_cast<size_t>(pendingCopy.layout.bytesPerPixel);
                        const size_t dstIdx = (static_cast<size_t>(row) * pendingCopy.width + col) * 3u;
                        dstBytes[dstIdx + 0] = srcBytes[srcIdx + (swapRedBlue ? 2u : 0u)];
                        dstBytes[dstIdx + 1] = srcBytes[srcIdx + 1];
                        dstBytes[dstIdx + 2] = srcBytes[srcIdx + (swapRedBlue ? 0u : 2u)];
                    }
                }
                return;
            }

            if (pendingCopy.format == NLS::Render::Settings::EPixelDataFormat::BGRA &&
                pendingCopy.layout.bytesPerPixel >= 4u)
            {
                for (uint32_t row = 0; row < pendingCopy.height; ++row)
                {
                    for (uint32_t col = 0; col < pendingCopy.width; ++col)
                    {
                        const size_t srcIdx =
                            row * sourceRowPitch + col * static_cast<size_t>(pendingCopy.layout.bytesPerPixel);
                        const size_t dstIdx = (static_cast<size_t>(row) * pendingCopy.width + col) * 4u;
                        dstBytes[dstIdx + 0] = srcBytes[srcIdx + 2];
                        dstBytes[dstIdx + 1] = srcBytes[srcIdx + 1];
                        dstBytes[dstIdx + 2] = srcBytes[srcIdx + 0];
                        dstBytes[dstIdx + 3] = srcBytes[srcIdx + 3];
                    }
                }
                return;
            }

            for (uint32_t row = 0; row < pendingCopy.height; ++row)
            {
                std::memcpy(
                    dstBytes + row * packedRowSize,
                    srcBytes + row * sourceRowPitch,
                    packedRowSize);
            }
        }

        class DX12ReadbackCompletionToken final : public RHICompletionToken
        {
        public:
            explicit DX12ReadbackCompletionToken(DX12ReadbackPendingCopy pendingCopy)
                : m_pendingCopy(std::move(pendingCopy))
            {
            }

            std::string_view GetDebugName() const override { return "DX12ReadbackCompletionToken"; }

            bool IsComplete() const override
            {
                if (m_status.IsComplete())
                    return true;
                return m_pendingCopy.fence != nullptr &&
                    m_pendingCopy.fence->GetCompletedValue() >= m_pendingCopy.fenceValue;
            }

            RHICompletionStatus GetStatus() const override
            {
                if (m_status.IsComplete())
                    return m_status;
                return IsComplete()
                    ? RHICompletionStatus{ RHICompletionStatusCode::Success, {} }
                    : RHICompletionStatus{ RHICompletionStatusCode::Pending, {} };
            }

            RHICompletionStatus Wait(uint64_t timeoutNanoseconds = 0) override
            {
                if (m_status.IsComplete())
                    return m_status;
                if (m_pendingCopy.fence == nullptr || m_pendingCopy.readbackResource == nullptr)
                    return CompleteWithFailure("DX12 readback completion token is missing backend resources");

                if (m_pendingCopy.fence->GetCompletedValue() < m_pendingCopy.fenceValue)
                {
                    HANDLE eventHandle = m_pendingCopy.fenceEvent;
                    HANDLE ownedEvent = nullptr;
                    if (eventHandle == nullptr)
                    {
                        ownedEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                        eventHandle = ownedEvent;
                    }
                    if (eventHandle == nullptr)
                        return CompleteWithFailure("ReadPixels failed to create fence event");

                    HRESULT hr = m_pendingCopy.fence->SetEventOnCompletion(m_pendingCopy.fenceValue, eventHandle);
                    if (FAILED(hr))
                    {
                        if (ownedEvent != nullptr)
                            CloseHandle(ownedEvent);
                        return CompleteWithFailure("ReadPixels failed to set fence completion event");
                    }

                    const DWORD waitMs =
                        timeoutNanoseconds > 0u
                            ? static_cast<DWORD>(timeoutNanoseconds / 1000000u)
                            : INFINITE;
                    const DWORD waitResult = WaitForSingleObject(eventHandle, waitMs);
                    if (ownedEvent != nullptr)
                        CloseHandle(ownedEvent);
                    if (waitResult != WAIT_OBJECT_0)
                        return { RHICompletionStatusCode::Pending, "ReadPixels completion wait timed out" };
                }

                void* mappedData = nullptr;
                D3D12_RANGE readRange{};
                readRange.Begin = 0;
                readRange.End = static_cast<SIZE_T>(m_pendingCopy.layout.readbackSize);
                HRESULT hr = m_pendingCopy.readbackResource->Map(0, &readRange, &mappedData);
                if (FAILED(hr) || mappedData == nullptr)
                    return CompleteWithFailure("ReadPixels failed to map readback resource");

                CopyDX12ReadbackRowsToDestination(m_pendingCopy, mappedData);

                D3D12_RANGE writeRange{};
                writeRange.Begin = 0;
                writeRange.End = 0;
                m_pendingCopy.readbackResource->Unmap(0, &writeRange);
                if (m_pendingCopy.inFlightFlag != nullptr)
                    m_pendingCopy.inFlightFlag->store(false);
                m_status = { RHICompletionStatusCode::Success, {} };
                return m_status;
            }

        private:
            RHICompletionStatus CompleteWithFailure(std::string message)
            {
                if (m_pendingCopy.inFlightFlag != nullptr)
                    m_pendingCopy.inFlightFlag->store(false);
                m_status = { RHICompletionStatusCode::Failed, std::move(message) };
                return m_status;
            }

            DX12ReadbackPendingCopy m_pendingCopy;
            RHICompletionStatus m_status{};
        };

        DX12ReadbackStatusCode ToDX12StatusCode(RHICompletionStatusCode code)
        {
            switch (code)
            {
            case RHICompletionStatusCode::Success:
                return DX12ReadbackStatusCode::Success;
            case RHICompletionStatusCode::Pending:
            case RHICompletionStatusCode::Failed:
            default:
                return DX12ReadbackStatusCode::BackendFailure;
            }
        }
    }

    DX12ReadbackContext::DX12ReadbackContext()
        : m_impl(std::make_shared<Impl>())
    {
    }

    DX12ReadbackContext::~DX12ReadbackContext() = default;

    DX12ReadbackContext::DX12ReadbackContext(DX12ReadbackContext&&) noexcept = default;

    DX12ReadbackContext& DX12ReadbackContext::operator=(DX12ReadbackContext&&) noexcept = default;

    DX12ReadbackResult DX12ReadbackContext::Execute(
        ID3D12Device* device,
        ID3D12CommandQueue* graphicsQueue,
        const std::shared_ptr<RHITexture>& texture,
        uint32_t x,
        uint32_t y,
        uint32_t width,
        uint32_t height,
        NLS::Render::Settings::EPixelDataFormat format,
        NLS::Render::Settings::EPixelDataType type,
        void* data)
    {
        auto result = Begin(device, graphicsQueue, texture, x, y, width, height, format, type, data);
        if (!result.Succeeded() || result.completion == nullptr)
            return result;

        const auto status = result.completion->Wait();
        result.code = ToDX12StatusCode(status.code);
        result.message = status.message;
        return result;
    }

    DX12ReadbackResult DX12ReadbackContext::Begin(
        ID3D12Device* device,
        ID3D12CommandQueue* graphicsQueue,
        const std::shared_ptr<RHITexture>& texture,
        uint32_t x,
        uint32_t y,
        uint32_t width,
        uint32_t height,
        NLS::Render::Settings::EPixelDataFormat format,
        NLS::Render::Settings::EPixelDataType type,
        void* data)
    {
        (void)type;

        if (device == nullptr)
            return { DX12ReadbackStatusCode::InvalidArgument, "ReadPixels DX12 device is null" };
        if (graphicsQueue == nullptr)
            return { DX12ReadbackStatusCode::InvalidArgument, "ReadPixels DX12 graphics queue is null" };

        const auto inputValidation = ValidateDX12ReadPixelsInputs(texture, width, height, format, type, data);
        if (!inputValidation.Succeeded())
            return inputValidation;

        auto imgHandle = texture->GetNativeImageHandle();
        ID3D12Resource* srcResource = (imgHandle.backend == NLS::Render::RHI::BackendType::DX12)
            ? static_cast<ID3D12Resource*>(imgHandle.handle)
            : nullptr;
        if (srcResource == nullptr)
            return { DX12ReadbackStatusCode::InvalidArgument, "ReadPixels texture is not a valid DX12 texture" };

        D3D12_RESOURCE_DESC srcDesc = srcResource->GetDesc();
        const uint64_t maxX = static_cast<uint64_t>(x) + static_cast<uint64_t>(width);
        const uint64_t maxY = static_cast<uint64_t>(y) + static_cast<uint64_t>(height);
        if (maxX > srcDesc.Width || maxY > static_cast<uint64_t>(srcDesc.Height))
            return { DX12ReadbackStatusCode::InvalidArgument, "ReadPixels region exceeds source texture bounds" };

        const auto readbackLayout = BuildDX12ReadbackLayout(srcDesc.Format, width, height);
        if (readbackLayout.bytesPerPixel == 0u || readbackLayout.readbackSize == 0u)
            return { DX12ReadbackStatusCode::UnsupportedFormat, "ReadPixels could not build a DX12 readback layout" };

        const auto validation = ValidateDX12ReadbackRequest(srcDesc.Format, format, type);
        if (!validation.supported)
        {
            NLS_LOG_WARNING("ExecuteDX12ReadPixels: " + validation.reason);
            return { DX12ReadbackStatusCode::UnsupportedFormat, validation.reason };
        }

        if (m_impl == nullptr)
            m_impl = std::make_shared<Impl>();
        if (m_impl->readbackInFlight != nullptr && m_impl->readbackInFlight->load())
        {
            return {
                DX12ReadbackStatusCode::BackendFailure,
                "ReadPixels previous async readback has not been completed"
            };
        }

        const auto scratchPlan = BuildDX12ReadbackScratchResourcePlan(
            m_impl->readbackCapacity,
            readbackLayout.readbackSize);
        if (scratchPlan.needsNewResource || m_impl->readbackResource == nullptr)
        {
            D3D12_HEAP_PROPERTIES heapProperties{};
            heapProperties.Type = D3D12_HEAP_TYPE_READBACK;
            heapProperties.CreationNodeMask = 0;
            heapProperties.VisibleNodeMask = 0;

            D3D12_RESOURCE_DESC bufferDesc{};
            bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufferDesc.Width = scratchPlan.committedCapacity;
            bufferDesc.Height = 1;
            bufferDesc.DepthOrArraySize = 1;
            bufferDesc.MipLevels = 1;
            bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
            bufferDesc.SampleDesc.Count = 1;
            bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

            HRESULT hr = device->CreateCommittedResource(
                &heapProperties,
                D3D12_HEAP_FLAG_NONE,
                &bufferDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(m_impl->readbackResource.ReleaseAndGetAddressOf()));
            if (FAILED(hr))
                return { DX12ReadbackStatusCode::BackendFailure, "ReadPixels failed to create readback resource" };
            m_impl->readbackCapacity = scratchPlan.committedCapacity;
        }

        HRESULT hr = S_OK;
        if (m_impl->commandAllocator == nullptr)
        {
            hr = device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(m_impl->commandAllocator.GetAddressOf()));
            if (FAILED(hr))
                return { DX12ReadbackStatusCode::BackendFailure, "ReadPixels failed to create command allocator" };
        }
        else
        {
            hr = m_impl->commandAllocator->Reset();
            if (FAILED(hr))
                return { DX12ReadbackStatusCode::BackendFailure, "ReadPixels failed to reset command allocator" };
        }

        if (m_impl->commandList == nullptr)
        {
            hr = device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                m_impl->commandAllocator.Get(),
                nullptr,
                IID_PPV_ARGS(m_impl->commandList.GetAddressOf()));
            if (FAILED(hr))
                return { DX12ReadbackStatusCode::BackendFailure, "ReadPixels failed to create command list" };
        }
        else
        {
            hr = m_impl->commandList->Reset(m_impl->commandAllocator.Get(), nullptr);
            if (FAILED(hr))
                return { DX12ReadbackStatusCode::BackendFailure, "ReadPixels failed to reset command list" };
        }

        const auto sourceBarrierStates = BuildDX12ReadbackBarrierStates(texture->GetState());
        D3D12_RESOURCE_BARRIER toCopySourceBarrier{};
        toCopySourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopySourceBarrier.Transition.pResource = srcResource;
        toCopySourceBarrier.Transition.StateBefore =
            NLS::Render::Backend::NativeDX12CommandBuffer::ToD3D12ResourceState(sourceBarrierStates.beforeCopy);
        toCopySourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toCopySourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        if (toCopySourceBarrier.Transition.StateBefore != toCopySourceBarrier.Transition.StateAfter)
            m_impl->commandList->ResourceBarrier(1, &toCopySourceBarrier);

        D3D12_TEXTURE_COPY_LOCATION srcLocation{};
        srcLocation.pResource = srcResource;
        srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLocation.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION dstLocation{};
        dstLocation.pResource = m_impl->readbackResource.Get();
        dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dstLocation.PlacedFootprint.Offset = 0;
        dstLocation.PlacedFootprint.Footprint.Format = srcDesc.Format;
        dstLocation.PlacedFootprint.Footprint.Width = width;
        dstLocation.PlacedFootprint.Footprint.Height = height;
        dstLocation.PlacedFootprint.Footprint.Depth = 1;
        dstLocation.PlacedFootprint.Footprint.RowPitch = readbackLayout.rowPitch;

        D3D12_BOX sourceBox{};
        sourceBox.left = x;
        sourceBox.top = y;
        sourceBox.front = 0;
        sourceBox.right = x + width;
        sourceBox.bottom = y + height;
        sourceBox.back = 1;

        m_impl->commandList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, &sourceBox);

        D3D12_RESOURCE_BARRIER toCommonBarrier{};
        toCommonBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCommonBarrier.Transition.pResource = srcResource;
        toCommonBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toCommonBarrier.Transition.StateAfter =
            NLS::Render::Backend::NativeDX12CommandBuffer::ToD3D12ResourceState(sourceBarrierStates.afterCopy);
        toCommonBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        if (toCommonBarrier.Transition.StateBefore != toCommonBarrier.Transition.StateAfter)
            m_impl->commandList->ResourceBarrier(1, &toCommonBarrier);

        hr = m_impl->commandList->Close();
        if (FAILED(hr))
            return { DX12ReadbackStatusCode::BackendFailure, "ReadPixels failed to close command list" };

        ID3D12CommandList* commandLists[] = { m_impl->commandList.Get() };
        graphicsQueue->ExecuteCommandLists(1, commandLists);

        if (m_impl->fence == nullptr)
        {
            hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_impl->fence.GetAddressOf()));
            if (FAILED(hr))
                return { DX12ReadbackStatusCode::BackendFailure, "ReadPixels failed to create fence" };
        }

        if (m_impl->fenceEvent == nullptr)
        {
            m_impl->fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (m_impl->fenceEvent == nullptr)
                return { DX12ReadbackStatusCode::BackendFailure, "ReadPixels failed to create fence event" };
        }

        const UINT64 fenceValue = ++m_impl->nextFenceValue;
        hr = graphicsQueue->Signal(m_impl->fence.Get(), fenceValue);
        if (FAILED(hr))
            return { DX12ReadbackStatusCode::BackendFailure, "ReadPixels failed to signal fence" };

        DX12ReadbackPendingCopy pendingCopy;
        pendingCopy.readbackResource = m_impl->readbackResource;
        pendingCopy.fence = m_impl->fence;
        pendingCopy.fenceEvent = m_impl->fenceEvent;
        pendingCopy.inFlightFlag = m_impl->readbackInFlight;
        pendingCopy.fenceValue = fenceValue;
        pendingCopy.layout = readbackLayout;
        pendingCopy.validation = validation;
        pendingCopy.format = format;
        pendingCopy.width = width;
        pendingCopy.height = height;
        pendingCopy.data = data;
        if (m_impl->readbackInFlight != nullptr)
            m_impl->readbackInFlight->store(true);

        return {
            DX12ReadbackStatusCode::Success,
            {},
            std::make_shared<DX12ReadbackCompletionToken>(std::move(pendingCopy))
        };
    }

    DX12ReadbackResult ExecuteDX12ReadPixels(
        ID3D12Device* device,
        ID3D12CommandQueue* graphicsQueue,
        const std::shared_ptr<RHITexture>& texture,
        uint32_t x,
        uint32_t y,
        uint32_t width,
        uint32_t height,
        NLS::Render::Settings::EPixelDataFormat format,
        NLS::Render::Settings::EPixelDataType type,
        void* data)
    {
        DX12ReadbackContext context;
        return context.Execute(
            device,
            graphicsQueue,
            texture,
            x,
            y,
            width,
            height,
            format,
            type,
            data);
    }
#endif
}
