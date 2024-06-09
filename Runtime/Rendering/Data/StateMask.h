#pragma once

#include <cstdint>

namespace NLS::Rendering::Data
{
	/**
	* State mask used by materials to override some Pipeline State settings.
	*/
	struct StateMask
	{
		union
		{
			struct
			{
				uint8_t depthWriting : 1;
				uint8_t colorWriting : 1;
				uint8_t blendable : 1;
				uint8_t depthTest : 1;
				uint8_t backfaceCulling : 1;
				uint8_t frontfaceCulling : 1;
			};

			uint8_t mask;
		};
	};
}
