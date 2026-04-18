#pragma once

#include "Rendering/Core/CompositeRenderer.h"
#include <Debug/Assertion.h>
#include <stdexcept>
namespace NLS::Render::Core
{
	template<typename T, typename ...Args>
	inline T& CompositeRenderer::AddPass(const std::string& p_name, uint32_t p_order, Args&& ...p_args)
	{
		NLS_ASSERT(!m_isDrawing, "You cannot add a render pass while drawing.");
		static_assert(std::is_base_of<ARenderPass, T>::value, "T must inherit from ARenderPass");
		for (const auto& [_, pass] : m_passes)
			NLS_ASSERT(pass.first != p_name, "This pass name is already in use!");
		T* pass = new T(*this, std::forward<Args>(p_args)...);
		m_passes.emplace(p_order, std::make_pair(p_name, std::unique_ptr<ARenderPass>(pass)));
		return *pass;
	}

	template<typename T>
	inline T& CompositeRenderer::GetPass(const std::string& p_name) const
	{
		static_assert(std::is_base_of<ARenderPass, T>::value, "T should derive from ARenderPass");
		for (const auto& [_, pass] : m_passes)
		{
			if (pass.first == p_name)
			{
				return dynamic_cast<T&>(*pass.second.get());
			}
		}

		NLS_ASSERT(false, "Couldn't find a render pass matching the given type T.");
		throw std::runtime_error("Couldn't find a render pass matching the given type T.");
	}
}
