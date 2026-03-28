#include "Rendering/Resources/BindingSet.h"
#include "Rendering/RHI/IRHIResource.h"
#include "Rendering/Resources/Texture.h"

namespace NLS::Render::Resources
{
	void BindingSet::Clear()
	{
		m_entries.clear();
	}

	void BindingSet::SetLayout(const ResourceBindingLayout& layout)
	{
		m_entries.clear();
		m_entries.reserve(layout.bindings.size());

		for (const auto& binding : layout.bindings)
		{
			m_entries.push_back({
				binding.name,
				binding.kind,
				binding.bindingSpace,
				binding.bindingIndex,
				binding.slot,
				nullptr,
				nullptr,
				nullptr,
				nullptr,
				nullptr,
				nullptr,
				{},
				false
			});
		}
	}

	void BindingSet::SetSampler(const std::string& name, const RHI::SamplerDesc& sampler)
	{
		for (auto& entry : m_entries)
		{
			if (entry.name == name)
			{
				entry.sampler = sampler;
				entry.hasSampler = true;
				return;
			}
		}
	}

	void BindingSet::SetTexture(const std::string& name, const Texture* texture)
	{
		for (auto& entry : m_entries)
		{
			if (entry.name == name)
			{
				entry.texture = texture;
				entry.textureHandle = texture ? texture->GetTextureHandle() : nullptr;
				entry.textureResource = texture ? texture->GetRHITexture() : nullptr;
				entry.resource = entry.textureResource;
				entry.bufferResource = nullptr;
				entry.bufferHandle.reset();
				return;
			}
		}
	}

	void BindingSet::SetBuffer(const std::string& name, const std::shared_ptr<RHI::RHIBuffer>& buffer, const RHI::IRHIBuffer* compatibilityBuffer)
	{
		for (auto& entry : m_entries)
		{
			if (entry.name == name)
			{
				entry.bufferHandle = buffer;
				entry.bufferResource = compatibilityBuffer;
				entry.resource = compatibilityBuffer;
				entry.textureHandle.reset();
				entry.textureResource = nullptr;
				entry.texture = nullptr;
				return;
			}
		}
	}

	void BindingSet::SetResource(const std::string& name, const RHI::IRHIResource* resource)
	{
		for (auto& entry : m_entries)
		{
			if (entry.name == name)
			{
				entry.resource = resource;
				entry.textureResource = resource && resource->GetResourceType() == RHI::RHIResourceType::Texture
					? static_cast<const RHI::IRHITexture*>(resource)
					: nullptr;
				entry.bufferResource = resource && resource->GetResourceType() == RHI::RHIResourceType::Buffer
					? static_cast<const RHI::IRHIBuffer*>(resource)
					: nullptr;
				entry.textureHandle.reset();
				entry.bufferHandle.reset();
				if (!entry.textureResource)
					entry.texture = nullptr;
				return;
			}
		}
	}

	void BindingSet::SetBuffer(const std::string& name, const RHI::IRHIBuffer* buffer)
	{
		for (auto& entry : m_entries)
		{
			if (entry.name == name)
			{
				entry.bufferResource = buffer;
				entry.resource = buffer;
				entry.textureResource = nullptr;
				entry.textureHandle.reset();
				entry.bufferHandle.reset();
				entry.texture = nullptr;
				return;
			}
		}
	}

	const RHI::SamplerDesc* BindingSet::GetSampler(const std::string& name) const
	{
		for (const auto& entry : m_entries)
		{
			if (entry.name == name && entry.hasSampler)
				return &entry.sampler;
		}

		return nullptr;
	}

	const Texture* BindingSet::GetTexture(const std::string& name) const
	{
		for (const auto& entry : m_entries)
		{
			if (entry.name == name)
				return entry.texture;
		}

		return nullptr;
	}

	std::shared_ptr<RHI::RHITexture> BindingSet::GetTextureHandle(const std::string& name) const
	{
		for (const auto& entry : m_entries)
		{
			if (entry.name == name)
				return entry.textureHandle;
		}

		return nullptr;
	}

	std::shared_ptr<RHI::RHIBuffer> BindingSet::GetBufferHandle(const std::string& name) const
	{
		for (const auto& entry : m_entries)
		{
			if (entry.name == name)
				return entry.bufferHandle;
		}

		return nullptr;
	}

	const RHI::IRHIResource* BindingSet::GetResource(const std::string& name) const
	{
		for (const auto& entry : m_entries)
		{
			if (entry.name == name)
				return entry.resource;
		}

		return nullptr;
	}

	const RHI::IRHIBuffer* BindingSet::GetBuffer(const std::string& name) const
	{
		for (const auto& entry : m_entries)
		{
			if (entry.name == name)
				return entry.bufferResource;
		}

		return nullptr;
	}

	const ResourceBindingEntry* BindingSet::Find(const std::string& name) const
	{
		for (const auto& entry : m_entries)
		{
			if (entry.name == name)
				return &entry;
		}

		return nullptr;
	}

	const std::vector<ResourceBindingEntry>& BindingSet::Entries() const
	{
		return m_entries;
	}
}
