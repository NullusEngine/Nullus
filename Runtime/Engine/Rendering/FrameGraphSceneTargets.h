#pragma once

#include "Rendering/FrameGraph/ExternalResourceBridge.h"

namespace NLS::Engine::Rendering
{
    inline NLS::Render::FrameGraph::FrameGraphTexture::Desc MakeSceneColorTargetDesc(uint16_t width, uint16_t height)
    {
        return NLS::Render::FrameGraph::MakeSceneColorTargetDesc(width, height);
    }

    inline NLS::Render::FrameGraph::FrameGraphTexture::Desc MakeSceneDepthTargetDesc(uint16_t width, uint16_t height)
    {
        return NLS::Render::FrameGraph::MakeSceneDepthTargetDesc(width, height);
    }
}
