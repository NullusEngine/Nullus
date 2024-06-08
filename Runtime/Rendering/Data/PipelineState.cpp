/**
* @project: Overload
* @author: Overload Tech.
* @licence: MIT
*/

#include "Rendering/Data/PipelineState.h"

Rendering::Data::PipelineState::PipelineState() :
    colorWriting{true, true, true, true},
    depthWriting(true),
    culling(true),
    dither(false),
    polygonOffsetFill(false),
    sampleAlphaToCoverage(false),
    depthTest(true),
    scissorTest(false),
    stencilTest(false),
    multisample(true),
    rasterizationMode(Settings::ERasterizationMode::FILL),
    stencilFuncOp(Settings::EComparaisonAlgorithm::ALWAYS),
    stencilFuncRef(0x00),
    stencilFuncMask(0xFF),
    stencilWriteMask(0xFF),
    stencilOpFail(Settings::EOperation::KEEP),
    depthOpFail(Settings::EOperation::KEEP),
    bothOpFail(Settings::EOperation::KEEP),
    depthFunc(Settings::EComparaisonAlgorithm::LESS),
    cullFace(Settings::ECullFace::BACK),
    lineWidthPow2(0x00)
{
}
