#include "Rendering/RHI/Backends/Metal/MetalExplicitDeviceFactory.h"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <spirv_msl.hpp>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#include <array>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Debug/Logger.h"
#include "Rendering/RHI/BindingPointMap.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHISubresourceRangeUtils.h"
#include "Rendering/RHI/RHITypes.h"

namespace NLS::Render::Backend
{
    namespace
    {
        constexpr NLS::Render::RHI::BackendType kMetalBackendType = NLS::Render::RHI::BackendType::Metal;
        constexpr uint32_t kMetalPushConstantBufferIndex = 30u;
        constexpr uint32_t kMetalPassStorageBufferBase = 23u;
        constexpr uint32_t kMetalSpirvStorageBindingOffset = 64u;
        constexpr uint32_t kMetalSpirvTextureBindingOffset = 128u;
        constexpr uint32_t kMetalSpirvSamplerBindingOffset = 192u;

        uint32_t ResolveMetalSwapchainImageCount(const uint32_t requestedImageCount)
        {
            constexpr uint32_t kDefaultImageCount = 3u;
            constexpr uint32_t kMinimumDrawableCount = 2u;
            constexpr uint32_t kMaximumDrawableCount = 3u;
            const uint32_t resolvedImageCount = requestedImageCount == 0u
                ? kDefaultImageCount
                : requestedImageCount;
            return std::clamp(
                resolvedImageCount,
                kMinimumDrawableCount,
                kMaximumDrawableCount);
        }

        std::optional<uint32_t> ToMetalBufferBindingIndex(const uint32_t bindingSpace, const uint32_t binding)
        {
            const uint32_t index = NLS::Render::RHI::BindingPointMap::GetUniformBufferBindingPoint(bindingSpace, binding);
            return index < kMetalPushConstantBufferIndex ? std::optional<uint32_t>(index) : std::nullopt;
        }

        std::optional<uint32_t> ToMetalStorageBufferBindingIndex(const uint32_t bindingSpace, const uint32_t binding)
        {
            // Pass parameters intentionally use both b0 and t0 in the same register space.
            // Keep storage buffers in a separate Metal buffer range from uniform buffers.
            if (bindingSpace == NLS::Render::RHI::BindingPointMap::kPassBindingSpace)
            {
                const uint32_t index = kMetalPassStorageBufferBase + binding;
                return index < kMetalPushConstantBufferIndex ? std::optional<uint32_t>(index) : std::nullopt;
            }

            return ToMetalBufferBindingIndex(bindingSpace, binding);
        }

        std::optional<uint32_t> ToMetalTextureBindingIndex(const uint32_t bindingSpace, const uint32_t binding)
        {
            const uint32_t index = NLS::Render::RHI::BindingPointMap::GetTextureBindingPoint(bindingSpace, binding);
            return index < 128u ? std::optional<uint32_t>(index) : std::nullopt;
        }

        std::optional<uint32_t> ToMetalSamplerBindingIndex(const uint32_t bindingSpace, const uint32_t binding)
        {
            const uint32_t index = bindingSpace * NLS::Render::RHI::BindingPointMap::kUniformBufferSpaceStride + binding;
            return index < 16u ? std::optional<uint32_t>(index) : std::nullopt;
        }

        std::optional<uint32_t> ToMetalBindingIndex(
            const NLS::Render::RHI::BindingType type,
            const uint32_t bindingSpace,
            const uint32_t binding)
        {
            using NLS::Render::RHI::BindingType;
            switch (type)
            {
            case BindingType::UniformBuffer:
                return ToMetalBufferBindingIndex(bindingSpace, binding);
            case BindingType::StructuredBuffer:
            case BindingType::StorageBuffer:
                return ToMetalStorageBufferBindingIndex(bindingSpace, binding);
            case BindingType::Texture:
            case BindingType::RWTexture:
                return ToMetalTextureBindingIndex(bindingSpace, binding);
            case BindingType::Sampler:
                return ToMetalSamplerBindingIndex(bindingSpace, binding);
            }
            return std::nullopt;
        }

        bool ValidateMetalBindingIndexSpan(
            const NLS::Render::RHI::RHIBindingLayoutEntry& entry,
            std::string& reason)
        {
            if (entry.count == 0u)
            {
                reason = "descriptor count must be non-zero";
                return false;
            }
            if (entry.stageMask == NLS::Render::RHI::ShaderStageMask::None)
            {
                reason = "shader stage mask must be non-empty";
                return false;
            }

            for (uint32_t descriptorIndex = 0u; descriptorIndex < entry.count; ++descriptorIndex)
            {
                const uint64_t binding = static_cast<uint64_t>(entry.binding) + descriptorIndex;
                if (binding > (std::numeric_limits<uint32_t>::max)() ||
                    !ToMetalBindingIndex(entry.type, entry.registerSpace, static_cast<uint32_t>(binding)).has_value())
                {
                    reason = "binding range exceeds Metal's supported resource indices";
                    return false;
                }
            }
            return true;
        }

        bool AreMetalBindingLayoutEntriesCompatible(
            const NLS::Render::RHI::RHIBindingLayoutEntry& expected,
            const NLS::Render::RHI::RHIBindingLayoutEntry& actual)
        {
            return expected.type == actual.type &&
                expected.set == actual.set &&
                expected.binding == actual.binding &&
                expected.count == actual.count &&
                expected.registerSpace == actual.registerSpace &&
                expected.elementStride == actual.elementStride;
        }

        uint32_t GetMetalBindingNamespace(const NLS::Render::RHI::BindingType type)
        {
            using NLS::Render::RHI::BindingType;
            switch (type)
            {
            case BindingType::UniformBuffer:
            case BindingType::StructuredBuffer:
            case BindingType::StorageBuffer:
                return 0u;
            case BindingType::Texture:
            case BindingType::RWTexture:
                return 1u;
            case BindingType::Sampler:
                return 2u;
            }
            return 3u;
        }

        NLS::Render::RHI::ResourceState ResolveUploadedMetalTextureState(
            const NLS::Render::RHI::RHITextureDesc& desc)
        {
            using NLS::Render::RHI::ResourceState;
            using NLS::Render::RHI::TextureUsageFlags;
            if (NLS::Render::RHI::HasTextureUsage(desc.usage, TextureUsageFlags::Sampled))
                return ResourceState::ShaderRead;
            if (NLS::Render::RHI::HasTextureUsage(desc.usage, TextureUsageFlags::Storage))
                return ResourceState::ShaderWrite;
            if (NLS::Render::RHI::HasTextureUsage(desc.usage, TextureUsageFlags::ColorAttachment))
                return ResourceState::RenderTarget;
            if (NLS::Render::RHI::HasTextureUsage(desc.usage, TextureUsageFlags::DepthStencilAttachment))
                return ResourceState::DepthWrite;
            if (NLS::Render::RHI::HasTextureUsage(desc.usage, TextureUsageFlags::CopySrc))
                return ResourceState::CopySrc;
            if (NLS::Render::RHI::HasTextureUsage(desc.usage, TextureUsageFlags::CopyDst))
                return ResourceState::CopyDst;
            if (NLS::Render::RHI::HasTextureUsage(desc.usage, TextureUsageFlags::Present))
                return ResourceState::Present;
            return ResourceState::Unknown;
        }

        bool IsLegalMetalCpuVisibleBufferState(
            const NLS::Render::RHI::RHIBuffer& buffer,
            const NLS::Render::RHI::ResourceState state)
        {
            using namespace NLS::Render::RHI;
            if (buffer.GetDesc().memoryUsage == MemoryUsage::CPUToGPU)
                return state == ResourceState::Unknown || state == ResourceState::GenericRead;
            if (buffer.GetDesc().memoryUsage == MemoryUsage::GPUToCPU)
                return state == ResourceState::Unknown || state == ResourceState::CopyDst;
            return true;
        }

        MTLLoadAction ToMetalLoadAction(const NLS::Render::RHI::LoadOp loadOp)
        {
            using namespace NLS::Render::RHI;
            switch (loadOp)
            {
            case LoadOp::Load: return MTLLoadActionLoad;
            case LoadOp::DontCare: return MTLLoadActionDontCare;
            case LoadOp::Clear:
            default: return MTLLoadActionClear;
            }
        }

        MTLStoreAction ToMetalStoreAction(const NLS::Render::RHI::StoreOp storeOp)
        {
            return storeOp == NLS::Render::RHI::StoreOp::Store
                ? MTLStoreActionStore
                : MTLStoreActionDontCare;
        }

        MTLPrimitiveType ToMetalPrimitiveType(const NLS::Render::RHI::PrimitiveTopology topology)
        {
            using namespace NLS::Render::RHI;
            switch (topology)
            {
            case PrimitiveTopology::PointList: return MTLPrimitiveTypePoint;
            case PrimitiveTopology::LineList: return MTLPrimitiveTypeLine;
            case PrimitiveTopology::TriangleList:
            default: return MTLPrimitiveTypeTriangle;
            }
        }

        MTLVertexFormat ToMetalVertexFormat(const uint32_t elementSize)
        {
            switch (elementSize)
            {
            case 4u: return MTLVertexFormatFloat;
            case 8u: return MTLVertexFormatFloat2;
            case 12u: return MTLVertexFormatFloat3;
            case 16u:
            default: return MTLVertexFormatFloat4;
            }
        }

        MTLBlendFactor ToMetalBlendFactor(const NLS::Render::RHI::RHIBlendFactor factor)
        {
            using namespace NLS::Render::RHI;
            switch (factor)
            {
            case RHIBlendFactor::Zero: return MTLBlendFactorZero;
            case RHIBlendFactor::One: return MTLBlendFactorOne;
            case RHIBlendFactor::SrcColor: return MTLBlendFactorSourceColor;
            case RHIBlendFactor::InvSrcColor: return MTLBlendFactorOneMinusSourceColor;
            case RHIBlendFactor::SrcAlpha: return MTLBlendFactorSourceAlpha;
            case RHIBlendFactor::InvSrcAlpha: return MTLBlendFactorOneMinusSourceAlpha;
            case RHIBlendFactor::DstAlpha: return MTLBlendFactorDestinationAlpha;
            case RHIBlendFactor::InvDstAlpha: return MTLBlendFactorOneMinusDestinationAlpha;
            case RHIBlendFactor::DstColor: return MTLBlendFactorDestinationColor;
            case RHIBlendFactor::InvDstColor: return MTLBlendFactorOneMinusDestinationColor;
            default: return MTLBlendFactorOne;
            }
        }

        MTLBlendOperation ToMetalBlendOperation(const NLS::Render::RHI::RHIBlendOp operation)
        {
            using namespace NLS::Render::RHI;
            switch (operation)
            {
            case RHIBlendOp::Subtract: return MTLBlendOperationSubtract;
            case RHIBlendOp::ReverseSubtract: return MTLBlendOperationReverseSubtract;
            case RHIBlendOp::Min: return MTLBlendOperationMin;
            case RHIBlendOp::Max: return MTLBlendOperationMax;
            case RHIBlendOp::Add:
            default: return MTLBlendOperationAdd;
            }
        }

        MTLColorWriteMask ToMetalColorWriteMask(const NLS::Render::RHI::RHIColorWriteMask mask)
        {
            using namespace NLS::Render::RHI;
            MTLColorWriteMask result = MTLColorWriteMaskNone;
            if (HasColorWriteMask(mask, RHIColorWriteMask::Red)) result |= MTLColorWriteMaskRed;
            if (HasColorWriteMask(mask, RHIColorWriteMask::Green)) result |= MTLColorWriteMaskGreen;
            if (HasColorWriteMask(mask, RHIColorWriteMask::Blue)) result |= MTLColorWriteMaskBlue;
            if (HasColorWriteMask(mask, RHIColorWriteMask::Alpha)) result |= MTLColorWriteMaskAlpha;
            return result;
        }

        MTLCompareFunction ToMetalCompareFunction(const NLS::Render::Settings::EComparaisonAlgorithm algorithm)
        {
            using NLS::Render::Settings::EComparaisonAlgorithm;
            switch (algorithm)
            {
            case EComparaisonAlgorithm::NEVER: return MTLCompareFunctionNever;
            case EComparaisonAlgorithm::LESS: return MTLCompareFunctionLess;
            case EComparaisonAlgorithm::EQUAL: return MTLCompareFunctionEqual;
            case EComparaisonAlgorithm::LESS_EQUAL: return MTLCompareFunctionLessEqual;
            case EComparaisonAlgorithm::GREATER: return MTLCompareFunctionGreater;
            case EComparaisonAlgorithm::NOTEQUAL: return MTLCompareFunctionNotEqual;
            case EComparaisonAlgorithm::GREATER_EQUAL: return MTLCompareFunctionGreaterEqual;
            case EComparaisonAlgorithm::ALWAYS:
            default: return MTLCompareFunctionAlways;
            }
        }

        MTLStencilOperation ToMetalStencilOperation(const NLS::Render::Settings::EOperation operation)
        {
            using NLS::Render::Settings::EOperation;
            switch (operation)
            {
            case EOperation::ZERO: return MTLStencilOperationZero;
            case EOperation::KEEP: return MTLStencilOperationKeep;
            case EOperation::REPLACE: return MTLStencilOperationReplace;
            case EOperation::INCREMENT: return MTLStencilOperationIncrementClamp;
            case EOperation::INCREMENT_WRAP: return MTLStencilOperationIncrementWrap;
            case EOperation::DECREMENT: return MTLStencilOperationDecrementClamp;
            case EOperation::DECREMENT_WRAP: return MTLStencilOperationDecrementWrap;
            case EOperation::INVERT: return MTLStencilOperationInvert;
            default: return MTLStencilOperationKeep;
            }
        }

        MTLSamplerMinMagFilter ToMetalSamplerFilter(const NLS::Render::RHI::TextureFilter filter)
        {
            return filter == NLS::Render::RHI::TextureFilter::Nearest
                ? MTLSamplerMinMagFilterNearest
                : MTLSamplerMinMagFilterLinear;
        }

        MTLSamplerMipFilter ToMetalSamplerMipFilter(const NLS::Render::RHI::TextureMipFilter filter)
        {
            return filter == NLS::Render::RHI::TextureMipFilter::Nearest
                ? MTLSamplerMipFilterNearest
                : MTLSamplerMipFilterLinear;
        }

        MTLSamplerAddressMode ToMetalSamplerAddressMode(const NLS::Render::RHI::TextureWrap wrap)
        {
            using NLS::Render::RHI::TextureWrap;
            switch (wrap)
            {
            case TextureWrap::ClampToEdge: return MTLSamplerAddressModeClampToEdge;
            case TextureWrap::MirrorRepeat: return MTLSamplerAddressModeMirrorRepeat;
            case TextureWrap::ClampToBorder: return MTLSamplerAddressModeClampToBorderColor;
            case TextureWrap::Repeat:
            default: return MTLSamplerAddressModeRepeat;
            }
        }

        bool UsesMetalBorderColor(const NLS::Render::RHI::SamplerDesc& desc)
        {
            using NLS::Render::RHI::TextureWrap;
            return desc.wrapU == TextureWrap::ClampToBorder ||
                desc.wrapV == TextureWrap::ClampToBorder ||
                desc.wrapW == TextureWrap::ClampToBorder;
        }

        std::optional<MTLSamplerBorderColor> ToMetalSamplerBorderColor(
            const std::array<float, 4>& color)
        {
            if (color == std::array<float, 4> { 0.0f, 0.0f, 0.0f, 0.0f })
                return MTLSamplerBorderColorTransparentBlack;
            if (color == std::array<float, 4> { 0.0f, 0.0f, 0.0f, 1.0f })
                return MTLSamplerBorderColorOpaqueBlack;
            if (color == std::array<float, 4> { 1.0f, 1.0f, 1.0f, 1.0f })
                return MTLSamplerBorderColorOpaqueWhite;
            return std::nullopt;
        }

        MTLPixelFormat ToMetalPixelFormat(
            const NLS::Render::RHI::TextureFormat format,
            const NLS::Render::RHI::TextureColorSpace colorSpace)
        {
            using namespace NLS::Render::RHI;
            switch (format)
            {
            case TextureFormat::R8: return MTLPixelFormatR8Unorm;
            case TextureFormat::RG8: return MTLPixelFormatRG8Unorm;
            case TextureFormat::RGB8:
            case TextureFormat::RGBA8:
                return colorSpace == TextureColorSpace::SRGB
                    ? MTLPixelFormatRGBA8Unorm_sRGB
                    : MTLPixelFormatRGBA8Unorm;
            case TextureFormat::R16F: return MTLPixelFormatR16Float;
            case TextureFormat::RG16F: return MTLPixelFormatRG16Float;
            case TextureFormat::RGBA16F: return MTLPixelFormatRGBA16Float;
            case TextureFormat::R32F: return MTLPixelFormatR32Float;
            case TextureFormat::RG32F: return MTLPixelFormatRG32Float;
            case TextureFormat::RGBA32F: return MTLPixelFormatRGBA32Float;
            case TextureFormat::BC1:
                return colorSpace == TextureColorSpace::SRGB
                    ? MTLPixelFormatBC1_RGBA_sRGB
                    : MTLPixelFormatBC1_RGBA;
            case TextureFormat::BC3:
                return colorSpace == TextureColorSpace::SRGB
                    ? MTLPixelFormatBC3_RGBA_sRGB
                    : MTLPixelFormatBC3_RGBA;
            case TextureFormat::BC5:
                return colorSpace == TextureColorSpace::SRGB
                    ? MTLPixelFormatInvalid
                    : MTLPixelFormatBC5_RGUnorm;
            case TextureFormat::BC7:
                return colorSpace == TextureColorSpace::SRGB
                    ? MTLPixelFormatBC7_RGBAUnorm_sRGB
                    : MTLPixelFormatBC7_RGBAUnorm;
            case TextureFormat::BC6H:
                return colorSpace == TextureColorSpace::SRGB
                    ? MTLPixelFormatInvalid
                    : MTLPixelFormatBC6H_RGBUfloat;
            case TextureFormat::ASTC4x4:
                return colorSpace == TextureColorSpace::SRGB
                    ? MTLPixelFormatASTC_4x4_sRGB
                    : MTLPixelFormatASTC_4x4_LDR;
            case TextureFormat::ETC2RGBA8:
                return colorSpace == TextureColorSpace::SRGB
                    ? MTLPixelFormatEAC_RGBA8_sRGB
                    : MTLPixelFormatEAC_RGBA8;
            case TextureFormat::Depth32F: return MTLPixelFormatDepth32Float;
            // Metal exposes Depth24Unorm_Stencil8 on macOS as the invalid sentinel
            // value 255. Use the supported 32-bit depth/stencil attachment instead.
            case TextureFormat::Depth24Stencil8: return MTLPixelFormatDepth32Float_Stencil8;
            case TextureFormat::Count:
            default:
                return MTLPixelFormatInvalid;
            }
        }

        bool ExpandMetalRgb8Upload(
            const void* source,
            const size_t sourceSize,
            const uint32_t width,
            const uint32_t height,
            const uint32_t depth,
            const size_t sourceRowPitch,
            const size_t sourceSlicePitch,
            std::vector<uint8_t>& expanded)
        {
            if (source == nullptr || width == 0u || height == 0u || depth == 0u)
                return false;

            const size_t packedRowBytes = static_cast<size_t>(width) * 3u;
            const size_t nativeRowBytes = static_cast<size_t>(width) * 4u;
            const size_t resolvedSourceRowPitch = sourceRowPitch != 0u
                ? sourceRowPitch
                : packedRowBytes;
            if (resolvedSourceRowPitch < packedRowBytes ||
                height > (std::numeric_limits<size_t>::max)() / resolvedSourceRowPitch)
            {
                return false;
            }
            const size_t minimumSourceSlicePitch = resolvedSourceRowPitch * static_cast<size_t>(height);
            const size_t resolvedSourceSlicePitch = sourceSlicePitch != 0u
                ? sourceSlicePitch
                : minimumSourceSlicePitch;
            if (resolvedSourceSlicePitch < minimumSourceSlicePitch ||
                depth > (std::numeric_limits<size_t>::max)() / resolvedSourceSlicePitch ||
                height > (std::numeric_limits<size_t>::max)() / nativeRowBytes)
            {
                return false;
            }

            const size_t requiredSourceBytes =
                resolvedSourceSlicePitch * static_cast<size_t>(depth - 1u) +
                resolvedSourceRowPitch * static_cast<size_t>(height - 1u) +
                packedRowBytes;
            const size_t nativeSlicePitch = nativeRowBytes * static_cast<size_t>(height);
            if (sourceSize < requiredSourceBytes ||
                depth > (std::numeric_limits<size_t>::max)() / nativeSlicePitch)
            {
                return false;
            }

            expanded.resize(nativeSlicePitch * static_cast<size_t>(depth));
            const auto* sourceBytes = static_cast<const uint8_t*>(source);
            for (uint32_t z = 0u; z < depth; ++z)
            {
                for (uint32_t y = 0u; y < height; ++y)
                {
                    const auto* sourceRow = sourceBytes +
                        static_cast<size_t>(z) * resolvedSourceSlicePitch +
                        static_cast<size_t>(y) * resolvedSourceRowPitch;
                    auto* destinationRow = expanded.data() +
                        static_cast<size_t>(z) * nativeSlicePitch +
                        static_cast<size_t>(y) * nativeRowBytes;
                    for (uint32_t x = 0u; x < width; ++x)
                    {
                        destinationRow[x * 4u + 0u] = sourceRow[x * 3u + 0u];
                        destinationRow[x * 4u + 1u] = sourceRow[x * 3u + 1u];
                        destinationRow[x * 4u + 2u] = sourceRow[x * 3u + 2u];
                        destinationRow[x * 4u + 3u] = 255u;
                    }
                }
            }
            return true;
        }

        MTLTextureUsage ToMetalTextureUsage(const NLS::Render::RHI::TextureUsageFlags usage)
        {
            using namespace NLS::Render::RHI;
            MTLTextureUsage result = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
            if (HasTextureUsage(usage, TextureUsageFlags::Storage))
                result |= MTLTextureUsageShaderWrite;
            if (HasTextureUsage(usage, TextureUsageFlags::ColorAttachment) ||
                HasTextureUsage(usage, TextureUsageFlags::DepthStencilAttachment) ||
                HasTextureUsage(usage, TextureUsageFlags::Present))
            {
                result |= MTLTextureUsageRenderTarget;
            }
            return result;
        }

        MTLTextureDescriptor* CreateMetalTextureDescriptor(
            const NLS::Render::RHI::RHITextureDesc& desc,
            const MTLTextureType textureType,
            const MTLPixelFormat pixelFormat,
            const MTLStorageMode storageMode)
        {
            using namespace NLS::Render::RHI;
            const uint32_t layerCount = GetTextureLayerCount(desc.dimension, desc.arrayLayers);
            MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
            descriptor.textureType = textureType;
            descriptor.pixelFormat = pixelFormat;
            descriptor.width = desc.extent.width;
            descriptor.height = desc.dimension == TextureDimension::Texture1D
                ? 1u
                : desc.extent.height;
            descriptor.depth = desc.dimension == TextureDimension::Texture3D
                ? desc.extent.depth
                : 1u;
            descriptor.mipmapLevelCount = desc.mipLevels;
            descriptor.sampleCount = desc.sampleCount;
            descriptor.arrayLength = desc.dimension == TextureDimension::TextureCubeArray
                ? layerCount / 6u
                : (desc.dimension == TextureDimension::Texture2DArray ? layerCount : 1u);
            descriptor.usage = ToMetalTextureUsage(desc.usage);
            descriptor.storageMode = storageMode;
            return descriptor;
        }

        uint32_t GetMipDimension(const uint32_t value, const uint32_t mipLevel)
        {
            return mipLevel < 32u
                ? (std::max)(1u, value >> mipLevel)
                : 1u;
        }

        std::optional<MTLTextureType> ToMetalTextureType(const NLS::Render::RHI::RHITextureDesc& desc)
        {
            using NLS::Render::RHI::TextureDimension;
            switch (desc.dimension)
            {
            case TextureDimension::Texture1D:
                return MTLTextureType1D;
            case TextureDimension::Texture2D:
                return desc.sampleCount > 1u
                    ? MTLTextureType2DMultisample
                    : MTLTextureType2D;
            case TextureDimension::Texture2DArray:
                return desc.sampleCount > 1u
                    ? MTLTextureType2DMultisampleArray
                    : MTLTextureType2DArray;
            case TextureDimension::TextureCube:
                return MTLTextureTypeCube;
            case TextureDimension::TextureCubeArray:
                return MTLTextureTypeCubeArray;
            case TextureDimension::Texture3D:
                return MTLTextureType3D;
            default:
                return std::nullopt;
            }
        }

        std::optional<MTLTextureType> ToMetalTextureViewType(
            const NLS::Render::RHI::RHITextureDesc& textureDesc,
            const NLS::Render::RHI::TextureViewType requestedViewType)
        {
            using namespace NLS::Render::RHI;
            TextureViewType viewType = requestedViewType;
            if (viewType == TextureViewType::Auto)
            {
                switch (textureDesc.dimension)
                {
                case TextureDimension::Texture2DArray: viewType = TextureViewType::Texture2DArray; break;
                case TextureDimension::TextureCube: viewType = TextureViewType::Cube; break;
                case TextureDimension::TextureCubeArray: viewType = TextureViewType::CubeArray; break;
                case TextureDimension::Texture3D: viewType = TextureViewType::Texture3D; break;
                case TextureDimension::Texture1D:
                case TextureDimension::Texture2D:
                default: viewType = TextureViewType::Texture2D; break;
                }
            }

            switch (viewType)
            {
            case TextureViewType::Texture2D:
                if (textureDesc.dimension == TextureDimension::Texture1D)
                    return MTLTextureType1D;
                return textureDesc.sampleCount > 1u
                    ? MTLTextureType2DMultisample
                    : MTLTextureType2D;
            case TextureViewType::Texture2DArray:
                return textureDesc.sampleCount > 1u
                    ? MTLTextureType2DMultisampleArray
                    : MTLTextureType2DArray;
            case TextureViewType::Texture3D:
                return MTLTextureType3D;
            case TextureViewType::Cube:
                return MTLTextureTypeCube;
            case TextureViewType::CubeArray:
                return MTLTextureTypeCubeArray;
            case TextureViewType::Auto:
            default:
                return std::nullopt;
            }
        }

        bool ValidateMetalTextureDesc(
            const NLS::Render::RHI::RHITextureDesc& desc,
            std::string& reason)
        {
            using namespace NLS::Render::RHI;
            if (desc.extent.width == 0u || desc.extent.height == 0u || desc.extent.depth == 0u ||
                desc.mipLevels == 0u || desc.sampleCount == 0u)
            {
                reason = "texture dimensions, mip count, and sample count must be non-zero";
                return false;
            }

            uint32_t largestDimension = desc.extent.width;
            if (desc.dimension != TextureDimension::Texture1D)
                largestDimension = (std::max)(largestDimension, desc.extent.height);
            if (desc.dimension == TextureDimension::Texture3D)
                largestDimension = (std::max)(largestDimension, desc.extent.depth);
            uint32_t maximumMipLevels = 1u;
            while (largestDimension > 1u)
            {
                largestDimension >>= 1u;
                ++maximumMipLevels;
            }
            if (desc.mipLevels > maximumMipLevels)
            {
                reason = "texture mip count exceeds the maximum supported by its dimensions";
                return false;
            }

            const uint32_t layerCount = GetTextureLayerCount(desc.dimension, desc.arrayLayers);
            if ((desc.dimension == TextureDimension::TextureCube ||
                    desc.dimension == TextureDimension::TextureCubeArray) &&
                desc.extent.width != desc.extent.height)
            {
                reason = "cube textures require square faces";
                return false;
            }
            if (desc.dimension == TextureDimension::TextureCubeArray &&
                (layerCount < 6u || (layerCount % 6u) != 0u))
            {
                reason = "cube-array layer counts must be a positive multiple of six faces";
                return false;
            }
            if (desc.dimension == TextureDimension::Texture3D && desc.arrayLayers > 1u)
            {
                reason = "3D texture arrays are not supported by Metal";
                return false;
            }
            if (desc.sampleCount > 1u)
            {
                if ((desc.dimension != TextureDimension::Texture2D &&
                        desc.dimension != TextureDimension::Texture2DArray) ||
                    desc.mipLevels != 1u)
                {
                    reason = "multisampled Metal textures must be 2D or 2D-array textures with one mip";
                    return false;
                }
            }
            const auto* formatInfo = GetTextureFormatDescriptor(desc.format);
            if (formatInfo != nullptr && formatInfo->isCompressed)
            {
                if (desc.dimension == TextureDimension::Texture1D ||
                    desc.dimension == TextureDimension::Texture3D)
                {
                    reason = "compressed Metal textures must be 2D, 2D-array, cube, or cube-array textures";
                    return false;
                }
                if (desc.sampleCount > 1u ||
                    HasTextureUsage(desc.usage, TextureUsageFlags::Storage) ||
                    HasTextureUsage(desc.usage, TextureUsageFlags::ColorAttachment) ||
                    HasTextureUsage(desc.usage, TextureUsageFlags::DepthStencilAttachment) ||
                    HasTextureUsage(desc.usage, TextureUsageFlags::Present))
                {
                    reason = "compressed Metal textures support sampled and copy usage only";
                    return false;
                }
            }
            return true;
        }

        bool ResolveMetalBufferToTextureCopyLayout(
            const NLS::Render::RHI::RHIBufferToTextureCopyDesc& desc,
            const NLS::Render::RHI::RHIBufferDesc& sourceDesc,
            const NLS::Render::RHI::RHITextureDesc& destinationDesc,
            uint32_t& rowPitch,
            uint32_t& slicePitch,
            std::string& reason)
        {
            using namespace NLS::Render::RHI;
            if (desc.mipLevel >= destinationDesc.mipLevels)
            {
                reason = "mip level is out of range";
                return false;
            }

            const uint32_t layerCount = destinationDesc.dimension == TextureDimension::Texture3D
                ? 1u
                : GetTextureLayerCount(destinationDesc.dimension, destinationDesc.arrayLayers);
            if (desc.arrayLayer >= layerCount)
            {
                reason = "array layer is out of range";
                return false;
            }
            if (desc.extent.width == 0u || desc.extent.height == 0u || desc.extent.depth == 0u ||
                desc.textureOffset.x < 0 || desc.textureOffset.y < 0 || desc.textureOffset.z < 0)
            {
                reason = "copy extent must be non-empty and texture offsets must be non-negative";
                return false;
            }
            if (destinationDesc.dimension == TextureDimension::Texture1D &&
                (desc.textureOffset.y != 0 || desc.textureOffset.z != 0 ||
                    desc.extent.height != 1u || desc.extent.depth != 1u))
            {
                reason = "1D texture copies require a single row and depth slice";
                return false;
            }
            if (destinationDesc.dimension != TextureDimension::Texture3D &&
                (desc.textureOffset.z != 0 || desc.extent.depth != 1u))
            {
                reason = "non-3D texture copies require a single depth slice";
                return false;
            }

            const uint32_t mipWidth = GetMipDimension(destinationDesc.extent.width, desc.mipLevel);
            const uint32_t mipHeight = destinationDesc.dimension == TextureDimension::Texture1D
                ? 1u
                : GetMipDimension(destinationDesc.extent.height, desc.mipLevel);
            const uint32_t mipDepth = destinationDesc.dimension == TextureDimension::Texture3D
                ? GetMipDimension(destinationDesc.extent.depth, desc.mipLevel)
                : 1u;
            const uint32_t offsetX = static_cast<uint32_t>(desc.textureOffset.x);
            const uint32_t offsetY = static_cast<uint32_t>(desc.textureOffset.y);
            const uint32_t offsetZ = static_cast<uint32_t>(desc.textureOffset.z);
            if (offsetX >= mipWidth || offsetY >= mipHeight || offsetZ >= mipDepth ||
                desc.extent.width > mipWidth - offsetX ||
                desc.extent.height > mipHeight - offsetY ||
                desc.extent.depth > mipDepth - offsetZ)
            {
                reason = "copy region is outside the destination mip bounds";
                return false;
            }

            const auto* formatInfo = GetTextureFormatDescriptor(destinationDesc.format);
            if (formatInfo == nullptr || formatInfo->bytesPerBlock == 0u ||
                formatInfo->blockWidth == 0u || formatInfo->blockHeight == 0u ||
                formatInfo->blockDepth == 0u)
            {
                reason = "destination texture format has no valid copy layout";
                return false;
            }
            if (formatInfo->isCompressed)
            {
                const bool alignedOrigin =
                    (offsetX % formatInfo->blockWidth) == 0u &&
                    (offsetY % formatInfo->blockHeight) == 0u &&
                    (offsetZ % formatInfo->blockDepth) == 0u;
                const bool alignedOrTouchesRightEdge =
                    (desc.extent.width % formatInfo->blockWidth) == 0u ||
                    offsetX + desc.extent.width == mipWidth;
                const bool alignedOrTouchesBottomEdge =
                    (desc.extent.height % formatInfo->blockHeight) == 0u ||
                    offsetY + desc.extent.height == mipHeight;
                const bool alignedOrTouchesBackEdge =
                    (desc.extent.depth % formatInfo->blockDepth) == 0u ||
                    offsetZ + desc.extent.depth == mipDepth;
                if (!alignedOrigin || !alignedOrTouchesRightEdge ||
                    !alignedOrTouchesBottomEdge || !alignedOrTouchesBackEdge)
                {
                    reason = "compressed copy regions must be block-aligned or reach the mip edge";
                    return false;
                }
            }

            const uint32_t minimumRowPitch = CalculateTextureRowPitch(
                destinationDesc.format,
                desc.extent.width);
            rowPitch = desc.rowPitch != 0u ? desc.rowPitch : minimumRowPitch;
            const uint32_t blockRows =
                (desc.extent.height + formatInfo->blockHeight - 1u) / formatInfo->blockHeight;
            const uint32_t blockDepth =
                (desc.extent.depth + formatInfo->blockDepth - 1u) / formatInfo->blockDepth;
            const uint64_t minimumSlicePitch = static_cast<uint64_t>(rowPitch) * blockRows;
            if (minimumRowPitch == 0u || rowPitch < minimumRowPitch ||
                (rowPitch % formatInfo->bytesPerBlock) != 0u ||
                minimumSlicePitch > (std::numeric_limits<uint32_t>::max)())
            {
                reason = "source row pitch is invalid for the destination format";
                return false;
            }
            slicePitch = desc.slicePitch != 0u
                ? desc.slicePitch
                : static_cast<uint32_t>(minimumSlicePitch);
            if (slicePitch < minimumSlicePitch)
            {
                reason = "source slice pitch is smaller than one block-row image";
                return false;
            }

            const uint64_t requiredBytes = static_cast<uint64_t>(slicePitch) * (blockDepth - 1u) +
                static_cast<uint64_t>(rowPitch) * (blockRows - 1u) + minimumRowPitch;
            if (desc.bufferOffset > sourceDesc.size || requiredBytes > sourceDesc.size - desc.bufferOffset)
            {
                reason = "source buffer span is smaller than the requested texture copy";
                return false;
            }
            return true;
        }

        bool ValidateMetalBufferCopyRegion(
            const NLS::Render::RHI::RHIBufferCopyRegion& region,
            const NLS::Render::RHI::RHIBufferDesc& sourceDesc,
            const NLS::Render::RHI::RHIBufferDesc& destinationDesc,
            std::string& reason)
        {
            if (region.size == 0u)
            {
                reason = "copy size must be non-zero";
                return false;
            }
            if (region.srcOffset > sourceDesc.size || region.size > sourceDesc.size - region.srcOffset)
            {
                reason = "source range exceeds the source buffer";
                return false;
            }
            if (region.dstOffset > destinationDesc.size || region.size > destinationDesc.size - region.dstOffset)
            {
                reason = "destination range exceeds the destination buffer";
                return false;
            }
            return true;
        }

        bool ValidateMetalTextureCopyDesc(
            const NLS::Render::RHI::RHITextureCopyDesc& desc,
            const NLS::Render::RHI::RHITextureDesc& sourceDesc,
            const NLS::Render::RHI::RHITextureDesc& destinationDesc,
            std::string& reason)
        {
            using namespace NLS::Render::RHI;
            if (desc.extent.width == 0u || desc.extent.height == 0u || desc.extent.depth == 0u)
            {
                reason = "copy extent must be non-empty";
                return false;
            }
            if (desc.sourceOffset.x < 0 || desc.sourceOffset.y < 0 || desc.sourceOffset.z < 0 ||
                desc.destinationOffset.x < 0 || desc.destinationOffset.y < 0 || desc.destinationOffset.z < 0)
            {
                reason = "texture offsets must be non-negative";
                return false;
            }
            if (desc.sourceRange.mipLevelCount != 1u || desc.sourceRange.arrayLayerCount != 1u ||
                desc.destinationRange.mipLevelCount != 1u || desc.destinationRange.arrayLayerCount != 1u)
            {
                reason = "texture copies operate on exactly one mip level and array layer";
                return false;
            }
            if (desc.sourceRange.baseMipLevel >= sourceDesc.mipLevels ||
                desc.destinationRange.baseMipLevel >= destinationDesc.mipLevels)
            {
                reason = "source or destination mip level is out of range";
                return false;
            }

            const uint32_t sourceLayerCount = sourceDesc.dimension == TextureDimension::Texture3D
                ? 1u
                : GetTextureLayerCount(sourceDesc.dimension, sourceDesc.arrayLayers);
            const uint32_t destinationLayerCount = destinationDesc.dimension == TextureDimension::Texture3D
                ? 1u
                : GetTextureLayerCount(destinationDesc.dimension, destinationDesc.arrayLayers);
            if (desc.sourceRange.baseArrayLayer >= sourceLayerCount ||
                desc.destinationRange.baseArrayLayer >= destinationLayerCount)
            {
                reason = "source or destination array layer is out of range";
                return false;
            }

            if (sourceDesc.sampleCount != 1u || destinationDesc.sampleCount != 1u)
            {
                reason = "multisampled textures require a resolve operation instead of a texture copy";
                return false;
            }
            const MTLPixelFormat sourceFormat = ToMetalPixelFormat(sourceDesc.format, sourceDesc.colorSpace);
            const MTLPixelFormat destinationFormat = ToMetalPixelFormat(
                destinationDesc.format,
                destinationDesc.colorSpace);
            if (sourceFormat == MTLPixelFormatInvalid || sourceFormat != destinationFormat)
            {
                reason = "source and destination Metal pixel formats must match";
                return false;
            }

            const bool sourceIs1D = sourceDesc.dimension == TextureDimension::Texture1D;
            const bool destinationIs1D = destinationDesc.dimension == TextureDimension::Texture1D;
            const bool sourceIs3D = sourceDesc.dimension == TextureDimension::Texture3D;
            const bool destinationIs3D = destinationDesc.dimension == TextureDimension::Texture3D;
            if (sourceIs1D != destinationIs1D || sourceIs3D != destinationIs3D)
            {
                reason = "1D and 3D textures can only be copied to the same texture dimension";
                return false;
            }
            if (sourceIs1D &&
                (desc.sourceOffset.y != 0 || desc.sourceOffset.z != 0 ||
                    desc.destinationOffset.y != 0 || desc.destinationOffset.z != 0 ||
                    desc.extent.height != 1u || desc.extent.depth != 1u))
            {
                reason = "1D texture copies require a single row and depth slice";
                return false;
            }
            if (!sourceIs3D &&
                (desc.sourceOffset.z != 0 || desc.destinationOffset.z != 0 || desc.extent.depth != 1u))
            {
                reason = "non-3D texture copies require a single depth slice";
                return false;
            }

            struct MipCopyBounds
            {
                uint32_t width = 1u;
                uint32_t height = 1u;
                uint32_t depth = 1u;
                uint32_t offsetX = 0u;
                uint32_t offsetY = 0u;
                uint32_t offsetZ = 0u;
            };
            const auto resolveBounds = [](const RHITextureDesc& textureDesc,
                                          const uint32_t mipLevel,
                                          const RHIOffset3D& offset)
            {
                MipCopyBounds bounds;
                bounds.width = GetMipDimension(textureDesc.extent.width, mipLevel);
                bounds.height = textureDesc.dimension == TextureDimension::Texture1D
                    ? 1u
                    : GetMipDimension(textureDesc.extent.height, mipLevel);
                bounds.depth = textureDesc.dimension == TextureDimension::Texture3D
                    ? GetMipDimension(textureDesc.extent.depth, mipLevel)
                    : 1u;
                bounds.offsetX = static_cast<uint32_t>(offset.x);
                bounds.offsetY = static_cast<uint32_t>(offset.y);
                bounds.offsetZ = static_cast<uint32_t>(offset.z);
                return bounds;
            };
            const MipCopyBounds sourceBounds = resolveBounds(
                sourceDesc,
                desc.sourceRange.baseMipLevel,
                desc.sourceOffset);
            const MipCopyBounds destinationBounds = resolveBounds(
                destinationDesc,
                desc.destinationRange.baseMipLevel,
                desc.destinationOffset);
            const auto isInsideBounds = [&desc](const MipCopyBounds& bounds)
            {
                return bounds.offsetX < bounds.width &&
                    bounds.offsetY < bounds.height &&
                    bounds.offsetZ < bounds.depth &&
                    desc.extent.width <= bounds.width - bounds.offsetX &&
                    desc.extent.height <= bounds.height - bounds.offsetY &&
                    desc.extent.depth <= bounds.depth - bounds.offsetZ;
            };
            if (!isInsideBounds(sourceBounds) || !isInsideBounds(destinationBounds))
            {
                reason = "copy region is outside the source or destination mip bounds";
                return false;
            }

            const auto* formatInfo = GetTextureFormatDescriptor(sourceDesc.format);
            if (formatInfo == nullptr || formatInfo->bytesPerBlock == 0u ||
                formatInfo->blockWidth == 0u || formatInfo->blockHeight == 0u ||
                formatInfo->blockDepth == 0u)
            {
                reason = "texture format has no valid copy layout";
                return false;
            }
            if (formatInfo->isCompressed)
            {
                const auto isBlockAligned = [&desc, formatInfo](const MipCopyBounds& bounds)
                {
                    return (bounds.offsetX % formatInfo->blockWidth) == 0u &&
                        (bounds.offsetY % formatInfo->blockHeight) == 0u &&
                        (bounds.offsetZ % formatInfo->blockDepth) == 0u &&
                        ((desc.extent.width % formatInfo->blockWidth) == 0u ||
                            bounds.offsetX + desc.extent.width == bounds.width) &&
                        ((desc.extent.height % formatInfo->blockHeight) == 0u ||
                            bounds.offsetY + desc.extent.height == bounds.height) &&
                        ((desc.extent.depth % formatInfo->blockDepth) == 0u ||
                            bounds.offsetZ + desc.extent.depth == bounds.depth);
                };
                if (!isBlockAligned(sourceBounds) || !isBlockAligned(destinationBounds))
                {
                    reason = "compressed copy regions must be block-aligned or reach both mip edges";
                    return false;
                }
            }
            return true;
        }

        bool ValidateMetalTextureView(
            const NLS::Render::RHI::RHITextureDesc& textureDesc,
            const NLS::Render::RHI::RHITextureViewDesc& viewDesc,
            const NLS::Render::RHI::RHISubresourceRange& range)
        {
            using namespace NLS::Render::RHI;
            TextureViewType viewType = viewDesc.viewType;
            if (viewType == TextureViewType::Auto)
            {
                switch (textureDesc.dimension)
                {
                case TextureDimension::Texture2DArray: viewType = TextureViewType::Texture2DArray; break;
                case TextureDimension::TextureCube: viewType = TextureViewType::Cube; break;
                case TextureDimension::TextureCubeArray: viewType = TextureViewType::CubeArray; break;
                case TextureDimension::Texture3D: viewType = TextureViewType::Texture3D; break;
                case TextureDimension::Texture1D:
                case TextureDimension::Texture2D:
                default: viewType = TextureViewType::Texture2D; break;
                }
            }

            if (textureDesc.dimension == TextureDimension::Texture3D)
                return viewType == TextureViewType::Texture3D;
            if (viewType == TextureViewType::Texture3D)
                return false;
            if (textureDesc.dimension == TextureDimension::Texture1D)
                return viewType == TextureViewType::Texture2D && range.arrayLayerCount == 1u;
            if (viewType == TextureViewType::Texture2D)
                return range.arrayLayerCount == 1u;
            if (viewType == TextureViewType::Cube)
            {
                return textureDesc.sampleCount == 1u &&
                    range.arrayLayerCount == 6u &&
                    (range.baseArrayLayer % 6u) == 0u;
            }
            if (viewType == TextureViewType::CubeArray)
            {
                return textureDesc.sampleCount == 1u &&
                    range.arrayLayerCount >= 6u &&
                    (range.arrayLayerCount % 6u) == 0u &&
                    (range.baseArrayLayer % 6u) == 0u;
            }
            return viewType == TextureViewType::Texture2DArray;
        }

        class MetalAdapter final : public NLS::Render::RHI::RHIAdapter
        {
        public:
            explicit MetalAdapter(std::string hardware)
                : m_hardware(std::move(hardware))
            {
            }

            std::string_view GetDebugName() const override { return "MetalAdapter"; }
            NLS::Render::RHI::NativeBackendType GetBackendType() const override
            {
                return NLS::Render::RHI::NativeBackendType::Metal;
            }
            std::string_view GetVendor() const override { return "Apple"; }
            std::string_view GetHardware() const override { return m_hardware; }

        private:
            std::string m_hardware;
        };

        class MetalBuffer final : public NLS::Render::RHI::RHIBuffer
        {
        public:
            MetalBuffer(id<MTLBuffer> buffer, NLS::Render::RHI::RHIBufferDesc desc)
                : m_buffer([buffer retain])
                , m_desc(std::move(desc))
            {
                if (m_desc.memoryUsage == NLS::Render::RHI::MemoryUsage::GPUToCPU)
                    m_state = NLS::Render::RHI::ResourceState::CopyDst;
                else if (m_desc.memoryUsage == NLS::Render::RHI::MemoryUsage::CPUToGPU)
                    m_state = NLS::Render::RHI::ResourceState::GenericRead;
            }

            ~MetalBuffer() override
            {
                [m_buffer release];
            }

            std::string_view GetDebugName() const override { return m_desc.debugName; }
            const NLS::Render::RHI::RHIBufferDesc& GetDesc() const override { return m_desc; }
            NLS::Render::RHI::ResourceState GetState() const override { return m_state; }
            void SetState(const NLS::Render::RHI::ResourceState state) { m_state = state; }
            uint64_t GetGPUAddress() const override { return 0u; }
            NLS::Render::RHI::NativeHandle GetNativeBufferHandle() override
            {
                return { kMetalBackendType, (__bridge void*)m_buffer };
            }
            NLS::Render::RHI::RHIUpdateResult UpdateData(
                const NLS::Render::RHI::RHIBufferUploadDesc& uploadDesc) override
            {
                if (m_buffer == nil || !uploadDesc.HasData() ||
                    uploadDesc.destinationOffset > m_desc.size ||
                    uploadDesc.dataSize > m_desc.size - uploadDesc.destinationOffset)
                {
                    return { NLS::Render::RHI::RHIUpdateStatusCode::InvalidArgument, "Invalid Metal buffer upload" };
                }
                if (m_desc.memoryUsage != NLS::Render::RHI::MemoryUsage::CPUToGPU)
                {
                    return {
                        NLS::Render::RHI::RHIUpdateStatusCode::Unsupported,
                        "Metal buffer updates require CPUToGPU memory usage"
                    };
                }

                auto* destination = static_cast<uint8_t*>([m_buffer contents]);
                if (destination == nullptr)
                {
                    return {
                        NLS::Render::RHI::RHIUpdateStatusCode::BackendFailure,
                        "Metal CPUToGPU buffer is not CPU-visible"
                    };
                }

                std::memcpy(
                    destination + uploadDesc.destinationOffset,
                    uploadDesc.data,
                    uploadDesc.dataSize);
                return { NLS::Render::RHI::RHIUpdateStatusCode::Success, {} };
            }

            id<MTLBuffer> GetBuffer() const { return m_buffer; }

        private:
            id<MTLBuffer> m_buffer = nil;
            NLS::Render::RHI::RHIBufferDesc m_desc{};
            NLS::Render::RHI::ResourceState m_state = NLS::Render::RHI::ResourceState::Unknown;
        };

        class MetalBufferReadbackCompletionToken final : public NLS::Render::RHI::RHICompletionToken
        {
        public:
            MetalBufferReadbackCompletionToken(
                id<MTLCommandBuffer> commandBuffer,
                std::shared_ptr<MetalBuffer> source,
                id<MTLBuffer> readbackBuffer,
                const uint64_t readbackOffset,
                const uint64_t size,
                void* destination,
                std::string debugName)
                : m_commandBuffer([commandBuffer retain])
                , m_source(std::move(source))
                , m_readbackBuffer([readbackBuffer retain])
                , m_readbackOffset(readbackOffset)
                , m_size(size)
                , m_destination(destination)
                , m_debugName(std::move(debugName))
            {
            }

            ~MetalBufferReadbackCompletionToken() override
            {
                [m_readbackBuffer release];
                [m_commandBuffer release];
            }

            std::string_view GetDebugName() const override
            {
                if (m_debugName.empty())
                    return "MetalBufferReadbackCompletionToken";
                return m_debugName;
            }

            NLS::Render::RHI::RHICompletionStatus Poll() override
            {
                std::lock_guard lock(m_mutex);
                if (m_status.IsComplete())
                    return m_status;
                if (m_commandBuffer == nil || m_source == nullptr || m_source->GetBuffer() == nil ||
                    m_readbackBuffer == nil)
                    return CompleteWithFailure("Metal buffer readback completion token is missing backend resources");

                switch (m_commandBuffer.status)
                {
                case MTLCommandBufferStatusError:
                {
                    const char* errorMessage = m_commandBuffer.error.localizedDescription.UTF8String;
                    return CompleteWithFailure(
                        "Metal buffer readback command failed: " +
                        std::string(errorMessage != nullptr ? errorMessage : "unknown error"));
                }
                case MTLCommandBufferStatusCompleted:
                {
                    const auto* sourceBytes = static_cast<const uint8_t*>(m_readbackBuffer.contents);
                    if (sourceBytes == nullptr || m_destination == nullptr || m_size == 0u)
                        return CompleteWithFailure("Metal buffer readback destination or mapped source is unavailable");

                    std::memcpy(
                        m_destination,
                        sourceBytes + m_readbackOffset,
                        static_cast<size_t>(m_size));
                    m_status = { NLS::Render::RHI::RHICompletionStatusCode::Success, {} };
                    return m_status;
                }
                case MTLCommandBufferStatusNotEnqueued:
                case MTLCommandBufferStatusEnqueued:
                case MTLCommandBufferStatusCommitted:
                case MTLCommandBufferStatusScheduled:
                default:
                    return { NLS::Render::RHI::RHICompletionStatusCode::Pending, {} };
                }
            }

            NLS::Render::RHI::RHICompletionStatus Wait(const uint64_t timeoutNanoseconds = 0u) override
            {
                if (timeoutNanoseconds == 0u)
                {
                    if (m_commandBuffer != nil)
                        [m_commandBuffer waitUntilCompleted];
                    return Poll();
                }

                const auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::nanoseconds(timeoutNanoseconds);
                for (;;)
                {
                    const auto status = Poll();
                    if (status.IsComplete())
                        return status;

                    const auto now = std::chrono::steady_clock::now();
                    if (now >= deadline)
                    {
                        return {
                            NLS::Render::RHI::RHICompletionStatusCode::Pending,
                            "Metal buffer readback completion wait timed out"
                        };
                    }

                    std::this_thread::sleep_for((std::min)(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now),
                        std::chrono::nanoseconds(100000u)));
                }
            }

        private:
            NLS::Render::RHI::RHICompletionStatus CompleteWithFailure(std::string message)
            {
                m_status = {
                    NLS::Render::RHI::RHICompletionStatusCode::Failed,
                    std::move(message)
                };
                return m_status;
            }

            id<MTLCommandBuffer> m_commandBuffer = nil;
            std::shared_ptr<MetalBuffer> m_source;
            id<MTLBuffer> m_readbackBuffer = nil;
            uint64_t m_readbackOffset = 0u;
            uint64_t m_size = 0u;
            void* m_destination = nullptr;
            std::string m_debugName;
            std::mutex m_mutex;
            NLS::Render::RHI::RHICompletionStatus m_status{};
        };

        class MetalTexture final : public NLS::Render::RHI::RHITexture
        {
        public:
            MetalTexture(
                id<MTLTexture> texture,
                NLS::Render::RHI::RHITextureDesc desc,
                const NLS::Render::RHI::ResourceState initialState = NLS::Render::RHI::ResourceState::Unknown)
                : m_texture([texture retain])
                , m_desc(std::move(desc))
                , m_state(initialState)
            {
            }

            ~MetalTexture() override
            {
                [m_texture release];
            }

            std::string_view GetDebugName() const override { return m_desc.debugName; }
            const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
            NLS::Render::RHI::ResourceState GetState() const override { return m_state; }
            void SetState(const NLS::Render::RHI::ResourceState state)
            {
                m_state = state;
                m_partialStateDirty = false;
            }
            void MarkPartialStateDirty() { m_partialStateDirty = true; }
            bool HasPartialStateDirty() const { return m_partialStateDirty; }
            NLS::Render::RHI::NativeHandle GetNativeImageHandle() override
            {
                return { kMetalBackendType, (__bridge void*)m_texture };
            }

            id<MTLTexture> GetTexture() const { return m_texture; }

        private:
            id<MTLTexture> m_texture = nil;
            NLS::Render::RHI::RHITextureDesc m_desc{};
            NLS::Render::RHI::ResourceState m_state = NLS::Render::RHI::ResourceState::Unknown;
            bool m_partialStateDirty = false;
        };

        std::optional<size_t> GetMetalReadbackBytesPerPixel(const MTLPixelFormat format)
        {
            switch (format)
            {
            case MTLPixelFormatR8Unorm:
                return 1u;
            case MTLPixelFormatRG8Unorm:
            case MTLPixelFormatR16Float:
                return 2u;
            case MTLPixelFormatRGBA8Unorm:
            case MTLPixelFormatRGBA8Unorm_sRGB:
            case MTLPixelFormatBGRA8Unorm:
            case MTLPixelFormatBGRA8Unorm_sRGB:
            case MTLPixelFormatRG16Float:
            case MTLPixelFormatR32Float:
            case MTLPixelFormatDepth32Float:
                return 4u;
            case MTLPixelFormatRGBA16Float:
            case MTLPixelFormatRG32Float:
                return 8u;
            case MTLPixelFormatRGBA32Float:
                return 16u;
            default:
                return std::nullopt;
            }
        }

        class MetalPixelReadbackCompletionToken final : public NLS::Render::RHI::RHICompletionToken
        {
        public:
            MetalPixelReadbackCompletionToken(
                id<MTLCommandBuffer> commandBuffer,
                std::shared_ptr<MetalTexture> source,
                id<MTLBuffer> stagingBuffer,
                const size_t sourceBytesPerPixel,
                const size_t destinationBytesPerPixel,
                const size_t stagingRowBytes,
                const uint32_t width,
                const uint32_t height,
                const bool sourceIsBgra,
                const NLS::Render::Settings::EPixelDataFormat destinationFormat,
                void* destination)
                : m_commandBuffer([commandBuffer retain])
                , m_source(std::move(source))
                , m_stagingBuffer([stagingBuffer retain])
                , m_sourceBytesPerPixel(sourceBytesPerPixel)
                , m_destinationBytesPerPixel(destinationBytesPerPixel)
                , m_stagingRowBytes(stagingRowBytes)
                , m_width(width)
                , m_height(height)
                , m_sourceIsBgra(sourceIsBgra)
                , m_destinationFormat(destinationFormat)
                , m_destination(destination)
            {
            }

            ~MetalPixelReadbackCompletionToken() override
            {
                [m_stagingBuffer release];
                [m_commandBuffer release];
            }

            std::string_view GetDebugName() const override
            {
                return "MetalPixelReadbackCompletionToken";
            }

            NLS::Render::RHI::RHICompletionStatus Poll() override
            {
                std::lock_guard lock(m_mutex);
                if (m_status.IsComplete())
                    return m_status;
                if (m_commandBuffer == nil || m_source == nullptr ||
                    m_source->GetTexture() == nil || m_stagingBuffer == nil ||
                    m_destination == nullptr)
                {
                    return CompleteWithFailure(
                        "Metal pixel readback completion token is missing backend resources");
                }

                switch (m_commandBuffer.status)
                {
                case MTLCommandBufferStatusError:
                {
                    const char* errorMessage = m_commandBuffer.error.localizedDescription.UTF8String;
                    return CompleteWithFailure(
                        "Metal pixel readback command failed: " +
                        std::string(errorMessage != nullptr ? errorMessage : "unknown error"));
                }
                case MTLCommandBufferStatusCompleted:
                    CopyToDestination();
                    m_status = { NLS::Render::RHI::RHICompletionStatusCode::Success, {} };
                    return m_status;
                case MTLCommandBufferStatusNotEnqueued:
                case MTLCommandBufferStatusEnqueued:
                case MTLCommandBufferStatusCommitted:
                case MTLCommandBufferStatusScheduled:
                default:
                    return { NLS::Render::RHI::RHICompletionStatusCode::Pending, {} };
                }
            }

            NLS::Render::RHI::RHICompletionStatus Wait(const uint64_t timeoutNanoseconds = 0u) override
            {
                if (timeoutNanoseconds == 0u)
                {
                    if (m_commandBuffer != nil)
                        [m_commandBuffer waitUntilCompleted];
                    return Poll();
                }

                const auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::nanoseconds(timeoutNanoseconds);
                for (;;)
                {
                    const auto status = Poll();
                    if (status.IsComplete())
                        return status;

                    const auto now = std::chrono::steady_clock::now();
                    if (now >= deadline)
                    {
                        return {
                            NLS::Render::RHI::RHICompletionStatusCode::Pending,
                            "Metal pixel readback completion wait timed out"
                        };
                    }

                    std::this_thread::sleep_for((std::min)(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now),
                        std::chrono::nanoseconds(100000u)));
                }
            }

        private:
            void CopyToDestination()
            {
                const auto* source = static_cast<const uint8_t*>(m_stagingBuffer.contents);
                auto* destination = static_cast<uint8_t*>(m_destination);
                const bool destinationIsBgr =
                    m_destinationFormat == NLS::Render::Settings::EPixelDataFormat::BGR ||
                    m_destinationFormat == NLS::Render::Settings::EPixelDataFormat::BGRA;
                const size_t redIndex = m_sourceIsBgra ? 2u : 0u;
                const size_t blueIndex = m_sourceIsBgra ? 0u : 2u;

                for (uint32_t row = 0u; row < m_height; ++row)
                {
                    for (uint32_t column = 0u; column < m_width; ++column)
                    {
                        const size_t sourceIndex = static_cast<size_t>(row) * m_stagingRowBytes +
                            static_cast<size_t>(column) * m_sourceBytesPerPixel;
                        const size_t destinationIndex =
                            (static_cast<size_t>(row) * m_width + column) * m_destinationBytesPerPixel;
                        destination[destinationIndex + 0u] =
                            source[sourceIndex + (destinationIsBgr ? blueIndex : redIndex)];
                        destination[destinationIndex + 1u] = source[sourceIndex + 1u];
                        destination[destinationIndex + 2u] =
                            source[sourceIndex + (destinationIsBgr ? redIndex : blueIndex)];
                        if (m_destinationBytesPerPixel == 4u)
                            destination[destinationIndex + 3u] = source[sourceIndex + 3u];
                    }
                }
            }

            NLS::Render::RHI::RHICompletionStatus CompleteWithFailure(std::string message)
            {
                m_status = {
                    NLS::Render::RHI::RHICompletionStatusCode::Failed,
                    std::move(message)
                };
                return m_status;
            }

            id<MTLCommandBuffer> m_commandBuffer = nil;
            std::shared_ptr<MetalTexture> m_source;
            id<MTLBuffer> m_stagingBuffer = nil;
            size_t m_sourceBytesPerPixel = 0u;
            size_t m_destinationBytesPerPixel = 0u;
            size_t m_stagingRowBytes = 0u;
            uint32_t m_width = 0u;
            uint32_t m_height = 0u;
            bool m_sourceIsBgra = false;
            NLS::Render::Settings::EPixelDataFormat m_destinationFormat =
                NLS::Render::Settings::EPixelDataFormat::RGBA;
            void* m_destination = nullptr;
            std::mutex m_mutex;
            NLS::Render::RHI::RHICompletionStatus m_status{};
        };

        class MetalTextureView final : public NLS::Render::RHI::RHITextureView
        {
        public:
            MetalTextureView(
                std::shared_ptr<NLS::Render::RHI::RHITexture> texture,
                NLS::Render::RHI::RHITextureViewDesc desc,
                id<MTLTexture> textureView)
                : m_texture(std::move(texture))
                , m_desc(std::move(desc))
                , m_textureView([textureView retain])
            {
            }

            ~MetalTextureView() override
            {
                [m_textureView release];
            }

            std::string_view GetDebugName() const override { return m_desc.debugName; }
            const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override { return m_texture; }
            NLS::Render::RHI::NativeHandle GetNativeShaderResourceView() override
            {
                return { kMetalBackendType, (__bridge void*)m_textureView };
            }
            NLS::Render::RHI::NativeHandle GetNativeRenderTargetView() override
            {
                return { kMetalBackendType, (__bridge void*)m_textureView };
            }
            NLS::Render::RHI::NativeHandle GetNativeDepthStencilView() override
            {
                return { kMetalBackendType, (__bridge void*)m_textureView };
            }

            id<MTLTexture> GetMetalTextureView() const { return m_textureView; }

        private:
            std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
            NLS::Render::RHI::RHITextureViewDesc m_desc{};
            id<MTLTexture> m_textureView = nil;
        };

        class MetalFence final : public NLS::Render::RHI::RHIFence
        {
        public:
            explicit MetalFence(std::string debugName) : m_debugName(std::move(debugName)) {}

            std::string_view GetDebugName() const override { return m_debugName; }
            bool IsSignaled() const override { return m_signaled.load(std::memory_order_acquire); }
            void Reset() override
            {
                std::lock_guard lock(m_mutex);
                m_signaled.store(false, std::memory_order_release);
            }
            bool Wait(const uint64_t timeoutNanoseconds) override
            {
                std::unique_lock lock(m_mutex);
                const auto signaled = [this]()
                {
                    return m_signaled.load(std::memory_order_acquire);
                };
                if (timeoutNanoseconds == 0u)
                {
                    m_condition.wait(lock, signaled);
                    return true;
                }
                return m_condition.wait_for(
                    lock,
                    std::chrono::nanoseconds(timeoutNanoseconds),
                    signaled);
            }
            void Signal()
            {
                {
                    std::lock_guard lock(m_mutex);
                    m_signaled.store(true, std::memory_order_release);
                }
                m_condition.notify_all();
            }

        private:
            std::string m_debugName;
            std::atomic_bool m_signaled { true };
            std::mutex m_mutex;
            std::condition_variable m_condition;
        };

        class MetalSemaphore final : public NLS::Render::RHI::RHISemaphore
        {
        public:
            MetalSemaphore(id<MTLDevice> device, std::string debugName)
                : m_event(device != nil ? [device newSharedEvent] : nil)
                , m_debugName(std::move(debugName))
            {
                if (m_event != nil && !m_debugName.empty())
                    m_event.label = [NSString stringWithUTF8String:m_debugName.c_str()];
            }

            ~MetalSemaphore() override
            {
                [m_event release];
            }

            std::string_view GetDebugName() const override { return m_debugName; }
            bool IsSignaled() const override
            {
                const uint64_t waitValue = m_waitValue.load(std::memory_order_acquire);
                return m_event != nil && waitValue != 0u && m_event.signaledValue >= waitValue;
            }
            void Reset() override { m_waitValue.store(0u, std::memory_order_release); }
            NLS::Render::RHI::NativeHandle GetNativeSemaphoreHandle() override
            {
                return m_event != nil
                    ? NLS::Render::RHI::NativeHandle {
                        kMetalBackendType,
                        (__bridge void*)m_event,
                        m_waitValue.load(std::memory_order_acquire)
                    }
                    : NLS::Render::RHI::NativeHandle {};
            }

            bool IsValid() const { return m_event != nil; }
            id<MTLSharedEvent> GetEvent() const { return m_event; }
            uint64_t GetWaitValue() const { return m_waitValue.load(std::memory_order_acquire); }
            uint64_t ReserveSignalValue()
            {
                const uint64_t value = m_signalValue.fetch_add(1u, std::memory_order_acq_rel) + 1u;
                m_waitValue.store(value, std::memory_order_release);
                return value;
            }
            bool SignalOnCpu()
            {
                if (m_event == nil)
                    return false;
                m_event.signaledValue = ReserveSignalValue();
                return true;
            }

        private:
            id<MTLSharedEvent> m_event = nil;
            std::string m_debugName;
            std::atomic_uint64_t m_signalValue { 0u };
            std::atomic_uint64_t m_waitValue { 0u };
        };

        class MetalSampler final : public NLS::Render::RHI::RHISampler
        {
        public:
            MetalSampler(id<MTLSamplerState> sampler, NLS::Render::RHI::SamplerDesc desc, std::string debugName)
                : m_sampler([sampler retain])
                , m_desc(std::move(desc))
                , m_debugName(std::move(debugName))
            {
            }

            ~MetalSampler() override
            {
                [m_sampler release];
            }

            std::string_view GetDebugName() const override { return m_debugName; }
            const NLS::Render::RHI::SamplerDesc& GetDesc() const override { return m_desc; }
            NLS::Render::RHI::NativeHandle GetNativeSamplerHandle() override
            {
                return { kMetalBackendType, (__bridge void*)m_sampler };
            }

            id<MTLSamplerState> GetSampler() const { return m_sampler; }

        private:
            id<MTLSamplerState> m_sampler = nil;
            NLS::Render::RHI::SamplerDesc m_desc{};
            std::string m_debugName;
        };

        class MetalBindingLayout final : public NLS::Render::RHI::RHIBindingLayout
        {
        public:
            explicit MetalBindingLayout(NLS::Render::RHI::RHIBindingLayoutDesc desc)
                : m_desc(std::move(desc))
            {
            }

            std::string_view GetDebugName() const override { return m_desc.debugName; }
            const NLS::Render::RHI::RHIBindingLayoutDesc& GetDesc() const override { return m_desc; }

        private:
            NLS::Render::RHI::RHIBindingLayoutDesc m_desc{};
        };

        class MetalBindingSet final : public NLS::Render::RHI::RHIBindingSet
        {
        public:
            explicit MetalBindingSet(NLS::Render::RHI::RHIBindingSetDesc desc)
                : m_desc(std::move(desc))
            {
            }

            std::string_view GetDebugName() const override { return m_desc.debugName; }
            const NLS::Render::RHI::RHIBindingSetDesc& GetDesc() const override { return m_desc; }

        private:
            NLS::Render::RHI::RHIBindingSetDesc m_desc{};
        };

        class MetalPipelineLayout final : public NLS::Render::RHI::RHIPipelineLayout
        {
        public:
            explicit MetalPipelineLayout(NLS::Render::RHI::RHIPipelineLayoutDesc desc)
                : m_desc(std::move(desc))
            {
            }

            std::string_view GetDebugName() const override { return m_desc.debugName; }
            const NLS::Render::RHI::RHIPipelineLayoutDesc& GetDesc() const override { return m_desc; }

        private:
            NLS::Render::RHI::RHIPipelineLayoutDesc m_desc{};
        };

        class MetalShaderModule final : public NLS::Render::RHI::RHIShaderModule
        {
        public:
            MetalShaderModule(
                id<MTLLibrary> library,
                id<MTLFunction> function,
                NLS::Render::RHI::RHIShaderModuleDesc desc,
                MTLSize threadgroupSize = MTLSizeMake(1u, 1u, 1u))
                : m_library([library retain])
                , m_function([function retain])
                , m_desc(std::move(desc))
                , m_threadgroupSize(threadgroupSize)
            {
            }

            ~MetalShaderModule() override
            {
                [m_function release];
                [m_library release];
            }

            std::string_view GetDebugName() const override { return m_desc.debugName; }
            const NLS::Render::RHI::RHIShaderModuleDesc& GetDesc() const override { return m_desc; }
            id<MTLFunction> GetFunction() const { return m_function; }
            MTLSize GetThreadgroupSize() const { return m_threadgroupSize; }

        private:
            id<MTLLibrary> m_library = nil;
            id<MTLFunction> m_function = nil;
            NLS::Render::RHI::RHIShaderModuleDesc m_desc{};
            MTLSize m_threadgroupSize = MTLSizeMake(1u, 1u, 1u);
        };

        class MetalGraphicsPipeline final : public NLS::Render::RHI::RHIGraphicsPipeline
        {
        public:
            MetalGraphicsPipeline(
                id<MTLRenderPipelineState> pipelineState,
                id<MTLDepthStencilState> depthStencilState,
                NLS::Render::RHI::RHIGraphicsPipelineDesc desc)
                : m_pipelineState([pipelineState retain])
                , m_depthStencilState([depthStencilState retain])
                , m_desc(std::move(desc))
            {
            }

            ~MetalGraphicsPipeline() override
            {
                [m_depthStencilState release];
                [m_pipelineState release];
            }

            std::string_view GetDebugName() const override { return m_desc.debugName; }
            const NLS::Render::RHI::RHIGraphicsPipelineDesc& GetDesc() const override { return m_desc; }
            uint64_t GetPipelineHandle() const override
            {
                return reinterpret_cast<uint64_t>((__bridge void*)m_pipelineState);
            }
            id<MTLRenderPipelineState> GetPipelineState() const { return m_pipelineState; }
            id<MTLDepthStencilState> GetDepthStencilState() const { return m_depthStencilState; }

        private:
            id<MTLRenderPipelineState> m_pipelineState = nil;
            id<MTLDepthStencilState> m_depthStencilState = nil;
            NLS::Render::RHI::RHIGraphicsPipelineDesc m_desc{};
        };

        class MetalComputePipeline final : public NLS::Render::RHI::RHIComputePipeline
        {
        public:
            MetalComputePipeline(
                id<MTLComputePipelineState> pipelineState,
                MTLSize threadgroupSize,
                NLS::Render::RHI::RHIComputePipelineDesc desc)
                : m_pipelineState([pipelineState retain])
                , m_threadgroupSize(threadgroupSize)
                , m_desc(std::move(desc))
            {
            }

            ~MetalComputePipeline() override
            {
                [m_pipelineState release];
            }

            std::string_view GetDebugName() const override { return m_desc.debugName; }
            const NLS::Render::RHI::RHIComputePipelineDesc& GetDesc() const override { return m_desc; }
            id<MTLComputePipelineState> GetPipelineState() const { return m_pipelineState; }
            MTLSize GetThreadgroupSize() const { return m_threadgroupSize; }

        private:
            id<MTLComputePipelineState> m_pipelineState = nil;
            MTLSize m_threadgroupSize = MTLSizeMake(1u, 1u, 1u);
            NLS::Render::RHI::RHIComputePipelineDesc m_desc{};
        };

        class MetalCommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
        {
        public:
            MetalCommandBuffer(id<MTLCommandQueue> queue, std::string debugName)
                : m_queue([queue retain])
                , m_debugName(std::move(debugName))
            {
            }

            ~MetalCommandBuffer() override
            {
                Reset();
                [m_queue release];
            }

            std::string_view GetDebugName() const override { return m_debugName; }

            void Begin() override
            {
                Reset();
                if (m_queue == nil)
                    return;

                m_commandBuffer = [[m_queue commandBuffer] retain];
                if (m_commandBuffer == nil)
                    return;
                if (!m_debugName.empty())
                    m_commandBuffer.label = [NSString stringWithUTF8String:m_debugName.c_str()];
                m_recording = true;
                m_closed = false;
            }

            void End() override
            {
                if (!m_recording)
                    return;
                EndActiveEncoders();
                m_recording = false;
                m_closed = m_commandBuffer != nil;
            }

            void Reset() override
            {
                EndActiveEncoders();
                [m_commandBuffer release];
                m_commandBuffer = nil;
                m_currentGraphicsPipeline.reset();
                m_currentComputePipeline.reset();
                m_currentPipelineLayout.reset();
                m_boundPushConstantRanges.clear();
                m_activeRenderPassTransitions.clear();
                m_pushConstantData.fill(0u);
                m_pushConstantDataSize = 0u;
                m_indexBuffer = nil;
                m_indexBufferOffset = 0u;
                m_activeRenderPassDebugName.clear();
                m_activeRenderPassDepthFormat = MTLPixelFormatInvalid;
                m_recording = false;
                m_closed = false;
                m_submitted = false;
            }

            bool IsRecording() const override { return m_recording; }
            NLS::Render::RHI::NativeHandle GetNativeCommandBuffer() const override
            {
                return { kMetalBackendType, (__bridge void*)m_commandBuffer };
            }

            void BeginRenderPass(const NLS::Render::RHI::RHIRenderPassDesc& desc) override
            {
                if (!m_recording || m_commandBuffer == nil)
                    return;
                EndActiveEncoders();
                if (desc.colorAttachments.empty() && !desc.depthStencilAttachment.has_value())
                    return;
                if (desc.colorAttachments.size() > 8u)
                {
                    NLS_LOG_ERROR("MetalCommandBuffer: render pass exceeds Metal's color attachment limit.");
                    return;
                }

                MTLRenderPassDescriptor* renderPass = [MTLRenderPassDescriptor renderPassDescriptor];
                m_activeRenderPassDebugName = desc.debugName;
                m_activeRenderPassDepthFormat = MTLPixelFormatInvalid;
                const size_t colorAttachmentCount = desc.colorAttachments.size();
                for (size_t index = 0u; index < colorAttachmentCount; ++index)
                {
                    const auto& source = desc.colorAttachments[index];
                    const auto view = std::dynamic_pointer_cast<MetalTextureView>(source.view);
                    if (view == nullptr || view->GetMetalTextureView() == nil)
                    {
                        NLS_LOG_ERROR("MetalCommandBuffer: render pass received a non-Metal color attachment.");
                        return;
                    }

                    MTLRenderPassColorAttachmentDescriptor* attachment = renderPass.colorAttachments[index];
                    attachment.texture = view->GetMetalTextureView();
                    attachment.loadAction = ToMetalLoadAction(source.loadOp);
                    attachment.storeAction = ToMetalStoreAction(source.storeOp);
                    attachment.clearColor = MTLClearColorMake(
                        source.clearValue.r,
                        source.clearValue.g,
                        source.clearValue.b,
                        source.clearValue.a);
                }

                if (desc.depthStencilAttachment.has_value())
                {
                    const auto& source = *desc.depthStencilAttachment;
                    const auto view = std::dynamic_pointer_cast<MetalTextureView>(source.view);
                    if (view == nullptr || view->GetMetalTextureView() == nil)
                    {
                        NLS_LOG_ERROR("MetalCommandBuffer: render pass received a non-Metal depth attachment.");
                        return;
                    }

                    renderPass.depthAttachment.texture = view->GetMetalTextureView();
                    m_activeRenderPassDepthFormat = view->GetMetalTextureView().pixelFormat;
                    renderPass.depthAttachment.loadAction = ToMetalLoadAction(source.depthLoadOp);
                    renderPass.depthAttachment.storeAction = ToMetalStoreAction(source.depthStoreOp);
                    renderPass.depthAttachment.clearDepth = source.clearValue.depth;
                    if (view->GetMetalTextureView().pixelFormat == MTLPixelFormatDepth32Float_Stencil8)
                    {
                        renderPass.stencilAttachment.texture = view->GetMetalTextureView();
                        renderPass.stencilAttachment.loadAction = ToMetalLoadAction(source.stencilLoadOp);
                        renderPass.stencilAttachment.storeAction = ToMetalStoreAction(source.stencilStoreOp);
                        renderPass.stencilAttachment.clearStencil = source.clearValue.stencil;
                    }
                }

                m_renderEncoder = [[m_commandBuffer renderCommandEncoderWithDescriptor:renderPass] retain];
                if (m_renderEncoder != nil && !desc.debugName.empty())
                    m_renderEncoder.label = [NSString stringWithUTF8String:desc.debugName.c_str()];
                if (m_renderEncoder == nil || desc.attachmentsRequireExternalStateTransitions)
                    return;

                const bool isBackbufferPass = desc.debugName == "BackbufferRenderPass";
                for (const auto& attachment : desc.colorAttachments)
                {
                    const auto view = std::dynamic_pointer_cast<MetalTextureView>(attachment.view);
                    const auto texture = view != nullptr
                        ? std::dynamic_pointer_cast<MetalTexture>(view->GetTexture())
                        : nullptr;
                    if (texture == nullptr)
                        continue;
                    const auto range = NLS::Render::RHI::NormalizeTextureSubresourceRange(
                        texture->GetDesc(),
                        view->GetDesc().subresourceRange);
                    const bool coversWholeTexture = range.has_value() &&
                        NLS::Render::RHI::IsFullTextureSubresourceRange(texture->GetDesc(), *range);
                    if (coversWholeTexture)
                        texture->SetState(NLS::Render::RHI::ResourceState::RenderTarget);
                    else
                        texture->MarkPartialStateDirty();
                    m_activeRenderPassTransitions.push_back({
                        texture,
                        isBackbufferPass
                            ? NLS::Render::RHI::ResourceState::Present
                            : NLS::Render::RHI::ResourceState::Unknown,
                        coversWholeTexture
                    });
                }
                if (desc.depthStencilAttachment.has_value())
                {
                    const auto& attachment = *desc.depthStencilAttachment;
                    const auto view = std::dynamic_pointer_cast<MetalTextureView>(attachment.view);
                    const auto texture = view != nullptr
                        ? std::dynamic_pointer_cast<MetalTexture>(view->GetTexture())
                        : nullptr;
                    if (texture != nullptr)
                    {
                        const auto range = NLS::Render::RHI::NormalizeTextureSubresourceRange(
                            texture->GetDesc(),
                            view->GetDesc().subresourceRange);
                        const bool coversWholeTexture = range.has_value() &&
                            NLS::Render::RHI::IsFullTextureSubresourceRange(texture->GetDesc(), *range);
                        if (coversWholeTexture)
                        {
                            texture->SetState(attachment.readOnlyDepthStencil
                                ? NLS::Render::RHI::ResourceState::DepthRead
                                : NLS::Render::RHI::ResourceState::DepthWrite);
                        }
                        else
                        {
                            texture->MarkPartialStateDirty();
                        }
                        m_activeRenderPassTransitions.push_back({
                            texture,
                            NLS::Render::RHI::ResourceState::Unknown,
                            coversWholeTexture
                        });
                    }
                }
            }

            void EndRenderPass() override
            {
                EndRenderEncoder();
            }

            void SetViewport(const NLS::Render::RHI::RHIViewport& viewport) override
            {
                if (m_renderEncoder == nil)
                    return;
                [m_renderEncoder setViewport:MTLViewport {
                    viewport.x,
                    viewport.y,
                    viewport.width,
                    viewport.height,
                    viewport.minDepth,
                    viewport.maxDepth
                }];
            }

            void SetScissor(const NLS::Render::RHI::RHIRect2D& rect) override
            {
                if (m_renderEncoder == nil || rect.x < 0 || rect.y < 0)
                    return;
                [m_renderEncoder setScissorRect:MTLScissorRect {
                    static_cast<NSUInteger>(rect.x),
                    static_cast<NSUInteger>(rect.y),
                    static_cast<NSUInteger>(rect.width),
                    static_cast<NSUInteger>(rect.height)
                }];
            }

            void BindGraphicsPipeline(
                const std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>& pipeline) override
            {
                m_currentGraphicsPipeline = std::dynamic_pointer_cast<MetalGraphicsPipeline>(pipeline);
                m_currentComputePipeline.reset();
                m_currentPipelineLayout.reset();
                m_boundPushConstantRanges.clear();
                if (m_renderEncoder == nil || m_currentGraphicsPipeline == nullptr)
                    return;

                if (const auto layout = std::dynamic_pointer_cast<MetalPipelineLayout>(
                        m_currentGraphicsPipeline->GetDesc().pipelineLayout);
                    layout != nullptr)
                {
                    m_currentPipelineLayout = layout;
                    m_boundPushConstantRanges = layout->GetDesc().pushConstants;
                }

                const auto& pipelineDesc = m_currentGraphicsPipeline->GetDesc();
                const MTLPixelFormat pipelineDepthFormat = pipelineDesc.renderTargetLayout.hasDepth
                    ? ToMetalPixelFormat(
                        pipelineDesc.renderTargetLayout.depthFormat,
                        NLS::Render::RHI::TextureColorSpace::Linear)
                    : MTLPixelFormatInvalid;
                if (pipelineDepthFormat != m_activeRenderPassDepthFormat)
                {
                    NLS_LOG_ERROR(
                        "MetalCommandBuffer: render-pass/pipeline depth attachment mismatch. pass=\"" +
                        m_activeRenderPassDebugName +
                        "\" pipeline=\"" + pipelineDesc.debugName +
                        "\" passFormat=" + std::to_string(static_cast<uint32_t>(m_activeRenderPassDepthFormat)) +
                        " pipelineFormat=" + std::to_string(static_cast<uint32_t>(pipelineDepthFormat)) + ".");
                }

                [m_renderEncoder setRenderPipelineState:m_currentGraphicsPipeline->GetPipelineState()];
                if (m_currentGraphicsPipeline->GetDepthStencilState() != nil)
                    [m_renderEncoder setDepthStencilState:m_currentGraphicsPipeline->GetDepthStencilState()];
                [m_renderEncoder setStencilReferenceValue:
                    m_currentGraphicsPipeline->GetDesc().depthStencilState.stencilReference];
                const auto& rasterState = m_currentGraphicsPipeline->GetDesc().rasterState;
                if (!rasterState.cullEnabled)
                {
                    [m_renderEncoder setCullMode:MTLCullModeNone];
                }
                else
                {
                    [m_renderEncoder setCullMode:rasterState.cullFace == NLS::Render::Settings::ECullFace::FRONT
                        ? MTLCullModeFront
                        : MTLCullModeBack];
                }
                [m_renderEncoder setTriangleFillMode:rasterState.wireframe
                    ? MTLTriangleFillModeLines
                    : MTLTriangleFillModeFill];
                [m_renderEncoder setFrontFacingWinding:MTLWindingCounterClockwise];
            }

            void BindComputePipeline(
                const std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>& pipeline) override
            {
                m_currentComputePipeline = std::dynamic_pointer_cast<MetalComputePipeline>(pipeline);
                m_currentGraphicsPipeline.reset();
                m_currentPipelineLayout.reset();
                m_boundPushConstantRanges.clear();
                if (!m_recording || m_commandBuffer == nil || m_currentComputePipeline == nullptr)
                    return;

                if (const auto layout = std::dynamic_pointer_cast<MetalPipelineLayout>(
                        m_currentComputePipeline->GetDesc().pipelineLayout);
                    layout != nullptr)
                {
                    m_currentPipelineLayout = layout;
                    m_boundPushConstantRanges = layout->GetDesc().pushConstants;
                }

                EndRenderEncoder();
                EndBlitEncoder();
                if (m_computeEncoder == nil)
                    m_computeEncoder = [[m_commandBuffer computeCommandEncoder] retain];
                if (m_computeEncoder == nil)
                {
                    NLS_LOG_ERROR("MetalCommandBuffer: failed to create compute encoder.");
                    return;
                }

                [m_computeEncoder setComputePipelineState:m_currentComputePipeline->GetPipelineState()];
            }

            void BindBindingSet(
                const uint32_t setIndex,
                const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& bindingSet) override
            {
                const bool bindCompute = m_computeEncoder != nil && m_renderEncoder == nil;
                if (m_renderEncoder == nil && !bindCompute)
                    return;
                auto currentBindingSet = bindingSet;
                std::shared_ptr<MetalBindingSet> metalSet;
                while (currentBindingSet != nullptr)
                {
                    metalSet = std::dynamic_pointer_cast<MetalBindingSet>(currentBindingSet);
                    if (metalSet != nullptr)
                        break;
                    currentBindingSet = currentBindingSet->GetWrappedBindingSetShared();
                }
                if (metalSet == nullptr)
                {
                    NLS_LOG_ERROR(
                        "MetalCommandBuffer::BindBindingSet rejected set " + std::to_string(setIndex) +
                        " because it has no native Metal binding set.");
                    return;
                }

                const auto& desc = metalSet->GetDesc();
                const auto actualLayout = std::dynamic_pointer_cast<MetalBindingLayout>(desc.layout);
                if (actualLayout == nullptr || m_currentPipelineLayout == nullptr)
                {
                    NLS_LOG_ERROR(
                        "MetalCommandBuffer::BindBindingSet rejected set " + std::to_string(setIndex) +
                        " because no compatible Metal pipeline layout is bound.");
                    return;
                }

                std::vector<const NLS::Render::RHI::RHIBindingLayoutEntry*> expectedEntries;
                for (const auto& pipelineBindingLayout : m_currentPipelineLayout->GetDesc().bindingLayouts)
                {
                    const auto metalLayout = std::dynamic_pointer_cast<MetalBindingLayout>(pipelineBindingLayout);
                    if (metalLayout == nullptr)
                        continue;
                    for (const auto& entry : metalLayout->GetDesc().entries)
                    {
                        if (entry.set == setIndex)
                            expectedEntries.push_back(&entry);
                    }
                }
                if (expectedEntries.empty())
                    return;

                const auto& actualLayoutEntries = actualLayout->GetDesc().entries;
                const auto actualMatchesExpected = [&expectedEntries, setIndex](
                    const NLS::Render::RHI::RHIBindingLayoutEntry& actual)
                {
                    if (actual.set != setIndex)
                        return true;
                    return std::any_of(
                        expectedEntries.begin(),
                        expectedEntries.end(),
                        [&actual](const auto* expected)
                        {
                            return expected != nullptr &&
                                AreMetalBindingLayoutEntriesCompatible(*expected, actual);
                        });
                };
                const bool expectedEntriesPresent = std::all_of(
                    expectedEntries.begin(),
                    expectedEntries.end(),
                    [&actualLayoutEntries](const auto* expected)
                    {
                        return expected != nullptr && std::any_of(
                            actualLayoutEntries.begin(),
                            actualLayoutEntries.end(),
                            [expected](const auto& actual)
                            {
                                return AreMetalBindingLayoutEntriesCompatible(*expected, actual);
                            });
                    });
                const bool actualEntriesCompatible = std::all_of(
                    actualLayoutEntries.begin(),
                    actualLayoutEntries.end(),
                    actualMatchesExpected);
                if (!expectedEntriesPresent || !actualEntriesCompatible)
                {
                    NLS_LOG_ERROR(
                        "MetalCommandBuffer::BindBindingSet rejected set " + std::to_string(setIndex) +
                        " because it is incompatible with the bound Metal pipeline layout.");
                    return;
                }

                for (const auto* layoutEntry : expectedEntries)
                {
                    const auto boundEntry = std::find_if(
                        desc.entries.begin(),
                        desc.entries.end(),
                        [layoutEntry](const NLS::Render::RHI::RHIBindingSetEntry& candidate)
                        {
                            return layoutEntry != nullptr &&
                                candidate.binding == layoutEntry->binding &&
                                candidate.type == layoutEntry->type;
                        });
                    const NLS::Render::RHI::RHIBindingSetEntry* entry = boundEntry != desc.entries.end()
                        ? &(*boundEntry)
                        : nullptr;
                    const auto stageMask = layoutEntry->stageMask;

                    const bool bindVertex = NLS::Render::RHI::HasShaderStage(
                        stageMask, NLS::Render::RHI::ShaderStageMask::Vertex);
                    const bool bindFragment = NLS::Render::RHI::HasShaderStage(
                        stageMask, NLS::Render::RHI::ShaderStageMask::Fragment);
                    const bool bindComputeStage = bindCompute && NLS::Render::RHI::HasShaderStage(
                        stageMask, NLS::Render::RHI::ShaderStageMask::Compute);
                    for (uint32_t descriptorIndex = 0u; descriptorIndex < layoutEntry->count; ++descriptorIndex)
                    {
                        const auto index = ToMetalBindingIndex(
                            layoutEntry->type,
                            layoutEntry->registerSpace,
                            layoutEntry->binding + descriptorIndex);
                        if (!index.has_value())
                            break;

                        switch (layoutEntry->type)
                        {
                        case NLS::Render::RHI::BindingType::UniformBuffer:
                        case NLS::Render::RHI::BindingType::StructuredBuffer:
                        case NLS::Render::RHI::BindingType::StorageBuffer:
                        {
                            const auto buffer = entry != nullptr
                                ? std::dynamic_pointer_cast<MetalBuffer>(entry->buffer)
                                : nullptr;
                            id<MTLBuffer> nativeBuffer = buffer != nullptr ? buffer->GetBuffer() : nil;
                            const uint64_t bufferOffset = buffer != nullptr ? entry->bufferOffset : 0u;
                            if (bindVertex)
                                [m_renderEncoder setVertexBuffer:nativeBuffer offset:bufferOffset atIndex:*index];
                            if (bindFragment)
                                [m_renderEncoder setFragmentBuffer:nativeBuffer offset:bufferOffset atIndex:*index];
                            if (bindComputeStage)
                                [m_computeEncoder setBuffer:nativeBuffer offset:bufferOffset atIndex:*index];
                            break;
                        }
                        case NLS::Render::RHI::BindingType::Texture:
                        case NLS::Render::RHI::BindingType::RWTexture:
                        {
                            const auto textureView = entry != nullptr
                                ? std::dynamic_pointer_cast<MetalTextureView>(entry->textureView)
                                : nullptr;
                            id<MTLTexture> nativeTexture = textureView != nullptr
                                ? textureView->GetMetalTextureView()
                                : nil;
                            if (bindVertex)
                                [m_renderEncoder setVertexTexture:nativeTexture atIndex:*index];
                            if (bindFragment)
                                [m_renderEncoder setFragmentTexture:nativeTexture atIndex:*index];
                            if (bindComputeStage)
                                [m_computeEncoder setTexture:nativeTexture atIndex:*index];
                            break;
                        }
                        case NLS::Render::RHI::BindingType::Sampler:
                        {
                            const auto sampler = entry != nullptr
                                ? std::dynamic_pointer_cast<MetalSampler>(entry->sampler)
                                : nullptr;
                            id<MTLSamplerState> nativeSampler = sampler != nullptr ? sampler->GetSampler() : nil;
                            if (bindVertex)
                                [m_renderEncoder setVertexSamplerState:nativeSampler atIndex:*index];
                            if (bindFragment)
                                [m_renderEncoder setFragmentSamplerState:nativeSampler atIndex:*index];
                            if (bindComputeStage)
                                [m_computeEncoder setSamplerState:nativeSampler atIndex:*index];
                            break;
                        }
                        }
                    }
                }
            }

            void PushConstants(
                const NLS::Render::RHI::ShaderStageMask stageMask,
                const uint32_t offset,
                const uint32_t size,
                const void* data) override
            {
                using namespace NLS::Render::RHI;
                if ((m_renderEncoder == nil && m_computeEncoder == nil) || data == nullptr || size == 0u ||
                    (size % sizeof(uint32_t)) != 0u || (offset % sizeof(uint32_t)) != 0u ||
                    offset > kRHIMaxPushConstantBytes || size > kRHIMaxPushConstantBytes - offset)
                    return;

                const uint64_t writeBegin = offset;
                const uint64_t writeEnd = writeBegin + size;
                const auto rangeIt = std::find_if(
                    m_boundPushConstantRanges.begin(),
                    m_boundPushConstantRanges.end(),
                    [stageMask, writeBegin, writeEnd](const RHIPushConstantRange& range)
                    {
                        if ((range.stageMask & stageMask) == ShaderStageMask::None)
                            return false;
                        const uint64_t rangeBegin = range.offset;
                        const uint64_t rangeEnd = rangeBegin + range.size;
                        return writeBegin >= rangeBegin && writeEnd <= rangeEnd;
                    });
                if (rangeIt == m_boundPushConstantRanges.end())
                    return;

                const auto bufferIndex = ToMetalBufferBindingIndex(
                    rangeIt->registerSpace,
                    rangeIt->shaderRegister);
                if (!bufferIndex.has_value())
                    return;

                std::memcpy(m_pushConstantData.data() + offset, data, size);
                m_pushConstantDataSize = (std::max)(m_pushConstantDataSize, offset + size);
                const uint32_t rangeDataEnd = (std::min)(
                    m_pushConstantDataSize,
                    rangeIt->offset + rangeIt->size);
                const uint32_t rangeDataSize = rangeDataEnd - rangeIt->offset;
                const uint8_t* rangeData = m_pushConstantData.data() + rangeIt->offset;
                const ShaderStageMask effectiveStageMask = rangeIt->stageMask & stageMask;
                if (m_renderEncoder != nil && HasShaderStage(effectiveStageMask, ShaderStageMask::Vertex))
                {
                    [m_renderEncoder setVertexBytes:rangeData
                                            length:rangeDataSize
                                           atIndex:*bufferIndex];
                }
                if (m_renderEncoder != nil && HasShaderStage(effectiveStageMask, ShaderStageMask::Fragment))
                {
                    [m_renderEncoder setFragmentBytes:rangeData
                                              length:rangeDataSize
                                             atIndex:*bufferIndex];
                }
                if (m_computeEncoder != nil && HasShaderStage(effectiveStageMask, ShaderStageMask::Compute))
                {
                    [m_computeEncoder setBytes:rangeData
                                        length:rangeDataSize
                                       atIndex:*bufferIndex];
                }
            }

            void BindVertexBuffer(const uint32_t slot, const NLS::Render::RHI::RHIVertexBufferView& view) override
            {
                if (m_renderEncoder == nil || slot >= 8u)
                    return;
                const auto buffer = std::dynamic_pointer_cast<MetalBuffer>(view.buffer);
                if (buffer != nullptr)
                    [m_renderEncoder setVertexBuffer:buffer->GetBuffer() offset:view.offset atIndex:slot];
            }

            void BindIndexBuffer(const NLS::Render::RHI::RHIIndexBufferView& view) override
            {
                const auto buffer = std::dynamic_pointer_cast<MetalBuffer>(view.buffer);
                m_indexBuffer = buffer != nullptr ? buffer->GetBuffer() : nil;
                m_indexBufferOffset = view.offset;
                m_indexType = view.indexType;
            }

            void Draw(
                const uint32_t vertexCount,
                const uint32_t instanceCount,
                const uint32_t firstVertex,
                const uint32_t firstInstance) override
            {
                if (!CanDraw())
                    return;
                [m_renderEncoder drawPrimitives:ToMetalPrimitiveType(m_currentGraphicsPipeline->GetDesc().primitiveTopology)
                                     vertexStart:firstVertex
                                     vertexCount:vertexCount
                                   instanceCount:instanceCount
                                    baseInstance:firstInstance];
            }

            void DrawIndexed(
                const uint32_t indexCount,
                const uint32_t instanceCount,
                const uint32_t firstIndex,
                const int32_t vertexOffset,
                const uint32_t firstInstance) override
            {
                if (!CanDraw() || m_indexBuffer == nil)
                    return;
                const NSUInteger indexSize = m_indexType == NLS::Render::RHI::IndexType::UInt16 ? 2u : 4u;
                [m_renderEncoder drawIndexedPrimitives:ToMetalPrimitiveType(m_currentGraphicsPipeline->GetDesc().primitiveTopology)
                                         indexCount:indexCount
                                          indexType:m_indexType == NLS::Render::RHI::IndexType::UInt16
                                              ? MTLIndexTypeUInt16
                                              : MTLIndexTypeUInt32
                                        indexBuffer:m_indexBuffer
                                  indexBufferOffset:m_indexBufferOffset + firstIndex * indexSize
                                      instanceCount:instanceCount
                                         baseVertex:vertexOffset
                                       baseInstance:firstInstance];
            }

            NLS::Render::RHI::RHICommandRecordingResult DrawChecked(
                const uint32_t vertexCount,
                const uint32_t instanceCount,
                const uint32_t firstVertex,
                const uint32_t firstInstance) override
            {
                if (!CanDraw())
                {
                    return {
                        NLS::Render::RHI::RHICommandRecordingStatusCode::InvalidArgument,
                        "Metal draw requires an active render pass and graphics pipeline."
                    };
                }
                Draw(vertexCount, instanceCount, firstVertex, firstInstance);
                return {};
            }

            NLS::Render::RHI::RHICommandRecordingResult DrawIndexedChecked(
                const uint32_t indexCount,
                const uint32_t instanceCount,
                const uint32_t firstIndex,
                const int32_t vertexOffset,
                const uint32_t firstInstance) override
            {
                if (!CanDraw() || m_indexBuffer == nil)
                {
                    return {
                        NLS::Render::RHI::RHICommandRecordingStatusCode::InvalidArgument,
                        "Metal indexed draw requires an active render pass, graphics pipeline, and index buffer."
                    };
                }
                DrawIndexed(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
                return {};
            }

            void Dispatch(
                const uint32_t groupCountX,
                const uint32_t groupCountY,
                const uint32_t groupCountZ) override
            {
                if (m_computeEncoder == nil || m_currentComputePipeline == nullptr ||
                    m_currentComputePipeline->GetPipelineState() == nil)
                    return;

                const MTLSize threadgroups = MTLSizeMake(groupCountX, groupCountY, groupCountZ);
                [m_computeEncoder dispatchThreadgroups:threadgroups
                                  threadsPerThreadgroup:m_currentComputePipeline->GetThreadgroupSize()];
            }

            void CopyBuffer(
                const std::shared_ptr<NLS::Render::RHI::RHIBuffer>& source,
                const std::shared_ptr<NLS::Render::RHI::RHIBuffer>& destination,
                const NLS::Render::RHI::RHIBufferCopyRegion& region) override
            {
                const auto sourceBuffer = std::dynamic_pointer_cast<MetalBuffer>(source);
                const auto destinationBuffer = std::dynamic_pointer_cast<MetalBuffer>(destination);
                if (sourceBuffer == nullptr || destinationBuffer == nullptr)
                {
                    NLS_LOG_ERROR("MetalCommandBuffer::CopyBuffer requires Metal source and destination resources.");
                    return;
                }
                std::string validationReason;
                if (!ValidateMetalBufferCopyRegion(
                        region,
                        sourceBuffer->GetDesc(),
                        destinationBuffer->GetDesc(),
                        validationReason))
                {
                    NLS_LOG_ERROR(
                        "MetalCommandBuffer::CopyBuffer rejected invalid copy: " +
                        validationReason);
                    return;
                }

                id<MTLBlitCommandEncoder> encoder = BeginBlitEncoder();
                if (encoder == nil)
                {
                    NLS_LOG_ERROR("MetalCommandBuffer::CopyBuffer requires a recording command buffer outside a render pass.");
                    return;
                }
                [encoder copyFromBuffer:sourceBuffer->GetBuffer()
                           sourceOffset:region.srcOffset
                               toBuffer:destinationBuffer->GetBuffer()
                      destinationOffset:region.dstOffset
                                    size:region.size];
            }

            void CopyBufferToTexture(const NLS::Render::RHI::RHIBufferToTextureCopyDesc& desc) override
            {
                const auto source = std::dynamic_pointer_cast<MetalBuffer>(desc.source);
                const auto destination = std::dynamic_pointer_cast<MetalTexture>(desc.destination);
                if (source == nullptr || destination == nullptr)
                {
                    NLS_LOG_ERROR("MetalCommandBuffer::CopyBufferToTexture requires Metal source and destination resources.");
                    return;
                }

                uint32_t rowPitch = 0u;
                uint32_t slicePitch = 0u;
                std::string validationReason;
                if (!ResolveMetalBufferToTextureCopyLayout(
                        desc,
                        source->GetDesc(),
                        destination->GetDesc(),
                        rowPitch,
                        slicePitch,
                        validationReason))
                {
                    NLS_LOG_ERROR(
                        "MetalCommandBuffer::CopyBufferToTexture rejected invalid copy: " +
                        validationReason);
                    return;
                }

                id<MTLBlitCommandEncoder> encoder = BeginBlitEncoder();
                if (encoder == nil)
                {
                    NLS_LOG_ERROR("MetalCommandBuffer::CopyBufferToTexture requires a recording command buffer outside a render pass.");
                    return;
                }
                [encoder copyFromBuffer:source->GetBuffer()
                           sourceOffset:desc.bufferOffset
                      sourceBytesPerRow:rowPitch
                    sourceBytesPerImage:slicePitch
                             sourceSize:MTLSizeMake(desc.extent.width, desc.extent.height, desc.extent.depth)
                              toTexture:destination->GetTexture()
                       destinationSlice:desc.arrayLayer
                       destinationLevel:desc.mipLevel
                      destinationOrigin:MTLOriginMake(desc.textureOffset.x, desc.textureOffset.y, desc.textureOffset.z)];
            }

            void CopyTexture(const NLS::Render::RHI::RHITextureCopyDesc& desc) override
            {
                const auto source = std::dynamic_pointer_cast<MetalTexture>(desc.source);
                const auto destination = std::dynamic_pointer_cast<MetalTexture>(desc.destination);
                if (source == nullptr || destination == nullptr)
                {
                    NLS_LOG_ERROR("MetalCommandBuffer::CopyTexture requires Metal source and destination resources.");
                    return;
                }
                std::string validationReason;
                if (!ValidateMetalTextureCopyDesc(
                        desc,
                        source->GetDesc(),
                        destination->GetDesc(),
                        validationReason))
                {
                    NLS_LOG_ERROR(
                        "MetalCommandBuffer::CopyTexture rejected invalid copy: " +
                        validationReason);
                    return;
                }

                id<MTLBlitCommandEncoder> encoder = BeginBlitEncoder();
                if (encoder == nil)
                {
                    NLS_LOG_ERROR("MetalCommandBuffer::CopyTexture requires a recording command buffer outside a render pass.");
                    return;
                }
                [encoder copyFromTexture:source->GetTexture()
                             sourceSlice:desc.sourceRange.baseArrayLayer
                             sourceLevel:desc.sourceRange.baseMipLevel
                            sourceOrigin:MTLOriginMake(desc.sourceOffset.x, desc.sourceOffset.y, desc.sourceOffset.z)
                              sourceSize:MTLSizeMake(desc.extent.width, desc.extent.height, desc.extent.depth)
                               toTexture:destination->GetTexture()
                        destinationSlice:desc.destinationRange.baseArrayLayer
                        destinationLevel:desc.destinationRange.baseMipLevel
                       destinationOrigin:MTLOriginMake(desc.destinationOffset.x, desc.destinationOffset.y, desc.destinationOffset.z)];
            }

            NLS::Render::RHI::RHICommandRecordingResult BarrierChecked(
                const NLS::Render::RHI::RHIBarrierDesc& desc) override
            {
                if (!m_recording || m_commandBuffer == nil)
                {
                    return {
                        NLS::Render::RHI::RHICommandRecordingStatusCode::InvalidArgument,
                        "Metal barrier requires a recording command buffer."
                    };
                }

                std::vector<std::pair<std::shared_ptr<MetalBuffer>, NLS::Render::RHI::ResourceState>> buffersToTransition;
                buffersToTransition.reserve(desc.bufferBarriers.size());
                for (const auto& barrier : desc.bufferBarriers)
                {
                    const auto buffer = std::dynamic_pointer_cast<MetalBuffer>(barrier.buffer);
                    if (buffer == nullptr)
                    {
                        return {
                            NLS::Render::RHI::RHICommandRecordingStatusCode::InvalidArgument,
                            "Metal barrier received a null or non-Metal buffer."
                        };
                    }
                    if (buffer->GetDesc().memoryUsage != NLS::Render::RHI::MemoryUsage::GPUOnly)
                    {
                        const auto effectiveBefore = barrier.before == NLS::Render::RHI::ResourceState::Unknown
                            ? buffer->GetState()
                            : barrier.before;
                        if (!IsLegalMetalCpuVisibleBufferState(*buffer, effectiveBefore) ||
                            !IsLegalMetalCpuVisibleBufferState(*buffer, barrier.after))
                        {
                            return {
                                NLS::Render::RHI::RHICommandRecordingStatusCode::InvalidArgument,
                                "Metal barrier rejected an illegal CPU-visible buffer state transition."
                            };
                        }
                        continue;
                    }
                    buffersToTransition.emplace_back(buffer, barrier.after);
                }
                std::vector<std::pair<std::shared_ptr<MetalTexture>, bool>> texturesToTransition;
                texturesToTransition.reserve(desc.textureBarriers.size());
                for (const auto& barrier : desc.textureBarriers)
                {
                    const auto texture = std::dynamic_pointer_cast<MetalTexture>(barrier.texture);
                    if (texture == nullptr)
                    {
                        return {
                            NLS::Render::RHI::RHICommandRecordingStatusCode::InvalidArgument,
                            "Metal barrier received a null or non-Metal texture."
                        };
                    }
                    const auto normalizedRange = NLS::Render::RHI::NormalizeTextureSubresourceRange(
                        texture->GetDesc(),
                        barrier.subresourceRange);
                    if (!normalizedRange.has_value())
                    {
                        return {
                            NLS::Render::RHI::RHICommandRecordingStatusCode::InvalidArgument,
                            "Metal barrier received an invalid texture subresource range."
                        };
                    }
                    texturesToTransition.emplace_back(
                        texture,
                        NLS::Render::RHI::IsFullTextureSubresourceRange(texture->GetDesc(), *normalizedRange));
                }

                if (desc.bufferBarriers.empty() && desc.textureBarriers.empty())
                    return {};

                MTLBarrierScope scope = static_cast<MTLBarrierScope>(0u);
                if (!desc.bufferBarriers.empty())
                    scope |= MTLBarrierScopeBuffers;
                if (!desc.textureBarriers.empty())
                    scope |= MTLBarrierScopeTextures;

                if (m_computeEncoder != nil)
                {
                    [m_computeEncoder memoryBarrierWithScope:scope];
                }
                else if (m_renderEncoder != nil)
                {
                    constexpr MTLRenderStages kAllGraphicsStages =
                        MTLRenderStageVertex | MTLRenderStageFragment;
                    [m_renderEncoder memoryBarrierWithScope:scope
                                               afterStages:kAllGraphicsStages
                                              beforeStages:kAllGraphicsStages];
                }
                else if (m_blitEncoder != nil)
                {
                    // A blit encoder has no in-encoder memory barrier API. Closing it creates
                    // the required ordering point before a later render or compute encoder.
                    EndBlitEncoder();
                }
                for (size_t index = 0u; index < buffersToTransition.size(); ++index)
                    buffersToTransition[index].first->SetState(buffersToTransition[index].second);
                for (size_t index = 0u; index < texturesToTransition.size(); ++index)
                {
                    if (texturesToTransition[index].second)
                        texturesToTransition[index].first->SetState(desc.textureBarriers[index].after);
                    else
                        texturesToTransition[index].first->MarkPartialStateDirty();
                }
                return {};
            }

            void Barrier(const NLS::Render::RHI::RHIBarrierDesc& desc) override
            {
                (void)BarrierChecked(desc);
            }
            bool IsClosedForSubmission() const override { return m_closed && !m_submitted; }
            id<MTLCommandBuffer> GetMetalCommandBuffer() const { return m_commandBuffer; }
            void MarkSubmitted() { m_submitted = true; }

        private:
            bool CanDraw() const
            {
                return m_renderEncoder != nil && m_currentGraphicsPipeline != nullptr &&
                    m_currentGraphicsPipeline->GetPipelineState() != nil;
            }

            id<MTLBlitCommandEncoder> BeginBlitEncoder()
            {
                if (!m_recording || m_commandBuffer == nil || m_renderEncoder != nil)
                    return nil;
                EndComputeEncoder();
                if (m_blitEncoder == nil)
                    m_blitEncoder = [[m_commandBuffer blitCommandEncoder] retain];
                return m_blitEncoder;
            }

            void EndRenderEncoder()
            {
                if (m_renderEncoder != nil)
                {
                    [m_renderEncoder endEncoding];
                    [m_renderEncoder release];
                    m_renderEncoder = nil;
                }
                for (const auto& transition : m_activeRenderPassTransitions)
                {
                    if (transition.texture == nullptr)
                        continue;
                    if (transition.coversWholeTexture)
                        transition.texture->SetState(transition.stateAfterEnd);
                    else
                        transition.texture->MarkPartialStateDirty();
                }
                m_activeRenderPassTransitions.clear();
                m_activeRenderPassDebugName.clear();
                m_activeRenderPassDepthFormat = MTLPixelFormatInvalid;
            }

            void EndBlitEncoder()
            {
                if (m_blitEncoder != nil)
                {
                    [m_blitEncoder endEncoding];
                    [m_blitEncoder release];
                    m_blitEncoder = nil;
                }
            }

            void EndComputeEncoder()
            {
                if (m_computeEncoder != nil)
                {
                    [m_computeEncoder endEncoding];
                    [m_computeEncoder release];
                    m_computeEncoder = nil;
                }
            }

            void EndActiveEncoders()
            {
                EndRenderEncoder();
                EndBlitEncoder();
                EndComputeEncoder();
            }

            id<MTLCommandQueue> m_queue = nil;
            id<MTLCommandBuffer> m_commandBuffer = nil;
            id<MTLRenderCommandEncoder> m_renderEncoder = nil;
            id<MTLBlitCommandEncoder> m_blitEncoder = nil;
            id<MTLComputeCommandEncoder> m_computeEncoder = nil;
            struct ActiveRenderPassTransition
            {
                std::shared_ptr<MetalTexture> texture;
                NLS::Render::RHI::ResourceState stateAfterEnd = NLS::Render::RHI::ResourceState::Unknown;
                bool coversWholeTexture = true;
            };
            std::vector<ActiveRenderPassTransition> m_activeRenderPassTransitions;
            std::string m_activeRenderPassDebugName;
            MTLPixelFormat m_activeRenderPassDepthFormat = MTLPixelFormatInvalid;
            std::shared_ptr<MetalGraphicsPipeline> m_currentGraphicsPipeline;
            std::shared_ptr<MetalComputePipeline> m_currentComputePipeline;
            std::shared_ptr<MetalPipelineLayout> m_currentPipelineLayout;
            std::vector<NLS::Render::RHI::RHIPushConstantRange> m_boundPushConstantRanges;
            std::array<uint8_t, NLS::Render::RHI::kRHIMaxPushConstantBytes> m_pushConstantData{};
            uint32_t m_pushConstantDataSize = 0u;
            id<MTLBuffer> m_indexBuffer = nil;
            uint64_t m_indexBufferOffset = 0u;
            NLS::Render::RHI::IndexType m_indexType = NLS::Render::RHI::IndexType::UInt32;
            std::string m_debugName;
            bool m_recording = false;
            bool m_closed = false;
            bool m_submitted = false;
        };

        class MetalCommandPool final : public NLS::Render::RHI::RHICommandPool
        {
        public:
            MetalCommandPool(id<MTLCommandQueue> queue, NLS::Render::RHI::QueueType queueType, std::string debugName)
                : m_queue([queue retain])
                , m_queueType(queueType)
                , m_debugName(std::move(debugName))
            {
            }

            ~MetalCommandPool() override
            {
                [m_queue release];
            }

            std::string_view GetDebugName() const override { return m_debugName; }
            NLS::Render::RHI::QueueType GetQueueType() const override { return m_queueType; }
            std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> CreateCommandBuffer(std::string debugName) override
            {
                if (debugName.empty())
                    debugName = m_debugName + ".CommandBuffer";
                return std::make_shared<MetalCommandBuffer>(m_queue, std::move(debugName));
            }
            void Reset() override {}

        private:
            id<MTLCommandQueue> m_queue = nil;
            NLS::Render::RHI::QueueType m_queueType;
            std::string m_debugName;
        };

        class MetalSwapchain final : public NLS::Render::RHI::RHISwapchain
        {
        public:
            MetalSwapchain(id<MTLDevice> device, NLS::Render::RHI::SwapchainDesc desc)
                : m_device([device retain])
                , m_desc(std::move(desc))
            {
                m_imageCount = ResolveMetalSwapchainImageCount(m_desc.imageCount);
                m_desc.imageCount = m_imageCount;
                m_desc.allowTearing = false;

                auto* glfwWindow = static_cast<GLFWwindow*>(m_desc.platformWindow);
                NSWindow* window = glfwWindow != nullptr ? glfwGetCocoaWindow(glfwWindow) : nil;
                if (window == nil || window.contentView == nil)
                    return;

                m_nativeWindowHandle = (__bridge void*)window;
                m_layer = [[CAMetalLayer alloc] init];
                m_layer.device = m_device;
                m_layer.pixelFormat = MTLPixelFormatRGBA8Unorm;
                m_layer.framebufferOnly = YES;
                m_layer.displaySyncEnabled = m_desc.vsync;
                m_layer.maximumDrawableCount = m_imageCount;

                NSView* view = window.contentView;
                view.wantsLayer = YES;
                view.layer = m_layer;
                UpdateDrawableSize(m_desc.width, m_desc.height);
            }

            ~MetalSwapchain() override
            {
                ReleaseCurrentDrawable();
                [m_layer release];
                [m_device release];
            }

            std::string_view GetDebugName() const override { return "MetalSwapchain"; }
            const NLS::Render::RHI::SwapchainDesc& GetDesc() const override { return m_desc; }
            uint32_t GetImageCount() const override { return m_imageCount; }
            std::optional<NLS::Render::RHI::RHIAcquiredImage> AcquireNextImage(
                const std::shared_ptr<NLS::Render::RHI::RHISemaphore>& signalSemaphore,
                const std::shared_ptr<NLS::Render::RHI::RHIFence>& signalFence) override
            {
                if (m_layer == nil)
                    return std::nullopt;

                if (m_drawable != nil)
                {
                    NLS_LOG_ERROR("MetalSwapchain::AcquireNextImage called while a drawable is still acquired.");
                    return std::nullopt;
                }
                id<CAMetalDrawable> drawable = [m_layer nextDrawable];
                if (drawable == nil)
                    return std::nullopt;

                m_drawable = [drawable retain];
                m_currentImageIndex = m_nextImageIndex;
                m_nextImageIndex = (m_nextImageIndex + 1u) % m_imageCount;
                NLS::Render::RHI::RHITextureDesc textureDesc{};
                textureDesc.extent = {
                    static_cast<uint32_t>(m_drawable.texture.width),
                    static_cast<uint32_t>(m_drawable.texture.height),
                    1u
                };
                textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
                textureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
                textureDesc.usage = NLS::Render::RHI::TextureUsageFlags::ColorAttachment |
                    NLS::Render::RHI::TextureUsageFlags::Present;
                textureDesc.debugName = "MetalSwapchainDrawable";
                m_backbufferTexture = std::make_shared<MetalTexture>(
                    m_drawable.texture,
                    std::move(textureDesc),
                    NLS::Render::RHI::ResourceState::Present);

                NLS::Render::RHI::RHITextureViewDesc viewDesc{};
                viewDesc.viewType = NLS::Render::RHI::TextureViewType::Texture2D;
                viewDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
                viewDesc.debugName = "MetalSwapchainDrawableView";
                m_backbufferView = std::make_shared<MetalTextureView>(
                    m_backbufferTexture,
                    std::move(viewDesc),
                    m_drawable.texture);
                if (const auto semaphore = std::dynamic_pointer_cast<MetalSemaphore>(signalSemaphore); semaphore != nullptr)
                    semaphore->SignalOnCpu();
                if (const auto fence = std::dynamic_pointer_cast<MetalFence>(signalFence); fence != nullptr)
                    fence->Signal();
                return NLS::Render::RHI::RHIAcquiredImage { *m_currentImageIndex, m_backbufferView, false };
            }
            std::shared_ptr<NLS::Render::RHI::RHITextureView> GetBackbufferView(uint32_t index) override
            {
                if (!m_currentImageIndex.has_value() || index != *m_currentImageIndex)
                    return nullptr;
                return m_backbufferView;
            }
            bool Resize(uint32_t width, uint32_t height) override
            {
                if (width == 0u || height == 0u || m_layer == nil)
                    return false;
                m_desc.width = width;
                m_desc.height = height;
                UpdateDrawableSize(width, height);
                return true;
            }
            NLS::Render::RHI::NativeHandle GetNativeSwapchainHandle() override
            {
                return { kMetalBackendType, (__bridge void*)m_layer };
            }
            void* GetNativeWindowHandle() const { return m_nativeWindowHandle; }

            bool Present(
                id<MTLCommandQueue> queue,
                const uint32_t imageIndex,
                const std::vector<std::shared_ptr<MetalSemaphore>>& waitSemaphores,
                std::shared_ptr<MetalFence> signalFence)
            {
                if (queue == nil || m_drawable == nil ||
                    !m_currentImageIndex.has_value() || imageIndex != *m_currentImageIndex)
                    return false;

                id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
                if (commandBuffer == nil)
                    return false;
                for (const auto& semaphore : waitSemaphores)
                {
                    if (semaphore == nullptr || semaphore->GetEvent() == nil || semaphore->GetWaitValue() == 0u)
                        return false;
                    [commandBuffer encodeWaitForEvent:semaphore->GetEvent()
                                               value:semaphore->GetWaitValue()];
                }
                [commandBuffer presentDrawable:m_drawable];
                if (signalFence != nullptr)
                {
                    const auto completionFence = std::move(signalFence);
                    completionFence->Reset();
                    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer>)
                    {
                        completionFence->Signal();
                    }];
                }
                [commandBuffer commit];
                ReleaseCurrentDrawable();
                return true;
            }

        private:
            void UpdateDrawableSize(uint32_t fallbackWidth, uint32_t fallbackHeight)
            {
                if (m_layer == nil)
                    return;

                uint32_t width = fallbackWidth;
                uint32_t height = fallbackHeight;
                if (auto* glfwWindow = static_cast<GLFWwindow*>(m_desc.platformWindow); glfwWindow != nullptr)
                {
                    int framebufferWidth = 0;
                    int framebufferHeight = 0;
                    glfwGetFramebufferSize(glfwWindow, &framebufferWidth, &framebufferHeight);
                    if (framebufferWidth > 0 && framebufferHeight > 0)
                    {
                        width = static_cast<uint32_t>(framebufferWidth);
                        height = static_cast<uint32_t>(framebufferHeight);
                    }
                }

                if (width > 0u && height > 0u)
                    m_layer.drawableSize = CGSizeMake(width, height);
            }

            void ReleaseCurrentDrawable()
            {
                m_backbufferView.reset();
                m_backbufferTexture.reset();
                [m_drawable release];
                m_drawable = nil;
                m_currentImageIndex.reset();
            }

            id<MTLDevice> m_device = nil;
            CAMetalLayer* m_layer = nil;
            id<CAMetalDrawable> m_drawable = nil;
            NLS::Render::RHI::SwapchainDesc m_desc{};
            uint32_t m_imageCount = 0u;
            uint32_t m_nextImageIndex = 0u;
            std::optional<uint32_t> m_currentImageIndex;
            void* m_nativeWindowHandle = nullptr;
            std::shared_ptr<NLS::Render::RHI::RHITexture> m_backbufferTexture;
            std::shared_ptr<NLS::Render::RHI::RHITextureView> m_backbufferView;
        };

        class MetalQueue final : public NLS::Render::RHI::RHIQueue
        {
        public:
            MetalQueue(id<MTLCommandQueue> queue, NLS::Render::RHI::QueueType queueType, std::string debugName)
                : m_queue([queue retain])
                , m_queueType(queueType)
                , m_debugName(std::move(debugName))
            {
            }

            ~MetalQueue() override
            {
                [m_queue release];
            }

            std::string_view GetDebugName() const override { return m_debugName; }
            NLS::Render::RHI::QueueType GetType() const override { return m_queueType; }
            void Submit(const NLS::Render::RHI::RHISubmitDesc& submitDesc) override
            {
                (void)SubmitChecked(submitDesc);
            }
            void Present(const NLS::Render::RHI::RHIPresentDesc& presentDesc) override
            {
                (void)PresentChecked(presentDesc);
            }
            NLS::Render::RHI::RHIQueueOperationResult SubmitChecked(
                const NLS::Render::RHI::RHISubmitDesc& submitDesc) override
            {
                std::vector<std::pair<std::shared_ptr<MetalSemaphore>, uint64_t>> waitSemaphores;
                waitSemaphores.reserve(submitDesc.waitSemaphores.size());
                for (const auto& waitSemaphore : submitDesc.waitSemaphores)
                {
                    const auto semaphore = std::dynamic_pointer_cast<MetalSemaphore>(waitSemaphore);
                    if (semaphore == nullptr || !semaphore->IsValid())
                    {
                        return {
                            NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument,
                            "Metal queue received a semaphore from another backend."
                        };
                    }
                    const uint64_t waitValue = semaphore->GetWaitValue();
                    if (waitValue == 0u)
                    {
                        return {
                            NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument,
                            "Metal queue cannot wait on an unscheduled semaphore value."
                        };
                    }
                    waitSemaphores.emplace_back(semaphore, waitValue);
                }

                std::vector<std::shared_ptr<MetalCommandBuffer>> commandBuffers;
                commandBuffers.reserve(submitDesc.commandBuffers.size());
                for (const auto& commandBuffer : submitDesc.commandBuffers)
                {
                    const auto metalCommandBuffer = std::dynamic_pointer_cast<MetalCommandBuffer>(commandBuffer);
                    if (metalCommandBuffer == nullptr)
                    {
                        return {
                            NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument,
                            "Metal queue received a command buffer from another backend."
                        };
                    }
                    if (!metalCommandBuffer->IsClosedForSubmission() || metalCommandBuffer->GetMetalCommandBuffer() == nil)
                    {
                        return {
                            NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument,
                            "Metal command buffer must be ended before queue submission."
                        };
                    }
                    commandBuffers.push_back(metalCommandBuffer);
                }

                const auto signalFence = std::dynamic_pointer_cast<MetalFence>(submitDesc.signalFence);
                if (submitDesc.signalFence != nullptr && signalFence == nullptr)
                {
                    return {
                        NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument,
                        "Metal queue received a fence from another backend."
                    };
                }

                std::vector<std::pair<std::shared_ptr<MetalSemaphore>, uint64_t>> signalSemaphores;
                signalSemaphores.reserve(submitDesc.signalSemaphores.size());
                for (const auto& signalSemaphore : submitDesc.signalSemaphores)
                {
                    const auto semaphore = std::dynamic_pointer_cast<MetalSemaphore>(signalSemaphore);
                    if (semaphore == nullptr || !semaphore->IsValid())
                    {
                        return {
                            NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument,
                            "Metal queue received a signal semaphore from another backend."
                        };
                    }
                    signalSemaphores.emplace_back(semaphore, semaphore->ReserveSignalValue());
                }

                if (signalFence != nullptr)
                    signalFence->Reset();

                id<MTLCommandBuffer> waitCommands = nil;
                if (!waitSemaphores.empty())
                {
                    waitCommands = [m_queue commandBuffer];
                    if (waitCommands == nil)
                    {
                        return {
                            NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure,
                            "Metal queue failed to create GPU semaphore wait commands."
                        };
                    }
                    for (const auto& [semaphore, value] : waitSemaphores)
                        [waitCommands encodeWaitForEvent:semaphore->GetEvent() value:value];
                }

                id<MTLCommandBuffer> completionCommands = nil;
                if (!signalSemaphores.empty() || signalFence != nullptr)
                {
                    completionCommands = [m_queue commandBuffer];
                    if (completionCommands == nil)
                    {
                        return {
                            NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure,
                            "Metal queue failed to create GPU semaphore signal commands."
                        };
                    }
                    for (const auto& [semaphore, value] : signalSemaphores)
                        [completionCommands encodeSignalEvent:semaphore->GetEvent() value:value];
                    if (signalFence != nullptr)
                    {
                        const auto completionFence = signalFence;
                        [completionCommands addCompletedHandler:^(id<MTLCommandBuffer>)
                        {
                            completionFence->Signal();
                        }];
                    }
                }

                if (waitCommands != nil)
                    [waitCommands commit];

                for (const auto& commandBuffer : commandBuffers)
                {
                    id<MTLCommandBuffer> nativeCommandBuffer = commandBuffer->GetMetalCommandBuffer();
                    [nativeCommandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completedCommandBuffer)
                    {
                        if (completedCommandBuffer.status == MTLCommandBufferStatusError)
                        {
                            const char* message = completedCommandBuffer.error.localizedDescription.UTF8String;
                            NLS_LOG_ERROR("Metal command buffer failed: " +
                                std::string(message != nullptr ? message : "unknown error"));
                        }
                    }];
                    [nativeCommandBuffer commit];
                    commandBuffer->MarkSubmitted();
                }
                if (completionCommands != nil)
                    [completionCommands commit];

                NLS::Render::RHI::RHIQueueOperationResult result{};
                result.mayHaveQueuedGpuWork = waitCommands != nil || !commandBuffers.empty() || completionCommands != nil;
                result.frameFenceSignalQueued = signalFence != nullptr && completionCommands != nil;
                return result;
            }
            NLS::Render::RHI::RHIQueueOperationResult PresentChecked(
                const NLS::Render::RHI::RHIPresentDesc& presentDesc) override
            {
                const auto swapchain = std::dynamic_pointer_cast<MetalSwapchain>(presentDesc.swapchain);
                if (swapchain == nullptr)
                {
                    return {
                        NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument,
                        "Metal queue received a swapchain from another backend."
                    };
                }
                const auto signalFence = std::dynamic_pointer_cast<MetalFence>(presentDesc.signalFence);
                if (presentDesc.signalFence != nullptr && signalFence == nullptr)
                {
                    return {
                        NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument,
                        "Metal queue received a present fence from another backend."
                    };
                }
                std::vector<std::shared_ptr<MetalSemaphore>> waitSemaphores;
                waitSemaphores.reserve(presentDesc.waitSemaphores.size());
                for (const auto& waitSemaphore : presentDesc.waitSemaphores)
                {
                    const auto semaphore = std::dynamic_pointer_cast<MetalSemaphore>(waitSemaphore);
                    if (semaphore == nullptr || !semaphore->IsValid() || semaphore->GetWaitValue() == 0u)
                    {
                        return {
                            NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument,
                            "Metal present received an invalid or unscheduled wait semaphore."
                        };
                    }
                    waitSemaphores.push_back(semaphore);
                }
                if (!swapchain->Present(
                        m_queue,
                        presentDesc.imageIndex,
                        waitSemaphores,
                        signalFence))
                {
                    return {
                        NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure,
                        "Metal swapchain does not own an acquired drawable."
                    };
                }
                NLS::Render::RHI::RHIQueueOperationResult result{};
                result.mayHaveQueuedGpuWork = true;
                result.frameFenceSignalQueued = signalFence != nullptr;
                return result;
            }

        private:
            id<MTLCommandQueue> m_queue = nil;
            NLS::Render::RHI::QueueType m_queueType;
            std::string m_debugName;
        };

        class MetalDevice final : public NLS::Render::RHI::RHIDevice
        {
        public:
            explicit MetalDevice(id<MTLDevice> device)
                : m_device([device retain])
                , m_graphicsQueue([m_device newCommandQueue])
                , m_computeQueue([m_device newCommandQueue])
                , m_copyQueue([m_device newCommandQueue])
                , m_adapter(std::make_shared<MetalAdapter>(
                    m_device.name != nil ? std::string(m_device.name.UTF8String) : std::string("Unknown Metal device")))
                , m_capabilities(CreateCapabilities(m_device))
            {
            }

            ~MetalDevice() override
            {
                [m_copyQueue release];
                [m_computeQueue release];
                [m_graphicsQueue release];
                [m_device release];
            }

            std::string_view GetDebugName() const override { return "MetalDevice"; }
            const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_adapter; }
            const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
            NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override
            {
                NLS::Render::RHI::NativeRenderDeviceInfo info{};
                info.backend = NLS::Render::RHI::NativeBackendType::Metal;
                info.device = (__bridge void*)m_device;
                info.graphicsQueue = (__bridge void*)m_graphicsQueue;
                info.swapchain = m_swapchainLayer;
                info.platformWindow = m_platformWindow;
                info.nativeWindowHandle = m_nativeWindowHandle;
                info.swapchainImageCount = m_swapchainImageCount;
                return info;
            }
            bool IsBackendReady() const override
            {
                return m_device != nil && m_graphicsQueue != nil && m_computeQueue != nil && m_copyQueue != nil;
            }
            std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType queueType) override
            {
                const size_t queueIndex = static_cast<size_t>(queueType);
                if (queueIndex >= m_queues.size())
                    return nullptr;
                if (m_queues[queueIndex] == nullptr)
                {
                    id<MTLCommandQueue> nativeQueue = queueType == NLS::Render::RHI::QueueType::Compute
                        ? m_computeQueue
                        : (queueType == NLS::Render::RHI::QueueType::Copy ? m_copyQueue : m_graphicsQueue);
                    m_queues[queueIndex] = std::make_shared<MetalQueue>(
                        nativeQueue,
                        queueType,
                        queueType == NLS::Render::RHI::QueueType::Graphics
                            ? "MetalGraphicsQueue"
                            : (queueType == NLS::Render::RHI::QueueType::Compute
                                ? "MetalComputeQueue"
                                : "MetalCopyQueue"));
                }
                return m_queues[queueIndex];
            }
            std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(
                const NLS::Render::RHI::SwapchainDesc& desc) override
            {
                auto swapchain = std::make_shared<MetalSwapchain>(m_device, desc);
                const auto handle = swapchain->GetNativeSwapchainHandle();
                if (!handle.IsValid())
                {
                    NLS_LOG_ERROR("CreateMetalSwapchain: GLFW did not provide a Cocoa window or CAMetalLayer creation failed.");
                    return nullptr;
                }
                m_swapchainLayer = handle.handle;
                m_swapchainImageCount = swapchain->GetImageCount();
                m_platformWindow = desc.platformWindow;
                m_nativeWindowHandle = swapchain->GetNativeWindowHandle();
                return swapchain;
            }
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(
                const NLS::Render::RHI::RHIBufferDesc& desc,
                const NLS::Render::RHI::RHIBufferUploadDesc& uploadDesc) override
            {
                using namespace NLS::Render::RHI;
                if (m_device == nil || desc.size == 0u)
                    return nullptr;
                if (uploadDesc.HasData() &&
                    (uploadDesc.destinationOffset > desc.size ||
                        uploadDesc.dataSize > desc.size - uploadDesc.destinationOffset))
                {
                    NLS_LOG_ERROR("CreateMetalBuffer: initial upload exceeds the destination buffer.");
                    return nullptr;
                }

                auto normalizedDesc = desc;
                if (HasBufferUsage(desc.usage, BufferUsageFlags::Uniform))
                    normalizedDesc.memoryUsage = MemoryUsage::CPUToGPU;

                const auto usageBits = static_cast<uint32_t>(normalizedDesc.usage);
                constexpr uint32_t kCpuToGpuAllowedUsage =
                    static_cast<uint32_t>(BufferUsageFlags::CopySrc) |
                    static_cast<uint32_t>(BufferUsageFlags::Vertex) |
                    static_cast<uint32_t>(BufferUsageFlags::Index) |
                    static_cast<uint32_t>(BufferUsageFlags::Uniform) |
                    static_cast<uint32_t>(BufferUsageFlags::ShaderRead);
                if (normalizedDesc.memoryUsage == MemoryUsage::CPUToGPU &&
                    (usageBits == 0u || (usageBits & ~kCpuToGpuAllowedUsage) != 0u))
                {
                    NLS_LOG_ERROR("CreateMetalBuffer: CPUToGPU usage is incompatible with the DX12 upload-heap contract.");
                    return nullptr;
                }
                if (normalizedDesc.memoryUsage == MemoryUsage::GPUToCPU &&
                    normalizedDesc.usage != BufferUsageFlags::CopyDst)
                {
                    NLS_LOG_ERROR("CreateMetalBuffer: GPUToCPU buffers only support CopyDst usage.");
                    return nullptr;
                }

                const MTLResourceOptions options = normalizedDesc.memoryUsage == MemoryUsage::GPUOnly
                    ? MTLResourceStorageModePrivate
                    : MTLResourceStorageModeShared;
                id<MTLBuffer> buffer = [m_device newBufferWithLength:normalizedDesc.size options:options];
                if (buffer == nil)
                    return nullptr;
                if (!normalizedDesc.debugName.empty())
                    buffer.label = [NSString stringWithUTF8String:normalizedDesc.debugName.c_str()];
                auto result = std::make_shared<MetalBuffer>(buffer, normalizedDesc);
                [buffer release];
                if (!uploadDesc.HasData())
                    return result;

                if (normalizedDesc.memoryUsage == MemoryUsage::CPUToGPU)
                    return result->UpdateData(uploadDesc).Succeeded() ? result : nullptr;
                if (normalizedDesc.memoryUsage == MemoryUsage::GPUToCPU)
                {
                    NLS_LOG_ERROR("CreateMetalBuffer: GPUToCPU buffers cannot be initialized from CPU upload data.");
                    return nullptr;
                }

                id<MTLBuffer> staging = [m_device newBufferWithBytes:uploadDesc.data
                    length:uploadDesc.dataSize
                    options:MTLResourceStorageModeShared];
                id<MTLCommandBuffer> uploadCommands = [m_graphicsQueue commandBuffer];
                id<MTLBlitCommandEncoder> blit = uploadCommands != nil
                    ? [uploadCommands blitCommandEncoder]
                    : nil;
                if (staging == nil || uploadCommands == nil || blit == nil)
                {
                    [staging release];
                    NLS_LOG_ERROR("CreateMetalBuffer: failed to create GPUOnly staging upload resources.");
                    return nullptr;
                }
                [blit copyFromBuffer:staging
                        sourceOffset:0u
                            toBuffer:result->GetBuffer()
                   destinationOffset:uploadDesc.destinationOffset
                                size:uploadDesc.dataSize];
                [blit endEncoding];
                [uploadCommands commit];
                [uploadCommands waitUntilCompleted];
                [staging release];
                if (uploadCommands.status == MTLCommandBufferStatusError)
                {
                    const char* message = uploadCommands.error.localizedDescription.UTF8String;
                    NLS_LOG_ERROR("CreateMetalBuffer: GPUOnly initial upload failed: " +
                        std::string(message != nullptr ? message : "unknown Metal error"));
                    return nullptr;
                }
                return result;
            }
            std::shared_ptr<NLS::Render::RHI::RHITexture> CreateTexture(
                const NLS::Render::RHI::RHITextureDesc& desc,
                const NLS::Render::RHI::RHITextureUploadDesc& uploadDesc) override
            {
                if (m_device == nil)
                    return nullptr;
                if (desc.memoryUsage != NLS::Render::RHI::MemoryUsage::GPUOnly)
                {
                    NLS_LOG_ERROR("CreateMetalTexture: textures require GPUOnly memory usage, matching DX12.");
                    return nullptr;
                }

                std::string validationReason;
                const auto textureType = ToMetalTextureType(desc);
                if (!textureType.has_value() || !ValidateMetalTextureDesc(desc, validationReason))
                {
                    NLS_LOG_WARNING(
                        "CreateMetalTexture: invalid descriptor for \"" + desc.debugName +
                        "\": " + (validationReason.empty() ? "unsupported texture dimension" : validationReason));
                    return nullptr;
                }

                const MTLPixelFormat pixelFormat = ToMetalPixelFormat(desc.format, desc.colorSpace);
                if (pixelFormat == MTLPixelFormatInvalid)
                {
                    NLS_LOG_WARNING("CreateMetalTexture: unsupported texture format " +
                        std::string(NLS::Render::RHI::GetTextureFormatName(desc.format)));
                    return nullptr;
                }

                const uint32_t layerCount = NLS::Render::RHI::GetTextureLayerCount(
                    desc.dimension,
                    desc.arrayLayers);
                MTLTextureDescriptor* descriptor = CreateMetalTextureDescriptor(
                    desc,
                    *textureType,
                    pixelFormat,
                    MTLStorageModePrivate);
                id<MTLTexture> texture = [m_device newTextureWithDescriptor:descriptor];
                [descriptor release];
                if (texture == nil)
                {
                    NLS_LOG_ERROR("CreateMetalTexture: Metal rejected texture descriptor for \"" + desc.debugName + "\"");
                    return nullptr;
                }
                if (!desc.debugName.empty())
                    texture.label = [NSString stringWithUTF8String:desc.debugName.c_str()];

                const auto* formatInfo = NLS::Render::RHI::GetTextureFormatDescriptor(desc.format);
                if (uploadDesc.HasData())
                {
                    if (formatInfo == nullptr || desc.sampleCount > 1u)
                    {
                        NLS_LOG_ERROR(
                            "CreateMetalTexture: initial CPU upload is unsupported for multisampled texture \"" +
                            desc.debugName + "\"");
                        [texture release];
                        return nullptr;
                    }

                    MTLTextureDescriptor* stagingDescriptor = CreateMetalTextureDescriptor(
                        desc,
                        *textureType,
                        pixelFormat,
                        MTLStorageModeShared);
                    id<MTLTexture> stagingTexture = [m_device newTextureWithDescriptor:stagingDescriptor];
                    [stagingDescriptor release];
                    if (stagingTexture == nil)
                    {
                        NLS_LOG_ERROR(
                            "CreateMetalTexture: failed to create shared staging texture for \"" +
                            desc.debugName + "\"");
                        [texture release];
                        return nullptr;
                    }

                    const uint32_t uploadLayerCount = desc.dimension == NLS::Render::RHI::TextureDimension::Texture3D
                        ? 1u
                        : layerCount;
                    const size_t subresourceCount =
                        static_cast<size_t>(uploadLayerCount) * static_cast<size_t>(desc.mipLevels);
                    if (!uploadDesc.subresources.empty() && uploadDesc.subresources.size() != subresourceCount)
                    {
                        NLS_LOG_ERROR(
                            "CreateMetalTexture: subresource count does not match layer-by-mip layout for \"" +
                            desc.debugName + "\"");
                        [stagingTexture release];
                        [texture release];
                        return nullptr;
                    }

                    const auto* packedData = static_cast<const uint8_t*>(uploadDesc.data);
                    size_t packedOffset = 0u;
                    size_t subresourceIndex = 0u;
                    for (uint32_t arrayLayer = 0u; arrayLayer < uploadLayerCount; ++arrayLayer)
                    {
                        for (uint32_t mipLevel = 0u; mipLevel < desc.mipLevels; ++mipLevel, ++subresourceIndex)
                        {
                            const uint32_t mipWidth = GetMipDimension(desc.extent.width, mipLevel);
                            const uint32_t mipHeight = desc.dimension == NLS::Render::RHI::TextureDimension::Texture1D
                                ? 1u
                                : GetMipDimension(desc.extent.height, mipLevel);
                            const uint32_t mipDepth = desc.dimension == NLS::Render::RHI::TextureDimension::Texture3D
                                ? GetMipDimension(desc.extent.depth, mipLevel)
                                : 1u;
                            const uint32_t rowPitch = NLS::Render::RHI::CalculateTextureRowPitch(
                                desc.format,
                                mipWidth);
                            const uint32_t bytesPerImage = NLS::Render::RHI::CalculateTextureSlicePitch(
                                desc.format,
                                mipWidth,
                                mipHeight,
                                1u);
                            const size_t requiredBytes = static_cast<size_t>(bytesPerImage) * mipDepth;

                            const void* source = nullptr;
                            size_t sourceSize = 0u;
                            if (!uploadDesc.subresources.empty())
                            {
                                source = uploadDesc.subresources[subresourceIndex].data;
                                sourceSize = uploadDesc.subresources[subresourceIndex].dataSize;
                            }
                            else if (packedData != nullptr && packedOffset <= uploadDesc.dataSize)
                            {
                                source = packedData + packedOffset;
                                sourceSize = uploadDesc.dataSize - packedOffset;
                            }

                            if (source == nullptr || rowPitch == 0u || bytesPerImage == 0u ||
                                sourceSize < requiredBytes)
                            {
                                NLS_LOG_ERROR(
                                    "CreateMetalTexture: texture subresource data is smaller than required for \"" +
                                    desc.debugName + "\"");
                                [stagingTexture release];
                                [texture release];
                                return nullptr;
                            }

                            std::vector<uint8_t> expandedRgb8;
                            const void* nativeSource = source;
                            uint32_t nativeRowPitch = rowPitch;
                            uint32_t nativeBytesPerImage = bytesPerImage;
                            if (desc.format == NLS::Render::RHI::TextureFormat::RGB8)
                            {
                                if (!ExpandMetalRgb8Upload(
                                    source,
                                    sourceSize,
                                    mipWidth,
                                    mipHeight,
                                    mipDepth,
                                    rowPitch,
                                    bytesPerImage,
                                    expandedRgb8))
                                {
                                    NLS_LOG_ERROR(
                                        "CreateMetalTexture: failed to expand RGB8 upload for \"" +
                                        desc.debugName + "\"");
                                    [stagingTexture release];
                                    [texture release];
                                    return nullptr;
                                }
                                nativeSource = expandedRgb8.data();
                                nativeRowPitch = mipWidth * 4u;
                                nativeBytesPerImage = nativeRowPitch * mipHeight;
                            }

                            MTLRegion region = desc.dimension == NLS::Render::RHI::TextureDimension::Texture1D
                                ? MTLRegionMake1D(0u, mipWidth)
                                : (desc.dimension == NLS::Render::RHI::TextureDimension::Texture3D
                                    ? MTLRegionMake3D(0u, 0u, 0u, mipWidth, mipHeight, mipDepth)
                                    : MTLRegionMake2D(0u, 0u, mipWidth, mipHeight));
                            [stagingTexture replaceRegion:region
                                             mipmapLevel:mipLevel
                                                    slice:arrayLayer
                                                withBytes:nativeSource
                                              bytesPerRow:nativeRowPitch
                                            bytesPerImage:nativeBytesPerImage];
                            packedOffset += requiredBytes;
                        }
                    }

                    id<MTLCommandBuffer> uploadCommands = [m_graphicsQueue commandBuffer];
                    id<MTLBlitCommandEncoder> blit = uploadCommands != nil
                        ? [uploadCommands blitCommandEncoder]
                        : nil;
                    if (uploadCommands == nil || blit == nil)
                    {
                        [stagingTexture release];
                        [texture release];
                        NLS_LOG_ERROR("CreateMetalTexture: failed to create staging upload commands.");
                        return nullptr;
                    }
                    for (uint32_t arrayLayer = 0u; arrayLayer < uploadLayerCount; ++arrayLayer)
                    {
                        for (uint32_t mipLevel = 0u; mipLevel < desc.mipLevels; ++mipLevel)
                        {
                            const MTLSize mipSize = MTLSizeMake(
                                GetMipDimension(desc.extent.width, mipLevel),
                                desc.dimension == NLS::Render::RHI::TextureDimension::Texture1D
                                    ? 1u
                                    : GetMipDimension(desc.extent.height, mipLevel),
                                desc.dimension == NLS::Render::RHI::TextureDimension::Texture3D
                                    ? GetMipDimension(desc.extent.depth, mipLevel)
                                    : 1u);
                            [blit copyFromTexture:stagingTexture
                                    sourceSlice:arrayLayer
                                    sourceLevel:mipLevel
                                   sourceOrigin:MTLOriginMake(0u, 0u, 0u)
                                     sourceSize:mipSize
                                      toTexture:texture
                               destinationSlice:arrayLayer
                               destinationLevel:mipLevel
                              destinationOrigin:MTLOriginMake(0u, 0u, 0u)];
                        }
                    }
                    [blit endEncoding];
                    [uploadCommands commit];
                    [uploadCommands waitUntilCompleted];
                    [stagingTexture release];
                    if (uploadCommands.status == MTLCommandBufferStatusError)
                    {
                        const char* message = uploadCommands.error.localizedDescription.UTF8String;
                        NLS_LOG_ERROR("CreateMetalTexture: GPUOnly initial upload failed: " +
                            std::string(message != nullptr ? message : "unknown Metal error"));
                        [texture release];
                        return nullptr;
                    }
                }

                const auto initialState = uploadDesc.HasData()
                    ? ResolveUploadedMetalTextureState(desc)
                    : NLS::Render::RHI::ResourceState::Unknown;
                auto result = std::make_shared<MetalTexture>(texture, desc, initialState);
                [texture release];
                return result;
            }
            NLS::Render::RHI::RHIUpdateResult UpdateTexture(
                const NLS::Render::RHI::RHITextureUpdateDesc& desc) override
            {
                const auto texture = std::dynamic_pointer_cast<MetalTexture>(desc.texture);
                if (texture == nullptr || texture->GetTexture() == nil || desc.data == nullptr ||
                    desc.extent.width == 0u || desc.extent.height == 0u || desc.extent.depth == 0u)
                {
                    return {
                        NLS::Render::RHI::RHIUpdateStatusCode::InvalidArgument,
                        "Invalid Metal texture update"
                    };
                }

                const auto& textureDesc = texture->GetDesc();
                const uint32_t mipLevels = (std::max)(textureDesc.mipLevels, 1u);
                const uint32_t layerCount = textureDesc.dimension == NLS::Render::RHI::TextureDimension::Texture3D
                    ? 1u
                    : NLS::Render::RHI::GetTextureLayerCount(textureDesc.dimension, textureDesc.arrayLayers);
                if (desc.mipLevel >= mipLevels || desc.arrayLayer >= layerCount ||
                    (textureDesc.dimension == NLS::Render::RHI::TextureDimension::Texture3D && desc.arrayLayer != 0u))
                {
                    return {
                        NLS::Render::RHI::RHIUpdateStatusCode::InvalidArgument,
                        "Metal texture update subresource is out of range"
                    };
                }

                const uint32_t mipWidth = GetMipDimension(textureDesc.extent.width, desc.mipLevel);
                const uint32_t mipHeight = textureDesc.dimension == NLS::Render::RHI::TextureDimension::Texture1D
                    ? 1u
                    : GetMipDimension(textureDesc.extent.height, desc.mipLevel);
                const uint32_t mipDepth = textureDesc.dimension == NLS::Render::RHI::TextureDimension::Texture3D
                    ? GetMipDimension(textureDesc.extent.depth, desc.mipLevel)
                    : 1u;
                if (desc.x >= mipWidth || desc.y >= mipHeight || desc.z >= mipDepth ||
                    desc.extent.width > mipWidth - desc.x ||
                    desc.extent.height > mipHeight - desc.y ||
                    desc.extent.depth > mipDepth - desc.z ||
                    (textureDesc.dimension != NLS::Render::RHI::TextureDimension::Texture3D &&
                        (desc.z != 0u || desc.extent.depth != 1u)))
                {
                    return {
                        NLS::Render::RHI::RHIUpdateStatusCode::InvalidArgument,
                        "Metal texture update extent is out of range"
                    };
                }

                const auto* formatInfo = NLS::Render::RHI::GetTextureFormatDescriptor(textureDesc.format);
                if (formatInfo == nullptr)
                {
                    return {
                        NLS::Render::RHI::RHIUpdateStatusCode::Unsupported,
                        "Metal texture update format is unsupported"
                    };
                }
                if (textureDesc.sampleCount > 1u)
                {
                    return {
                        NLS::Render::RHI::RHIUpdateStatusCode::Unsupported,
                        "Metal texture updates do not support multisampled storage"
                    };
                }

                const uint32_t minimumRowPitch = NLS::Render::RHI::CalculateTextureRowPitch(
                    textureDesc.format,
                    desc.extent.width);
                const uint32_t rowPitch = desc.rowPitch != 0u ? desc.rowPitch : minimumRowPitch;
                const uint32_t blockRows =
                    (desc.extent.height + formatInfo->blockHeight - 1u) / formatInfo->blockHeight;
                const uint32_t blockDepth =
                    (desc.extent.depth + formatInfo->blockDepth - 1u) / formatInfo->blockDepth;
                const uint32_t minimumBytesPerImage = rowPitch * blockRows;
                const uint32_t bytesPerImage = desc.slicePitch != 0u
                    ? desc.slicePitch
                    : minimumBytesPerImage;
                const uint64_t requiredBytes = static_cast<uint64_t>(bytesPerImage) * (blockDepth - 1u) +
                    static_cast<uint64_t>(rowPitch) * (blockRows - 1u) + minimumRowPitch;
                if (minimumRowPitch == 0u || rowPitch < minimumRowPitch ||
                    bytesPerImage < minimumBytesPerImage ||
                    requiredBytes > desc.dataSize)
                {
                    return {
                        NLS::Render::RHI::RHIUpdateStatusCode::InvalidArgument,
                        "Invalid Metal texture update row pitch"
                    };
                }
                if (formatInfo->isCompressed)
                {
                    const bool alignedOrigin =
                        (desc.x % formatInfo->blockWidth) == 0u &&
                        (desc.y % formatInfo->blockHeight) == 0u &&
                        (desc.z % formatInfo->blockDepth) == 0u;
                    const bool alignedOrTouchesRightEdge =
                        (desc.extent.width % formatInfo->blockWidth) == 0u ||
                        desc.x + desc.extent.width == mipWidth;
                    const bool alignedOrTouchesBottomEdge =
                        (desc.extent.height % formatInfo->blockHeight) == 0u ||
                        desc.y + desc.extent.height == mipHeight;
                    const bool alignedOrTouchesBackEdge =
                        (desc.extent.depth % formatInfo->blockDepth) == 0u ||
                        desc.z + desc.extent.depth == mipDepth;
                    if (!alignedOrigin || !alignedOrTouchesRightEdge ||
                        !alignedOrTouchesBottomEdge || !alignedOrTouchesBackEdge)
                    {
                        return {
                            NLS::Render::RHI::RHIUpdateStatusCode::InvalidArgument,
                            "Compressed Metal texture updates must be block-aligned or reach the mip edge"
                        };
                    }
                }

                std::vector<uint8_t> expandedRgb8;
                const void* nativeData = desc.data;
                uint32_t nativeRowPitch = rowPitch;
                uint32_t nativeBytesPerImage = bytesPerImage;
                if (textureDesc.format == NLS::Render::RHI::TextureFormat::RGB8)
                {
                    if (!ExpandMetalRgb8Upload(
                        desc.data,
                        desc.dataSize,
                        desc.extent.width,
                        desc.extent.height,
                        desc.extent.depth,
                        rowPitch,
                        bytesPerImage,
                        expandedRgb8))
                    {
                        return {
                            NLS::Render::RHI::RHIUpdateStatusCode::InvalidArgument,
                            "Invalid Metal RGB8 texture update layout"
                        };
                    }
                    nativeData = expandedRgb8.data();
                    nativeRowPitch = desc.extent.width * 4u;
                    nativeBytesPerImage = nativeRowPitch * desc.extent.height;
                }

                const MTLRegion region = textureDesc.dimension == NLS::Render::RHI::TextureDimension::Texture1D
                    ? MTLRegionMake1D(desc.x, desc.extent.width)
                    : (textureDesc.dimension == NLS::Render::RHI::TextureDimension::Texture3D
                        ? MTLRegionMake3D(
                            desc.x,
                            desc.y,
                            desc.z,
                            desc.extent.width,
                            desc.extent.height,
                            desc.extent.depth)
                        : MTLRegionMake2D(
                            desc.x,
                            desc.y,
                            desc.extent.width,
                            desc.extent.height));
                const auto textureType = ToMetalTextureType(textureDesc);
                if (!textureType.has_value())
                {
                    return {
                        NLS::Render::RHI::RHIUpdateStatusCode::Unsupported,
                        "Metal texture update dimension is unsupported"
                    };
                }
                MTLTextureDescriptor* stagingDescriptor = CreateMetalTextureDescriptor(
                    textureDesc,
                    *textureType,
                    texture->GetTexture().pixelFormat,
                    MTLStorageModeShared);
                id<MTLTexture> stagingTexture = [m_device newTextureWithDescriptor:stagingDescriptor];
                [stagingDescriptor release];
                if (stagingTexture == nil)
                {
                    return {
                        NLS::Render::RHI::RHIUpdateStatusCode::BackendFailure,
                        "Metal texture update failed to create a shared staging texture"
                    };
                }
                [stagingTexture replaceRegion:region
                                  mipmapLevel:desc.mipLevel
                                         slice:desc.arrayLayer
                                     withBytes:nativeData
                                   bytesPerRow:nativeRowPitch
                                 bytesPerImage:nativeBytesPerImage];

                id<MTLCommandBuffer> updateCommands = [m_graphicsQueue commandBuffer];
                id<MTLBlitCommandEncoder> blit = updateCommands != nil
                    ? [updateCommands blitCommandEncoder]
                    : nil;
                if (updateCommands == nil || blit == nil)
                {
                    [stagingTexture release];
                    return {
                        NLS::Render::RHI::RHIUpdateStatusCode::BackendFailure,
                        "Metal texture update failed to create staging copy commands"
                    };
                }
                [blit copyFromTexture:stagingTexture
                          sourceSlice:desc.arrayLayer
                          sourceLevel:desc.mipLevel
                         sourceOrigin:MTLOriginMake(desc.x, desc.y, desc.z)
                           sourceSize:MTLSizeMake(
                               desc.extent.width,
                               desc.extent.height,
                               desc.extent.depth)
                            toTexture:texture->GetTexture()
                     destinationSlice:desc.arrayLayer
                     destinationLevel:desc.mipLevel
                    destinationOrigin:MTLOriginMake(desc.x, desc.y, desc.z)];
                [blit endEncoding];
                [updateCommands commit];
                [updateCommands waitUntilCompleted];
                [stagingTexture release];
                if (updateCommands.status == MTLCommandBufferStatusError)
                {
                    const char* message = updateCommands.error.localizedDescription.UTF8String;
                    return {
                        NLS::Render::RHI::RHIUpdateStatusCode::BackendFailure,
                        "Metal texture staging update failed: " +
                            std::string(message != nullptr ? message : "unknown Metal error")
                    };
                }
                texture->SetState(textureDesc.usage != NLS::Render::RHI::TextureUsageFlags::None
                    ? ResolveUploadedMetalTextureState(textureDesc)
                    : texture->GetState());
                return { NLS::Render::RHI::RHIUpdateStatusCode::Success, {} };
            }
            std::shared_ptr<NLS::Render::RHI::RHITextureView> CreateTextureView(
                const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
                const NLS::Render::RHI::RHITextureViewDesc& desc) override
            {
                const auto metalTexture = std::dynamic_pointer_cast<MetalTexture>(texture);
                if (metalTexture == nullptr)
                    return nullptr;

                const MTLPixelFormat viewFormat = ToMetalPixelFormat(desc.format, desc.colorSpace);
                if (viewFormat == MTLPixelFormatInvalid)
                    return nullptr;
                const auto& textureDesc = metalTexture->GetDesc();
                const auto normalizedRange = NLS::Render::RHI::NormalizeTextureSubresourceRange(
                    textureDesc,
                    desc.subresourceRange);
                const auto textureType = ToMetalTextureType(textureDesc);
                const auto viewType = ToMetalTextureViewType(textureDesc, desc.viewType);
                if (!normalizedRange.has_value() || !textureType.has_value() || !viewType.has_value() ||
                    !ValidateMetalTextureView(textureDesc, desc, *normalizedRange))
                    return nullptr;

                id<MTLTexture> textureView = nil;
                if (*viewType == *textureType &&
                    NLS::Render::RHI::IsFullTextureSubresourceRange(textureDesc, *normalizedRange))
                {
                    textureView = [metalTexture->GetTexture() newTextureViewWithPixelFormat:viewFormat];
                }
                else
                {
                    textureView = [metalTexture->GetTexture()
                        newTextureViewWithPixelFormat:viewFormat
                        textureType:*viewType
                        levels:NSMakeRange(normalizedRange->baseMipLevel, normalizedRange->mipLevelCount)
                        slices:NSMakeRange(normalizedRange->baseArrayLayer, normalizedRange->arrayLayerCount)];
                }
                if (textureView == nil)
                    return nullptr;
                auto normalizedDesc = desc;
                normalizedDesc.subresourceRange = *normalizedRange;
                auto result = std::make_shared<MetalTextureView>(texture, std::move(normalizedDesc), textureView);
                [textureView release];
                return result;
            }
            std::shared_ptr<NLS::Render::RHI::RHISampler> CreateSampler(
                const NLS::Render::RHI::SamplerDesc& desc, std::string debugName) override
            {
                if (m_device == nil)
                    return nullptr;
                if (desc.mipLodBias != 0.0f)
                {
                    NLS_LOG_ERROR(
                        "CreateMetalSampler: Metal sampler states cannot represent a non-zero mip LOD bias.");
                    return nullptr;
                }
                const auto borderColor = ToMetalSamplerBorderColor(desc.borderColor);
                if (UsesMetalBorderColor(desc) && !borderColor.has_value())
                {
                    NLS_LOG_ERROR(
                        "CreateMetalSampler: Metal supports only transparent black, opaque black, or opaque white border colors.");
                    return nullptr;
                }
                MTLSamplerDescriptor* descriptor = [[MTLSamplerDescriptor alloc] init];
                descriptor.minFilter = ToMetalSamplerFilter(desc.minFilter);
                descriptor.magFilter = ToMetalSamplerFilter(desc.magFilter);
                descriptor.mipFilter = ToMetalSamplerMipFilter(desc.mipFilter);
                descriptor.sAddressMode = ToMetalSamplerAddressMode(desc.wrapU);
                descriptor.tAddressMode = ToMetalSamplerAddressMode(desc.wrapV);
                descriptor.rAddressMode = ToMetalSamplerAddressMode(desc.wrapW);
                descriptor.maxAnisotropy = (std::max)(1u, desc.maxAnisotropy);
                descriptor.lodMinClamp = desc.minLod;
                descriptor.lodMaxClamp = desc.maxLod;
                if (borderColor.has_value())
                    descriptor.borderColor = *borderColor;
                descriptor.compareFunction = desc.compareEnabled
                    ? ToMetalCompareFunction(desc.compareFunc)
                    : MTLCompareFunctionNever;
                id<MTLSamplerState> sampler = [m_device newSamplerStateWithDescriptor:descriptor];
                [descriptor release];
                if (sampler == nil)
                    return nullptr;
                if (debugName.empty())
                    debugName = "MetalSampler";
                auto result = std::make_shared<MetalSampler>(sampler, desc, std::move(debugName));
                [sampler release];
                return result;
            }
            std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateBindingLayout(
                const NLS::Render::RHI::RHIBindingLayoutDesc& desc) override
            {
                for (size_t entryIndex = 0u; entryIndex < desc.entries.size(); ++entryIndex)
                {
                    const auto& entry = desc.entries[entryIndex];
                    std::string reason;
                    if (!ValidateMetalBindingIndexSpan(entry, reason))
                    {
                        NLS_LOG_ERROR(
                            "CreateMetalBindingLayout rejected binding \"" + entry.name + "\": " + reason + ".");
                        return nullptr;
                    }

                    for (size_t previousIndex = 0u; previousIndex < entryIndex; ++previousIndex)
                    {
                        const auto& previous = desc.entries[previousIndex];
                        if (GetMetalBindingNamespace(previous.type) != GetMetalBindingNamespace(entry.type) ||
                            (static_cast<uint32_t>(previous.stageMask) & static_cast<uint32_t>(entry.stageMask)) == 0u)
                        {
                            continue;
                        }

                        for (uint32_t descriptorIndex = 0u; descriptorIndex < entry.count; ++descriptorIndex)
                        {
                            const auto index = ToMetalBindingIndex(
                                entry.type,
                                entry.registerSpace,
                                entry.binding + descriptorIndex);
                            for (uint32_t previousDescriptorIndex = 0u;
                                previousDescriptorIndex < previous.count;
                                ++previousDescriptorIndex)
                            {
                                const auto previousBindingIndex = ToMetalBindingIndex(
                                    previous.type,
                                    previous.registerSpace,
                                    previous.binding + previousDescriptorIndex);
                                if (index.has_value() && previousBindingIndex == index)
                                {
                                    NLS_LOG_ERROR(
                                        "CreateMetalBindingLayout rejected bindings \"" + previous.name +
                                        "\" and \"" + entry.name +
                                        "\" because they overlap the same Metal resource index.");
                                    return nullptr;
                                }
                            }
                        }
                    }
                }
                return std::make_shared<MetalBindingLayout>(desc);
            }
            std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateBindingSet(
                const NLS::Render::RHI::RHIBindingSetDesc& desc) override
            {
                const auto layout = std::dynamic_pointer_cast<MetalBindingLayout>(desc.layout);
                if (layout == nullptr)
                {
                    NLS_LOG_ERROR("CreateMetalBindingSet requires a native Metal binding layout.");
                    return nullptr;
                }

                const auto& layoutEntries = layout->GetDesc().entries;
                for (size_t entryIndex = 0u; entryIndex < desc.entries.size(); ++entryIndex)
                {
                    const auto& entry = desc.entries[entryIndex];
                    if (std::any_of(
                            desc.entries.begin(),
                            desc.entries.begin() + static_cast<std::ptrdiff_t>(entryIndex),
                            [&entry](const auto& previous)
                            {
                                return previous.binding == entry.binding && previous.type == entry.type;
                            }))
                    {
                        NLS_LOG_ERROR("CreateMetalBindingSet rejected a duplicate binding entry.");
                        return nullptr;
                    }

                    const auto layoutEntry = std::find_if(
                        layoutEntries.begin(),
                        layoutEntries.end(),
                        [&entry](const auto& candidate)
                        {
                            return candidate.binding == entry.binding && candidate.type == entry.type;
                        });
                    if (layoutEntry == layoutEntries.end())
                    {
                        NLS_LOG_ERROR("CreateMetalBindingSet rejected an entry absent from its binding layout.");
                        return nullptr;
                    }

                    using namespace NLS::Render::RHI;
                    switch (entry.type)
                    {
                    case BindingType::UniformBuffer:
                    case BindingType::StructuredBuffer:
                    case BindingType::StorageBuffer:
                    {
                        if (entry.buffer == nullptr)
                            break;
                        const auto buffer = std::dynamic_pointer_cast<MetalBuffer>(entry.buffer);
                        if (buffer == nullptr)
                        {
                            NLS_LOG_ERROR("CreateMetalBindingSet rejected a non-Metal buffer resource.");
                            return nullptr;
                        }
                        const auto& bufferDesc = buffer->GetDesc();
                        const uint64_t bufferRange = entry.bufferRange != 0u
                            ? entry.bufferRange
                            : (entry.bufferOffset <= bufferDesc.size ? bufferDesc.size - entry.bufferOffset : 0u);
                        if (entry.bufferOffset > bufferDesc.size || bufferRange == 0u ||
                            bufferRange > bufferDesc.size - entry.bufferOffset)
                        {
                            NLS_LOG_ERROR("CreateMetalBindingSet rejected an out-of-range buffer binding.");
                            return nullptr;
                        }
                        if (entry.type == BindingType::UniformBuffer &&
                            !HasBufferUsage(bufferDesc.usage, BufferUsageFlags::Uniform))
                        {
                            NLS_LOG_ERROR("CreateMetalBindingSet requires Uniform usage for a uniform buffer binding.");
                            return nullptr;
                        }
                        if (entry.type == BindingType::StructuredBuffer &&
                            !HasBufferUsage(bufferDesc.usage, BufferUsageFlags::ShaderRead) &&
                            !HasBufferUsage(bufferDesc.usage, BufferUsageFlags::Storage))
                        {
                            NLS_LOG_ERROR("CreateMetalBindingSet requires ShaderRead or Storage usage for a structured buffer binding.");
                            return nullptr;
                        }
                        if (entry.type == BindingType::StorageBuffer &&
                            !HasBufferUsage(bufferDesc.usage, BufferUsageFlags::Storage))
                        {
                            NLS_LOG_ERROR("CreateMetalBindingSet requires Storage usage for a storage buffer binding.");
                            return nullptr;
                        }
                        if (entry.type == BindingType::StructuredBuffer || entry.type == BindingType::StorageBuffer)
                        {
                            const uint32_t elementStride = entry.elementStride != 0u
                                ? entry.elementStride
                                : layoutEntry->elementStride != 0u
                                    ? layoutEntry->elementStride
                                    : sizeof(uint32_t);
                            if ((entry.bufferOffset % elementStride) != 0u ||
                                (bufferRange % elementStride) != 0u)
                            {
                                NLS_LOG_ERROR("CreateMetalBindingSet rejected an unaligned structured buffer range.");
                                return nullptr;
                            }
                        }
                        break;
                    }
                    case BindingType::Texture:
                    case BindingType::RWTexture:
                    {
                        if (entry.textureView == nullptr)
                            break;
                        const auto textureView = std::dynamic_pointer_cast<MetalTextureView>(entry.textureView);
                        const auto texture = textureView != nullptr
                            ? std::dynamic_pointer_cast<MetalTexture>(textureView->GetTexture())
                            : nullptr;
                        if (texture == nullptr)
                        {
                            NLS_LOG_ERROR("CreateMetalBindingSet rejected a non-Metal texture view.");
                            return nullptr;
                        }
                        const auto requiredUsage = entry.type == BindingType::RWTexture
                            ? TextureUsageFlags::Storage
                            : TextureUsageFlags::Sampled;
                        if (!HasTextureUsage(texture->GetDesc().usage, requiredUsage))
                        {
                            NLS_LOG_ERROR("CreateMetalBindingSet rejected a texture with incompatible usage.");
                            return nullptr;
                        }
                        break;
                    }
                    case BindingType::Sampler:
                        if (entry.sampler != nullptr && std::dynamic_pointer_cast<MetalSampler>(entry.sampler) == nullptr)
                        {
                            NLS_LOG_ERROR("CreateMetalBindingSet rejected a non-Metal sampler.");
                            return nullptr;
                        }
                        break;
                    }
                }
                return std::make_shared<MetalBindingSet>(desc);
            }
            std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> CreatePipelineLayout(
                const NLS::Render::RHI::RHIPipelineLayoutDesc& desc) override
            {
                for (const auto& layout : desc.bindingLayouts)
                {
                    if (layout != nullptr && std::dynamic_pointer_cast<MetalBindingLayout>(layout) == nullptr)
                        return nullptr;
                }
                for (const auto& range : desc.pushConstants)
                {
                    if (range.stageMask == NLS::Render::RHI::ShaderStageMask::None ||
                        range.size == 0u ||
                        (range.offset % sizeof(uint32_t)) != 0u ||
                        (range.size % sizeof(uint32_t)) != 0u ||
                        range.offset > NLS::Render::RHI::kRHIMaxPushConstantBytes ||
                        range.size > NLS::Render::RHI::kRHIMaxPushConstantBytes - range.offset)
                    {
                        NLS_LOG_ERROR(
                            "CreateMetalPipelineLayout: push constant range must be non-empty, "
                            "4-byte aligned, and contained within the RHI push constant limit.");
                        return nullptr;
                    }
                }
                return std::make_shared<MetalPipelineLayout>(desc);
            }
            std::shared_ptr<NLS::Render::RHI::RHIShaderModule> CreateShaderModule(
                const NLS::Render::RHI::RHIShaderModuleDesc& desc) override
            {
                if (m_device == nil || desc.bytecode.empty())
                    return nullptr;

                try
                {
                    std::string source;
                    std::string metalEntryPoint = desc.entryPoint;
                    MTLSize threadgroupSize = MTLSizeMake(1u, 1u, 1u);
                    uint32_t magic = 0u;
                    if (desc.bytecode.size() >= sizeof(magic))
                        std::memcpy(&magic, desc.bytecode.data(), sizeof(magic));
                    const bool isSpirv = desc.bytecode.size() % sizeof(uint32_t) == 0u && magic == 0x07230203u;
                    if (isSpirv)
                    {
                        std::vector<uint32_t> spirv(desc.bytecode.size() / sizeof(uint32_t));
                        std::memcpy(spirv.data(), desc.bytecode.data(), desc.bytecode.size());
                        spirv_cross::CompilerMSL compiler(std::move(spirv));
                        auto options = compiler.get_msl_options();
                        options.platform = spirv_cross::CompilerMSL::Options::macOS;
                        options.msl_version = spirv_cross::CompilerMSL::Options::make_msl_version(2u, 4u);
                        compiler.set_msl_options(options);

                        const auto executionModel = compiler.get_execution_model();
                        if (desc.stage == NLS::Render::RHI::ShaderStage::Compute)
                        {
                            threadgroupSize = MTLSizeMake(
                                compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 0u),
                                compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 1u),
                                compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 2u));
                        }
                        const auto addBufferBindings = [&](const auto& resources, const bool storageBuffer)
                        {
                            for (const auto& resource : resources)
                            {
                                const uint32_t bindingSpace = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
                                const uint32_t sourceBinding = compiler.get_decoration(resource.id, spv::DecorationBinding);
                                const uint32_t binding = storageBuffer
                                    ? sourceBinding + kMetalSpirvStorageBindingOffset
                                    : sourceBinding;
                                if (storageBuffer)
                                    compiler.set_decoration(resource.id, spv::DecorationBinding, binding);
                                const auto index = storageBuffer
                                    ? ToMetalStorageBufferBindingIndex(bindingSpace, sourceBinding)
                                    : ToMetalBufferBindingIndex(bindingSpace, sourceBinding);
                                if (!index.has_value())
                                    throw std::runtime_error("Metal buffer binding exceeds the supported binding map.");
                                spirv_cross::MSLResourceBinding remap{};
                                remap.stage = executionModel;
                                remap.desc_set = bindingSpace;
                                remap.binding = binding;
                                remap.msl_buffer = *index;
                                compiler.add_msl_resource_binding(remap);
                            }
                        };
                        const auto addTextureBindings = [&](const auto& resources, const bool includeSampler)
                        {
                            for (const auto& resource : resources)
                            {
                                const uint32_t bindingSpace = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
                                const uint32_t sourceBinding = compiler.get_decoration(resource.id, spv::DecorationBinding);
                                const uint32_t binding = sourceBinding + kMetalSpirvTextureBindingOffset;
                                compiler.set_decoration(resource.id, spv::DecorationBinding, binding);
                                const auto index = ToMetalTextureBindingIndex(bindingSpace, sourceBinding);
                                if (!index.has_value())
                                    throw std::runtime_error("Metal texture binding exceeds the supported binding map.");
                                const auto samplerIndex = includeSampler
                                    ? ToMetalSamplerBindingIndex(bindingSpace, sourceBinding)
                                    : std::optional<uint32_t>{};
                                if (includeSampler && !samplerIndex.has_value())
                                    throw std::runtime_error("Metal sampler binding exceeds the supported binding map.");
                                spirv_cross::MSLResourceBinding remap{};
                                remap.stage = executionModel;
                                remap.desc_set = bindingSpace;
                                remap.binding = binding;
                                remap.msl_texture = *index;
                                remap.msl_sampler = samplerIndex.value_or(0u);
                                compiler.add_msl_resource_binding(remap);
                            }
                        };
                        const auto addSamplerBindings = [&](const auto& resources)
                        {
                            for (const auto& resource : resources)
                            {
                                const uint32_t bindingSpace = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
                                const uint32_t sourceBinding = compiler.get_decoration(resource.id, spv::DecorationBinding);
                                const uint32_t binding = sourceBinding + kMetalSpirvSamplerBindingOffset;
                                compiler.set_decoration(resource.id, spv::DecorationBinding, binding);
                                const auto index = ToMetalSamplerBindingIndex(bindingSpace, sourceBinding);
                                if (!index.has_value())
                                    throw std::runtime_error("Metal sampler binding exceeds the supported binding map.");
                                spirv_cross::MSLResourceBinding remap{};
                                remap.stage = executionModel;
                                remap.desc_set = bindingSpace;
                                remap.binding = binding;
                                remap.msl_sampler = *index;
                                compiler.add_msl_resource_binding(remap);
                            }
                        };

                        const auto resources = compiler.get_shader_resources();
                        addBufferBindings(resources.uniform_buffers, false);
                        addBufferBindings(resources.storage_buffers, true);
                        addTextureBindings(resources.sampled_images, true);
                        addTextureBindings(resources.separate_images, false);
                        addTextureBindings(resources.storage_images, false);
                        addSamplerBindings(resources.separate_samplers);
                        source = compiler.compile();
                        metalEntryPoint = compiler.get_cleansed_entry_point_name(desc.entryPoint, executionModel);
                    }
                    else
                    {
                        source.assign(reinterpret_cast<const char*>(desc.bytecode.data()), desc.bytecode.size());
                    }

                    NSString* sourceString = [[NSString alloc] initWithBytes:source.data()
                        length:source.size()
                        encoding:NSUTF8StringEncoding];
                    NSError* error = nil;
                    id<MTLLibrary> library = [m_device newLibraryWithSource:sourceString options:nil error:&error];
                    [sourceString release];
                    if (library == nil)
                    {
                        const char* message = error != nil ? error.localizedDescription.UTF8String : "Unknown Metal shader compiler error.";
                        NLS_LOG_ERROR("CreateMetalShaderModule: " + std::string(message != nullptr ? message : "Unknown error."));
                        return nullptr;
                    }
                    NSString* entryPoint = [NSString stringWithUTF8String:metalEntryPoint.c_str()];
                    id<MTLFunction> function = [library newFunctionWithName:entryPoint];
                    if (function == nil)
                    {
                        NLS_LOG_ERROR("CreateMetalShaderModule: MSL entry point not found: " + metalEntryPoint);
                        [library release];
                        return nullptr;
                    }
                    auto result = std::make_shared<MetalShaderModule>(library, function, desc, threadgroupSize);
                    [function release];
                    [library release];
                    return result;
                }
                catch (const std::exception& exception)
                {
                    NLS_LOG_ERROR("CreateMetalShaderModule: SPIR-V to MSL translation failed: " + std::string(exception.what()));
                    return nullptr;
                }
            }
            std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> CreateGraphicsPipeline(
                const NLS::Render::RHI::RHIGraphicsPipelineDesc& desc) override
            {
                const auto vertexShader = std::dynamic_pointer_cast<MetalShaderModule>(desc.vertexShader);
                const auto fragmentShader = std::dynamic_pointer_cast<MetalShaderModule>(desc.fragmentShader);
                if (m_device == nil || vertexShader == nullptr || fragmentShader == nullptr)
                    return nullptr;

                MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
                descriptor.label = [NSString stringWithUTF8String:desc.debugName.c_str()];
                descriptor.vertexFunction = vertexShader->GetFunction();
                descriptor.fragmentFunction = fragmentShader->GetFunction();
                descriptor.rasterSampleCount = (std::max)(1u, desc.renderTargetLayout.sampleCount);
                descriptor.alphaToCoverageEnabled = desc.blendState.alphaToCoverageEnable;

                const size_t colorAttachmentCount = (std::max)(size_t{1u}, desc.renderTargetLayout.colorFormats.size());
                if (colorAttachmentCount > 8u)
                {
                    [descriptor release];
                    return nullptr;
                }
                for (size_t index = 0u; index < colorAttachmentCount; ++index)
                {
                    const auto format = index < desc.renderTargetLayout.colorFormats.size()
                        ? desc.renderTargetLayout.colorFormats[index]
                        : NLS::Render::RHI::TextureFormat::RGBA8;
                    const MTLPixelFormat pixelFormat = ToMetalPixelFormat(
                        format,
                        desc.renderTargetLayout.GetColorSpace(index));
                    if (pixelFormat == MTLPixelFormatInvalid)
                    {
                        [descriptor release];
                        return nullptr;
                    }
                    const size_t blendStateIndex = desc.blendState.independentBlendEnable
                        ? index
                        : 0u;
                    const auto& sourceBlend = blendStateIndex < desc.blendState.renderTargets.size()
                        ? desc.blendState.renderTargets[blendStateIndex]
                        : NLS::Render::RHI::RHIRenderTargetBlendStateDesc {
                            desc.blendState.enabled,
                            NLS::Render::RHI::RHIBlendFactor::SrcAlpha,
                            NLS::Render::RHI::RHIBlendFactor::InvSrcAlpha,
                            NLS::Render::RHI::RHIBlendOp::Add,
                            NLS::Render::RHI::RHIBlendFactor::One,
                            NLS::Render::RHI::RHIBlendFactor::InvSrcAlpha,
                            NLS::Render::RHI::RHIBlendOp::Add,
                            desc.blendState.colorWrite
                                ? NLS::Render::RHI::RHIColorWriteMask::All
                                : NLS::Render::RHI::RHIColorWriteMask::None
                        };
                    MTLRenderPipelineColorAttachmentDescriptor* color = descriptor.colorAttachments[index];
                    color.pixelFormat = pixelFormat;
                    color.blendingEnabled = sourceBlend.blendEnable;
                    color.sourceRGBBlendFactor = ToMetalBlendFactor(sourceBlend.srcColor);
                    color.destinationRGBBlendFactor = ToMetalBlendFactor(sourceBlend.dstColor);
                    color.rgbBlendOperation = ToMetalBlendOperation(sourceBlend.colorOp);
                    color.sourceAlphaBlendFactor = ToMetalBlendFactor(sourceBlend.srcAlpha);
                    color.destinationAlphaBlendFactor = ToMetalBlendFactor(sourceBlend.dstAlpha);
                    color.alphaBlendOperation = ToMetalBlendOperation(sourceBlend.alphaOp);
                    color.writeMask = ToMetalColorWriteMask(sourceBlend.colorWriteMask);
                }

                if (desc.renderTargetLayout.hasDepth)
                {
                    const MTLPixelFormat depthFormat = ToMetalPixelFormat(
                        desc.renderTargetLayout.depthFormat,
                        NLS::Render::RHI::TextureColorSpace::Linear);
                    if (depthFormat == MTLPixelFormatInvalid)
                    {
                        [descriptor release];
                        return nullptr;
                    }
                    descriptor.depthAttachmentPixelFormat = depthFormat;
                    if (depthFormat == MTLPixelFormatDepth32Float_Stencil8)
                        descriptor.stencilAttachmentPixelFormat = depthFormat;
                }

                MTLVertexDescriptor* vertexDescriptor = [[MTLVertexDescriptor alloc] init];
                for (const auto& buffer : desc.vertexBuffers)
                {
                    if (buffer.binding >= 8u)
                    {
                        [vertexDescriptor release];
                        [descriptor release];
                        return nullptr;
                    }
                    MTLVertexBufferLayoutDescriptor* layout = vertexDescriptor.layouts[buffer.binding];
                    layout.stride = buffer.stride;
                    layout.stepFunction = buffer.perInstance
                        ? MTLVertexStepFunctionPerInstance
                        : MTLVertexStepFunctionPerVertex;
                    layout.stepRate = 1u;
                }
                for (const auto& attribute : desc.vertexAttributes)
                {
                    if (attribute.location >= 31u || attribute.binding >= 8u)
                    {
                        [vertexDescriptor release];
                        [descriptor release];
                        return nullptr;
                    }
                    MTLVertexAttributeDescriptor* target = vertexDescriptor.attributes[attribute.location];
                    target.format = ToMetalVertexFormat(attribute.elementSize);
                    target.offset = attribute.offset;
                    target.bufferIndex = attribute.binding;
                }
                descriptor.vertexDescriptor = vertexDescriptor;
                [vertexDescriptor release];

                NSError* error = nil;
                id<MTLRenderPipelineState> pipelineState = [m_device newRenderPipelineStateWithDescriptor:descriptor error:&error];
                [descriptor release];
                if (pipelineState == nil)
                {
                    const char* message = error != nil ? error.localizedDescription.UTF8String : "Unknown Metal pipeline error.";
                    NLS_LOG_ERROR("CreateMetalGraphicsPipeline: " + std::string(message != nullptr ? message : "Unknown error."));
                    return nullptr;
                }

                id<MTLDepthStencilState> depthStencilState = nil;
                if (desc.renderTargetLayout.hasDepth)
                {
                    MTLDepthStencilDescriptor* depthDescriptor = [[MTLDepthStencilDescriptor alloc] init];
                    depthDescriptor.depthCompareFunction = desc.depthStencilState.depthTest
                        ? ToMetalCompareFunction(desc.depthStencilState.depthCompare)
                        : MTLCompareFunctionAlways;
                    depthDescriptor.depthWriteEnabled = desc.depthStencilState.depthWrite;
                    if (desc.depthStencilState.stencilTest)
                    {
                        MTLStencilDescriptor* stencil = [[MTLStencilDescriptor alloc] init];
                        stencil.stencilCompareFunction = ToMetalCompareFunction(desc.depthStencilState.stencilCompare);
                        stencil.stencilFailureOperation = ToMetalStencilOperation(desc.depthStencilState.stencilFailOp);
                        stencil.depthFailureOperation = ToMetalStencilOperation(desc.depthStencilState.stencilDepthFailOp);
                        stencil.depthStencilPassOperation = ToMetalStencilOperation(desc.depthStencilState.stencilPassOp);
                        stencil.readMask = desc.depthStencilState.stencilReadMask;
                        stencil.writeMask = desc.depthStencilState.stencilWriteMask;
                        depthDescriptor.frontFaceStencil = stencil;
                        depthDescriptor.backFaceStencil = stencil;
                        [stencil release];
                    }
                    depthStencilState = [m_device newDepthStencilStateWithDescriptor:depthDescriptor];
                    [depthDescriptor release];
                }
                auto result = std::make_shared<MetalGraphicsPipeline>(pipelineState, depthStencilState, desc);
                [depthStencilState release];
                [pipelineState release];
                return result;
            }
            std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> CreateComputePipeline(
                const NLS::Render::RHI::RHIComputePipelineDesc& desc) override
            {
                const auto computeShader = std::dynamic_pointer_cast<MetalShaderModule>(desc.computeShader);
                if (m_device == nil || computeShader == nullptr || computeShader->GetFunction() == nil)
                    return nullptr;

                NSError* error = nil;
                id<MTLComputePipelineState> pipelineState =
                    [m_device newComputePipelineStateWithFunction:computeShader->GetFunction() error:&error];
                if (pipelineState == nil)
                {
                    const char* message = error != nil ? error.localizedDescription.UTF8String : "Unknown Metal compute pipeline error.";
                    NLS_LOG_ERROR("CreateMetalComputePipeline: " +
                        std::string(message != nullptr ? message : "Unknown compute pipeline error."));
                    return nullptr;
                }

                auto result = std::make_shared<MetalComputePipeline>(
                    pipelineState,
                    computeShader->GetThreadgroupSize(),
                    desc);
                [pipelineState release];
                return result;
            }
            std::shared_ptr<NLS::Render::RHI::RHICommandPool> CreateCommandPool(
                NLS::Render::RHI::QueueType queueType, std::string debugName) override
            {
                id<MTLCommandQueue> nativeQueue = queueType == NLS::Render::RHI::QueueType::Compute
                    ? m_computeQueue
                    : (queueType == NLS::Render::RHI::QueueType::Copy ? m_copyQueue : m_graphicsQueue);
                return std::make_shared<MetalCommandPool>(nativeQueue, queueType,
                    debugName.empty() ? "MetalCommandPool" : std::move(debugName));
            }
            std::shared_ptr<NLS::Render::RHI::RHIFence> CreateFence(std::string debugName) override
            {
                return std::make_shared<MetalFence>(debugName.empty() ? "MetalFence" : std::move(debugName));
            }
            std::shared_ptr<NLS::Render::RHI::RHISemaphore> CreateSemaphore(std::string debugName) override
            {
                auto semaphore = std::make_shared<MetalSemaphore>(
                    m_device,
                    debugName.empty() ? "MetalSemaphore" : std::move(debugName));
                return semaphore->IsValid() ? semaphore : nullptr;
            }
            NLS::Render::RHI::RHIReadbackResult BeginReadBuffer(
                const NLS::Render::RHI::RHIBufferReadbackDesc& desc) override
            {
                const auto source = std::dynamic_pointer_cast<MetalBuffer>(desc.source);
                if (source == nullptr || source->GetBuffer() == nil)
                {
                    return {
                        NLS::Render::RHI::RHIReadbackStatusCode::InvalidArgument,
                        "BeginReadBuffer source is not a valid Metal buffer"
                    };
                }
                if (desc.data == nullptr)
                {
                    return {
                        NLS::Render::RHI::RHIReadbackStatusCode::InvalidArgument,
                        "BeginReadBuffer destination data is null"
                    };
                }
                if (desc.size == 0u)
                {
                    return {
                        NLS::Render::RHI::RHIReadbackStatusCode::InvalidArgument,
                        "BeginReadBuffer size is zero"
                    };
                }
                const uint64_t sourceSize = source->GetDesc().size;
                if (desc.sourceOffset > sourceSize || desc.size > sourceSize - desc.sourceOffset)
                {
                    return {
                        NLS::Render::RHI::RHIReadbackStatusCode::InvalidArgument,
                        "BeginReadBuffer range exceeds source buffer size"
                    };
                }
                std::vector<std::pair<std::shared_ptr<MetalSemaphore>, uint64_t>> waitSemaphores;
                waitSemaphores.reserve(desc.waitSemaphores.size());
                for (const auto& waitSemaphore : desc.waitSemaphores)
                {
                    const auto semaphore = std::dynamic_pointer_cast<MetalSemaphore>(waitSemaphore);
                    if (semaphore == nullptr || !semaphore->IsValid() || semaphore->GetWaitValue() == 0u)
                    {
                        return {
                            NLS::Render::RHI::RHIReadbackStatusCode::InvalidArgument,
                            "BeginReadBuffer received an invalid or unscheduled wait semaphore"
                        };
                    }
                    waitSemaphores.emplace_back(semaphore, semaphore->GetWaitValue());
                }
                if (m_graphicsQueue == nil)
                {
                    return {
                        NLS::Render::RHI::RHIReadbackStatusCode::BackendFailure,
                        "BeginReadBuffer Metal graphics queue is unavailable"
                    };
                }

                // Every exposed Metal queue is backed by this serial MTLCommandQueue. Enqueuing
                // the completion marker after frame submission orders it after graphics/compute
                // writes, including logical semaphore dependencies from the shared queue.
                id<MTLCommandBuffer> completionMarker = [m_graphicsQueue commandBuffer];
                if (completionMarker == nil)
                {
                    return {
                        NLS::Render::RHI::RHIReadbackStatusCode::BackendFailure,
                        "BeginReadBuffer failed to create a Metal completion command buffer"
                    };
                }
                if (!desc.debugName.empty())
                    completionMarker.label = [NSString stringWithUTF8String:desc.debugName.c_str()];
                for (const auto& [semaphore, value] : waitSemaphores)
                    [completionMarker encodeWaitForEvent:semaphore->GetEvent() value:value];

                id<MTLBuffer> readbackBuffer = source->GetBuffer();
                uint64_t readbackOffset = desc.sourceOffset;
                if (source->GetBuffer().storageMode == MTLStorageModePrivate)
                {
                    readbackBuffer = [m_device newBufferWithLength:desc.size
                        options:MTLResourceStorageModeShared];
                    id<MTLBlitCommandEncoder> blit = readbackBuffer != nil
                        ? [completionMarker blitCommandEncoder]
                        : nil;
                    if (readbackBuffer == nil || blit == nil)
                    {
                        [readbackBuffer release];
                        return {
                            NLS::Render::RHI::RHIReadbackStatusCode::BackendFailure,
                            "BeginReadBuffer failed to create private-buffer staging copy commands"
                        };
                    }
                    [blit copyFromBuffer:source->GetBuffer()
                            sourceOffset:desc.sourceOffset
                                toBuffer:readbackBuffer
                       destinationOffset:0u
                                    size:desc.size];
                    [blit endEncoding];
                    readbackOffset = 0u;
                }

                auto completion = std::make_shared<MetalBufferReadbackCompletionToken>(
                    completionMarker,
                    source,
                    readbackBuffer,
                    readbackOffset,
                    desc.size,
                    desc.data,
                    desc.debugName);
                if (readbackBuffer != source->GetBuffer())
                    [readbackBuffer release];
                [completionMarker commit];
                return {
                    NLS::Render::RHI::RHIReadbackStatusCode::Success,
                    {},
                    std::move(completion)
                };
            }
            void ReadPixels(
                const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
                const uint32_t x,
                const uint32_t y,
                const uint32_t width,
                const uint32_t height,
                const NLS::Render::Settings::EPixelDataFormat format,
                const NLS::Render::Settings::EPixelDataType type,
                void* data) override
            {
                const auto result = ReadPixelsChecked(texture, x, y, width, height, format, type, data);
                if (!result.Succeeded())
                    NLS_LOG_WARNING("MetalDevice::ReadPixels failed: " + result.message);
            }
            NLS::Render::RHI::RHIReadbackResult ReadPixelsChecked(
                const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
                const uint32_t x,
                const uint32_t y,
                const uint32_t width,
                const uint32_t height,
                const NLS::Render::Settings::EPixelDataFormat format,
                const NLS::Render::Settings::EPixelDataType type,
                void* data) override
            {
                auto result = BeginReadPixels(texture, x, y, width, height, format, type, data);
                if (!result.Succeeded() || result.completion == nullptr)
                    return result;

                const auto completionStatus = result.completion->Wait();
                switch (completionStatus.code)
                {
                case NLS::Render::RHI::RHICompletionStatusCode::Success:
                    result.code = NLS::Render::RHI::RHIReadbackStatusCode::Success;
                    result.message = completionStatus.message;
                    return result;
                case NLS::Render::RHI::RHICompletionStatusCode::DeviceLost:
                    result.code = NLS::Render::RHI::RHIReadbackStatusCode::DeviceLost;
                    result.message = completionStatus.message;
                    return result;
                case NLS::Render::RHI::RHICompletionStatusCode::Failed:
                    result.code = NLS::Render::RHI::RHIReadbackStatusCode::BackendFailure;
                    result.message = completionStatus.message;
                    return result;
                case NLS::Render::RHI::RHICompletionStatusCode::Pending:
                default:
                    result.code = NLS::Render::RHI::RHIReadbackStatusCode::BackendFailure;
                    result.message = completionStatus.message.empty()
                        ? "Metal pixel readback completion did not finish"
                        : completionStatus.message;
                    return result;
                }
            }
            NLS::Render::RHI::RHIReadbackResult BeginReadPixels(
                const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
                const uint32_t x,
                const uint32_t y,
                const uint32_t width,
                const uint32_t height,
                const NLS::Render::Settings::EPixelDataFormat format,
                const NLS::Render::Settings::EPixelDataType type,
                void* data) override
            {
                const auto metalTexture = std::dynamic_pointer_cast<MetalTexture>(texture);
                if (metalTexture == nullptr || metalTexture->GetTexture() == nil)
                    return { NLS::Render::RHI::RHIReadbackStatusCode::InvalidArgument, "Metal readback texture is unavailable" };
                if (data == nullptr || width == 0u || height == 0u)
                    return { NLS::Render::RHI::RHIReadbackStatusCode::InvalidArgument, "Metal readback arguments are invalid" };
                if (x >= metalTexture->GetTexture().width || y >= metalTexture->GetTexture().height ||
                    width > metalTexture->GetTexture().width - x || height > metalTexture->GetTexture().height - y)
                {
                    return { NLS::Render::RHI::RHIReadbackStatusCode::InvalidArgument, "Metal readback region is outside the texture" };
                }
                if (type != NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE)
                    return { NLS::Render::RHI::RHIReadbackStatusCode::UnsupportedFormat, "Metal readback supports only UNSIGNED_BYTE data" };

                size_t destinationBytesPerPixel = 0u;
                switch (format)
                {
                case NLS::Render::Settings::EPixelDataFormat::RGB:
                case NLS::Render::Settings::EPixelDataFormat::BGR:
                    destinationBytesPerPixel = 3u;
                    break;
                case NLS::Render::Settings::EPixelDataFormat::RGBA:
                case NLS::Render::Settings::EPixelDataFormat::BGRA:
                    destinationBytesPerPixel = 4u;
                    break;
                default:
                    return { NLS::Render::RHI::RHIReadbackStatusCode::UnsupportedFormat, "Metal readback destination format is unsupported" };
                }

                const auto nativeFormat = metalTexture->GetTexture().pixelFormat;
                const auto sourceBytesPerPixel = GetMetalReadbackBytesPerPixel(nativeFormat);
                if (!sourceBytesPerPixel.has_value())
                    return { NLS::Render::RHI::RHIReadbackStatusCode::UnsupportedFormat, "Metal readback source format is unsupported" };
                if (destinationBytesPerPixel > *sourceBytesPerPixel)
                {
                    return {
                        NLS::Render::RHI::RHIReadbackStatusCode::UnsupportedFormat,
                        "Metal readback destination format requires more channels than the source format provides"
                    };
                }
                if (metalTexture->GetTexture().sampleCount != 1u)
                {
                    return {
                        NLS::Render::RHI::RHIReadbackStatusCode::UnsupportedFormat,
                        "Metal readback requires a resolved single-sample texture"
                    };
                }
                const bool nativeBgra = nativeFormat == MTLPixelFormatBGRA8Unorm ||
                    nativeFormat == MTLPixelFormatBGRA8Unorm_sRGB;

                constexpr NSUInteger kReadbackRowAlignment = 256u;
                if (static_cast<uint64_t>(width) >
                    static_cast<uint64_t>((std::numeric_limits<NSUInteger>::max)()) / *sourceBytesPerPixel)
                {
                    return { NLS::Render::RHI::RHIReadbackStatusCode::InvalidArgument, "Metal readback row size overflows NSUInteger" };
                }
                const NSUInteger packedRowBytes =
                    static_cast<NSUInteger>(width) * static_cast<NSUInteger>(*sourceBytesPerPixel);
                const NSUInteger stagingRowBytes =
                    (packedRowBytes + kReadbackRowAlignment - 1u) & ~(kReadbackRowAlignment - 1u);
                if (height > 0u && stagingRowBytes >
                    (std::numeric_limits<NSUInteger>::max)() / static_cast<NSUInteger>(height))
                {
                    return { NLS::Render::RHI::RHIReadbackStatusCode::InvalidArgument, "Metal readback buffer size overflows NSUInteger" };
                }
                const NSUInteger stagingSize = stagingRowBytes * static_cast<NSUInteger>(height);
                id<MTLBuffer> stagingBuffer = [m_device newBufferWithLength:stagingSize
                    options:MTLResourceStorageModeShared];
                if (stagingBuffer == nil)
                    return { NLS::Render::RHI::RHIReadbackStatusCode::BackendFailure, "Metal readback failed to create staging buffer" };

                id<MTLCommandBuffer> readbackCommands = [m_graphicsQueue commandBuffer];
                id<MTLBlitCommandEncoder> blitEncoder = readbackCommands != nil
                    ? [readbackCommands blitCommandEncoder]
                    : nil;
                if (readbackCommands == nil || blitEncoder == nil)
                {
                    [stagingBuffer release];
                    return { NLS::Render::RHI::RHIReadbackStatusCode::BackendFailure, "Metal readback failed to create staging copy commands" };
                }
                [blitEncoder copyFromTexture:metalTexture->GetTexture()
                                 sourceSlice:0u
                                 sourceLevel:0u
                                sourceOrigin:MTLOriginMake(x, y, 0u)
                                  sourceSize:MTLSizeMake(width, height, 1u)
                                    toBuffer:stagingBuffer
                           destinationOffset:0u
                      destinationBytesPerRow:stagingRowBytes
                    destinationBytesPerImage:stagingSize];
                [blitEncoder endEncoding];
                auto completion = std::make_shared<MetalPixelReadbackCompletionToken>(
                    readbackCommands,
                    metalTexture,
                    stagingBuffer,
                    *sourceBytesPerPixel,
                    destinationBytesPerPixel,
                    stagingRowBytes,
                    width,
                    height,
                    nativeBgra,
                    format,
                    data);
                [stagingBuffer release];
                [readbackCommands commit];
                return {
                    NLS::Render::RHI::RHIReadbackStatusCode::Success,
                    {},
                    std::move(completion)
                };
            }

        private:
            static NLS::Render::RHI::RHIDeviceCapabilities CreateCapabilities(id<MTLDevice> device)
            {
                NLS::Render::RHI::RHIDeviceCapabilities capabilities{};
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::BackendReady, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::Graphics, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::Compute, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::AsyncCompute, false,
                    "Metal async compute scheduling remains gated to match the current DX12 runtime policy");
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::DedicatedComputeQueue, true,
                    "Metal compute command pools use a dedicated MTLCommandQueue with SharedEvent handoffs");
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::CopyQueue, true,
                    "Metal copy command pools use a dedicated MTLCommandQueue with SharedEvent handoffs");
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::Swapchain, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::FramebufferBlit, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::DepthBlit, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::CurrentSceneRenderer, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::OffscreenFramebuffers, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::FramebufferReadback, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::EditorPickingReadback, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::UITextureHandles, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::UITextureAtlasRegionUpload, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::Cubemaps, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::MultiRenderTargets, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::ExplicitBarriers, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::ParallelCommandRecording, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::ParallelCommandTranslation, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::InRenderPassChildCommandBuffers, false,
                    "Metal records independent command buffers; in-render-pass child encoders are not exposed by the RHI");
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::TransientResourceAllocator, false,
                    "Metal transient resource allocation is not implemented yet");
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::CentralizedDescriptorManagement, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::PipelineStateCache, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::HierarchicalZBuffer, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::ConservativeOcclusion, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::AsyncReadback, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph, true,
                    "Metal uses the backend-neutral RHI ImGui overlay pass and ordered swapchain submission");
                capabilities.maxTextureDimension2D = 16384u;
                capabilities.maxColorAttachments = 8u;

                for (const auto format : {
                    NLS::Render::RHI::TextureFormat::R8,
                    NLS::Render::RHI::TextureFormat::RG8,
                    NLS::Render::RHI::TextureFormat::RGB8,
                    NLS::Render::RHI::TextureFormat::RGBA8,
                    NLS::Render::RHI::TextureFormat::R16F,
                    NLS::Render::RHI::TextureFormat::RG16F,
                    NLS::Render::RHI::TextureFormat::RGBA16F,
                    NLS::Render::RHI::TextureFormat::R32F,
                    NLS::Render::RHI::TextureFormat::RG32F,
                    NLS::Render::RHI::TextureFormat::RGBA32F,
                    NLS::Render::RHI::TextureFormat::Depth32F,
                    NLS::Render::RHI::TextureFormat::Depth24Stencil8 })
                {
                    NLS::Render::RHI::TextureFormatCapability formatCapability{};
                    formatCapability.sampled = true;
                    formatCapability.upload = true;
                    formatCapability.colorAttachment =
                        format != NLS::Render::RHI::TextureFormat::Depth32F &&
                        format != NLS::Render::RHI::TextureFormat::Depth24Stencil8;
                    formatCapability.storage = formatCapability.colorAttachment;
                    const auto* descriptor = NLS::Render::RHI::GetTextureFormatDescriptor(format);
                    formatCapability.supportsSrgbView =
                        descriptor != nullptr && descriptor->supportsSrgbView;
                    capabilities.SetTextureFormatCapability(format, formatCapability);
                }
                if (device != nil && device.supportsBCTextureCompression)
                {
                    for (const auto format : {
                        NLS::Render::RHI::TextureFormat::BC1,
                        NLS::Render::RHI::TextureFormat::BC3,
                        NLS::Render::RHI::TextureFormat::BC5,
                        NLS::Render::RHI::TextureFormat::BC7,
                        NLS::Render::RHI::TextureFormat::BC6H })
                    {
                        const auto* descriptor = NLS::Render::RHI::GetTextureFormatDescriptor(format);
                        NLS::Render::RHI::TextureFormatCapability formatCapability{};
                        formatCapability.sampled = true;
                        formatCapability.upload = true;
                        formatCapability.supportsSrgbView =
                            descriptor != nullptr && descriptor->supportsSrgbView;
                        formatCapability.requiresAlignedTopLevelBlocks = true;
                        formatCapability.supportsUnalignedBlockTextures = true;
                        capabilities.SetTextureFormatCapability(format, formatCapability);
                    }
                }

                const bool supportsAppleCompressedFormats =
                    device != nil && [device supportsFamily:MTLGPUFamilyApple1];
                if (supportsAppleCompressedFormats)
                {
                    for (const auto format : {
                        NLS::Render::RHI::TextureFormat::ASTC4x4,
                        NLS::Render::RHI::TextureFormat::ETC2RGBA8 })
                    {
                        NLS::Render::RHI::TextureFormatCapability formatCapability{};
                        formatCapability.sampled = true;
                        formatCapability.upload = true;
                        formatCapability.supportsSrgbView = true;
                        formatCapability.requiresAlignedTopLevelBlocks = true;
                        formatCapability.supportsUnalignedBlockTextures = true;
                        capabilities.SetTextureFormatCapability(format, formatCapability);
                    }
                }
                capabilities.SynchronizeLegacyFields();
                return capabilities;
            }

            id<MTLDevice> m_device = nil;
            id<MTLCommandQueue> m_graphicsQueue = nil;
            id<MTLCommandQueue> m_computeQueue = nil;
            id<MTLCommandQueue> m_copyQueue = nil;
            std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
            NLS::Render::RHI::RHIDeviceCapabilities m_capabilities{};
            std::array<std::shared_ptr<NLS::Render::RHI::RHIQueue>, 3u> m_queues{};
            void* m_swapchainLayer = nullptr;
            uint32_t m_swapchainImageCount = 0u;
            void* m_platformWindow = nullptr;
            void* m_nativeWindowHandle = nullptr;
        };
    }

    std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateMetalRhiDevice(const bool debugMode)
    {
        (void)debugMode;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil)
        {
            NLS_LOG_ERROR("CreateMetalRhiDevice: MTLCreateSystemDefaultDevice returned null.");
            return nullptr;
        }

        auto result = std::make_shared<MetalDevice>(device);
        if (!result->IsBackendReady())
        {
            NLS_LOG_ERROR("CreateMetalRhiDevice: failed to create a Metal command queue.");
            return nullptr;
        }
        NLS_LOG_INFO("CreateMetalRhiDevice: created Metal device " + std::string(device.name.UTF8String));
        return result;
    }
}
