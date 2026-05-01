#include "Rendering/Resources/BindingSet.h"
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

	void BindingSet::SetTexture(const std::string& name, const std::shared_ptr<RHI::RHITexture>& texture)
	{
		for (auto& entry : m_entries)
		{
			if (entry.name == name)
			{
				entry.textureHandle = texture;
				entry.bufferHandle.reset();
				return;
			}
		}
	}

	void BindingSet::SetBuffer(const std::string& name, const std::shared_ptr<RHI::RHIBuffer>& buffer)
	{
		for (auto& entry : m_entries)
		{
			if (entry.name == name)
			{
				entry.bufferHandle = buffer;
				entry.textureHandle.reset();
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
}
