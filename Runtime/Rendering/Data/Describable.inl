#pragma once

#include <Debug/Assertion.h>

#include "Rendering/Data/Describable.h"

namespace NLS::Render::Data
{
	template<typename T>
	inline void Describable::AddDescriptor(T&& p_descriptor)
	{
		NLS_ASSERT(!HasDescriptor<T>(), "Descriptor already added");
		m_descriptors.emplace(typeid(T), std::move(p_descriptor));
	}

	template<typename T>
	inline void Describable::RemoveDescriptor()
	{
		NLS_ASSERT(!HasDescriptor<T>(), "Descriptor doesn't exist.");

		auto it = m_descriptors.find(typeid(T));
		if (it != m_descriptors.end())
		{
			m_descriptors.erase(it);
		}
	}

	template<typename T>
	inline bool Describable::HasDescriptor() const
	{
		auto it = m_descriptors.find(typeid(T));
		return it != m_descriptors.end();
	}

	template<typename T>
	inline const T& Describable::GetDescriptor() const
	{
		auto it = m_descriptors.find(typeid(T));
		NLS_ASSERT(it != m_descriptors.end(), "Couldn't find a descriptor matching the given type T.");
		return std::any_cast<const T&>(it->second);
	}

	template<typename T>
	inline bool Describable::TryGetDescriptor(T& p_outDescriptor) const
	{
		auto it = m_descriptors.find(typeid(T));
		if (it != m_descriptors.end())
		{
			p_outDescriptor = std::any_cast<const T&>(it->second);
			return true;
		}

		return false;
	}
}
