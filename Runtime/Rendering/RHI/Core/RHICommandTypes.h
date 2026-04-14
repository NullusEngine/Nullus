#pragma once

// Command types for unified command list abstraction
// This header contains the Command base class and all concrete command types
// used by RHICommandList for recording and by executors for playback.

#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/RHI/Core/RHISync.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include <vector>
#include <cstdint>
#include <memory>

namespace NLS::Render::RHI
{
    // Forward declarations
    class RHIGraphicsPipeline;
    class RHIComputePipeline;
    class RHIBindingSet;
    class IRHIResource;

    // ==========================
    // Command argument structures
    // ==========================

    struct DrawArguments
    {
        uint32_t vertexCount = 0;
        uint32_t instanceCount = 1;
        uint32_t firstVertex = 0;
        uint32_t firstInstance = 0;
    };

    struct DrawIndexedArguments
    {
        uint32_t indexCount = 0;
        uint32_t instanceCount = 1;
        uint32_t firstIndex = 0;
        int32_t vertexOffset = 0;
        uint32_t firstInstance = 0;
    };

    struct DrawInstancedArguments
    {
        uint32_t vertexCount = 0;
        uint32_t instanceCount = 1;
        uint32_t firstVertex = 0;
        uint32_t firstInstance = 0;
    };

    struct DrawIndexedInstancedArguments
    {
        uint32_t indexCount = 0;
        uint32_t instanceCount = 1;
        uint32_t firstIndex = 0;
        int32_t vertexOffset = 0;
        uint32_t firstInstance = 0;
    };

    struct DispatchArguments
    {
        uint32_t threadGroupX = 0;
        uint32_t threadGroupY = 0;
        uint32_t threadGroupZ = 0;
    };

    struct CopyBufferArguments
    {
        std::shared_ptr<RHIBuffer> source;
        std::shared_ptr<RHIBuffer> destination;
        RHIBufferCopyRegion region;
    };

    struct CopyBufferToTextureDesc
    {
        std::shared_ptr<RHIBuffer> source;
        std::shared_ptr<RHITexture> destination;
        uint64_t bufferOffset = 0;
        uint32_t mipLevel = 0;
        uint32_t arrayLayer = 0;
        RHIOffset3D textureOffset{};
        RHIExtent3D extent{};
    };

    struct CopyTextureDesc
    {
        std::shared_ptr<RHITexture> source;
        std::shared_ptr<RHITexture> destination;
        RHISubresourceRange sourceRange{};
        RHISubresourceRange destinationRange{};
        RHIOffset3D sourceOffset{};
        RHIOffset3D destinationOffset{};
        RHIExtent3D extent{};
    };

    struct CopyBufferToTextureArguments
    {
        CopyBufferToTextureDesc desc;
    };

    struct CopyTextureArguments
    {
        CopyTextureDesc desc;
    };

    struct Viewport
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float minDepth = 0.0f;
        float maxDepth = 1.0f;
    };

    struct Rect2D
    {
        int32_t x = 0;
        int32_t y = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct RenderPassColorAttachmentDesc
    {
        std::shared_ptr<class RHITextureView> view;
        LoadOp loadOp = LoadOp::Clear;
        StoreOp storeOp = StoreOp::Store;
        float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    };

    struct RenderPassDepthStencilAttachmentDesc
    {
        std::shared_ptr<class RHITextureView> view;
        LoadOp depthLoadOp = LoadOp::Clear;
        StoreOp depthStoreOp = StoreOp::Store;
        LoadOp stencilLoadOp = LoadOp::DontCare;
        StoreOp stencilStoreOp = StoreOp::DontCare;
        float clearDepth = 1.0f;
        uint32_t clearStencil = 0;
    };

    struct RenderPassDesc
    {
        std::vector<RenderPassColorAttachmentDesc> colorAttachments;
        std::optional<RenderPassDepthStencilAttachmentDesc> depthStencilAttachment;
        Rect2D renderArea{};
        std::string debugName;
    };

    struct BufferBarrier
    {
        std::shared_ptr<RHIBuffer> buffer;
        ResourceState before = ResourceState::Unknown;
        ResourceState after = ResourceState::Unknown;
    };

    struct TextureBarrier
    {
        std::shared_ptr<RHITexture> texture;
        ResourceState before = ResourceState::Unknown;
        ResourceState after = ResourceState::Unknown;
        RHISubresourceRange subresourceRange{};
    };

    struct BarrierDesc
    {
        enum class Type : uint8_t
        {
            Transition,
            UAV,
            Alias
        };

        Type type = Type::Transition;
        std::vector<BufferBarrier> bufferBarriers;
        std::vector<TextureBarrier> textureBarriers;
    };

    // ==========================
    // Command base class
    // ==========================

    class NLS_RENDER_API Command
    {
    public:
        virtual ~Command() = default;
        enum class Type : uint8_t
        {
            BeginRenderPass,
            EndRenderPass,
            SetViewport,
            SetScissor,
            BindGraphicsPipeline,
            BindComputePipeline,
            BindBindingSet,
            PushConstants,
            BindVertexBuffer,
            BindIndexBuffer,
            Draw,
            DrawIndexed,
            DrawInstanced,
            DrawIndexedInstanced,
            Dispatch,
            DispatchIndirect,
            SetStencilRef,
            SetBlendFactor,
            Barrier,
            UAVBarrier,
            AliasBarrier,
            CopyBuffer,
            CopyBufferToTexture,
            CopyTexture
        };

        explicit Command(Type type) : type(type) {}
        Type type;
    };

    // ==========================
    // Concrete command types
    // ==========================

    struct BeginRenderPassCmd : Command
    {
        BeginRenderPassCmd();
        RenderPassDesc desc;
    };

    struct EndRenderPassCmd : Command
    {
        EndRenderPassCmd();
    };

    struct SetViewportCmd : Command
    {
        SetViewportCmd();
        Viewport viewport;
    };

    struct SetScissorCmd : Command
    {
        SetScissorCmd();
        Rect2D rect;
    };

    struct BindGraphicsPipelineCmd : Command
    {
        BindGraphicsPipelineCmd();
        std::shared_ptr<RHIGraphicsPipeline> pipeline;
    };

    struct BindComputePipelineCmd : Command
    {
        BindComputePipelineCmd();
        std::shared_ptr<RHIComputePipeline> pipeline;
    };

    struct BindBindingSetCmd : Command
    {
        BindBindingSetCmd();
        uint32_t setIndex = 0;
        std::shared_ptr<RHIBindingSet> bindingSet;
    };

    struct PushConstantsCmd : Command
    {
        PushConstantsCmd();
        ShaderStageMask stageMask = ShaderStageMask::None;
        uint32_t offset = 0;
        uint32_t size = 0;
        std::vector<uint8_t> data;
    };

    struct BindVertexBufferCmd : Command
    {
        BindVertexBufferCmd();
        uint32_t slot = 0;
        RHIVertexBufferView view;
    };

    struct BindIndexBufferCmd : Command
    {
        BindIndexBufferCmd();
        RHIIndexBufferView view;
    };

    struct DrawCmd : Command
    {
        DrawCmd();
        DrawArguments args;
    };

    struct DrawIndexedCmd : Command
    {
        DrawIndexedCmd();
        DrawIndexedArguments args;
    };

    struct DrawInstancedCmd : Command
    {
        DrawInstancedCmd();
        DrawInstancedArguments args;
    };

    struct DrawIndexedInstancedCmd : Command
    {
        DrawIndexedInstancedCmd();
        DrawIndexedInstancedArguments args;
    };

    struct DispatchCmd : Command
    {
        DispatchCmd();
        DispatchArguments args;
    };

    struct DispatchIndirectCmd : Command
    {
        DispatchIndirectCmd();
        RHIBuffer* indirectBuffer = nullptr;
        uint64_t offset = 0;
    };

    struct SetStencilRefCmd : Command
    {
        SetStencilRefCmd();
        uint32_t stencilRef = 0;
    };

    struct SetBlendFactorCmd : Command
    {
        SetBlendFactorCmd();
        float blendFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    };

    struct UAVBarrierCmd : Command
    {
        UAVBarrierCmd();
        IRHIResource* resource = nullptr;
    };

    struct AliasBarrierCmd : Command
    {
        AliasBarrierCmd();
        IRHIResource* before = nullptr;
        IRHIResource* after = nullptr;
    };

    struct CopyBufferCmd : Command
    {
        CopyBufferCmd();
        CopyBufferArguments args;
    };

    struct CopyBufferToTextureCmd : Command
    {
        CopyBufferToTextureCmd();
        CopyBufferToTextureArguments args;
    };

    struct CopyTextureCmd : Command
    {
        CopyTextureCmd();
        CopyTextureArguments args;
    };

    struct BarrierCmd : Command
    {
        BarrierCmd();
        BarrierDesc barrier;
    };
} // namespace NLS::Render::RHI