#include "Rendering/FrameGraph/FrameGraphTexture.h"

#include <Debug/Assertion.h>

#include "Rendering/Context/Driver.h"
#include "Rendering/FrameGraph/FrameGraphExecutionContext.h"

namespace NLS::Render::FrameGraph
{
	namespace
	{
		NLS::Render::RHI::TextureUsageFlags ToExplicitTextureUsage(const NLS::Render::RHI::TextureUsage usage)
		{
			NLS::Render::RHI::TextureUsageFlags flags = NLS::Render::RHI::TextureUsageFlags::None;
			if (NLS::Render::RHI::HasUsage(usage, NLS::Render::RHI::TextureUsage::Sampled))
				flags = flags | NLS::Render::RHI::TextureUsageFlags::Sampled;
			if (NLS::Render::RHI::HasUsage(usage, NLS::Render::RHI::TextureUsage::ColorAttachment))
				flags = flags | NLS::Render::RHI::TextureUsageFlags::ColorAttachment;
			if (NLS::Render::RHI::HasUsage(usage, NLS::Render::RHI::TextureUsage::DepthStencilAttachment))
				flags = flags | NLS::Render::RHI::TextureUsageFlags::DepthStencilAttachment;
			if (NLS::Render::RHI::HasUsage(usage, NLS::Render::RHI::TextureUsage::Storage))
				flags = flags | NLS::Render::RHI::TextureUsageFlags::Storage;
			return flags;
		}

		NLS::Render::RHI::RHITextureDesc ToExplicitTextureDesc(const FrameGraphTexture::Desc& desc)
		{
			NLS::Render::RHI::RHITextureDesc explicitDesc;
			explicitDesc.extent.width = desc.width;
			explicitDesc.extent.height = desc.height;
			explicitDesc.extent.depth = 1u;
			explicitDesc.dimension = desc.dimension;
			explicitDesc.format = desc.format;
			explicitDesc.arrayLayers = desc.dimension == NLS::Render::RHI::TextureDimension::TextureCube ? 6u : 1u;
			explicitDesc.usage = ToExplicitTextureUsage(desc.usage);
			explicitDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUOnly;
			explicitDesc.debugName = "FrameGraphTexture";
			return explicitDesc;
		}
	}

	void FrameGraphTexture::create(const Desc& desc, void* allocator)
	{
		auto* executionContext = static_cast<FrameGraphExecutionContext*>(allocator);
		NLS_ASSERT(executionContext != nullptr, "FrameGraphTexture requires a valid frame graph execution context");
		auto& driver = executionContext->driver;
		auto* device = executionContext->device;

		if (id != 0)
		{
			if (explicitTexture != nullptr && explicitView == nullptr && device != nullptr)
			{
				NLS::Render::RHI::RHITextureViewDesc viewDesc;
				viewDesc.format = explicitTexture->GetDesc().format;
				viewDesc.debugName = "FrameGraphTextureView";
				explicitView = device->CreateTextureView(explicitTexture, viewDesc);
			}
			return;
		}

		if (device != nullptr)
		{
			explicitTexture = device->CreateTexture(ToExplicitTextureDesc(desc));
			ownsResource = explicitTexture != nullptr;
		}

		if (explicitTexture != nullptr)
		{
			id = 0;
			NLS::Render::RHI::RHITextureViewDesc viewDesc;
			viewDesc.format = explicitTexture->GetDesc().format;
			viewDesc.debugName = "FrameGraphTextureView";
			explicitView = device != nullptr ? device->CreateTextureView(explicitTexture, viewDesc) : nullptr;
			return;
		}

		textureResource = driver.CreateTextureResource(desc.dimension);
		id = textureResource != nullptr ? textureResource->GetResourceId() : driver.CreateTexture();
		ownsResource = true;
		driver.BindTexture(desc.dimension, id);
		driver.SetupTexture(desc, nullptr);
		driver.BindTexture(desc.dimension, 0);
	}

	void FrameGraphTexture::destroy(const Desc& desc, void* allocator)
	{
		(void)desc;
		auto* executionContext = static_cast<FrameGraphExecutionContext*>(allocator);
		NLS_ASSERT(executionContext != nullptr, "FrameGraphTexture requires a valid frame graph execution context");
		auto& driver = executionContext->driver;

		if (id == 0)
			return;

		explicitView.reset();
		explicitTexture.reset();
		if (ownsResource)
		{
			if (textureResource != nullptr)
				textureResource.reset();
			else
				driver.DestroyTexture(id);
		}
		id = 0;
		ownsResource = false;
	}

	void FrameGraphTexture::preRead(const Desc& desc, uint32_t flags, void* context)
	{
		auto* executionContext = static_cast<FrameGraphExecutionContext*>(context);
		NLS_ASSERT(executionContext != nullptr, "FrameGraphTexture requires a valid frame graph execution context");
		auto& driver = executionContext->driver;

		if (executionContext->HasExplicitContext() && explicitTexture != nullptr)
			return;

		if (id == 0)
			return;

		const auto slot = flags == 0xFFFFFFFFu ? 0u : flags;
		driver.ActivateTexture(slot);
		driver.BindTexture(desc.dimension, id);
	}

	void FrameGraphTexture::preWrite(const Desc&, uint32_t, void*)
	{
		// Attachment routing stays in the pass execution path.
	}

	std::string FrameGraphTexture::toString(const Desc& desc)
	{
		return std::to_string(desc.width) + "x" + std::to_string(desc.height);
	}
}
