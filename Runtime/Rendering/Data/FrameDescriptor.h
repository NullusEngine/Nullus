#pragma once


#include "Rendering/Buffers/Framebuffer.h"
#include "Rendering/Entities/Camera.h"

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
        Buffers::Framebuffer* outputBuffer = nullptr;

		/**
		* Ensures that the data provided in the frame descriptor is valid
		*/
		bool IsValid() const
		{
			return camera != nullptr;
		}
	};
}
