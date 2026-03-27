#pragma once

#include <fg/Blackboard.hpp>
#include <fg/FrameGraph.hpp>

#include <Rendering/Data/FrameDescriptor.h>
#include <Rendering/FrameGraph/FrameGraphTexture.h>
#include <Rendering/RHI/RHITypes.h>

namespace NLS::Engine::Rendering
{
	struct SceneRenderTargetsData
	{
		FrameGraphResource color = -1;
		FrameGraphResource depth = -1;
	};

	inline NLS::Render::FrameGraph::FrameGraphTexture::Desc MakeSceneColorTargetDesc(uint16_t width, uint16_t height)
	{
		NLS::Render::FrameGraph::FrameGraphTexture::Desc desc;
		desc.width = width;
		desc.height = height;
		desc.format = NLS::Render::RHI::TextureFormat::RGB8;
		desc.usage = NLS::Render::RHI::TextureUsage::ColorAttachment;
		return desc;
	}

	inline NLS::Render::FrameGraph::FrameGraphTexture::Desc MakeSceneDepthTargetDesc(uint16_t width, uint16_t height)
	{
		NLS::Render::FrameGraph::FrameGraphTexture::Desc desc;
		desc.width = width;
		desc.height = height;
		desc.format = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
		desc.usage = NLS::Render::RHI::TextureUsage::DepthStencilAttachment;
		return desc;
	}

	inline void ImportSceneRenderTargets(FrameGraph& frameGraph, FrameGraphBlackboard& blackboard, const NLS::Render::Data::FrameDescriptor& frame, const char* colorResourceName, const char* depthResourceName)
	{
		if (!frame.outputBuffer)
			return;

		SceneRenderTargetsData targets;
		targets.color = frameGraph.import<NLS::Render::FrameGraph::FrameGraphTexture>(
			colorResourceName,
			MakeSceneColorTargetDesc(frame.renderWidth, frame.renderHeight),
			NLS::Render::FrameGraph::FrameGraphTexture::WrapExternal(
				frame.outputBuffer->GetExplicitTextureHandle(),
				frame.outputBuffer->GetOrCreateExplicitColorView("SceneOutputColorView"),
				frame.outputBuffer->GetTextureID())
		);
		targets.depth = frameGraph.import<NLS::Render::FrameGraph::FrameGraphTexture>(
			depthResourceName,
			MakeSceneDepthTargetDesc(frame.renderWidth, frame.renderHeight),
			NLS::Render::FrameGraph::FrameGraphTexture::WrapExternal(
				frame.outputBuffer->GetExplicitDepthStencilTextureHandle(),
				frame.outputBuffer->GetOrCreateExplicitDepthStencilView("SceneOutputDepthView"),
				frame.outputBuffer->GetDepthStencilTextureID())
		);

		blackboard.add<SceneRenderTargetsData>(targets);
	}
}
