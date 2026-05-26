#pragma once

#if defined(_WIN32)

#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <memory>
#include <mutex>
#include <wrl/client.h>

namespace NLS::Render::RHI::DX12
{
	inline std::mutex& Dx12InfoQueueMessageFilterMutex()
	{
		static std::mutex mutex;
		return mutex;
	}

	class ScopedDx12InfoQueueMessageScope final
	{
	public:
		ScopedDx12InfoQueueMessageScope()
			: m_filterLock(Dx12InfoQueueMessageFilterMutex())
		{
		}

		ScopedDx12InfoQueueMessageScope(const ScopedDx12InfoQueueMessageScope&) = delete;
		ScopedDx12InfoQueueMessageScope& operator=(const ScopedDx12InfoQueueMessageScope&) = delete;

	private:
		std::unique_lock<std::mutex> m_filterLock;
	};

	// D3D12 info-queue filters are device-wide. Keep instances scoped to the
	// single API call whose unavoidable external-resource warning is denied,
	// and serialize every RTV clear through ScopedDx12InfoQueueMessageScope so
	// parallel command recording cannot hide the same warning from another
	// command list while the storage filter is active.
	class ScopedDx12InfoQueueMessageFilter final
	{
	public:
		ScopedDx12InfoQueueMessageFilter(ID3D12Device* device, const D3D12_MESSAGE_ID messageId)
		{
			if (device == nullptr)
				return;

			m_filterScope = std::make_unique<ScopedDx12InfoQueueMessageScope>();

			if (FAILED(device->QueryInterface(IID_PPV_ARGS(&m_infoQueue))) || m_infoQueue == nullptr)
			{
				m_filterScope.reset();
				return;
			}

			D3D12_MESSAGE_ID filteredMessageId = messageId;
			D3D12_INFO_QUEUE_FILTER filter{};
			filter.DenyList.NumIDs = 1;
			filter.DenyList.pIDList = &filteredMessageId;
			if (SUCCEEDED(m_infoQueue->PushStorageFilter(&filter)))
			{
				m_filterPushed = true;
			}
			else
			{
				m_filterScope.reset();
			}
		}

		~ScopedDx12InfoQueueMessageFilter()
		{
			if (m_filterPushed && m_infoQueue != nullptr)
				m_infoQueue->PopStorageFilter();
		}

		ScopedDx12InfoQueueMessageFilter(const ScopedDx12InfoQueueMessageFilter&) = delete;
		ScopedDx12InfoQueueMessageFilter& operator=(const ScopedDx12InfoQueueMessageFilter&) = delete;

	private:
		Microsoft::WRL::ComPtr<ID3D12InfoQueue> m_infoQueue;
		std::unique_ptr<ScopedDx12InfoQueueMessageScope> m_filterScope;
		bool m_filterPushed = false;
	};
}

#endif
