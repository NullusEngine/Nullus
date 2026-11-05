#pragma once

#include <memory>
#include <optional>

#include "Math/Vector4.h"
#include "Rendering/Buffers/Framebuffer.h"
#include "Rendering/Entities/Camera.h"

namespace NLS::Render::RHI
{
	class RHITexture;
	class RHITextureView;
}
namespace NLS::Render::UI { struct UiDrawDataSnapshot; }

namespace NLS::Render::Data
{
	/**
	* Describe how a given frame should be rendered
	*/
	struct FrameDescriptor
	{
		uint16_t renderWidth = 0;
		uint16_t renderHeight = 0;
			NLS::Render::Entities::Camera* camera = nullptr;
			std::optional<NLS::Maths::Vector4> clearColorOverride;
            bool applyToneMapping = true;
            // Camera projection uses renderWidth/Height; attachments can be a larger shared output.
            std::optional<NLS::Maths::Vector4> outputViewport;
            std::shared_ptr<const UI::UiDrawDataSnapshot> gameUiSnapshot;
            std::optional<double> gameTime;
            uint64_t playSessionId = 0;
            uint64_t simulationFrame = 0;
	        Buffers::Framebuffer* outputBuffer = nullptr;
		std::shared_ptr<NLS::Render::RHI::RHITexture> outputColorTexture;
		std::shared_ptr<NLS::Render::RHI::RHITexture> outputDepthStencilTexture;
		std::shared_ptr<NLS::Render::RHI::RHITextureView> outputColorView;
		std::shared_ptr<NLS::Render::RHI::RHITextureView> outputDepthStencilView;

		/**
		* Ensures that the data provided in the frame descriptor is valid
		*/
		bool IsValid() const
		{
			return camera != nullptr;
			}
		};
	}
