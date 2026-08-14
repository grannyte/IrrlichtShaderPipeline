
#ifndef __C_VIDEO_DIRECTX_11ON12_H_INCLUDED__
#define __C_VIDEO_DIRECTX_11ON12_H_INCLUDED__
#include "os.h"
#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_12_
#include <d3d12.h>
#include <dxgi1_6.h>

#include <dxgi.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

#include "CNullDriver.h"
#include "SIrrCreationParameters.h"
#include "IMaterialRendererServices.h"
#include "d3d11on12.h"
#include <d3d11.h>

#include <chrono>
#include "CD3D11Driver.h"
#include "CD3D12Helpers.h"
namespace irr
{
	namespace video
	{

		class CD3D11On12Driver : public CD3D11Driver {
		public:
			CD3D11On12Driver(const irr::SIrrlichtCreationParameters& params,
				io::IFileSystem* io, HWND window);
			bool initDriver(HWND hwnd, bool pureSoftware);
			~CD3D11On12Driver();

			// Distingue explicitement l'interop de CD3D12Driver (natif) : sans ce override,
			// CD3D11Driver::getDriverType() renverrait EDT_DIRECT3D11, ce qui etait deja
			// le comportement (jamais corrige) avant ce renommage.
			virtual E_DRIVER_TYPE getDriverType() const _IRR_OVERRIDE_ { return EDT_DIRECT3D11ON12; }
			virtual const wchar_t* getName() const _IRR_OVERRIDE_ { return L"Direct3D 11-on-12 (interop)"; }

			ID3D12CommandQueue* CreateCommandQueue(D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_QUEUE_FLAGS flags = D3D12_COMMAND_QUEUE_FLAG_NONE, D3D12_COMMAND_QUEUE_PRIORITY priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL)
			{
				ID3D12CommandQueue* d3d12CommandQueue;

				D3D12_COMMAND_QUEUE_DESC desc = {};
				desc.Type = type;
				desc.Priority = priority;
				desc.Flags = flags;
				desc.NodeMask = 0;
				DeviceADV->CreateCommandQueue(&desc, IID_PPV_ARGS(&d3d12CommandQueue));
				return d3d12CommandQueue;
			}
			IDXGISwapChain4* CreateSwapChain(HWND hWnd, ID3D12CommandQueue* commandQueue,
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
				DeviceADV->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&descriptorHeap));
				return descriptorHeap;
			}
			void UpdateRenderTargetViews(IDXGISwapChain4* swapChain, ID3D12DescriptorHeap* descriptorHeap)
			{
				auto rtvDescriptorSize = DeviceADV->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
				CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(descriptorHeap->GetCPUDescriptorHandleForHeapStart());
				for (int i = 0; i < 3; ++i)
				{
					ID3D12Resource* backBuffer;

					(swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer)));
					DeviceADV->CreateRenderTargetView(backBuffer, nullptr, rtvHandle);
					g_BackBuffers[i] = backBuffer;
					rtvHandle.Offset(rtvDescriptorSize);
				}
			}
			ID3D12CommandAllocator* CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type)
			{
				ID3D12CommandAllocator* commandAllocator;
				(DeviceADV->CreateCommandAllocator(type, IID_PPV_ARGS(&commandAllocator)));
				return commandAllocator;
			}
			ID3D12GraphicsCommandList* CreateCommandList(ID3D12CommandAllocator* commandAllocator, D3D12_COMMAND_LIST_TYPE type)
			{
				ID3D12GraphicsCommandList* commandList;
				(DeviceADV->CreateCommandList(0, type, commandAllocator, nullptr, IID_PPV_ARGS(&commandList)));
				(commandList->Close());
				return commandList;
			}
			ID3D12Fence* CreateFence(D3D12_FENCE_FLAGS flags = D3D12_FENCE_FLAG_NONE)
			{
				ID3D12Fence* fence;
				(DeviceADV->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
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
		private:
			// DXGI objects
			//DXGI_SWAP_CHAIN_DESC present;
			//IDXGISwapChain4* SwapChain;
			IDXGIOutput* Output;

			//ID3D12Device* Device;
			ID3D12Device2* DeviceADV;

			// pointer to array of 3d command queues
			ID3D12CommandQueue* g_CommandQueues[16];
			// array of compute command queues
			ID3D12CommandQueue* g_ComputeCommandQueues[16];
			// array of copy command queues
			ID3D12CommandQueue* g_CopyCommandQueues[16];


			ID3D12Resource* g_BackBuffers[3];
			ID3D12GraphicsCommandList* g_CommandList;
			ID3D12CommandAllocator* g_CommandAllocators[3];
			ID3D12DescriptorHeap* g_RTVDescriptorHeap;
			UINT g_RTVDescriptorSize;
			UINT g_CurrentBackBufferIndex;


			// Just one clip plane for now
			core::array<core::plane3df> ClipPlanes;
			bool ClipPlaneEnabled[3];

			ID3D12Fence* Fence;
			HANDLE g_FenceEvent;


			uint64_t g_FenceValue = 0;

			uint64_t g_FrameFenceValues[3] = {};

			// Inherited via IMaterialRendererServices
			private:

				inline void logFormatError(HRESULT hr, irr::core::stringc msg)
				{
					LPTSTR errorText = NULL;
					FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS,
						NULL,
						hr,
						MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
						(LPTSTR)&errorText,
						0,
						NULL);

					if (errorText != NULL)
					{
						irr::os::Printer::log(msg.c_str(), errorText, irr::ELL_ERROR);
					}
					else
					{
						irr::os::Printer::log((msg + ".").c_str(), irr::ELL_ERROR);
					}

					LocalFree(errorText);
					errorText = NULL;
				}

			};
		}
	}


namespace irr
{
	namespace video
	{

#ifdef _IRR_COMPILE_WITH_DIRECT3D_12_
		//! creates a video driver
		IVideoDriver* createDirectX11On12Driver(const irr::SIrrlichtCreationParameters& params,
			io::IFileSystem* io, HWND window)
		{
			CD3D11On12Driver* dx12 = new CD3D11On12Driver(params, io, window);
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
