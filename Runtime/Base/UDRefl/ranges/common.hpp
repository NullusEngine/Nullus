#pragma once

#include "../Object.hpp"

#include "../Info.hpp" 

namespace NLS::UDRefl::Ranges {
	struct NLS_BASE_API Derived {
		ObjectView obj;
		TypeInfo* typeinfo; // not nullptr
		std::unordered_map<Type, BaseInfo>::iterator curbase;

		NLS_BASE_API friend bool operator==(const Derived& lhs, const Derived& rhs) {
			return lhs.obj.GetType() == rhs.obj.GetType()
				&& lhs.obj.GetPtr() == rhs.obj.GetPtr()
				&& lhs.curbase == rhs.curbase;
		}
	};
}
