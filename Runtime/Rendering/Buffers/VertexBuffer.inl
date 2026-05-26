#pragma once

#include "Rendering/Buffers/VertexBuffer.h"
#include "Debug/Logger.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIDevice.h"

namespace
{
	std::shared_ptr<NLS::Render::RHI::RHIDevice> GetExplicitDevice()
	{
		auto& driver = NLS::Render::Context::RequireLocatedDriver("VertexBuffer");
		return NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver);
	}
}

namespace NLS::Render::Buffers
{
	template <class T>
	inline VertexBuffer<T>::VertexBuffer(
		const T* p_data,
		size_t p_elements,
		const NLS::Render::RHI::MemoryUsage memoryUsage)
	{
		if (p_data == nullptr || p_elements == 0u)
			return;

		NLS::Render::RHI::RHIBufferDesc desc;
		desc.size = p_elements * sizeof(T);
		desc.usage = NLS::Render::RHI::BufferUsageFlags::Vertex;
		desc.memoryUsage = memoryUsage;
		desc.debugName = "VertexBuffer";

		auto device = GetExplicitDevice();
		if (device != nullptr)
		{
			NLS::Render::RHI::RHIBufferUploadDesc uploadDesc;
			uploadDesc.data = p_data;
			uploadDesc.dataSize = desc.size;
			uploadDesc.debugName = "VertexBufferInitialUpload";
			m_explicitBuffer = device->CreateBuffer(desc, uploadDesc);
		}
	}

	template<class T>
	inline VertexBuffer<T>::VertexBuffer(
		const std::vector<T>& p_data,
		const NLS::Render::RHI::MemoryUsage memoryUsage) : VertexBuffer(p_data.data(), p_data.size(), memoryUsage)
	{
	}

	template<class T>
	inline bool VertexBuffer<T>::Update(
		const T* p_data,
		const size_t p_elements,
		const size_t p_destinationElementOffset)
	{
		if (m_explicitBuffer == nullptr || p_data == nullptr || p_elements == 0u)
			return false;

		NLS::Render::RHI::RHIBufferUploadDesc uploadDesc;
		uploadDesc.data = p_data;
		uploadDesc.dataSize = p_elements * sizeof(T);
		uploadDesc.destinationOffset = p_destinationElementOffset * sizeof(T);
		uploadDesc.debugName = "VertexBufferUpdate";
		return m_explicitBuffer->UpdateData(uploadDesc).Succeeded();
	}

	template<class T>
	inline VertexBuffer<T>::~VertexBuffer()
	{
		m_explicitBuffer.reset();
	}
}
