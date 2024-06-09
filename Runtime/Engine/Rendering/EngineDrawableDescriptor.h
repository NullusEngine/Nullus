#pragma once

#include <Math/Matrix4.h>

namespace NLS::Engine::Rendering
{
	/**
	* Descriptor for drawable entities that adds a model and a user matrix.
	* This descriptor, when added on a drawable, is read by the EngineBufferRenderFeature
	* and its data is uploaded to the GPU before issuing a draw call.
	*/
	struct EngineDrawableDescriptor
	{
		Maths::Matrix4 modelMatrix;
		Maths::Matrix4 userMatrix;
	};
}
