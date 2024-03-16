#pragma once
#include "EngineDef.h"
namespace NLS {
	namespace Engine {
		class NLS_ENGINE_API Constraint	{
		public:
			Constraint() {}
			virtual ~Constraint() {}

			virtual void UpdateConstraint(float dt) = 0;
		};
	}
}