#include "Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.h"
#include "Rendering/RHI/Backends/DX12/DX12GraphicsPipelineUtils.h"
#include "Rendering/RHI/Backends/DX12/DX12PipelineLayoutUtils.h"
#include "Rendering/RHI/Backends/DX12/DX12ReadbackUtils.h"
#include "Rendering/RHI/Backends/DX12/DX12RenderPassUtils.h"
#include "Rendering/RHI/Backends/DX12/DX12TextureUploadUtils.h"
#include "Rendering/RHI/Backends/DX12/DX12TextureViewUtils.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <Debug/Logger.h>
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHISync.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/RHI/Core/RHIResource.h"

#if defined(_WIN32)
#include <dxgi1_6.h>
#undef CreateSemaphore  // Windows macro conflicts with RHIDevice::CreateSemaphore
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#endif

namespace NLS::Render::Backend
{
#if defined(_WIN32)
	using Microsoft::WRL::ComPtr;
#endif

	namespace
	{
#if defined(_WIN32)
		D3D12_COMPARISON_FUNC ToD3D12ComparisonFunc(NLS::Render::Settings::EComparaisonAlgorithm algorithm)
		{
			switch (algorithm)
			{
			case NLS::Render::Settings::EComparaisonAlgorithm::NEVER: return D3D12_COMPARISON_FUNC_NEVER;
			case NLS::Render::Settings::EComparaisonAlgorithm::LESS: return D3D12_COMPARISON_FUNC_LESS;
			case NLS::Render::Settings::EComparaisonAlgorithm::EQUAL: return D3D12_COMPARISON_FUNC_EQUAL;
			case NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
			case NLS::Render::Settings::EComparaisonAlgorithm::GREATER: return D3D12_COMPARISON_FUNC_GREATER;
			case NLS::Render::Settings::EComparaisonAlgorithm::NOTEQUAL: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
			case NLS::Render::Settings::EComparaisonAlgorithm::GREATER_EQUAL: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
			case NLS::Render::Settings::EComparaisonAlgorithm::ALWAYS:
			default:
				return D3D12_COMPARISON_FUNC_ALWAYS;
			}
		}

		D3D12_STENCIL_OP ToD3D12StencilOp(NLS::Render::Settings::EOperation operation)
		{
			switch (operation)
			{
			case NLS::Render::Settings::EOperation::KEEP: return D3D12_STENCIL_OP_KEEP;
			case NLS::Render::Settings::EOperation::ZERO: return D3D12_STENCIL_OP_ZERO;
			case NLS::Render::Settings::EOperation::REPLACE: return D3D12_STENCIL_OP_REPLACE;
			case NLS::Render::Settings::EOperation::INCREMENT: return D3D12_STENCIL_OP_INCR_SAT;
			case NLS::Render::Settings::EOperation::INCREMENT_WRAP: return D3D12_STENCIL_OP_INCR;
			case NLS::Render::Settings::EOperation::DECREMENT: return D3D12_STENCIL_OP_DECR_SAT;
			case NLS::Render::Settings::EOperation::DECREMENT_WRAP: return D3D12_STENCIL_OP_DECR;
			case NLS::Render::Settings::EOperation::INVERT: return D3D12_STENCIL_OP_INVERT;
			default: return D3D12_STENCIL_OP_KEEP;
			}
		}

		std::string WideStringToUtf8(const wchar_t* value)
		{
			if (value == nullptr || value[0] == L'\0')
				return {};

			const int requiredBytes = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
			if (requiredBytes <= 1)
				return {};

			std::string utf8(static_cast<size_t>(requiredBytes), '\0');
			WideCharToMultiByte(CP_UTF8, 0, value, -1, utf8.data(), requiredBytes, nullptr, nullptr);
			utf8.resize(static_cast<size_t>(requiredBytes - 1));
			return utf8;
		}

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
			if (wideName.empty())
				return;

			object->SetName(wideName.c_str());
		}

		std::string FormatHexValue(UINT64 value)
		{
			std::ostringstream stream;
			stream << "0x" << std::hex << value;
			return stream.str();
		}

		std::string GetDx12ObjectDebugName(const char* nameA, const wchar_t* nameW, const std::string& fallback)
		{
			if (nameA != nullptr && nameA[0] != '\0')
				return std::string(nameA);

			const std::string utf8Name = WideStringToUtf8(nameW);
			return utf8Name.empty() ? fallback : utf8Name;
		}

		const char* Dx12BreadcrumbOpToString(D3D12_AUTO_BREADCRUMB_OP op)
		{
			switch (op)
			{
			case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SetMarker";
			case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BeginEvent";
			case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "EndEvent";
			case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
			case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
			case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "ExecuteIndirect";
			case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
			case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
			case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
			case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
			case D3D12_AUTO_BREADCRUMB_OP_COPYTILES: return "CopyTiles";
			case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return "ResolveSubresource";
			case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "ClearRenderTargetView";
			case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "ClearUnorderedAccessView";
			case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "ClearDepthStencilView";
			case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
			case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE: return "ExecuteBundle";
			case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "Present";
			case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return "ResolveQueryData";
			case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION: return "BeginSubmission";
			case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION: return "EndSubmission";
			case D3D12_AUTO_BREADCRUMB_OP_DECODEFRAME: return "DecodeFrame";
			case D3D12_AUTO_BREADCRUMB_OP_PROCESSFRAMES: return "ProcessFrames";
			case D3D12_AUTO_BREADCRUMB_OP_ATOMICCOPYBUFFERUINT: return "AtomicCopyBufferUint";
			case D3D12_AUTO_BREADCRUMB_OP_ATOMICCOPYBUFFERUINT64: return "AtomicCopyBufferUint64";
			case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCEREGION: return "ResolveSubresourceRegion";
			case D3D12_AUTO_BREADCRUMB_OP_WRITEBUFFERIMMEDIATE: return "WriteBufferImmediate";
			case D3D12_AUTO_BREADCRUMB_OP_DECODEFRAME1: return "DecodeFrame1";
			case D3D12_AUTO_BREADCRUMB_OP_SETPROTECTEDRESOURCESESSION: return "SetProtectedResourceSession";
			case D3D12_AUTO_BREADCRUMB_OP_DECODEFRAME2: return "DecodeFrame2";
			case D3D12_AUTO_BREADCRUMB_OP_PROCESSFRAMES1: return "ProcessFrames1";
			case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return "BuildRaytracingAccelerationStructure";
			case D3D12_AUTO_BREADCRUMB_OP_EMITRAYTRACINGACCELERATIONSTRUCTUREPOSTBUILDINFO: return "EmitRaytracingAccelerationStructurePostbuildInfo";
			case D3D12_AUTO_BREADCRUMB_OP_COPYRAYTRACINGACCELERATIONSTRUCTURE: return "CopyRaytracingAccelerationStructure";
			case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return "DispatchRays";
			case D3D12_AUTO_BREADCRUMB_OP_INITIALIZEMETACOMMAND: return "InitializeMetaCommand";
			case D3D12_AUTO_BREADCRUMB_OP_EXECUTEMETACOMMAND: return "ExecuteMetaCommand";
			case D3D12_AUTO_BREADCRUMB_OP_ESTIMATEMOTION: return "EstimateMotion";
			case D3D12_AUTO_BREADCRUMB_OP_RESOLVEMOTIONVECTORHEAP: return "ResolveMotionVectorHeap";
			case D3D12_AUTO_BREADCRUMB_OP_SETPIPELINESTATE1: return "SetPipelineState1";
			case D3D12_AUTO_BREADCRUMB_OP_INITIALIZEEXTENSIONCOMMAND: return "InitializeExtensionCommand";
			case D3D12_AUTO_BREADCRUMB_OP_EXECUTEEXTENSIONCOMMAND: return "ExecuteExtensionCommand";
			case D3D12_AUTO_BREADCRUMB_OP_DISPATCHMESH: return "DispatchMesh";
			case D3D12_AUTO_BREADCRUMB_OP_ENCODEFRAME: return "EncodeFrame";
			case D3D12_AUTO_BREADCRUMB_OP_RESOLVEENCODEROUTPUTMETADATA: return "ResolveEncoderOutputMetadata";
			default: return "Unknown";
			}
		}

		const char* Dx12DredAllocationTypeToString(D3D12_DRED_ALLOCATION_TYPE type)
		{
			switch (type)
			{
			case D3D12_DRED_ALLOCATION_TYPE_COMMAND_QUEUE: return "CommandQueue";
			case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR: return "CommandAllocator";
			case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE: return "PipelineState";
			case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST: return "CommandList";
			case D3D12_DRED_ALLOCATION_TYPE_FENCE: return "Fence";
			case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP: return "DescriptorHeap";
			case D3D12_DRED_ALLOCATION_TYPE_HEAP: return "Heap";
			case D3D12_DRED_ALLOCATION_TYPE_QUERY_HEAP: return "QueryHeap";
			case D3D12_DRED_ALLOCATION_TYPE_COMMAND_SIGNATURE: return "CommandSignature";
			case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_LIBRARY: return "PipelineLibrary";
			case D3D12_DRED_ALLOCATION_TYPE_RESOURCE: return "Resource";
			case D3D12_DRED_ALLOCATION_TYPE_PASS: return "Pass";
			case D3D12_DRED_ALLOCATION_TYPE_COMMAND_POOL: return "CommandPool";
			case D3D12_DRED_ALLOCATION_TYPE_COMMAND_RECORDER: return "CommandRecorder";
			case D3D12_DRED_ALLOCATION_TYPE_STATE_OBJECT: return "StateObject";
			case D3D12_DRED_ALLOCATION_TYPE_METACOMMAND: return "MetaCommand";
			default: return "Other";
			}
		}

		void EnableDx12Dred()
		{
			ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dredSettings1;
			const HRESULT dredSettings1Hr = D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings1));
			if (SUCCEEDED(dredSettings1Hr) && dredSettings1 != nullptr)
			{
				dredSettings1->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
				dredSettings1->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
				dredSettings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
				NLS_LOG_INFO("CreateDX12RhiDevice: DRED v1 auto breadcrumbs, page faults, and breadcrumb context forced on");
				return;
			}

			ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
			const HRESULT dredSettingsHr = D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings));
			if (FAILED(dredSettingsHr) || dredSettings == nullptr)
			{
				NLS_LOG_WARNING(
					"CreateDX12RhiDevice: DRED settings unavailable hr=" + std::to_string(dredSettingsHr) +
					", settings1 hr=" + std::to_string(dredSettings1Hr));
				return;
			}

			dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			NLS_LOG_INFO("CreateDX12RhiDevice: DRED auto breadcrumbs and page faults forced on");
		}

		void LogDx12DebugMessages(ID3D12Device* device, const std::string& context)
		{
			if (device == nullptr)
				return;

			ComPtr<ID3D12InfoQueue> infoQueue;
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

		bool TryMarkDx12RemovalDiagnosticsLogged(ID3D12Device* device)
		{
			static std::mutex mutex;
			static std::vector<ID3D12Device*> loggedDevices;

			if (device == nullptr)
				return false;

			std::lock_guard<std::mutex> lock(mutex);
			if (std::find(loggedDevices.begin(), loggedDevices.end(), device) != loggedDevices.end())
				return false;

			loggedDevices.push_back(device);
			return true;
		}

		void LogDx12DredBreadcrumbContexts(
			const D3D12_AUTO_BREADCRUMB_NODE1* node,
			UINT suspectBreadcrumbIndex,
			const std::string& context)
		{
			if (node == nullptr || node->pBreadcrumbContexts == nullptr || node->BreadcrumbContextsCount == 0)
				return;

			for (UINT contextIndex = 0; contextIndex < node->BreadcrumbContextsCount; ++contextIndex)
			{
				const D3D12_DRED_BREADCRUMB_CONTEXT& breadcrumbContext = node->pBreadcrumbContexts[contextIndex];
				const UINT breadcrumbIndex = breadcrumbContext.BreadcrumbIndex;
				const bool nearFailure =
					breadcrumbIndex >= (suspectBreadcrumbIndex > 2 ? suspectBreadcrumbIndex - 2 : 0) &&
					breadcrumbIndex <= suspectBreadcrumbIndex + 2;
				if (!nearFailure)
					continue;

				const std::string contextString = WideStringToUtf8(breadcrumbContext.pContextString);
				if (contextString.empty())
					continue;

				NLS_LOG_ERROR(
					context +
					": DRED breadcrumb context index=" + std::to_string(breadcrumbIndex) +
					" text=" + contextString);
			}
		}

		void LogDx12DredAllocationList(
			const D3D12_DRED_ALLOCATION_NODE1* head,
			const std::string& context,
			const char* label)
		{
			UINT loggedCount = 0;
			for (auto* node = head; node != nullptr && loggedCount < 8; node = node->pNext, ++loggedCount)
			{
				const std::string objectName = GetDx12ObjectDebugName(node->ObjectNameA, node->ObjectNameW, "<unnamed>");
				NLS_LOG_ERROR(
					context +
					": DRED " + label +
					" allocation[" + std::to_string(loggedCount) +
					"] type=" + Dx12DredAllocationTypeToString(node->AllocationType) +
					" name=" + objectName);
			}

			if (head == nullptr)
			{
				NLS_LOG_ERROR(context + ": DRED " + std::string(label) + " allocations: none");
			}
		}

		void LogDx12RemovalDiagnostics(ID3D12Device* device, const std::string& context, HRESULT removalReason)
		{
			if (device == nullptr || !TryMarkDx12RemovalDiagnosticsLogged(device))
				return;

			NLS_LOG_ERROR(
				context +
				": collecting DRED diagnostics for removal reason hr=" + std::to_string(removalReason));
			LogDx12DebugMessages(device, context);

			ComPtr<ID3D12DeviceRemovedExtendedData1> dred1;
			const HRESULT dredHr = device->QueryInterface(IID_PPV_ARGS(&dred1));
			if (FAILED(dredHr) || dred1 == nullptr)
			{
				NLS_LOG_ERROR(context + ": ID3D12DeviceRemovedExtendedData1 unavailable hr=" + std::to_string(dredHr));
				return;
			}

			D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbOutput{};
			const HRESULT breadcrumbHr = dred1->GetAutoBreadcrumbsOutput1(&breadcrumbOutput);
			if (FAILED(breadcrumbHr))
			{
				NLS_LOG_ERROR(context + ": GetAutoBreadcrumbsOutput1 failed hr=" + std::to_string(breadcrumbHr));
			}
			else if (breadcrumbOutput.pHeadAutoBreadcrumbNode == nullptr)
			{
				NLS_LOG_ERROR(context + ": DRED auto breadcrumbs returned no nodes");
			}
			else
			{
				UINT nodeIndex = 0;
				for (auto* node = breadcrumbOutput.pHeadAutoBreadcrumbNode; node != nullptr && nodeIndex < 16; node = node->pNext, ++nodeIndex)
				{
					const UINT lastCompleted = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0;
					const UINT suspectIndex = node->BreadcrumbCount == 0
						? 0
						: (std::min)(lastCompleted, node->BreadcrumbCount - 1);
					const std::string commandListName = GetDx12ObjectDebugName(
						node->pCommandListDebugNameA,
						node->pCommandListDebugNameW,
						"<unnamed-command-list>");
					const std::string commandQueueName = GetDx12ObjectDebugName(
						node->pCommandQueueDebugNameA,
						node->pCommandQueueDebugNameW,
						"<unnamed-command-queue>");

					NLS_LOG_ERROR(
						context +
						": DRED breadcrumb node[" + std::to_string(nodeIndex) +
						"] commandList=" + commandListName +
						" commandQueue=" + commandQueueName +
						" completed=" + std::to_string(lastCompleted) +
						" total=" + std::to_string(node->BreadcrumbCount));

					if (node->pCommandHistory != nullptr && node->BreadcrumbCount > 0)
					{
						const UINT startIndex = suspectIndex > 4 ? suspectIndex - 4 : 0;
						const UINT endIndex = (std::min)(node->BreadcrumbCount, suspectIndex + 5);
						for (UINT historyIndex = startIndex; historyIndex < endIndex; ++historyIndex)
						{
							const char* marker = historyIndex == suspectIndex ? " <-- next suspect" : "";
							NLS_LOG_ERROR(
								context +
								": DRED breadcrumb op[" + std::to_string(historyIndex) +
								"]=" + Dx12BreadcrumbOpToString(node->pCommandHistory[historyIndex]) +
								marker);
						}
					}

					LogDx12DredBreadcrumbContexts(node, suspectIndex, context);
				}
			}

			D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFaultOutput{};
			const HRESULT pageFaultHr = dred1->GetPageFaultAllocationOutput1(&pageFaultOutput);
			if (FAILED(pageFaultHr))
			{
				NLS_LOG_ERROR(context + ": GetPageFaultAllocationOutput1 failed hr=" + std::to_string(pageFaultHr));
				return;
			}

			NLS_LOG_ERROR(
				context +
				": DRED page fault VA=" + FormatHexValue(pageFaultOutput.PageFaultVA));
			LogDx12DredAllocationList(pageFaultOutput.pHeadExistingAllocationNode, context, "existing");
			LogDx12DredAllocationList(pageFaultOutput.pHeadRecentFreedAllocationNode, context, "recent-freed");
		}

		UINT64 AlignUp(UINT64 value, UINT64 alignment)
		{
			if (alignment == 0)
				return value;

			return (value + (alignment - 1u)) & ~(alignment - 1u);
		}

		bool ShouldLogDx12ValidationMessages()
		{
			static const bool enabled = []()
			{
				const char* value = std::getenv("NLS_DX12_LOG_MESSAGES");
				if (value == nullptr)
					return false;
				return std::strcmp(value, "1") == 0 || _stricmp(value, "true") == 0;
			}();
			return enabled;
		}

		bool ShouldLogDx12FrameFlow()
		{
			static const bool enabled = []()
			{
				const char* value = std::getenv("NLS_DX12_LOG_FRAME_FLOW");
				if (value == nullptr)
					return false;
				return std::strcmp(value, "1") == 0 || _stricmp(value, "true") == 0;
			}();
			return enabled;
		}
#endif

		// Forward declarations inside anonymous namespace
		class NativeDX12BindingSet;

		class NativeDX12Fence final : public NLS::Render::RHI::RHIFence
		{
		public:
			NativeDX12Fence(ID3D12Device* device, const std::string& debugName)
			{
#if defined(_WIN32)
				m_device = device;
				m_debugName = debugName;
				if (device != nullptr)
				{
					device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
				}
#endif
			}

			std::string_view GetDebugName() const override { return m_debugName; }
			bool IsSignaled() const override
			{
#if defined(_WIN32)
				if (m_fence == nullptr)
					return false;
				return m_fence->GetCompletedValue() >= m_targetValue;
#else
				return false;
#endif
			}
			void Reset() override
			{
#if defined(_WIN32)
				++m_targetValue;
#endif
			}
			bool Wait(uint64_t timeoutNanoseconds) override
			{
#if defined(_WIN32)
				if (m_fence == nullptr)
					return false;
				if (m_targetValue == 0)
					return true;
				UINT64 completedValue = m_fence->GetCompletedValue();
				if (completedValue >= m_targetValue)
					return true;
				HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
				if (event == nullptr)
					return false;
				m_fence->SetEventOnCompletion(m_targetValue, event);
				DWORD waitMs = timeoutNanoseconds > 0 ? static_cast<DWORD>(timeoutNanoseconds / 1000000) : INFINITE;
				DWORD result = WaitForSingleObject(event, waitMs);
				CloseHandle(event);
				return result == WAIT_OBJECT_0;
#else
				return false;
#endif
			}

#if defined(_WIN32)
			ID3D12Fence* GetFence() const { return m_fence.Get(); }
			UINT64 GetTargetValue() const { return m_targetValue; }
#endif

		private:
			std::string m_debugName;
#if defined(_WIN32)
			ID3D12Device* m_device = nullptr;
			ComPtr<ID3D12Fence> m_fence;
			UINT64 m_targetValue = 0;
#endif
		};

		class NativeDX12Semaphore final : public NLS::Render::RHI::RHISemaphore
		{
		public:
			NativeDX12Semaphore(ID3D12Device* device, const std::string& debugName)
			{
#if defined(_WIN32)
				m_device = device;
				m_debugName = debugName;
				if (device != nullptr)
				{
					device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
				}
#endif
			}

			std::string_view GetDebugName() const override { return m_debugName; }
			bool IsSignaled() const override
			{
#if defined(_WIN32)
				if (m_fence == nullptr)
					return false;
				return m_fence->GetCompletedValue() != 0;
#else
				return false;
#endif
			}
			void Reset() override
			{
#if defined(_WIN32)
				if (m_fence != nullptr)
					m_fence->Signal(0);
#endif
			}

			void* GetNativeSemaphoreHandle() override
			{
#if defined(_WIN32)
				return m_fence.Get();
#else
				return nullptr;
#endif
			}

#if defined(_WIN32)
			ID3D12Fence* GetFence() const { return m_fence.Get(); }
#endif

		private:
			std::string m_debugName;
#if defined(_WIN32)
			ID3D12Device* m_device = nullptr;
			ComPtr<ID3D12Fence> m_fence;
#endif
		};

		class NativeDX12Adapter final : public NLS::Render::RHI::RHIAdapter
		{
		public:
			NativeDX12Adapter(const std::string& vendor, const std::string& hardware)
				: m_vendor(vendor)
				, m_hardware(hardware)
			{
			}

			std::string_view GetDebugName() const override { return "NativeDX12Adapter"; }
			NLS::Render::RHI::NativeBackendType GetBackendType() const override { return NLS::Render::RHI::NativeBackendType::DX12; }
			std::string_view GetVendor() const override { return m_vendor; }
			std::string_view GetHardware() const override { return m_hardware; }

		private:
			std::string m_vendor;
			std::string m_hardware;
		};

		class NativeDX12CommandBuffer;

		class NativeDX12Queue final : public NLS::Render::RHI::RHIQueue
		{
		public:
			NativeDX12Queue(ID3D12Device* device, ID3D12CommandQueue* queue, const std::string& debugName)
				: m_device(device)
				, m_queue(queue)
				, m_debugName(debugName)
			{
				SetDx12ObjectName(m_queue, m_debugName);
				NLS_LOG_INFO("NativeDX12Queue created: device=" + std::to_string(reinterpret_cast<uintptr_t>(device)) + " queue=" + std::to_string(reinterpret_cast<uintptr_t>(queue)));
			}

			std::string_view GetDebugName() const override { return m_debugName; }
			NLS::Render::RHI::QueueType GetType() const override { return NLS::Render::RHI::QueueType::Graphics; }
			void Submit(const NLS::Render::RHI::RHISubmitDesc& submitDesc) override;
			void Present(const NLS::Render::RHI::RHIPresentDesc& presentDesc) override
			{
#if defined(_WIN32)
				if (ShouldLogDx12FrameFlow())
				{
					NLS_LOG_INFO("NativeDX12Queue::Present: called with swapchain=" + std::to_string(reinterpret_cast<uintptr_t>(presentDesc.swapchain.get())) + " imageIndex=" + std::to_string(presentDesc.imageIndex));
				}

				// Check if device was already removed before we try to present
				if (m_device != nullptr)
				{
					// Try to get the device removed reason to see if device is already in removed state
					// This is just for diagnostics
				}

				if (m_queue == nullptr || presentDesc.swapchain == nullptr)
				{
					NLS_LOG_ERROR("NativeDX12Queue::Present: queue or swapchain is null");
					return;
				}

				IDXGISwapChain3* swapchain = reinterpret_cast<IDXGISwapChain3*>(presentDesc.swapchain->GetNativeSwapchainHandle());
				if (swapchain == nullptr)
				{
					NLS_LOG_ERROR("NativeDX12Queue::Present: GetNativeSwapchainHandle returned null");
					return;
				}

				if (ShouldLogDx12FrameFlow())
				{
					NLS_LOG_INFO("NativeDX12Queue::Present: swapchain ptr=" + std::to_string(reinterpret_cast<uintptr_t>(swapchain)) + " uiSemaphore=" + std::to_string(reinterpret_cast<uintptr_t>(presentDesc.uiSignalSemaphore)));
				}

				// Wait on UI render finished fence if provided - ensures UI rendering completes before present
				if (presentDesc.uiSignalSemaphore != nullptr)
				{
					ID3D12Fence* uiFence = reinterpret_cast<ID3D12Fence*>(presentDesc.uiSignalSemaphore);
					if (uiFence != nullptr)
					{
						HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
						if (fenceEvent != nullptr)
						{
							UINT64 lastCompletedValue = uiFence->GetCompletedValue();
							if (lastCompletedValue == UINT64_MAX)
							{
								NLS_LOG_WARNING("NativeDX12Queue::Present: UI fence in error state");
							}
							else
							{
								uiFence->AddRef();
								uiFence->Release();
							}
							CloseHandle(fenceEvent);
						}
					}
				}

				UINT presentFlags = 0;
				UINT syncInterval = 0;

				if (ShouldLogDx12FrameFlow())
				{
					NLS_LOG_INFO("NativeDX12Queue::Present: calling swapchain->Present");
				}
				HRESULT hr = swapchain->Present(syncInterval, presentFlags);
				if (ShouldLogDx12FrameFlow())
				{
					NLS_LOG_INFO("NativeDX12Queue::Present: Present returned hr=" + std::to_string(hr));
				}
				if (FAILED(hr))
				{
					NLS_LOG_ERROR("NativeDX12Queue::Present: Present failed with hr=" + std::to_string(hr));
					LogDx12DebugMessages(m_device, "NativeDX12Queue::Present");

					if (m_device != nullptr)
					{
						HRESULT reason = m_device->GetDeviceRemovedReason();
						if (FAILED(reason))
						{
							if (hr == DXGI_ERROR_DEVICE_REMOVED)
								NLS_LOG_ERROR("NativeDX12Queue::Present: DXGI_ERROR_DEVICE_REMOVED");
							NLS_LOG_ERROR("NativeDX12Queue::Present: Device removed reason hr=" + std::to_string(reason));
							LogDx12RemovalDiagnostics(m_device, "NativeDX12Queue::Present", reason);
						}
					}
				}
#endif
			}

		private:
			ID3D12Device* m_device = nullptr;
			ID3D12CommandQueue* m_queue = nullptr;
			std::string m_debugName;
		};

		class NativeDX12GraphicsPipeline;
		class NativeDX12CommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
		{
		public:
			NativeDX12CommandBuffer(ID3D12Device* device, ID3D12CommandQueue* queue, const std::string& debugName)
				: m_device(device)
				, m_queue(queue)
				, m_debugName(debugName)
			{
#if defined(_WIN32)
				if (device != nullptr)
				{
					device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_allocator));
					device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_allocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList));
					SetDx12ObjectName(m_allocator.Get(), m_debugName + "_Allocator");
					SetDx12ObjectName(m_commandList.Get(), m_debugName);
					if (m_commandList != nullptr)
						m_commandList->Close();
				}
#endif
			}

			~NativeDX12CommandBuffer()
			{
#if defined(_WIN32)
				// m_commandList/m_allocator are ComPtr-managed; manual Release() here
				// causes double-release during object teardown.
#endif
			}

			std::string_view GetDebugName() const override { return m_debugName; }
			void Begin() override
			{
#if defined(_WIN32)
				if (m_allocator != nullptr && m_commandList != nullptr)
				{
					m_allocator->Reset();
					m_commandList->Reset(m_allocator.Get(), nullptr);
					m_currentResourceDescriptorHeap = nullptr;
					m_currentSamplerDescriptorHeap = nullptr;
					m_boundDescriptorTables.clear();
					m_boundBindingSets.clear();
					m_recordedBindingSetKeepAlive.clear();
					m_recordedPipelineKeepAlive.clear();
					m_recording = true;
				}
#endif
			}
			void End() override
			{
#if defined(_WIN32)
				if (m_commandList != nullptr)
				{
					m_commandList->Close();
					m_recording = false;
				}
#endif
			}
			void Reset() override
			{
#if defined(_WIN32)
				if (m_allocator != nullptr)
					m_allocator->Reset();
#endif
			}
			bool IsRecording() const override { return m_recording; }
			void* GetNativeCommandBuffer() const override
			{
#if defined(_WIN32)
				return m_commandList.Get();
#else
				return nullptr;
#endif
			}

			void BeginRenderPass(const NLS::Render::RHI::RHIRenderPassDesc& desc) override
			{
#if defined(_WIN32)
				if (m_commandList == nullptr)
					return;

				m_activeRenderPassTransitions.clear();
				const auto clearPlan = NLS::Render::RHI::DX12::BuildDX12RenderPassClearPlan(desc);
				const bool isBackbufferPass = desc.debugName == "BackbufferRenderPass";
				for (const auto& colorAttachment : desc.colorAttachments)
				{
					if (colorAttachment.view == nullptr)
						continue;

					const auto& texture = colorAttachment.view->GetTexture();
					if (texture == nullptr)
						continue;

					const auto textureHandle = texture->GetNativeImageHandle();
					auto* resource = textureHandle.backend == NLS::Render::RHI::BackendType::DX12
						? static_cast<ID3D12Resource*>(textureHandle.handle)
						: nullptr;
					if (resource == nullptr)
						continue;

					const D3D12_RESOURCE_STATES stateBefore = isBackbufferPass
						? D3D12_RESOURCE_STATE_PRESENT
						: D3D12_RESOURCE_STATE_COMMON;
					const D3D12_RESOURCE_STATES stateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
					const D3D12_RESOURCE_STATES stateAfterPass = isBackbufferPass
						? D3D12_RESOURCE_STATE_PRESENT
						: D3D12_RESOURCE_STATE_COMMON;

					D3D12_RESOURCE_BARRIER barrier{};
					barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
					barrier.Transition.pResource = resource;
					barrier.Transition.StateBefore = stateBefore;
					barrier.Transition.StateAfter = stateAfter;
					barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
					m_commandList->ResourceBarrier(1, &barrier);

					m_activeRenderPassTransitions.push_back({
						resource,
						stateAfter,
						stateAfterPass
					});
				}

				if (desc.depthStencilAttachment.has_value() && desc.depthStencilAttachment->view != nullptr)
				{
					const auto& texture = desc.depthStencilAttachment->view->GetTexture();
					if (texture != nullptr)
					{
						const auto textureHandle = texture->GetNativeImageHandle();
						auto* resource = textureHandle.backend == NLS::Render::RHI::BackendType::DX12
							? static_cast<ID3D12Resource*>(textureHandle.handle)
							: nullptr;
						if (resource != nullptr)
						{
							D3D12_RESOURCE_BARRIER barrier{};
							barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
							barrier.Transition.pResource = resource;
							barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
							barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
							barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
							m_commandList->ResourceBarrier(1, &barrier);

							m_activeRenderPassTransitions.push_back({
								resource,
								D3D12_RESOURCE_STATE_DEPTH_WRITE,
								D3D12_RESOURCE_STATE_COMMON
							});
						}
					}
				}

				std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvHandles;
				rtvHandles.reserve(desc.colorAttachments.size());
				D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {};
				std::vector<bool> hasRtvHandle;
				hasRtvHandle.reserve(desc.colorAttachments.size());
				bool hasDSV = false;

				// Get RTVs from color attachments
				for (const auto& colorAttachment : desc.colorAttachments)
				{
					D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{};
					bool hasHandle = false;

					if (colorAttachment.view != nullptr)
					{
						NLS::Render::RHI::NativeHandle rtvHandleNative = colorAttachment.view->GetNativeRenderTargetView();
						void* rtv = rtvHandleNative.handle;
						if (rtv != nullptr)
						{
							rtvHandle.ptr = reinterpret_cast<UINT64>(rtv);
							hasHandle = true;
						}
					}

					rtvHandles.push_back(rtvHandle);
					hasRtvHandle.push_back(hasHandle);
				}

				// Get DSV from depth stencil attachment
				if (desc.depthStencilAttachment.has_value() && desc.depthStencilAttachment->view != nullptr)
				{
					NLS::Render::RHI::NativeHandle dsvHandleNative = desc.depthStencilAttachment->view->GetNativeDepthStencilView();
					void* dsv = dsvHandleNative.handle;
					if (dsv != nullptr)
					{
						dsvHandle.ptr = reinterpret_cast<UINT64>(dsv);
						hasDSV = true;
					}
				}

				// Clear values for render targets
				std::vector<D3D12_RECT> scissorRects;
				D3D12_RECT fullRect{};
				fullRect.left = 0;
				fullRect.top = 0;
				fullRect.right = desc.renderArea.width > 0 ? desc.renderArea.width : 1920;
				fullRect.bottom = desc.renderArea.height > 0 ? desc.renderArea.height : 1080;
				scissorRects.push_back(fullRect);
				m_commandList->RSSetScissorRects(1, scissorRects.data());

				std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> boundRtvHandles;
				boundRtvHandles.reserve(rtvHandles.size());
				for (size_t attachmentIndex = 0; attachmentIndex < rtvHandles.size(); ++attachmentIndex)
				{
					if (hasRtvHandle[attachmentIndex])
						boundRtvHandles.push_back(rtvHandles[attachmentIndex]);
				}

				if (!boundRtvHandles.empty() && hasDSV)
					m_commandList->OMSetRenderTargets(static_cast<UINT>(boundRtvHandles.size()), boundRtvHandles.data(), FALSE, &dsvHandle);
				else if (!boundRtvHandles.empty())
					m_commandList->OMSetRenderTargets(static_cast<UINT>(boundRtvHandles.size()), boundRtvHandles.data(), FALSE, nullptr);

				for (const uint32_t colorAttachmentIndex : clearPlan.colorAttachmentIndices)
				{
					if (colorAttachmentIndex >= rtvHandles.size() || !hasRtvHandle[colorAttachmentIndex])
						continue;

					const auto& clearValue = desc.colorAttachments[colorAttachmentIndex].clearValue;
					const FLOAT color[] = {
						clearValue.r,
						clearValue.g,
						clearValue.b,
						clearValue.a
					};
					m_commandList->ClearRenderTargetView(rtvHandles[colorAttachmentIndex], color, 1, &fullRect);
				}

				if (hasDSV && (clearPlan.clearDepth || clearPlan.clearStencil) && desc.depthStencilAttachment.has_value())
				{
					D3D12_CLEAR_FLAGS clearFlags = static_cast<D3D12_CLEAR_FLAGS>(0);
					if (clearPlan.clearDepth)
						clearFlags |= D3D12_CLEAR_FLAG_DEPTH;
					if (clearPlan.clearStencil)
						clearFlags |= D3D12_CLEAR_FLAG_STENCIL;

					const auto& clearValue = desc.depthStencilAttachment->clearValue;
					m_commandList->ClearDepthStencilView(
						dsvHandle,
						clearFlags,
						clearValue.depth,
						static_cast<UINT8>(clearValue.stencil),
						1,
						&fullRect);
				}
#endif
			}
			void EndRenderPass() override
			{
#if defined(_WIN32)
				if (m_commandList == nullptr || m_activeRenderPassTransitions.empty())
					return;

				std::vector<D3D12_RESOURCE_BARRIER> barriers;
				barriers.reserve(m_activeRenderPassTransitions.size());
				for (const auto& transition : m_activeRenderPassTransitions)
				{
					if (transition.resource == nullptr)
						continue;

					D3D12_RESOURCE_BARRIER barrier{};
					barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
					barrier.Transition.pResource = transition.resource;
					barrier.Transition.StateBefore = transition.stateAfterBegin;
					barrier.Transition.StateAfter = transition.stateAfterEnd;
					barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
					barriers.push_back(barrier);
				}

				if (!barriers.empty())
					m_commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
				m_activeRenderPassTransitions.clear();
#endif
			}
			void SetViewport(const NLS::Render::RHI::RHIViewport& viewport) override
			{
#if defined(_WIN32)
				if (m_commandList == nullptr)
					return;
				D3D12_VIEWPORT vp{};
				vp.Width = viewport.width;
				vp.Height = viewport.height;
				vp.MinDepth = viewport.minDepth;
				vp.MaxDepth = viewport.maxDepth;
				m_commandList->RSSetViewports(1, &vp);
#endif
			}
			void SetScissor(const NLS::Render::RHI::RHIRect2D& rect) override
			{
#if defined(_WIN32)
				if (m_commandList == nullptr)
					return;
				D3D12_RECT scissor{};
				scissor.left = rect.x;
				scissor.top = rect.y;
				scissor.right = rect.x + rect.width;
				scissor.bottom = rect.y + rect.height;
				m_commandList->RSSetScissorRects(1, &scissor);
#endif
			}
			void BindGraphicsPipeline(const std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>& pipeline) override;
			void BindComputePipeline(const std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>& pipeline) override
			{
#if defined(_WIN32)
				if (m_commandList == nullptr || pipeline == nullptr)
					return;
				m_boundComputePipeline = pipeline;
#endif
			}
			void BindBindingSet(uint32_t setIndex, const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& bindingSet) override;
			void PushConstants(NLS::Render::RHI::ShaderStageMask stageMask, uint32_t offset, uint32_t size, const void* data) override
			{
#if defined(_WIN32)
				if (m_commandList == nullptr)
					return;
				m_commandList->SetGraphicsRoot32BitConstants(0, size / sizeof(uint32_t), data, offset / sizeof(uint32_t));
#endif
			}
			void BindVertexBuffer(uint32_t slot, const NLS::Render::RHI::RHIVertexBufferView& view) override
			{
#if defined(_WIN32)
				if (m_commandList == nullptr || view.buffer == nullptr)
					return;
				D3D12_VERTEX_BUFFER_VIEW vbView{};
				vbView.BufferLocation = view.buffer->GetGPUAddress() + view.offset;
				vbView.SizeInBytes = static_cast<UINT>(view.buffer->GetDesc().size - view.offset);
				vbView.StrideInBytes = view.stride;
				m_commandList->IASetVertexBuffers(slot, 1, &vbView);
#endif
			}
			void BindIndexBuffer(const NLS::Render::RHI::RHIIndexBufferView& view) override
			{
#if defined(_WIN32)
				if (m_commandList == nullptr || view.buffer == nullptr)
					return;
				D3D12_INDEX_BUFFER_VIEW ibView{};
				ibView.BufferLocation = view.buffer->GetGPUAddress() + view.offset;
				ibView.SizeInBytes = static_cast<UINT>(view.buffer->GetDesc().size - view.offset);
				ibView.Format = view.indexType == NLS::Render::RHI::IndexType::UInt16
					? DXGI_FORMAT_R16_UINT
					: DXGI_FORMAT_R32_UINT;
				m_commandList->IASetIndexBuffer(&ibView);
#endif
			}
			void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override
			{
#if defined(_WIN32)
				if (m_commandList != nullptr)
					m_commandList->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
#endif
			}
			void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) override
			{
#if defined(_WIN32)
				if (m_commandList == nullptr || m_boundPipeline == nullptr)
					return;
				m_commandList->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
#endif
			}
			void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override
			{
#if defined(_WIN32)
				if (m_commandList != nullptr)
					m_commandList->Dispatch(groupCountX, groupCountY, groupCountZ);
#endif
			}
			void CopyBuffer(const std::shared_ptr<NLS::Render::RHI::RHIBuffer>& source, const std::shared_ptr<NLS::Render::RHI::RHIBuffer>& destination, const NLS::Render::RHI::RHIBufferCopyRegion& region) override
			{
#if defined(_WIN32)
				if (m_commandList == nullptr || source == nullptr || destination == nullptr)
					return;

				auto srcHandle = source->GetNativeBufferHandle();
				auto dstHandle = destination->GetNativeBufferHandle();
				if (srcHandle.backend != NLS::Render::RHI::BackendType::DX12 || dstHandle.backend != NLS::Render::RHI::BackendType::DX12)
					return;

				ID3D12Resource* srcResource = static_cast<ID3D12Resource*>(srcHandle.handle);
				ID3D12Resource* dstResource = static_cast<ID3D12Resource*>(dstHandle.handle);
				if (srcResource == nullptr || dstResource == nullptr)
					return;

				m_commandList->CopyBufferRegion(dstResource, region.dstOffset, srcResource, region.srcOffset, region.size);
#endif
			}
			void CopyBufferToTexture(const NLS::Render::RHI::RHIBufferToTextureCopyDesc& desc) override
			{
#if defined(_WIN32)
				if (m_commandList == nullptr || desc.source == nullptr || desc.destination == nullptr)
					return;

				auto srcHandle = desc.source->GetNativeBufferHandle();
				ID3D12Resource* srcResource = (srcHandle.backend == NLS::Render::RHI::BackendType::DX12) ? static_cast<ID3D12Resource*>(srcHandle.handle) : nullptr;
				auto dstHandle = desc.destination->GetNativeImageHandle();
				ID3D12Resource* dstResource = (dstHandle.backend == NLS::Render::RHI::BackendType::DX12) ? static_cast<ID3D12Resource*>(dstHandle.handle) : nullptr;
				if (srcResource == nullptr || dstResource == nullptr)
					return;

				DXGI_FORMAT format = ToDXGIFormat(desc.destination->GetDesc().format);
				uint32_t bytesPerPixel = GetBytesPerPixel(format);

				D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
				footprint.Offset = desc.bufferOffset;
				footprint.Footprint.Format = format;
				footprint.Footprint.Width = desc.extent.width;
				footprint.Footprint.Height = desc.extent.height;
				footprint.Footprint.Depth = 1;
				footprint.Footprint.RowPitch = desc.extent.width * bytesPerPixel;

				D3D12_TEXTURE_COPY_LOCATION srcLocation{};
				srcLocation.pResource = srcResource;
				srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				srcLocation.PlacedFootprint = footprint;

				D3D12_TEXTURE_COPY_LOCATION dstLocation{};
				dstLocation.pResource = dstResource;
				dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				dstLocation.SubresourceIndex = desc.mipLevel;

				D3D12_BOX srcBox{};
				srcBox.left = 0;
				srcBox.top = 0;
				srcBox.front = 0;
				srcBox.right = desc.extent.width;
				srcBox.bottom = desc.extent.height;
				srcBox.back = 1;

				m_commandList->CopyTextureRegion(&dstLocation, desc.textureOffset.x, desc.textureOffset.y, desc.textureOffset.z, &srcLocation, &srcBox);
#endif
			}
			void CopyTexture(const NLS::Render::RHI::RHITextureCopyDesc& desc) override
			{
#if defined(_WIN32)
				if (m_commandList == nullptr || desc.source == nullptr || desc.destination == nullptr)
					return;

				auto srcHandle2 = desc.source->GetNativeImageHandle();
				ID3D12Resource* srcResource = (srcHandle2.backend == NLS::Render::RHI::BackendType::DX12) ? static_cast<ID3D12Resource*>(srcHandle2.handle) : nullptr;
				auto dstHandle = desc.destination->GetNativeImageHandle();
				ID3D12Resource* dstResource = (dstHandle.backend == NLS::Render::RHI::BackendType::DX12) ? static_cast<ID3D12Resource*>(dstHandle.handle) : nullptr;
				if (srcResource == nullptr || dstResource == nullptr)
					return;

				D3D12_TEXTURE_COPY_LOCATION srcLocation{};
				srcLocation.pResource = srcResource;
				srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				srcLocation.SubresourceIndex = desc.sourceRange.baseMipLevel;

				D3D12_TEXTURE_COPY_LOCATION dstLocation{};
				dstLocation.pResource = dstResource;
				dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				dstLocation.SubresourceIndex = desc.destinationRange.baseMipLevel;

				D3D12_BOX srcBox{};
				srcBox.left = desc.sourceOffset.x;
				srcBox.top = desc.sourceOffset.y;
				srcBox.front = desc.sourceOffset.z;
				srcBox.right = desc.sourceOffset.x + desc.extent.width;
				srcBox.bottom = desc.sourceOffset.y + desc.extent.height;
				srcBox.back = desc.sourceOffset.z + desc.extent.depth;

				m_commandList->CopyTextureRegion(&dstLocation, desc.destinationOffset.x, desc.destinationOffset.y, desc.destinationOffset.z, &srcLocation, &srcBox);
#endif
			}
			void Barrier(const NLS::Render::RHI::RHIBarrierDesc& barrier) override
			{
#if defined(_WIN32)
				if (m_commandList == nullptr)
					return;

				std::vector<D3D12_RESOURCE_BARRIER> barriers;

				// Convert buffer barriers
				for (const auto& bb : barrier.bufferBarriers)
				{
					D3D12_RESOURCE_BARRIER barrier{};
					barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
					auto bbHandle = bb.buffer->GetNativeBufferHandle();
					barrier.Transition.pResource = (bbHandle.backend == NLS::Render::RHI::BackendType::DX12) ? static_cast<ID3D12Resource*>(bbHandle.handle) : nullptr;
					barrier.Transition.StateBefore = ToD3D12ResourceState(bb.before);
					barrier.Transition.StateAfter = ToD3D12ResourceState(bb.after);
					barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
					barriers.push_back(barrier);
				}

				// Convert texture barriers
				for (const auto& tb : barrier.textureBarriers)
				{
					D3D12_RESOURCE_BARRIER barrier{};
					barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
					auto tbHandle = tb.texture->GetNativeImageHandle();
					barrier.Transition.pResource = (tbHandle.backend == NLS::Render::RHI::BackendType::DX12) ? static_cast<ID3D12Resource*>(tbHandle.handle) : nullptr;
					barrier.Transition.StateBefore = ToD3D12ResourceState(tb.before);
					barrier.Transition.StateAfter = ToD3D12ResourceState(tb.after);
					barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
					barriers.push_back(barrier);
				}

				if (!barriers.empty())
					m_commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
#endif
			}

#if defined(_WIN32)
			ID3D12GraphicsCommandList* GetCommandList() const { return m_commandList.Get(); }

			static DXGI_FORMAT ToDXGIFormat(NLS::Render::RHI::TextureFormat format)
			{
				switch (format)
				{
				case NLS::Render::RHI::TextureFormat::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
				case NLS::Render::RHI::TextureFormat::RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
				case NLS::Render::RHI::TextureFormat::Depth24Stencil8: return DXGI_FORMAT_D24_UNORM_S8_UINT;
				case NLS::Render::RHI::TextureFormat::RGB8: return DXGI_FORMAT_R8G8B8A8_UNORM;
				default: return DXGI_FORMAT_R8G8B8A8_UNORM;
				}
			}

			static uint32_t GetBytesPerPixel(DXGI_FORMAT format)
			{
				switch (format)
				{
				case DXGI_FORMAT_R8G8B8A8_UNORM:
				case DXGI_FORMAT_R8G8B8A8_UINT:
				case DXGI_FORMAT_R8G8B8A8_SNORM:
				case DXGI_FORMAT_R8G8B8A8_SINT:
				case DXGI_FORMAT_D24_UNORM_S8_UINT:
					return 4;
				case DXGI_FORMAT_R16G16B16A16_FLOAT:
				case DXGI_FORMAT_R16G16B16A16_UNORM:
				case DXGI_FORMAT_R16G16B16A16_UINT:
				case DXGI_FORMAT_R16G16B16A16_SNORM:
				case DXGI_FORMAT_R16G16B16A16_SINT:
					return 8;
				default:
					return 4;
				}
			}

			static D3D12_RESOURCE_STATES ToD3D12ResourceState(NLS::Render::RHI::ResourceState state)
			{
				D3D12_RESOURCE_STATES result = D3D12_RESOURCE_STATE_COMMON;
				if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::CopySrc))
					result |= D3D12_RESOURCE_STATE_COPY_SOURCE;
				if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::CopyDst))
					result |= D3D12_RESOURCE_STATE_COPY_DEST;
				if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::VertexBuffer))
					result |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
				if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::IndexBuffer))
					result |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
				if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::UniformBuffer))
					result |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
				if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::ShaderRead))
					result |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
				if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::ShaderWrite))
					result |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
				if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::RenderTarget))
					result |= D3D12_RESOURCE_STATE_RENDER_TARGET;
				if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::DepthRead))
					result |= D3D12_RESOURCE_STATE_DEPTH_READ;
				if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::DepthWrite))
					result |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
				if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::Present))
					result |= D3D12_RESOURCE_STATE_PRESENT;
				return result;
			}
#endif

		private:
			std::string m_debugName;
			bool m_recording = false;
#if defined(_WIN32)
			ID3D12Device* m_device = nullptr;
			ID3D12CommandQueue* m_queue = nullptr;
			ComPtr<ID3D12CommandAllocator> m_allocator;
			ComPtr<ID3D12GraphicsCommandList> m_commandList;
			ID3D12DescriptorHeap* m_currentResourceDescriptorHeap = nullptr;
			ID3D12DescriptorHeap* m_currentSamplerDescriptorHeap = nullptr;
			struct ActiveRenderPassTransition
			{
				ID3D12Resource* resource = nullptr;
				D3D12_RESOURCE_STATES stateAfterBegin = D3D12_RESOURCE_STATE_COMMON;
				D3D12_RESOURCE_STATES stateAfterEnd = D3D12_RESOURCE_STATE_COMMON;
			};
			std::vector<ActiveRenderPassTransition> m_activeRenderPassTransitions;
#endif
			std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> m_boundPipeline;
			std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> m_boundComputePipeline;
			std::vector<NLS::Render::RHI::DX12::DX12DescriptorTableDesc> m_boundDescriptorTables;
			std::vector<std::pair<uint32_t, std::shared_ptr<NLS::Render::RHI::RHIBindingSet>>> m_boundBindingSets;
			std::vector<std::shared_ptr<NLS::Render::RHI::RHIBindingSet>> m_recordedBindingSetKeepAlive;
			std::vector<std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>> m_recordedPipelineKeepAlive;
		};

		void NativeDX12Queue::Submit(const NLS::Render::RHI::RHISubmitDesc& submitDesc)
		{
#if defined(_WIN32)
			if (m_queue == nullptr)
				return;

			std::vector<ID3D12CommandList*> commandLists;
			for (const auto& cmdBuffer : submitDesc.commandBuffers)
			{
				if (cmdBuffer == nullptr)
					continue;
				auto* nativeCmdBuffer = dynamic_cast<NativeDX12CommandBuffer*>(cmdBuffer.get());
				if (nativeCmdBuffer != nullptr && nativeCmdBuffer->GetCommandList() != nullptr)
					commandLists.push_back(nativeCmdBuffer->GetCommandList());
			}

			if (!commandLists.empty())
			{
				if (ShouldLogDx12FrameFlow())
				{
					NLS_LOG_INFO("NativeDX12Queue::Submit: executing " + std::to_string(commandLists.size()) + " command lists");
				}
				m_queue->ExecuteCommandLists(static_cast<UINT>(commandLists.size()), commandLists.data());
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
						NLS_LOG_ERROR("NativeDX12Queue::Submit: device status after ExecuteCommandLists hr=" + std::to_string(deviceStatus));
						LogDx12RemovalDiagnostics(m_device, "NativeDX12Queue::Submit", deviceStatus);
					}
				}
			}
			else
			{
				if (ShouldLogDx12FrameFlow())
				{
					NLS_LOG_INFO("NativeDX12Queue::Submit: no command lists to execute");
				}
			}

			if (submitDesc.signalFence != nullptr)
			{
				auto* nativeFence = dynamic_cast<NativeDX12Fence*>(submitDesc.signalFence.get());
				if (nativeFence != nullptr && nativeFence->GetFence() != nullptr)
				{
					const UINT64 fenceValue = nativeFence->GetTargetValue();
					const HRESULT signalHr = m_queue->Signal(nativeFence->GetFence(), fenceValue);
					if (FAILED(signalHr))
					{
						NLS_LOG_ERROR(
							"NativeDX12Queue::Submit: queue signal fence failed hr=" +
							std::to_string(signalHr) +
							" value=" + std::to_string(fenceValue));
					}
				}
			}
#endif
		}

		class NativeDX12CommandPool final : public NLS::Render::RHI::RHICommandPool
		{
		public:
			NativeDX12CommandPool(ID3D12Device* device, ID3D12CommandQueue* queue, NLS::Render::RHI::QueueType queueType, const std::string& debugName)
				: m_device(device)
				, m_queue(queue)
				, m_queueType(queueType)
				, m_debugName(debugName)
			{
			}

			std::string_view GetDebugName() const override { return m_debugName; }
			NLS::Render::RHI::QueueType GetQueueType() const override { return m_queueType; }
			std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> CreateCommandBuffer(std::string debugName) override
			{
#if defined(_WIN32)
				return std::make_shared<NativeDX12CommandBuffer>(m_device, m_queue, debugName.empty() ? m_debugName : debugName);
#else
				return nullptr;
#endif
			}
			void Reset() override
			{
			}

		private:
			ID3D12Device* m_device = nullptr;
			ID3D12CommandQueue* m_queue = nullptr;
			NLS::Render::RHI::QueueType m_queueType = NLS::Render::RHI::QueueType::Graphics;
			std::string m_debugName;
		};

		// Simple view wrapper for swapchain backbuffer RTVs
		class NativeDX12BackbufferTexture final : public NLS::Render::RHI::RHITexture
		{
		public:
			NativeDX12BackbufferTexture(
				Microsoft::WRL::ComPtr<ID3D12Resource> backbuffer,
				DXGI_FORMAT format,
				uint32_t width,
				uint32_t height)
				: m_backbuffer(backbuffer)
			{
				m_desc.extent.width = width;
				m_desc.extent.height = height;
				m_desc.extent.depth = 1;
				m_desc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
				m_desc.format = format == DXGI_FORMAT_R16G16B16A16_FLOAT
					? NLS::Render::RHI::TextureFormat::RGBA16F
					: NLS::Render::RHI::TextureFormat::RGBA8;
				m_desc.usage = NLS::Render::RHI::TextureUsageFlags::ColorAttachment;
				m_desc.debugName = "BackbufferTexture";
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
			NLS::Render::RHI::ResourceState GetState() const override { return NLS::Render::RHI::ResourceState::Present; }
			NLS::Render::RHI::NativeHandle GetNativeImageHandle() override
			{
				return { NLS::Render::RHI::BackendType::DX12, static_cast<void*>(m_backbuffer.Get()) };
			}

		private:
			NLS::Render::RHI::RHITextureDesc m_desc{};
			Microsoft::WRL::ComPtr<ID3D12Resource> m_backbuffer;
		};

		class NativeDX12BackbufferView final : public NLS::Render::RHI::RHITextureView
		{
		public:
			NativeDX12BackbufferView(
				Microsoft::WRL::ComPtr<ID3D12Resource> backbuffer,
				D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle,
				DXGI_FORMAT format,
				uint32_t width,
				uint32_t height)
				: m_backbuffer(backbuffer)
				, m_rtvHandle(rtvHandle)
				, m_format(format)
			{
				m_texture = std::make_shared<NativeDX12BackbufferTexture>(backbuffer, format, width, height);
			}

			std::string_view GetDebugName() const override { return "BackbufferView"; }
			const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
			const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override { return m_texture; }
			NLS::Render::RHI::NativeHandle GetNativeRenderTargetView() override { return { NLS::Render::RHI::BackendType::DX12, reinterpret_cast<void*>(m_rtvHandle.ptr) }; }
			NLS::Render::RHI::NativeHandle GetNativeDepthStencilView() override { return { NLS::Render::RHI::BackendType::DX12, nullptr }; }
			NLS::Render::RHI::NativeHandle GetNativeShaderResourceView() override { return { NLS::Render::RHI::BackendType::DX12, nullptr }; }

		private:
			NLS::Render::RHI::RHITextureViewDesc m_desc{};
			std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
			Microsoft::WRL::ComPtr<ID3D12Resource> m_backbuffer;
			D3D12_CPU_DESCRIPTOR_HANDLE m_rtvHandle = {};
			DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
		};

		class NativeDX12Swapchain final : public NLS::Render::RHI::RHISwapchain
		{
		public:
			NativeDX12Swapchain(Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain, ID3D12Device* device, const NLS::Render::RHI::SwapchainDesc& desc)
				: m_swapchain(swapchain)
				, m_device(device)
				, m_desc(desc)
				, m_imageCount(desc.imageCount > 0 ? desc.imageCount : 2)
			{
				NLS_LOG_INFO("NativeDX12Swapchain created: swapchain=" + std::to_string(reinterpret_cast<uintptr_t>(m_swapchain.Get())) + " device=" + std::to_string(reinterpret_cast<uintptr_t>(device)) + " imageCount=" + std::to_string(m_imageCount));
				// Pre-create RTVs for all backbuffer images
				RecreateBackbufferViews();
				NLS_LOG_INFO("NativeDX12Swapchain created: backbufferViews.size=" + std::to_string(m_backbufferViews.size()));
			}

			std::string_view GetDebugName() const override { return "NativeDX12Swapchain"; }
			const NLS::Render::RHI::SwapchainDesc& GetDesc() const override { return m_desc; }
			uint32_t GetImageCount() const override { return m_imageCount; }
			std::optional<NLS::Render::RHI::RHIAcquiredImage> AcquireNextImage(
				const std::shared_ptr<NLS::Render::RHI::RHISemaphore>& signalSemaphore,
				const std::shared_ptr<NLS::Render::RHI::RHIFence>& fence) override
			{
				(void)signalSemaphore;
				(void)fence;
				if (ShouldLogDx12FrameFlow())
				{
					NLS_LOG_INFO("NativeDX12Swapchain::AcquireNextImage: called");
				}

				// Frame synchronization is handled by Driver::BeginExplicitFrame() before Reset().
				// Waiting here again with the same fence target introduces a false 5s stall per frame.

				// Use GetCurrentBackBufferIndex to get the actual current backbuffer
				UINT currentIndex = m_swapchain->GetCurrentBackBufferIndex();
				if (ShouldLogDx12FrameFlow())
				{
					NLS_LOG_INFO("NativeDX12Swapchain::AcquireNextImage: currentIndex=" + std::to_string(currentIndex) + " backbufferViews.size=" + std::to_string(m_backbufferViews.size()));
				}

				NLS::Render::RHI::RHIAcquiredImage image;
				image.imageIndex = currentIndex;
				image.imageView = GetBackbufferView(currentIndex);
				return image;
			}
			std::shared_ptr<NLS::Render::RHI::RHITextureView> GetBackbufferView(uint32_t index) override
			{
				if (ShouldLogDx12FrameFlow())
				{
					NLS_LOG_INFO("NativeDX12Swapchain::GetBackbufferView: index=" + std::to_string(index) + " backbufferViews.size=" + std::to_string(m_backbufferViews.size()) + " m_device=" + std::to_string(reinterpret_cast<uintptr_t>(m_device)) + " m_swapchain=" + std::to_string(reinterpret_cast<uintptr_t>(m_swapchain.Get())));
				}
				// Ensure backbuffer views are created
				if (m_backbufferViews.empty() && m_device != nullptr && m_swapchain != nullptr)
				{
					if (ShouldLogDx12FrameFlow())
					{
						NLS_LOG_INFO("NativeDX12Swapchain::GetBackbufferView: calling RecreateBackbufferViews");
					}
					RecreateBackbufferViews();
					if (ShouldLogDx12FrameFlow())
					{
						NLS_LOG_INFO("NativeDX12Swapchain::GetBackbufferView: after RecreateBackbufferViews, size=" + std::to_string(m_backbufferViews.size()));
					}
				}
				if (index >= m_backbufferViews.size())
				{
					NLS_LOG_ERROR("NativeDX12Swapchain::GetBackbufferView: index " + std::to_string(index) + " >= size " + std::to_string(m_backbufferViews.size()));
					return nullptr;
				}
				return m_backbufferViews[index];
			}
			void Resize(uint32_t width, uint32_t height) override
			{
				m_desc.width = width;
				m_desc.height = height;
				if (m_swapchain != nullptr)
				{
					// DXGI requires every reference to the current backbuffers to be released
					// before ResizeBuffers, otherwise the swapchain stays on the old extent.
					ReleaseBackbufferViews();
					const HRESULT hr = m_swapchain->ResizeBuffers(
						m_imageCount,
						width,
						height,
						DXGI_FORMAT_R8G8B8A8_UNORM,
						0);
					if (FAILED(hr))
					{
						NLS_LOG_ERROR(
							"NativeDX12Swapchain::Resize: ResizeBuffers failed with hr=" +
							std::to_string(hr) +
							" requested=" +
							std::to_string(width) + "x" + std::to_string(height));
					}
				}
				RecreateBackbufferViews();
			}

			void* GetNativeSwapchainHandle() override { return m_swapchain.Get(); }

		private:
			void ReleaseBackbufferViews()
			{
				m_backbufferViews.clear();
				m_rtvHeap.Reset();
			}

			void RecreateBackbufferViews()
			{
				ReleaseBackbufferViews();
#if defined(_WIN32)
				if (m_device == nullptr || m_swapchain == nullptr)
					return;

				// Create an RTV heap for the backbuffers
				D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
				heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
				heapDesc.NumDescriptors = m_imageCount;
				heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

				if (FAILED(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_rtvHeap))))
					return;

				D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
				UINT rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

				for (uint32_t i = 0; i < m_imageCount; ++i)
				{
					ComPtr<ID3D12Resource> backbuffer;
					if (SUCCEEDED(m_swapchain->GetBuffer(i, IID_PPV_ARGS(&backbuffer))))
					{
						D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
						rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
						rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
						rtvDesc.Texture2D.MipSlice = 0;

						m_device->CreateRenderTargetView(backbuffer.Get(), &rtvDesc, rtvHandle);

						// Create a wrapper texture view that returns this RTV handle
						// Pass backbuffer to keep the resource alive for the lifetime of the view
						auto view = std::make_shared<NativeDX12BackbufferView>(
							backbuffer,
							rtvHandle,
							DXGI_FORMAT_R8G8B8A8_UNORM,
							m_desc.width,
							m_desc.height);
						m_backbufferViews.push_back(view);

						rtvHandle.ptr += rtvDescriptorSize;
					}
				}
#endif
			}

			Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapchain;
			ID3D12Device* m_device = nullptr;
			NLS::Render::RHI::SwapchainDesc m_desc{};
			uint32_t m_imageCount = 2;
			uint32_t m_nextImageIndex = 0;
			Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
			std::vector<std::shared_ptr<NLS::Render::RHI::RHITextureView>> m_backbufferViews;
		};

		class NativeDX12Buffer final : public NLS::Render::RHI::RHIBuffer
		{
		public:
			NativeDX12Buffer(
				ID3D12Device* device,
				ID3D12CommandQueue* graphicsQueue,
				const NLS::Render::RHI::RHIBufferDesc& desc,
				const void* initialData)
				: m_device(device)
				, m_graphicsQueue(graphicsQueue)
				, m_desc(desc)
			{
#if defined(_WIN32)
				if (device == nullptr)
					return;

				D3D12_HEAP_PROPERTIES heapProps{};
				heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
				heapProps.CreationNodeMask = 1;
				heapProps.VisibleNodeMask = 1;

				D3D12_RESOURCE_DESC resourceDesc{};
				resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				resourceDesc.Height = 1;
				resourceDesc.DepthOrArraySize = 1;
				resourceDesc.MipLevels = 1;
				resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
				resourceDesc.SampleDesc.Count = 1;
				resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

				// Determine appropriate heap type based on usage and memory usage
				D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;
				if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(NLS::Render::RHI::BufferUsageFlags::Uniform))
					heapType = D3D12_HEAP_TYPE_UPLOAD;
				else if (desc.memoryUsage == NLS::Render::RHI::MemoryUsage::CPUToGPU)
					heapType = D3D12_HEAP_TYPE_UPLOAD;
				else if (desc.memoryUsage == NLS::Render::RHI::MemoryUsage::GPUToCPU)
					heapType = D3D12_HEAP_TYPE_READBACK;

				const bool isUniformBuffer =
					(static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(NLS::Render::RHI::BufferUsageFlags::Uniform)) != 0u;
				const UINT64 resourceSize = isUniformBuffer
					? AlignUp(static_cast<UINT64>(desc.size), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT)
					: static_cast<UINT64>(desc.size);
				resourceDesc.Width = resourceSize;
				const bool needsDefaultHeapUpload = initialData != nullptr && heapType == D3D12_HEAP_TYPE_DEFAULT;

				heapProps.Type = heapType;
				const D3D12_RESOURCE_STATES initialState =
					heapType == D3D12_HEAP_TYPE_UPLOAD
						? D3D12_RESOURCE_STATE_GENERIC_READ
						: heapType == D3D12_HEAP_TYPE_READBACK
							? D3D12_RESOURCE_STATE_COPY_DEST
							: needsDefaultHeapUpload
								? D3D12_RESOURCE_STATE_COPY_DEST
								: D3D12_RESOURCE_STATE_COMMON;

				const HRESULT createResourceResult = device->CreateCommittedResource(
					&heapProps,
					D3D12_HEAP_FLAG_NONE,
					&resourceDesc,
					initialState,
					nullptr,
					IID_PPV_ARGS(&m_resource));
				if (FAILED(createResourceResult) || m_resource == nullptr)
					return;

				// Copy initial data if provided
				if (initialData != nullptr && heapType == D3D12_HEAP_TYPE_UPLOAD)
				{
					void* mappedData = nullptr;
					D3D12_RANGE readRange{};
					readRange.Begin = 0;
					readRange.End = 0;
					m_resource->Map(0, &readRange, &mappedData);
					if (mappedData != nullptr)
					{
						std::memcpy(mappedData, initialData, desc.size);
						D3D12_RANGE writeRange{};
						writeRange.Begin = 0;
						writeRange.End = desc.size;
						m_resource->Unmap(0, &writeRange);
					}
				}
				else if (needsDefaultHeapUpload && m_graphicsQueue != nullptr)
				{
					D3D12_HEAP_PROPERTIES uploadHeapProps{};
					uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
					uploadHeapProps.CreationNodeMask = 1;
					uploadHeapProps.VisibleNodeMask = 1;

					D3D12_RESOURCE_DESC uploadDesc = resourceDesc;
					uploadDesc.Width = static_cast<UINT64>(desc.size);
					ComPtr<ID3D12Resource> uploadBuffer;
					const HRESULT uploadCreateResult = device->CreateCommittedResource(
						&uploadHeapProps,
						D3D12_HEAP_FLAG_NONE,
						&uploadDesc,
						D3D12_RESOURCE_STATE_GENERIC_READ,
						nullptr,
						IID_PPV_ARGS(uploadBuffer.GetAddressOf()));
					if (FAILED(uploadCreateResult))
					{
						NLS_LOG_ERROR("NativeDX12Buffer: failed to create upload buffer hr=" + std::to_string(uploadCreateResult));
						return;
					}

					void* mappedData = nullptr;
					D3D12_RANGE readRange{};
					readRange.Begin = 0;
					readRange.End = 0;
					const HRESULT mapResult = uploadBuffer->Map(0, &readRange, &mappedData);
					if (FAILED(mapResult) || mappedData == nullptr)
					{
						NLS_LOG_ERROR("NativeDX12Buffer: failed to map upload buffer hr=" + std::to_string(mapResult));
						return;
					}
					std::memcpy(mappedData, initialData, desc.size);
					D3D12_RANGE writeRange{};
					writeRange.Begin = 0;
					writeRange.End = desc.size;
					uploadBuffer->Unmap(0, &writeRange);

					ComPtr<ID3D12CommandAllocator> commandAllocator;
					const HRESULT allocatorResult = device->CreateCommandAllocator(
						D3D12_COMMAND_LIST_TYPE_DIRECT,
						IID_PPV_ARGS(commandAllocator.GetAddressOf()));
					if (FAILED(allocatorResult))
					{
						NLS_LOG_ERROR("NativeDX12Buffer: failed to create upload command allocator hr=" + std::to_string(allocatorResult));
						return;
					}

					ComPtr<ID3D12GraphicsCommandList> commandList;
					const HRESULT commandListResult = device->CreateCommandList(
						0,
						D3D12_COMMAND_LIST_TYPE_DIRECT,
						commandAllocator.Get(),
						nullptr,
						IID_PPV_ARGS(commandList.GetAddressOf()));
					if (FAILED(commandListResult))
					{
						NLS_LOG_ERROR("NativeDX12Buffer: failed to create upload command list hr=" + std::to_string(commandListResult));
						return;
					}

					commandList->CopyBufferRegion(m_resource.Get(), 0, uploadBuffer.Get(), 0, desc.size);
					D3D12_RESOURCE_BARRIER toCommonBarrier{};
					toCommonBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
					toCommonBarrier.Transition.pResource = m_resource.Get();
					toCommonBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
					toCommonBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
					toCommonBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
					commandList->ResourceBarrier(1, &toCommonBarrier);

					const HRESULT closeResult = commandList->Close();
					if (FAILED(closeResult))
					{
						NLS_LOG_ERROR("NativeDX12Buffer: failed to close upload command list hr=" + std::to_string(closeResult));
						return;
					}

					ID3D12CommandList* commandLists[] = { commandList.Get() };
					m_graphicsQueue->ExecuteCommandLists(1, commandLists);

					ComPtr<ID3D12Fence> fence;
					const HRESULT fenceResult = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()));
					if (FAILED(fenceResult))
					{
						NLS_LOG_ERROR("NativeDX12Buffer: failed to create upload fence hr=" + std::to_string(fenceResult));
						return;
					}

					HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
					if (fenceEvent == nullptr)
					{
						NLS_LOG_ERROR("NativeDX12Buffer: failed to create upload fence event");
						return;
					}

					const HRESULT signalResult = m_graphicsQueue->Signal(fence.Get(), 1);
					if (FAILED(signalResult))
					{
						NLS_LOG_ERROR("NativeDX12Buffer: failed to signal upload fence hr=" + std::to_string(signalResult));
						CloseHandle(fenceEvent);
						return;
					}

					fence->SetEventOnCompletion(1, fenceEvent);
					WaitForSingleObject(fenceEvent, INFINITE);
					CloseHandle(fenceEvent);
				}
#endif
			}

			~NativeDX12Buffer()
			{
#if defined(_WIN32)
				if (m_resource != nullptr)
					m_resource.Reset();
#endif
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHIBufferDesc& GetDesc() const override { return m_desc; }
			NLS::Render::RHI::ResourceState GetState() const override { return m_state; }
			uint64_t GetGPUAddress() const override
			{
#if defined(_WIN32)
				if (m_resource == nullptr)
					return 0;
				return m_resource->GetGPUVirtualAddress();
#else
				return 0;
#endif
			}
			NLS::Render::RHI::NativeHandle GetNativeBufferHandle() override
			{
#if defined(_WIN32)
				return { NLS::Render::RHI::BackendType::DX12, static_cast<void*>(m_resource.Get()) };
#else
				return {};
#endif
			}

		private:
			ID3D12Device* m_device = nullptr;
			ID3D12CommandQueue* m_graphicsQueue = nullptr;
			NLS::Render::RHI::RHIBufferDesc m_desc{};
			NLS::Render::RHI::ResourceState m_state = NLS::Render::RHI::ResourceState::Unknown;
#if defined(_WIN32)
			ComPtr<ID3D12Resource> m_resource;
#endif
		};

		class NativeDX12Texture final : public NLS::Render::RHI::RHITexture
		{
		public:
			NativeDX12Texture(ID3D12Device* device, const NLS::Render::RHI::RHITextureDesc& desc, const void* initialData)
				: m_device(device)
				, m_desc(desc)
			{
#if defined(_WIN32)
				if (device == nullptr)
					return;

				const bool isDepthTexture = desc.format == NLS::Render::RHI::TextureFormat::Depth24Stencil8;
				const auto layerCount = static_cast<UINT16>(
					desc.dimension == NLS::Render::RHI::TextureDimension::TextureCube
						? NLS::Render::RHI::GetTextureLayerCount(desc.dimension)
						: (std::max)(desc.arrayLayers, 1u));

				D3D12_RESOURCE_DIMENSION dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
				if (desc.dimension == NLS::Render::RHI::TextureDimension::TextureCube)
					dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; // Cubemaps are stored as Texture2D arrays

				D3D12_RESOURCE_DESC resourceDesc{};
				resourceDesc.Dimension = dimension;
				resourceDesc.Width = desc.extent.width;
				resourceDesc.Height = desc.extent.height;
				resourceDesc.DepthOrArraySize = (desc.dimension == NLS::Render::RHI::TextureDimension::TextureCube) ? 6 : layerCount;
				resourceDesc.MipLevels = desc.mipLevels;
				resourceDesc.Format = ToDxgiFormat(desc.format);
				resourceDesc.SampleDesc.Count = desc.sampleCount;
				resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
				resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

				if (isDepthTexture)
					resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
				if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(NLS::Render::RHI::TextureUsageFlags::ColorAttachment))
					resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
				if (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(NLS::Render::RHI::TextureUsageFlags::Storage))
					resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

				const D3D12_HEAP_PROPERTIES heapProperties{
					D3D12_HEAP_TYPE_DEFAULT,
					D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
					D3D12_MEMORY_POOL_UNKNOWN,
					1,
					1
				};

				const D3D12_RESOURCE_STATES initialState =
					(initialData != nullptr && !isDepthTexture)
					? D3D12_RESOURCE_STATE_COPY_DEST
					: D3D12_RESOURCE_STATE_COMMON;

				D3D12_CLEAR_VALUE clearValue{};
				clearValue.Format = ToDxgiFormat(desc.format);
				if (isDepthTexture)
					clearValue.DepthStencil = { 1.0f, 0 };

				const bool hasClearValue = isDepthTexture;
				device->CreateCommittedResource(
					&heapProperties,
					D3D12_HEAP_FLAG_NONE,
					&resourceDesc,
					initialState,
					hasClearValue ? &clearValue : nullptr,
					IID_PPV_ARGS(&m_resource));

				if (m_resource != nullptr)
				{
					SetDx12ObjectName(m_resource.Get(), desc.debugName);
					m_state = initialState == D3D12_RESOURCE_STATE_COPY_DEST
						? NLS::Render::RHI::ResourceState::CopyDst
						: NLS::Render::RHI::ResourceState::Unknown;
				}
#endif
			}

			~NativeDX12Texture()
			{
#if defined(_WIN32)
				if (m_resource != nullptr)
					m_resource.Reset();
#endif
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
			NLS::Render::RHI::ResourceState GetState() const override { return m_state; }
			void SetState(NLS::Render::RHI::ResourceState state) { m_state = state; }
			NLS::Render::RHI::NativeHandle GetNativeImageHandle() override
			{
#if defined(_WIN32)
				return { NLS::Render::RHI::BackendType::DX12, static_cast<void*>(m_resource.Get()) };
#else
				return {};
#endif
			}
			void* GetNativeTextureHandle() const
			{
#if defined(_WIN32)
				return m_resource.Get();
#else
				return nullptr;
#endif
			}

			ID3D12Resource* GetResource() const
			{
#if defined(_WIN32)
				return m_resource.Get();
#else
				return nullptr;
#endif
			}

#if defined(_WIN32)
			static DXGI_FORMAT ToDxgiFormat(NLS::Render::RHI::TextureFormat format)
			{
				switch (format)
				{
				case NLS::Render::RHI::TextureFormat::RGB8:
				case NLS::Render::RHI::TextureFormat::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
				case NLS::Render::RHI::TextureFormat::RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
				case NLS::Render::RHI::TextureFormat::Depth24Stencil8: return DXGI_FORMAT_D24_UNORM_S8_UINT;
				default: return DXGI_FORMAT_R8G8B8A8_UNORM;
				}
			}
#endif

		private:
			ID3D12Device* m_device = nullptr;
			NLS::Render::RHI::RHITextureDesc m_desc{};
			NLS::Render::RHI::ResourceState m_state = NLS::Render::RHI::ResourceState::Unknown;
#if defined(_WIN32)
			ComPtr<ID3D12Resource> m_resource;
#endif
		};

		class NativeDX12TextureView final : public NLS::Render::RHI::RHITextureView
		{
		public:
			NativeDX12TextureView(ID3D12Device* device, const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture, const NLS::Render::RHI::RHITextureViewDesc& desc)
				: m_device(device)
				, m_texture(texture)
				, m_desc(desc)
			{
#if defined(_WIN32)
				if (device == nullptr || texture == nullptr)
					return;

				auto* nativeTexture = dynamic_cast<NativeDX12Texture*>(texture.get());
				if (nativeTexture == nullptr)
					return;

				ID3D12Resource* resource = nativeTexture->GetResource();
				if (resource == nullptr)
					return;

				// Create SRV heap for shader resource views
				D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
				srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
				srvHeapDesc.NumDescriptors = 1;
				srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
				if (FAILED(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap))))
					return;
				m_srvHandle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();

				// Create SRV for shader binding
				const auto descriptors = NLS::Render::RHI::DX12::BuildDX12TextureViewDescriptorSet(texture->GetDesc(), desc);
				if (descriptors.hasSrv)
					device->CreateShaderResourceView(resource, &descriptors.srvDesc, m_srvHandle);

				// Create RTV heap only for render-target-capable views
				if (descriptors.hasRtv)
				{
					D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
					rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
					rtvHeapDesc.NumDescriptors = 1;
					rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
					if (SUCCEEDED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap))))
					{
						m_rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
						device->CreateRenderTargetView(resource, &descriptors.rtvDesc, m_rtvHandle);
					}
				}

				// Create DSV only for depth views
				if (descriptors.hasDsv)
				{
					D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
					dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
					dsvHeapDesc.NumDescriptors = 1;
					dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
					if (SUCCEEDED(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap))))
					{
						m_dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
						device->CreateDepthStencilView(resource, &descriptors.dsvDesc, m_dsvHandle);
					}
				}
#endif
			}

			~NativeDX12TextureView()
			{
#if defined(_WIN32)
				m_srvHeap.Reset();
				m_rtvHeap.Reset();
				m_dsvHeap.Reset();
#endif
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
			const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override { return m_texture; }
			NLS::Render::RHI::NativeHandle GetNativeRenderTargetView() override { return { NLS::Render::RHI::BackendType::DX12, reinterpret_cast<void*>(m_rtvHandle.ptr) }; }
			NLS::Render::RHI::NativeHandle GetNativeDepthStencilView() override { return { NLS::Render::RHI::BackendType::DX12, reinterpret_cast<void*>(m_dsvHandle.ptr) }; }
			NLS::Render::RHI::NativeHandle GetNativeShaderResourceView() override
			{
				if (m_srvHeap == nullptr)
					return {};
				return { NLS::Render::RHI::BackendType::DX12, reinterpret_cast<void*>(m_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr) };
			}
			D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptorHandle() const
			{
				return m_srvHeap != nullptr ? m_srvHeap->GetGPUDescriptorHandleForHeapStart() : D3D12_GPU_DESCRIPTOR_HANDLE{};
			}

#if defined(_WIN32)
			ID3D12Resource* GetResource() const
			{
				auto* nativeTexture = dynamic_cast<NativeDX12Texture*>(m_texture.get());
				return nativeTexture ? nativeTexture->GetResource() : nullptr;
			}
#endif

		private:
			ID3D12Device* m_device = nullptr;
			std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
			NLS::Render::RHI::RHITextureViewDesc m_desc{};
#if defined(_WIN32)
			ComPtr<ID3D12DescriptorHeap> m_srvHeap;
			ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
			ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
			D3D12_CPU_DESCRIPTOR_HANDLE m_srvHandle = {};
			D3D12_CPU_DESCRIPTOR_HANDLE m_rtvHandle = {};
			D3D12_CPU_DESCRIPTOR_HANDLE m_dsvHandle = {};
#endif
		};

		// Shared shader-visible descriptor heap allocator for binding sets
		class DX12ShaderVisibleDescriptorHeapAllocator
		{
		public:
			DX12ShaderVisibleDescriptorHeapAllocator(
				ID3D12Device* device,
				ID3D12CommandQueue* commandQueue,
				D3D12_DESCRIPTOR_HEAP_TYPE heapType,
				UINT descriptorCapacity,
				const char* heapDebugName)
				: m_device(device)
				, m_commandQueue(commandQueue)
				, m_heapType(heapType)
				, m_descriptorCapacity(descriptorCapacity)
				, m_heapDebugName(heapDebugName != nullptr ? heapDebugName : "DX12DescriptorHeap")
			{
#if defined(_WIN32)
				if (m_device != nullptr)
				{
					D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
					srvHeapDesc.Type = m_heapType;
					srvHeapDesc.NumDescriptors = m_descriptorCapacity;
					srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
					srvHeapDesc.NodeMask = 0;
					if (SUCCEEDED(m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_heap))))
					{
						m_descriptorSize = m_device->GetDescriptorHandleIncrementSize(m_heapType);
						m_freeDescriptors.push_back({0, m_descriptorCapacity});

						D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = m_heap->GetGPUDescriptorHandleForHeapStart();

						// Create a temporary command list to ensure the heap is properly initialized on the GPU
						if (m_commandQueue != nullptr)
						{
							ComPtr<ID3D12CommandAllocator> allocator;
							ComPtr<ID3D12GraphicsCommandList> tempCmdList;
							if (SUCCEEDED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
							{
								if (SUCCEEDED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&tempCmdList))))
								{
									tempCmdList->Close();
									ID3D12CommandList* cmdLists[] = { tempCmdList.Get() };
									m_commandQueue->ExecuteCommandLists(1, cmdLists);

									ComPtr<ID3D12Fence> fence;
									if (SUCCEEDED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
									{
										HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
										if (fenceEvent != nullptr)
										{
											fence->SetEventOnCompletion(1, fenceEvent);
											m_commandQueue->Signal(fence.Get(), 1);
											WaitForSingleObject(fenceEvent, INFINITE);
											CloseHandle(fenceEvent);
										}
									}

									gpuHandle = m_heap->GetGPUDescriptorHandleForHeapStart();
								}
							}
						}

						// Only check for zero - modern Windows WDDM can assign very large GPU virtual addresses (PB range)
						// The 256TB threshold was too restrictive for RTX 3080 Ti on Windows
						if (gpuHandle.ptr == 0)
						{
							NLS_LOG_ERROR(m_heapDebugName + ": GPU handle is zero after initialization!");
						}
					}
					else
					{
						NLS_LOG_ERROR(m_heapDebugName + ": Failed to create descriptor heap");
					}
				}
#endif
			}

			ID3D12DescriptorHeap* GetHeap() const { return m_heap.Get(); }
			UINT GetDescriptorSize() const { return m_descriptorSize; }
			D3D12_CPU_DESCRIPTOR_HANDLE GetCpuHandle() const { return m_heap->GetCPUDescriptorHandleForHeapStart(); }
			D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandle() const
			{
				if (m_heap == nullptr)
				{
					NLS_LOG_ERROR(m_heapDebugName + "::GetGpuHandle: heap is null");
					return {};
				}
				D3D12_GPU_DESCRIPTOR_HANDLE handle = m_heap->GetGPUDescriptorHandleForHeapStart();

				// Only check for zero - modern Windows WDDM can assign very large GPU virtual addresses (PB range)
				// The 256TB threshold was too restrictive for RTX 3080 Ti on Windows
				if (handle.ptr == 0)
				{
					NLS_LOG_ERROR(m_heapDebugName + "::GetGpuHandle: GPU handle is zero");
					return {};
				}

				return handle;
			}

			UINT Allocate(UINT count = 1)
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				for (auto it = m_freeDescriptors.begin(); it != m_freeDescriptors.end(); ++it)
				{
					if (it->second >= count)
					{
						UINT offset = it->first;
						it->first += count;
						it->second -= count;
						if (it->second == 0)
							m_freeDescriptors.erase(it);
						return offset;
					}
				}
				NLS_LOG_ERROR(m_heapDebugName + ": Out of descriptors!");
				return UINT_MAX;
			}

			void Free(UINT offset, UINT count = 1)
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_freeDescriptors.push_back({offset, count});
			}

		private:
			ID3D12Device* m_device = nullptr;
			ID3D12CommandQueue* m_commandQueue = nullptr;
			D3D12_DESCRIPTOR_HEAP_TYPE m_heapType = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			UINT m_descriptorCapacity = 0;
			std::string m_heapDebugName;
#if defined(_WIN32)
			Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
			UINT m_descriptorSize = 0;
			std::mutex m_mutex;
			std::vector<std::pair<UINT, UINT>> m_freeDescriptors;
#endif
		};

		class NativeDX12BindingSet final : public NLS::Render::RHI::RHIBindingSet
		{
		public:
			explicit NativeDX12BindingSet(
				ID3D12Device* device,
				NLS::Render::RHI::RHIBindingSetDesc desc,
				DX12ShaderVisibleDescriptorHeapAllocator* resourceHeapAllocator,
				DX12ShaderVisibleDescriptorHeapAllocator* samplerHeapAllocator)
				: m_device(device)
				, m_desc(std::move(desc))
				, m_resourceHeapAllocator(resourceHeapAllocator)
				, m_samplerHeapAllocator(samplerHeapAllocator)
			{
#if defined(_WIN32)
				if (m_device == nullptr || m_desc.layout == nullptr)
				{
					NLS_LOG_ERROR("NativeDX12BindingSet: device or layout is null");
					return;
				}

				NLS::Render::RHI::RHIPipelineLayoutDesc pipelineLayoutDesc;
				pipelineLayoutDesc.bindingLayouts.push_back(m_desc.layout);
				const auto tables = NLS::Render::RHI::DX12::BuildDX12DescriptorTableDescs(pipelineLayoutDesc);

				m_resourceDescriptorCount = 0;
				m_samplerDescriptorCount = 0;
				for (const auto& table : tables)
				{
					for (const auto& range : table.ranges)
					{
						if (table.heapKind == NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Sampler)
							m_samplerDescriptorCount += range.descriptorCount;
						else
							m_resourceDescriptorCount += range.descriptorCount;
					}
				}

				if (m_resourceDescriptorCount > 0)
				{
					if (m_resourceHeapAllocator == nullptr || m_resourceHeapAllocator->GetHeap() == nullptr)
					{
						NLS_LOG_ERROR("NativeDX12BindingSet: resource descriptor heap allocator is null");
						return;
					}

					m_resourceDescriptorOffset = m_resourceHeapAllocator->Allocate(m_resourceDescriptorCount);
					if (m_resourceDescriptorOffset == UINT_MAX)
					{
						NLS_LOG_ERROR("NativeDX12BindingSet: failed to allocate resource descriptors");
						return;
					}

					m_resourceDescriptorSize = m_resourceHeapAllocator->GetDescriptorSize();
				}

				if (m_samplerDescriptorCount > 0)
				{
					if (m_samplerHeapAllocator == nullptr || m_samplerHeapAllocator->GetHeap() == nullptr)
					{
						NLS_LOG_ERROR("NativeDX12BindingSet: sampler descriptor heap allocator is null");
						return;
					}

					m_samplerDescriptorOffset = m_samplerHeapAllocator->Allocate(m_samplerDescriptorCount);
					if (m_samplerDescriptorOffset == UINT_MAX)
					{
						NLS_LOG_ERROR("NativeDX12BindingSet: failed to allocate sampler descriptors");
						return;
					}

					m_samplerDescriptorSize = m_samplerHeapAllocator->GetDescriptorSize();
				}

				UINT resourceCursor = 0;
				UINT samplerCursor = 0;
				for (const auto& table : tables)
				{
					UINT tableDescriptorCount = 0;
					for (const auto& range : table.ranges)
						tableDescriptorCount += range.descriptorCount;

					if (tableDescriptorCount == 0)
						continue;

					const bool usesSamplerHeap =
						table.heapKind == NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Sampler;
					D3D12_CPU_DESCRIPTOR_HANDLE tableCpuHandle = usesSamplerHeap
						? ComputeSamplerCpuHandle(samplerCursor)
						: ComputeResourceCpuHandle(resourceCursor);
					const D3D12_GPU_DESCRIPTOR_HANDLE tableGpuHandle = usesSamplerHeap
						? ComputeSamplerGpuHandle(samplerCursor)
						: ComputeResourceGpuHandle(resourceCursor);

					m_descriptorTables.push_back({ table.set, table.heapKind, tableGpuHandle });

					D3D12_CPU_DESCRIPTOR_HANDLE destHandle = tableCpuHandle;
					for (const auto& range : table.ranges)
					{
						const auto* layoutEntry = FindLayoutEntry(range);
						const auto* boundEntry = layoutEntry != nullptr ? FindBoundEntry(*layoutEntry) : nullptr;
						for (uint32_t descriptorIndex = 0; descriptorIndex < range.descriptorCount; ++descriptorIndex)
						{
							if (usesSamplerHeap)
								WriteSamplerDescriptor(boundEntry, destHandle);
							else
								WriteResourceDescriptor(layoutEntry, boundEntry, destHandle);

							destHandle.ptr += usesSamplerHeap ? m_samplerDescriptorSize : m_resourceDescriptorSize;
						}
					}

					if (usesSamplerHeap)
						samplerCursor += tableDescriptorCount;
					else
						resourceCursor += tableDescriptorCount;
				}
#endif
			}

			~NativeDX12BindingSet()
			{
#if defined(_WIN32)
				if (m_resourceHeapAllocator != nullptr && m_resourceDescriptorOffset != UINT_MAX && m_resourceDescriptorCount > 0)
					m_resourceHeapAllocator->Free(m_resourceDescriptorOffset, m_resourceDescriptorCount);
				if (m_samplerHeapAllocator != nullptr && m_samplerDescriptorOffset != UINT_MAX && m_samplerDescriptorCount > 0)
					m_samplerHeapAllocator->Free(m_samplerDescriptorOffset, m_samplerDescriptorCount);
#endif
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHIBindingSetDesc& GetDesc() const override { return m_desc; }

			D3D12_GPU_DESCRIPTOR_HANDLE GetGPUHandle(
				uint32_t set,
				NLS::Render::RHI::DX12::DX12DescriptorHeapKind heapKind) const
			{
				const auto tableIt = std::find_if(
					m_descriptorTables.begin(),
					m_descriptorTables.end(),
					[set, heapKind](const DescriptorTableBinding& table)
					{
						return table.set == set && table.heapKind == heapKind;
					});
				return tableIt != m_descriptorTables.end() ? tableIt->gpuHandle : D3D12_GPU_DESCRIPTOR_HANDLE{};
			}

			ID3D12DescriptorHeap* GetDescriptorHeap(NLS::Render::RHI::DX12::DX12DescriptorHeapKind heapKind) const
			{
				return heapKind == NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Sampler
					? (m_samplerHeapAllocator != nullptr ? m_samplerHeapAllocator->GetHeap() : nullptr)
					: (m_resourceHeapAllocator != nullptr ? m_resourceHeapAllocator->GetHeap() : nullptr);
			}

		private:
			struct DescriptorTableBinding
			{
				uint32_t set = 0;
				NLS::Render::RHI::DX12::DX12DescriptorHeapKind heapKind = NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Resource;
				D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = {};
			};

			static D3D12_FILTER ToD3D12Filter(
				NLS::Render::RHI::TextureFilter minFilter,
				NLS::Render::RHI::TextureFilter magFilter)
			{
				if (minFilter == NLS::Render::RHI::TextureFilter::Nearest &&
					magFilter == NLS::Render::RHI::TextureFilter::Nearest)
				{
					return D3D12_FILTER_MIN_MAG_MIP_POINT;
				}

				if (minFilter == NLS::Render::RHI::TextureFilter::Nearest)
					return D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
				if (magFilter == NLS::Render::RHI::TextureFilter::Nearest)
					return D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT;
				return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			}

			static D3D12_TEXTURE_ADDRESS_MODE ToD3D12AddressMode(NLS::Render::RHI::TextureWrap wrap)
			{
				return wrap == NLS::Render::RHI::TextureWrap::ClampToEdge
					? D3D12_TEXTURE_ADDRESS_MODE_CLAMP
					: D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			}

			static D3D12_SAMPLER_DESC BuildSamplerDesc(const NLS::Render::RHI::SamplerDesc& desc)
			{
				D3D12_SAMPLER_DESC samplerDesc{};
				samplerDesc.Filter = ToD3D12Filter(desc.minFilter, desc.magFilter);
				samplerDesc.AddressU = ToD3D12AddressMode(desc.wrapU);
				samplerDesc.AddressV = ToD3D12AddressMode(desc.wrapV);
				samplerDesc.AddressW = ToD3D12AddressMode(desc.wrapW);
				samplerDesc.MinLOD = 0.0f;
				samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
				samplerDesc.MaxAnisotropy = 1;
				samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
				return samplerDesc;
			}

			static NLS::Render::RHI::DX12::DX12DescriptorRangeCategory ToRangeCategory(
				NLS::Render::RHI::BindingType type)
			{
				switch (type)
				{
				case NLS::Render::RHI::BindingType::UniformBuffer:
				case NLS::Render::RHI::BindingType::StorageBuffer:
					return NLS::Render::RHI::DX12::DX12DescriptorRangeCategory::ConstantBuffer;
				case NLS::Render::RHI::BindingType::Texture:
				case NLS::Render::RHI::BindingType::RWTexture:
					return NLS::Render::RHI::DX12::DX12DescriptorRangeCategory::ShaderResource;
				case NLS::Render::RHI::BindingType::Sampler:
				default:
					return NLS::Render::RHI::DX12::DX12DescriptorRangeCategory::Sampler;
				}
			}

			const NLS::Render::RHI::RHIBindingLayoutEntry* FindLayoutEntry(
				const NLS::Render::RHI::DX12::DX12DescriptorTableRangeDesc& range) const
			{
				const auto& entries = m_desc.layout->GetDesc().entries;
				const auto entryIt = std::find_if(
					entries.begin(),
					entries.end(),
					[&range](const NLS::Render::RHI::RHIBindingLayoutEntry& entry)
					{
						return entry.binding == range.binding &&
							entry.registerSpace == range.registerSpace &&
							entry.count == range.descriptorCount &&
							ToRangeCategory(entry.type) == range.category;
					});
				return entryIt != entries.end() ? &(*entryIt) : nullptr;
			}

			const NLS::Render::RHI::RHIBindingSetEntry* FindBoundEntry(
				const NLS::Render::RHI::RHIBindingLayoutEntry& layoutEntry) const
			{
				const auto entryIt = std::find_if(
					m_desc.entries.begin(),
					m_desc.entries.end(),
					[&layoutEntry](const NLS::Render::RHI::RHIBindingSetEntry& entry)
					{
						return entry.binding == layoutEntry.binding && entry.type == layoutEntry.type;
					});
				return entryIt != m_desc.entries.end() ? &(*entryIt) : nullptr;
			}

			D3D12_CPU_DESCRIPTOR_HANDLE ComputeResourceCpuHandle(UINT descriptorIndex) const
			{
				D3D12_CPU_DESCRIPTOR_HANDLE handle = m_resourceHeapAllocator->GetCpuHandle();
				handle.ptr += static_cast<SIZE_T>(m_resourceDescriptorOffset + descriptorIndex) * m_resourceDescriptorSize;
				return handle;
			}

			D3D12_GPU_DESCRIPTOR_HANDLE ComputeResourceGpuHandle(UINT descriptorIndex) const
			{
				D3D12_GPU_DESCRIPTOR_HANDLE handle = m_resourceHeapAllocator->GetGpuHandle();
				handle.ptr += static_cast<UINT64>(m_resourceDescriptorOffset + descriptorIndex) * m_resourceDescriptorSize;
				return handle;
			}

			D3D12_CPU_DESCRIPTOR_HANDLE ComputeSamplerCpuHandle(UINT descriptorIndex) const
			{
				D3D12_CPU_DESCRIPTOR_HANDLE handle = m_samplerHeapAllocator->GetCpuHandle();
				handle.ptr += static_cast<SIZE_T>(m_samplerDescriptorOffset + descriptorIndex) * m_samplerDescriptorSize;
				return handle;
			}

			D3D12_GPU_DESCRIPTOR_HANDLE ComputeSamplerGpuHandle(UINT descriptorIndex) const
			{
				D3D12_GPU_DESCRIPTOR_HANDLE handle = m_samplerHeapAllocator->GetGpuHandle();
				handle.ptr += static_cast<UINT64>(m_samplerDescriptorOffset + descriptorIndex) * m_samplerDescriptorSize;
				return handle;
			}

			void WriteSamplerDescriptor(
				const NLS::Render::RHI::RHIBindingSetEntry* boundEntry,
				D3D12_CPU_DESCRIPTOR_HANDLE destination) const
			{
				const auto defaultSamplerDesc = NLS::Render::RHI::SamplerDesc{};
				const auto& samplerDesc = boundEntry != nullptr && boundEntry->sampler != nullptr
					? boundEntry->sampler->GetDesc()
					: defaultSamplerDesc;
				const auto nativeSamplerDesc = BuildSamplerDesc(samplerDesc);
				m_device->CreateSampler(&nativeSamplerDesc, destination);
			}

			void WriteResourceDescriptor(
				const NLS::Render::RHI::RHIBindingLayoutEntry* layoutEntry,
				const NLS::Render::RHI::RHIBindingSetEntry* boundEntry,
				D3D12_CPU_DESCRIPTOR_HANDLE destination) const
			{
				if (layoutEntry == nullptr)
					return;

				switch (layoutEntry->type)
				{
				case NLS::Render::RHI::BindingType::UniformBuffer:
				case NLS::Render::RHI::BindingType::StorageBuffer:
				{
					if (boundEntry == nullptr || boundEntry->buffer == nullptr)
					{
						m_device->CreateConstantBufferView(nullptr, destination);
						return;
					}

					const auto bufferHandle = boundEntry->buffer->GetNativeBufferHandle();
					auto* resource = bufferHandle.backend == NLS::Render::RHI::BackendType::DX12
						? static_cast<ID3D12Resource*>(bufferHandle.handle)
						: nullptr;
					if (resource == nullptr)
					{
						m_device->CreateConstantBufferView(nullptr, destination);
						return;
					}

					const UINT64 bufferSize = boundEntry->bufferRange > 0
						? boundEntry->bufferRange
						: (boundEntry->buffer->GetDesc().size > boundEntry->bufferOffset
							? boundEntry->buffer->GetDesc().size - boundEntry->bufferOffset
							: 0u);
					if (bufferSize == 0)
					{
						m_device->CreateConstantBufferView(nullptr, destination);
						return;
					}

					D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
					cbvDesc.BufferLocation = resource->GetGPUVirtualAddress() + boundEntry->bufferOffset;
					cbvDesc.SizeInBytes = static_cast<UINT>(AlignUp(bufferSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT));
					m_device->CreateConstantBufferView(&cbvDesc, destination);
					return;
				}
				case NLS::Render::RHI::BindingType::Texture:
				case NLS::Render::RHI::BindingType::RWTexture:
				{
					D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
					ID3D12Resource* resource = nullptr;
					if (boundEntry != nullptr && boundEntry->textureView != nullptr)
					{
						auto* nativeTextureView = dynamic_cast<NativeDX12TextureView*>(boundEntry->textureView.get());
						if (nativeTextureView != nullptr)
						{
							const auto& viewDesc = nativeTextureView->GetDesc();
							const auto& texture = nativeTextureView->GetTexture();
							resource = nativeTextureView->GetResource();

							if (resource != nullptr &&
								texture != nullptr &&
								viewDesc.format != NLS::Render::RHI::TextureFormat::Depth24Stencil8 &&
								texture->GetDesc().format != NLS::Render::RHI::TextureFormat::Depth24Stencil8)
							{
								srvDesc.Format = NativeDX12Texture::ToDxgiFormat(viewDesc.format);
								srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

								if (viewDesc.viewType == NLS::Render::RHI::TextureViewType::Cube ||
									texture->GetDesc().dimension == NLS::Render::RHI::TextureDimension::TextureCube)
								{
									srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
									srvDesc.TextureCube.MipLevels =
										viewDesc.subresourceRange.mipLevelCount > 0 ? viewDesc.subresourceRange.mipLevelCount : 1u;
									srvDesc.TextureCube.MostDetailedMip = viewDesc.subresourceRange.baseMipLevel;
									m_device->CreateShaderResourceView(resource, &srvDesc, destination);
									return;
								}

								srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
								srvDesc.Texture2D.MipLevels =
									viewDesc.subresourceRange.mipLevelCount > 0 ? viewDesc.subresourceRange.mipLevelCount : 1u;
								srvDesc.Texture2D.MostDetailedMip = viewDesc.subresourceRange.baseMipLevel;
								m_device->CreateShaderResourceView(resource, &srvDesc, destination);
								return;
							}
						}
					}

					ZeroMemory(&srvDesc, sizeof(srvDesc));
					srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
					srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
					srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
					srvDesc.Texture2D.MipLevels = 1;
					m_device->CreateShaderResourceView(nullptr, &srvDesc, destination);
					return;
				}
				case NLS::Render::RHI::BindingType::Sampler:
				default:
					return;
				}
			}

			ID3D12Device* m_device = nullptr;
			NLS::Render::RHI::RHIBindingSetDesc m_desc;
			DX12ShaderVisibleDescriptorHeapAllocator* m_resourceHeapAllocator = nullptr;
			DX12ShaderVisibleDescriptorHeapAllocator* m_samplerHeapAllocator = nullptr;
#if defined(_WIN32)
			std::vector<DescriptorTableBinding> m_descriptorTables;
			UINT m_resourceDescriptorOffset = UINT_MAX;
			UINT m_resourceDescriptorCount = 0;
			UINT m_resourceDescriptorSize = 0;
			UINT m_samplerDescriptorOffset = UINT_MAX;
			UINT m_samplerDescriptorCount = 0;
			UINT m_samplerDescriptorSize = 0;
#endif
		};

		// BindBindingSet implementation - defined after NativeDX12BindingSet
		void NativeDX12CommandBuffer::BindBindingSet(uint32_t setIndex, const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& bindingSet)
		{
#if defined(_WIN32)
			if (m_commandList == nullptr || bindingSet == nullptr)
				return;

			auto* nativeBindingSet = dynamic_cast<NativeDX12BindingSet*>(bindingSet.get());
			if (nativeBindingSet == nullptr)
				return;

			if (m_boundDescriptorTables.empty())
				return;

			const auto existingBindingSetIt = std::find_if(
				m_boundBindingSets.begin(),
				m_boundBindingSets.end(),
				[setIndex](const auto& boundSet)
				{
					return boundSet.first == setIndex;
				});
			if (existingBindingSetIt != m_boundBindingSets.end())
			{
				existingBindingSetIt->second = bindingSet;
			}
			else
			{
				m_boundBindingSets.emplace_back(setIndex, bindingSet);
			}

			// Keep per-draw binding sets alive for the whole command-buffer recording.
			// Dynamic material snapshots (constant buffers) are owned by binding sets.
			// Releasing them before GPU execution can trigger device removal.
			m_recordedBindingSetKeepAlive.push_back(bindingSet);

			ID3D12DescriptorHeap* desiredResourceHeap = nullptr;
			ID3D12DescriptorHeap* desiredSamplerHeap = nullptr;
			for (const auto& [boundSetIndex, boundSet] : m_boundBindingSets)
			{
				auto* boundNativeBindingSet = dynamic_cast<NativeDX12BindingSet*>(boundSet.get());
				if (boundNativeBindingSet == nullptr)
					continue;

				if (desiredResourceHeap == nullptr)
				{
					desiredResourceHeap =
						boundNativeBindingSet->GetDescriptorHeap(NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Resource);
				}
				if (desiredSamplerHeap == nullptr)
				{
					desiredSamplerHeap =
						boundNativeBindingSet->GetDescriptorHeap(NLS::Render::RHI::DX12::DX12DescriptorHeapKind::Sampler);
				}
			}

			m_currentResourceDescriptorHeap = desiredResourceHeap;
			m_currentSamplerDescriptorHeap = desiredSamplerHeap;

			ID3D12DescriptorHeap* activeHeaps[2] = {};
			UINT activeHeapCount = 0;
			if (m_currentResourceDescriptorHeap != nullptr)
				activeHeaps[activeHeapCount++] = m_currentResourceDescriptorHeap;
			if (m_currentSamplerDescriptorHeap != nullptr)
				activeHeaps[activeHeapCount++] = m_currentSamplerDescriptorHeap;
			if (activeHeapCount > 0)
				m_commandList->SetDescriptorHeaps(activeHeapCount, activeHeaps);

			for (const auto& [boundSetIndex, boundSet] : m_boundBindingSets)
			{
				auto* boundNativeBindingSet = dynamic_cast<NativeDX12BindingSet*>(boundSet.get());
				if (boundNativeBindingSet == nullptr)
					continue;

				for (UINT rootParameterIndex = 0; rootParameterIndex < m_boundDescriptorTables.size(); ++rootParameterIndex)
				{
					const auto& table = m_boundDescriptorTables[rootParameterIndex];
					if (table.set != boundSetIndex)
						continue;

					const auto gpuHandle = boundNativeBindingSet->GetGPUHandle(table.set, table.heapKind);
					if (gpuHandle.ptr == 0)
						continue;

					m_commandList->SetGraphicsRootDescriptorTable(rootParameterIndex, gpuHandle);
				}
			}
#endif
		}

		class NativeDX12Sampler final : public NLS::Render::RHI::RHISampler
		{
		public:
			NativeDX12Sampler(ID3D12Device*, const NLS::Render::RHI::SamplerDesc& desc, const std::string& debugName)
				: m_desc(desc)
				, m_debugName(debugName)
			{
			}

			std::string_view GetDebugName() const override { return m_debugName; }
			const NLS::Render::RHI::SamplerDesc& GetDesc() const override { return m_desc; }
			NLS::Render::RHI::NativeHandle GetNativeSamplerHandle() override { return { NLS::Render::RHI::BackendType::DX12, nullptr }; }

		private:
			NLS::Render::RHI::SamplerDesc m_desc{};
			std::string m_debugName;
		};

		class NativeDX12BindingLayout final : public NLS::Render::RHI::RHIBindingLayout
		{
		public:
			explicit NativeDX12BindingLayout(NLS::Render::RHI::RHIBindingLayoutDesc desc)
				: m_desc(std::move(desc))
			{
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHIBindingLayoutDesc& GetDesc() const override { return m_desc; }

		private:
			NLS::Render::RHI::RHIBindingLayoutDesc m_desc;
		};

		class NativeDX12PipelineLayout final : public NLS::Render::RHI::RHIPipelineLayout
		{
		public:
			explicit NativeDX12PipelineLayout(ID3D12Device* device, NLS::Render::RHI::RHIPipelineLayoutDesc desc);
			~NativeDX12PipelineLayout();

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHIPipelineLayoutDesc& GetDesc() const override { return m_desc; }
			ID3D12RootSignature* GetRootSignature() const { return m_rootSignature.Get(); }
			const std::vector<NLS::Render::RHI::DX12::DX12DescriptorTableDesc>& GetDescriptorTables() const { return m_descriptorTables; }

		private:
			ID3D12Device* m_device = nullptr;
			NLS::Render::RHI::RHIPipelineLayoutDesc m_desc;
			ComPtr<ID3D12RootSignature> m_rootSignature;
			std::vector<NLS::Render::RHI::DX12::DX12DescriptorTableDesc> m_descriptorTables;
		};

		// NativeDX12PipelineLayout implementation
		NativeDX12PipelineLayout::NativeDX12PipelineLayout(ID3D12Device* device, NLS::Render::RHI::RHIPipelineLayoutDesc desc)
			: m_device(device)
			, m_desc(std::move(desc))
		{
			if (m_device != nullptr && !m_desc.bindingLayouts.empty())
			{
				const auto ownedRootParameters = NLS::Render::RHI::DX12::BuildDX12OwnedRootParameters(m_desc);
				m_descriptorTables = ownedRootParameters.descriptorTables;
				if (ownedRootParameters.rootParameters.empty())
					return;

				D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
				rootSigDesc.NumParameters = static_cast<UINT>(ownedRootParameters.rootParameters.size());
				rootSigDesc.pParameters = ownedRootParameters.rootParameters.data();
				rootSigDesc.NumStaticSamplers = 0;
				rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

				ID3DBlob* signatureBlob = nullptr;
				ID3DBlob* errorBlob = nullptr;
				HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &errorBlob);
				if (FAILED(hr))
				{
					if (errorBlob != nullptr)
					{
						NLS_LOG_ERROR("NativeDX12PipelineLayout: D3D12SerializeRootSignature failed: " + std::string(static_cast<char*>(errorBlob->GetBufferPointer())));
						errorBlob->Release();
					}
					return;
				}

				hr = m_device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature));
				signatureBlob->Release();
				if (FAILED(hr))
				{
					NLS_LOG_ERROR("NativeDX12PipelineLayout: CreateRootSignature failed with hr=" + std::to_string(hr));
					return;
				}
			}
		}

		NativeDX12PipelineLayout::~NativeDX12PipelineLayout()
		{
		}

		class NativeDX12ShaderModule final : public NLS::Render::RHI::RHIShaderModule
		{
		public:
			explicit NativeDX12ShaderModule(NLS::Render::RHI::RHIShaderModuleDesc desc)
				: m_desc(std::move(desc))
			{
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHIShaderModuleDesc& GetDesc() const override { return m_desc; }

		private:
			NLS::Render::RHI::RHIShaderModuleDesc m_desc;
		};

		class NativeDX12GraphicsPipeline final : public NLS::Render::RHI::RHIGraphicsPipeline
		{
		public:
			explicit NativeDX12GraphicsPipeline(ID3D12Device* device, NLS::Render::RHI::RHIGraphicsPipelineDesc desc)
				: m_device(device)
				, m_desc(std::move(desc))
			{
#if defined(_WIN32)
				if (m_device == nullptr)
					return;

				// Try to use the pipeline layout's root signature first
				bool usedLayoutRootSignature = false;
				if (m_desc.pipelineLayout != nullptr)
				{
					auto* nativeLayout = dynamic_cast<NativeDX12PipelineLayout*>(m_desc.pipelineLayout.get());
					if (nativeLayout != nullptr)
					{
						ID3D12RootSignature* layoutRootSig = nativeLayout->GetRootSignature();
						if (layoutRootSig != nullptr)
						{
							m_rootSignature = layoutRootSig;
							layoutRootSig->AddRef();
							usedLayoutRootSignature = true;
						}
					}
				}

				// If no pipeline layout or no root signature from it, create default
				if (!usedLayoutRootSignature)
				{
					CreateDefaultRootSignature();
				}

				// Create pipeline state
				if (m_rootSignature != nullptr)
				{
					CreatePipelineState();
				}
#endif
			}

			~NativeDX12GraphicsPipeline()
			{
#if defined(_WIN32)
				if (m_pipelineState != nullptr)
					m_pipelineState->Release();
				if (m_rootSignature != nullptr)
					m_rootSignature->Release();
#endif
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHIGraphicsPipelineDesc& GetDesc() const override { return m_desc; }
			ID3D12PipelineState* GetPipelineState() const { return m_pipelineState; }
			ID3D12RootSignature* GetRootSignature() const { return m_rootSignature; }

		private:
			void CreateDefaultRootSignature()
			{
#if defined(_WIN32)
				if (m_device == nullptr)
					return;

				// Create a root signature with one descriptor table containing one range
				// The range covers 1 CBV/SRV/UAV descriptor
				D3D12_DESCRIPTOR_RANGE range = {};
				range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV; // Will be updated based on actual usage
				range.NumDescriptors = 1;
				range.BaseShaderRegister = 0;
				range.RegisterSpace = 0;
				range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

				D3D12_ROOT_PARAMETER rootParam = {};
				rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
				rootParam.DescriptorTable.NumDescriptorRanges = 1;
				rootParam.DescriptorTable.pDescriptorRanges = &range;
				rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

				D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
				rootSigDesc.NumParameters = 1;
				rootSigDesc.pParameters = &rootParam;
				rootSigDesc.NumStaticSamplers = 0;
				rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

				ComPtr<ID3DBlob> signature;
				ComPtr<ID3DBlob> error;
				HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
				if (FAILED(hr))
				{
					if (error != nullptr)
					{
						NLS_LOG_ERROR("NativeDX12GraphicsPipeline: D3D12SerializeRootSignature failed: " + std::string(static_cast<char*>(error->GetBufferPointer())));
					}
					return;
				}

				hr = m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature));
				if (FAILED(hr))
				{
					NLS_LOG_ERROR("NativeDX12GraphicsPipeline: CreateRootSignature failed with hr=" + std::to_string(hr));
					return;
				}
#endif
			}

			void CreatePipelineState()
			{
#if defined(_WIN32)
				if (m_device == nullptr || m_rootSignature == nullptr)
					return;

				D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
				psoDesc.pRootSignature = m_rootSignature;
				const auto inputLayout = NLS::Render::RHI::DX12::BuildDX12OwnedInputLayout(m_desc);

				// Set shader bytecode from the shader modules
				if (m_desc.vertexShader)
				{
					const auto& vsDesc = m_desc.vertexShader->GetDesc();
					if (!vsDesc.bytecode.empty())
					{
						psoDesc.VS.pShaderBytecode = vsDesc.bytecode.data();
						psoDesc.VS.BytecodeLength = vsDesc.bytecode.size();
					}
				}
				if (m_desc.fragmentShader)
				{
					const auto& fsDesc = m_desc.fragmentShader->GetDesc();
					if (!fsDesc.bytecode.empty())
					{
						psoDesc.PS.pShaderBytecode = fsDesc.bytecode.data();
						psoDesc.PS.BytecodeLength = fsDesc.bytecode.size();
					}
				}

				// Other shader stages - not used currently
				psoDesc.DS.pShaderBytecode = nullptr;
				psoDesc.DS.BytecodeLength = 0;
				psoDesc.HS.pShaderBytecode = nullptr;
				psoDesc.HS.BytecodeLength = 0;
				psoDesc.GS.pShaderBytecode = nullptr;
				psoDesc.GS.BytecodeLength = 0;
				psoDesc.StreamOutput.pSODeclaration = nullptr;
				psoDesc.StreamOutput.NumEntries = 0;
				psoDesc.StreamOutput.pBufferStrides = nullptr;
				psoDesc.StreamOutput.NumStrides = 0;
				psoDesc.StreamOutput.RasterizedStream = 0;

				// Blend state
				psoDesc.BlendState.AlphaToCoverageEnable = FALSE;
				psoDesc.BlendState.IndependentBlendEnable = FALSE;
				psoDesc.BlendState.RenderTarget[0].BlendEnable = m_desc.blendState.enabled ? TRUE : FALSE;
				psoDesc.BlendState.RenderTarget[0].LogicOpEnable = FALSE;
				psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
				psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
				psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
				psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
				psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
				psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
				psoDesc.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
				psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask =
					m_desc.blendState.colorWrite ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;

				// Sample mask
				psoDesc.SampleMask = UINT_MAX;

				// Rasterizer state
				psoDesc.RasterizerState.FillMode = m_desc.rasterState.wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
				psoDesc.RasterizerState.CullMode = m_desc.rasterState.cullEnabled ?
					(m_desc.rasterState.cullFace == NLS::Render::Settings::ECullFace::FRONT ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK) :
					D3D12_CULL_MODE_NONE;
				psoDesc.RasterizerState.FrontCounterClockwise = TRUE;
				psoDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
				psoDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
				psoDesc.RasterizerState.DepthClipEnable = TRUE;
				psoDesc.RasterizerState.MultisampleEnable = FALSE;
				psoDesc.RasterizerState.AntialiasedLineEnable = FALSE;
				psoDesc.RasterizerState.ForcedSampleCount = 0;
				psoDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

				// Depth stencil state
				psoDesc.DepthStencilState.DepthEnable = m_desc.depthStencilState.depthTest;
				psoDesc.DepthStencilState.DepthWriteMask = m_desc.depthStencilState.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
				psoDesc.DepthStencilState.DepthFunc = ToD3D12ComparisonFunc(m_desc.depthStencilState.depthCompare);
				psoDesc.DepthStencilState.StencilEnable = m_desc.depthStencilState.stencilTest ? TRUE : FALSE;
				psoDesc.DepthStencilState.StencilReadMask = static_cast<UINT8>(m_desc.depthStencilState.stencilReadMask);
				psoDesc.DepthStencilState.StencilWriteMask = static_cast<UINT8>(m_desc.depthStencilState.stencilWriteMask);
				psoDesc.DepthStencilState.FrontFace.StencilFunc = ToD3D12ComparisonFunc(m_desc.depthStencilState.stencilCompare);
				psoDesc.DepthStencilState.FrontFace.StencilDepthFailOp = ToD3D12StencilOp(m_desc.depthStencilState.stencilDepthFailOp);
				psoDesc.DepthStencilState.FrontFace.StencilPassOp = ToD3D12StencilOp(m_desc.depthStencilState.stencilPassOp);
				psoDesc.DepthStencilState.FrontFace.StencilFailOp = ToD3D12StencilOp(m_desc.depthStencilState.stencilFailOp);
				psoDesc.DepthStencilState.BackFace = psoDesc.DepthStencilState.FrontFace;

				// Input layout
				psoDesc.InputLayout.pInputElementDescs = inputLayout.elements.empty() ? nullptr : inputLayout.elements.data();
				psoDesc.InputLayout.NumElements = static_cast<UINT>(inputLayout.elements.size());

				// Primitive topology
				D3D12_PRIMITIVE_TOPOLOGY_TYPE topoType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
				switch (m_desc.primitiveTopology)
				{
				case NLS::Render::RHI::PrimitiveTopology::PointList: topoType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT; break;
				case NLS::Render::RHI::PrimitiveTopology::LineList: topoType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; break;
				case NLS::Render::RHI::PrimitiveTopology::TriangleList: topoType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; break;
				}
				psoDesc.PrimitiveTopologyType = topoType;
				psoDesc.NumRenderTargets = 1;
				psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
				psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
				psoDesc.SampleDesc.Count = 1;
				psoDesc.SampleDesc.Quality = 0;
				psoDesc.NodeMask = 0;
				psoDesc.CachedPSO.CachedBlobSizeInBytes = 0;
				psoDesc.CachedPSO.pCachedBlob = nullptr;
				psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

				HRESULT hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState));
				if (FAILED(hr))
				{
					NLS_LOG_ERROR("NativeDX12GraphicsPipeline: CreateGraphicsPipelineState failed with hr=" + std::to_string(hr));
					LogDx12DebugMessages(m_device, "NativeDX12GraphicsPipeline");
					return;
				}
#endif
			}

			ID3D12Device* m_device = nullptr;
			NLS::Render::RHI::RHIGraphicsPipelineDesc m_desc;
#if defined(_WIN32)
			ID3D12RootSignature* m_rootSignature = nullptr;
			ID3D12PipelineState* m_pipelineState = nullptr;
#endif
		};

		// BindGraphicsPipeline implementation - defined after NativeDX12GraphicsPipeline so we can use dynamic_cast
		void NativeDX12CommandBuffer::BindGraphicsPipeline(const std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>& pipeline)
		{
#if defined(_WIN32)
			if (m_commandList == nullptr || pipeline == nullptr)
				return;

			auto* nativePipeline = dynamic_cast<NativeDX12GraphicsPipeline*>(pipeline.get());
			if (nativePipeline == nullptr)
				return;

			ID3D12RootSignature* rootSig = nativePipeline->GetRootSignature();
			ID3D12PipelineState* pso = nativePipeline->GetPipelineState();

			if (rootSig != nullptr)
				m_commandList->SetGraphicsRootSignature(rootSig);

			if (pso != nullptr)
				m_commandList->SetPipelineState(pso);

			D3D12_PRIMITIVE_TOPOLOGY primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			switch (pipeline->GetDesc().primitiveTopology)
			{
			case NLS::Render::RHI::PrimitiveTopology::PointList:
				primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
				break;
			case NLS::Render::RHI::PrimitiveTopology::LineList:
				primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
				break;
			case NLS::Render::RHI::PrimitiveTopology::TriangleList:
			default:
				primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
				break;
			}
			m_commandList->IASetPrimitiveTopology(primitiveTopology);
			if (pipeline->GetDesc().depthStencilState.stencilTest)
				m_commandList->OMSetStencilRef(pipeline->GetDesc().depthStencilState.stencilReference);

			m_boundDescriptorTables.clear();
			if (pipeline->GetDesc().pipelineLayout != nullptr)
			{
				auto* nativePipelineLayout = dynamic_cast<NativeDX12PipelineLayout*>(pipeline->GetDesc().pipelineLayout.get());
				if (nativePipelineLayout != nullptr)
					m_boundDescriptorTables = nativePipelineLayout->GetDescriptorTables();
			}

			m_boundPipeline = pipeline;
			m_recordedPipelineKeepAlive.push_back(pipeline);
#endif
		}

		class NativeDX12ComputePipeline final : public NLS::Render::RHI::RHIComputePipeline
		{
		public:
			explicit NativeDX12ComputePipeline(NLS::Render::RHI::RHIComputePipelineDesc desc)
				: m_desc(std::move(desc))
			{
			}

			std::string_view GetDebugName() const override { return m_desc.debugName; }
			const NLS::Render::RHI::RHIComputePipelineDesc& GetDesc() const override { return m_desc; }

		private:
			NLS::Render::RHI::RHIComputePipelineDesc m_desc;
		};

		class NativeDX12ExplicitDevice final : public NLS::Render::RHI::RHIDevice
		{
		public:
			NativeDX12ExplicitDevice(
				ID3D12Device* device,
				ID3D12CommandQueue* graphicsQueue,
				IDXGIFactory6* factory,
				IDXGIAdapter1* adapter,
				const NLS::Render::RHI::RHIDeviceCapabilities& capabilities,
				const std::string& vendor,
				const std::string& hardware)
				: m_device(device)
				, m_graphicsQueue(graphicsQueue)
				, m_factory(factory)
				, m_adapter(adapter)
				, m_capabilities(capabilities)
				, m_rhiAdapter(std::make_shared<NativeDX12Adapter>(vendor, hardware))
				, m_resourceHeapAllocator(std::make_unique<DX12ShaderVisibleDescriptorHeapAllocator>(
					device,
					graphicsQueue,
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
					512,
					"DX12ResourceHeapAllocator"))
				, m_samplerHeapAllocator(std::make_unique<DX12ShaderVisibleDescriptorHeapAllocator>(
					device,
					graphicsQueue,
					D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
					128,
					"DX12SamplerHeapAllocator"))
			{
			}

			~NativeDX12ExplicitDevice()
			{
				m_samplerHeapAllocator.reset();
				m_resourceHeapAllocator.reset();
			}

			std::string_view GetDebugName() const override { return "NativeDX12ExplicitDevice"; }
			const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_rhiAdapter; }
			const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
			NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override
			{
				NLS::Render::RHI::NativeRenderDeviceInfo info{};
				info.backend = NLS::Render::RHI::NativeBackendType::DX12;
#if defined(_WIN32)
				info.device = m_device.Get();
				info.graphicsQueue = m_graphicsQueue.Get();
				info.swapchain = m_swapchain.Get();
				info.nativeWindowHandle = m_nativeWindowHandle;
#endif
				return info;
			}
			bool IsBackendReady() const override { return m_device != nullptr; }

			std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType queueType) override
			{
				const auto queueIndex = static_cast<size_t>(queueType);
				if (m_queues[queueIndex] == nullptr)
					m_queues[queueIndex] = std::make_shared<NativeDX12Queue>(m_device.Get(), m_graphicsQueue.Get(), "GraphicsQueue");
				return m_queues[queueIndex];
			}

			std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(const NLS::Render::RHI::SwapchainDesc& desc) override
			{
#if defined(_WIN32)
				if (m_factory == nullptr || m_device == nullptr || desc.nativeWindowHandle == nullptr)
				{
					NLS_LOG_ERROR("NativeDX12ExplicitDevice::CreateSwapchain failed: factory=" + std::to_string(reinterpret_cast<uintptr_t>(m_factory.Get())) + " device=" + std::to_string(reinterpret_cast<uintptr_t>(m_device.Get())) + " window=" + std::to_string(reinterpret_cast<uintptr_t>(desc.nativeWindowHandle)));
					return nullptr;
				}

				m_nativeWindowHandle = desc.nativeWindowHandle;

				ComPtr<IDXGISwapChain1> swapChain1;
				DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
				swapChainDesc.Width = desc.width;
				swapChainDesc.Height = desc.height;
				swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				swapChainDesc.SampleDesc.Count = 1;
				swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
				swapChainDesc.BufferCount = desc.imageCount > 0 ? desc.imageCount : 2;
				swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
				swapChainDesc.Scaling = DXGI_SCALING_NONE;

			 HRESULT hr = m_factory->CreateSwapChainForHwnd(
					m_graphicsQueue.Get(),
					static_cast<HWND>(desc.nativeWindowHandle),
					&swapChainDesc,
					nullptr,
					nullptr,
					&swapChain1);

				if (FAILED(hr) && swapChainDesc.Scaling == DXGI_SCALING_NONE)
				{
					swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
					hr = m_factory->CreateSwapChainForHwnd(
						m_graphicsQueue.Get(),
						static_cast<HWND>(desc.nativeWindowHandle),
						&swapChainDesc,
						nullptr,
						nullptr,
						&swapChain1);
				}

				if (FAILED(hr))
				{
					NLS_LOG_ERROR("NativeDX12ExplicitDevice::CreateSwapchain: CreateSwapChainForHwnd failed with hr=" + std::to_string(hr));
					return nullptr;
				}

				ComPtr<IDXGISwapChain3> swapChain3;
				swapChain1.As(&swapChain3);
				m_swapchain = swapChain3;
				return std::make_shared<NativeDX12Swapchain>(swapChain3, m_device.Get(), desc);
#else
				return nullptr;
#endif
			}

			std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(const NLS::Render::RHI::RHIBufferDesc& desc, const void* initialData) override;
			std::shared_ptr<NLS::Render::RHI::RHITexture> CreateTexture(const NLS::Render::RHI::RHITextureDesc& desc, const void* initialData) override;
			std::shared_ptr<NLS::Render::RHI::RHITextureView> CreateTextureView(const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture, const NLS::Render::RHI::RHITextureViewDesc& desc) override;
			std::shared_ptr<NLS::Render::RHI::RHISampler> CreateSampler(const NLS::Render::RHI::SamplerDesc& desc, std::string debugName) override;
			std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateBindingLayout(const NLS::Render::RHI::RHIBindingLayoutDesc& desc) override;
			std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateBindingSet(const NLS::Render::RHI::RHIBindingSetDesc& desc) override;
			std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> CreatePipelineLayout(const NLS::Render::RHI::RHIPipelineLayoutDesc& desc) override;
			std::shared_ptr<NLS::Render::RHI::RHIShaderModule> CreateShaderModule(const NLS::Render::RHI::RHIShaderModuleDesc& desc) override;
			std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> CreateGraphicsPipeline(const NLS::Render::RHI::RHIGraphicsPipelineDesc& desc) override;
			std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> CreateComputePipeline(const NLS::Render::RHI::RHIComputePipelineDesc& desc) override;
			std::shared_ptr<NLS::Render::RHI::RHICommandPool> CreateCommandPool(NLS::Render::RHI::QueueType queueType, std::string debugName) override
			{
#if defined(_WIN32)
				return std::make_shared<NativeDX12CommandPool>(m_device.Get(), m_graphicsQueue.Get(), queueType, debugName.empty() ? "CommandPool" : debugName);
#else
				return nullptr;
#endif
			}
			std::shared_ptr<NLS::Render::RHI::RHIFence> CreateFence(std::string debugName) override
			{
#if defined(_WIN32)
				return std::make_shared<NativeDX12Fence>(m_device.Get(), debugName.empty() ? "Fence" : debugName);
#else
				return nullptr;
#endif
			}
			std::shared_ptr<NLS::Render::RHI::RHISemaphore> CreateSemaphore(std::string debugName) override
			{
#if defined(_WIN32)
				return std::make_shared<NativeDX12Semaphore>(m_device.Get(), debugName.empty() ? "Semaphore" : debugName);
#else
				return nullptr;
#endif
			}

			// Readback support
			void ReadPixels(
			    const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
			    uint32_t x,
			    uint32_t y,
			    uint32_t width,
			    uint32_t height,
			    NLS::Render::Settings::EPixelDataFormat format,
			    NLS::Render::Settings::EPixelDataType type,
			    void* data) override;

		private:
			Microsoft::WRL::ComPtr<ID3D12Device> m_device;
			Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_graphicsQueue;
			Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
			Microsoft::WRL::ComPtr<IDXGIAdapter1> m_adapter;
			Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapchain;
			void* m_nativeWindowHandle = nullptr;
			NLS::Render::RHI::RHIDeviceCapabilities m_capabilities{};
			std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_rhiAdapter;
			std::array<std::shared_ptr<NLS::Render::RHI::RHIQueue>, 3> m_queues{};
			std::unique_ptr<DX12ShaderVisibleDescriptorHeapAllocator> m_resourceHeapAllocator;
			std::unique_ptr<DX12ShaderVisibleDescriptorHeapAllocator> m_samplerHeapAllocator;
		};

#if defined(_WIN32)
		namespace
		{
			NLS::Render::RHI::ResourceState ResolveUploadedTextureState(const NLS::Render::RHI::RHITextureDesc& desc)
			{
				using NLS::Render::RHI::ResourceState;
				using NLS::Render::RHI::TextureUsageFlags;

				if (NLS::Render::RHI::HasTextureUsage(desc.usage, TextureUsageFlags::Sampled))
					return ResourceState::ShaderRead;
				if (NLS::Render::RHI::HasTextureUsage(desc.usage, TextureUsageFlags::Storage))
					return ResourceState::ShaderWrite;
				if (NLS::Render::RHI::HasTextureUsage(desc.usage, TextureUsageFlags::ColorAttachment))
					return ResourceState::RenderTarget;
				if (NLS::Render::RHI::HasTextureUsage(desc.usage, TextureUsageFlags::DepthStencilAttachment))
					return ResourceState::DepthWrite;
				if (NLS::Render::RHI::HasTextureUsage(desc.usage, TextureUsageFlags::CopySrc))
					return ResourceState::CopySrc;
				if (NLS::Render::RHI::HasTextureUsage(desc.usage, TextureUsageFlags::CopyDst))
					return ResourceState::CopyDst;
				if (NLS::Render::RHI::HasTextureUsage(desc.usage, TextureUsageFlags::Present))
					return ResourceState::Present;
				return ResourceState::Unknown;
			}

			bool UploadInitialTextureData(
				ID3D12Device* device,
				ID3D12CommandQueue* graphicsQueue,
				ID3D12Resource* textureResource,
				const NLS::Render::RHI::RHITextureDesc& desc,
				const void* initialData,
				const std::string& debugName,
				NLS::Render::RHI::ResourceState& outFinalState)
			{
				if (device == nullptr || graphicsQueue == nullptr || textureResource == nullptr || initialData == nullptr)
					return false;

				const auto uploadPlan = NLS::Render::RHI::DX12::BuildDX12TextureUploadPlan(desc);
				if (uploadPlan.subresources.empty() || uploadPlan.totalBytes == 0)
				{
					NLS_LOG_ERROR("UploadInitialTextureData: no upload plan for texture \"" + debugName + "\"");
					return false;
				}

				const UINT subresourceCount = static_cast<UINT>(uploadPlan.subresources.size());
				std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(subresourceCount);
				std::vector<UINT> rowCounts(subresourceCount);
				std::vector<UINT64> rowSizes(subresourceCount);
				UINT64 uploadBufferSize = 0;

				const D3D12_RESOURCE_DESC textureDesc = textureResource->GetDesc();
				device->GetCopyableFootprints(
					&textureDesc,
					0,
					subresourceCount,
					0,
					layouts.data(),
					rowCounts.data(),
					rowSizes.data(),
					&uploadBufferSize);

				if (uploadBufferSize == 0)
				{
					NLS_LOG_ERROR("UploadInitialTextureData: GetCopyableFootprints returned zero upload size for texture \"" + debugName + "\"");
					return false;
				}

				D3D12_HEAP_PROPERTIES uploadHeapProperties{};
				uploadHeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
				uploadHeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
				uploadHeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
				uploadHeapProperties.CreationNodeMask = 1;
				uploadHeapProperties.VisibleNodeMask = 1;

				D3D12_RESOURCE_DESC uploadBufferDesc{};
				uploadBufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				uploadBufferDesc.Width = uploadBufferSize;
				uploadBufferDesc.Height = 1;
				uploadBufferDesc.DepthOrArraySize = 1;
				uploadBufferDesc.MipLevels = 1;
				uploadBufferDesc.Format = DXGI_FORMAT_UNKNOWN;
				uploadBufferDesc.SampleDesc.Count = 1;
				uploadBufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				uploadBufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

				ComPtr<ID3D12Resource> uploadBuffer;
				HRESULT hr = device->CreateCommittedResource(
					&uploadHeapProperties,
					D3D12_HEAP_FLAG_NONE,
					&uploadBufferDesc,
					D3D12_RESOURCE_STATE_GENERIC_READ,
					nullptr,
					IID_PPV_ARGS(&uploadBuffer));
				if (FAILED(hr))
				{
					NLS_LOG_ERROR("UploadInitialTextureData: failed to create upload buffer for texture \"" + debugName + "\" hr=" + std::to_string(hr));
					return false;
				}

				SetDx12ObjectName(uploadBuffer.Get(), debugName + "UploadBuffer");

				auto* uploadBase = static_cast<uint8_t*>(nullptr);
				D3D12_RANGE readRange{};
				hr = uploadBuffer->Map(0, &readRange, reinterpret_cast<void**>(&uploadBase));
				if (FAILED(hr) || uploadBase == nullptr)
				{
					NLS_LOG_ERROR("UploadInitialTextureData: failed to map upload buffer for texture \"" + debugName + "\" hr=" + std::to_string(hr));
					return false;
				}

				const auto* sourceBytes = static_cast<const uint8_t*>(initialData);
				for (UINT subresourceIndex = 0; subresourceIndex < subresourceCount; ++subresourceIndex)
				{
					const auto& subresource = uploadPlan.subresources[subresourceIndex];
					const auto& layout = layouts[subresourceIndex];
					const size_t dstRowPitch = static_cast<size_t>(layout.Footprint.RowPitch);
					const size_t rowByteCount = static_cast<size_t>(rowSizes[subresourceIndex]);
					const size_t dstSlicePitch = dstRowPitch * static_cast<size_t>(rowCounts[subresourceIndex]);
					const auto* srcSubresource = sourceBytes + subresource.dataOffset;
					auto* dstSubresource = uploadBase + static_cast<size_t>(layout.Offset);

					for (uint32_t depthSlice = 0; depthSlice < subresource.depth; ++depthSlice)
					{
						const auto* srcSlice = srcSubresource + static_cast<size_t>(depthSlice) * subresource.slicePitch;
						auto* dstSlice = dstSubresource + static_cast<size_t>(depthSlice) * dstSlicePitch;
						for (UINT row = 0; row < rowCounts[subresourceIndex]; ++row)
						{
							std::memcpy(
								dstSlice + static_cast<size_t>(row) * dstRowPitch,
								srcSlice + static_cast<size_t>(row) * subresource.rowPitch,
								rowByteCount);
						}
					}
				}

				uploadBuffer->Unmap(0, nullptr);

				ComPtr<ID3D12CommandAllocator> commandAllocator;
				hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator));
				if (FAILED(hr))
				{
					NLS_LOG_ERROR("UploadInitialTextureData: failed to create command allocator for texture \"" + debugName + "\" hr=" + std::to_string(hr));
					return false;
				}

				SetDx12ObjectName(commandAllocator.Get(), debugName + "UploadAllocator");

				ComPtr<ID3D12GraphicsCommandList> commandList;
				hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.Get(), nullptr, IID_PPV_ARGS(&commandList));
				if (FAILED(hr))
				{
					NLS_LOG_ERROR("UploadInitialTextureData: failed to create command list for texture \"" + debugName + "\" hr=" + std::to_string(hr));
					return false;
				}

				SetDx12ObjectName(commandList.Get(), debugName + "UploadCommandList");

				for (UINT subresourceIndex = 0; subresourceIndex < subresourceCount; ++subresourceIndex)
				{
					D3D12_TEXTURE_COPY_LOCATION srcLocation{};
					srcLocation.pResource = uploadBuffer.Get();
					srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
					srcLocation.PlacedFootprint = layouts[subresourceIndex];

					D3D12_TEXTURE_COPY_LOCATION dstLocation{};
					dstLocation.pResource = textureResource;
					dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					dstLocation.SubresourceIndex = subresourceIndex;

					commandList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
				}

				outFinalState = ResolveUploadedTextureState(desc);
				const D3D12_RESOURCE_STATES finalDxState =
					outFinalState == NLS::Render::RHI::ResourceState::Unknown
					? D3D12_RESOURCE_STATE_COMMON
					: NativeDX12CommandBuffer::ToD3D12ResourceState(outFinalState);

				if (finalDxState != D3D12_RESOURCE_STATE_COPY_DEST)
				{
					D3D12_RESOURCE_BARRIER barrier{};
					barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
					barrier.Transition.pResource = textureResource;
					barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
					barrier.Transition.StateAfter = finalDxState;
					barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
					commandList->ResourceBarrier(1, &barrier);
				}

				hr = commandList->Close();
				if (FAILED(hr))
				{
					NLS_LOG_ERROR("UploadInitialTextureData: failed to close command list for texture \"" + debugName + "\" hr=" + std::to_string(hr));
					return false;
				}

				ID3D12CommandList* commandLists[] = { commandList.Get() };
				graphicsQueue->ExecuteCommandLists(1, commandLists);

				ComPtr<ID3D12Fence> fence;
				hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
				if (FAILED(hr))
				{
					NLS_LOG_ERROR("UploadInitialTextureData: failed to create fence for texture \"" + debugName + "\" hr=" + std::to_string(hr));
					return false;
				}

				SetDx12ObjectName(fence.Get(), debugName + "UploadFence");

				HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
				if (fenceEvent == nullptr)
				{
					NLS_LOG_ERROR("UploadInitialTextureData: failed to create fence event for texture \"" + debugName + "\"");
					return false;
				}

				const UINT64 fenceValue = 1;
				hr = fence->SetEventOnCompletion(fenceValue, fenceEvent);
				if (FAILED(hr))
				{
					CloseHandle(fenceEvent);
					NLS_LOG_ERROR("UploadInitialTextureData: failed to set fence completion event for texture \"" + debugName + "\" hr=" + std::to_string(hr));
					return false;
				}

				hr = graphicsQueue->Signal(fence.Get(), fenceValue);
				if (FAILED(hr))
				{
					CloseHandle(fenceEvent);
					NLS_LOG_ERROR("UploadInitialTextureData: failed to signal fence for texture \"" + debugName + "\" hr=" + std::to_string(hr));
					return false;
				}

				WaitForSingleObject(fenceEvent, INFINITE);
				CloseHandle(fenceEvent);
				return true;
			}
		}
#endif

		// NativeDX12ExplicitDevice method implementations
		std::shared_ptr<NLS::Render::RHI::RHIBuffer> NativeDX12ExplicitDevice::CreateBuffer(const NLS::Render::RHI::RHIBufferDesc& desc, const void* initialData)
		{
			if (m_device == nullptr)
				return nullptr;
			return std::make_shared<NativeDX12Buffer>(m_device.Get(), m_graphicsQueue.Get(), desc, initialData);
		}

		std::shared_ptr<NLS::Render::RHI::RHITexture> NativeDX12ExplicitDevice::CreateTexture(const NLS::Render::RHI::RHITextureDesc& desc, const void* initialData)
		{
#if defined(_WIN32)
			if (m_device == nullptr)
				return nullptr;

			auto texture = std::make_shared<NativeDX12Texture>(m_device.Get(), desc, initialData);
			if (texture == nullptr)
				return nullptr;

			const bool needsInitialUpload =
				initialData != nullptr &&
				desc.format != NLS::Render::RHI::TextureFormat::Depth24Stencil8;

			if (needsInitialUpload)
			{
				auto* resource = texture->GetResource();
				if (resource == nullptr)
				{
					NLS_LOG_ERROR("NativeDX12ExplicitDevice::CreateTexture: texture resource creation failed for \"" + desc.debugName + "\"");
					return nullptr;
				}

				NLS::Render::RHI::ResourceState finalState = NLS::Render::RHI::ResourceState::Unknown;
				const std::string textureName = desc.debugName.empty() ? "TextureResource" : desc.debugName;
				if (!UploadInitialTextureData(
					m_device.Get(),
					m_graphicsQueue.Get(),
					resource,
					desc,
					initialData,
					textureName,
					finalState))
				{
					NLS_LOG_ERROR("NativeDX12ExplicitDevice::CreateTexture: initial upload failed for \"" + textureName + "\"");
					return nullptr;
				}

				texture->SetState(finalState);
			}

			return texture;
#else
			return nullptr;
#endif
		}

		std::shared_ptr<NLS::Render::RHI::RHITextureView> NativeDX12ExplicitDevice::CreateTextureView(const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture, const NLS::Render::RHI::RHITextureViewDesc& desc)
		{
			if (texture == nullptr)
				return nullptr;
			return std::make_shared<NativeDX12TextureView>(m_device.Get(), texture, desc);
		}

		std::shared_ptr<NLS::Render::RHI::RHISampler> NativeDX12ExplicitDevice::CreateSampler(const NLS::Render::RHI::SamplerDesc& desc, std::string debugName)
		{
#if defined(_WIN32)
			if (m_device == nullptr)
				return nullptr;
			return std::make_shared<NativeDX12Sampler>(m_device.Get(), desc, debugName.empty() ? "Sampler" : debugName);
#else
			return nullptr;
#endif
		}

		std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> NativeDX12ExplicitDevice::CreateBindingLayout(const NLS::Render::RHI::RHIBindingLayoutDesc& desc)
		{
			return std::make_shared<NativeDX12BindingLayout>(desc);
		}

		std::shared_ptr<NLS::Render::RHI::RHIBindingSet> NativeDX12ExplicitDevice::CreateBindingSet(const NLS::Render::RHI::RHIBindingSetDesc& desc)
		{
#if defined(_WIN32)
			return std::make_shared<NativeDX12BindingSet>(
				m_device.Get(),
				desc,
				m_resourceHeapAllocator.get(),
				m_samplerHeapAllocator.get());
#else
			return nullptr;
#endif
		}

		std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> NativeDX12ExplicitDevice::CreatePipelineLayout(const NLS::Render::RHI::RHIPipelineLayoutDesc& desc)
		{
			return std::make_shared<NativeDX12PipelineLayout>(m_device.Get(), desc);
		}

		std::shared_ptr<NLS::Render::RHI::RHIShaderModule> NativeDX12ExplicitDevice::CreateShaderModule(const NLS::Render::RHI::RHIShaderModuleDesc& desc)
		{
			return std::make_shared<NativeDX12ShaderModule>(desc);
		}

		std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> NativeDX12ExplicitDevice::CreateGraphicsPipeline(const NLS::Render::RHI::RHIGraphicsPipelineDesc& desc)
		{
			return std::make_shared<NativeDX12GraphicsPipeline>(m_device.Get(), desc);
		}

		std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> NativeDX12ExplicitDevice::CreateComputePipeline(const NLS::Render::RHI::RHIComputePipelineDesc& desc)
		{
			return std::make_shared<NativeDX12ComputePipeline>(desc);
		}

		void NativeDX12ExplicitDevice::ReadPixels(
		    const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
		    uint32_t x,
		    uint32_t y,
		    uint32_t width,
		    uint32_t height,
		    NLS::Render::Settings::EPixelDataFormat format,
		    NLS::Render::Settings::EPixelDataType type,
		    void* data)
		{
#if defined(_WIN32)
			if (texture == nullptr || data == nullptr || width == 0 || height == 0 || m_device == nullptr || m_graphicsQueue == nullptr)
				return;

			auto imgHandle = texture->GetNativeImageHandle();
			ID3D12Resource* srcResource = (imgHandle.backend == NLS::Render::RHI::BackendType::DX12) ? static_cast<ID3D12Resource*>(imgHandle.handle) : nullptr;
			if (srcResource == nullptr)
				return;

			D3D12_RESOURCE_DESC srcDesc = srcResource->GetDesc();
			const uint64_t maxX = static_cast<uint64_t>(x) + static_cast<uint64_t>(width);
			const uint64_t maxY = static_cast<uint64_t>(y) + static_cast<uint64_t>(height);
			if (maxX > srcDesc.Width || maxY > static_cast<uint64_t>(srcDesc.Height))
				return;

			const auto readbackLayout = NLS::Render::RHI::DX12::BuildDX12ReadbackLayout(srcDesc.Format, width, height);
			if (readbackLayout.bytesPerPixel == 0u || readbackLayout.readbackSize == 0u)
				return;

			D3D12_HEAP_PROPERTIES heapProperties{};
			heapProperties.Type = D3D12_HEAP_TYPE_READBACK;
			heapProperties.CreationNodeMask = 0;
			heapProperties.VisibleNodeMask = 0;

			D3D12_RESOURCE_DESC bufferDesc{};
			bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bufferDesc.Width = readbackLayout.readbackSize;
			bufferDesc.Height = 1;
			bufferDesc.DepthOrArraySize = 1;
			bufferDesc.MipLevels = 1;
			bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
			bufferDesc.SampleDesc.Count = 1;
			bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

			Microsoft::WRL::ComPtr<ID3D12Resource> readbackResource;
			HRESULT hr = m_device->CreateCommittedResource(
				&heapProperties,
				D3D12_HEAP_FLAG_NONE,
				&bufferDesc,
				D3D12_RESOURCE_STATE_COPY_DEST,
				nullptr,
				IID_PPV_ARGS(readbackResource.GetAddressOf()));
			if (FAILED(hr))
				return;

			Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocator;
			hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(commandAllocator.GetAddressOf()));
			if (FAILED(hr))
				return;

			Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
			hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.Get(), nullptr, IID_PPV_ARGS(commandList.GetAddressOf()));
			if (FAILED(hr))
				return;

			D3D12_RESOURCE_BARRIER toCopySourceBarrier{};
			toCopySourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			toCopySourceBarrier.Transition.pResource = srcResource;
			toCopySourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
			toCopySourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
			toCopySourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			commandList->ResourceBarrier(1, &toCopySourceBarrier);

			D3D12_TEXTURE_COPY_LOCATION srcLocation{};
			srcLocation.pResource = srcResource;
			srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			srcLocation.SubresourceIndex = 0;

			D3D12_TEXTURE_COPY_LOCATION dstLocation{};
			dstLocation.pResource = readbackResource.Get();
			dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			dstLocation.PlacedFootprint.Offset = 0;
			dstLocation.PlacedFootprint.Footprint.Format = srcDesc.Format;
			dstLocation.PlacedFootprint.Footprint.Width = width;
			dstLocation.PlacedFootprint.Footprint.Height = height;
			dstLocation.PlacedFootprint.Footprint.Depth = 1;
			dstLocation.PlacedFootprint.Footprint.RowPitch = readbackLayout.rowPitch;

			D3D12_BOX sourceBox{};
			sourceBox.left = x;
			sourceBox.top = y;
			sourceBox.front = 0;
			sourceBox.right = x + width;
			sourceBox.bottom = y + height;
			sourceBox.back = 1;

			commandList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, &sourceBox);

			D3D12_RESOURCE_BARRIER toCommonBarrier{};
			toCommonBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			toCommonBarrier.Transition.pResource = srcResource;
			toCommonBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
			toCommonBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
			toCommonBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			commandList->ResourceBarrier(1, &toCommonBarrier);

			hr = commandList->Close();
			if (FAILED(hr))
				return;

			ID3D12CommandList* commandLists[] = { commandList.Get() };
			m_graphicsQueue->ExecuteCommandLists(1, commandLists);

			Microsoft::WRL::ComPtr<ID3D12Fence> fence;
			hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()));
			if (FAILED(hr))
				return;

			HANDLE fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
			if (fenceEvent == nullptr)
				return;

			const UINT64 fenceValue = 1u;
			hr = m_graphicsQueue->Signal(fence.Get(), fenceValue);
			if (FAILED(hr))
			{
				CloseHandle(fenceEvent);
				return;
			}

			hr = fence->SetEventOnCompletion(fenceValue, fenceEvent);
			if (FAILED(hr))
			{
				CloseHandle(fenceEvent);
				return;
			}

			WaitForSingleObject(fenceEvent, INFINITE);
			CloseHandle(fenceEvent);

			void* mappedData = nullptr;
			D3D12_RANGE readRange{};
			readRange.Begin = 0;
			readRange.End = static_cast<SIZE_T>(readbackLayout.readbackSize);
			hr = readbackResource->Map(0, &readRange, &mappedData);
			if (FAILED(hr) || mappedData == nullptr)
				return;

			const auto* srcBytes = static_cast<const uint8_t*>(mappedData);
			auto* dstBytes = static_cast<uint8_t*>(data);
			const size_t sourceRowPitch = static_cast<size_t>(readbackLayout.rowPitch);
			const size_t packedRowSize = static_cast<size_t>(width) * static_cast<size_t>(readbackLayout.bytesPerPixel);

			if (format == NLS::Render::Settings::EPixelDataFormat::RGB && readbackLayout.bytesPerPixel >= 3u)
			{
				for (uint32_t row = 0; row < height; ++row)
				{
					for (uint32_t col = 0; col < width; ++col)
					{
						const size_t srcIdx = row * sourceRowPitch + col * static_cast<size_t>(readbackLayout.bytesPerPixel);
						const size_t dstIdx = (static_cast<size_t>(row) * width + col) * 3u;
						dstBytes[dstIdx + 0] = srcBytes[srcIdx + 0];
						dstBytes[dstIdx + 1] = srcBytes[srcIdx + 1];
						dstBytes[dstIdx + 2] = srcBytes[srcIdx + 2];
					}
				}
			}
			else
			{
				for (uint32_t row = 0; row < height; ++row)
				{
					std::memcpy(
						dstBytes + row * packedRowSize,
						srcBytes + row * sourceRowPitch,
						packedRowSize);
				}
			}

			D3D12_RANGE writeRange{};
			writeRange.Begin = 0;
			writeRange.End = 0;
			readbackResource->Unmap(0, &writeRange);
#else
			(void)texture;
			(void)x;
			(void)y;
			(void)width;
			(void)height;
			(void)format;
			(void)type;
			(void)data;
#endif
		}
	}


	std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateNativeDX12ExplicitDevice(
		ID3D12Device* device,
		ID3D12CommandQueue* graphicsQueue,
		IDXGIFactory6* factory,
		IDXGIAdapter1* adapter,
		const NLS::Render::RHI::RHIDeviceCapabilities& capabilities,
		const std::string& vendor,
		const std::string& hardware)
	{
		return std::make_shared<NativeDX12ExplicitDevice>(device, graphicsQueue, factory, adapter, capabilities, vendor, hardware);
	}

#if defined(_WIN32)
	std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateDX12RhiDevice(bool debugMode)
	{
		EnableDx12Dred();

		UINT factoryFlags = 0;
#if defined(_DEBUG)
		if (debugMode)
		{
			Microsoft::WRL::ComPtr<ID3D12Debug5> debugController5;
			const HRESULT debug5Hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debugController5));
			if (SUCCEEDED(debug5Hr) && debugController5 != nullptr)
			{
				debugController5->EnableDebugLayer();
				debugController5->SetEnableGPUBasedValidation(TRUE);
				debugController5->SetEnableAutoName(TRUE);
				NLS_LOG_INFO("CreateDX12RhiDevice: enabled DX12 debug layer with GPU-based validation and auto names via ID3D12Debug5");
				factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
			}
			else
			{
				Microsoft::WRL::ComPtr<ID3D12Debug3> debugController3;
				const HRESULT debug3Hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debugController3));
				if (SUCCEEDED(debug3Hr) && debugController3 != nullptr)
				{
					debugController3->EnableDebugLayer();
					debugController3->SetEnableGPUBasedValidation(TRUE);
					debugController3->SetEnableSynchronizedCommandQueueValidation(TRUE);
					NLS_LOG_INFO("CreateDX12RhiDevice: enabled DX12 debug layer with GPU-based validation via ID3D12Debug3");
					factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
				}
				else
				{
					Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
					const HRESULT debugHr = D3D12GetDebugInterface(IID_PPV_ARGS(&debugController));
					if (SUCCEEDED(debugHr) && debugController != nullptr)
					{
						debugController->EnableDebugLayer();
						NLS_LOG_INFO("CreateDX12RhiDevice: enabled DX12 debug layer via ID3D12Debug");
						factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
					}
					else
					{
						NLS_LOG_WARNING(
							"CreateDX12RhiDevice: failed to acquire DX12 debug controller hr=" + std::to_string(debugHr) +
							", debug3 hr=" + std::to_string(debug3Hr) +
							", debug5 hr=" + std::to_string(debug5Hr));
					}
				}
			}
		}
#endif

		ComPtr<IDXGIFactory6> factory;
		if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory))))
		{
			NLS_LOG_ERROR("CreateDX12RhiDevice: failed to create DXGI factory");
			return nullptr;
		}

		// Find suitable adapter
		ComPtr<IDXGIAdapter1> adapter;
		for (UINT adapterIndex = 0; ; ++adapterIndex)
		{
			ComPtr<IDXGIAdapter1> candidate;
			if (factory->EnumAdapters1(adapterIndex, &candidate) == DXGI_ERROR_NOT_FOUND)
				break;

			DXGI_ADAPTER_DESC1 adapterDesc{};
			candidate->GetDesc1(&adapterDesc);
			if ((adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
				continue;

			// Check if this adapter supports D3D12
			ComPtr<ID3D12Device> testDevice;
			if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&testDevice))))
			{
				adapter = candidate;
				break;
			}
		}

		if (!adapter)
		{
			NLS_LOG_ERROR("CreateDX12RhiDevice: failed to find suitable DX12 adapter");
			return nullptr;
		}

		// Get adapter description for vendor/hardware info
		DXGI_ADAPTER_DESC1 adapterDesc{};
		adapter->GetDesc1(&adapterDesc);
		std::string hardware;
		hardware.assign(adapterDesc.Description, adapterDesc.Description + wcslen(adapterDesc.Description));

		// Create device
		ComPtr<ID3D12Device> device;
		if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
		{
			NLS_LOG_ERROR("CreateDX12RhiDevice: failed to create DX12 device");
			return nullptr;
		}

		ComPtr<ID3D12InfoQueue> infoQueue;
		if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&infoQueue))) && infoQueue != nullptr)
		{
			NLS_LOG_INFO("CreateDX12RhiDevice: DX12 info queue available");
		}
		else
		{
			NLS_LOG_WARNING("CreateDX12RhiDevice: DX12 info queue unavailable");
		}

		// Create command queue
		const D3D12_COMMAND_QUEUE_DESC queueDesc{
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			0,
			D3D12_COMMAND_QUEUE_FLAG_NONE,
			0
		};

		ComPtr<ID3D12CommandQueue> graphicsQueue;
		if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&graphicsQueue))))
		{
			NLS_LOG_ERROR("CreateDX12RhiDevice: failed to create DX12 command queue");
			return nullptr;
		}

		// Build capabilities
		NLS::Render::RHI::RHIDeviceCapabilities capabilities{};
		capabilities.backendReady = true;
		capabilities.supportsGraphics = true;
		capabilities.supportsCompute = true;
		capabilities.supportsSwapchain = true;
		capabilities.supportsFramebufferBlit = true;
		capabilities.supportsDepthBlit = true;
		capabilities.supportsCurrentSceneRenderer = true;
		capabilities.supportsOffscreenFramebuffers = true;
		capabilities.supportsFramebufferReadback = true;
		capabilities.supportsEditorPickingReadback = true;
		capabilities.supportsUITextureHandles = true;
		capabilities.supportsCubemaps = true;
		capabilities.supportsMultiRenderTargets = true;
		capabilities.maxTextureDimension2D = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
		capabilities.maxColorAttachments = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT;
		capabilities.supportsExplicitBarriers = true;

		return std::make_shared<NativeDX12ExplicitDevice>(
			device.Get(),
			graphicsQueue.Get(),
			factory.Get(),
			adapter.Get(),
			capabilities,
			"DX12",
			hardware);
	}
#else
	std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateDX12RhiDevice(bool /*debugMode*/)
	{
		NLS_LOG_WARNING("CreateDX12RhiDevice: DX12 only supported on Windows");
		return nullptr;
	}
#endif
}
