
#ifndef __C_VIDEO_DIRECTX_12_H_INCLUDED__
#define __C_VIDEO_DIRECTX_12_H_INCLUDED__
#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_12_
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

#include "CNullDriver.h"
#include "SIrrCreationParameters.h"
#include "IMaterialRendererServices.h"


#include <chrono>
namespace irr
{
	namespace video
	{

		struct CD3DX12_DEFAULT {};
		struct CD3DX12_CPU_DESCRIPTOR_HANDLE : public D3D12_CPU_DESCRIPTOR_HANDLE
		{
			CD3DX12_CPU_DESCRIPTOR_HANDLE() = default;
			explicit CD3DX12_CPU_DESCRIPTOR_HANDLE(const D3D12_CPU_DESCRIPTOR_HANDLE &o) :
				D3D12_CPU_DESCRIPTOR_HANDLE(o)
			{}
			CD3DX12_CPU_DESCRIPTOR_HANDLE(CD3DX12_DEFAULT) { ptr = 0; }
			CD3DX12_CPU_DESCRIPTOR_HANDLE(_In_ const D3D12_CPU_DESCRIPTOR_HANDLE &other, INT offsetScaledByIncrementSize)
			{
				InitOffsetted(other, offsetScaledByIncrementSize);
			}

			CD3DX12_CPU_DESCRIPTOR_HANDLE(_In_ const D3D12_CPU_DESCRIPTOR_HANDLE &other, INT offsetInDescriptors, UINT descriptorIncrementSize)
			{
				InitOffsetted(other, offsetInDescriptors, descriptorIncrementSize);
			}

			CD3DX12_CPU_DESCRIPTOR_HANDLE& Offset(INT offsetInDescriptors, UINT descriptorIncrementSize)
			{
				ptr += INT64(offsetInDescriptors) * UINT64(descriptorIncrementSize);
				return *this;
			}

			CD3DX12_CPU_DESCRIPTOR_HANDLE& Offset(INT offsetScaledByIncrementSize)
			{
				ptr += offsetScaledByIncrementSize;
				return *this;
			}
			bool operator==(_In_ const D3D12_CPU_DESCRIPTOR_HANDLE& other) const
			{
				return (ptr == other.ptr);
			}
			bool operator!=(_In_ const D3D12_CPU_DESCRIPTOR_HANDLE& other) const
			{
				return (ptr != other.ptr);
			}
			CD3DX12_CPU_DESCRIPTOR_HANDLE &operator=(const D3D12_CPU_DESCRIPTOR_HANDLE &other)
			{
				ptr = other.ptr;
				return *this;
			}
			inline void InitOffsetted(_In_ const D3D12_CPU_DESCRIPTOR_HANDLE &base, INT offsetScaledByIncrementSize)
			{
				InitOffsetted(*this, base, offsetScaledByIncrementSize);
			}
			inline void InitOffsetted(_In_ const D3D12_CPU_DESCRIPTOR_HANDLE &base, INT offsetInDescriptors, UINT descriptorIncrementSize)
			{
				InitOffsetted(*this, base, offsetInDescriptors, descriptorIncrementSize);
			}
			static inline void InitOffsetted(_Out_ D3D12_CPU_DESCRIPTOR_HANDLE &handle, _In_ const D3D12_CPU_DESCRIPTOR_HANDLE &base, INT offsetScaledByIncrementSize)
			{
				handle.ptr = base.ptr + offsetScaledByIncrementSize;
			}
			static inline void InitOffsetted(_Out_ D3D12_CPU_DESCRIPTOR_HANDLE &handle, _In_ const D3D12_CPU_DESCRIPTOR_HANDLE &base, INT offsetInDescriptors, UINT descriptorIncrementSize)
			{
				handle.ptr = static_cast<SIZE_T>(base.ptr + INT64(offsetInDescriptors) * UINT64(descriptorIncrementSize));
			}
		};
		struct CD3DX12_RESOURCE_BARRIER : public D3D12_RESOURCE_BARRIER
		{
			CD3DX12_RESOURCE_BARRIER() = default;
			explicit CD3DX12_RESOURCE_BARRIER(const D3D12_RESOURCE_BARRIER &o) :
				D3D12_RESOURCE_BARRIER(o)
			{}
			static inline CD3DX12_RESOURCE_BARRIER Transition(
				_In_ ID3D12Resource* pResource,
				D3D12_RESOURCE_STATES stateBefore,
				D3D12_RESOURCE_STATES stateAfter,
				UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
				D3D12_RESOURCE_BARRIER_FLAGS flags = D3D12_RESOURCE_BARRIER_FLAG_NONE)
			{
				CD3DX12_RESOURCE_BARRIER result = {};
				D3D12_RESOURCE_BARRIER &barrier = result;
				result.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				result.Flags = flags;
				barrier.Transition.pResource = pResource;
				barrier.Transition.StateBefore = stateBefore;
				barrier.Transition.StateAfter = stateAfter;
				barrier.Transition.Subresource = subresource;
				return result;
			}
			static inline CD3DX12_RESOURCE_BARRIER Aliasing(
				_In_ ID3D12Resource* pResourceBefore,
				_In_ ID3D12Resource* pResourceAfter)
			{
				CD3DX12_RESOURCE_BARRIER result = {};
				D3D12_RESOURCE_BARRIER &barrier = result;
				result.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
				barrier.Aliasing.pResourceBefore = pResourceBefore;
				barrier.Aliasing.pResourceAfter = pResourceAfter;
				return result;
			}
			static inline CD3DX12_RESOURCE_BARRIER UAV(
				_In_ ID3D12Resource* pResource)
			{
				CD3DX12_RESOURCE_BARRIER result = {};
				D3D12_RESOURCE_BARRIER &barrier = result;
				result.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
				barrier.UAV.pResource = pResource;
				return result;
			}
		};



		class CD3D12Driver : public CNullDriver, IMaterialRendererServices {
		public:
			CD3D12Driver(const irr::SIrrlichtCreationParameters& params,
				io::IFileSystem* io, HWND window)
				: CNullDriver(io, params.WindowSize),
				Params(params),
				DriverType(D3D_DRIVER_TYPE_HARDWARE),
				Device(NULL), SwapChain(NULL), Adapter(NULL), DXGIFactory(NULL),
				Output(NULL),ColorFormat(ECF_A8R8G8B8),
				DepthStencilFormat(DXGI_FORMAT_UNKNOWN), D3DColorFormat(DXGI_FORMAT_R8G8B8A8_UNORM),
				MaxTextureUnits(MATERIAL_MAX_TEXTURES), // DirectX 12 can handle much more than this value, but keep compatibility
				Name("Direct3D ") // which version will be added later
			{
				UINT createFactoryFlags = 0;

#if defined(_DEBUG)

				createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;

#endif
				(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&DXGIFactory)));
				IDXGIAdapter1* dxgiAdapter1;
				SIZE_T maxDedicatedVideoMemory = 0;

				for (UINT i = 0; DXGIFactory->EnumAdapters1(i, &dxgiAdapter1) != DXGI_ERROR_NOT_FOUND; ++i)
				{

					DXGI_ADAPTER_DESC1 dxgiAdapterDesc1;

					dxgiAdapter1->GetDesc1(&dxgiAdapterDesc1);



					// Check to see if the adapter can create a D3D12 device without actually 

					// creating it. The adapter with the largest dedicated video memory

					// is favored.

					

				}
				D3D12CreateDevice(Adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&Device));

				g_CommandQueue = CreateCommandQueue(D3D12_COMMAND_LIST_TYPE_DIRECT);

				SwapChain = CreateSwapChain(window, g_CommandQueue, params.WindowSize.Width, params.WindowSize.Height, 3);

				g_CurrentBackBufferIndex = SwapChain->GetCurrentBackBufferIndex();

				g_RTVDescriptorHeap = CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3);

				g_RTVDescriptorSize = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);



				UpdateRenderTargetViews(SwapChain, g_RTVDescriptorHeap);



				for (int i = 0; i < 3; ++i)

				{

					g_CommandAllocators[i] = CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT);

				}

				g_CommandList = CreateCommandList(g_CommandAllocators[g_CurrentBackBufferIndex], D3D12_COMMAND_LIST_TYPE_DIRECT);



				Fence = CreateFence();
				
			}
			~CD3D12Driver()
			{
				Flush(g_CommandQueue, Fence	, g_FenceValue ,g_FenceEvent);
			}

			bool initDriver(HWND window, bool somthing)
			{
				return true;
			}
			ID3D12CommandQueue* CreateCommandQueue(D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_QUEUE_FLAGS flags = D3D12_COMMAND_QUEUE_FLAG_NONE,D3D12_COMMAND_QUEUE_PRIORITY priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL)
			{
				ID3D12CommandQueue* d3d12CommandQueue;

				D3D12_COMMAND_QUEUE_DESC desc = {};
				desc.Type = type;
				desc.Priority = priority;
				desc.Flags = flags;
				desc.NodeMask = 0;
				Device->CreateCommandQueue(&desc, IID_PPV_ARGS(&d3d12CommandQueue));
				return d3d12CommandQueue;
			}
			IDXGISwapChain4* CreateSwapChain(HWND hWnd,	ID3D12CommandQueue* commandQueue,
				uint32_t width, uint32_t height, uint32_t bufferCount)
				{
				IDXGISwapChain4* dxgiSwapChain4;
				IDXGIFactory4* dxgiFactory4;
				UINT createFactoryFlags = 0;
#if defined(_DEBUG)
				createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
				(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory4)));
				DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
				swapChainDesc.Width = width;
				swapChainDesc.Height = height;
				swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				swapChainDesc.Stereo = FALSE;
				swapChainDesc.SampleDesc = { 1, 0 };
				swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
				swapChainDesc.BufferCount = bufferCount;
				swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
				swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
				swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
				// It is recommended to always allow tearing if tearing support is available.
				swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
				
				IDXGISwapChain1* swapChain1;
				(dxgiFactory4->CreateSwapChainForHwnd(
					commandQueue,
					hWnd,
					&swapChainDesc,
					nullptr,
					nullptr,
					&swapChain1));
				// Disable the Alt+Enter fullscreen toggle feature. Switching to fullscreen
				// will be handled manually.
				(dxgiFactory4->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER));
				(dxgiSwapChain4 = static_cast <IDXGISwapChain4*>(swapChain1));
				return dxgiSwapChain4;
			}

			ID3D12DescriptorHeap* CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t numDescriptors)
			{

				ID3D12DescriptorHeap* descriptorHeap;
				D3D12_DESCRIPTOR_HEAP_DESC desc = {};
				desc.NumDescriptors = numDescriptors;
				desc.Type = type;
				Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&descriptorHeap));
				return descriptorHeap;
			}
			void UpdateRenderTargetViews(IDXGISwapChain4* swapChain, ID3D12DescriptorHeap* descriptorHeap)
			{
				auto rtvDescriptorSize = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
				CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(descriptorHeap->GetCPUDescriptorHandleForHeapStart());
				for (int i = 0; i < 3; ++i)
				{
					ID3D12Resource* backBuffer;

					(swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer)));
					Device->CreateRenderTargetView(backBuffer, nullptr, rtvHandle);
					g_BackBuffers[i] = backBuffer;
					rtvHandle.Offset(rtvDescriptorSize);
				}
			}
			ID3D12CommandAllocator* CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type)
			{
				ID3D12CommandAllocator* commandAllocator;
				(Device->CreateCommandAllocator(type, IID_PPV_ARGS(&commandAllocator)));
				return commandAllocator;
			}
			ID3D12GraphicsCommandList* CreateCommandList(ID3D12CommandAllocator* commandAllocator, D3D12_COMMAND_LIST_TYPE type)
			{
							ID3D12GraphicsCommandList* commandList;
				(Device->CreateCommandList(0, type, commandAllocator, nullptr, IID_PPV_ARGS(&commandList)));
				(commandList->Close());
				return commandList;
			}
			ID3D12Fence* CreateFence(D3D12_FENCE_FLAGS flags = D3D12_FENCE_FLAG_NONE)
			{
				ID3D12Fence* fence;
				(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
				return fence;
			}

			uint64_t Signal(ID3D12CommandQueue* commandQueue, ID3D12Fence* fence, uint64_t& fenceValue)
			{
				uint64_t fenceValueForSignal = ++fenceValue;
				(commandQueue->Signal(fence, fenceValueForSignal));
				return fenceValueForSignal;
			}



			void WaitForFenceValue(ID3D12Fence* fence, uint64_t fenceValue, HANDLE fenceEvent,
				std::chrono::milliseconds duration = std::chrono::milliseconds::duration())
			{
				if (fence->GetCompletedValue() < fenceValue)
				{
					(fence->SetEventOnCompletion(fenceValue, fenceEvent));
					::WaitForSingleObject(fenceEvent, static_cast<DWORD>(duration.count()));
				}
			}



			void Flush(ID3D12CommandQueue* commandQueue, ID3D12Fence* fence,
				uint64_t& fenceValue, HANDLE fenceEvent)
				{
				uint64_t fenceValueForSignal = Signal(commandQueue, fence, fenceValue);
				WaitForFenceValue(fence, fenceValueForSignal, fenceEvent);
			}

			virtual bool beginScene(bool backBuffer = true, bool zBuffer = true,
				SColor color = SColor(255, 0, 0, 0),
				const SExposedVideoData& videoData = SExposedVideoData(),
				core::rect<s32>* sourceRect = 0)
			{
				auto commandAllocator = g_CommandAllocators[g_CurrentBackBufferIndex];

				auto lbackBuffer = g_BackBuffers[g_CurrentBackBufferIndex];



				commandAllocator->Reset();

				g_CommandList->Reset(commandAllocator, nullptr);



				// Clear the render target.

				ClearBackBuffer(lbackBuffer);
				return true;

			}
			virtual bool endScene()
			{
				// Present

				{

					auto lbackBuffer = g_BackBuffers[g_CurrentBackBufferIndex];
					CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(lbackBuffer,D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

					g_CommandList->ResourceBarrier(1, &barrier);



					(g_CommandList->Close());



					ID3D12CommandList* const commandLists[] = {g_CommandList};

					g_CommandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);
					g_FrameFenceValues[g_CurrentBackBufferIndex] = Signal(g_CommandQueue, Fence, g_FenceValue);



					UINT syncInterval =0;

					UINT presentFlags = 0;

					SwapChain->Present(syncInterval, presentFlags);



					g_CurrentBackBufferIndex = SwapChain->GetCurrentBackBufferIndex();



					WaitForFenceValue(Fence, g_FrameFenceValues[g_CurrentBackBufferIndex], g_FenceEvent);

				}
				return true;
			}
			void ClearBackBuffer(ID3D12Resource * lbackBuffer)
			{
				{

					CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(lbackBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
					g_CommandList->ResourceBarrier(1, &barrier);
					FLOAT clearColor[] = { 0.4f, 0.6f, 0.9f, 1.0f };
					CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(g_RTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
						g_CurrentBackBufferIndex, g_RTVDescriptorSize);
					g_CommandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

				}
			}
		private:
			// DXGI objects
			DXGI_SWAP_CHAIN_DESC present;
			IDXGISwapChain4* SwapChain;
			IDXGIAdapter4* Adapter;
			IDXGIOutput* Output;
			IDXGIFactory4* DXGIFactory;

			// D3D 12 Device objects
			D3D_DRIVER_TYPE DriverType;
			D3D_FEATURE_LEVEL FeatureLevel;
			//ID3D12Device* Device;
			ID3D12Device2* Device;
			ID3D12CommandQueue* g_CommandQueue;
			ID3D12Resource* g_BackBuffers[3];
			ID3D12GraphicsCommandList* g_CommandList;
			ID3D12CommandAllocator* g_CommandAllocators[3];
			ID3D12DescriptorHeap* g_RTVDescriptorHeap;
			UINT g_RTVDescriptorSize;
			UINT g_CurrentBackBufferIndex;

			u32 MaxTextureUnits;
			u32 MaxUserClipPlanes;
			f32 MaxLightDistance;
			s32 LastSetLight;

			ECOLOR_FORMAT ColorFormat;
			DXGI_FORMAT D3DColorFormat;
			DXGI_FORMAT DepthStencilFormat;		// Best format for depth stencil
			SIrrlichtCreationParameters Params;



			ID3D12Fence * Fence;
			HANDLE g_FenceEvent;


			uint64_t g_FenceValue = 0;

			uint64_t g_FrameFenceValues[3] = {};
			core::stringw Name;

			// Inherited via IMaterialRendererServices
			virtual void setBasicRenderStates(const SMaterial & material, const SMaterial & lastMaterial, bool resetAllRenderstates) override;
			virtual s32 getVertexShaderConstantID(const c8 * name) override;
			virtual bool setVertexShaderConstant(s32 index, const f32 * floats, int count) override;
			virtual bool setVertexShaderConstant(s32 index, const s32 * ints, int count) override;
			virtual void setVertexShaderConstant(const f32 * data, s32 startRegister, s32 constantAmount = 1) override;
			virtual void setPixelShaderConstant(const f32 * data, s32 startRegister, s32 constantAmount = 1) override;
			virtual s32 getPixelShaderConstantID(const c8 * name) override;
			virtual bool setPixelShaderConstant(s32 index, const f32 * floats, int count) override;
			virtual bool setPixelShaderConstant(s32 index, const s32 * ints, int count) override;
			virtual IVideoDriver * getVideoDriver() override;
};
	}
}


namespace irr
{
	namespace video
	{

#ifdef _IRR_COMPILE_WITH_DIRECT3D_12_
		//! creates a video driver
		IVideoDriver* createDirectX12Driver(const irr::SIrrlichtCreationParameters& params,
			io::IFileSystem* io, HWND window)
		{
			CD3D12Driver* dx12 = new CD3D12Driver(params, io, window);
			if (!dx12->initDriver(window, false))
			{
				dx12->drop();
				dx12 = 0;
			}

			return dx12;
		}
#endif // _IRR_COMPILE_WITH_DIRECT3D_11_

	} // end namespace video
} // end namespace irr
#endif
#endif