#pragma once

#include "Debug/Assertion.h"
#include "Core/ServiceLocator.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Buffers/VertexBuffer.h"
#include "Rendering/RHI/Backends/OpenGL/Compat/ExplicitRHICompat.h"

namespace NLS::Render::Buffers
{
	template <class T>
	inline VertexBuffer<T>::VertexBuffer(T* p_data, size_t p_elements)
	{
		NLS_ASSERT(NLS::Core::ServiceLocator::Contains<NLS::Render::Context::Driver>(), "VertexBuffer requires an initialized Driver.");
		auto& driver = NLS_SERVICE(NLS::Render::Context::Driver);
		m_bufferResource = driver.CreateBufferResource(NLS::Render::RHI::BufferType::Vertex);
		if (const auto explicitDevice = driver.GetExplicitDevice(); explicitDevice != nullptr)
		{
			NLS::Render::RHI::RHIBufferDesc desc;
			desc.size = p_elements * sizeof(T);
			desc.usage = NLS::Render::RHI::BufferUsageFlags::Vertex;
			desc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUOnly;
			desc.debugName = "VertexBuffer";
			m_explicitBuffer = explicitDevice->CreateBuffer(desc, p_data);
		}
		else
		{
			m_explicitBuffer = m_bufferResource
				? NLS::Render::RHI::WrapCompatibilityBuffer(m_bufferResource, "VertexBuffer")
				: nullptr;
		}
		m_bufferID = m_bufferResource ? m_bufferResource->GetResourceId() : 0;
		driver.BindBuffer(NLS::Render::RHI::BufferType::Vertex, m_bufferID);
		driver.SetBufferData(NLS::Render::RHI::BufferType::Vertex, p_elements * sizeof(T), p_data, NLS::Render::RHI::BufferUsage::StaticDraw);
	}

	template<class T>
	inline VertexBuffer<T>::VertexBuffer(std::vector<T>& p_data) : VertexBuffer(p_data.data(), p_data.size())
	{
	}

	template<class T>
	inline VertexBuffer<T>::~VertexBuffer()
	{
		if (m_bufferResource)
			m_bufferResource.reset();
		else if (m_bufferID != 0)
		{
			NLS_ASSERT(NLS::Core::ServiceLocator::Contains<NLS::Render::Context::Driver>(), "VertexBuffer requires an initialized Driver.");
			NLS_SERVICE(NLS::Render::Context::Driver).DestroyBuffer(m_bufferID);
		}
		m_explicitBuffer.reset();
	}

	template <class T>
	inline void VertexBuffer<T>::Bind()
	{
		NLS_ASSERT(NLS::Core::ServiceLocator::Contains<NLS::Render::Context::Driver>(), "VertexBuffer requires an initialized Driver.");
		NLS_SERVICE(NLS::Render::Context::Driver).BindBuffer(NLS::Render::RHI::BufferType::Vertex, m_bufferID);
	}

	template <class T>
	inline void VertexBuffer<T>::Unbind()
	{
		NLS_ASSERT(NLS::Core::ServiceLocator::Contains<NLS::Render::Context::Driver>(), "VertexBuffer requires an initialized Driver.");
		NLS_SERVICE(NLS::Render::Context::Driver).BindBuffer(NLS::Render::RHI::BufferType::Vertex, 0);
	}

	template <class T>
	inline uint32_t VertexBuffer<T>::GetID()
	{
		return m_bufferID;
	}
}
