#pragma once

#include <concepts>

#include "Rendering/Data/PipelineState.h"
#include "Rendering/Utils/Conversions.h"

namespace NLS::Editor::Rendering
{
    template<typename T>
    concept PipelineStateProvider = requires(const T& value)
    {
        { value.CreatePipelineState() } -> std::same_as<NLS::Render::Data::PipelineState>;
    };

    inline NLS::Render::Data::PipelineState CreateEditorUnculledPipelineState(
        NLS::Render::Data::PipelineState baseState)
    {
        baseState.culling = false;
        return baseState;
    }

    template<PipelineStateProvider T>
    inline NLS::Render::Data::PipelineState CreateEditorUnculledPipelineState(
        const T& renderer)
    {
        return CreateEditorUnculledPipelineState(renderer.CreatePipelineState());
    }

    inline NLS::Render::Data::PipelineState CreateEditorOverlayPipelineState(
        NLS::Render::Data::PipelineState baseState)
    {
        baseState.depthWriting = false;
        baseState.depthTest = false;
        baseState.culling = false;
        return baseState;
    }

    template<PipelineStateProvider T>
    inline NLS::Render::Data::PipelineState CreateEditorOverlayPipelineState(
        const T& renderer)
    {
        return CreateEditorOverlayPipelineState(renderer.CreatePipelineState());
    }

    inline NLS::Render::Data::PipelineState CreateEditorGridPipelineState(
        NLS::Render::Data::PipelineState baseState)
    {
        baseState = CreateEditorUnculledPipelineState(baseState);
        baseState.depthWriting = false;
        return baseState;
    }

    template<PipelineStateProvider T>
    inline NLS::Render::Data::PipelineState CreateEditorGridPipelineState(
        const T& renderer)
    {
        return CreateEditorGridPipelineState(renderer.CreatePipelineState());
    }

    inline NLS::Render::Data::PipelineState CreateEditorDebugLinePipelineState(
        NLS::Render::Data::PipelineState baseState)
    {
        baseState = CreateEditorUnculledPipelineState(baseState);
        baseState.depthWriting = false;
        return baseState;
    }

    template<PipelineStateProvider T>
    inline NLS::Render::Data::PipelineState CreateEditorDebugLinePipelineState(
        const T& renderer)
    {
        return CreateEditorDebugLinePipelineState(renderer.CreatePipelineState());
    }

    inline NLS::Render::Data::PipelineState CreateEditorNoDepthPipelineState(
        NLS::Render::Data::PipelineState baseState)
    {
        baseState.depthTest = false;
        return baseState;
    }

    template<PipelineStateProvider T>
    inline NLS::Render::Data::PipelineState CreateEditorNoDepthPipelineState(
        const T& renderer)
    {
        return CreateEditorNoDepthPipelineState(renderer.CreatePipelineState());
    }

    inline NLS::Render::Data::PipelineState CreateEditorOutlineStencilPipelineState(
        NLS::Render::Data::PipelineState baseState,
        uint32_t stencilMask,
        int32_t stencilReference)
    {
        baseState.stencilTest = true;
        baseState.stencilWriteMask = stencilMask;
        baseState.stencilFuncRef = stencilReference;
        baseState.stencilFuncMask = stencilMask;
        baseState.stencilOpFail = NLS::Render::Settings::EOperation::REPLACE;
        baseState.depthOpFail = NLS::Render::Settings::EOperation::REPLACE;
        baseState.bothOpFail = NLS::Render::Settings::EOperation::REPLACE;
        baseState.colorWriting.mask = 0x00;
        baseState.depthTest = false;
        baseState.depthWriting = false;
        baseState.culling = true;
        baseState.cullFace = NLS::Render::Settings::ECullFace::BACK;
        return baseState;
    }

    template<PipelineStateProvider T>
    inline NLS::Render::Data::PipelineState CreateEditorOutlineStencilPipelineState(
        const T& renderer,
        uint32_t stencilMask,
        int32_t stencilReference)
    {
        return CreateEditorOutlineStencilPipelineState(renderer.CreatePipelineState(), stencilMask, stencilReference);
    }

    inline NLS::Render::Data::PipelineState CreateEditorOutlineStrokePipelineState(
        NLS::Render::Data::PipelineState baseState,
        int32_t stencilReference,
        uint32_t stencilMask,
        float thickness)
    {
        baseState.stencilTest = true;
        baseState.stencilOpFail = NLS::Render::Settings::EOperation::KEEP;
        baseState.depthOpFail = NLS::Render::Settings::EOperation::KEEP;
        baseState.bothOpFail = NLS::Render::Settings::EOperation::REPLACE;
        baseState.stencilFuncOp = NLS::Render::Settings::EComparaisonAlgorithm::NOTEQUAL;
        baseState.stencilFuncRef = stencilReference;
        baseState.stencilFuncMask = stencilMask;
        baseState.stencilWriteMask = 0x00;
        baseState.rasterizationMode = NLS::Render::Settings::ERasterizationMode::LINE;
        baseState.lineWidthPow2 = NLS::Render::Utils::Conversions::FloatToPow2(thickness);
        baseState.depthTest = false;
        baseState.depthWriting = false;
        return baseState;
    }

    template<PipelineStateProvider T>
    inline NLS::Render::Data::PipelineState CreateEditorOutlineStrokePipelineState(
        const T& renderer,
        int32_t stencilReference,
        uint32_t stencilMask,
        float thickness)
    {
        return CreateEditorOutlineStrokePipelineState(renderer.CreatePipelineState(), stencilReference, stencilMask, thickness);
    }

    inline NLS::Render::Data::PipelineState CreateEditorOutlineShellPipelineState(
        NLS::Render::Data::PipelineState baseState,
        int32_t stencilReference,
        uint32_t stencilMask)
    {
        baseState.stencilTest = true;
        baseState.stencilOpFail = NLS::Render::Settings::EOperation::KEEP;
        baseState.depthOpFail = NLS::Render::Settings::EOperation::KEEP;
        baseState.bothOpFail = NLS::Render::Settings::EOperation::REPLACE;
        baseState.stencilFuncOp = NLS::Render::Settings::EComparaisonAlgorithm::NOTEQUAL;
        baseState.stencilFuncRef = stencilReference;
        baseState.stencilFuncMask = stencilMask;
        baseState.stencilWriteMask = 0x00;
        baseState.rasterizationMode = NLS::Render::Settings::ERasterizationMode::FILL;
        baseState.depthTest = false;
        baseState.depthWriting = false;
        baseState.culling = true;
        baseState.cullFace = NLS::Render::Settings::ECullFace::FRONT;
        return baseState;
    }

    template<PipelineStateProvider T>
    inline NLS::Render::Data::PipelineState CreateEditorOutlineShellPipelineState(
        const T& renderer,
        int32_t stencilReference,
        uint32_t stencilMask)
    {
        return CreateEditorOutlineShellPipelineState(renderer.CreatePipelineState(), stencilReference, stencilMask);
    }
}
