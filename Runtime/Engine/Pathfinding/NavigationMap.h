#pragma once
#include "Vector3.h"
#include "NavigationPath.h"
#include "EngineDef.h"
namespace NLS {
	using namespace NLS::Maths;
	namespace Engine {
		class NLS_ENGINE_API NavigationMap
		{
		public:
			NavigationMap() {}
			~NavigationMap() {}

			virtual bool FindPath(const Vector3& from, const Vector3& to, NavigationPath& outPath) = 0;
		};
	}
}

