#pragma once

#include <any>
#include <unordered_map>
#include "CoreDef.h"
#define OVSERVICE(Type) Core::ServiceLocator::Get<Type>()

namespace NLS::Core
{
	/**
	* Provide a way to access to core services
	*/
	class NLS_CORE_API ServiceLocator
	{
	public:
		/**
		* Register a service in the service locator
		* @param p_service
		*/
		template<typename T>
		static void Provide(T& p_service)
		{
			__SERVICES[typeid(T).hash_code()] = std::any(&p_service);
		}
		
		/**
		* Returns a service of the given type (Make sure that your provided the service before calling this method)
		*/
		template<typename T>
		static T& Get()
		{
			return *std::any_cast<T*>(__SERVICES[typeid(T).hash_code()]);
		}

	private:
		static std::unordered_map<size_t, std::any> __SERVICES;
	};
}