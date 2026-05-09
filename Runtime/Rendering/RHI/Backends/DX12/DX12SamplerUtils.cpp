#include "Rendering/RHI/Backends/DX12/DX12SamplerUtils.h"

#include <algorithm>

namespace NLS::Render::RHI::DX12
{
#if defined(_WIN32)
    D3D12_FILTER ToD3D12Filter(
        TextureFilter minFilter,
        TextureFilter magFilter,
        TextureMipFilter mipFilter,
        uint32_t maxAnisotropy,
        bool comparison)
    {
        if (maxAnisotropy > 1u)
            return comparison ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC;

        const bool minLinear = minFilter == TextureFilter::Linear;
        const bool magLinear = magFilter == TextureFilter::Linear;
        const bool mipLinear = mipFilter == TextureMipFilter::Linear;

        if (!comparison)
        {
            if (!minLinear && !magLinear && !mipLinear) return D3D12_FILTER_MIN_MAG_MIP_POINT;
            if (!minLinear && !magLinear && mipLinear) return D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
            if (!minLinear && magLinear && !mipLinear) return D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
            if (!minLinear && magLinear && mipLinear) return D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR;
            if (minLinear && !magLinear && !mipLinear) return D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT;
            if (minLinear && !magLinear && mipLinear) return D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
            if (minLinear && magLinear && !mipLinear) return D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
            return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        }

        if (!minLinear && !magLinear && !mipLinear) return D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        if (!minLinear && !magLinear && mipLinear) return D3D12_FILTER_COMPARISON_MIN_MAG_POINT_MIP_LINEAR;
        if (!minLinear && magLinear && !mipLinear) return D3D12_FILTER_COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT;
        if (!minLinear && magLinear && mipLinear) return D3D12_FILTER_COMPARISON_MIN_POINT_MAG_MIP_LINEAR;
        if (minLinear && !magLinear && !mipLinear) return D3D12_FILTER_COMPARISON_MIN_LINEAR_MAG_MIP_POINT;
        if (minLinear && !magLinear && mipLinear) return D3D12_FILTER_COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
        if (minLinear && magLinear && !mipLinear) return D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        return D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    }

    D3D12_TEXTURE_ADDRESS_MODE ToD3D12AddressMode(TextureWrap wrap)
    {
        switch (wrap)
        {
        case TextureWrap::ClampToEdge: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case TextureWrap::MirrorRepeat: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case TextureWrap::ClampToBorder: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        case TextureWrap::Repeat:
        default:
            return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        }
    }

    D3D12_COMPARISON_FUNC ToD3D12ComparisonFunc(NLS::Render::Settings::EComparaisonAlgorithm algorithm)
    {
        switch (algorithm)
        {
        case NLS::Render::Settings::EComparaisonAlgorithm::NEVER: return D3D12_COMPARISON_FUNC_NEVER;
        case NLS::Render::Settings::EComparaisonAlgorithm::LESS: return D3D12_COMPARISON_FUNC_LESS;
        case NLS::Render::Settings::EComparaisonAlgorithm::EQUAL: return D3D12_COMPARISON_FUNC_EQUAL;
        case NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case NLS::Render::Settings::EComparaisonAlgorithm::GREATER: return D3D12_COMPARISON_FUNC_GREATER;
        case NLS::Render::Settings::EComparaisonAlgorithm::NOTEQUAL: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case NLS::Render::Settings::EComparaisonAlgorithm::GREATER_EQUAL: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case NLS::Render::Settings::EComparaisonAlgorithm::ALWAYS:
        default:
            return D3D12_COMPARISON_FUNC_ALWAYS;
        }
    }

    D3D12_SAMPLER_DESC BuildDX12SamplerDesc(const SamplerDesc& desc)
    {
        D3D12_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = ToD3D12Filter(
            desc.minFilter,
            desc.magFilter,
            desc.mipFilter,
            desc.maxAnisotropy,
            desc.compareEnabled);
        samplerDesc.AddressU = ToD3D12AddressMode(desc.wrapU);
        samplerDesc.AddressV = ToD3D12AddressMode(desc.wrapV);
        samplerDesc.AddressW = ToD3D12AddressMode(desc.wrapW);
        samplerDesc.MipLODBias = desc.mipLodBias;
        samplerDesc.MaxAnisotropy = (std::max)(1u, desc.maxAnisotropy);
        samplerDesc.ComparisonFunc = desc.compareEnabled
            ? ToD3D12ComparisonFunc(desc.compareFunc)
            : D3D12_COMPARISON_FUNC_NEVER;
        samplerDesc.BorderColor[0] = desc.borderColor[0];
        samplerDesc.BorderColor[1] = desc.borderColor[1];
        samplerDesc.BorderColor[2] = desc.borderColor[2];
        samplerDesc.BorderColor[3] = desc.borderColor[3];
        samplerDesc.MinLOD = desc.minLod;
        samplerDesc.MaxLOD = desc.maxLod;
        return samplerDesc;
    }
#endif
}
