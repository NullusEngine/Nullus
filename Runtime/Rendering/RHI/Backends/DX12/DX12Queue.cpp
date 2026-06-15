#include "Rendering/RHI/Backends/DX12/DX12Queue.h"

#include "Profiling/Profiler.h"
#include "Rendering/RHI/Backends/DX12/DX12PresentPolicy.h"
#include "Rendering/RHI/Backends/DX12/DX12QueueSynchronization.h"
#include "Rendering/RHI/Backends/DX12/DX12Synchronization.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"

#include <cstdint>
#include <string>
#include <vector>

#include <Debug/Logger.h>

#if defined(_WIN32)
#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3d12video.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif

namespace NLS::Render::Backend
{
#if defined(_WIN32)
	namespace
	{
		std::wstring Utf8ToWideString(const std::string& value)
		{
			if (value.empty())
				return {};

			const int requiredChars = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
			if (requiredChars <= 1)
				return {};

			std::wstring wide(static_cast<size_t>(requiredChars), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, wide.data(), requiredChars);
			wide.resize(static_cast<size_t>(requiredChars - 1));
			return wide;
		}

		void SetDx12ObjectName(ID3D12Object* object, const std::string& debugName)
		{
			if (object == nullptr || debugName.empty())
				return;

			const std::wstring wideName = Utf8ToWideString(debugName);
			if (!wideName.empty())
				object->SetName(wideName.c_str());
		}

		bool ShouldLogDx12ValidationMessages()
		{
			return NLS::Render::Settings::GetThreadDiagnosticsSettings().dx12LogMessages;
		}

		bool ShouldLogDx12FrameFlow()
		{
			return NLS::Render::Settings::GetThreadDiagnosticsSettings().dx12LogFrameFlow;
		}

		NLS::Render::RHI::RHIQueueOperationResult ClassifyDx12QueuedWorkFailure(
			ID3D12Device* device,
			const std::string& message,
			const bool mayHaveQueuedGpuWork,
			const bool frameFenceSignalQueued = false)
		{
			if (device != nullptr)
			{
				const HRESULT deviceStatus = device->GetDeviceRemovedReason();
				if (FAILED(deviceStatus))
				{
					return {
						NLS::Render::RHI::RHIQueueOperationStatusCode::DeviceLost,
						message + " deviceRemovedHr=" + std::to_string(deviceStatus),
						mayHaveQueuedGpuWork,
						frameFenceSignalQueued
					};
				}
			}

			return {
				NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure,
				message,
				mayHaveQueuedGpuWork,
				frameFenceSignalQueued
			};
		}

		void LogDx12DebugMessages(ID3D12Device* device, const std::string& context)
		{
			if (device == nullptr)
				return;

			Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
			if (FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue))) || infoQueue == nullptr)
				return;

			const UINT64 messageCount = infoQueue->GetNumStoredMessages();
			if (messageCount == 0)
				return;

			const UINT64 firstMessage = messageCount > 8 ? messageCount - 8 : 0;
			for (UINT64 messageIndex = firstMessage; messageIndex < messageCount; ++messageIndex)
			{
				SIZE_T messageSize = 0;
				if (FAILED(infoQueue->GetMessage(messageIndex, nullptr, &messageSize)) || messageSize == 0)
					continue;

				std::vector<char> messageBytes(messageSize);
				auto* message = reinterpret_cast<D3D12_MESSAGE*>(messageBytes.data());
				if (FAILED(infoQueue->GetMessage(messageIndex, message, &messageSize)))
					continue;

				NLS_LOG_ERROR(
					context +
					": D3D12 message id=" + std::to_string(message->ID) +
					" severity=" + std::to_string(static_cast<int>(message->Severity)) +
					" text=" + std::string(message->pDescription));
			}

			infoQueue->ClearStoredMessages();
		}

		std::string DredOperationName(const D3D12_AUTO_BREADCRUMB_OP operation)
		{
			switch (operation)
			{
			case D3D12_AUTO_BREADCRUMB_OP_SETMARKER:
				return "SetMarker";
			case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT:
				return "BeginEvent";
			case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT:
				return "EndEvent";
			case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED:
				return "DrawInstanced";
			case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED:
				return "DrawIndexedInstanced";
			case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT:
				return "ExecuteIndirect";
			case D3D12_AUTO_BREADCRUMB_OP_DISPATCH:
				return "Dispatch";
			case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION:
				return "CopyBufferRegion";
			case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION:
				return "CopyTextureRegion";
			case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE:
				return "CopyResource";
			case D3D12_AUTO_BREADCRUMB_OP_COPYTILES:
				return "CopyTiles";
			case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE:
				return "ResolveSubresource";
			case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW:
				return "ClearRenderTargetView";
			case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW:
				return "ClearUnorderedAccessView";
			case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW:
				return "ClearDepthStencilView";
			case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER:
				return "ResourceBarrier";
			case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE:
				return "ExecuteBundle";
			case D3D12_AUTO_BREADCRUMB_OP_PRESENT:
				return "Present";
			case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA:
				return "ResolveQueryData";
			case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION:
				return "BeginSubmission";
			case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION:
				return "EndSubmission";
			case D3D12_AUTO_BREADCRUMB_OP_DECODEFRAME:
				return "DecodeFrame";
			case D3D12_AUTO_BREADCRUMB_OP_PROCESSFRAMES:
				return "ProcessFrames";
			case D3D12_AUTO_BREADCRUMB_OP_ATOMICCOPYBUFFERUINT:
				return "AtomicCopyBufferUint";
			case D3D12_AUTO_BREADCRUMB_OP_ATOMICCOPYBUFFERUINT64:
				return "AtomicCopyBufferUint64";
			case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCEREGION:
				return "ResolveSubresourceRegion";
			case D3D12_AUTO_BREADCRUMB_OP_WRITEBUFFERIMMEDIATE:
				return "WriteBufferImmediate";
			case D3D12_AUTO_BREADCRUMB_OP_DECODEFRAME1:
				return "DecodeFrame1";
			case D3D12_AUTO_BREADCRUMB_OP_SETPROTECTEDRESOURCESESSION:
				return "SetProtectedResourceSession";
			case D3D12_AUTO_BREADCRUMB_OP_DECODEFRAME2:
				return "DecodeFrame2";
			case D3D12_AUTO_BREADCRUMB_OP_PROCESSFRAMES1:
				return "ProcessFrames1";
			case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE:
				return "BuildRaytracingAccelerationStructure";
			case D3D12_AUTO_BREADCRUMB_OP_EMITRAYTRACINGACCELERATIONSTRUCTUREPOSTBUILDINFO:
				return "EmitRaytracingAccelerationStructurePostbuildInfo";
			case D3D12_AUTO_BREADCRUMB_OP_COPYRAYTRACINGACCELERATIONSTRUCTURE:
				return "CopyRaytracingAccelerationStructure";
			case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS:
				return "DispatchRays";
			case D3D12_AUTO_BREADCRUMB_OP_INITIALIZEMETACOMMAND:
				return "InitializeMetaCommand";
			case D3D12_AUTO_BREADCRUMB_OP_EXECUTEMETACOMMAND:
				return "ExecuteMetaCommand";
			case D3D12_AUTO_BREADCRUMB_OP_ESTIMATEMOTION:
				return "EstimateMotion";
			case D3D12_AUTO_BREADCRUMB_OP_RESOLVEMOTIONVECTORHEAP:
				return "ResolveMotionVectorHeap";
			case D3D12_AUTO_BREADCRUMB_OP_SETPIPELINESTATE1:
				return "SetPipelineState1";
			case D3D12_AUTO_BREADCRUMB_OP_INITIALIZEEXTENSIONCOMMAND:
				return "InitializeExtensionCommand";
			case D3D12_AUTO_BREADCRUMB_OP_EXECUTEEXTENSIONCOMMAND:
				return "ExecuteExtensionCommand";
			default:
				return "Unknown(" + std::to_string(static_cast<int>(operation)) + ")";
			}
		}

		void LogDx12DredBreadcrumbs(ID3D12Device* device, const std::string& context)
		{
			if (device == nullptr)
				return;

			Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData1> dred1;
			const HRESULT dred1Hr = device->QueryInterface(IID_PPV_ARGS(&dred1));
			if (SUCCEEDED(dred1Hr) && dred1 != nullptr)
			{
				D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
				if (SUCCEEDED(dred1->GetAutoBreadcrumbsOutput1(&breadcrumbs)))
				{
					uint32_t nodeIndex = 0u;
					for (auto* node = breadcrumbs.pHeadAutoBreadcrumbNode; node != nullptr; node = node->pNext)
					{
						const UINT executed =
							node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0u;
						const UINT operationCount = node->BreadcrumbCount;
						const UINT lastOperationIndex = executed > 0u ? executed - 1u : 0u;
						const auto operation = node->pCommandHistory != nullptr && lastOperationIndex < operationCount
							? DredOperationName(node->pCommandHistory[lastOperationIndex])
							: std::string{ "Unavailable" };
						NLS_LOG_ERROR(
							context +
							": DRED breadcrumb node=" + std::to_string(nodeIndex) +
							" commandList=\"" + (node->pCommandListDebugNameA != nullptr ? node->pCommandListDebugNameA : "") + "\"" +
							" commandQueue=\"" + (node->pCommandQueueDebugNameA != nullptr ? node->pCommandQueueDebugNameA : "") + "\"" +
							" executed=" + std::to_string(executed) +
							"/" + std::to_string(operationCount) +
							" lastOp=" + operation);
						++nodeIndex;
					}
				}

				D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFault{};
				if (SUCCEEDED(dred1->GetPageFaultAllocationOutput1(&pageFault)) &&
					pageFault.PageFaultVA != 0u)
				{
					NLS_LOG_ERROR(
						context +
						": DRED page fault VA=" + std::to_string(pageFault.PageFaultVA));
				}
				return;
			}

			Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> dred;
			if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))) || dred == nullptr)
				return;

			D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs{};
			if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs)))
			{
				uint32_t nodeIndex = 0u;
				for (auto* node = breadcrumbs.pHeadAutoBreadcrumbNode; node != nullptr; node = node->pNext)
				{
					const UINT executed =
						node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0u;
					const UINT operationCount = node->BreadcrumbCount;
					const UINT lastOperationIndex = executed > 0u ? executed - 1u : 0u;
					const auto operation = node->pCommandHistory != nullptr && lastOperationIndex < operationCount
						? DredOperationName(node->pCommandHistory[lastOperationIndex])
						: std::string{ "Unavailable" };
					NLS_LOG_ERROR(
						context +
						": DRED breadcrumb node=" + std::to_string(nodeIndex) +
						" commandList=\"" + (node->pCommandListDebugNameA != nullptr ? node->pCommandListDebugNameA : "") + "\"" +
						" commandQueue=\"" + (node->pCommandQueueDebugNameA != nullptr ? node->pCommandQueueDebugNameA : "") + "\"" +
						" executed=" + std::to_string(executed) +
						"/" + std::to_string(operationCount) +
						" lastOp=" + operation);
					++nodeIndex;
				}
			}
		}

		NLS::Render::RHI::RHIQueueOperationResult ValidateDX12SemaphoreWaitValue(
			const NativeDX12Semaphore& semaphore,
			const std::string& context)
		{
			const UINT64 waitValue = semaphore.GetWaitValue();
			if (waitValue != 0u)
				return {};

			const std::string message =
				context +
				": refusing to wait on an unsignaled DX12 semaphore value=0";
			NLS_LOG_ERROR(message);
			return { NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument, message };
		}
	}
#endif

	NativeDX12Queue::NativeDX12Queue(
		ID3D12Device* device,
		ID3D12CommandQueue* queue,
		NLS::Render::RHI::QueueType queueType,
		const std::string& debugName)
		: m_device(device)
		, m_queue(queue)
		, m_queueType(queueType)
		, m_debugName(debugName)
	{
#if defined(_WIN32)
		SetDx12ObjectName(m_queue, m_debugName);
		NLS_LOG_INFO(
			"NativeDX12Queue created: device=" +
			std::to_string(reinterpret_cast<uintptr_t>(device)) +
			" queue=" +
			std::to_string(reinterpret_cast<uintptr_t>(queue)));
#else
		(void)device;
		(void)queue;
		(void)queueType;
#endif
	}

	NativeDX12Queue::~NativeDX12Queue()
	{
#if defined(_WIN32)
		NLS::Render::RHI::DX12::Detail::ReleaseQueueMutex(m_queue);
#endif
	}

	std::string_view NativeDX12Queue::GetDebugName() const
	{
		return m_debugName;
	}

	NLS::Render::RHI::QueueType NativeDX12Queue::GetType() const
	{
		return m_queueType;
	}

	void NativeDX12Queue::Submit(const NLS::Render::RHI::RHISubmitDesc& submitDesc)
	{
		(void)SubmitChecked(submitDesc);
	}

	NLS::Render::RHI::RHIQueueOperationResult NativeDX12Queue::SubmitChecked(
		const NLS::Render::RHI::RHISubmitDesc& submitDesc)
	{
		NLS_PROFILE_SCOPE();
#if defined(_WIN32)
		if (m_queue == nullptr)
			return {
				NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument,
				"NativeDX12Queue::Submit: queue is null"
			};

		std::vector<ID3D12CommandList*> commandLists;
		{
			NLS_PROFILE_NAMED_SCOPE("NativeDX12Queue::CollectCommandLists");
			for (const auto& cmdBuffer : submitDesc.commandBuffers)
			{
				if (cmdBuffer == nullptr)
					continue;
				if (cmdBuffer->IsChildCommandBuffer())
				{
					const std::string message =
						"NativeDX12Queue::Submit: DX12 child bundle command buffers must be executed by a parent command list";
					NLS_LOG_ERROR(message);
					return {
						NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument,
						message
					};
				}
				if (!cmdBuffer->IsClosedForSubmission())
				{
					const std::string message =
						"NativeDX12Queue::Submit: DX12 command buffer must be closed successfully before submission";
					NLS_LOG_ERROR(message);
					return {
						NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument,
						message
					};
				}

				const auto nativeCommandBuffer = cmdBuffer->GetNativeCommandBuffer();
				auto* nativeCommandList = nativeCommandBuffer.backend == NLS::Render::RHI::BackendType::DX12
					? static_cast<ID3D12CommandList*>(nativeCommandBuffer.handle)
					: nullptr;
				if (nativeCommandList != nullptr)
					commandLists.push_back(nativeCommandList);
			}
		}

		bool mayHaveQueuedGpuWork = false;
		bool commandListsQueued = false;
		bool submitShouldReturn = false;
		const bool shouldPublishProfilerMetadata =
			!commandLists.empty() && NLS::Base::Profiling::Profiler::IsEnabled();
		std::uint64_t profilerSubmitSequence = 0u;
		NLS::Render::RHI::RHIQueueOperationResult result;
		{
			NLS_PROFILE_NAMED_SCOPE("NativeDX12Queue::ValidateSubmitSemaphoreWaits");
			for (const auto& semaphore : submitDesc.waitSemaphores)
			{
				auto* nativeSemaphore = dynamic_cast<NativeDX12Semaphore*>(semaphore.get());
				if (nativeSemaphore == nullptr || nativeSemaphore->GetFence() == nullptr)
					continue;

				const auto waitValueValidation = ValidateDX12SemaphoreWaitValue(
					*nativeSemaphore,
					"NativeDX12Queue::Submit");
				if (!waitValueValidation.Succeeded())
					return waitValueValidation;
			}
		}
		{
			NLS::Render::RHI::DX12::ScopedDX12QueueLock queueLock(m_queue);
			{
				NLS_PROFILE_NAMED_SCOPE("NativeDX12Queue::WaitSubmitSemaphores");
				for (const auto& semaphore : submitDesc.waitSemaphores)
				{
					auto* nativeSemaphore = dynamic_cast<NativeDX12Semaphore*>(semaphore.get());
					if (nativeSemaphore == nullptr || nativeSemaphore->GetFence() == nullptr)
						continue;

					mayHaveQueuedGpuWork = true;
					const HRESULT waitHr = m_queue->Wait(
						nativeSemaphore->GetFence(),
						nativeSemaphore->GetWaitValue());
					if (FAILED(waitHr))
					{
						const std::string message =
							"NativeDX12Queue::Submit: queue wait on semaphore failed hr=" +
							std::to_string(waitHr);
						NLS_LOG_ERROR(message);
						return ClassifyDx12QueuedWorkFailure(
							m_device,
							message,
							mayHaveQueuedGpuWork);
					}
				}
			}

			if (!commandLists.empty())
			{
				if (ShouldLogDx12FrameFlow())
				{
					NLS_LOG_INFO("NativeDX12Queue::Submit: executing " + std::to_string(commandLists.size()) + " command lists");
				}
				{
					NLS_PROFILE_NAMED_SCOPE("ID3D12CommandQueue::ExecuteCommandLists");
					mayHaveQueuedGpuWork = true;
					m_queue->ExecuteCommandLists(static_cast<UINT>(commandLists.size()), commandLists.data());
				}
				commandListsQueued = true;
				if (shouldPublishProfilerMetadata)
				{
					profilerSubmitSequence =
						NLS::Render::RHI::DX12::Detail::ReserveQueueProfilerSubmitSequence(m_queue);
				}
				if (ShouldLogDx12FrameFlow())
				{
					NLS_LOG_INFO("NativeDX12Queue::Submit: ExecuteCommandLists succeeded");
				}

				if (m_device != nullptr && ShouldLogDx12ValidationMessages())
				{
					LogDx12DebugMessages(m_device, "NativeDX12Queue::Submit(after ExecuteCommandLists)");
				}

				if (m_device != nullptr)
				{
					const HRESULT deviceStatus = m_device->GetDeviceRemovedReason();
					if (FAILED(deviceStatus))
					{
						const std::string message =
							"NativeDX12Queue::Submit: device status after ExecuteCommandLists hr=" +
							std::to_string(deviceStatus);
						NLS_LOG_ERROR(message);
						LogDx12DebugMessages(m_device, "NativeDX12Queue::Submit");
						LogDx12DredBreadcrumbs(m_device, "NativeDX12Queue::Submit");
						result = {
							NLS::Render::RHI::RHIQueueOperationStatusCode::DeviceLost,
							message,
							true
						};
						submitShouldReturn = true;
					}
				}
			}
			else if (ShouldLogDx12FrameFlow())
			{
				NLS_LOG_INFO("NativeDX12Queue::Submit: no command lists to execute");
			}

			if (!submitShouldReturn)
			{
				NLS_PROFILE_NAMED_SCOPE("NativeDX12Queue::SignalSubmitSemaphores");
				for (const auto& semaphore : submitDesc.signalSemaphores)
				{
					auto* nativeSemaphore = dynamic_cast<NativeDX12Semaphore*>(semaphore.get());
					if (nativeSemaphore == nullptr || nativeSemaphore->GetFence() == nullptr)
						continue;

					mayHaveQueuedGpuWork = true;
					const HRESULT signalHr = nativeSemaphore->SignalOnQueueChecked(m_queue);
					if (FAILED(signalHr))
					{
						const std::string message =
							"NativeDX12Queue::Submit: queue signal semaphore failed hr=" +
							std::to_string(signalHr);
						NLS_LOG_ERROR(message);
						bool frameFenceSignalQueued = false;
						if (submitDesc.signalFence != nullptr)
						{
							auto* nativeFence = dynamic_cast<NativeDX12Fence*>(submitDesc.signalFence.get());
							if (nativeFence != nullptr && nativeFence->GetFence() != nullptr)
							{
								submitDesc.signalFence->Reset();
								const HRESULT fenceSignalHr = m_queue->Signal(
									nativeFence->GetFence(),
									nativeFence->GetTargetValue());
								frameFenceSignalQueued = SUCCEEDED(fenceSignalHr);
								if (!frameFenceSignalQueued)
								{
									NLS_LOG_ERROR(
										"NativeDX12Queue::Submit: failed to queue retirement fence after semaphore signal failure hr=" +
										std::to_string(fenceSignalHr));
								}
							}
						}
						result = ClassifyDx12QueuedWorkFailure(
							m_device,
							message,
							mayHaveQueuedGpuWork,
							frameFenceSignalQueued);
						submitShouldReturn = true;
						break;
					}
				}
			}

			if (!submitShouldReturn && submitDesc.signalFence != nullptr)
			{
				NLS_PROFILE_NAMED_SCOPE("NativeDX12Queue::SignalSubmitFence");
				auto* nativeFence = dynamic_cast<NativeDX12Fence*>(submitDesc.signalFence.get());
				if (nativeFence != nullptr && nativeFence->GetFence() != nullptr)
				{
					submitDesc.signalFence->Reset();
					const UINT64 fenceValue = nativeFence->GetTargetValue();
					const HRESULT signalHr = m_queue->Signal(nativeFence->GetFence(), fenceValue);
					if (FAILED(signalHr))
					{
						const std::string message =
							"NativeDX12Queue::Submit: queue signal fence failed hr=" +
							std::to_string(signalHr) +
							" value=" + std::to_string(fenceValue);
						NLS_LOG_ERROR(message);
						LogDx12DredBreadcrumbs(m_device, "NativeDX12Queue::Submit");
						result = ClassifyDx12QueuedWorkFailure(m_device, message, mayHaveQueuedGpuWork);
						submitShouldReturn = true;
					}
					else
					{
						result.frameFenceSignalQueued = true;
					}
				}
			}
		}
		if (commandListsQueued && shouldPublishProfilerMetadata)
		{
			NLS::Render::RHI::DX12::ScopedDX12QueueProfilerSubmissionOrder profilerSubmitOrder(
				m_queue,
				profilerSubmitSequence);
			NLS::Base::Profiling::ProfilerGpuCommandListSubmitEvent profilerSubmit;
			profilerSubmit.nativeCommandQueue = m_queue;
			profilerSubmit.nativeCommandLists.reserve(commandLists.size());
			for (auto* commandList : commandLists)
				profilerSubmit.nativeCommandLists.push_back(commandList);
			NLS::Base::Profiling::Profiler::SubmitGpuCommandLists(profilerSubmit);
		}
		result.mayHaveQueuedGpuWork = mayHaveQueuedGpuWork;
		return result;
#else
		(void)submitDesc;
		return { NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure, "DX12 queue submit is only available on Windows" };
#endif
	}

	void NativeDX12Queue::Present(const NLS::Render::RHI::RHIPresentDesc& presentDesc)
	{
		(void)PresentChecked(presentDesc);
	}

	NLS::Render::RHI::RHIQueueOperationResult NativeDX12Queue::PresentChecked(
		const NLS::Render::RHI::RHIPresentDesc& presentDesc)
	{
		NLS_PROFILE_SCOPE();
#if defined(_WIN32)
		if (ShouldLogDx12FrameFlow())
		{
			NLS_LOG_INFO(
				"NativeDX12Queue::Present: called with swapchain=" +
				std::to_string(reinterpret_cast<uintptr_t>(presentDesc.swapchain.get())) +
				" imageIndex=" +
				std::to_string(presentDesc.imageIndex));
		}

		if (m_queue == nullptr || presentDesc.swapchain == nullptr)
		{
			const std::string message = "NativeDX12Queue::Present: queue or swapchain is null";
			NLS_LOG_ERROR(message);
			return { NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument, message };
		}

		const auto swapchainHandle = presentDesc.swapchain->GetNativeSwapchainHandle();
		auto* swapchain = swapchainHandle.backend == NLS::Render::RHI::BackendType::DX12
			? reinterpret_cast<IDXGISwapChain3*>(swapchainHandle.handle)
			: nullptr;
		if (swapchain == nullptr)
		{
			const std::string message = "NativeDX12Queue::Present: GetNativeSwapchainHandle returned null";
			NLS_LOG_ERROR(message);
			return { NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument, message };
		}

		if (ShouldLogDx12FrameFlow())
		{
			NLS_LOG_INFO(
				"NativeDX12Queue::Present: swapchain ptr=" +
				std::to_string(reinterpret_cast<uintptr_t>(swapchain)) +
				" uiSemaphore=" +
				std::to_string(reinterpret_cast<uintptr_t>(presentDesc.uiSignalSemaphore.handle)) +
				" uiSignalValue=" +
				std::to_string(presentDesc.uiSignalValue));
		}

		if (presentDesc.uiSignalSemaphore.backend == NLS::Render::RHI::BackendType::DX12 &&
			presentDesc.uiSignalSemaphore.handle != nullptr &&
			presentDesc.uiSignalValue == 0u)
		{
			const std::string message =
				"NativeDX12Queue::Present: refusing to wait on UI fence value=0";
			NLS_LOG_ERROR(message);
			return { NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument, message };
		}

		bool mayHaveQueuedGpuWork = false;
		{
			NLS_PROFILE_NAMED_SCOPE("NativeDX12Queue::ValidatePresentSemaphoreWaits");
			for (const auto& semaphore : presentDesc.waitSemaphores)
			{
				auto* nativeSemaphore = dynamic_cast<NativeDX12Semaphore*>(semaphore.get());
				if (nativeSemaphore == nullptr || nativeSemaphore->GetFence() == nullptr)
					continue;

				const auto waitValueValidation = ValidateDX12SemaphoreWaitValue(
					*nativeSemaphore,
					"NativeDX12Queue::Present");
				if (!waitValueValidation.Succeeded())
					return waitValueValidation;
			}
		}
		NLS::Render::RHI::DX12::ScopedDX12QueueLock queueLock(m_queue);
		{
			NLS_PROFILE_NAMED_SCOPE("NativeDX12Queue::WaitPresentSemaphores");
			for (const auto& semaphore : presentDesc.waitSemaphores)
			{
				auto* nativeSemaphore = dynamic_cast<NativeDX12Semaphore*>(semaphore.get());
				if (nativeSemaphore == nullptr || nativeSemaphore->GetFence() == nullptr)
					continue;

				mayHaveQueuedGpuWork = true;
				const HRESULT waitHr = m_queue->Wait(
					nativeSemaphore->GetFence(),
					nativeSemaphore->GetWaitValue());
				if (FAILED(waitHr))
				{
					const std::string message =
						"NativeDX12Queue::Present: queue wait on present semaphore failed hr=" +
						std::to_string(waitHr);
					NLS_LOG_ERROR(message);
					return ClassifyDx12QueuedWorkFailure(
						m_device,
						message,
						mayHaveQueuedGpuWork);
				}
			}
		}

		if (presentDesc.uiSignalSemaphore.backend == NLS::Render::RHI::BackendType::DX12 &&
			presentDesc.uiSignalSemaphore.handle != nullptr)
		{
			NLS_PROFILE_NAMED_SCOPE("NativeDX12Queue::WaitUiFenceBeforePresent");
			auto* uiFence = reinterpret_cast<ID3D12Fence*>(presentDesc.uiSignalSemaphore.handle);
			if (uiFence != nullptr)
			{
				mayHaveQueuedGpuWork = true;
				const HRESULT waitHr = m_queue->Wait(uiFence, presentDesc.uiSignalValue);
				if (FAILED(waitHr))
				{
					const std::string message =
						"NativeDX12Queue::Present: failed to wait on UI fence before present hr=" +
						std::to_string(waitHr);
					NLS_LOG_WARNING(message);
					return ClassifyDx12QueuedWorkFailure(
						m_device,
						message,
						mayHaveQueuedGpuWork);
				}
			}
		}

		if (ShouldLogDx12FrameFlow())
		{
			NLS_LOG_INFO("NativeDX12Queue::Present: calling swapchain->Present");
		}
		const UINT syncInterval = ResolveDX12PresentSyncInterval(presentDesc.swapchain->GetDesc().vsync);
		const UINT presentFlags = ResolveDX12PresentFlags(
			presentDesc.swapchain->GetDesc().vsync,
			presentDesc.swapchain->GetDesc().allowTearing);
		HRESULT hr = S_OK;
		{
			NLS_PROFILE_NAMED_SCOPE("IDXGISwapChain::Present");
			mayHaveQueuedGpuWork = true;
			hr = swapchain->Present(syncInterval, presentFlags);
		}
		if (ShouldLogDx12FrameFlow())
		{
			NLS_LOG_INFO("NativeDX12Queue::Present: Present returned hr=" + std::to_string(hr));
		}
		if (FAILED(hr))
		{
			std::string message = "NativeDX12Queue::Present: Present failed with hr=" + std::to_string(hr);
			NLS_LOG_ERROR(message);
			LogDx12DebugMessages(m_device, "NativeDX12Queue::Present");
			LogDx12DredBreadcrumbs(m_device, "NativeDX12Queue::Present");

			if (m_device != nullptr)
			{
				const HRESULT reason = m_device->GetDeviceRemovedReason();
				if (FAILED(reason))
				{
					if (hr == DXGI_ERROR_DEVICE_REMOVED)
						NLS_LOG_ERROR("NativeDX12Queue::Present: DXGI_ERROR_DEVICE_REMOVED");
					NLS_LOG_ERROR("NativeDX12Queue::Present: Device removed reason hr=" + std::to_string(reason));
					message += " deviceRemovedReason=" + std::to_string(reason);
				}
			}
			const auto statusCode =
				(hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
					? NLS::Render::RHI::RHIQueueOperationStatusCode::DeviceLost
					: NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure;
			return { statusCode, message, mayHaveQueuedGpuWork };
		}
		NLS::Render::RHI::RHIQueueOperationResult result;
		result.mayHaveQueuedGpuWork = mayHaveQueuedGpuWork;
		if (presentDesc.signalFence != nullptr)
		{
			NLS_PROFILE_NAMED_SCOPE("NativeDX12Queue::SignalPresentFence");
			auto* nativeFence = dynamic_cast<NativeDX12Fence*>(presentDesc.signalFence.get());
			if (nativeFence != nullptr && nativeFence->GetFence() != nullptr)
			{
				presentDesc.signalFence->Reset();
				const UINT64 fenceValue = nativeFence->GetTargetValue();
				const HRESULT signalHr = m_queue->Signal(nativeFence->GetFence(), fenceValue);
				if (FAILED(signalHr))
				{
					const std::string message =
						"NativeDX12Queue::Present: queue signal fence failed hr=" +
						std::to_string(signalHr) +
						" value=" + std::to_string(fenceValue);
					NLS_LOG_ERROR(message);
					return ClassifyDx12QueuedWorkFailure(m_device, message, mayHaveQueuedGpuWork);
				}
				result.frameFenceSignalQueued = true;
			}
		}
		return result;
#else
		(void)presentDesc;
		return { NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure, "DX12 present is only available on Windows" };
#endif
	}
}
