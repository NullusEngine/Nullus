#pragma once
#include "Constraint.h"
#include "EngineDef.h"
namespace NLS {
	namespace Engine {
		class GameObject;
		class NLS_ENGINE_API PositionConstraint : public Constraint {
		public:
			PositionConstraint(GameObject* a, GameObject* b, float d) {
				objectA = a;
				objectB = b;
				distance = d;
			}
			~PositionConstraint() {}

			void UpdateConstraint(float dt) override;
		protected:
			GameObject* objectA;
			GameObject* objectB;
			float distance;
		};
	}
}

