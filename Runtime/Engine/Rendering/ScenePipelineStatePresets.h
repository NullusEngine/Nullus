#pragma once

#include <concepts>

#include "Rendering/Data/PipelineState.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Settings/EComparaisonAlgorithm.h"

namespace NLS::Engine::Rendering
{
    template<typename T>
    concept PipelineStateProvider = requires(const T& value)
    {
        { value.CreatePipelineState() } -> std::same_as<NLS::Render::Data::PipelineState>;
    };

    inline NLS::Render::Data::PipelineState CreateSceneDefaultPipelineState(
        NLS::Render::Data::PipelineState baseState)
    {
        return baseState;
    }

    template<PipelineStateProvider T>
    inline NLS::Render::Data::PipelineState CreateSceneDefaultPipelineState(
        const T& renderer)
    {
        return CreateSceneDefaultPipelineState(renderer.CreatePipelineState());
    }

    inline NLS::Render::Data::PipelineState CreateSceneGBufferPipelineState(
        NLS::Render::Data::PipelineState baseState,
        const NLS::Render::Resources::Material& sourceMaterial)
    {
        baseState.depthTest = sourceMaterial.HasDepthTest();
        baseState.depthWriting = sourceMaterial.HasDepthWriting();
        baseState.colorWriting.mask = 0xFF;
        baseState.culling = sourceMaterial.HasBackfaceCulling() || sourceMaterial.HasFrontfaceCulling();
        baseState.cullFace = sourceMaterial.HasBackfaceCulling() && sourceMaterial.HasFrontfaceCulling()
            ? NLS::Render::Settings::ECullFace::FRONT_AND_BACK
            : sourceMaterial.HasFrontfaceCulling()
                ? NLS::Render::Settings::ECullFace::FRONT
                : NLS::Render::Settings::ECullFace::BACK;
        return baseState;
    }

    template<PipelineStateProvider T>
    inline NLS::Render::Data::PipelineState CreateSceneGBufferPipelineState(
        const T& renderer,
        const NLS::Render::Resources::Material& sourceMaterial)
    {
        return CreateSceneGBufferPipelineState(CreateSceneDefaultPipelineState(renderer), sourceMaterial);
    }

    inline NLS::Render::Data::PipelineState CreateSceneSkyboxPipelineState(
        NLS::Render::Data::PipelineState baseState)
    {
        baseState.depthFunc = NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL;
        return baseState;
    }

    template<PipelineStateProvider T>
    inline NLS::Render::Data::PipelineState CreateSceneSkyboxPipelineState(
        const T& renderer)
    {
        return CreateSceneSkyboxPipelineState(CreateSceneDefaultPipelineState(renderer));
    }

    inline NLS::Render::Data::PipelineState CreateSceneFullscreenCompositePipelineState(
        NLS::Render::Data::PipelineState baseState)
    {
        baseState.depthTest = false;
        baseState.depthWriting = false;
        baseState.culling = false;
        baseState.colorWriting.mask = 0x0F;
        return baseState;
    }

    template<PipelineStateProvider T>
    inline NLS::Render::Data::PipelineState CreateSceneFullscreenCompositePipelineState(
        const T& renderer)
    {
        return CreateSceneFullscreenCompositePipelineState(CreateSceneDefaultPipelineState(renderer));
    }
}
