#include "Rendering/RHI/Backends/DX12/DX12ReadbackUtils.h"

#if defined(_WIN32)
#include <cstring>
#include <memory>

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

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
    }

    uint32_t GetDX12ReadbackBytesPerPixel(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            return 4u;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return 8u;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            return 16u;
        default:
            return 0u;
        }
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

    void ExecuteDX12ReadPixels(
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

        if (texture == nullptr || data == nullptr || width == 0 || height == 0 || device == nullptr || graphicsQueue == nullptr)
            return;

        auto imgHandle = texture->GetNativeImageHandle();
        ID3D12Resource* srcResource = (imgHandle.backend == NLS::Render::RHI::BackendType::DX12)
            ? static_cast<ID3D12Resource*>(imgHandle.handle)
            : nullptr;
        if (srcResource == nullptr)
            return;

        D3D12_RESOURCE_DESC srcDesc = srcResource->GetDesc();
        const uint64_t maxX = static_cast<uint64_t>(x) + static_cast<uint64_t>(width);
        const uint64_t maxY = static_cast<uint64_t>(y) + static_cast<uint64_t>(height);
        if (maxX > srcDesc.Width || maxY > static_cast<uint64_t>(srcDesc.Height))
            return;

        const auto readbackLayout = BuildDX12ReadbackLayout(srcDesc.Format, width, height);
        if (readbackLayout.bytesPerPixel == 0u || readbackLayout.readbackSize == 0u)
            return;

        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = D3D12_HEAP_TYPE_READBACK;
        heapProperties.CreationNodeMask = 0;
        heapProperties.VisibleNodeMask = 0;

        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = readbackLayout.readbackSize;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3D12Resource> readbackResource;
        HRESULT hr = device->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(readbackResource.GetAddressOf()));
        if (FAILED(hr))
            return;

        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(commandAllocator.GetAddressOf()));
        if (FAILED(hr))
            return;

        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.Get(), nullptr, IID_PPV_ARGS(commandList.GetAddressOf()));
        if (FAILED(hr))
            return;

        D3D12_RESOURCE_BARRIER toCopySourceBarrier{};
        toCopySourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopySourceBarrier.Transition.pResource = srcResource;
        toCopySourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        toCopySourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toCopySourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &toCopySourceBarrier);

        D3D12_TEXTURE_COPY_LOCATION srcLocation{};
        srcLocation.pResource = srcResource;
        srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLocation.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION dstLocation{};
        dstLocation.pResource = readbackResource.Get();
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

        commandList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, &sourceBox);

        D3D12_RESOURCE_BARRIER toCommonBarrier{};
        toCommonBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCommonBarrier.Transition.pResource = srcResource;
        toCommonBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toCommonBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        toCommonBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &toCommonBarrier);

        hr = commandList->Close();
        if (FAILED(hr))
            return;

        ID3D12CommandList* commandLists[] = { commandList.Get() };
        graphicsQueue->ExecuteCommandLists(1, commandLists);

        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()));
        if (FAILED(hr))
            return;

        HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (fenceEvent == nullptr)
            return;

        const UINT64 fenceValue = 1u;
        hr = graphicsQueue->Signal(fence.Get(), fenceValue);
        if (FAILED(hr))
        {
            CloseHandle(fenceEvent);
            return;
        }

        hr = fence->SetEventOnCompletion(fenceValue, fenceEvent);
        if (FAILED(hr))
        {
            CloseHandle(fenceEvent);
            return;
        }

        WaitForSingleObject(fenceEvent, INFINITE);
        CloseHandle(fenceEvent);

        void* mappedData = nullptr;
        D3D12_RANGE readRange{};
        readRange.Begin = 0;
        readRange.End = static_cast<SIZE_T>(readbackLayout.readbackSize);
        hr = readbackResource->Map(0, &readRange, &mappedData);
        if (FAILED(hr) || mappedData == nullptr)
            return;

        const auto* srcBytes = static_cast<const uint8_t*>(mappedData);
        auto* dstBytes = static_cast<uint8_t*>(data);
        const size_t sourceRowPitch = static_cast<size_t>(readbackLayout.rowPitch);
        const size_t packedRowSize = static_cast<size_t>(width) * static_cast<size_t>(readbackLayout.bytesPerPixel);

        if (format == NLS::Render::Settings::EPixelDataFormat::RGB && readbackLayout.bytesPerPixel >= 3u)
        {
            for (uint32_t row = 0; row < height; ++row)
            {
                for (uint32_t col = 0; col < width; ++col)
                {
                    const size_t srcIdx = row * sourceRowPitch + col * static_cast<size_t>(readbackLayout.bytesPerPixel);
                    const size_t dstIdx = (static_cast<size_t>(row) * width + col) * 3u;
                    dstBytes[dstIdx + 0] = srcBytes[srcIdx + 0];
                    dstBytes[dstIdx + 1] = srcBytes[srcIdx + 1];
                    dstBytes[dstIdx + 2] = srcBytes[srcIdx + 2];
                }
            }
        }
        else
        {
            for (uint32_t row = 0; row < height; ++row)
            {
                std::memcpy(
                    dstBytes + row * packedRowSize,
                    srcBytes + row * sourceRowPitch,
                    packedRowSize);
            }
        }

        D3D12_RANGE writeRange{};
        writeRange.Begin = 0;
        writeRange.End = 0;
        readbackResource->Unmap(0, &writeRange);
    }
#endif
}
