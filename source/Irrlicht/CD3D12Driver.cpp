// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// D3D12 video driver: device/queue/swapchain/heaps/sync, buffer and texture
// management, pipeline state construction, and scene drawing.

#include "CD3D12Driver.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_12_

#include <cmath>
#include <set>
#include <mutex>
#include "CAttributes.h"
#include "CVertexDescriptor.h"
#include "CD3D12DeferredContext.h"
#include "CImage.h"
#include "CColorConverter.h"
#include "IImageLoader.h"
#include "IImageWriter.h"
#include "IWriteFile.h"
#include "CMeshManipulator.h"
#include "coreutil.h"

namespace irr
{
	namespace video
	{
		// Declared this way (no #include of the CImageLoaderXXX.h headers) for the same
		// reason as CNullDriver.cpp: CImageLoaderDDS.h, for example, defines its endianness
		// functions (DDSLittleLong etc.) directly in the header without "inline", so a second
		// #include of that header in another translation unit causes an LNK2005 (symbol
		// already defined in CImageLoaderDDS.obj) -- the factories (defined in their own
		// .cpp) avoid the problem.
		IImageLoader* createImageLoaderBMP();
		IImageLoader* createImageLoaderJPG();
		IImageLoader* createImageLoaderTGA();
		IImageLoader* createImageLoaderPSD();
		IImageLoader* createImageLoaderDDS();
		IImageLoader* createImageLoaderPCX();
		IImageLoader* createImageLoaderPNG();
		IImageLoader* createImageLoaderWAL();
		IImageLoader* createImageLoaderHalfLife();
		IImageLoader* createImageLoaderLMP();
		IImageLoader* createImageLoaderPPM();
		IImageLoader* createImageLoaderRGB();
		IImageWriter* createImageWriterBMP();
		IImageWriter* createImageWriterJPG();
		IImageWriter* createImageWriterTGA();
		IImageWriter* createImageWriterPSD();
		IImageWriter* createImageWriterPCX();
		IImageWriter* createImageWriterPNG();
		IImageWriter* createImageWriterPPM();

		CD3D12Driver::CD3D12Driver(const irr::SIrrlichtCreationParameters& params,
			io::IFileSystem* io, HWND window, CD3D12Driver* sharedResources)
			// CNullDriver handles all of the common groundwork: FileSystem (grab), MeshManipulator,
			// DriverAttributes, the 12 image loaders / 7 writers, the standard vertex descriptors,
			// ViewPort = screen size, setFog(), and the default texture creation flags
			// (ETCF_OPTIMIZED_FOR_QUALITY + ETCF_CREATE_MIP_MAPS). None of this is redone here --
			// that was exactly the duplication that inheriting from CNullDriverCommon used to
			// force.
			: CNullDriver(io, params.WindowSize)
			, Params(params)
			, ResourceOwner(sharedResources ? sharedResources->ResourceOwner : this)
			, RTVHeap(sharedResources ? sharedResources->OwnedRTVHeap : OwnedRTVHeap)
			, DSVHeap(sharedResources ? sharedResources->OwnedDSVHeap : OwnedDSVHeap)
			, CBVSRVUAVHeap(sharedResources ? sharedResources->OwnedCBVSRVUAVHeap : OwnedCBVSRVUAVHeap)
			, UploadAllocator(sharedResources ? sharedResources->OwnedUploadAllocator : OwnedUploadAllocator)
			, UploadCommandList(sharedResources ? sharedResources->OwnedUploadCommandList : OwnedUploadCommandList)
			, UploadInProgress(sharedResources ? sharedResources->OwnedUploadInProgress : OwnedUploadInProgress)
			, UploadFence(sharedResources ? sharedResources->OwnedUploadFence : OwnedUploadFence)
			, UploadFenceValue(sharedResources ? sharedResources->OwnedUploadFenceValue : OwnedUploadFenceValue)
			, UploadFenceEvent(sharedResources ? sharedResources->OwnedUploadFenceEvent : OwnedUploadFenceEvent)
			, UploadMutex(sharedResources ? sharedResources->OwnedUploadMutex : OwnedUploadMutex)
		{
#ifdef _DEBUG
			setDebugName("CD3D12Driver");
#endif
			WindowSize = params.WindowSize;
			CurrentRenderTargetSize = WindowSize;

			// Initial "back buffer" state (1 target, swap chain format) -- see
			// CurrentRTVCount/CurrentRTVFormats.
			CurrentRTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;

			// Three user clip planes, same bounds as CD3D11Driver.
			ClipPlanes.push_back(core::plane3df());
			ClipPlanes.push_back(core::plane3df());
			ClipPlanes.push_back(core::plane3df());
		}

		CD3D12Driver::~CD3D12Driver()
		{
			if (DirectQueue && Fence)
			{
				// Wait for the GPU to finish before destroying anything.
				UINT64 finalValue = signalFence();
				if (Fence->GetCompletedValue() < finalValue)
				{
					Fence->SetEventOnCompletion(finalValue, FenceEvent);
					::WaitForSingleObject(FenceEvent, INFINITE);
				}
			}
			// CRITICAL ORDER. ~CNullDriver() (which runs after this body) calls
			// deleteAllTextures() -- but by then CD3D12Driver's members, including the retire
			// queue, will ALREADY be destroyed (a derived destructor releases its members before
			// the base destructor runs). And ~CD3D12Texture calls exactly
			// Driver->retireResource(). Destroy the textures here, while everything is still alive.
			//
			// This is the same trap the old "if (ResourceOwner == this) removeAllTextures();"
			// used to avoid -- it must not be lost when moving to CNullDriver.
			// deleteAllTextures() is a no-op for a deferred context (its cache stays empty, it
			// delegates everything to the immediate driver).
			deleteAllTextures();
			NullTexture = nullptr;

			// The GPU is idle (waited for above): no in-flight command list can reference anything
			// anymore -- drain the queue without consulting the fence, which permanently releases
			// the resources the texture destructors just deposited into it.
			drainRetiredResources(true);

			if (FenceEvent)
				::CloseHandle(FenceEvent);

			// UploadFenceEvent is shared with ImmediateDriver for a CD3D12DeferredContext
			// (see its declaration in the .h) -- only close it if this instance actually
			// owns it.
			if (ResourceOwner == this && UploadFenceEvent)
				::CloseHandle(UploadFenceEvent);

			// NativeRenderers owns nothing (see its declaration): the drop() of each
			// IMaterialRenderer, of the vertex descriptors, of the image loaders/writers,
			// of the mesh manipulator, and of DriverAttributes is done by ~CNullDriver().
			NativeRenderers.clear();
		}

		// ============================== Phase 1: foundation ==============================

		bool CD3D12Driver::initDriver(HWND hwnd)
		{
			if (!createDeviceAndQueue())
				return false;

			if (!createDescriptorHeaps())
				return false;

			if (!createSwapChain(hwnd, WindowSize.Width, WindowSize.Height))
				return false;

			ExposedData.D3D12.D3DDev12 = Device.Get();
			ExposedData.D3D12.SwapChain = SwapChain.Get();
			ExposedData.D3D12.HWnd = hwnd;

			updateRenderTargetViews();

			// The 4 standard descriptors ("standard"/"2tcoords"/"tangents"/"standardcolorf",
			// in that order -- every mesh loader in the engine indexes into them) are the ones
			// from CNullDriver::createVertexDescriptors(), same as on CD3D11Driver.
			createVertexDescriptors();

			if (!createDepthStencilBuffer(WindowSize.Width, WindowSize.Height))
			{
				os::Printer::log("CD3D12Driver: createDepthStencilBuffer a echoue"
					" (zBuffer ne sera pas disponible)", ELL_WARNING);
				// Not fatal: continue without a depth buffer rather than failing the whole
				// initialization, but beginScene(zBuffer=true) will have no real effect.
			}

			if (!createRootSignature())
			{
				os::Printer::log("CD3D12Driver: createRootSignature a echoue", ELL_ERROR);
				return false; // fatal: without a root signature, no PSO can be created (phase 4+)
			}

			if (!createComputeRootSignature())
			{
				os::Printer::log("CD3D12Driver: createComputeRootSignature a echoue"
					" (addComputeShader()/dispatchComputeShader() ne fonctionneront pas)", ELL_WARNING);
				// Not fatal: the graphics pipeline doesn't depend on it, see getOrCreateComputePSO()
				// which returns nullptr cleanly if ComputeRootSignature is absent.
			}

			if (!createMipGenPipeline())
			{
				os::Printer::log("CD3D12Driver: createMipGenPipeline a echoue"
					" (ETCF_CREATE_MIP_MAPS/regenerateMipMapLevels() ne produiront qu'une seule mip level)", ELL_WARNING);
				// Not fatal: CD3D12Texture::generateMips() checks getOrCreateMipGenPSO()
				// and just logs a warning if the dedicated pipeline isn't available.
			}

			// Same default values as CNullDriver::CNullDriver() -- without this,
			// getTextureCreationFlag(ETCF_CREATE_MIP_MAPS) would default to false and no
			// texture load would ever request a mipmap chain.
			setTextureCreationFlag(video::ETCF_OPTIMIZED_FOR_QUALITY, true);
			setTextureCreationFlag(video::ETCF_CREATE_MIP_MAPS, true);

			if (!createBuiltInMaterialRenderers())
			{
				os::Printer::log("CD3D12Driver: createBuiltInMaterialRenderers a echoue", ELL_ERROR);
				return false; // fatal: without the registry, buildPSOKeyFromMaterial()/choosePixelShaderForMaterial() can't resolve anything
			}

			if (!createFrameDrawResources())
			{
				os::Printer::log("CD3D12Driver: createFrameDrawResources a echoue", ELL_ERROR);
				return false; // fatal: without the constant ring/shader-visible SRV heap, drawMeshBuffer() can't bind anything
			}

			HRESULT hr = Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
				Frames[0].CommandAllocator.Get(), nullptr, IID_PPV_ARGS(&CommandList));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: CreateCommandList a echoue", ELL_ERROR);
				return false;
			}
			CommandList->Close();

			hr = Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&UploadAllocator));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: CreateCommandAllocator (upload) a echoue", ELL_ERROR);
				return false;
			}
			hr = Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
				UploadAllocator.Get(), nullptr, IID_PPV_ARGS(&UploadCommandList));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: CreateCommandList (upload) a echoue", ELL_ERROR);
				return false;
			}
			UploadCommandList->Close();

			hr = Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: CreateFence a echoue", ELL_ERROR);
				return false;
			}
			FenceEvent = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
			if (!FenceEvent)
			{
				os::Printer::log("CD3D12Driver: CreateEvent a echoue", ELL_ERROR);
				return false;
			}

			// Fence dedicated to endUploadAndWait(), distinct from Fence/FenceEvent above (see its
			// declaration in the .h for the reason: shared with the CD3D12DeferredContext
			// instances, unlike Fence/FenceEvent which stay private to each instance).
			hr = Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&UploadFence));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: CreateFence (upload) a echoue", ELL_ERROR);
				return false;
			}
			UploadFenceEvent = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);
			if (!UploadFenceEvent)
			{
				os::Printer::log("CD3D12Driver: CreateEvent (upload) a echoue", ELL_ERROR);
				return false;
			}

			// Must come after Fence/FenceEvent/UploadCommandList: createNullTexture() goes through
			// CD3D12Texture::lock()/unlock(), which call beginUpload()/endUploadAndWait().
			if (!createNullTexture())
			{
				os::Printer::log("CD3D12Driver: createNullTexture a echoue"
					" (drawMeshBuffer() sur un materiau sans texture produira un handle SRV nul)", ELL_WARNING);
				// Not fatal: see allocateSRVTableSlot(), which logs its own warning and
				// binds nothing rather than dereferencing a null pointer.
			}

			if (!createStreamOutputResources())
			{
				os::Printer::log("CD3D12Driver: createStreamOutputResources a echoue"
					" (setStreamOutputBuffer() echouera, le reste du driver n'est pas affecte)", ELL_WARNING);
				// Not fatal: nothing besides setStreamOutputBuffer() (Milestone B) depends on
				// StreamOutputCounter/StreamOutputCounterUpload.
			}

			Name = L"Direct3D 12 (natif)";
			os::Printer::log("CD3D12Driver initialise", core::stringc(Name).c_str(), ELL_INFORMATION);
			return true;
		}

		bool CD3D12Driver::createDeviceAndQueue()
		{
			UINT factoryFlags = 0;
#ifdef _DEBUG
			{
				ComPtr<ID3D12Debug> debugController;
				if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
				{
					debugController->EnableDebugLayer();
					// GPU-Based Validation: the standard debug layer only validates what is
					// submitted (API parameters), not what the GPU actually does during
					// execution -- an illegal GPU access (SRV/UAV/vertex out of bounds, etc.) only
					// surfaces as DXGI_ERROR_DEVICE_REMOVED/DRIVER_INTERNAL_ERROR, with the
					// message "the root cause occurred EARLIER" and no useful call stack. GBV
					// instruments the shaders to detect these accesses at the moment they
					// occur (see SetBreakOnSeverity below), at a significant CPU/GPU cost --
					// reserved for debug builds. It is not compatible with GPU capture tools
					// (VS Graphics Debugger, PIX): their capture layer crashes if it is enabled
					// while their hook DLL is already loaded in the process, so skip it whenever
					// one is detected.
					bool captureToolAttached = GetModuleHandleW(L"DXCaptureReplay.dll") != nullptr
						|| GetModuleHandleW(L"WinPixGpuCapturer.dll") != nullptr;
					ComPtr<ID3D12Debug1> debugController1;
					if (!captureToolAttached && SUCCEEDED(debugController.As(&debugController1)))
						debugController1->SetEnableGPUBasedValidation(TRUE);
				}
				factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
			}
#endif
			HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&DXGIFactory));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: CreateDXGIFactory2 a echoue", ELL_ERROR);
				return false;
			}

			// Candidate levels, highest first: probing each in turn (ppDevice=nullptr, so no
			// device is actually constructed) tells us the true ceiling for an adapter without
			// paying for a real ID3D12Device, and without hardcoding 11_0 as if it were the max.
			static const D3D_FEATURE_LEVEL candidateLevels[] = {
				D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
				D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
			};

			// Picks the hardware adapter with the most dedicated VRAM that supports at least
			// D3D_FEATURE_LEVEL_11_0, remembering the highest level each one actually offers.
			SIZE_T bestVRAM = 0;
			D3D_FEATURE_LEVEL bestFeatureLevel = D3D_FEATURE_LEVEL_11_0;
			ComPtr<IDXGIAdapter1> candidate;
			for (UINT i = 0; DXGIFactory->EnumAdapterByGpuPreference(i,
				DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate)) != DXGI_ERROR_NOT_FOUND; ++i)
			{
				DXGI_ADAPTER_DESC1 desc;
				candidate->GetDesc1(&desc);
				if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
					continue;

				D3D_FEATURE_LEVEL supportedLevel = D3D_FEATURE_LEVEL_11_0;
				bool supported = false;
				for (D3D_FEATURE_LEVEL level : candidateLevels)
				{
					if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), level, _uuidof(ID3D12Device), nullptr)))
					{
						supportedLevel = level;
						supported = true;
						break;
					}
				}
				if (!supported)
					continue;

				if (desc.DedicatedVideoMemory > bestVRAM)
				{
					bestVRAM = desc.DedicatedVideoMemory;
					bestFeatureLevel = supportedLevel;
					candidate.As(&Adapter);
				}
			}
			if (!Adapter)
			{
				os::Printer::log("CD3D12Driver: aucun adaptateur compatible D3D12 trouve", ELL_ERROR);
				return false;
			}

			hr = D3D12CreateDevice(Adapter.Get(), bestFeatureLevel, IID_PPV_ARGS(&Device));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: D3D12CreateDevice a echoue", ELL_ERROR);
				return false;
			}

#ifdef _DEBUG
			// EnableDebugLayer() alone only sends its messages to OutputDebugString: they
			// never reach the engine log (os::Printer) and can't be correlated to the
			// passes that trigger them. InfoQueue makes them actionable: break in the debugger
			// ON the offending draw (the call stack then points directly at the pass), instead of
			// a wall of text with no context.
			{
				ComPtr<ID3D12InfoQueue> infoQueue;
				if (SUCCEEDED(Device.As(&infoQueue)))
				{
					infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
					infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);

					// The suboptimal clear-value warnings (#820/#821) are emitted by EVERY
					// post-process pass every frame (about thirty of them): purely a perf concern, they
					// would drown out any genuinely interesting message.
					D3D12_MESSAGE_ID ignored[] = {
						D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
						D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
					};
					D3D12_INFO_QUEUE_FILTER filter = {};
					filter.DenyList.NumIDs = _countof(ignored);
					filter.DenyList.pIDList = ignored;
					infoQueue->AddStorageFilterEntries(&filter);
				}
			}
#endif

			D3D12_COMMAND_QUEUE_DESC queueDesc = {};
			queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
			hr = Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&DirectQueue));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: CreateCommandQueue a echoue", ELL_ERROR);
				return false;
			}

			ComPtr<IDXGIFactory5> factory5;
			if (SUCCEEDED(DXGIFactory.As(&factory5)))
			{
				BOOL allowTearing = FALSE;
				if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
					&allowTearing, sizeof(allowTearing))))
					TearingSupported = (allowTearing == TRUE);
			}

			for (auto& frame : Frames)
			{
				hr = Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
					IID_PPV_ARGS(&frame.CommandAllocator));
				if (FAILED(hr))
				{
					os::Printer::log("CD3D12Driver: CreateCommandAllocator a echoue", ELL_ERROR);
					return false;
				}
			}

			return true;
		}

		bool CD3D12Driver::createDescriptorHeaps()
		{
			// These heaps are CPU-only (shader-visible = false): a descriptor costs a few
			// bytes of system RAM, nothing more. Sizing them generously therefore costs almost
			// nothing, while running out breaks rendering silently.
			//
			// NativeFrameCount + 32 (~35 RTV) was too tight for real deferred rendering: the
			// G-buffer of a deferred renderer plus its post-process chain (bloom, blur...)
			// comfortably exceeds 32 live render targets, and generateMips() borrows one more
            // along the way. Once the heap was full, addRenderTargetTexture() returned a texture
			// with no RTV, and setRenderTarget() ended up looping with "no valid MRT target".
			RTVHeap.init(Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, NativeFrameCount + 1024, false);

			// Back buffer + pool of self-managed depth/stencil buffers (checkRTTDepthBuffer(), one
			// per distinct RTT size) + now also the explicit DEPTH render-target-textures, which
			// also consume a DSV slot (see CD3D12Texture::createDepthStencilView()).
			DSVHeap.init(Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 256, false);

			CBVSRVUAVHeap.init(Device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16384, false);
			return true;
		}

		bool CD3D12Driver::createSwapChain(HWND hwnd, uint32_t width, uint32_t height)
		{
			DXGI_SWAP_CHAIN_DESC1 desc = {};
			desc.Width = width;
			desc.Height = height;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.Stereo = FALSE;
			desc.SampleDesc = { 1, 0 };
			desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			desc.BufferCount = NativeFrameCount;
			desc.Scaling = DXGI_SCALING_STRETCH;
			desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
			desc.Flags = TearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

			ComPtr<IDXGISwapChain1> swapChain1;
			HRESULT hr = DXGIFactory->CreateSwapChainForHwnd(
				DirectQueue.Get(), hwnd, &desc, nullptr, nullptr, &swapChain1);
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: CreateSwapChainForHwnd a echoue", ELL_ERROR);
				return false;
			}
			DXGIFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

			hr = swapChain1.As(&SwapChain);
			if (FAILED(hr))
				return false;

			CurrentFrameIndex = SwapChain->GetCurrentBackBufferIndex();
			return true;
		}

		void CD3D12Driver::updateRenderTargetViews()
		{
			for (UINT i = 0; i < NativeFrameCount; ++i)
			{
				HRESULT hr = SwapChain->GetBuffer(i, IID_PPV_ARGS(&Frames[i].BackBuffer));
				if (FAILED(hr))
				{
					os::Printer::log("CD3D12Driver: SwapChain->GetBuffer a echoue", ELL_ERROR);
					continue;
				}
				UINT rtvIndex;
				CD3DX12_CPU_DESCRIPTOR_HANDLE handle;
				RTVHeap.allocate(rtvIndex, handle);
				Device->CreateRenderTargetView(Frames[i].BackBuffer.Get(), nullptr, handle);
				Frames[i].RTVHandle = handle;
			}
		}

		bool CD3D12Driver::createDepthStencilBuffer(UINT width, UINT height)
		{
			if (width == 0 || height == 0)
				return false;

			D3D12_HEAP_PROPERTIES heapProps = {};
			heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

			D3D12_RESOURCE_DESC desc = {};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = width;
			desc.Height = height;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = DepthStencilFormat;
			desc.SampleDesc = { 1, 0 };
			desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

			D3D12_CLEAR_VALUE clearValue = {};
			clearValue.Format = DepthStencilFormat;
			// 0.0f, not 1.0f: must match the value actually used by clearZBuffer()/
			// setRenderTarget() (inverted depth convention, see their comment) -- otherwise D3D12
			// refuses to use this optimized clear value and Clear() degrades in performance.
			clearValue.DepthStencil.Depth = 0.0f;
			clearValue.DepthStencil.Stencil = 0;

			// Release the old resource before creating a new one at the new size
			// (called from OnResize()) -- the DSV view is rewritten at the same heap slot
			// below, no need for free()/allocate() on every resize.
			DepthStencilBuffer.Reset();

			HRESULT hr = Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue, IID_PPV_ARGS(&DepthStencilBuffer));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: CreateCommittedResource (depth) a echoue", ELL_ERROR);
				HasDepthStencilBuffer = false;
				return false;
			}

			if (!HasDepthStencilBuffer)
			{
				if (!DSVHeap.allocate(DSVHeapIndex, DSVHandle))
				{
					os::Printer::log("CD3D12Driver: heap DSV plein", ELL_ERROR);
					return false;
				}
			}

			D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
			dsvDesc.Format = DepthStencilFormat;
			dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
			Device->CreateDepthStencilView(DepthStencilBuffer.Get(), &dsvDesc, DSVHandle);

			HasDepthStencilBuffer = true;
			return true;
		}

		// Pool of depth/stencil buffers dedicated to render-target-textures, indexed by
		// size -- same strategy as CD3D11Driver::checkDepthBuffer()/DepthBuffers (see
		// CD3D11Driver.cpp), simplified here: no refcounting (no equivalent
		// removeDepthSurface()), the pool grows as distinct RTT sizes are encountered and
		// lives until the driver is destroyed.
		SD3D12RTTDepthBuffer* CD3D12Driver::checkRTTDepthBuffer(const core::dimension2d<u32>& size, UINT sampleCount)
		{
			if (size.Width == 0 || size.Height == 0)
				return nullptr;

			for (auto& depth : RTTDepthBuffers)
			{
				if (depth->Size == size && depth->SampleCount == sampleCount)
					return depth.get();
			}

			auto depth = std::make_unique<SD3D12RTTDepthBuffer>();
			depth->Size = size;
			depth->SampleCount = sampleCount;

			D3D12_HEAP_PROPERTIES heapProps = {};
			heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

			D3D12_RESOURCE_DESC desc = {};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = size.Width;
			desc.Height = size.Height;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = DepthStencilFormat;
			// Must match the SampleDesc.Count of the RTV(s) bound at the same time as
			// this DSV (see CD3D12Driver::setRenderTarget()) -- Quality stays 0, same as
			// CD3D12Texture::createResource() (see its comment).
			desc.SampleDesc = { sampleCount, 0 };
			desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

			D3D12_CLEAR_VALUE clearValue = {};
			clearValue.Format = DepthStencilFormat;
			// 0.0f: same reason as createDepthStencilBuffer() (inverted depth convention).
			clearValue.DepthStencil.Depth = 0.0f;
			clearValue.DepthStencil.Stencil = 0;

			HRESULT hr = Device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue, IID_PPV_ARGS(&depth->Buffer));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver::checkRTTDepthBuffer: CreateCommittedResource a echoue", ELL_ERROR);
				return nullptr;
			}

			CD3DX12_CPU_DESCRIPTOR_HANDLE handle;
			if (!DSVHeap.allocate(depth->DSVHeapIndex, handle))
			{
				os::Printer::log("CD3D12Driver::checkRTTDepthBuffer: heap DSV plein", ELL_ERROR);
				return nullptr;
			}
			depth->DSVHandle = handle;

			D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
			dsvDesc.Format = DepthStencilFormat;
			// A multisampled resource (sampleCount > 1, see desc.SampleDesc
			// above) requires the TEXTURE2DMS view -- TEXTURE2D (single-sample) on an MSAA
			// resource is a mismatch that the debug layer flags, and that WARP/some drivers
			// silently accept at creation time but crash on the first
			// ClearDepthStencilView/OMSetRenderTargets that follows.
			dsvDesc.ViewDimension = (sampleCount > 1) ?
				D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
			Device->CreateDepthStencilView(depth->Buffer.Get(), &dsvDesc, depth->DSVHandle);

			SD3D12RTTDepthBuffer* raw = depth.get();
			RTTDepthBuffers.push_back(std::move(depth));
			return raw;
		}

		bool CD3D12Driver::createRootSignature()
		{
			// The DEFAULT layout: VS+PS present, no user CBV table -- see the declaration comment.
			// Goes through the same cache as every material root signature rather than creating a
			// private object, so the ~24 built-in materials (which compute exactly this key in
			// buildMaterialRootSignature(): compileBuiltIn() produces a VS+PS and reflects nothing)
			// get this very object back instead of a duplicate.
			const u32 builtInLayoutKey =
				(1u << (UserCBVLayoutStageBit + ED3D12UCS_VERTEX)) |
				(1u << (UserCBVLayoutStageBit + ED3D12UCS_PIXEL));

			ID3D12RootSignature* defaultRootSignature = getOrCreateRootSignature(builtInLayoutKey);
			if (!defaultRootSignature)
				return false;

			RootSignature = defaultRootSignature;
			return true;
		}

		ID3D12RootSignature* CD3D12Driver::getOrCreateRootSignature(u32 layoutKey)
		{
			auto cached = RootSignatureCache.find(layoutKey);
			if (cached != RootSignatureCache.end())
				return cached->second.Get();

			if (!Device)
			{
				os::Printer::log("CD3D12Driver::getOrCreateRootSignature: pas de device", ELL_ERROR);
				return nullptr;
			}

			// Layout (see also the comment at the top of CD3D12PSOCache.h). The first 7 parameters are
			// FIXED and present in every material root signature -- bindTransformsAndTexture()/
			// bindLighting()/bindFog() hardcode their indices (WorldConstantSlot..FogConstantSlot,
			// CD3D12Driver.h) precisely so they need not know which material is bound:
			//   [0] CBV b0, root descriptor  -- world matrix, per object, visible in VS.
			//   [1] CBV b1, root descriptor  -- view+projection matrices, visible in VS.
			//   [2] Descriptor table, MaxUserShaderTextureSlots SRVs t0.. -- textures, visible to ALL.
			//   [3] Descriptor table, MaxUserShaderTextureSlots samplers s0.. -- filter/addressing per
			//       SMaterialLayer, visible to ALL (see allocateSamplerTableSlot()).
			//   [4] CBV b2, root descriptor -- user clip planes, visible in PS
			//       (see setClipPlane()/bindTransformsAndTexture()).
			//   [5] CBV b3 (LightingConstantSlot), root descriptor, visible in VS AND PS -- dynamic
			//       lighting (SMaterial::Lighting/AmbientColor/DiffuseColor/SpecularColor/
			//       EmissiveColor/ColorMaterial/NormalizeNormals + the light list
			//       CNullDriver::Lights, see addDynamicLight()/getDynamicLight()). VSMain
			//       (calcLighting(), D3D12DefaultShaderHLSL) reads it for per-vertex lighting of the
			//       ~24 standard built-in types, same VS/PS split as
			//       CD3D11FixedPipelineRenderer.cpp's standardVS()/coords2TVS()/tangentsVS() -- see
			//       bindLighting() (CD3D12Driver.cpp) for the layout details.
			//       PSMainNormalMap*/PSMainParallaxMap* read it TOO, on the PS side this time --
			//       per-pixel lighting is needed for these 6 types (the normal perturbed by the
			//       normal map varies per texel, a per-vertex normal wouldn't be enough), hence the
			//       ALL visibility rather than VERTEX alone.
			//   [6] CBV b4 (FogConstantSlot), root descriptor, visible in PS only -- fog (
			//       SMaterial::FogEnable + setFog()/getFog(), see bindFog()/CD3D12DefaultShaders.h's
			//       FogCB/calcFogFactor()). Same pattern as rootParams[4] (ClipPlanes): root
			//       descriptor PS-only, content evaluated entirely in the pixel shader (fogDist is
			//       computed by VSMain and interpolated, but the fog decision itself -- mode/
			//       start/end/density/enableFog -- is only read by PSMainXxx).
			//   [7..] Descriptor tables, CBV b0..b7 (see MaxUserShaderCBVSlotsPerStage) -- user
			//       cbuffers, ONE PER (stage, register space) PAIR SET IN layoutKey, i.e. one per pair
			//       the shader actually declares a cbuffer at (see reflectCBuffer()/
			//       SD3D12UserShaderCBuffer::Space). Walked stage-major/space-ascending below, which is
			//       the order buildMaterialRootSignature() mirrors when filling
			//       CD3D12MaterialRenderer::UserCBVTables -- the two MUST stay in step, since that
			//       vector is what bindDrawState() feeds to SetGraphicsRootDescriptorTable().
			//       A built-in material sets none of these bits and stops at 7 parameters.
			// b0/b1/b2/b3/b4 are root descriptors (no table): faster to update per draw, a single CBV
			// each. [7..] are tables: multiple user cbuffers per (stage, space) are possible, and a
			// table is the only way to expose several without blowing up the number of root parameters.
			D3D12_ROOT_PARAMETER1 rootParams[MaxRootParameters] = {};

			// The driver's internal CBVs (b0..b4) are in space4, NOT space0 -- see the long
			// comment at the top of CD3D12DefaultShaders.h. In short: the engine's user shaders
			// are written for D3D11 and declare their cbuffers as register(bN), hence
			// space0. As long as the driver occupied space0 with its own constants, any PSO
			// using a user shader with a cbuffer failed at creation ("Root Signature
			// doesn't match Pixel Shader: Shader CBV descriptor range (BaseShaderRegister=0,
			// RegisterSpace=0) is not fully bound") -- black screen. space0..space3 now belong to
			// user shaders (the rootParams[FirstUserCBVRootSlot..] tables below).
			rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			rootParams[0].Descriptor.ShaderRegister = 0;
			rootParams[0].Descriptor.RegisterSpace = DriverConstantRegisterSpace;
			rootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
			rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

			rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			rootParams[1].Descriptor.ShaderRegister = 1;
			rootParams[1].Descriptor.RegisterSpace = DriverConstantRegisterSpace;
			rootParams[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
			rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

			// MaxUserShaderTextureSlots (9) descriptors (t0..t8) instead of the earlier 2 (t0/t1)
			// -- raised to 9 after surveying a real user shader catalog, whose most
			// texture-hungry shaders declared 9. Covers both the ~12
			// built-in multi-texture E_MATERIAL_TYPE values (EMT_SOLID_2_LAYER, EMT_LIGHTMAP*,
			// EMT_DETAIL_MAP, EMT_SPHERE_MAP, EMT_REFLECTION_2_LAYER,
			// EMT_TRANSPARENT_REFLECTION_2_LAYER -- see CD3D12DefaultShaders.h/
			// createBuiltInMaterialRenderers(), which only read t0/t1) and user shaders with
			// N textures (addHighLevelShaderMaterial*). allocateSRVTableSlot() always fills all 9
			// descriptors (NullTexture for any register beyond the layers actually used by
			// the material), so a material with fewer than 9 textures stays valid even if its shader
			// doesn't declare every register -- and a shader with NON-CONTIGUOUS registers (e.g.
			// t0/t1/t3, t2 skipped) is also still valid: D3D12 only requires the root signature to be a
			// superset of the registers actually declared, not an exact match
			// (see the MaxUserShaderTextureSlots comment in CD3D12Driver.h).
			D3D12_DESCRIPTOR_RANGE1 srvRange = {};
			srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			srvRange.NumDescriptors = MaxUserShaderTextureSlots;
			srvRange.BaseShaderRegister = 0;
			srvRange.RegisterSpace = 0;
			srvRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE; // the bound texture changes from one draw to the next
			srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

			rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
			rootParams[2].DescriptorTable.pDescriptorRanges = &srvRange;
			// ALL visibility, not PIXEL: a vertex shader is fully entitled to sample
			// a texture (vertex texture fetch). This is the case for displacement-mapped terrain, whose
			// VS reads a heightmap to displace the patch's vertices. With PIXEL visibility, D3D12
			// validation rejects the PSO ("Root Signature doesn't match Vertex Shader: Shader
			// [sampler|SRV] descriptor range ... is not fully bound in root signature") and
			// CreateGraphicsPipelineState returns E_INVALIDARG: the material can then NEVER
			// be drawn.
			rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			// Sampler table s0 (replaces the old single static linear/
			// wrap sampler) -- a different descriptor is copied in per draw according to the
			// current SMaterialLayer's filter/addressing settings, see allocateSamplerTableSlot().
			// MaxUserShaderTextureSlots (9) descriptors (s0..s8), same reason as srvRange
			// above -- one sampler per texture layer.
			D3D12_DESCRIPTOR_RANGE1 samplerRange = {};
			samplerRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
			samplerRange.NumDescriptors = MaxUserShaderTextureSlots;
			samplerRange.BaseShaderRegister = 0;
			samplerRange.RegisterSpace = 0;
			samplerRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
			samplerRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

			rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			rootParams[3].DescriptorTable.NumDescriptorRanges = 1;
			rootParams[3].DescriptorTable.pDescriptorRanges = &samplerRange;
			// ALL visibility, same reason as rootParams[2] above: the sampler must follow the
			// SRV wherever it's sampled, including in a vertex shader.
			rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			// CBV b2 -- user clip planes (setClipPlane/enableClipPlane),
			// evaluated on the pixel shader side (see CD3D12DefaultShaders.h).
			rootParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			rootParams[4].Descriptor.ShaderRegister = 2;
			rootParams[4].Descriptor.RegisterSpace = DriverConstantRegisterSpace;
			rootParams[4].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
			rootParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			// CBV b0..b7 tables -- user cbuffers (see addHighLevelShaderMaterial()/
			// CD3D12MaterialRenderer::reflectCBuffer()). DATA_VOLATILE (not
			// DATA_STATIC_WHILE_SET_AT_EXECUTE): unlike b0/b1/b2, the content changes from one draw to
			// the next for the same user shader (written by
			// IShaderConstantSetCallBack::OnSetConstants() on every draw), so it isn't "static during
			// execution" in the sense of that flag.
			//
			// ONLY the pairs set in layoutKey get a parameter -- the whole point of building this per
			// material. A GS/HS/DS table now exists only for a material that HAS that stage AND
			// declares a cbuffer for it, instead of all ED3D12UCS_COUNT*MaxUserShaderRegisterSpaces
			// (20) existing unconditionally and being rebound on every draw.
			//
			// The ranges must outlive the D3D12SerializeVersionedRootSignature() call below (each root
			// param's pDescriptorRanges just points at one), hence the array living in this function's
			// scope rather than being built inline per stage.
			static const D3D12_SHADER_VISIBILITY kStageVisibility[ED3D12UCS_COUNT] = {
				D3D12_SHADER_VISIBILITY_VERTEX,   // ED3D12UCS_VERTEX
				D3D12_SHADER_VISIBILITY_PIXEL,    // ED3D12UCS_PIXEL
				D3D12_SHADER_VISIBILITY_GEOMETRY, // ED3D12UCS_GEOMETRY
				D3D12_SHADER_VISIBILITY_HULL,     // ED3D12UCS_HULL
				D3D12_SHADER_VISIBILITY_DOMAIN,   // ED3D12UCS_DOMAIN
			};

			D3D12_DESCRIPTOR_RANGE1 userCBVRanges[ED3D12UCS_COUNT * MaxUserShaderRegisterSpaces] = {};
			UINT paramCount = FirstUserCBVRootSlot;
			UINT rangeCount = 0;
			for (UINT stage = 0; stage < ED3D12UCS_COUNT; ++stage)
			{
				for (UINT space = 0; space < MaxUserShaderRegisterSpaces; ++space)
				{
					if (!(layoutKey & (1u << (stage * MaxUserShaderRegisterSpaces + space))))
						continue;

					D3D12_DESCRIPTOR_RANGE1& range = userCBVRanges[rangeCount++];
					range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
					range.NumDescriptors = MaxUserShaderCBVSlotsPerStage;
					range.BaseShaderRegister = 0;
					range.RegisterSpace = space;
					range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
					range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

					D3D12_ROOT_PARAMETER1& param = rootParams[paramCount++];
					param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
					param.DescriptorTable.NumDescriptorRanges = 1;
					param.DescriptorTable.pDescriptorRanges = &range;
					param.ShaderVisibility = kStageVisibility[stage];
				}
			}

			// CBV b3, space4 (DriverConstantRegisterSpace) -- dynamic lighting, same pattern as
			// rootParams[4] (ClipPlanes) above: single root descriptor visible on the VS side only
			// (see bindLighting()/CD3D12DefaultShaders.h).
			rootParams[LightingConstantSlot].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			rootParams[LightingConstantSlot].Descriptor.ShaderRegister = 3;
			rootParams[LightingConstantSlot].Descriptor.RegisterSpace = DriverConstantRegisterSpace;
			rootParams[LightingConstantSlot].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
			// ALL (not VERTEX alone) -- PSMainNormalMap*/PSMainParallaxMap* (CD3D12DefaultShaders.h)
			// now call calcLighting() from the pixel shader, see the comment above.
			rootParams[LightingConstantSlot].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			// CBV b4, space4 (DriverConstantRegisterSpace) -- fog, same pattern as rootParams[4]
			// (ClipPlanes) above: single root descriptor, but visible on the PS side (see bindFog()/
			// CD3D12DefaultShaders.h).
			rootParams[FogConstantSlot].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			rootParams[FogConstantSlot].Descriptor.ShaderRegister = 4;
			rootParams[FogConstantSlot].Descriptor.RegisterSpace = DriverConstantRegisterSpace;
			rootParams[FogConstantSlot].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
			rootParams[FogConstantSlot].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			const bool hasGS = (layoutKey & (1u << (UserCBVLayoutStageBit + ED3D12UCS_GEOMETRY))) != 0;
			const bool hasHS = (layoutKey & (1u << (UserCBVLayoutStageBit + ED3D12UCS_HULL))) != 0;
			const bool hasDS = (layoutKey & (1u << (UserCBVLayoutStageBit + ED3D12UCS_DOMAIN))) != 0;

			D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
			desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
			// paramCount, NOT _countof(rootParams): that array is only sized for the worst case
			// (MaxRootParameters), and the tail beyond paramCount is zeroed padding that
			// D3D12SerializeVersionedRootSignature would reject as invalid parameters.
			desc.Desc_1_1.NumParameters = paramCount;
			desc.Desc_1_1.pParameters = rootParams;
			desc.Desc_1_1.NumStaticSamplers = 0;
			desc.Desc_1_1.pStaticSamplers = nullptr;
			// DENY_*_SHADER_ROOT_ACCESS is now derived from the stage bits in layoutKey rather than
			// omitted wholesale: a material without a GS/HS/DS cannot read any root argument from that
			// stage by definition, and saying so lets the driver skip broadcasting root arguments to
			// it. Denying a stage is compatible with the ALL-visibility parameters above ([2]/[3]/[5]):
			// ALL means "every stage not denied". VS/PS are never denied -- every graphics material has
			// both (compileFromHLSL() requires them, compileBuiltIn() always produces them).
			//
			// ALLOW_STREAM_OUTPUT is required as soon as a PSO created through this root signature
			// declares a D3D12_STREAM_OUTPUT_DESC (see getPSOForMaterial()/hasStreamOutput) -- without
			// it, CreateGraphicsPipelineState simply and purely fails for any GS+stream-output material
			// ("Graphics pipeline state object uses stream-output, but the root signature does not have
			// the D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT flag set", found via the debug layer
			// while diagnosing VSGS_StreamOutput_ReadbackValidation) -- the entire draw is then
			// silently skipped (getOrCreate() returns nullptr, bindDrawState() returns false), not just
			// the stream-output. Gated on hasGS because hasStreamOutput is itself gated on a GS being
			// present; a signature without a GS can never back a stream-output PSO.
			D3D12_ROOT_SIGNATURE_FLAGS flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
			if (hasGS)
				flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
			else
				flags |= D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
			if (!hasHS)
				flags |= D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS;
			if (!hasDS)
				flags |= D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS;
			desc.Desc_1_1.Flags = flags;

			ComPtr<ID3DBlob> serialized, errors;
			HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &serialized, &errors);
			if (FAILED(hr))
			{
				if (errors)
					os::Printer::log("CD3D12Driver: D3D12SerializeVersionedRootSignature: ",
						static_cast<const char*>(errors->GetBufferPointer()), ELL_ERROR);
				else
					os::Printer::log("CD3D12Driver: D3D12SerializeVersionedRootSignature a echoue", ELL_ERROR);
				return nullptr;
			}

			ComPtr<ID3D12RootSignature> rootSignature;
			hr = Device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
				IID_PPV_ARGS(&rootSignature));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: CreateRootSignature a echoue", ELL_ERROR);
				return nullptr;
			}

			ID3D12RootSignature* raw = rootSignature.Get();
			RootSignatureCache[layoutKey] = std::move(rootSignature);
			return raw;
		}

		bool CD3D12Driver::buildMaterialRootSignature(CD3D12MaterialRenderer* renderer)
		{
			if (!renderer)
				return false;

			const bool stagePresent[ED3D12UCS_COUNT] = {
				renderer->VS != nullptr, // ED3D12UCS_VERTEX
				renderer->PS != nullptr, // ED3D12UCS_PIXEL
				renderer->GS != nullptr, // ED3D12UCS_GEOMETRY
				renderer->HS != nullptr, // ED3D12UCS_HULL
				renderer->DS != nullptr, // ED3D12UCS_DOMAIN
			};

			u32 layoutKey = 0;
			for (UINT stage = 0; stage < ED3D12UCS_COUNT; ++stage)
			{
				if (stagePresent[stage])
					layoutKey |= 1u << (UserCBVLayoutStageBit + stage);

				const std::vector<SD3D12UserShaderCBuffer>* buffers =
					renderer->getStageBuffers(static_cast<E_D3D12_USER_CBV_STAGE>(stage));
				if (!buffers)
					continue;

				for (const SD3D12UserShaderCBuffer& buffer : *buffers)
				{
					// reflectCBuffer() already rejects a cbuffer outside [0, MaxUserShaderRegisterSpaces)
					// with a clear error, so this is belt-and-braces -- but an out-of-range space here
					// would shift every subsequent bit and silently desynchronize the layout from
					// UserCBVTables below, so it is worth not trusting.
					if (buffer.Space >= MaxUserShaderRegisterSpaces)
						continue;
					layoutKey |= 1u << (stage * MaxUserShaderRegisterSpaces + buffer.Space);
				}
			}

			ID3D12RootSignature* rootSignature = getOrCreateRootSignature(layoutKey);
			if (!rootSignature)
			{
				os::Printer::log("CD3D12Driver::buildMaterialRootSignature: root signature indisponible"
					" pour ce materiau -- il ne pourra pas etre dessine", ELL_ERROR);
				return false;
			}
			renderer->RootSignature = rootSignature;

			// Same walk, same order as getOrCreateRootSignature() above -- this is what makes
			// UserCBVTables[i].RootSlot the index that parameter genuinely has in `rootSignature`.
			// Any change to the iteration order there must be mirrored here (and vice versa).
			renderer->UserCBVTables.clear();
			UINT rootSlot = FirstUserCBVRootSlot;
			for (UINT stage = 0; stage < ED3D12UCS_COUNT; ++stage)
			{
				for (UINT space = 0; space < MaxUserShaderRegisterSpaces; ++space)
				{
					if (!(layoutKey & (1u << (stage * MaxUserShaderRegisterSpaces + space))))
						continue;

					SD3D12UserCBVTable table;
					table.RootSlot = rootSlot++;
					table.Stage = static_cast<E_D3D12_USER_CBV_STAGE>(stage);
					table.Space = space;
					renderer->UserCBVTables.push_back(table);
				}
			}

			return true;
		}

		bool CD3D12Driver::createMipGenPipeline()
		{
			// Minimal root signature, distinct from RootSignature (phase 3) -- a full-screen
			// blit only needs its source texture (t0) and a linear/clamp sampler,
			// no World/View/Proj/clip planes (see D3D12MipGenShaderHLSL,
			// CD3D12DefaultShaders.h). The sampler is a static sampler embedded in the root
			// signature rather than a table (like t0 below): only one filter/addressing
			// combination is possible for this blit, no need for the SamplerCache/
			// ShaderVisibleSamplerHeap reserved for user SMaterialLayer entries.
			D3D12_DESCRIPTOR_RANGE1 srvRange = {};
			srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			srvRange.NumDescriptors = 1;
			srvRange.BaseShaderRegister = 0;
			srvRange.RegisterSpace = 0;
			srvRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
			srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

			D3D12_ROOT_PARAMETER1 rootParam = {};
			rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			rootParam.DescriptorTable.NumDescriptorRanges = 1;
			rootParam.DescriptorTable.pDescriptorRanges = &srvRange;
			rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_STATIC_SAMPLER_DESC staticSampler = {};
			staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			staticSampler.MaxLOD = D3D12_FLOAT32_MAX;
			staticSampler.ShaderRegister = 0;
			staticSampler.RegisterSpace = 0;
			staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
			desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
			desc.Desc_1_1.NumParameters = 1;
			desc.Desc_1_1.pParameters = &rootParam;
			desc.Desc_1_1.NumStaticSamplers = 1;
			desc.Desc_1_1.pStaticSamplers = &staticSampler;
			desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE; // no vertex buffer/input layout (SV_VertexID)

			ComPtr<ID3DBlob> serialized, errors;
			HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &serialized, &errors);
			if (FAILED(hr))
			{
				if (errors)
					os::Printer::log("CD3D12Driver: D3D12SerializeVersionedRootSignature (mipgen): ",
						static_cast<const char*>(errors->GetBufferPointer()), ELL_ERROR);
				else
					os::Printer::log("CD3D12Driver: D3D12SerializeVersionedRootSignature (mipgen) a echoue", ELL_ERROR);
				return false;
			}

			hr = Device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
				IID_PPV_ARGS(&MipGenRootSignature));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: CreateRootSignature (mipgen) a echoue", ELL_ERROR);
				return false;
			}

			UINT compileFlags = 0;
#ifdef _DEBUG
			compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
			ComPtr<ID3DBlob> compileErrors;
			hr = D3DCompile(D3D12MipGenShaderHLSL, strlen(D3D12MipGenShaderHLSL), "D3D12MipGenShaderHLSL",
				nullptr, nullptr, "VSMipGen", "vs_5_1", compileFlags, 0, &MipGenVS, &compileErrors);
			if (FAILED(hr))
			{
				if (compileErrors)
					os::Printer::log("CD3D12Driver: compilation VSMipGen: ",
						static_cast<const char*>(compileErrors->GetBufferPointer()), ELL_ERROR);
				return false;
			}

			compileErrors.Reset();
			hr = D3DCompile(D3D12MipGenShaderHLSL, strlen(D3D12MipGenShaderHLSL), "D3D12MipGenShaderHLSL",
				nullptr, nullptr, "PSMipGen", "ps_5_1", compileFlags, 0, &MipGenPS, &compileErrors);
			if (FAILED(hr))
			{
				if (compileErrors)
					os::Printer::log("CD3D12Driver: compilation PSMipGen: ",
						static_cast<const char*>(compileErrors->GetBufferPointer()), ELL_ERROR);
				return false;
			}

			// Dedicated shader-visible SRV heap, only 1 descriptor: CD3D12Texture::generateMips()
			// copies into it (CopyDescriptorsSimple) the transient SRV of the current source mip on each
			// iteration of its loop; the path is synchronous (beginUpload()/endUploadAndWait()), so
			// never two mip levels being generated at the same time -- no need for a
			// ring like ShaderVisibleSRVHeap (per frame, phase 5).
			D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
			heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			heapDesc.NumDescriptors = 1;
			heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			hr = Device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&MipGenSRVHeap));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: CreateDescriptorHeap (mipgen SRV) a echoue", ELL_ERROR);
				return false;
			}
			MipGenSRVHeapCPU = MipGenSRVHeap->GetCPUDescriptorHandleForHeapStart();
			MipGenSRVHeapGPU = MipGenSRVHeap->GetGPUDescriptorHandleForHeapStart();

			return true;
		}

		ID3D12PipelineState* CD3D12Driver::getOrCreateMipGenPSO(DXGI_FORMAT rtvFormat)
		{
			if (!MipGenVS || !MipGenPS || !MipGenRootSignature)
				return nullptr;

			// Minimal key: no blend/depth/stencil/input layout (see createMipGenPipeline()),
			// only RTVFormat actually varies from one texture to another -- fixed VSHash/PSHash
			// (same blobs for every format) guarantee that only one PSO per distinct
			// DXGI_FORMAT is created, all sharing MipGenRootSignature (never the material
			// pipeline's RootSignature, see PSOCache.getOrCreate() below).
			SPSOKey key;
			key.VSHash = std::hash<void*>()(MipGenVS.Get());
			key.PSHash = std::hash<void*>()(MipGenPS.Get());
			key.BlendMode = SPSOKey::EBlendMode::None;
			key.DepthTestEnable = false;
			key.DepthWriteEnable = false;
			key.CullMode = D3D12_CULL_MODE_NONE;
			key.FillMode = D3D12_FILL_MODE_SOLID;
			key.TopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			key.RTVFormats[0] = rtvFormat;
			key.DSVFormat = DXGI_FORMAT_UNKNOWN;
			key.InputLayoutHash = 0; // no input layout: VSMipGen only reads SV_VertexID
			// Same PSOCache is shared with the material pipeline, so the key must say which root
			// signature this PSO is built against -- see SPSOKey::RootSignatureHash. The fixed
			// VSHash/PSHash above already separate mipgen PSOs from every material PSO in practice,
			// but leaving this at 0 would make the key claim a root signature it does not use.
			key.RootSignatureHash = std::hash<void*>()(MipGenRootSignature.Get());

			return PSOCache.getOrCreate(Device.Get(), MipGenRootSignature.Get(), key,
				MipGenVS.Get(), MipGenPS.Get(), nullptr, 0);
		}

		bool CD3D12Driver::createComputeRootSignature()
		{
			// Milestone D: compute root signature, distinct from RootSignature (graphics) --
			// a compute shader has no VS/PS/blend/etc to share, just its input/output
			// resources and its user cbuffer.
			//   [0] Descriptor table, 1 SRV t0 -- Src buffer (read), see dispatchComputeShader().
			//   [1] Descriptor table, 1 UAV u0 -- Dst buffer (write).
			//   [2] Descriptor table, CBV b0..b7 in space0 (UserShaderRegisterSpace) -- user cbuffer(s),
			//       same MaxUserShaderCBVSlotsPerStage-wide table shape buildMaterialRootSignature()
			//       emits on the graphics side. Unlike graphics, this signature stays driver-wide and
			//       hardcodes space0: see the MaxUserShaderRegisterSpaces comment
			//       (CD3D12MaterialRenderer.h) on compute not being covered by the per-space handling.
			D3D12_ROOT_PARAMETER1 rootParams[3] = {};

			D3D12_DESCRIPTOR_RANGE1 srvRange = {};
			srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			srvRange.NumDescriptors = 1;
			srvRange.BaseShaderRegister = 0;
			srvRange.RegisterSpace = 0;
			srvRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
			srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

			rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
			rootParams[0].DescriptorTable.pDescriptorRanges = &srvRange;
			rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			D3D12_DESCRIPTOR_RANGE1 uavRange = {};
			uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
			uavRange.NumDescriptors = 1;
			uavRange.BaseShaderRegister = 0;
			uavRange.RegisterSpace = 0;
			uavRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
			uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

			rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
			rootParams[1].DescriptorTable.pDescriptorRanges = &uavRange;
			rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			D3D12_DESCRIPTOR_RANGE1 cbvRange = {};
			cbvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
			cbvRange.NumDescriptors = MaxUserShaderCBVSlotsPerStage;
			cbvRange.BaseShaderRegister = 0;
			cbvRange.RegisterSpace = UserShaderRegisterSpace;
			cbvRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
			cbvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

			rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
			rootParams[2].DescriptorTable.pDescriptorRanges = &cbvRange;
			rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

			D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
			desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
			desc.Desc_1_1.NumParameters = _countof(rootParams);
			desc.Desc_1_1.pParameters = rootParams;
			desc.Desc_1_1.NumStaticSamplers = 0;
			desc.Desc_1_1.pStaticSamplers = nullptr;
			desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE; // no input assembler/rasterizer for compute

			ComPtr<ID3DBlob> serialized, errors;
			HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &serialized, &errors);
			if (FAILED(hr))
			{
				if (errors)
					os::Printer::log("CD3D12Driver: D3D12SerializeVersionedRootSignature (compute): ",
						static_cast<const char*>(errors->GetBufferPointer()), ELL_ERROR);
				else
					os::Printer::log("CD3D12Driver: D3D12SerializeVersionedRootSignature (compute) a echoue", ELL_ERROR);
				return false;
			}

			hr = Device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
				IID_PPV_ARGS(&ComputeRootSignature));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: CreateRootSignature (compute) a echoue", ELL_ERROR);
				return false;
			}

			return true;
		}

		ID3D12PipelineState* CD3D12Driver::getOrCreateComputePSO(ID3DBlob* computeShader)
		{
			if (!computeShader || !ComputeRootSignature)
				return nullptr;

			size_t hash = std::hash<void*>()(computeShader);
			auto it = ComputePSOCache.find(hash);
			if (it != ComputePSOCache.end())
				return it->second.Get();

			D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
			desc.pRootSignature = ComputeRootSignature.Get();
			desc.CS = { computeShader->GetBufferPointer(), computeShader->GetBufferSize() };

			ComPtr<ID3D12PipelineState> pso;
			HRESULT hr = Device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver::getOrCreateComputePSO: CreateComputePipelineState a echoue", ELL_ERROR);
				return nullptr;
			}

			ID3D12PipelineState* raw = pso.Get();
			ComputePSOCache[hash] = std::move(pso);
			return raw;
		}

		// S3DVertex (Pos 12b + Normal 12b + Color 4b packed ARGB + TCoords 8b = 36b stride).
		// See the comment at the top of CD3D12DefaultShaders.h -- must stay in sync
		// with the layout expected by D3D12DefaultShaderHLSL.
		static const D3D12_INPUT_ELEMENT_DESC kS3DVertexInputLayout[4] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			// R8G8B8A8 and NOT B8G8R8A8, even though SColor (u32 0xAARRGGBB) is B,G,R,A in memory:
			// CD3D11VertexDescriptor::getFormat() returns R8G8B8A8_UNORM for any UBYTE*4 and doesn't
			// correct anything either. Engine shaders that care about the true color compensate
			// themselves with a ".bgra" (see IrrRocketRenderer's RmlUI shader) -- same reasoning
			// as for textures, see getD3D12ColorFormat() in CD3D12Texture.cpp.
			{ "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};

		//! Returns the CD3D12MaterialRenderer registered for this MaterialType, or nullptr
		//! if the index is out of bounds (stale/corrupt MaterialType) or if the entry exists but
		//! wasn't built by this driver (a "foreign" renderer registered via
		//! addMaterialRenderer(), see NativeRenderers).
		const CD3D12MaterialRenderer* CD3D12Driver::getNativeMaterialRenderer(const SMaterial& material) const
		{
			return getNativeRenderer(material.MaterialType);
		}

		ID3DBlob* CD3D12Driver::getSolidVertexShader() const
		{
			const CD3D12MaterialRenderer* solid = getNativeRenderer(EMT_SOLID);
			return solid ? solid->VS.Get() : nullptr;
		}

		ID3DBlob* CD3D12Driver::getSolidPixelShader() const
		{
			const CD3D12MaterialRenderer* solid = getNativeRenderer(EMT_SOLID);
			return solid ? solid->PS.Get() : nullptr;
		}

		ID3DBlob* CD3D12Driver::choosePixelShaderForMaterial(const SMaterial& material, IVertexDescriptor* descriptor) const
		{
			const CD3D12MaterialRenderer* renderer = getNativeMaterialRenderer(material);
			// EVT_2TCOORDS mesh AND a renderer that has a UV2 variant (the 12
			// multi-texture materials) -> Layer1Texture samples at its own UV (see
			// CD3D12DefaultShaders.h) rather than at the same UV as BaseTexture.
			if (renderer && renderer->PS2TCoords && descriptor && descriptor->getID() == EVT_2TCOORDS)
				return renderer->PS2TCoords.Get();
			// EMT_SOLID fallback: MaterialType never registered (registry not yet populated,
			// corrupt material) or "foreign" renderer (see above) -- draw solid opaque
			// rather than dereference a null ID3DBlob* in getOrCreate()/D3DCompile.
			return (renderer && renderer->PS) ? renderer->PS.Get() : getSolidPixelShader();
		}

		ID3DBlob* CD3D12Driver::chooseVertexShaderForMaterial(const SMaterial& material, IVertexDescriptor* descriptor) const
		{
			const CD3D12MaterialRenderer* renderer = getNativeMaterialRenderer(material);
			if (renderer && renderer->VS2TCoords && descriptor && descriptor->getID() == EVT_2TCOORDS)
				return renderer->VS2TCoords.Get();
			return (renderer && renderer->VS) ? renderer->VS.Get() : getSolidVertexShader();
		}

		D3D12_BLEND CD3D12Driver::getD3D12BlendFactor(E_BLEND_FACTOR factor, bool forAlphaChannel)
		{
			switch (factor)
			{
			case EBF_ZERO: return D3D12_BLEND_ZERO;
			case EBF_ONE: return D3D12_BLEND_ONE;
			case EBF_DST_COLOR: return forAlphaChannel ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
			case EBF_ONE_MINUS_DST_COLOR: return forAlphaChannel ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
			case EBF_SRC_COLOR: return forAlphaChannel ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
			case EBF_ONE_MINUS_SRC_COLOR: return forAlphaChannel ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
			case EBF_SRC_ALPHA: return D3D12_BLEND_SRC_ALPHA;
			case EBF_ONE_MINUS_SRC_ALPHA: return D3D12_BLEND_INV_SRC_ALPHA;
			case EBF_DST_ALPHA: return D3D12_BLEND_DEST_ALPHA;
			case EBF_ONE_MINUS_DST_ALPHA: return D3D12_BLEND_INV_DEST_ALPHA;
			case EBF_SRC_ALPHA_SATURATE: return D3D12_BLEND_SRC_ALPHA_SAT;
			default: return D3D12_BLEND_ONE;
			}
		}

		D3D12_COMPARISON_FUNC CD3D12Driver::getD3D12DepthFunc(E_COMPARISON_FUNC func)
		{
			switch (func)
			{
			case ECFN_LESSEQUAL: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
			case ECFN_EQUAL: return D3D12_COMPARISON_FUNC_EQUAL;
			case ECFN_LESS: return D3D12_COMPARISON_FUNC_LESS;
			case ECFN_NOTEQUAL: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
			case ECFN_GREATEREQUAL: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
			case ECFN_GREATER: return D3D12_COMPARISON_FUNC_GREATER;
			case ECFN_ALWAYS: return D3D12_COMPARISON_FUNC_ALWAYS;
			case ECFN_NEVER: return D3D12_COMPARISON_FUNC_NEVER;
			case ECFN_DISABLED: default: return D3D12_COMPARISON_FUNC_ALWAYS;
			}
		}

		SPSOKey CD3D12Driver::buildPSOKeyFromMaterial(const SMaterial& material, IVertexDescriptor* descriptor,
			scene::E_PRIMITIVE_TYPE primitiveType) const
		{
			SPSOKey key;
			key.VSHash = std::hash<void*>()(chooseVertexShaderForMaterial(material, descriptor));
			key.PSHash = std::hash<void*>()(choosePixelShaderForMaterial(material, descriptor));

			// resolveInputLayout() falls back to kS3DVertexInputLayout if descriptor is
			// null -- same result as before for all callers that don't pass a
			// descriptor (2D/immediate drawing, shadow volumes, occlusion queries).
			std::vector<D3D12_INPUT_ELEMENT_DESC> layoutStorage;
			const D3D12_INPUT_ELEMENT_DESC* layoutElements = nullptr;
			UINT layoutCount = 0;
			resolveInputLayout(descriptor, layoutStorage, layoutElements, layoutCount);
			key.InputLayoutHash = hashInputLayout(layoutElements, layoutCount);

			// "Base" blend read from the CD3D12MaterialRenderer registered for this
			// MaterialType (see createBuiltInMaterialRenderers() for the ~24 built-in types,
			// registerUserShaderMaterial() for user shaders) -- replaces the old
			// switch(material.MaterialType), same result for each type.
			const CD3D12MaterialRenderer* renderer = getNativeMaterialRenderer(material);
			if (renderer)
			{
				key.BlendMode = renderer->BlendMode;
				key.CustomSrcBlend = renderer->CustomSrcBlend;
				key.CustomDestBlend = renderer->CustomDestBlend;
				key.CustomSrcBlendAlpha = renderer->CustomSrcBlendAlpha;
				key.CustomDestBlendAlpha = renderer->CustomDestBlendAlpha;
				key.CustomBlendOp = renderer->CustomBlendOp;
				// Milestone B: two materials can only share a PSO if they have the same
				// GS (or none) AND the same stream-output mode (see SPSOKey::GSHash/StreamOutputHash).
				if (renderer->GS)
					key.GSHash = std::hash<void*>()(renderer->GS.Get());
				if (renderer->StreamOutputVertexType)
					key.StreamOutputHash = std::hash<void*>()(renderer->StreamOutputVertexType);
				// Milestone C: HS/DS only exist together (see CD3D12MaterialRenderer::HS/DS) --
				// their presence switches the topology to PATCH, the only value D3D12 accepts for
				// a PSO with HS/DS bound (see drawMeshBuffer(), which must then emit
				// D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST rather than triangle-list).
				// Require HS *and* DS: getPSOForMaterial() only passes non-null blobs to the PSO, so
				// an HS without a DS produced a PATCH topology without a complete tessellation stage, which
				// D3D12 validation rejects ("the input topology type is patch. You need either a
				// Hull Shader and Domain Shader, or a Geometry Shader") -> E_INVALIDARG, material
				// never drawn. Without both, keep a regular triangle topology.
				if (renderer->HS && renderer->DS)
				{
					key.HSHash = std::hash<void*>()(renderer->HS.Get());
					key.DSHash = std::hash<void*>()(renderer->DS.Get());
					key.TopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
				}
			}
			else
			{
				// MaterialType never registered, or "foreign" renderer (see
				// SD3D12MaterialRendererEntry::Native): opaque fallback, same as choosePixelShaderForMaterial().
				key.BlendMode = SPSOKey::EBlendMode::None;
			}

			// Must agree with the root signature getPSOForMaterial() actually passes to
			// CD3D12PSOCache::getOrCreate(), hence the shared rootSignatureForRenderer().
			key.RootSignatureHash = std::hash<void*>()(rootSignatureForRenderer(renderer));

			// EMT_ONETEXTURE_BLEND: "BlendFunc = source * sourceFactor + dest * destFactor"
			// (EMaterialTypes.h), factors encoded PER INSTANCE in SMaterial::MaterialTypeParam
			// via pack_textureBlendFunc() -- unlike the rest of the fields above, these can't
			// be "baked" once and for all into the registered CD3D12MaterialRenderer
			// (two SMaterial with the same EMT_ONETEXTURE_BLEND MaterialType can have different
			// factors), so always decoded here on every call, unchanged from before. This
			// driver only decodes the non-separate variant (same RGB/alpha factor, see
			// SPSOKey::CustomSrcBlend/CustomDestBlend) -- pack_textureBlendFuncSeparate() (distinct
			// RGB/alpha factors) is not handled here.
			// Also applies to a user shader whose BASE material is EMT_ONETEXTURE_BLEND
			// (see CD3D12MaterialRenderer::BaseMaterialType): its MaterialType is a user
			// index, not EMT_ONETEXTURE_BLEND, but its factors are still encoded per instance
			// in MaterialTypeParam exactly the same way.
			if (material.MaterialType == EMT_ONETEXTURE_BLEND ||
				(renderer && renderer->BaseMaterialType == EMT_ONETEXTURE_BLEND))
			{
				E_BLEND_FACTOR srcFact, dstFact;
				E_MODULATE_FUNC modulate;
				u32 alphaSource;
				unpack_textureBlendFunc(srcFact, dstFact, modulate, alphaSource, material.MaterialTypeParam);
				key.BlendMode = SPSOKey::EBlendMode::Custom;
				key.CustomSrcBlend = getD3D12BlendFactor(srcFact, false);
				key.CustomDestBlend = getD3D12BlendFactor(dstFact, false);
				key.CustomSrcBlendAlpha = getD3D12BlendFactor(srcFact, true);
				key.CustomDestBlendAlpha = getD3D12BlendFactor(dstFact, true);
				// modulate (1x/2x/4x) and alphaSource (texture/vertex/both) are not
				// applied: PSMain stays texColor*vertexColor (equivalent to MODULATE_1X,
				// alphaSource implicitly "both") regardless of what's encoded here -- fix
				// if real content ever depends on MODULATE_2X/4X or a different alpha
				// source.
			}

			// The 12 single-UV multi-texture types (EMT_SOLID_2_LAYER, EMT_LIGHTMAP*,
			// EMT_DETAIL_MAP, EMT_SPHERE_MAP, EMT_REFLECTION_2_LAYER,
			// EMT_TRANSPARENT_REFLECTION_2_LAYER) now have their own PS/BlendMode (see
			// createBuiltInMaterialRenderers(), CD3D12DefaultShaders.h) -- no longer a "solid" fallback.
			// The 6 normal map/parallax map types (EVT_TANGENTS) also have their own
			// VS (VSMainTangents)/PS/BlendMode now (see createBuiltInMaterialRenderers()).
			// The 12 types above now sample Layer1Texture at a dedicated UV
			// (TEXCOORD1, distinct from BaseTexture) when the drawn mesh uses the
			// "2tcoords" descriptor (EVT_2TCOORDS) -- see choosePixelShaderForMaterial()/
			// chooseVertexShaderForMaterial() (VS2TCoords/PS2TCoords) and the comment at the top of
			// CD3D12DefaultShaders.h. BlendMode/CustomSrcBlend/etc. above stay the same for
			// both variants (only the PS/VS changes, not the PSO blend state).

			// SMaterial::BlendOperation/BlendFactor, generic and independent of MaterialType --
			// a mechanism that COpenGLDriver/CD3D9Driver/CD3D8Driver apply in their
			// setBasicRenderStates(), and that CD3D11Driver itself NEVER reads (verified: neither
			// "BlendOperation" nor "BlendFactor" appear in CD3D11*.cpp).
			//
			// BlendFactor != 0 is essential, not just a safety guard: EMF_BLEND_OPERATION
			// only sets BlendOperation = EBO_ADD (SMaterial::setFlag()) without ever touching
			// BlendFactor, which therefore stays 0 until the caller has explicitly packed
			// factors (pack_textureBlendFunc[Separate]()). But unpack_textureBlendFuncSeparate(0)
			// returns EBF_ZERO EVERYWHERE: the blend becomes src*0 + dst*0, i.e. BLACK. A node
			// that merely calls setMaterialFlag(EMF_BLEND_OPERATION, true) -- a flag inert under
			// D3D11, hence set freely all over typical engine content (lens-flare-style
			// billboards in particular) -- would end up rendered entirely black, in the
			// process overwriting the correct blend inherited from its base E_MATERIAL_TYPE (see
			// registerUserShaderMaterial(), which already includes the lens flares' additive blend).
			//
			// Without real factors, nothing is touched and the base blend above is kept.
			if (material.BlendOperation != EBO_NONE && material.BlendFactor != 0.0f)
			{
				E_BLEND_FACTOR srcRGBFact, dstRGBFact, srcAlphaFact, dstAlphaFact;
				E_MODULATE_FUNC modulate;
				u32 alphaSource;
				unpack_textureBlendFuncSeparate(srcRGBFact, dstRGBFact, srcAlphaFact, dstAlphaFact,
					modulate, alphaSource, material.BlendFactor);

				// E_BLEND_OPERATION -> D3D12_BLEND_OP. EBO_MIN_FACTOR/MAX_FACTOR/MIN_ALPHA/MAX_ALPHA
				// have no direct D3D12 equivalent (D3D12_BLEND_OP only has ADD/SUBTRACT/
				// REV_SUBTRACT/MIN/MAX, "not widely supported" even per SMaterial.h) -- approximated
				// as MIN/MAX the same way this file already does elsewhere for other edge cases
				// (see the comment on simultaneous BackfaceCulling+FrontfaceCulling further below).
				static const D3D12_BLEND_OP kBlendOpMap[] =
				{
					D3D12_BLEND_OP_ADD,          // EBO_NONE (never reached, kept here for indexing)
					D3D12_BLEND_OP_ADD,          // EBO_ADD
					D3D12_BLEND_OP_SUBTRACT,     // EBO_SUBTRACT
					D3D12_BLEND_OP_REV_SUBTRACT, // EBO_REVSUBTRACT
					D3D12_BLEND_OP_MIN,          // EBO_MIN
					D3D12_BLEND_OP_MAX,          // EBO_MAX
					D3D12_BLEND_OP_MIN,          // EBO_MIN_FACTOR (approx.)
					D3D12_BLEND_OP_MAX,          // EBO_MAX_FACTOR (approx.)
					D3D12_BLEND_OP_MIN,          // EBO_MIN_ALPHA (approx.)
					D3D12_BLEND_OP_MAX,          // EBO_MAX_ALPHA (approx.)
				};

				key.BlendMode = SPSOKey::EBlendMode::Custom;
				key.CustomBlendOp = (material.BlendOperation >= 0 &&
					material.BlendOperation < static_cast<s32>(_countof(kBlendOpMap))) ?
					kBlendOpMap[material.BlendOperation] : D3D12_BLEND_OP_ADD;
				key.CustomSrcBlend = getD3D12BlendFactor(srcRGBFact, false);
				key.CustomDestBlend = getD3D12BlendFactor(dstRGBFact, false);
				key.CustomSrcBlendAlpha = getD3D12BlendFactor(srcAlphaFact, true);
				key.CustomDestBlendAlpha = getD3D12BlendFactor(dstAlphaFact, true);
			}

			// DepthEnable/StencilEnable set to TRUE with DSVFormat==UNKNOWN (see below) is
			// rejected by D3D12 (CreateGraphicsPipelineState) -- a material that requests the depth
			// test must fall back to no test at all if the current scene has no DSV bound,
			// not just have its DSVFormat set to UNKNOWN independently.
			key.DepthTestEnable = CurrentSceneHasDepthStencil && (material.ZBuffer != ECFN_DISABLED);
			key.DepthWriteEnable = material.ZWriteEnable && key.DepthTestEnable;
			key.DepthFunc = getD3D12DepthFunc(static_cast<E_COMPARISON_FUNC>(material.ZBuffer));

			key.CullMode = material.BackfaceCulling ?
				(material.FrontfaceCulling ? D3D12_CULL_MODE_FRONT /* both: approximation, see note */ : D3D12_CULL_MODE_BACK) :
				(material.FrontfaceCulling ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_NONE);
			// Note: SMaterial allows BackfaceCulling AND FrontfaceCulling simultaneously
			// (both faces culled, nothing drawn). D3D12_CULL_MODE only has
			// NONE/FRONT/BACK, no "both". Approximated as FRONT in this case rather than
			// adding a "draw nothing" path at the PSO level -- a rare edge case, to be
			// fixed if it turns out to matter in practice.

			key.FillMode = material.Wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;

			// SMaterial::ColorMask, same mapping as CD3D11Driver::setBasicRenderStates(): the
			// ECP_* bits (SMaterial.h) don't numerically correspond to the D3D12_COLOR_WRITE_ENABLE_*
			// bits, an explicit mapping is required.
			key.RenderTargetWriteMask =
				((material.ColorMask & ECP_RED)   ? D3D12_COLOR_WRITE_ENABLE_RED   : 0) |
				((material.ColorMask & ECP_GREEN) ? D3D12_COLOR_WRITE_ENABLE_GREEN : 0) |
				((material.ColorMask & ECP_BLUE)  ? D3D12_COLOR_WRITE_ENABLE_BLUE  : 0) |
				((material.ColorMask & ECP_ALPHA) ? D3D12_COLOR_WRITE_ENABLE_ALPHA : 0);

			// SMaterial::PolygonOffsetFactor/PolygonOffsetDirection, same sign mapping as
			// CD3D11Driver::setBasicRenderStates() (EPO_BACK: positive bias; EPO_FRONT: negative bias).
			key.DepthBias = 0;
			key.SlopeScaledDepthBias = 0.0f;
			if (material.PolygonOffsetFactor)
			{
				if (material.PolygonOffsetDirection == EPO_BACK)
				{
					key.SlopeScaledDepthBias = 1.f;
					key.DepthBias = material.PolygonOffsetFactor;
				}
				else
				{
					key.SlopeScaledDepthBias = -1.f;
					key.DepthBias = -static_cast<INT>(material.PolygonOffsetFactor);
				}
			}
			// Milestone C: don't overwrite the PATCH set above when renderer->HS is present -- a
			// PSO with HS/DS bound only accepts PATCH regardless of the caller's native
			// topology (drawMeshBuffer() then converts each triangle to a 3-point patch, see
			// its own comment). Otherwise: the PSO must have the same
			// D3D12_PRIMITIVE_TOPOLOGY_TYPE as the one actually submitted to IASetPrimitiveTopology --
			// a GS that declares a point/line input primitive (see CD3D12MaterialRenderer::GS,
			// stream-output) requires a PSO of the same type, otherwise it doesn't execute.
			if (key.TopologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH)
			{
				// Must stay the exact counterpart of mapMeshPrimitiveTypeToTopology(): D3D12 requires
				// the PSO's D3D12_PRIMITIVE_TOPOLOGY_TYPE to match the topology submitted to
				// IASetPrimitiveTopology(), otherwise the draw is rejected.
				switch (primitiveType)
				{
				case scene::EPT_POINT_SPRITES:
				case scene::EPT_POINTS:
					key.TopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
					break;
				case scene::EPT_LINES:
				case scene::EPT_LINE_LOOP:
				case scene::EPT_LINE_STRIP:
					key.TopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
					break;
				case scene::EPT_CONTROL_POINT:
					key.TopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
					break;
				default:
					key.TopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
					break;
				}
				// SMaterial::PointCloud: neither D3D11 nor D3D12 have an equivalent to D3D8/D3D9's
				// D3DFILL_POINT (rasterizing only the vertices of a triangle topology as points,
				// without touching the index/vertex buffers) -- D3D12_FILL_MODE only has WIREFRAME/SOLID.
				// Approximated here by switching the topology to POINT (drawMeshBuffer() then submits
				// D3D_PRIMITIVE_TOPOLOGY_POINTLIST), which gives an equivalent render for a
				// non-shared, non-indexed mesh (each vertex drawn as a point) even though the number of points
				// emitted differs from native D3DFILL_POINT on an indexed mesh sharing vertices.
				if (material.PointCloud)
					key.TopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
			}
			// NumRenderTargets/RTVFormats reflect the target(s) actually bound by
			// the last setRenderTarget() (see CurrentRTVCount/CurrentRTVFormats, updated by
			// setRenderTarget()/beginScene()) rather than a hardcoded DXGI_FORMAT_R8G8B8A8_UNORM
			// -- necessary for the PSO to exactly match what OMSetRenderTargets() has
			// actually bound (D3D12 requires NumRenderTargets/RTVFormats[] to be identical between the PSO
			// and the current binding), and therefore for MRT (2+ targets) to work at all.
			key.NumRenderTargets = CurrentRTVCount;
			for (UINT i = 0; i < CurrentRTVCount && i < 8; ++i)
				key.RTVFormats[i] = CurrentRTVFormats[i];
			// A PSO whose DSVFormat != UNKNOWN requires a DSV actually bound to the draw (see
			// CurrentSceneHasDepthStencil, updated by beginScene()) -- otherwise D3D12 rejects the
			// DrawIndexedInstanced/DrawInstanced ("A null depth stencil view may only be bound when
			// the pipeline state depth stencil format is UNKNOWN", found via the debug layer while
			// diagnosing VSGS_StreamOutput_ReadbackValidation, which calls
			// beginScene(backBuffer, false) -- no zBuffer for a pure stream-output pass).
			// CurrentDSVFormat, not DepthStencilFormat: the bound depth target isn't
			// necessarily the driver's depth buffer (see CurrentDSVFormat).
			key.DSVFormat = CurrentSceneHasDepthStencil ? CurrentDSVFormat : DXGI_FORMAT_UNKNOWN;
			// See the CurrentRTVSampleCount comment -- same reasoning as
			// NumRenderTargets/RTVFormats just above, D3D12 requires the PSO's SampleDesc.Count
			// to exactly match that of the actually bound target(s).
			key.SampleCount = CurrentRTVSampleCount;

			return key;
		}

		ID3D12PipelineState* CD3D12Driver::getPSOForMaterial(const SMaterial& material, IVertexDescriptor* descriptor,
			scene::E_PRIMITIVE_TYPE primitiveType)
		{
			SPSOKey key = buildPSOKeyFromMaterial(material, descriptor, primitiveType);
			const CD3D12MaterialRenderer* renderer = getNativeMaterialRenderer(material);
			ID3DBlob* gs = (renderer && renderer->GS) ? renderer->GS.Get() : nullptr;
			ID3DBlob* hs = (renderer && renderer->HS) ? renderer->HS.Get() : nullptr;
			ID3DBlob* ds = (renderer && renderer->DS) ? renderer->DS.Get() : nullptr;

			// Milestone B: a stream-output GS (StreamOutputVertexType set) adds the
			// SO description to the PSO; built on every call rather than cached on the
			// renderer -- less lifetime to track, and getOrCreate() is already
			// behind a per-SPSOKey cache anyway (see its header comment).
			std::vector<D3D12_SO_DECLARATION_ENTRY> soEntries;
			std::vector<UINT> soStrides;
			bool hasStreamOutput = gs && renderer && renderer->StreamOutputVertexType &&
				buildStreamOutputDeclaration(renderer->StreamOutputVertexType, soEntries, soStrides);

			// Same resolution as buildPSOKeyFromMaterial() above (kS3DVertexInputLayout
			// if descriptor is null) -- must stay the same layout as the one already hashed into key,
			// hence the shared resolveInputLayout() rather than two independent fallback paths.
			std::vector<D3D12_INPUT_ELEMENT_DESC> layoutStorage;
			const D3D12_INPUT_ELEMENT_DESC* layoutElements = nullptr;
			UINT layoutCount = 0;
			resolveInputLayout(descriptor, layoutStorage, layoutElements, layoutCount);

			// This material's OWN root signature (see CD3D12MaterialRenderer::RootSignature). Same
			// rootSignatureForRenderer() buildPSOKeyFromMaterial() hashed into key.RootSignatureHash
			// just above -- the cache would otherwise hand back a PSO built against another layout.
			return PSOCache.getOrCreate(Device.Get(), rootSignatureForRenderer(renderer), key,
				chooseVertexShaderForMaterial(material, descriptor), choosePixelShaderForMaterial(material, descriptor),
				layoutElements, layoutCount,
				gs,
				hasStreamOutput ? soEntries.data() : nullptr, hasStreamOutput ? static_cast<UINT>(soEntries.size()) : 0,
				hasStreamOutput ? soStrides.data() : nullptr, hasStreamOutput ? static_cast<UINT>(soStrides.size()) : 0,
				hasStreamOutput,
				hs, ds);
		}

		namespace
		{
			// Milestone B: same table as CD3D11VertexDescriptor::getSemanticName() --
			// shared between buildStreamOutputDeclaration() (SO) and buildInputLayoutDescription()
			// (input layout) below, since D3D12_SO_DECLARATION_ENTRY::SemanticName and
			// D3D12_INPUT_ELEMENT_DESC::SemanticName use the same HLSL naming convention.
			const char* getStreamOutputSemanticName(E_VERTEX_ATTRIBUTE_SEMANTIC semantic)
			{
				switch (semantic)
				{
				case EVAS_POSITION: return "POSITION";
				case EVAS_NORMAL: return "NORMAL";
				case EVAS_COLOR: return "COLOR";
				case EVAS_TEXCOORD0: case EVAS_TEXCOORD1: case EVAS_TEXCOORD2: case EVAS_TEXCOORD3:
				case EVAS_TEXCOORD4: case EVAS_TEXCOORD5: case EVAS_TEXCOORD6: case EVAS_TEXCOORD7:
				case EVAS_TEXCOORD8: case EVAS_TEXCOORD9: case EVAS_TEXCOORD10: case EVAS_TEXCOORD11:
				case EVAS_TEXCOORD12: case EVAS_TEXCOORD13: case EVAS_TEXCOORD14: case EVAS_TEXCOORD15:
					return "TEXCOORD";
				case EVAS_TANGENT: return "TANGENT";
				case EVAS_BINORMAL: return "BINORMAL";
				case EVAS_BLEND_WEIGHTS: return "BLENDWEIGHT";
				case EVAS_BLEND_INDICES: return "BLENDINDICES";
				case EVAS_CUSTOM: return "CUSTOM";
				default: return "POSITION";
				}
			}

			// Same table as CD3D11VertexDescriptor::getFormat() -- DXGI_FORMAT is
			// shared between D3D11 and D3D12 (dxgiformat.h), so the type/component-count
			// -> format mapping is identical on the D3D12 side.
			DXGI_FORMAT getD3D12VertexAttributeFormat(E_VERTEX_ATTRIBUTE_TYPE type, u32 count)
			{
				switch (type)
				{
				case EVAT_BYTE:
					switch (count)
					{
					case 1: return DXGI_FORMAT_R8_SNORM;
					case 2: return DXGI_FORMAT_R8G8_SNORM;
					case 4: return DXGI_FORMAT_R8G8B8A8_SNORM;
					default: return DXGI_FORMAT_UNKNOWN;
					}
				case EVAT_UBYTE:
					switch (count)
					{
					case 1: return DXGI_FORMAT_R8_UNORM;
					case 2: return DXGI_FORMAT_R8G8_UNORM;
					case 4: return DXGI_FORMAT_R8G8B8A8_UNORM;
					default: return DXGI_FORMAT_UNKNOWN;
					}
				case EVAT_SHORT:
					switch (count)
					{
					case 1: return DXGI_FORMAT_R16_SINT;
					case 2: return DXGI_FORMAT_R16G16_SINT;
					case 4: return DXGI_FORMAT_R16G16B16A16_SINT;
					default: return DXGI_FORMAT_UNKNOWN;
					}
				case EVAT_USHORT:
					switch (count)
					{
					case 1: return DXGI_FORMAT_R16_UINT;
					case 2: return DXGI_FORMAT_R16G16_UINT;
					case 4: return DXGI_FORMAT_R16G16B16A16_UINT;
					default: return DXGI_FORMAT_UNKNOWN;
					}
				case EVAT_INT:
					switch (count)
					{
					case 1: return DXGI_FORMAT_R32_SINT;
					case 2: return DXGI_FORMAT_R32G32_SINT;
					case 3: return DXGI_FORMAT_R32G32B32_SINT;
					case 4: return DXGI_FORMAT_R32G32B32A32_SINT;
					default: return DXGI_FORMAT_UNKNOWN;
					}
				case EVAT_UINT:
					switch (count)
					{
					case 1: return DXGI_FORMAT_R32_UINT;
					case 2: return DXGI_FORMAT_R32G32_UINT;
					case 3: return DXGI_FORMAT_R32G32B32_UINT;
					case 4: return DXGI_FORMAT_R32G32B32A32_UINT;
					default: return DXGI_FORMAT_UNKNOWN;
					}
				case EVAT_DOUBLE:
				case EVAT_FLOAT:
					switch (count)
					{
					case 1: return DXGI_FORMAT_R32_FLOAT;
					case 2: return DXGI_FORMAT_R32G32_FLOAT;
					case 3: return DXGI_FORMAT_R32G32B32_FLOAT;
					case 4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
					default: return DXGI_FORMAT_UNKNOWN;
					}
				default:
					return DXGI_FORMAT_UNKNOWN;
				}
			}
		}

		bool CD3D12Driver::buildStreamOutputDeclaration(IVertexDescriptor* descriptor,
			std::vector<D3D12_SO_DECLARATION_ENTRY>& outEntries, std::vector<UINT>& outStrides)
		{
			outEntries.clear();
			outStrides.clear();
			if (!descriptor)
				return false;

			u32 localIndex[EVAS_COUNT] = {};
			u32 maxBufferID = 0;
			const u32 count = descriptor->getAttributeCount();
			for (u32 i = 0; i < count; ++i)
			{
				IVertexAttribute* attr = descriptor->getAttribute(i);
				if (!attr)
					continue;

				D3D12_SO_DECLARATION_ENTRY entry = {};
				entry.Stream = 0;
				entry.SemanticName = getStreamOutputSemanticName(attr->getSemantic());

				E_VERTEX_ATTRIBUTE_SEMANTIC semantic = attr->getSemantic();
				entry.SemanticIndex = (semantic >= EVAS_TEXCOORD0 && semantic <= EVAS_TEXCOORD15) ?
					localIndex[EVAS_TEXCOORD0]++ : localIndex[semantic]++;
				entry.StartComponent = 0;
				// Number of 32-bit scalars captured -- computed from the attribute's byte
				// size rather than its raw element count, same reasoning as
				// CD3D11VertexDescriptor::rebuildOutput() (a packed format like EVAT_UBYTE*4
				// fits in 1 single component, not 4).
				u32 byteCount = attr->getTypeSize() * attr->getElementCount();
				entry.ComponentCount = static_cast<BYTE>(byteCount / 4 > 0 ? byteCount / 4 : 1);
				entry.OutputSlot = static_cast<BYTE>(attr->getBufferID());

				outEntries.push_back(entry);
				if (attr->getBufferID() > maxBufferID)
					maxBufferID = attr->getBufferID();
			}

			if (outEntries.empty())
				return false;

			outStrides.assign(static_cast<size_t>(maxBufferID) + 1, 0);
			for (UINT slot = 0; slot <= maxBufferID; ++slot)
				outStrides[slot] = descriptor->getVertexSize(slot);

			return true;
		}

		// D3D12 equivalent of CD3D11VertexDescriptor::rebuild(), but generic
		// (IVertexAttribute) rather than on a dedicated D3D12 subclass -- same reasoning as
		// buildStreamOutputDeclaration() above (D3D12_INPUT_ELEMENT_DESC[] is just a
		// description consumed by CreateGraphicsPipelineState(), not a persistent object like
		// ID3D11InputLayout, so nothing to cache on the descriptor itself -- rebuilt
		// on every potentially new PSO, already behind CD3D12PSOCache's per-SPSOKey
		// cache). AlignedByteOffset uses D3D12_APPEND_ALIGNED_ELEMENT (as
		// CD3D11VertexDescriptor::rebuild() uses the D3D11 equivalent): assumes the
		// attributes are registered in the same order as the vertex's members in memory
		// (verified for standard/2tcoords/tangents/standardcolorf against S3DVertex/
		// S3DVertex2TCoords/S3DVertexTangents, see createVertexDescriptors()).
		bool CD3D12Driver::buildInputLayoutDescription(IVertexDescriptor* descriptor,
			std::vector<D3D12_INPUT_ELEMENT_DESC>& outElements)
		{
			outElements.clear();
			if (!descriptor)
				return false;

			u32 localIndex[EVAS_COUNT] = {};
			const u32 count = descriptor->getAttributeCount();
			for (u32 i = 0; i < count; ++i)
			{
				IVertexAttribute* attr = descriptor->getAttribute(i);
				if (!attr)
					continue;

				D3D12_INPUT_ELEMENT_DESC desc = {};
				desc.SemanticName = getStreamOutputSemanticName(attr->getSemantic());

				E_VERTEX_ATTRIBUTE_SEMANTIC semantic = attr->getSemantic();
				desc.SemanticIndex = (semantic >= EVAS_TEXCOORD0 && semantic <= EVAS_TEXCOORD15) ?
					localIndex[EVAS_TEXCOORD0]++ : localIndex[semantic]++;
				// No B8G8R8A8 "correction" for EVAS_COLOR: getD3D12VertexAttributeFormat() returns
				// R8G8B8A8_UNORM for a UBYTE*4, exactly like CD3D11VertexDescriptor::getFormat(),
				// and that's the (shifted) convention all of the engine's content is written against
				// -- the engine's shaders carry a ".bgra" on the vertex color to compensate
				// (see CD3D12DefaultShaders.h and IrrRocketRenderer's RmlUI shader), never on
				// textures. See also kS3DVertexInputLayout and getD3D12ColorFormat() (CD3D12Texture.cpp).
				desc.Format = getD3D12VertexAttributeFormat(attr->getType(), attr->getElementCount());
				desc.InputSlot = attr->getBufferID();
				desc.AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;

				if (descriptor->getInstanceDataStepRate(attr->getBufferID()) == EIDSR_PER_VERTEX)
				{
					desc.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
					desc.InstanceDataStepRate = 0;
				}
				else
				{
					desc.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
					desc.InstanceDataStepRate = 1;
				}

				outElements.push_back(desc);
			}

			return !outElements.empty();
		}

		// Single point of resolution for "which input layout for this draw" -- used by
		// buildPSOKeyFromMaterial() (to hash the right layout into the key) and getPSOForMaterial()
		// (to build the PSO with); both must resolve to the same result for the same
		// descriptor, hence this shared helper rather than two copies of the same fallback logic.
		// A null descriptor (any caller without a scene::IMeshBuffer on hand: 2D/
		// immediate drawing, shadow volumes, occlusion queries -- see their respective comments) falls back
		// to kS3DVertexInputLayout, same behavior as before.
		void CD3D12Driver::resolveInputLayout(IVertexDescriptor* descriptor,
			std::vector<D3D12_INPUT_ELEMENT_DESC>& storage,
			const D3D12_INPUT_ELEMENT_DESC*& outElements, UINT& outCount) const
		{
			if (descriptor && buildInputLayoutDescription(descriptor, storage))
			{
				outElements = storage.data();
				outCount = static_cast<UINT>(storage.size());
			}
			else
			{
				outElements = kS3DVertexInputLayout;
				outCount = _countof(kS3DVertexInputLayout);
			}
		}

		// ================================ Phase 5: drawing ================================

		bool CD3D12Driver::createFrameDrawResources()
		{
			CBVSRVUAVDescriptorSize = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

			D3D12_HEAP_PROPERTIES uploadHeapProps = {};
			uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

			D3D12_RESOURCE_DESC ringDesc = {};
			ringDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			ringDesc.Width = ConstantRingSizePerFrame;
			ringDesc.Height = 1;
			ringDesc.DepthOrArraySize = 1;
			ringDesc.MipLevels = 1;
			ringDesc.Format = DXGI_FORMAT_UNKNOWN;
			ringDesc.SampleDesc = { 1, 0 };
			ringDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ringDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

			for (auto& frame : Frames)
			{
				HRESULT hr = Device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &ringDesc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&frame.ConstantRing));
				if (FAILED(hr))
				{
					os::Printer::log("CD3D12Driver: CreateCommittedResource (anneau de constantes) a echoue", ELL_ERROR);
					return false;
				}

				D3D12_RANGE noRead = { 0, 0 };
				hr = frame.ConstantRing->Map(0, &noRead, &frame.ConstantRingMapped);
				if (FAILED(hr))
				{
					os::Printer::log("CD3D12Driver: Map (anneau de constantes) a echoue", ELL_ERROR);
					return false;
				}

				D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
				srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
				srvHeapDesc.NumDescriptors = ShaderVisibleSRVCapacityPerFrame;
				srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
				hr = Device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&frame.ShaderVisibleSRVHeap));
				if (FAILED(hr))
				{
					os::Printer::log("CD3D12Driver: CreateDescriptorHeap (SRV shader-visible) a echoue", ELL_ERROR);
					return false;
				}
				frame.ShaderVisibleSRVHeapStartCPU = frame.ShaderVisibleSRVHeap->GetCPUDescriptorHandleForHeapStart();
				frame.ShaderVisibleSRVHeapStartGPU = frame.ShaderVisibleSRVHeap->GetGPUDescriptorHandleForHeapStart();

				D3D12_RESOURCE_DESC vbRingDesc = ringDesc;
				vbRingDesc.Width = VertexRingSizePerFrame;
				hr = Device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &vbRingDesc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&frame.VertexRing));
				if (FAILED(hr))
				{
					os::Printer::log("CD3D12Driver: CreateCommittedResource (anneau de vertex) a echoue", ELL_ERROR);
					return false;
				}
				hr = frame.VertexRing->Map(0, &noRead, &frame.VertexRingMapped);
				if (FAILED(hr))
				{
					os::Printer::log("CD3D12Driver: Map (anneau de vertex) a echoue", ELL_ERROR);
					return false;
				}
			}

			// PERSISTENT shader-visible sampler heap (a single one, not per frame -- see the
			// header comment on its declaration in CD3D12Driver.h).
			SamplerDescriptorSize = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
			D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc = {};
			samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
			samplerHeapDesc.NumDescriptors = ShaderVisibleSamplerCapacity;
			samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			HRESULT samplerHr = Device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&ShaderVisibleSamplerHeap));
			if (FAILED(samplerHr))
			{
				os::Printer::log("CD3D12Driver: CreateDescriptorHeap (sampler shader-visible) a echoue", ELL_ERROR);
				return false;
			}
			ShaderVisibleSamplerHeapStartCPU = ShaderVisibleSamplerHeap->GetCPUDescriptorHandleForHeapStart();
			ShaderVisibleSamplerHeapStartGPU = ShaderVisibleSamplerHeap->GetGPUDescriptorHandleForHeapStart();

			return createOcclusionQueryResources();
		}

		bool CD3D12Driver::createOcclusionQueryResources()
		{
			D3D12_QUERY_HEAP_DESC heapDesc = {};
			heapDesc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION; // covers OCCLUSION and BINARY_OCCLUSION
			heapDesc.Count = OcclusionQueryCapacity;
			HRESULT hr = Device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&OcclusionQueryHeap));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: CreateQueryHeap (occlusion) a echoue", ELL_ERROR);
				return false;
			}

			D3D12_HEAP_PROPERTIES readbackHeapProps = {};
			readbackHeapProps.Type = D3D12_HEAP_TYPE_READBACK;

			D3D12_RESOURCE_DESC readbackDesc = {};
			readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			readbackDesc.Width = static_cast<UINT64>(OcclusionQueryCapacity) * sizeof(UINT64);
			readbackDesc.Height = 1;
			readbackDesc.DepthOrArraySize = 1;
			readbackDesc.MipLevels = 1;
			readbackDesc.Format = DXGI_FORMAT_UNKNOWN;
			readbackDesc.SampleDesc = { 1, 0 };
			readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

			hr = Device->CreateCommittedResource(&readbackHeapProps, D3D12_HEAP_FLAG_NONE, &readbackDesc,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&OcclusionReadback));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: CreateCommittedResource (occlusion readback) a echoue", ELL_ERROR);
				return false;
			}

			// Mapped for the entire lifetime of the driver: only read after confirming (via Fence)
			// that the ResolveQueryData which produced each slot has actually been executed by the GPU --
			// see updateOcclusionQuery(). No restricted read D3D12_RANGE here because
			// the valid interval changes dynamically per slot.
			hr = OcclusionReadback->Map(0, nullptr, &OcclusionReadbackMapped);
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: Map (occlusion readback) a echoue", ELL_ERROR);
				return false;
			}

			FreeOcclusionSlots.clear();
			FreeOcclusionSlots.reserve(OcclusionQueryCapacity);
			for (UINT i = 0; i < OcclusionQueryCapacity; ++i)
				FreeOcclusionSlots.push_back(OcclusionQueryCapacity - 1 - i);

			return true;
		}

		bool CD3D12Driver::createNullTexture()
		{
			u32 whitePixel = 0xffffffff;
			CD3D12Texture* texture = new CD3D12Texture(ResourceOwner, core::dimension2d<u32>(1, 1), "NullTexture", ECF_A8R8G8B8, false);
			if (!texture->hasDeviceResource() || !texture->hasShaderResourceView())
			{
				texture->drop();
				return false;
			}

			void* mapped = texture->lock(ETLM_WRITE_ONLY);
			if (!mapped)
			{
				texture->drop();
				return false;
			}
			memcpy(mapped, &whitePixel, sizeof(whitePixel));
			texture->unlock();

			// CNullDriver's cache takes the reference; NullTexture is just a shortcut to
			// the object it owns (see its declaration).
			CNullDriver::addTexture(texture);
			texture->drop();
			NullTexture = texture;
			return true;
		}

		bool CD3D12Driver::createStreamOutputResources()
		{
			// StreamOutputCounterUpload: upload heap, mapped once for its entire lifetime
			// (same scheme as ConstantRing/VertexRing) -- permanently holds a UINT64 = 0,
			// the source for the reset-to-zero copied in resetStreamOutputCounter().
			D3D12_HEAP_PROPERTIES uploadHeapProps = {};
			uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

			D3D12_RESOURCE_DESC counterDesc = {};
			counterDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			counterDesc.Width = sizeof(UINT64);
			counterDesc.Height = 1;
			counterDesc.DepthOrArraySize = 1;
			counterDesc.MipLevels = 1;
			counterDesc.Format = DXGI_FORMAT_UNKNOWN;
			counterDesc.SampleDesc = { 1, 0 };
			counterDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

			HRESULT hr = Device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE,
				&counterDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&StreamOutputCounterUpload));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: CreateCommittedResource (StreamOutputCounterUpload) a echoue", ELL_ERROR);
				return false;
			}

			void* mapped = nullptr;
			D3D12_RANGE noRead = { 0, 0 };
			hr = StreamOutputCounterUpload->Map(0, &noRead, &mapped);
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: Map (StreamOutputCounterUpload) a echoue", ELL_ERROR);
				return false;
			}
			const UINT64 zero = 0;
			memcpy(mapped, &zero, sizeof(zero));
			StreamOutputCounterUpload->Unmap(0, nullptr); // coherent by construction, its content never varies afterward

			// StreamOutputCounter: default heap (VRAM), the only heap type that supports
			// transitioning to D3D12_RESOURCE_STATE_STREAM_OUT -- its initial content doesn't matter
			// (resetStreamOutputCounter() resets it to 0 before every real use), no need
			// for the beginUpload()/endUploadAndWait() path here.
			D3D12_HEAP_PROPERTIES defaultHeapProps = {};
			defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

			// Chosen resting state = STREAM_OUT (the one resetStreamOutputCounter()/
			// setStreamOutputBuffer() leave it in once bound) to avoid a needless transition
			// on the very first call.
			hr = Device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE,
				&counterDesc, D3D12_RESOURCE_STATE_STREAM_OUT, nullptr, IID_PPV_ARGS(&StreamOutputCounter));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: CreateCommittedResource (StreamOutputCounter) a echoue", ELL_ERROR);
				return false;
			}
			StreamOutputCounterState = D3D12_RESOURCE_STATE_STREAM_OUT;

			return true;
		}

		void CD3D12Driver::resetStreamOutputCounter()
		{
			if (!StreamOutputCounter || !StreamOutputCounterUpload || !CommandList)
				return;

			if (StreamOutputCounterState != D3D12_RESOURCE_STATE_COPY_DEST)
			{
				CD3DX12_RESOURCE_BARRIER toCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(
					StreamOutputCounter.Get(), StreamOutputCounterState, D3D12_RESOURCE_STATE_COPY_DEST);
				CommandList->ResourceBarrier(1, &toCopyDest);
				StreamOutputCounterState = D3D12_RESOURCE_STATE_COPY_DEST;
			}

			CommandList->CopyBufferRegion(StreamOutputCounter.Get(), 0, StreamOutputCounterUpload.Get(), 0, sizeof(UINT64));

			CD3DX12_RESOURCE_BARRIER toStreamOut = CD3DX12_RESOURCE_BARRIER::Transition(
				StreamOutputCounter.Get(), StreamOutputCounterState, D3D12_RESOURCE_STATE_STREAM_OUT);
			CommandList->ResourceBarrier(1, &toStreamOut);
			StreamOutputCounterState = D3D12_RESOURCE_STATE_STREAM_OUT;
		}

		bool CD3D12Driver::setStreamOutputBuffer(scene::IVertexBuffer* buffer)
		{
			// Detach/re-transition the previous SO target first (if any), whether we're
			// replacing buffer with another one or purely detaching (buffer == nullptr) -- in
			// both cases it must become readable again as a normal vertex buffer so a later
			// draw can read back what the GS wrote to it (the whole point of stream-output).
			// CommandList is closed until the first beginScene() (see initDriver()): a caller may
			// legally call setStreamOutputBuffer() before beginScene() (see
			// VSGS_StreamOutput_ReadbackValidation) -- recording onto a closed command list here
			// would crash, so only touch CommandList while a scene is actually open. beginScene()
			// already re-establishes SOSetTargets/the STREAM_OUT transition for
			// CurrentStreamOutputBuffer after its Reset() (see its own comment on that), so
			// skipping these calls here just defers them to that resync.
			if (CurrentStreamOutputBuffer)
			{
				if (SceneOpen)
					CurrentStreamOutputBuffer->transitionTo(CommandList.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
				CurrentStreamOutputBuffer = nullptr;
			}

			if (!buffer)
			{
				if (SceneOpen)
					CommandList->SOSetTargets(0, 0, nullptr);
				return true;
			}

			// Parity with CD3D11Driver::setStreamOutputBuffer() -- the mapping hint
			// (EHM_STATIC/EHM_DYNAMIC/...) no longer determines stream-output eligibility, only
			// the buffer type matters (scene::EBT_STREAM -> EHBT_STREAM_OUTPUT, see
			// CD3D12HardwareBuffer(scene::IVertexBuffer*, ...) and ConvertBufferType() on the D3D11 side).
			// It's the type that routes this buffer to the default-heap/GPU-writable path required
			// to transition to D3D12_RESOURCE_STATE_STREAM_OUT -- EHM_DYNAMIC remains the
			// natural hint for a buffer the GPU writes and the CPU later reads back
			// (downloadFromGPU()), EHM_STATIC is no longer forced here.
			auto hwBuff = buffer->getHardwareBuffer();
			if (!hwBuff)
			{
				createHardwareBuffer(buffer);
				hwBuff = buffer->getHardwareBuffer();
			}
			if (!hwBuff || hwBuff->getDriverType() != EDT_DIRECT3D12)
			{
				os::Printer::log("CD3D12Driver::setStreamOutputBuffer: buffer nul ou pas cree par ce driver", ELL_ERROR);
				return false;
			}
			if (hwBuff->getType() != EHBT_STREAM_OUTPUT)
			{
				os::Printer::log("CD3D12Driver::setStreamOutputBuffer: le buffer cible doit avoir ete "
					"declare avec setBufferType(scene::EBT_STREAM) avant sa premiere creation de "
					"ressource GPU (voir IVertexBuffer::setBufferType() / CD3D12HardwareBuffer)", ELL_ERROR);
				return false;
			}

			CD3D12HardwareBuffer* nativeBuffer = static_cast<CD3D12HardwareBuffer*>(hwBuff.get());
			if (SceneOpen)
			{
				nativeBuffer->transitionTo(CommandList.Get(), D3D12_RESOURCE_STATE_STREAM_OUT);

				resetStreamOutputCounter();

				D3D12_STREAM_OUTPUT_BUFFER_VIEW soView = {};
				soView.BufferLocation = nativeBuffer->getResource()->GetGPUVirtualAddress();
				soView.SizeInBytes = nativeBuffer->getResource()->GetDesc().Width;
				soView.BufferFilledSizeLocation = StreamOutputCounter->GetGPUVirtualAddress();

				CommandList->SOSetTargets(0, 1, &soView);
			}
			CurrentStreamOutputBuffer = nativeBuffer;
			return true;
		}

		// All the functions below follow the same pattern: create a bigger resource, put it in
		// place (Map()/GetXxxDescriptorHandleForHeapStart()), then retireResource() the old one
		// instead of destroying it immediately -- GPU handles already recorded into the current
		// command list for earlier allocations in THIS frame still point into the old resource,
		// kept alive until the GPU has actually moved past it (see retireResource()'s own
		// comment). No content is copied: these resources are linear allocators reset every
		// frame (except the persistent sampler heap, see growShaderVisibleSamplerHeap()), so
		// there's nothing to preserve beyond what the GPU is already reading from the old one.

		void CD3D12Driver::rebindShaderVisibleHeaps()
		{
			// A heap can grow while no scene is open (dispatchComputeShader() reserves/allocates
			// through this same path from its own UploadScope, see its comment) -- CommandList is
			// closed until the first beginScene() (see initDriver()), so rebinding onto it here
			// would crash. Target whichever command list is actually open and recording:
			// CommandList during a scene, UploadCommandList during an UploadScope (UploadInProgress
			// is true for exactly its duration, see beginUpload()/endUploadAndWait()).
			ID3D12GraphicsCommandList* target = SceneOpen ? CommandList.Get() :
				(UploadInProgress ? UploadCommandList.Get() : nullptr);
			if (!target)
				return;

			SD3D12FrameContext& frame = Frames[CurrentFrameIndex];
			ID3D12DescriptorHeap* shaderVisibleHeaps[] = { frame.ShaderVisibleSRVHeap.Get(), ShaderVisibleSamplerHeap.Get() };
			target->SetDescriptorHeaps(2, shaderVisibleHeaps);
		}

		bool CD3D12Driver::growConstantRing(SD3D12FrameContext& frame, UINT64 minCapacity)
		{
			// minCapacity is the size of the single allocation that just didn't fit, not the
			// frame's total consumption (the ring is a linear allocator whose offset restarts at
			// zero in the new resource, see below). The doubling must therefore be
			// UNCONDITIONAL: looping only "while newCapacity < minCapacity" left the capacity
			// unchanged whenever the requested allocation was smaller than the ring, so a
			// same-size block got recreated on every saturation. On a dense scene (a
			// CTerrainSceneNode up close) that meant hundreds of 1 MB CreateCommittedResource
			// calls per frame, all kept alive by retireResource() until the GPU passed the frame
			// -- memory exploded and CreateCommittedResource eventually failed. Doubling
			// converges the capacity to what the scene actually needs within a few frames, after
			// which no further growth happens.
			UINT64 newCapacity = frame.ConstantRingCapacity ? frame.ConstantRingCapacity : ConstantRingSizePerFrame;
			newCapacity *= 2;
			while (newCapacity < minCapacity)
				newCapacity *= 2;

			D3D12_HEAP_PROPERTIES uploadHeapProps = {};
			uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

			D3D12_RESOURCE_DESC ringDesc = {};
			ringDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			ringDesc.Width = newCapacity;
			ringDesc.Height = 1;
			ringDesc.DepthOrArraySize = 1;
			ringDesc.MipLevels = 1;
			ringDesc.Format = DXGI_FORMAT_UNKNOWN;
			ringDesc.SampleDesc = { 1, 0 };
			ringDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ringDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

			ComPtr<ID3D12Resource> newRing;
			HRESULT hr = Device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &ringDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&newRing));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: growing the constant ring failed"
					" (CreateCommittedResource)", ELL_ERROR);
				return false;
			}

			void* newMapped = nullptr;
			D3D12_RANGE noRead = { 0, 0 };
			hr = newRing->Map(0, &noRead, &newMapped);
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: growing the constant ring failed (Map)", ELL_ERROR);
				return false;
			}

			retireResource(std::move(frame.ConstantRing));
			frame.ConstantRing = std::move(newRing);
			frame.ConstantRingMapped = newMapped;
			frame.ConstantRingCapacity = newCapacity;
			frame.ConstantRingOffset = 0;

			os::Printer::log("CD3D12Driver: constant ring grown for this frame", ELL_INFORMATION);
			return true;
		}

		bool CD3D12Driver::growVertexRing(SD3D12FrameContext& frame, UINT64 minCapacity)
		{
			// Unconditional doubling, same reason as growConstantRing() -- allocateVertices() also
			// passes the size of the single allocation that just failed, not the frame's total.
			UINT64 newCapacity = frame.VertexRingCapacity ? frame.VertexRingCapacity : VertexRingSizePerFrame;
			newCapacity *= 2;
			while (newCapacity < minCapacity)
				newCapacity *= 2;

			D3D12_HEAP_PROPERTIES uploadHeapProps = {};
			uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

			D3D12_RESOURCE_DESC ringDesc = {};
			ringDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			ringDesc.Width = newCapacity;
			ringDesc.Height = 1;
			ringDesc.DepthOrArraySize = 1;
			ringDesc.MipLevels = 1;
			ringDesc.Format = DXGI_FORMAT_UNKNOWN;
			ringDesc.SampleDesc = { 1, 0 };
			ringDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ringDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

			ComPtr<ID3D12Resource> newRing;
			HRESULT hr = Device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &ringDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&newRing));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: growing the vertex ring failed"
					" (CreateCommittedResource)", ELL_ERROR);
				return false;
			}

			void* newMapped = nullptr;
			D3D12_RANGE noRead = { 0, 0 };
			hr = newRing->Map(0, &noRead, &newMapped);
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: growing the vertex ring failed (Map)", ELL_ERROR);
				return false;
			}

			retireResource(std::move(frame.VertexRing));
			frame.VertexRing = std::move(newRing);
			frame.VertexRingMapped = newMapped;
			frame.VertexRingCapacity = newCapacity;
			frame.VertexRingOffset = 0;

			os::Printer::log("CD3D12Driver: vertex ring grown for this frame", ELL_INFORMATION);
			return true;
		}

		bool CD3D12Driver::growShaderVisibleSRVHeap(SD3D12FrameContext& frame, UINT minCapacity)
		{
			UINT newCapacity = frame.ShaderVisibleSRVCapacity ? frame.ShaderVisibleSRVCapacity : ShaderVisibleSRVCapacityPerFrame;
			while (newCapacity < minCapacity)
				newCapacity *= 2;

			D3D12_DESCRIPTOR_HEAP_DESC desc = {};
			desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			desc.NumDescriptors = newCapacity;
			desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

			ComPtr<ID3D12DescriptorHeap> newHeap;
			HRESULT hr = Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&newHeap));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: growing the shader-visible SRV heap failed", ELL_ERROR);
				return false;
			}

			retireResource(std::move(frame.ShaderVisibleSRVHeap));
			frame.ShaderVisibleSRVHeap = std::move(newHeap);
			frame.ShaderVisibleSRVHeapStartCPU = frame.ShaderVisibleSRVHeap->GetCPUDescriptorHandleForHeapStart();
			frame.ShaderVisibleSRVHeapStartGPU = frame.ShaderVisibleSRVHeap->GetGPUDescriptorHandleForHeapStart();
			frame.ShaderVisibleSRVCapacity = newCapacity;
			frame.ShaderVisibleSRVNext = 0;

			// The new heap must be bound to the current command list so that subsequent
			// SetGraphicsRootDescriptorTable() calls (within this same frame) resolve their GPU
			// handles into it -- see rebindShaderVisibleHeaps().
			rebindShaderVisibleHeaps();

			os::Printer::log("CD3D12Driver: shader-visible SRV heap grown for this frame", ELL_INFORMATION);
			return true;
		}

		bool CD3D12Driver::reserveShaderVisibleSRVDescriptors(SD3D12FrameContext& frame, UINT count)
		{
			if (frame.ShaderVisibleSRVNext + count <= frame.ShaderVisibleSRVCapacity)
				return true;
			return growShaderVisibleSRVHeap(frame, frame.ShaderVisibleSRVNext + count);
		}

		bool CD3D12Driver::growShaderVisibleSamplerHeap(UINT minCapacity)
		{
			UINT newCapacity = ShaderVisibleSamplerHeapCapacity ? ShaderVisibleSamplerHeapCapacity : ShaderVisibleSamplerCapacity;
			while (newCapacity < minCapacity)
				newCapacity *= 2;

			D3D12_DESCRIPTOR_HEAP_DESC desc = {};
			desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
			desc.NumDescriptors = newCapacity;
			desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

			ComPtr<ID3D12DescriptorHeap> newHeap;
			HRESULT hr = Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&newHeap));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver: growing the shader-visible sampler heap failed", ELL_ERROR);
				return false;
			}

			D3D12_CPU_DESCRIPTOR_HANDLE newStartCPU = newHeap->GetCPUDescriptorHandleForHeapStart();
			// Unlike ConstantRing/ShaderVisibleSRVHeap, this heap is PERSISTENT (not reset every
			// frame): SamplerCombinationCache references its existing entries by slot INDEX,
			// valid across frames -- so they must be copied into the new heap to keep those
			// indices valid, rather than starting over from scratch.
			if (ShaderVisibleSamplerCount > 0)
			{
				Device->CopyDescriptorsSimple(ShaderVisibleSamplerCount, newStartCPU, ShaderVisibleSamplerHeapStartCPU,
					D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
			}

			retireResource(std::move(ShaderVisibleSamplerHeap));
			ShaderVisibleSamplerHeap = std::move(newHeap);
			ShaderVisibleSamplerHeapStartCPU = newStartCPU;
			ShaderVisibleSamplerHeapStartGPU = ShaderVisibleSamplerHeap->GetGPUDescriptorHandleForHeapStart();
			ShaderVisibleSamplerHeapCapacity = newCapacity;
			// ShaderVisibleSamplerCount is NOT reset: the slots already assigned remain valid
			// (just copied above) and SamplerCombinationCache keeps referencing them by the same
			// indices.

			rebindShaderVisibleHeaps();

			os::Printer::log("CD3D12Driver: shader-visible sampler heap grown", ELL_INFORMATION);
			return true;
		}

		D3D12_GPU_VIRTUAL_ADDRESS CD3D12Driver::allocateConstant(const void* data, size_t sizeBytes)
		{
			SD3D12FrameContext& frame = Frames[CurrentFrameIndex];

			const UINT64 alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT; // 256 bytes
			const UINT64 alignedSize = (static_cast<UINT64>(sizeBytes) + alignment - 1) & ~(alignment - 1);

			if (frame.ConstantRingOffset + alignedSize > frame.ConstantRingCapacity)
			{
				if (!growConstantRing(frame, alignedSize))
				{
					os::Printer::log("CD3D12Driver: constant ring full for this frame and growing"
						" it failed (see ConstantRingSizePerFrame)", ELL_WARNING);
					return 0;
				}
			}

			memcpy(static_cast<u8*>(frame.ConstantRingMapped) + frame.ConstantRingOffset, data, sizeBytes);
			D3D12_GPU_VIRTUAL_ADDRESS address = frame.ConstantRing->GetGPUVirtualAddress() + frame.ConstantRingOffset;
			frame.ConstantRingOffset += alignedSize;
			return address;
		}

		D3D12_GPU_DESCRIPTOR_HANDLE CD3D12Driver::allocateSRVTableSlot(const std::array<CD3D12Texture*, MaxUserShaderTextureSlots>& texturesIn)
		{
			SD3D12FrameContext& frame = Frames[CurrentFrameIndex];

			// Any null/SRV-less register falls back to NullTexture -- the normal case for any
			// register beyond the number of layers actually used by the material (see
			// SMaterial::TextureLayer[]) whose shader doesn't even declare/read that register, see
			// CD3D12DefaultShaders.h -- the table must still stay valid (MaxUserShaderTextureSlots
			// descriptors) regardless.
			std::array<CD3D12Texture*, MaxUserShaderTextureSlots> textures = texturesIn;
			for (CD3D12Texture*& tex : textures)
			{
				if (!tex || !tex->hasShaderResourceView())
					tex = static_cast<CD3D12Texture*>(NullTexture);
			}

			D3D12_GPU_DESCRIPTOR_HANDLE nullHandle = {};
			if (!textures[0] || !textures[0]->hasShaderResourceView())
			{
				os::Printer::log("CD3D12Driver: pas de texture a binder et NullTexture indisponible", ELL_WARNING);
				return nullHandle;
			}
			if (frame.ShaderVisibleSRVNext + MaxUserShaderTextureSlots > frame.ShaderVisibleSRVCapacity)
			{
				if (!growShaderVisibleSRVHeap(frame, frame.ShaderVisibleSRVNext + MaxUserShaderTextureSlots))
				{
					os::Printer::log("CD3D12Driver: heap SRV shader-visible plein pour cette frame et son"
						" agrandissement a echoue (voir ShaderVisibleSRVCapacityPerFrame)", ELL_WARNING);
					return nullHandle;
				}
			}

			// The MaxUserShaderTextureSlots (9) registers must land in CONTIGUOUS
			// slots -- the root signature's table covers all of them together (see
			// createRootSignature()).
			UINT slot = frame.ShaderVisibleSRVNext;
			frame.ShaderVisibleSRVNext += MaxUserShaderTextureSlots;
			D3D12_GPU_DESCRIPTOR_HANDLE destGPU = frame.ShaderVisibleSRVHeapStartGPU;
			destGPU.ptr += static_cast<UINT64>(slot) * CBVSRVUAVDescriptorSize;

			for (UINT i = 0; i < MaxUserShaderTextureSlots; ++i)
			{
				// An MSAA render-target-texture carries its SRV on ResolvedResource (see
				// CD3D12Texture::createResolveResource()), never up to date until resolveIfNeeded()
				// has copied the multisampled content into it since the last time it was used
				// as a render target -- done here, just before the SRV descriptor (already
				// valid, it has pointed to ResolvedResource since creation) is copied into this
				// frame's shader-visible heap. No-op for any non-MSAA texture (SampleCount == 1).
				textures[i]->resolveIfNeeded(CommandList.Get());

				// A texture that just served as a render target (e.g. prevLum/finalLum
				// ping-ponging in a post-process) stays in the RENDER_TARGET state until something
				// explicitly transitions it: reading its SRV without this barrier violates D3D12
				// rules and produces undefined content. transitionTo() is a no-op if the
				// current state already matches.
				// The shared SRV table is ShaderVisibility ALL (see createRootSignature() --
				// needed for vertex texture fetch, e.g. terrain heightmaps):
				// PIXEL_SHADER_RESOURCE alone only covers PS-side access. Without the
				// NON_PIXEL_SHADER_RESOURCE bit, a VS reading this same texture does so from a
				// layout that doesn't allow it -- undetected by the standard debug layer, but
				// flagged by GPU-Based Validation (GPU_BASED_VALIDATION_INCOMPATIBLE_TEXTURE_LAYOUT)
				// and a source of DEVICE_REMOVED/DRIVER_INTERNAL_ERROR on some drivers.
				transitionTexture(textures[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
					D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

				D3D12_CPU_DESCRIPTOR_HANDLE destCPU = frame.ShaderVisibleSRVHeapStartCPU;
				destCPU.ptr += static_cast<SIZE_T>(slot + i) * CBVSRVUAVDescriptorSize;
				Device->CopyDescriptorsSimple(1, destCPU, textures[i]->getShaderResourceView(),
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			}

			return destGPU;
		}

		D3D12_GPU_DESCRIPTOR_HANDLE CD3D12Driver::allocateUserCBVTable(const std::vector<SD3D12UserShaderCBuffer>& buffers, UINT space)
		{
			D3D12_GPU_DESCRIPTOR_HANDLE nullHandle = {};

			// An empty `buffers` is NOT a case to bail out on: the loop below then produces a
			// table of 8 null CBVs, perfectly valid (same as for a register not declared by the
			// shader, see its comment). Returning a null handle here made bindDrawState() skip
			// the corresponding SetGraphicsRootDescriptorTable() call, and the root argument
			// then kept the PREVIOUS draw's handle -- see the comment over there.

			SD3D12FrameContext& frame = Frames[CurrentFrameIndex];
			if (frame.ShaderVisibleSRVNext + MaxUserShaderCBVSlotsPerStage > frame.ShaderVisibleSRVCapacity)
			{
				if (!growShaderVisibleSRVHeap(frame, frame.ShaderVisibleSRVNext + MaxUserShaderCBVSlotsPerStage))
				{
					os::Printer::log("CD3D12Driver: heap CBV/SRV shader-visible plein pour cette frame et son"
						" agrandissement a echoue (voir ShaderVisibleSRVCapacityPerFrame)", ELL_WARNING);
					return nullHandle;
				}
			}

			UINT baseSlot = frame.ShaderVisibleSRVNext;
			frame.ShaderVisibleSRVNext += MaxUserShaderCBVSlotsPerStage;

			D3D12_GPU_DESCRIPTOR_HANDLE tableStart = frame.ShaderVisibleSRVHeapStartGPU;
			tableStart.ptr += static_cast<UINT64>(baseSlot) * CBVSRVUAVDescriptorSize;

			for (UINT slot = 0; slot < MaxUserShaderCBVSlotsPerStage; ++slot)
			{
				D3D12_CPU_DESCRIPTOR_HANDLE destCPU = frame.ShaderVisibleSRVHeapStartCPU;
				destCPU.ptr += static_cast<SIZE_T>(baseSlot + slot) * CBVSRVUAVDescriptorSize;

				const SD3D12UserShaderCBuffer* match = nullptr;
				for (const SD3D12UserShaderCBuffer& buf : buffers)
				{
					if (buf.BindPoint == slot && buf.Space == space)
					{
						match = &buf;
						break;
					}
				}

				D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
				if (match && !match->Scratch.empty())
				{
					D3D12_GPU_VIRTUAL_ADDRESS va = allocateConstant(match->Scratch.data(), match->Scratch.size());
					const UINT64 alignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
					cbvDesc.BufferLocation = va;
					cbvDesc.SizeInBytes = static_cast<UINT>((match->Scratch.size() + alignment - 1) & ~(alignment - 1));
				}
				// match == nullptr (register not declared by this shader): cbvDesc stays {0, 0},
				// a valid null CBV (see D3D12_CONSTANT_BUFFER_VIEW_DESC) -- the entire table must
				// stay valid even when the shader only reads a subset of the 8 registers.
				Device->CreateConstantBufferView(&cbvDesc, destCPU);
			}

			return tableStart;
		}

		D3D12_GPU_DESCRIPTOR_HANDLE CD3D12Driver::allocateDescriptorTableSlot(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle)
		{
			SD3D12FrameContext& frame = Frames[CurrentFrameIndex];
			D3D12_GPU_DESCRIPTOR_HANDLE nullHandle = {};
			if (frame.ShaderVisibleSRVNext >= ShaderVisibleSRVCapacityPerFrame)
			{
				os::Printer::log("CD3D12Driver: heap CBV/SRV/UAV shader-visible plein pour cette frame"
					" (voir ShaderVisibleSRVCapacityPerFrame)", ELL_WARNING);
				return nullHandle;
			}

			UINT slot = frame.ShaderVisibleSRVNext++;
			D3D12_CPU_DESCRIPTOR_HANDLE destCPU = frame.ShaderVisibleSRVHeapStartCPU;
			destCPU.ptr += static_cast<SIZE_T>(slot) * CBVSRVUAVDescriptorSize;
			D3D12_GPU_DESCRIPTOR_HANDLE destGPU = frame.ShaderVisibleSRVHeapStartGPU;
			destGPU.ptr += static_cast<UINT64>(slot) * CBVSRVUAVDescriptorSize;

			Device->CopyDescriptorsSimple(1, destCPU, cpuHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			return destGPU;
		}

		// Identical to CD3D11Driver::getTextureWrapMode() (CD3D11Driver.cpp) -- the
		// D3D11_TEXTURE_ADDRESS_MODE/D3D12_TEXTURE_ADDRESS_MODE values are numerically
		// identical, only the type name changes between the two APIs.
		D3D12_TEXTURE_ADDRESS_MODE CD3D12Driver::getD3D12TextureWrapMode(u8 clamp)
		{
			switch (clamp)
			{
			case ETC_REPEAT:
				return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			case ETC_CLAMP:
			case ETC_CLAMP_TO_EDGE:
				return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			case ETC_MIRROR:
				return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
			case ETC_CLAMP_TO_BORDER:
				return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
			case ETC_MIRROR_CLAMP:
			case ETC_MIRROR_CLAMP_TO_EDGE:
			case ETC_MIRROR_CLAMP_TO_BORDER:
				return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
			default:
				return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			}
		}

		//! Builds the D3D12_SAMPLER_DESC for layer + its 64-bit dedup key -- factored
		//! out of allocateSamplerTableSlot() since it's now needed twice (layer0
		//! for s0, layer1 for s1), see its comment in CD3D12Driver.h.
		//! useMipMaps (SMaterial::UseMipMaps, not an SMaterialLayer field -- see its caller):
		//! when false, clamps MaxLOD to 0 to force mip 0 regardless of the LOD computed by the
		//! hardware, same semantics as D3DTEXF_NONE on D3D8/D3D9 (D3D11 doesn't read it either,
		//! same debt this driver had before this change).
		static D3D12_SAMPLER_DESC buildD3D12SamplerDesc(const SMaterialLayer& layer, D3D12_TEXTURE_ADDRESS_MODE wrapU,
			D3D12_TEXTURE_ADDRESS_MODE wrapV, bool useMipMaps, UINT64& outKey)
		{
			D3D12_SAMPLER_DESC desc = {};
			desc.AddressU = wrapU;
			desc.AddressV = wrapV;
			desc.AddressW = desc.AddressU; // ETT_2D only (see CD3D12Texture.h): the W axis is never sampled.
			desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
			desc.MinLOD = 0.0f;
			desc.MaxLOD = useMipMaps ? D3D12_FLOAT32_MAX : 0.0f;
			desc.MipLODBias = layer.LODBias * 0.125f; // same factor as CD3D11Driver::setBasicRenderStates()

			// Same priority order as CD3D11Driver::setBasicRenderStates() (CD3D11CallBridge.h):
			// bilinear > trilinear > anisotropic. The default SMaterialLayer (AnisotropicFilter=16,
			// Bilinear/Trilinear=false) therefore falls back to ANISOTROPIC -- consistent with CD3D11Driver.
			if (layer.BilinearFilter)
			{
				desc.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
				desc.MaxAnisotropy = 1;
			}
			else if (layer.TrilinearFilter)
			{
				desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
				desc.MaxAnisotropy = 1;
			}
			else if (layer.AnisotropicFilter)
			{
				desc.Filter = D3D12_FILTER_ANISOTROPIC;
				desc.MaxAnisotropy = layer.AnisotropicFilter;
			}
			else
			{
				desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
				desc.MaxAnisotropy = 1;
			}

			// Dedup key: the only fields that vary here (Filter/AddressU/AddressV/
			// MaxAnisotropy/truncated MipLODBias/useMipMaps) fit in 64 bits -- no need to hash
			// the whole struct like CD3D11's SamplerMap (core::map<SD3D11_SAMPLER_DESC, ...>). Bit
			// 48 (beyond Filter's range, which occupies bit 32 and above) for useMipMaps.
			outKey = (static_cast<UINT64>(desc.Filter) << 32) |
				(static_cast<UINT64>(desc.AddressU) << 24) |
				(static_cast<UINT64>(desc.AddressV) << 16) |
				(static_cast<UINT64>(desc.MaxAnisotropy) << 8) |
				(useMipMaps ? (UINT64(1) << 48) : 0) |
				static_cast<UINT64>(static_cast<u8>(layer.LODBias));
			return desc;
		}

		D3D12_GPU_DESCRIPTOR_HANDLE CD3D12Driver::allocateSamplerTableSlot(const std::array<SMaterialLayer, MaxUserShaderTextureSlots>& layers, bool useMipMaps)
		{
			std::array<D3D12_SAMPLER_DESC, MaxUserShaderTextureSlots> descs;
			std::array<UINT64, MaxUserShaderTextureSlots> keys;
			for (UINT i = 0; i < MaxUserShaderTextureSlots; ++i)
			{
				descs[i] = buildD3D12SamplerDesc(layers[i],
					getD3D12TextureWrapMode(layers[i].TextureWrapU), getD3D12TextureWrapMode(layers[i].TextureWrapV), useMipMaps, keys[i]);
			}

			D3D12_GPU_DESCRIPTOR_HANDLE nullHandle = {};
			// Deduplicates the COMBINATION of the MaxUserShaderTextureSlots (9) layers, not
			// each layer independently -- the sampler table covers CONTIGUOUS s0..s8 (see
			// createRootSignature()), so all slots must be reserved/created together,
			// as a single cache unit (std::array has a lexicographic operator<, usable
			// directly as a std::map key).
			auto found = SamplerCombinationCache.find(keys);
			UINT slot;
			if (found != SamplerCombinationCache.end())
			{
				slot = found->second;
			}
			else
			{
				if (ShaderVisibleSamplerCount + MaxUserShaderTextureSlots > ShaderVisibleSamplerHeapCapacity)
				{
					if (!growShaderVisibleSamplerHeap(ShaderVisibleSamplerCount + MaxUserShaderTextureSlots))
					{
						os::Printer::log("CD3D12Driver: shader-visible sampler heap full and growing"
							" it failed (see ShaderVisibleSamplerCapacity)", ELL_WARNING);
						return nullHandle;
					}
				}
				slot = ShaderVisibleSamplerCount;
				ShaderVisibleSamplerCount += MaxUserShaderTextureSlots;
				for (UINT i = 0; i < MaxUserShaderTextureSlots; ++i)
				{
					D3D12_CPU_DESCRIPTOR_HANDLE destCPU = ShaderVisibleSamplerHeapStartCPU;
					destCPU.ptr += static_cast<SIZE_T>(slot + i) * SamplerDescriptorSize;
					Device->CreateSampler(&descs[i], destCPU);
				}
				SamplerCombinationCache[keys] = slot;
			}

			D3D12_GPU_DESCRIPTOR_HANDLE destGPU = ShaderVisibleSamplerHeapStartGPU;
			destGPU.ptr += static_cast<UINT64>(slot) * SamplerDescriptorSize;
			return destGPU;
		}

		D3D12_VERTEX_BUFFER_VIEW CD3D12Driver::allocateVertices(const void* data, u32 vertexCount, u32 stride)
		{
			D3D12_VERTEX_BUFFER_VIEW view = {};
			if (!vertexCount || !stride)
				return view;

			SD3D12FrameContext& frame = Frames[CurrentFrameIndex];
			const UINT64 sizeBytes = static_cast<UINT64>(vertexCount) * stride;
			if (frame.VertexRingOffset + sizeBytes > frame.VertexRingCapacity)
			{
				if (!growVertexRing(frame, sizeBytes))
				{
					os::Printer::log("CD3D12Driver: anneau de vertex plein pour cette frame et son"
						" agrandissement a echoue (voir VertexRingSizePerFrame)", ELL_WARNING);
					return view;
				}
			}

			memcpy(static_cast<u8*>(frame.VertexRingMapped) + frame.VertexRingOffset, data, sizeBytes);
			view.BufferLocation = frame.VertexRing->GetGPUVirtualAddress() + frame.VertexRingOffset;
			view.SizeInBytes = static_cast<UINT>(sizeBytes);
			view.StrideInBytes = stride;
			frame.VertexRingOffset += sizeBytes;
			return view;
		}

		void CD3D12Driver::bindTransformsAndTexture(const core::matrix4& world, const core::matrix4& view,
			const core::matrix4& proj, video::ITexture* texture, const SMaterialLayer& layer,
			video::ITexture* texture2, const SMaterialLayer& layer2,
			video::ITexture* const* extraTextures, const SMaterialLayer* extraLayers, UINT extraCount,
			bool useMipMaps)
		{
			// Transpose before upload: core::matrix4 is row-major/row-vector (v' = v*M, see
			// matrix4.h) whereas an HLSL float4x4 not annotated "row_major" is packed
			// column-major; transposing before upload makes mul(vector, M) on the shader
			// side reproduce v*M -- same convention as CD3D11FixedPipelineRenderer (see also
			// the analysis in CD3D12DefaultShaders.h).
			core::matrix4 worldT = world.getTransposed();
			D3D12_GPU_VIRTUAL_ADDRESS worldAddr = allocateConstant(worldT.pointer(), sizeof(f32) * 16);
			if (worldAddr)
				CommandList->SetGraphicsRootConstantBufferView(0, worldAddr);

			struct SPerFrameConstants
			{
				core::matrix4 View;
				core::matrix4 Proj;
				// See the comment on CameraPosWorld in the PerFrame cbuffer
				// (D3D12DefaultShaderHLSL, CD3D12DefaultShaders.h) -- added at the end of the struct,
				// doesn't affect any existing shader that only reads View/Proj from this CBV.
				f32 CameraPosWorld[4];
			} perFrame;
			perFrame.View = view.getTransposed();
			perFrame.Proj = proj.getTransposed();
			// World-space camera position -- same technique as
			// CD3D9ParallaxMapRenderer::OnSetConstants() (CD3D9ParallaxMapRenderer.cpp): the origin
			// of view space (0,0,0,1) transformed by the inverse of the view matrix.
			{
				core::matrix4 viewInv = view;
				viewInv.makeInverse();
				f32 originPoint[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
				viewInv.multiplyWith1x4Matrix(originPoint);
				perFrame.CameraPosWorld[0] = originPoint[0];
				perFrame.CameraPosWorld[1] = originPoint[1];
				perFrame.CameraPosWorld[2] = originPoint[2];
				perFrame.CameraPosWorld[3] = 0.0f;
			}
			D3D12_GPU_VIRTUAL_ADDRESS frameAddr = allocateConstant(&perFrame, sizeof(perFrame));
			if (frameAddr)
				CommandList->SetGraphicsRootConstantBufferView(1, frameAddr);

			// Assembles the MaxUserShaderTextureSlots (9) textures/layers -- slots 0/1 from
			// texture/texture2 (compat with all existing callers), slots 2..8 from
			// extraTextures/extraLayers (only bindDrawState() fills these in, from
			// SMaterial::TextureLayer[2..8] -- see its comment). Any slot beyond extraCount
			// stays nullptr/default SMaterialLayer(), which allocateSRVTableSlot()/
			// allocateSamplerTableSlot() already fall back to the default NullTexture/sampler for.
			std::array<CD3D12Texture*, MaxUserShaderTextureSlots> textures{};
			std::array<SMaterialLayer, MaxUserShaderTextureSlots> layers{};
			textures[0] = static_cast<CD3D12Texture*>(texture);
			layers[0] = layer;
			textures[1] = static_cast<CD3D12Texture*>(texture2);
			layers[1] = layer2;
			for (UINT i = 0; i < extraCount && (2 + i) < MaxUserShaderTextureSlots; ++i)
			{
				textures[2 + i] = static_cast<CD3D12Texture*>(extraTextures[i]);
				layers[2 + i] = extraLayers[i];
			}

			D3D12_GPU_DESCRIPTOR_HANDLE srvTable = allocateSRVTableSlot(textures);
			if (srvTable.ptr != 0)
				CommandList->SetGraphicsRootDescriptorTable(2, srvTable);

			// Sampler table s0..s8 -- filter/addressing settings of the current layers.
			// useMipMaps (SMaterial::UseMipMaps) applies uniformly to all 9 slots -- it isn't a
			// SMaterialLayer field, see buildD3D12SamplerDesc().
			D3D12_GPU_DESCRIPTOR_HANDLE samplerTable = allocateSamplerTableSlot(layers, useMipMaps);
			if (samplerTable.ptr != 0)
				CommandList->SetGraphicsRootDescriptorTable(3, samplerTable);

			// CBV b2 -- user clip planes, evaluated on the pixel shader side (see
			// CD3D12DefaultShaders.h). A disabled plane is encoded as (0,0,0,1): the shader
			// always computes dot(WorldPos.xyz1, plane), so a "neutral" plane must always
			// give a positive result (never clipped) regardless of WorldPos -- (0,0,0,1) does
			// exactly that (dot = 1 > 0 for every point).
			struct SFloat4 { f32 X, Y, Z, W; };
			struct SClipPlaneConstants
			{
				SFloat4 Planes[3];
			} clipConstants;
			for (u32 i = 0; i < 3; ++i)
			{
				if (ClipPlaneEnabled[i])
					clipConstants.Planes[i] = { ClipPlanes[i].Normal.X,
						ClipPlanes[i].Normal.Y, ClipPlanes[i].Normal.Z, ClipPlanes[i].D };
				else
					clipConstants.Planes[i] = { 0.0f, 0.0f, 0.0f, 1.0f };
			}
			D3D12_GPU_VIRTUAL_ADDRESS clipAddr = allocateConstant(&clipConstants, sizeof(clipConstants));
			if (clipAddr)
				CommandList->SetGraphicsRootConstantBufferView(4, clipAddr);
		}

		namespace
		{
			// EXACT C++ mirror layout of the LightingCB block (D3D12DefaultShaderHLSL,
			// CD3D12DefaultShaders.h) -- no reflection here, like PerObject/PerFrame/
			// ClipPlanesCB (see bindTransformsAndTexture() above), so any change on one
			// side must be mirrored exactly on the other. Each "float[4]" is an
			// HLSL float4 (16 bytes, already aligned); the trailing 4 ints (LightCount/EnableLighting/
			// ColorMaterialMode/NormalizeNormalsFlag) fit together in a single 16-byte
			// register, no padding needed.
			struct SD3D12ShaderLight
			{
				f32 Position[4]; // xyz = world position, w = 0 (see vector3df::getAs4Values())
				f32 Diffuse[4];
				f32 Specular[4];
				f32 Ambient[4];
				f32 Atten[4];    // x=constant, y=linear, z=quadratic, w = 0 (same)
			};

			struct SD3D12ShaderLightMaterial
			{
				f32 Ambient[4];
				f32 Diffuse[4];
				f32 Specular[4];
				f32 Emissive[4];
			};

			struct SD3D12LightingConstants
			{
				SD3D12ShaderLight Lights[8]; // MAX_LIGHTS on the HLSL side -- see getMaximalDynamicLightAmount()
				SD3D12ShaderLightMaterial Material;
				s32 LightCount;
				s32 EnableLighting;
				s32 ColorMaterialMode;
				s32 NormalizeNormalsFlag;
			};

			void writeShaderColor(f32* dst, const video::SColorf& c)
			{
				dst[0] = c.r; dst[1] = c.g; dst[2] = c.b; dst[3] = c.a;
			}

			// EXACT C++ mirror layout of the FogCB block (D3D12DefaultShaderHLSL,
			// CD3D12DefaultShaders.h) -- same absence of reflection as SD3D12LightingConstants
			// above. Color is an HLSL float4 (16 bytes); Mode/Start/End/Density fit
			// together in the next 16-byte register; EnableFog starts a third
			// register. ParallaxHeightScale occupies the padding byte that follows EnableFog
			// in that same register (now actually declared on the HLSL side, see FogCB); Pad[2]
			// fills up to the 48-byte rounding the HLSL compiler applies (unchanged).
			struct SD3D12FogConstants
			{
				f32 Color[4];
				s32 Mode;
				f32 Start;
				f32 End;
				f32 Density;
				s32 EnableFog;
				f32 ParallaxHeightScale;
				f32 Pad[2];
			};
		}

		// CBV b3 -- dynamic lighting. Same split of responsibilities as
		// CD3D11FixedPipelineRenderer::OnSetConstants() (CD3D11FixedPipelineRenderer.cpp): reads
		// SMaterial::Lighting/AmbientColor/DiffuseColor/SpecularColor/EmissiveColor/ColorMaterial/
		// NormalizeNormals and, if Lighting is active, the dynamic light list
		// (getDynamicLightCount()/getDynamicLight(), now the real CNullDriver
		// implementations since CD3D12Driver.h no longer has the -1/0 stubs -- see its comment).
		// The lighting computation itself stays entirely in VSMain (calcLighting(),
		// D3D12DefaultShaderHLSL): this cbuffer is just a data carrier, same VS/PS split
		// as the D3D11 renderer (per-vertex computation, not per-pixel).
		void CD3D12Driver::bindLighting(const SMaterial& material)
		{
			SD3D12LightingConstants constants;
			ZeroMemory(&constants, sizeof(constants));

			constants.EnableLighting = material.Lighting ? 1 : 0;
			constants.ColorMaterialMode = static_cast<s32>(material.ColorMaterial);
			constants.NormalizeNormalsFlag = material.NormalizeNormals ? 1 : 0;

			// Material term always filled in (even if Lighting==false) -- harmless, VSMain only
			// reads it in the EnableLighting!=0 branch (see CD3D12DefaultShaders.h).
			writeShaderColor(constants.Material.Ambient, material.AmbientColor);
			writeShaderColor(constants.Material.Diffuse, material.DiffuseColor);
			writeShaderColor(constants.Material.Specular, material.SpecularColor);
			writeShaderColor(constants.Material.Emissive, material.EmissiveColor);

			if (material.Lighting)
			{
				u32 count = getDynamicLightCount();
				if (count > 8)
					count = 8; // MAX_LIGHTS on the HLSL side -- same bound as calcLighting()'s min(MAX_LIGHTS, LightCount)
				constants.LightCount = static_cast<s32>(count);

				for (u32 i = 0; i < count; ++i)
				{
					const SLight& dl = getDynamicLight(i);
					SD3D12ShaderLight& l = constants.Lights[i];
					l.Position[0] = dl.Position.X;
					l.Position[1] = dl.Position.Y;
					l.Position[2] = dl.Position.Z;
					l.Position[3] = 0.0f;
					writeShaderColor(l.Diffuse, dl.DiffuseColor);
					writeShaderColor(l.Specular, dl.SpecularColor);
					writeShaderColor(l.Ambient, dl.AmbientColor);
					l.Atten[0] = dl.Attenuation.X;
					l.Atten[1] = dl.Attenuation.Y;
					l.Atten[2] = dl.Attenuation.Z;
					l.Atten[3] = 0.0f;
				}
			}

			D3D12_GPU_VIRTUAL_ADDRESS addr = allocateConstant(&constants, sizeof(constants));
			if (addr)
				CommandList->SetGraphicsRootConstantBufferView(LightingConstantSlot, addr);
		}

		// CBV b4 -- fog. Same split of responsibilities as
		// CD3D11FixedPipelineRenderer::OnSetConstants() (CD3D11FixedPipelineRenderer.cpp): reads
		// SMaterial::FogEnable and the global parameters set by setFog()/getFog(). FAITHFULLY
		// reproduces a quirk of the D3D11 renderer rather than "fixing" it here (bit-for-bit
		// parity is required for this driver, same principle as the specular hardcoded to 64 in
		// calcLighting()): cbPerTechnique.fogMode is assigned DIRECTLY from
		// video::E_FOG_TYPE (EFT_FOG_EXP=0/EFT_FOG_LINEAR=1/EFT_FOG_EXP2=2), which does NOT correspond
		// to the FOGMODE_* constants expected by calcFogFactor() (NONE=0/LINEAR=1/EXP=2/EXP2=3) --
		// EFT_FOG_EXP(0) therefore falls back to FOGMODE_NONE (no switch case matches, fogCoeff
		// stays at 1.0), EFT_FOG_LINEAR(1) actually matches FOGMODE_LINEAR, and EFT_FOG_EXP2(2)
		// matches FOGMODE_EXP (not EXP2, which is in fact never reachable from
		// E_FOG_TYPE). Only EFT_FOG_LINEAR therefore produces visually correct fog on
		// D3D11 as well as here -- see CD3D12DriverTests.cpp for the type chosen in the tests.
		void CD3D12Driver::bindFog(const SMaterial& material)
		{
			SD3D12FogConstants constants;
			ZeroMemory(&constants, sizeof(constants));

			constants.EnableFog = material.FogEnable ? 1 : 0;
			writeShaderColor(constants.Color, video::SColorf(FogColor));
			constants.Mode = static_cast<s32>(FogType);
			constants.Start = FogStart;
			constants.End = FogEnd;
			constants.Density = FogDensity;
			// See EMT_PARALLAX_MAP_SOLID (EMaterialTypes.h) -- "If set to zero, the
			// default value (0.02f) will be applied", same fallback as
			// CD3D9ParallaxMapRenderer::OnSetConstants(). Filled in unconditionally (like the
			// rest of this cbuffer); only PSMainParallaxMap*/*(CD3D12DefaultShaders.h) reads it.
			constants.ParallaxHeightScale = (material.MaterialTypeParam != 0.0f) ? material.MaterialTypeParam : 0.02f;

			D3D12_GPU_VIRTUAL_ADDRESS addr = allocateConstant(&constants, sizeof(constants));
			if (addr)
				CommandList->SetGraphicsRootConstantBufferView(FogConstantSlot, addr);
		}


		bool CD3D12Driver::bindDrawState(const SMaterial& material, const core::matrix4& world,
			const core::matrix4& view, const core::matrix4& proj, IVertexDescriptor* descriptor,
			scene::E_PRIMITIVE_TYPE primitiveType)
		{
			// Outside a scene, CommandList is closed: anything recorded onto it would be purely and
			// simply lost (the debug layer flags it, the release driver doesn't). D3D11's
			// immediate context is always recording and absorbs this caller error, hence engine
			// code that draws outside beginScene()/endScene() without it being noticeable -- here we refuse
			// and say so, once.
			if (!SceneOpen)
			{
				if (!WarnedDrawOutsideScene)
				{
					os::Printer::log("CD3D12Driver: draw hors beginScene()/endScene() -- ignore."
						" Une command list D3D12 fermee n'enregistre rien (contrairement au contexte"
						" immediat D3D11) : ce draw serait perdu silencieusement.", ELL_WARNING);
					WarnedDrawOutsideScene = true;
				}
				return false;
			}

			ID3D12PipelineState* pso = getPSOForMaterial(material, descriptor, primitiveType);
			if (!pso)
				return false;

			// Active material -- see getNativeMaterialRenderer()/SD3D12MaterialRendererEntry.
			// ActiveMaterialRendererIndex conditions setVertexShaderConstant()/
			// getVertexShaderConstantID() etc. (IMaterialRendererServices, see further below in this
			// file) during the call to OnSetConstants() below. Equals material.MaterialType
			// both for a built-in type and for a user shader (unified registry); only a
			// user shader has a CallBack/non-empty cbuffers (see
			// createBuiltInMaterialRenderers()), so the blocks below are a no-op for a
			// built-in material without needing to distinguish them explicitly here.
			// Bound against the owner's (ResourceOwner) registry, not ours: a deferred context
			// has an empty MaterialRenderers and delegates everything to the immediate driver.
			//
			// Resolved BEFORE the reservation and the root signature bind below: both now depend on
			// WHICH material is being drawn (its own root signature, and how many CBV tables that
			// signature declares) instead of on one driver-wide layout. Same renderer
			// getPSOForMaterial() resolved just above via getNativeMaterialRenderer(), which is
			// getNativeRenderer(material.MaterialType).
			ActiveMaterialRendererIndex = -1;
			if (material.MaterialType >= 0 && static_cast<u32>(material.MaterialType) < getMaterialRendererCount())
				ActiveMaterialRendererIndex = material.MaterialType;

			CD3D12MaterialRenderer* activeRenderer = getNativeRenderer(ActiveMaterialRendererIndex);

			// Reserves ALL of the descriptors this draw can consume in one shot (the SRV table plus
			// one CBV table per entry in UserCBVTables): if the heap needs to grow, it grows HERE,
			// before the least root argument is set. Without this reservation, growth used to
			// happen in the middle of a draw -- allocateSRVTableSlot() would place the SRV table in the
			// current heap, then allocateUserCBVTable() would overflow, create a NEW heap and bind it, and the
			// draw would end up with an SRV table pointing into the old (no longer bound) heap: wrong
			// descriptors, hence wrong textures, varying from frame to frame depending on when the
			// overflow happened. See reserveShaderVisibleSRVDescriptors().
			//
			// Sized from THIS material rather than from the worst case
			// (MaxShaderVisibleSRVDescriptorsPerDraw): now that a material only declares the CBV
			// tables its shaders actually use, a built-in material reserves MaxUserShaderTextureSlots
			// descriptors instead of that plus the 5*MaxUserShaderRegisterSpaces*
			// MaxUserShaderCBVSlotsPerStage it would never touch. Must stay >= what the
			// allocateUserCBVTable() loop at the end of this function consumes -- it is derived from
			// the same UserCBVTables vector that loop walks, so the two cannot drift.
			const UINT drawDescriptorCount = MaxUserShaderTextureSlots +
				static_cast<UINT>(activeRenderer ? activeRenderer->UserCBVTables.size() : 0) *
				MaxUserShaderCBVSlotsPerStage;

			SD3D12FrameContext& drawFrame = Frames[CurrentFrameIndex];
			if (!reserveShaderVisibleSRVDescriptors(drawFrame, drawDescriptorCount))
				return false;

			// This material's OWN root signature, which is the one getPSOForMaterial() built `pso`
			// against -- D3D12 requires the root signature bound at draw time to be the PSO's, hence
			// the shared rootSignatureForRenderer() rather than a third copy of the same choice.
			CommandList->SetGraphicsRootSignature(rootSignatureForRenderer(activeRenderer));
			CommandList->SetPipelineState(pso);

			if (activeRenderer && activeRenderer->CallBack)
			{
				activeRenderer->CallBack->OnSetMaterial(material);
				activeRenderer->CallBack->OnSetConstants(this, activeRenderer->UserData);
			}

			// Registers t2..t8/s2..s8 -- beyond the 2 layers that used to be hardcoded, for
			// user shaders with N textures (addHighLevelShaderMaterial*). No
			// effect on built-in materials (whose shader never declares/reads beyond t1).
			video::ITexture* extraTextures[MaxUserShaderTextureSlots - 2];
			SMaterialLayer extraLayers[MaxUserShaderTextureSlots - 2];
			for (UINT i = 0; i < MaxUserShaderTextureSlots - 2; ++i)
			{
				extraTextures[i] = material.getTexture(2 + i);
				extraLayers[i] = material.TextureLayer[2 + i];
			}

			bindTransformsAndTexture(world, view, proj, material.getTexture(0), material.TextureLayer[0],
				material.getTexture(1), material.TextureLayer[1],
				extraTextures, extraLayers, MaxUserShaderTextureSlots - 2, material.UseMipMaps);

			// CBV b3 -- dynamic lighting (see bindLighting() for the details). Independent
			// of the OnSetConstants()/activeRenderer block above: the ~24 built-in materials (including
			// EMT_SOLID) have no CallBack (see createBuiltInMaterialRenderers()), so nothing
			// else would fill this cbuffer for them if bindDrawState() didn't call it
			// explicitly here.
			bindLighting(material);

			// CBV b4 -- fog (see bindFog() for the details). Same reasoning for
			// independence from OnSetConstants()/activeRenderer as bindLighting()
			// just above.
			bindFog(material);

			// Upload + bind of the CBV b0..b7 tables -- after bindTransformsAndTexture() since the
			// content was just written into the scratch buffers by OnSetConstants() above
			// (setVertexShaderConstant() etc. memcpy into CD3D12MaterialRenderer::VSBuffers/
			// PSBuffers[i].Scratch, see further below).
			//
			// EXACTLY the tables this material's root signature declares, no more: UserCBVTables holds
			// one entry per (stage, register space) pair its shaders reflect a cbuffer at, in root
			// parameter order (see buildMaterialRootSignature()). A built-in material has none and
			// binds nothing here -- where the old driver-wide root signature forced all
			// 5*MaxUserShaderRegisterSpaces (20) tables on EVERY draw, 2D/GUI blits included, each one
			// costing a SetGraphicsRootDescriptorTable() plus MaxUserShaderCBVSlotsPerStage (8)
			// CreateConstantBufferView() calls writing null CBVs into 8 freshly burned descriptors.
			//
			// Binding fewer tables is safe here in a way it was NOT under the shared signature. The
			// hazard there was that a TABLE-type root argument a draw doesn't reset keeps the PREVIOUS
			// draw's handle (rebinding the same root signature does not invalidate root arguments),
			// so once growShaderVisibleSRVHeap() swapped the heap that stale handle pointed into an
			// unbound one -- EXECUTION ERROR #554 SET_DESCRIPTOR_HEAP_INVALID, wrong textures on all
			// 2D/GUI drawing as soon as the frame's heap grew. That cannot recur:
			//   - a draw whose material has a DIFFERENT root signature invalidates every root argument
			//     by definition (SetGraphicsRootSignature with a different object), and
			//   - a draw whose material has the SAME root signature has, by construction, the same
			//     UserCBVTables -- so this loop rewrites every table that signature declares.
			// Either way no table in the bound signature can survive a draw un-rewritten.
			if (activeRenderer)
			{
				for (const SD3D12UserCBVTable& table : activeRenderer->UserCBVTables)
				{
					const std::vector<SD3D12UserShaderCBuffer>* buffers = activeRenderer->getStageBuffers(table.Stage);
					if (!buffers)
						continue;
					CommandList->SetGraphicsRootDescriptorTable(table.RootSlot,
						allocateUserCBVTable(*buffers, table.Space));
				}
			}

			return true;
		}

		SPSOKey CD3D12Driver::buildShadowVolumeStencilKey(D3D12_CULL_MODE cullMode, D3D12_STENCIL_OP op, bool useDepthFailOp) const
		{
			SPSOKey key;
			key.VSHash = std::hash<void*>()(getSolidVertexShader());
			key.PSHash = std::hash<void*>()(getSolidPixelShader());
			key.InputLayoutHash = hashInputLayout(kS3DVertexInputLayout, _countof(kS3DVertexInputLayout));
			key.BlendMode = SPSOKey::EBlendMode::None;
			key.DepthTestEnable = true;
			key.DepthWriteEnable = false; // stencil marking must never modify the depth buffer
			// Same comparison as CD3D11Driver::setRenderStatesStencilShadowMode()
			// (D3D11_COMPARISON_GREATER), used for both zpass and zfail, consistent with
			// this fork's inverted depth convention rather than with the drawn material's
			// SMaterial::ZBuffer.
			key.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
			key.CullMode = cullMode;
			key.FillMode = D3D12_FILL_MODE_SOLID;
			key.TopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			// Stencil marking, always against the currently bound target (see
			// CurrentRTVFormats) -- so far always the back buffer in practice (no intent
			// to draw shadow volumes into an RTT), but stays consistent with the
			// real target rather than assuming a hardcoded R8G8B8A8_UNORM.
			key.RTVFormats[0] = CurrentRTVFormats[0];
			key.SampleCount = CurrentRTVSampleCount; // see buildPSOKeyFromMaterial()
			key.DSVFormat = CurrentDSVFormat; // same reason as buildPSOKeyFromMaterial(), see CurrentDSVFormat
			key.StencilEnable = true;
			key.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
			key.StencilFailOp = D3D12_STENCIL_OP_KEEP;
			// zpass fires op on depth-test success (StencilPassOp); zfail fires it on depth-test
			// failure (StencilDepthFailOp) -- the field not selected stays KEEP.
			key.StencilDepthFailOp = useDepthFailOp ? op : D3D12_STENCIL_OP_KEEP;
			key.StencilPassOp = useDepthFailOp ? D3D12_STENCIL_OP_KEEP : op;
			key.RenderTargetWriteMask = 0; // stencil marking only, no color writes
			return key;
		}

		namespace
		{
			//! Reciprocal of mapMeshPrimitiveTypeToTopology() (further below): the PSO must declare the
			//! D3D12_PRIMITIVE_TOPOLOGY_TYPE matching the topology actually submitted to
			//! IASetPrimitiveTopology(), otherwise D3D12 rejects the draw. drawImmediate() used to let
			//! bindDrawState() default to EPT_TRIANGLES while submitting a LINELIST:
			//! no line ever came out (draw3DLine/draw2DLine, bounding boxes, the graph
			//! scenenode...).
			scene::E_PRIMITIVE_TYPE mapTopologyToMeshPrimitiveType(D3D_PRIMITIVE_TOPOLOGY topology)
			{
				switch (topology)
				{
				case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:     return scene::EPT_POINTS;
				case D3D_PRIMITIVE_TOPOLOGY_LINELIST:      return scene::EPT_LINES;
				case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:     return scene::EPT_LINE_STRIP;
				case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return scene::EPT_TRIANGLE_STRIP;
				default:                                   return scene::EPT_TRIANGLES;
				}
			}
		}

		void CD3D12Driver::drawImmediate(const S3DVertex* vertices, u32 vertexCount, D3D_PRIMITIVE_TOPOLOGY topology,
			const SMaterial& material, const core::matrix4& world, const core::matrix4& view, const core::matrix4& proj)
		{
			if (!vertexCount)
				return;

			D3D12_VERTEX_BUFFER_VIEW vbView = allocateVertices(vertices, vertexCount, sizeof(S3DVertex));
			if (vbView.SizeInBytes == 0)
				return;

			if (!bindDrawState(material, world, view, proj, nullptr, mapTopologyToMeshPrimitiveType(topology)))
				return;

			CommandList->IASetPrimitiveTopology(topology);
			CommandList->IASetVertexBuffers(0, 1, &vbView);
			CommandList->DrawInstanced(vertexCount, 1, 0, 0);
		}

		core::matrix4 CD3D12Driver::build2DProjection() const
		{
			// Same formula as CD3D11Driver::draw2DImage (region "if (CurrentRenderMode !=
			// ERM_2D..."): negative height to flip the Y axis (screen downward, NDC
			// upward), then a translation to re-anchor onto [-1,1].
			core::matrix4 m;
			m.buildProjectionMatrixOrthoLH(f32(CurrentRenderTargetSize.Width),
				f32(-static_cast<s32>(CurrentRenderTargetSize.Height)), -1.0f, 1.0f);
			m.setTranslation(core::vector3df(-1, 1, 0));
			return m;
		}

		SMaterial CD3D12Driver::build2DMaterial(bool alphaBlend, video::ITexture* texture) const
		{
			SMaterial m;
			m.Lighting = false;
			m.ZBuffer = ECFN_DISABLED;
			m.ZWriteEnable = false;
			m.BackfaceCulling = false;
			m.FrontfaceCulling = false;
			m.MaterialType = alphaBlend ? EMT_TRANSPARENT_ALPHA_CHANNEL : EMT_SOLID;
			m.setTexture(0, texture);
			return m;
		}

		// Same body as CD3D11Driver::setViewPort(): clips the requested area against the current
		// render target, pushes it to the rasterizer, and -- this is the critical part -- remembers the
		// result in ViewPort so getViewPort() returns something other than an empty rect. See the
		// declaration in CD3D12Driver.h (RmlUI's UI is invisible without this).
		void CD3D12Driver::setViewPort(const core::rect<s32>& area)
		{
			const core::dimension2du size = getCurrentRenderTargetSize();

			core::rect<s32> vp = area;
			const core::rect<s32> rendert(0, 0, static_cast<s32>(size.Width), static_cast<s32>(size.Height));
			vp.clipAgainst(rendert);

			if (vp.getWidth() <= 0 || vp.getHeight() <= 0)
				return; // an empty viewport is rejected by D3D12, and would leave ViewPort unusable

			D3D12_VIEWPORT viewport = {
				static_cast<float>(vp.UpperLeftCorner.X), static_cast<float>(vp.UpperLeftCorner.Y),
				static_cast<float>(vp.getWidth()), static_cast<float>(vp.getHeight()), 0.0f, 1.0f };
			CommandList->RSSetViewports(1, &viewport);

			ViewPort = vp;
		}

		const core::rect<s32>& CD3D12Driver::getViewPort() const
		{
			return ViewPort;
		}

		void CD3D12Driver::setScissorFromClip(const core::rect<s32>* clip)
		{
			D3D12_RECT rect;
			if (clip)
			{
				rect.left = clip->UpperLeftCorner.X;
				rect.top = clip->UpperLeftCorner.Y;
				rect.right = clip->LowerRightCorner.X;
				rect.bottom = clip->LowerRightCorner.Y;
			}
			else
			{
				rect.left = 0;
				rect.top = 0;
				rect.right = static_cast<LONG>(CurrentRenderTargetSize.Width);
				rect.bottom = static_cast<LONG>(CurrentRenderTargetSize.Height);
			}
			CommandList->RSSetScissorRects(1, &rect);
		}

		ID3D12PipelineState* CD3D12Driver::getOrCreateAuxPSO(const SPSOKey& key)
		{
			// Auxiliary PSOs always draw with the built-in solid shaders, hence always against the
			// DEFAULT root signature -- forced here rather than trusted from the caller, since some
			// keys reaching this function come from buildPSOKeyFromMaterial() (see the occlusion query
			// path in runOcclusionQuery()), which hashes the ROLE MATERIAL's root signature. That is
			// the same object for an EMT_SOLID occlusion material today, but nothing guarantees the
			// caller passes such a material, and a mismatch here would silently cache the PSO under a
			// key that no longer describes it.
			SPSOKey auxKey = key;
			auxKey.RootSignatureHash = std::hash<void*>()(RootSignature.Get());

			return PSOCache.getOrCreate(Device.Get(), RootSignature.Get(), auxKey,
				getSolidVertexShader(), getSolidPixelShader(), kS3DVertexInputLayout, _countof(kS3DVertexInputLayout));
		}

		void CD3D12Driver::setTransform(E_TRANSFORMATION_STATE state, const core::matrix4& mat)
		{
			Matrices[state] = mat;
		}

		const core::matrix4& CD3D12Driver::getTransform(E_TRANSFORMATION_STATE state) const
		{
			return Matrices[state];
		}

		void CD3D12Driver::setMaterial(const SMaterial& material)
		{
			Material = material;
			OverrideMaterial.apply(Material);
		}

		// See the header comment on the declaration (CD3D12Driver.h) -- same
		// contract as CD3D11Driver::setClipPlane()/enableClipPlane()/getClipPlane().
		bool CD3D12Driver::setClipPlane(u32 index, const core::plane3df& plane, bool enable)
		{
			if (index > 2)
				return false;

			ClipPlanes[index] = plane;
			enableClipPlane(index, enable);
			return true;
		}

		void CD3D12Driver::enableClipPlane(u32 index, bool enable)
		{
			if (index > 2)
				return;
			ClipPlaneEnabled[index] = enable;
		}

		namespace
		{
			// Same set of topologies as draw2DVertexPrimitiveList() (POINTS/LINES/
			// LINE_STRIP/TRIANGLES/TRIANGLE_STRIP) -- the others (fans/quads/loops/polygon/point
			// sprites) have no 1:1 D3D12 equivalent and would require a CPU expansion of the
			// index/vertex buffers, out of scope here. Returns D3D_PRIMITIVE_TOPOLOGY_UNDEFINED for
			// everything else, leaving it to the caller to refuse drawing in that case rather than force
			// an incorrect topology (see drawMeshBuffer()).
			//! Strict parity with CD3D11Driver::getTopology(). The four types beyond the
			//! five "obvious" ones were missing and fell back to UNDEFINED, which made
			//! drawMeshBuffer() bail out BEFORE the draw: the mesh wasn't drawn at all (as opposed to
			//! being drawn incorrectly). Any scenenode built on one of them -- billboards/point
			//! sprites, halos and lens flares, triangle fans -- therefore vanished
			//! purely and simply under D3D12 while it rendered fine under D3D11.
			D3D_PRIMITIVE_TOPOLOGY mapMeshPrimitiveTypeToTopology(scene::E_PRIMITIVE_TYPE primitiveType)
			{
				switch (primitiveType)
				{
				case scene::EPT_POINT_SPRITES:  return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
				case scene::EPT_POINTS:         return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
				case scene::EPT_LINE_STRIP:     return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
				case scene::EPT_LINE_LOOP:      return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
				case scene::EPT_LINES:          return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
				case scene::EPT_TRIANGLE_STRIP: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
				case scene::EPT_TRIANGLES:      return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
				// D3D10+ no longer has a triangle-fan: CD3D11Driver renders it as TRIANGLESTRIP, so we do
				// the same (exact parity -- the geometry isn't identical to a real fan, but
				// it's the behavior the rest of the engine has come to expect from the D3D11 driver).
				case scene::EPT_TRIANGLE_FAN:   return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
				case scene::EPT_CONTROL_POINT:  return D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
				default:                        return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
				}
			}
		}

		void CD3D12Driver::drawMeshBuffer(const scene::IMeshBuffer* mb)
		{
			if (!mb || mb->getVertexBufferCount() == 0)
				return;

			const u32 vbCount = mb->getVertexBufferCount();
			if (vbCount > D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT)
			{
				os::Printer::log("CD3D12Driver::drawMeshBuffer: trop de vertex buffers pour l'IA D3D12"
					" (limite 32)", ELL_WARNING);
				return;
			}

			// Multi-stream (IASetVertexBuffers across every slot, not just slot 0) and
			// instancing -- same logic as CD3D11Driver::drawMeshBuffer()/renderArray(): each
			// vertex buffer is classified via mb->getVertexDescriptor()->getInstanceDataStepRate(bufferID)
			// (see buildInputLayoutDescription() further below -- the corresponding PER_VERTEX_
			// DATA/PER_INSTANCE_DATA layout already existed, only this call site wasn't using it)
			// ; the EIDSR_PER_VERTEX buffer(s) give drawVertexCount (non-indexed path), the
			// EIDSR_PER_INSTANCE buffer(s) give instanceVertexCount. instanceVertexCount==0 => a
			// non-instanced draw, rendered identically to before via InstanceCount=1 (D3D12 has only
			// one family of Draw*Instanced calls, unlike D3D11 which distinguishes Draw/
			// DrawInstanced).
			IVertexDescriptor* descriptor = mb->getVertexDescriptor();
			D3D12_VERTEX_BUFFER_VIEW vbViews[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
			u32 drawVertexCount = 0;
			u32 instanceVertexCount = 0;

			for (u32 i = 0; i < vbCount; ++i)
			{
				scene::IVertexBuffer* streamVb = mb->getVertexBuffer(i);
				if (!streamVb || streamVb->getVertexCount() == 0)
					return;

				auto streamHardware = streamVb->getHardwareBuffer();
				if (!streamHardware || streamHardware->getDriverType() != EDT_DIRECT3D12)
					streamHardware = createHardwareBuffer(streamVb);
				else if (streamHardware->isRequiredUpdate())
					streamHardware->update(streamVb->getHardwareMappingHint(),
						streamVb->getVertexCount() * streamVb->getVertexSize(), streamVb->getVertices());
				if (!streamHardware)
					return;

				vbViews[i] = static_cast<CD3D12HardwareBuffer*>(streamHardware.get())->getVertexBufferView();

				if (descriptor && descriptor->getInstanceDataStepRate(i) == EIDSR_PER_INSTANCE)
					instanceVertexCount = streamVb->getVertexCount();
				else
					drawVertexCount = streamVb->getVertexCount();
			}

			// The mesh buffer's native topology determines both what's submitted to
			// IASetPrimitiveTopology further below AND (via bindDrawState()/buildPSOKeyFromMaterial())
			// the PSO's D3D12_PRIMITIVE_TOPOLOGY_TYPE -- the two must match (see the
			// comment on buildPSOKeyFromMaterial()). A GS expecting a point/line input
			// primitive (stream-output, see setStreamOutputBuffer()) doesn't execute otherwise.
			D3D_PRIMITIVE_TOPOLOGY meshTopology = mapMeshPrimitiveTypeToTopology(mb->getPrimitiveType());
			if (meshTopology == D3D_PRIMITIVE_TOPOLOGY_UNDEFINED)
			{
				os::Printer::log("CD3D12Driver::drawMeshBuffer: unsupported E_PRIMITIVE_TYPE", ELL_WARNING);
				return;
			}

			// bindDrawState(): PSO/root signature/CBV b0-b1 (World/View+Proj, transposed before
			// upload -- see CD3D12DefaultShaders.h: core::matrix4 is row-major/row-vector,
			// v' = v*M, whereas an HLSL float4x4 not annotated "row_major" is packed
			// column-major; transposing before upload makes mul(vector, M) on the shader side
			// reproduce v*M, same convention as CD3D11FixedPipelineRenderer) and the t0 SRV table.
			// Shared with all the immediate-drawing functions (see drawImmediate()).
			// descriptor determines the PSO's real input layout, including the
			// PER_INSTANCE_DATA slots -- see resolveInputLayout()/buildInputLayoutDescription().
			// mb->getPrimitiveType() determines key.TopologyType -- see
			// buildPSOKeyFromMaterial().
			if (!bindDrawState(Material, Matrices[ETS_WORLD], Matrices[ETS_VIEW], Matrices[ETS_PROJECTION],
				descriptor, mb->getPrimitiveType()))
				return;

			// Milestone C: a material with HS/DS (see buildPSOKeyFromMaterial()/
			// TopologyType==PATCH) must draw as a patch-list, not triangle-list -- the PSO bound by
			// bindDrawState() was built for that and would reject any other input
			// topology. 3 control points: only EPT_TRIANGLES is reinterpreted this way (one
			// 3-point patch per existing triangle, without touching the vertex/index buffers) --
			// an HS combined with another native topology (POINTS/LINES/...) is not handled.
			const CD3D12MaterialRenderer* activeRenderer = (ActiveMaterialRendererIndex >= 0) ?
				getNativeRenderer(ActiveMaterialRendererIndex) : nullptr;
			// HS *and* DS, same condition as when building key.TopologyType (buildPSOKeyFromMaterial()):
			// the two must stay in agreement, otherwise the PSO and the submitted topology contradict each other.
			D3D_PRIMITIVE_TOPOLOGY topology = (activeRenderer && activeRenderer->HS && activeRenderer->DS) ?
				D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST : meshTopology;
			// SMaterial::PointCloud (see buildPSOKeyFromMaterial(), which already switches the PSO to
			// D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT in this case): the topology actually submitted to
			// the IA must match the same PSO type, otherwise D3D12 executes nothing.
			if (Material.PointCloud && !(activeRenderer && activeRenderer->HS))
				topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
			CommandList->IASetPrimitiveTopology(topology);

			CommandList->IASetVertexBuffers(0, vbCount, vbViews);

			// instanceVertexCount==0 (no EIDSR_PER_INSTANCE buffer in the descriptor)
			// => InstanceCount=1, non-instanced draw identical to the previous behavior.
			const UINT drawInstanceCount = instanceVertexCount ? instanceVertexCount : 1;

			scene::IIndexBuffer* ib = mb->getIndexBuffer();
			if (ib && ib->getIndexCount() > 0)
			{
				auto ibHardware = ib->getHardwareBuffer();
				if (!ibHardware || ibHardware->getDriverType() != EDT_DIRECT3D12)
					ibHardware = createHardwareBuffer(ib);
				else if (ibHardware->isRequiredUpdate())
				{
					u32 indexSize = (ib->getType() == EIT_32BIT) ? 4 : 2;
					ibHardware->update(ib->getHardwareMappingHint(), ib->getIndexCount() * indexSize, ib->getIndices());
				}
				if (!ibHardware)
					return;

				D3D12_INDEX_BUFFER_VIEW ibView = static_cast<CD3D12HardwareBuffer*>(ibHardware.get())->getIndexBufferView();
				CommandList->IASetIndexBuffer(&ibView);
				CommandList->DrawIndexedInstanced(ib->getIndexCount(), drawInstanceCount, 0, 0, 0);
			}
			else
			{
				CommandList->DrawInstanced(drawVertexCount, drawInstanceCount, 0, 0);
			}
		}

		void CD3D12Driver::drawMeshBufferNormals(const scene::IMeshBuffer* mb, f32 length, SColor color)
		{
			if (!mb || mb->getVertexBufferCount() == 0)
				return;
			scene::IVertexBuffer* vb = mb->getVertexBuffer(0);
			if (!vb || vb->getVertexCount() == 0)
				return;

			// Same reduced scope as drawMeshBuffer(): assumes EVT_STANDARD/S3DVertex.
			const S3DVertex* src = static_cast<const S3DVertex*>(vb->getVertices());
			std::vector<S3DVertex> lines;
			lines.reserve(static_cast<size_t>(vb->getVertexCount()) * 2);
			for (u32 i = 0; i < vb->getVertexCount(); ++i)
			{
				const core::vector3df& p = src[i].Pos;
				core::vector3df n = p + src[i].Normal * length;
				lines.push_back(S3DVertex(p.X, p.Y, p.Z, 0, 0, 0, color, 0, 0));
				lines.push_back(S3DVertex(n.X, n.Y, n.Z, 0, 0, 0, color, 0, 0));
			}

			SMaterial m; // default: Lighting/texture/etc. not relevant, ZBuffer is still tested normally
			m.Lighting = false;
			m.setTexture(0, nullptr);
			drawImmediate(lines.data(), static_cast<u32>(lines.size()), D3D_PRIMITIVE_TOPOLOGY_LINELIST,
				m, Matrices[ETS_WORLD], Matrices[ETS_VIEW], Matrices[ETS_PROJECTION]);
		}

		void CD3D12Driver::draw3DLine(const core::vector3df& start, const core::vector3df& end, SColor color)
		{
			S3DVertex verts[2] = {
				S3DVertex(start.X, start.Y, start.Z, 0, 0, 0, color, 0, 0),
				S3DVertex(end.X, end.Y, end.Z, 0, 0, 0, color, 0, 0)
			};
			// Contract documented in IVideoDriver.h: draws with the CURRENT material/transform
			// (the caller is expected to have set them before the call).
			drawImmediate(verts, 2, D3D_PRIMITIVE_TOPOLOGY_LINELIST, Material,
				Matrices[ETS_WORLD], Matrices[ETS_VIEW], Matrices[ETS_PROJECTION]);
		}

		void CD3D12Driver::draw3DTriangle(const core::triangle3df& triangle, SColor color)
		{
			S3DVertex verts[3] = {
				S3DVertex(triangle.pointA.X, triangle.pointA.Y, triangle.pointA.Z, 0, 0, 0, color, 0, 0),
				S3DVertex(triangle.pointB.X, triangle.pointB.Y, triangle.pointB.Z, 0, 0, 0, color, 0, 0),
				S3DVertex(triangle.pointC.X, triangle.pointC.Y, triangle.pointC.Z, 0, 0, 0, color, 0, 0)
			};
			drawImmediate(verts, 3, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, Material,
				Matrices[ETS_WORLD], Matrices[ETS_VIEW], Matrices[ETS_PROJECTION]);
		}

		void CD3D12Driver::draw3DBox(const core::aabbox3d<f32>& box, SColor color)
		{
			const core::vector3df& mn = box.MinEdge;
			const core::vector3df& mx = box.MaxEdge;
			const core::vector3df v[8] = {
				core::vector3df(mn.X, mn.Y, mn.Z), core::vector3df(mx.X, mn.Y, mn.Z),
				core::vector3df(mx.X, mx.Y, mn.Z), core::vector3df(mn.X, mx.Y, mn.Z),
				core::vector3df(mn.X, mn.Y, mx.Z), core::vector3df(mx.X, mn.Y, mx.Z),
				core::vector3df(mx.X, mx.Y, mx.Z), core::vector3df(mn.X, mx.Y, mx.Z)
			};
			static const u32 edges[12][2] = {
				{0,1},{1,2},{2,3},{3,0}, // Z=min face
				{4,5},{5,6},{6,7},{7,4}, // Z=max face
				{0,4},{1,5},{2,6},{3,7}  // vertical edges connecting the two faces
			};
			S3DVertex verts[24];
			for (u32 i = 0; i < 12; ++i)
			{
				const core::vector3df& a = v[edges[i][0]];
				const core::vector3df& b = v[edges[i][1]];
				verts[i * 2]     = S3DVertex(a.X, a.Y, a.Z, 0, 0, 0, color, 0, 0);
				verts[i * 2 + 1] = S3DVertex(b.X, b.Y, b.Z, 0, 0, 0, color, 0, 0);
			}
			drawImmediate(verts, 24, D3D_PRIMITIVE_TOPOLOGY_LINELIST, Material,
				Matrices[ETS_WORLD], Matrices[ETS_VIEW], Matrices[ETS_PROJECTION]);
		}

		// ============================ Phase 5: render target management ============================

		u32 CD3D12Driver::queryMultisampleLevels(ECOLOR_FORMAT format, u32 numSamples) const
		{
			// Same contract as CD3D11Driver::queryMultisampleLevels() -- 0 if numSamples
			// isn't supported for this format (getD3D12ColorFormat() already returns UNKNOWN for an
			// uncovered ECOLOR_FORMAT, and CheckFeatureSupport then fails cleanly on it).
			D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels = {};
			levels.Format = getD3D12ColorFormat(format);
			levels.SampleCount = numSamples;
			levels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;

			if (!Device)
				return 0;

			HRESULT hr = Device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
				&levels, sizeof(levels));
			if (FAILED(hr) || levels.NumQualityLevels == 0)
				return 0;

			return levels.NumQualityLevels;
		}

		bool CD3D12Driver::copyTexture(ITexture* dest, ITexture* source)
		{
			if (!dest || !source || dest == source)
				return false;

			if (dest->getSize() != source->getSize() ||
				dest->getColorFormat() != source->getColorFormat())
			{
				os::Printer::log("copyTexture needs matching size and format.", ELL_ERROR);
				return false;
			}

			CD3D12Texture* d = static_cast<CD3D12Texture*>(dest);
			CD3D12Texture* s = static_cast<CD3D12Texture*>(source);
			if (!d->getResource() || !s->getResource() || !CommandList)
				return false;

			// Unlike D3D11 the copy states must be requested explicitly; transitionTo()
			// is a no-op when the resource already rests in the state asked for.
			d->transitionTo(CommandList.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
			s->transitionTo(CommandList.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
			CommandList->CopyResource(d->getResource(), s->getResource());
			return true;
		}

		void CD3D12Driver::clearZBuffer()
		{
			// Targets the DSV actually bound by the last setRenderTarget() (back buffer or
			// a render-target-texture's dedicated depth buffer), not always DSVHandle (back buffer).
			// Clears to 0.0f (not 1.0f): this fork uses an inverted depth convention
			// (CMatrix4::buildProjectionMatrixPerspectiveFovLH maps the near plane to ~1 and the far
			// plane to ~0, default SMaterial::ZBuffer=ECFN_GREATER) -- same value as
			// CD3D11Driver::clearZBuffer(), which already clears to 0.0f.
			if (CurrentDSVHandle.ptr != 0)
				CommandList->ClearDepthStencilView(CurrentDSVHandle,
					D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, nullptr);
		}

		bool CD3D12Driver::setRenderTarget(video::ITexture* texture, bool clearBackBuffer,
			bool clearZBufferFlag, SColor color, video::ITexture* depthStencil)
		{
			if (!texture)
			{
				// Always back to the BACK BUFFER -- see the comment on CurrentRenderTarget.
				CurrentRenderTarget = nullptr;
			}
			else
			{
				if (texture->getDriverType() != EDT_DIRECT3D12)
				{
					os::Printer::log("CD3D12Driver::setRenderTarget: texture non D3D12", ELL_ERROR);
					return false;
				}
				CD3D12Texture* d3dTex = static_cast<CD3D12Texture*>(texture);
				if (!d3dTex->hasRenderTargetView())
				{
					os::Printer::log("CD3D12Driver::setRenderTarget: texture sans RTV"
						" (pas creee via addRenderTargetTexture())", ELL_ERROR);
					return false;
				}
				CurrentRenderTarget = texture;
			}

			CurrentRenderTargetSize = CurrentRenderTarget ? CurrentRenderTarget->getSize() : WindowSize;

			D3D12_CPU_DESCRIPTOR_HANDLE rtv;
			D3D12_CPU_DESCRIPTOR_HANDLE* dsvPtr = nullptr;
			if (CurrentRenderTarget)
			{
				CD3D12Texture* d3dTex = static_cast<CD3D12Texture*>(CurrentRenderTarget);
				transitionTexture(d3dTex, D3D12_RESOURCE_STATE_RENDER_TARGET);
				rtv = d3dTex->getRenderTargetView();
				CurrentRTVCount = 1;
				CurrentRTVFormats[0] = d3dTex->getDxgiFormat();
				CurrentRTVSampleCount = d3dTex->getSampleCount();

				// Explicit depthStencil (post-process and deferred rendering paths do this on the
				// D3D11 side): now actually honored -- a
				// CD3D12Texture created with a depth format carries a real DSV (see
				// CD3D12Texture::createDepthStencilView()). Previously, this parameter was ignored with a
				// warning and the caller received the auto-managed depth buffer, which made it
				// impossible to read back the G-buffer's depth.
				CD3D12Texture* explicitDepth = static_cast<CD3D12Texture*>(depthStencil);
				if (explicitDepth && explicitDepth->hasDepthStencilView())
				{
					transitionTexture(explicitDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
					CurrentDSVHandle = explicitDepth->getDepthStencilView();
					// The PSO's format must match THIS texture, not DepthStencilFormat -- see
					// CurrentDSVFormat.
					CurrentDSVFormat = explicitDepth->getDxgiFormat();
					if (clearZBufferFlag)
					{
						CommandList->ClearDepthStencilView(CurrentDSVHandle,
							D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, nullptr);
					}
					dsvPtr = &CurrentDSVHandle;
				}
				else if (depthStencil && !WarnedDepthStencilWithoutDSV)
				{
					os::Printer::log("CD3D12Driver::setRenderTarget: la texture depthStencil fournie"
						" n'a pas de DSV (creee avec un format non-profondeur ?), repli sur le depth"
						" buffer auto-gere", ELL_WARNING);
					WarnedDepthStencilWithoutDSV = true;
				}

				// checkRTTDepthBuffer() only pools single-slice buffers
				// (DepthOrArraySize=1) -- binding them to an array RTV (D3D12_RTV_DIMENSION_
				// TEXTURE2DARRAY, ArraySize=NumberOfArraySlices) would be a view-size mismatch
				// that D3D12 rejects. Parity with CD3D11Driver::setRenderTarget(), which for the
				// same reason binds no dsview when tex->getTextureType() == ETT_2D_ARRAY.
				if (dsvPtr)
				{
					// already resolved from depthStencil above
				}
				else if (d3dTex->getTextureType() == ETT_2D_ARRAY)
				{
					CurrentDSVHandle = {};
				}
				else
				{
					// The DSV is always bound as soon as a depth buffer exists for this
					// target -- clearZBufferFlag ONLY controls the clear (see
					// CD3D11Driver::setRenderTarget(), which binds dsview unconditionally and
					// only uses clearZBuffer to decide whether to call this->clearZBuffer()
					// afterward). The old code before this pass only bound dsvPtr when
					// clearZBufferFlag was true, which silently disabled the
					// depth-test on every setRenderTarget(tex, ..., false, ...) call -- a parity
					// gap with D3D11, fixed here.
					SD3D12RTTDepthBuffer* rttDepth = checkRTTDepthBuffer(CurrentRenderTargetSize, CurrentRTVSampleCount);
					if (rttDepth)
					{
						CurrentDSVHandle = rttDepth->DSVHandle;
						CurrentDSVFormat = DepthStencilFormat; // the pool is always created in this format
						if (clearZBufferFlag)
						{
							CommandList->ClearDepthStencilView(CurrentDSVHandle,
								D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, nullptr);
						}
						dsvPtr = &CurrentDSVHandle;
					}
					else
					{
						CurrentDSVHandle = {};
					}
				}
			}
			else
			{
				rtv = Frames[CurrentFrameIndex].RTVHandle;
				CurrentRTVCount = 1;
				CurrentRTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
				// The back buffer always stays single-sample in this pass (see the
				// CurrentRTVSampleCount comment) -- without this explicit reset, returning to the back
				// buffer after drawing into an MSAA RTT would leave CurrentRTVSampleCount > 1,
				// throwing off the next buildPSOKeyFromMaterial() (PSO SampleDesc.Count != 1 while
				// the back buffer itself is 1).
				CurrentRTVSampleCount = 1;
				CurrentDSVHandle = DSVHandle;
				CurrentDSVFormat = DepthStencilFormat; // the back buffer's depth buffer
				if (HasDepthStencilBuffer) // see the comment above (RTT branch)
				{
					if (clearZBufferFlag)
					{
						CommandList->ClearDepthStencilView(CurrentDSVHandle,
							D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, nullptr);
					}
					dsvPtr = &CurrentDSVHandle;
				}
			}

			// buildPSOKeyFromMaterial() must reflect what's ACTUALLY bound by the
			// OMSetRenderTargets() below, not what beginScene() had decided at the start of the
			// frame (see its own comment on CurrentSceneHasDepthStencil) -- without this, a
			// draw following this setRenderTarget() onto a target with no depth buffer would build a PSO
			// with DepthTestEnable/DSVFormat not UNKNOWN while no DSV is bound, rejected by
			// D3D12 ("A null depth stencil view may only be bound when the pipeline state depth
			// stencil format is UNKNOWN").
			CurrentSceneHasDepthStencil = (dsvPtr != nullptr);

			if (clearBackBuffer)
			{
				FLOAT clearColor[4] = {
					color.getRed() / 255.0f, color.getGreen() / 255.0f,
					color.getBlue() / 255.0f, color.getAlpha() / 255.0f
				};
				CommandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
			}
			CommandList->OMSetRenderTargets(1, &rtv, FALSE, dsvPtr);
			CurrentRTVHandles[0] = rtv; // to re-bind after a flushCommandList()

			D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(CurrentRenderTargetSize.Width),
				static_cast<float>(CurrentRenderTargetSize.Height), 0.0f, 1.0f };
			CommandList->RSSetViewports(1, &viewport);
			// Keeps getViewPort() consistent with what's actually set on the rasterizer -- a
			// different render target resets the viewport to full frame (see setViewPort()).
			ViewPort = core::rect<s32>(0, 0, static_cast<s32>(CurrentRenderTargetSize.Width),
				static_cast<s32>(CurrentRenderTargetSize.Height));
			setScissorFromClip(nullptr);

			return true;
		}

		bool CD3D12Driver::setRenderTarget(const core::array<video::IRenderTarget>& targets,
			const core::array<bool>& clearBackBuffer, bool clearZBufferFlag, SColor color,
			video::ITexture* depthStencil)
		{
			// No target: restores the back buffer, same contract as the single-target overload
			// (see CD3D11Driver::setRenderTarget(array<IRenderTarget>&, ...), which delegates the same way).
			if (targets.empty())
				return setRenderTarget(static_cast<video::ITexture*>(nullptr), true, clearZBufferFlag, color, depthStencil);

			// Real MRT -- up to D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT (8) simultaneous
			// targets, same limit as D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT on the CD3D11Driver side.
			// Validation identical to CD3D11Driver::setRenderTarget() (type/driver/RTV/size against
			// the first entry), truncated at the first problem encountered rather than
			// rejecting everything -- an app that requests 4 targets but only has 3 valid ones still gets
			// MRT with 3 rather than a total failure.
			u32 count = core::min_((u32)D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT, targets.size());
			for (u32 i = 0; i < count; ++i)
			{
				if (targets[i].TargetType != ERT_RENDER_TEXTURE || !targets[i].RenderTexture)
				{
					os::Printer::log("CD3D12Driver::setRenderTarget: texture manquante pour le MRT", ELL_WARNING);
					count = i;
					break;
				}
				if (targets[i].RenderTexture->getDriverType() != EDT_DIRECT3D12)
				{
					os::Printer::log("CD3D12Driver::setRenderTarget: texture non D3D12 dans le MRT", ELL_WARNING);
					count = i;
					break;
				}
				CD3D12Texture* d3dTex = static_cast<CD3D12Texture*>(targets[i].RenderTexture);
				if (!d3dTex->hasRenderTargetView())
				{
					os::Printer::log("CD3D12Driver::setRenderTarget: texture sans RTV dans le MRT"
						" (pas creee via addRenderTargetTexture())", ELL_WARNING);
					count = i;
					break;
				}
				if (targets[0].RenderTexture->getSize() != targets[i].RenderTexture->getSize())
				{
					os::Printer::log("CD3D12Driver::setRenderTarget: taille incoherente dans le MRT", ELL_WARNING);
					count = i;
					break;
				}
				// All simultaneous targets must share the same SampleDesc.Count
				// (D3D12 requires this just as much as the size above, and the shared depth buffer
				// chosen further below via checkRTTDepthBuffer() can in any case only match
				// one).
				if (static_cast<CD3D12Texture*>(targets[0].RenderTexture)->getSampleCount() != d3dTex->getSampleCount())
				{
					os::Printer::log("CD3D12Driver::setRenderTarget: SampleCount incoherent dans le MRT", ELL_WARNING);
					count = i;
					break;
				}
			}
			if (count == 0)
			{
				os::Printer::log("CD3D12Driver::setRenderTarget: aucune cible MRT valide", ELL_ERROR);
				return false;
			}

			CurrentRenderTarget = targets[0].RenderTexture;
			CurrentRenderTargetSize = CurrentRenderTarget->getSize();
			CurrentRTVCount = count;
			CurrentRTVSampleCount = static_cast<CD3D12Texture*>(targets[0].RenderTexture)->getSampleCount();

			D3D12_CPU_DESCRIPTOR_HANDLE rtvs[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
			for (u32 i = 0; i < count; ++i)
			{
				CD3D12Texture* d3dTex = static_cast<CD3D12Texture*>(targets[i].RenderTexture);
				transitionTexture(d3dTex, D3D12_RESOURCE_STATE_RENDER_TARGET);
				rtvs[i] = d3dTex->getRenderTargetView();
				CurrentRTVFormats[i] = d3dTex->getDxgiFormat();

				bool doClear = clearBackBuffer.empty() ? true :
					clearBackBuffer[core::min_(i, clearBackBuffer.size() - 1)];
				if (doClear)
				{
					FLOAT clearColor[4] = {
						color.getRed() / 255.0f, color.getGreen() / 255.0f,
						color.getBlue() / 255.0f, color.getAlpha() / 255.0f
					};
					CommandList->ClearRenderTargetView(rtvs[i], clearColor, 0, nullptr);
				}
			}

			// Explicit depthStencil: same handling as the single-target overload -- this is the
			// central case for deferred rendering (the G-buffer writes its N color targets AND its depth, then
			// the lighting pass reads that same depth back as an SRV).
			D3D12_CPU_DESCRIPTOR_HANDLE* dsvPtr = nullptr;
			CD3D12Texture* explicitDepth = static_cast<CD3D12Texture*>(depthStencil);
			if (explicitDepth && explicitDepth->hasDepthStencilView())
			{
				transitionTexture(explicitDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
				CurrentDSVHandle = explicitDepth->getDepthStencilView();
				// The PSO's format must match THIS texture, not DepthStencilFormat -- see
				// CurrentDSVFormat. This is where it mattered most: the G-buffer's depth is
				// D32_FLOAT, while every PSO used to default to D24_UNORM_S8_UINT.
				CurrentDSVFormat = explicitDepth->getDxgiFormat();
				if (clearZBufferFlag)
				{
					CommandList->ClearDepthStencilView(CurrentDSVHandle,
						D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, nullptr);
				}
				dsvPtr = &CurrentDSVHandle;
			}
			else
			{
				if (depthStencil && !WarnedDepthStencilWithoutDSV)
				{
					os::Printer::log("CD3D12Driver::setRenderTarget: la texture depthStencil fournie"
						" n'a pas de DSV (creee avec un format non-profondeur ?), repli sur le depth"
						" buffer auto-gere", ELL_WARNING);
					WarnedDepthStencilWithoutDSV = true;
				}

				// Auto-managed depth/stencil (sized on targets[0], already verified to be the
				// same size as the other targets above).
				SD3D12RTTDepthBuffer* rttDepth = checkRTTDepthBuffer(CurrentRenderTargetSize, CurrentRTVSampleCount);
				if (rttDepth)
				{
					CurrentDSVHandle = rttDepth->DSVHandle;
					CurrentDSVFormat = DepthStencilFormat; // the pool is always created in this format
					if (clearZBufferFlag)
					{
						CommandList->ClearDepthStencilView(CurrentDSVHandle,
							D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, nullptr);
					}
					dsvPtr = &CurrentDSVHandle;
				}
				else
				{
					CurrentDSVHandle = {};
				}
			}

			// See the equivalent comment in the single-target overload.
			CurrentSceneHasDepthStencil = (dsvPtr != nullptr);

			CommandList->OMSetRenderTargets(count, rtvs, FALSE, dsvPtr);
			for (UINT i = 0; i < count && i < 8; ++i)
				CurrentRTVHandles[i] = rtvs[i]; // to re-bind after a flushCommandList()

			D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(CurrentRenderTargetSize.Width),
				static_cast<float>(CurrentRenderTargetSize.Height), 0.0f, 1.0f };
			CommandList->RSSetViewports(1, &viewport);
			// Keeps getViewPort() consistent with what's actually set on the rasterizer -- a
			// different render target resets the viewport to full frame (see setViewPort()).
			ViewPort = core::rect<s32>(0, 0, static_cast<s32>(CurrentRenderTargetSize.Width),
				static_cast<s32>(CurrentRenderTargetSize.Height));
			setScissorFromClip(nullptr);

			return true;
		}

		bool CD3D12Driver::setRenderTarget(E_RENDER_TARGET target, bool clearTarget,
			bool clearZBufferFlag, SColor color)
		{
			if (target == ERT_FRAME_BUFFER)
				return setRenderTarget(static_cast<video::ITexture*>(nullptr), clearTarget, clearZBufferFlag, color);

			os::Printer::log("CD3D12Driver::setRenderTarget: seul ERT_FRAME_BUFFER est supporte"
				" dans cette passe (pas de stereo/aux buffers)", ELL_WARNING);
			return false;
		}

		// ================================ Phase 5: 2D drawing ================================

		//! Builds the 6 vertices (2 triangles) of a screen quad, corners colored/textured
		//! independently (order: top-left, top-right, bottom-left, bottom-right).
		static void buildQuadVertices(S3DVertex out[6], const core::rect<s32>& destRect,
			const core::rect<f32>& uvRect, SColor colorUL, SColor colorUR, SColor colorLL, SColor colorLR)
		{
			const f32 x0 = f32(destRect.UpperLeftCorner.X), y0 = f32(destRect.UpperLeftCorner.Y);
			const f32 x1 = f32(destRect.LowerRightCorner.X), y1 = f32(destRect.LowerRightCorner.Y);
			out[0] = S3DVertex(x0, y0, 0, 0, 0, 0, colorUL, uvRect.UpperLeftCorner.X, uvRect.UpperLeftCorner.Y);
			out[1] = S3DVertex(x1, y0, 0, 0, 0, 0, colorUR, uvRect.LowerRightCorner.X, uvRect.UpperLeftCorner.Y);
			out[2] = S3DVertex(x0, y1, 0, 0, 0, 0, colorLL, uvRect.UpperLeftCorner.X, uvRect.LowerRightCorner.Y);
			out[3] = out[1];
			out[4] = S3DVertex(x1, y1, 0, 0, 0, 0, colorLR, uvRect.LowerRightCorner.X, uvRect.LowerRightCorner.Y);
			out[5] = out[2];
		}

		//! Converts a rectangle in texture pixels (sourceRect) into normalized UV coordinates.
		static core::rect<f32> toUVRect(const core::rect<s32>& sourceRect, const core::dimension2d<u32>& texSize)
		{
			if (texSize.Width == 0 || texSize.Height == 0)
				return core::rect<f32>(0, 0, 1, 1);
			return core::rect<f32>(
				f32(sourceRect.UpperLeftCorner.X) / texSize.Width,
				f32(sourceRect.UpperLeftCorner.Y) / texSize.Height,
				f32(sourceRect.LowerRightCorner.X) / texSize.Width,
				f32(sourceRect.LowerRightCorner.Y) / texSize.Height);
		}

		void CD3D12Driver::draw2DImage(const video::ITexture* texture, const core::position2d<s32>& destPos)
		{
			if (!texture)
				return;
			core::dimension2d<u32> size = texture->getSize();
			core::rect<s32> destRect(destPos.X, destPos.Y,
				destPos.X + static_cast<s32>(size.Width), destPos.Y + static_cast<s32>(size.Height));
			S3DVertex verts[6];
			buildQuadVertices(verts, destRect, core::rect<f32>(0, 0, 1, 1),
				SColor(255, 255, 255, 255), SColor(255, 255, 255, 255),
				SColor(255, 255, 255, 255), SColor(255, 255, 255, 255));
			SMaterial m = build2DMaterial(false, const_cast<video::ITexture*>(texture));
			setScissorFromClip(nullptr);
			drawImmediate(verts, 6, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, m,
				core::IdentityMatrix, core::IdentityMatrix, build2DProjection());
		}

		void CD3D12Driver::draw2DImage(const video::ITexture* texture, const core::position2d<s32>& destPos,
			const core::rect<s32>& sourceRect, const core::rect<s32>* clipRect,
			SColor color, bool useAlphaChannelOfTexture)
		{
			if (!texture)
				return;
			core::rect<s32> destRect(destPos.X, destPos.Y,
				destPos.X + sourceRect.getWidth(), destPos.Y + sourceRect.getHeight());
			core::rect<f32> uv = toUVRect(sourceRect, texture->getSize());
			S3DVertex verts[6];
			buildQuadVertices(verts, destRect, uv, color, color, color, color);
			bool alphaBlend = useAlphaChannelOfTexture || color.getAlpha() < 255;
			SMaterial m = build2DMaterial(alphaBlend, const_cast<video::ITexture*>(texture));
			setScissorFromClip(clipRect);
			drawImmediate(verts, 6, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, m,
				core::IdentityMatrix, core::IdentityMatrix, build2DProjection());
			if (clipRect)
				setScissorFromClip(nullptr);
		}

		void CD3D12Driver::draw2DImage(const video::ITexture* texture, const core::rect<s32>& destRect,
			const core::rect<s32>& sourceRect, const core::rect<s32>* clipRect,
			const video::SColor* const colors, bool useAlphaChannelOfTexture)
		{
			if (!texture)
				return;
			core::rect<f32> uv = toUVRect(sourceRect, texture->getSize());
			SColor cUL = colors ? colors[0] : SColor(255, 255, 255, 255);
			SColor cUR = colors ? colors[1] : SColor(255, 255, 255, 255);
			SColor cLL = colors ? colors[2] : SColor(255, 255, 255, 255);
			SColor cLR = colors ? colors[3] : SColor(255, 255, 255, 255);
			S3DVertex verts[6];
			buildQuadVertices(verts, destRect, uv, cUL, cUR, cLL, cLR);
			bool alphaBlend = useAlphaChannelOfTexture ||
				cUL.getAlpha() < 255 || cUR.getAlpha() < 255 || cLL.getAlpha() < 255 || cLR.getAlpha() < 255;
			SMaterial m = build2DMaterial(alphaBlend, const_cast<video::ITexture*>(texture));
			setScissorFromClip(clipRect);
			drawImmediate(verts, 6, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, m,
				core::IdentityMatrix, core::IdentityMatrix, build2DProjection());
			if (clipRect)
				setScissorFromClip(nullptr);
		}

		void CD3D12Driver::draw2DImageBatch(const video::ITexture* texture,
			const core::position2d<s32>& pos, const core::array<core::rect<s32> >& sourceRects,
			const core::array<s32>& indices, s32 kerningWidth, const core::rect<s32>* clipRect,
			SColor color, bool useAlphaChannelOfTexture)
		{
			// No real batching in this pass: one D3D12 draw per image (see the
			// header comment on the "2D drawing" section in CD3D12Driver.h). Correct, not
			// optimal -- revisit if the call count becomes a measured performance problem
			// (batching into a single vertex buffer/draw would be the logical next step).
			if (!texture)
				return;
			core::position2d<s32> targetPos = pos;
			for (u32 i = 0; i < indices.size(); ++i)
			{
				s32 idx = indices[i];
				if (idx < 0 || static_cast<u32>(idx) >= sourceRects.size())
					continue;
				const core::rect<s32>& src = sourceRects[idx];
				draw2DImage(texture, targetPos, src, clipRect, color, useAlphaChannelOfTexture);
				targetPos.X += src.getWidth() + kerningWidth;
			}
		}

		void CD3D12Driver::draw2DImageBatch(const video::ITexture* texture,
			const core::array<core::position2d<s32> >& positions, const core::array<core::rect<s32> >& sourceRects,
			const core::rect<s32>* clipRect, SColor color, bool useAlphaChannelOfTexture)
		{
			if (!texture)
				return;
			const u32 count = positions.size() < sourceRects.size() ? positions.size() : sourceRects.size();
			for (u32 i = 0; i < count; ++i)
				draw2DImage(texture, positions[i], sourceRects[i], clipRect, color, useAlphaChannelOfTexture);
		}

		void CD3D12Driver::draw2DRectangle(SColor color, const core::rect<s32>& pos, const core::rect<s32>* clip)
		{
			S3DVertex verts[6];
			buildQuadVertices(verts, pos, core::rect<f32>(0, 0, 1, 1), color, color, color, color);
			SMaterial m = build2DMaterial(color.getAlpha() < 255, nullptr);
			setScissorFromClip(clip);
			drawImmediate(verts, 6, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, m,
				core::IdentityMatrix, core::IdentityMatrix, build2DProjection());
			if (clip)
				setScissorFromClip(nullptr);
		}

		void CD3D12Driver::draw2DRectangle(const core::rect<s32>& pos, SColor colorLeftUp, SColor colorRightUp,
			SColor colorLeftDown, SColor colorRightDown, const core::rect<s32>* clip)
		{
			S3DVertex verts[6];
			buildQuadVertices(verts, pos, core::rect<f32>(0, 0, 1, 1), colorLeftUp, colorRightUp, colorLeftDown, colorRightDown);
			bool alphaBlend = colorLeftUp.getAlpha() < 255 || colorRightUp.getAlpha() < 255 ||
				colorLeftDown.getAlpha() < 255 || colorRightDown.getAlpha() < 255;
			SMaterial m = build2DMaterial(alphaBlend, nullptr);
			setScissorFromClip(clip);
			drawImmediate(verts, 6, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, m,
				core::IdentityMatrix, core::IdentityMatrix, build2DProjection());
			if (clip)
				setScissorFromClip(nullptr);
		}

		void CD3D12Driver::batchDraw2DRectangles(const irr::core::array<core::rect<s32>>& pos,
			const irr::core::array<SColor>& color, const irr::core::array<core::rect<s32>>* clip)
		{
			const u32 count = pos.size();
			for (u32 i = 0; i < count; ++i)
			{
				SColor c = i < color.size() ? color[i] : SColor(255, 255, 255, 255);
				const core::rect<s32>* c2 = (clip && i < clip->size()) ? &(*clip)[i] : nullptr;
				draw2DRectangle(c, pos[i], c2);
			}
		}

		void CD3D12Driver::batchDraw2DRectangles(const irr::core::array<core::rect<s32>>& pos,
			irr::core::array<SColor>& colorLeftUp, irr::core::array<SColor>& colorRightUp,
			irr::core::array<SColor>& colorLeftDown, irr::core::array<SColor>& colorRightDown,
			const irr::core::array<core::rect<s32>>* clip)
		{
			const u32 count = pos.size();
			for (u32 i = 0; i < count; ++i)
			{
				SColor cul = i < colorLeftUp.size() ? colorLeftUp[i] : SColor(255, 255, 255, 255);
				SColor cru = i < colorRightUp.size() ? colorRightUp[i] : SColor(255, 255, 255, 255);
				SColor cld = i < colorLeftDown.size() ? colorLeftDown[i] : SColor(255, 255, 255, 255);
				SColor crd = i < colorRightDown.size() ? colorRightDown[i] : SColor(255, 255, 255, 255);
				const core::rect<s32>* c2 = (clip && i < clip->size()) ? &(*clip)[i] : nullptr;
				draw2DRectangle(pos[i], cul, cru, cld, crd, c2);
			}
		}

		void CD3D12Driver::draw2DRectangleOutline(const core::recti& pos, SColor color)
		{
			const f32 x0 = f32(pos.UpperLeftCorner.X), y0 = f32(pos.UpperLeftCorner.Y);
			const f32 x1 = f32(pos.LowerRightCorner.X), y1 = f32(pos.LowerRightCorner.Y);
			S3DVertex verts[8] = {
				S3DVertex(x0, y0, 0, 0, 0, 0, color, 0, 0), S3DVertex(x1, y0, 0, 0, 0, 0, color, 0, 0),
				S3DVertex(x1, y0, 0, 0, 0, 0, color, 0, 0), S3DVertex(x1, y1, 0, 0, 0, 0, color, 0, 0),
				S3DVertex(x1, y1, 0, 0, 0, 0, color, 0, 0), S3DVertex(x0, y1, 0, 0, 0, 0, color, 0, 0),
				S3DVertex(x0, y1, 0, 0, 0, 0, color, 0, 0), S3DVertex(x0, y0, 0, 0, 0, 0, color, 0, 0)
			};
			SMaterial m = build2DMaterial(color.getAlpha() < 255, nullptr);
			setScissorFromClip(nullptr);
			drawImmediate(verts, 8, D3D_PRIMITIVE_TOPOLOGY_LINELIST, m,
				core::IdentityMatrix, core::IdentityMatrix, build2DProjection());
		}

		void CD3D12Driver::draw2DLine(const core::position2d<s32>& start, const core::position2d<s32>& end, SColor color)
		{
			S3DVertex verts[2] = {
				S3DVertex(f32(start.X), f32(start.Y), 0, 0, 0, 0, color, 0, 0),
				S3DVertex(f32(end.X), f32(end.Y), 0, 0, 0, 0, color, 0, 0)
			};
			SMaterial m = build2DMaterial(color.getAlpha() < 255, nullptr);
			setScissorFromClip(nullptr);
			drawImmediate(verts, 2, D3D_PRIMITIVE_TOPOLOGY_LINELIST, m,
				core::IdentityMatrix, core::IdentityMatrix, build2DProjection());
		}

		void CD3D12Driver::drawPixel(u32 x, u32 y, const SColor& color)
		{
			core::rect<s32> r(static_cast<s32>(x), static_cast<s32>(y), static_cast<s32>(x) + 1, static_cast<s32>(y) + 1);
			S3DVertex verts[6];
			buildQuadVertices(verts, r, core::rect<f32>(0, 0, 1, 1), color, color, color, color);
			SMaterial m = build2DMaterial(color.getAlpha() < 255, nullptr);
			setScissorFromClip(nullptr);
			drawImmediate(verts, 6, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, m,
				core::IdentityMatrix, core::IdentityMatrix, build2DProjection());
		}

		void CD3D12Driver::draw2DPolygon(core::position2d<s32> center, f32 radius, video::SColor color, s32 vertexCount)
		{
			if (vertexCount < 2)
				return;
			std::vector<S3DVertex> verts;
			verts.reserve(static_cast<size_t>(vertexCount) * 2);
			for (s32 i = 0; i < vertexCount; ++i)
			{
				f32 a0 = (2.0f * core::PI * i) / vertexCount;
				f32 a1 = (2.0f * core::PI * ((i + 1) % vertexCount)) / vertexCount;
				verts.push_back(S3DVertex(center.X + radius * cosf(a0), center.Y + radius * sinf(a0), 0, 0, 0, 0, color, 0, 0));
				verts.push_back(S3DVertex(center.X + radius * cosf(a1), center.Y + radius * sinf(a1), 0, 0, 0, 0, color, 0, 0));
			}
			SMaterial m = build2DMaterial(color.getAlpha() < 255, nullptr);
			setScissorFromClip(nullptr);
			drawImmediate(verts.data(), static_cast<u32>(verts.size()), D3D_PRIMITIVE_TOPOLOGY_LINELIST, m,
				core::IdentityMatrix, core::IdentityMatrix, build2DProjection());
		}

		void CD3D12Driver::draw2DVertexPrimitiveList(const void* vertices, u32 vertexCount,
			const void* indexList, u32 primCount, E_VERTEX_TYPE vType, scene::E_PRIMITIVE_TYPE pType, E_INDEX_TYPE iType)
		{
			if (!vertices || !vertexCount || !primCount)
				return;
			if (vType != EVT_STANDARD)
			{
				os::Printer::log("CD3D12Driver::draw2DVertexPrimitiveList: seul EVT_STANDARD est"
					" supporte dans cette passe", ELL_WARNING);
				return;
			}

			D3D_PRIMITIVE_TOPOLOGY topology;
			u32 verticesToDraw;
			switch (pType)
			{
			case scene::EPT_POINTS:         topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;     verticesToDraw = primCount; break;
			case scene::EPT_LINE_STRIP:     topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;     verticesToDraw = primCount + 1; break;
			case scene::EPT_LINES:          topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;      verticesToDraw = primCount * 2; break;
			case scene::EPT_TRIANGLE_STRIP: topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; verticesToDraw = primCount + 2; break;
			case scene::EPT_TRIANGLES:      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;  verticesToDraw = primCount * 3; break;
			default:
				os::Printer::log("CD3D12Driver::draw2DVertexPrimitiveList: E_PRIMITIVE_TYPE non"
					" supporte dans cette passe (seuls POINTS/LINES/LINE_STRIP/TRIANGLES/"
					"TRIANGLE_STRIP le sont)", ELL_WARNING);
				return;
			}

			const S3DVertex* srcVertices = static_cast<const S3DVertex*>(vertices);
			std::vector<S3DVertex> flattened;
			const S3DVertex* toDraw = srcVertices;
			u32 countToDraw = verticesToDraw;

			if (indexList)
			{
				flattened.resize(verticesToDraw);
				if (iType == EIT_32BIT)
				{
					const u32* idx = static_cast<const u32*>(indexList);
					for (u32 i = 0; i < verticesToDraw; ++i)
						flattened[i] = srcVertices[idx[i]];
				}
				else
				{
					const u16* idx = static_cast<const u16*>(indexList);
					for (u32 i = 0; i < verticesToDraw; ++i)
						flattened[i] = srcVertices[idx[i]];
				}
				toDraw = flattened.data();
			}
			else if (verticesToDraw > vertexCount)
			{
				countToDraw = vertexCount; // safety: no indexList, don't read past the provided array
			}

			SMaterial m = build2DMaterial(false, Material.getTexture(0));
			setScissorFromClip(nullptr);
			drawImmediate(toDraw, countToDraw, topology, m,
				core::IdentityMatrix, core::IdentityMatrix, build2DProjection());
		}

		// ========================== Phase 5: stencil shadow volumes ==========================
		// Two passes draw the shadow volume into the stencil without writing color or depth
		// (RenderTargetWriteMask=0, DepthWriteEnable=false); the area where stencil != 0 is in
		// shadow (see drawStencilShadow()). zpass (see buildShadowVolumeStencilKey()) increments
		// on the standard depth test passing: front faces (cull back) increment, back faces
		// (cull front) decrement. zpass under/over-marks the stencil when the volume isn't fully
		// capped or the camera/near-clip-plane intersects it. zfail avoids that by incrementing
		// on depth-test FAILURE instead, with cull modes swapped relative to zpass (matching
		// CD3D11Driver::drawStencilShadowVolume()): cull front increments, cull back decrements.

		void CD3D12Driver::drawStencilShadowVolume(const core::array<core::vector3df>& triangles, bool zfail, u32 debugDataVisible)
		{
			const u32 count = triangles.size();
			if (!count)
				return;

			std::vector<S3DVertex> verts(count);
			for (u32 i = 0; i < count; ++i)
				verts[i] = S3DVertex(triangles[i].X, triangles[i].Y, triangles[i].Z,
					0, 0, 0, SColor(255, 255, 255, 255), 0, 0);

			D3D12_VERTEX_BUFFER_VIEW vbView = allocateVertices(verts.data(), count, sizeof(S3DVertex));
			if (vbView.SizeInBytes == 0)
				return;

			SPSOKey keyIncr = zfail
				? buildShadowVolumeStencilKey(D3D12_CULL_MODE_FRONT, D3D12_STENCIL_OP_INCR, true)
				: buildShadowVolumeStencilKey(D3D12_CULL_MODE_BACK, D3D12_STENCIL_OP_INCR, false);
			ID3D12PipelineState* psoIncr = getOrCreateAuxPSO(keyIncr);
			if (psoIncr)
			{
				CommandList->SetGraphicsRootSignature(RootSignature.Get());
				CommandList->SetPipelineState(psoIncr);
				CommandList->OMSetStencilRef(0);
				bindTransformsAndTexture(Matrices[ETS_WORLD], Matrices[ETS_VIEW], Matrices[ETS_PROJECTION], nullptr);
				CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				CommandList->IASetVertexBuffers(0, 1, &vbView);
				CommandList->DrawInstanced(count, 1, 0, 0);
			}

			SPSOKey keyDecr = zfail
				? buildShadowVolumeStencilKey(D3D12_CULL_MODE_BACK, D3D12_STENCIL_OP_DECR, true)
				: buildShadowVolumeStencilKey(D3D12_CULL_MODE_FRONT, D3D12_STENCIL_OP_DECR, false);
			ID3D12PipelineState* psoDecr = getOrCreateAuxPSO(keyDecr);
			if (psoDecr)
			{
				CommandList->SetGraphicsRootSignature(RootSignature.Get());
				CommandList->SetPipelineState(psoDecr);
				CommandList->OMSetStencilRef(0);
				bindTransformsAndTexture(Matrices[ETS_WORLD], Matrices[ETS_VIEW], Matrices[ETS_PROJECTION], nullptr);
				CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				CommandList->IASetVertexBuffers(0, 1, &vbView);
				CommandList->DrawInstanced(count, 1, 0, 0);
			}
		}

		void CD3D12Driver::drawStencilShadow(bool clearStencilBuffer, video::SColor leftUpEdge,
			video::SColor rightUpEdge, video::SColor leftDownEdge, video::SColor rightDownEdge)
		{
			core::rect<s32> full(0, 0, static_cast<s32>(CurrentRenderTargetSize.Width), static_cast<s32>(CurrentRenderTargetSize.Height));
			S3DVertex verts[6];
			buildQuadVertices(verts, full, core::rect<f32>(0, 0, 1, 1), leftUpEdge, rightUpEdge, leftDownEdge, rightDownEdge);

			D3D12_VERTEX_BUFFER_VIEW vbView = allocateVertices(verts, 6, sizeof(S3DVertex));
			if (vbView.SizeInBytes != 0)
			{
				SPSOKey key;
				key.VSHash = std::hash<void*>()(getSolidVertexShader());
				key.PSHash = std::hash<void*>()(getSolidPixelShader());
				key.InputLayoutHash = hashInputLayout(kS3DVertexInputLayout, _countof(kS3DVertexInputLayout));
				key.BlendMode = SPSOKey::EBlendMode::AlphaBlend; // alpha of the 4 corners = shadow transparency (see IVideoDriver.h docs)
				key.DepthTestEnable = false;
				key.DepthWriteEnable = false;
				key.CullMode = D3D12_CULL_MODE_NONE;
				key.FillMode = D3D12_FILL_MODE_SOLID;
				key.TopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
				key.RTVFormats[0] = CurrentRTVFormats[0]; // see buildShadowVolumeStencilKey()
				key.SampleCount = CurrentRTVSampleCount; // same
				key.DSVFormat = CurrentDSVFormat; // same reason as buildPSOKeyFromMaterial(), see CurrentDSVFormat
				key.StencilEnable = true;
				key.StencilFunc = D3D12_COMPARISON_FUNC_NOT_EQUAL; // only where the volume has marked the stencil (!= 0)
				key.StencilFailOp = D3D12_STENCIL_OP_KEEP;
				key.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
				key.StencilPassOp = D3D12_STENCIL_OP_KEEP; // this pass doesn't modify the stencil, only the color
				key.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

				ID3D12PipelineState* pso = getOrCreateAuxPSO(key);
				if (pso)
				{
					CommandList->SetGraphicsRootSignature(RootSignature.Get());
					CommandList->SetPipelineState(pso);
					CommandList->OMSetStencilRef(0);
					bindTransformsAndTexture(core::IdentityMatrix, core::IdentityMatrix, build2DProjection(), NullTexture);
					setScissorFromClip(nullptr);
					CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
					CommandList->IASetVertexBuffers(0, 1, &vbView);
					CommandList->DrawInstanced(6, 1, 0, 0);
				}
			}

			if (clearStencilBuffer && HasDepthStencilBuffer)
				CommandList->ClearDepthStencilView(DSVHandle, D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
		}

		// ============================ Phase 5: occlusion queries ============================

		void CD3D12Driver::addOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node, const scene::IMesh* mesh)
		{
			if (!node)
				return;
			if (!mesh)
			{
				os::Printer::log("CD3D12Driver::addOcclusionQuery: mesh nul non supporte dans"
					" cette passe (pas de recherche automatique via le noeud)", ELL_WARNING);
				return;
			}
			if (OcclusionQueries.find(node) != OcclusionQueries.end())
				return; // already registered

			if (FreeOcclusionSlots.empty())
			{
				os::Printer::log("CD3D12Driver::addOcclusionQuery: capacite d'occlusion queries"
					" atteinte (voir OcclusionQueryCapacity)", ELL_WARNING);
				return;
			}

			SD3D12OcclusionQuery query;
			query.Slot = FreeOcclusionSlots.back();
			FreeOcclusionSlots.pop_back();

			for (u32 mbIdx = 0; mbIdx < mesh->getMeshBufferCount(); ++mbIdx)
			{
				scene::IMeshBuffer* buffer = mesh->getMeshBuffer(mbIdx);
				if (!buffer || buffer->getVertexBufferCount() == 0)
					continue;
				scene::IVertexBuffer* vb = buffer->getVertexBuffer(0);
				scene::IIndexBuffer* ib = buffer->getIndexBuffer();
				if (!vb || vb->getVertexCount() == 0)
					continue;
				const S3DVertex* verts = static_cast<const S3DVertex*>(vb->getVertices());

				if (ib && ib->getIndexCount() > 0)
				{
					if (ib->getType() == EIT_32BIT)
					{
						const u32* idx = static_cast<const u32*>(ib->getIndices());
						for (u32 i = 0; i < ib->getIndexCount(); ++i)
							query.Positions.push_back(verts[idx[i]].Pos);
					}
					else
					{
						const u16* idx = static_cast<const u16*>(ib->getIndices());
						for (u32 i = 0; i < ib->getIndexCount(); ++i)
							query.Positions.push_back(verts[idx[i]].Pos);
					}
				}
				else
				{
					for (u32 i = 0; i < vb->getVertexCount(); ++i)
						query.Positions.push_back(verts[i].Pos);
				}
			}

			OcclusionQueries[node] = std::move(query);
		}

		void CD3D12Driver::removeOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node)
		{
			auto it = OcclusionQueries.find(node);
			if (it == OcclusionQueries.end())
				return;
			FreeOcclusionSlots.push_back(it->second.Slot);
			OcclusionQueries.erase(it);
		}

		void CD3D12Driver::removeAllOcclusionQueries()
		{
			for (auto& kv : OcclusionQueries)
				FreeOcclusionSlots.push_back(kv.second.Slot);
			OcclusionQueries.clear();
		}

		void CD3D12Driver::runOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node, bool visible)
		{
			if (!node)
				return;
			auto it = OcclusionQueries.find(node);
			if (it == OcclusionQueries.end())
				return;
			SD3D12OcclusionQuery& q = it->second;
			if (q.Positions.empty())
				return;

			std::vector<S3DVertex> verts(q.Positions.size());
			for (size_t i = 0; i < q.Positions.size(); ++i)
				verts[i] = S3DVertex(q.Positions[i].X, q.Positions[i].Y, q.Positions[i].Z,
					0, 0, 0, SColor(255, 255, 255, 255), 0, 0);

			D3D12_VERTEX_BUFFER_VIEW vbView = allocateVertices(verts.data(), static_cast<u32>(verts.size()), sizeof(S3DVertex));
			if (vbView.SizeInBytes == 0)
				return;

			// visible: the test geometry is also drawn normally (useful for debugging occlusion
			// volumes visually); false (normal usage): neither color nor depth is written, only
			// the occlusion result counts (same auxiliary-PSO technique as stencil shadow volume
			// marking: RenderTargetWriteMask=0).
			// Dedicated occlusion material, NOT `Material` (the driver's current state, i.e.
			// whatever the last draw left behind) -- this is what CNullDriver::runOcclusionQuery()
			// does for D3D9/D3D11, and relying on the current state made the query
			// non-deterministic. Concretely, a lens-flare-style node forces ZBuffer = ECFN_ALWAYS on
			// its own material: if that is what's sitting in Material at query time, the depth
			// test is disabled, EVERY pixel of the queried volume passes, and the node is reported
			// as never occluded.
			//
			// The queried node has usually already drawn itself (and so already wrote its own
			// depth) before the query runs -- see CNullDriver::runOcclusionQuery()'s equivalent
			// comment. A fresh SMaterial's default ZBuffer (ECFN_GREATER, fully inverted-Z in
			// this fork -- do NOT "fix" to ECFN_LESSEQUAL despite SMaterial.h's stale comment) is
			// a STRICT test: it rejects fragments landing exactly on that already-stored depth,
			// collapsing the query to a handful of z-fighting-noise pixels. ECFN_GREATEREQUAL
			// accepts the equality, same as CD3D9Driver/CNullDriver::runOcclusionQuery() do for
			// D3D9/D3D11.
			SMaterial occlusionMaterial;
			occlusionMaterial.Lighting = false;
			occlusionMaterial.AntiAliasing = 0;
			occlusionMaterial.ColorMask = ECP_NONE;
			occlusionMaterial.GouraudShading = false;
			occlusionMaterial.ZWriteEnable = false;
			occlusionMaterial.ZBuffer = ECFN_GREATEREQUAL;
			// Both cases go through bindDrawState(), the ORDINARY draw path -- the same choice
			// CNullDriver::runOcclusionQuery() makes for D3D9/D3D11, where the query is nothing more
			// than setMaterial() + drawMeshBuffer() and the driver binds whatever that material needs.
			//
			// This replaces a hand-rolled sequence (pick a PSO, SetGraphicsRootSignature,
			// SetPipelineState, bindTransformsAndTexture, bindLighting, bindFog) that had to be kept in
			// step with bindDrawState() by hand, and repeatedly wasn't. Each omission produced a bug
			// whose symptom appeared far from this function and only under some orderings:
			//   - no bindLighting()/bindFog() -> b3/b4 kept whatever a PRIOR draw left in them, and
			//     nothing at all when the query was the frame's first draw ("Uninitialized root
			//     argument accessed" under GPU-based validation, then DXGI_ERROR_DEVICE_HUNG). Fixed
			//     here once already, by adding the two calls rather than the cause.
			//   - no OnSetConstants()/allocateUserCBVTable() -> a queried node whose material is a
			//     cbuffer-reading user shader drew with the previous draw's user CBV tables, or with
			//     uninitialized ones. Invisible whenever the node happened to draw itself immediately
			//     before, which is the common case -- hence "random".
			// Deferring to the one function that knows the whole bind protocol removes the class,
			// not the instance: anything added to bindDrawState() later is automatically honoured here.
			//
			// The !visible PSO comes out IDENTICAL to the getOrCreateAuxPSO() one this replaces:
			// occlusionMaterial is EMT_SOLID, so chooseVertexShaderForMaterial()/
			// choosePixelShaderForMaterial() resolve to the very blobs getSolidVertexShader()/
			// getSolidPixelShader() return (both read getNativeRenderer(EMT_SOLID)); a null descriptor
			// resolves to the same kS3DVertexInputLayout; and ColorMask=ECP_NONE/ZWriteEnable=false
			// already give buildPSOKeyFromMaterial() the RenderTargetWriteMask=0/DepthWriteEnable=false
			// the old code re-applied by hand afterwards.
			//
			// bindDrawState() returning false means no PSO could be built -- same outcome as the
			// `if (!pso) return;` it replaces, so the query simply does not run.
			//
			// REMAINING DIVERGENCE from CNullDriver, deliberately left alone (pre-existing, and
			// `visible` is a debug-visualisation flag): CNullDriver draws each mesh buffer with ITS OWN
			// material (setMaterial(mesh->getMeshBuffer(i)->getMaterial())), whereas addOcclusionQuery()
			// above flattens every buffer into one position-only S3DVertex list and forgets their
			// materials, so this reaches for `Material` -- the driver's current state, i.e. whatever
			// the last draw left behind. That is exactly the arbitrariness the occlusionMaterial
			// comment above condemns for the !visible path. It also means a `visible` query whose
			// leftover Material needs a richer vertex format (EVT_TANGENTS: VSMainTangents declares
			// TANGENT/BINORMAL, absent from kS3DVertexInputLayout) fails PSO creation and silently
			// skips. Storing the per-buffer materials in SD3D12OcclusionQuery would be the real fix.
			const SMaterial& drawMaterial = visible ? Material : occlusionMaterial;
			if (!bindDrawState(drawMaterial, node->getAbsoluteTransformation(),
				Matrices[ETS_VIEW], Matrices[ETS_PROJECTION]))
				return;

			CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			CommandList->IASetVertexBuffers(0, 1, &vbView);

			// OCCLUSION rather than BINARY_OCCLUSION: IVideoDriver::getOcclusionQueryResult()'s
			// contract is "the number of visible pixels/fragments" (see CD3D9Driver, which uses
			// D3DQUERYTYPE_OCCLUSION) -- callers rely on that count to scale an intensity (e.g.
			// a lens flare scaling its strength by the visible pixel count). The query heap is already
			// D3D12_QUERY_HEAP_TYPE_OCCLUSION, which covers both query types.
			CommandList->BeginQuery(OcclusionQueryHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, q.Slot);
			CommandList->DrawInstanced(static_cast<u32>(verts.size()), 1, 0, 0);
			CommandList->EndQuery(OcclusionQueryHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, q.Slot);
			CommandList->ResolveQueryData(OcclusionQueryHeap.Get(), D3D12_QUERY_TYPE_OCCLUSION, q.Slot, 1,
				OcclusionReadback.Get(), static_cast<UINT64>(q.Slot) * sizeof(UINT64));

			// Only valid once THIS frame (the one containing this ResolveQueryData) has been
			// executed by the GPU -- the current frame's endScene() will signal Fence with this
			// value (FenceValue+1, same calculation as frame.FenceValue = signalFence() in
			// endScene()). No mid-frame flush/wait here, see updateOcclusionQuery().
			q.PendingFenceValue = FenceValue + 1;
		}

		void CD3D12Driver::runAllOcclusionQueries(bool visible)
		{
			for (auto& kv : OcclusionQueries)
				runOcclusionQuery(kv.first, visible);
		}

		void CD3D12Driver::updateOcclusionQuery(std::shared_ptr<irr::scene::ISceneNode> node, bool block)
		{
			if (!node)
				return;
			auto it = OcclusionQueries.find(node);
			if (it == OcclusionQueries.end())
				return;
			SD3D12OcclusionQuery& q = it->second;
			if (q.PendingFenceValue == 0)
				return; // never launched (runOcclusionQuery() not called yet)

			if (Fence->GetCompletedValue() < q.PendingFenceValue)
			{
				if (!block)
					return; // "Update might not occur in this case" -- documented in IVideoDriver.h
				Fence->SetEventOnCompletion(q.PendingFenceValue, FenceEvent);
				::WaitForSingleObject(FenceEvent, INFINITE);
			}

			const UINT64* results = static_cast<const UINT64*>(OcclusionReadbackMapped);
			q.LastResult = static_cast<u32>(results[q.Slot]);
			q.PendingFenceValue = 0;
		}

		void CD3D12Driver::updateAllOcclusionQueries(bool block)
		{
			for (auto& kv : OcclusionQueries)
				updateOcclusionQuery(kv.first, block);
		}

		u32 CD3D12Driver::getOcclusionQueryResult(std::shared_ptr<scene::ISceneNode> node) const
		{
			auto it = OcclusionQueries.find(node);
			if (it == OcclusionQueries.end())
				return ~0u; // "no query for this node", NOT "zero pixels visible" -- see CD3D9Driver
			return it->second.LastResult;
		}

		// Point de branchement unique des textures sur CNullDriver (voir CD3D12Driver.h) : la
		// base a deja lu le fichier / prepare l'IImage et gere le cache refcounte autour de cet
		// appel -- il ne reste qu'a fabriquer la ressource GPU. Retourne nullptr (plutot qu'une
		// CD3D12Texture a moitie construite) si D3D12 n'a pas pu creer la ressource : la base
		// propage alors l'echec a l'appelant au lieu de le mettre en cache.
		ITexture* CD3D12Driver::createDeviceDependentTexture(IImage* surface, const io::path& name, void* mipmapData)
		{
			if (!surface)
				return nullptr;

			CD3D12Texture* texture = new CD3D12Texture(surface, ResourceOwner, TextureCreationFlags, name);
			if (!texture->hasDeviceResource())
			{
				os::Printer::log("CD3D12Driver::createDeviceDependentTexture: creation de la ressource D3D12 impossible", name, ELL_ERROR);
				texture->drop();
				return nullptr;
			}
			return texture;
		}

		ITexture* CD3D12Driver::createDeviceDependentTexture(const core::array<ITexture*>& surfaces,
			const E_TEXTURE_TYPE Type, const io::path& name, void* mipmapData)
		{
			const u32 nslices = surfaces.size();

			// CD3D12Texture(const array<ITexture*>&, ...) trusts the caller on the
			// face count -- validate here, the only place both paths go through
			// (the base's getTexture(files[], Type) and a direct call).
			if (Type == ETT_CUBE && nslices != 6)
			{
				os::Printer::log("CD3D12Driver::createDeviceDependentTexture: ETT_CUBE necessite exactement 6 faces", ELL_ERROR);
				return nullptr;
			}
			if (Type == ETT_CUBE_ARRAY && (nslices == 0 || (nslices % 6) != 0))
			{
				os::Printer::log("CD3D12Driver::createDeviceDependentTexture: ETT_CUBE_ARRAY necessite un multiple de 6 faces", ELL_ERROR);
				return nullptr;
			}
			for (u32 i = 0; i < nslices; ++i)
			{
				if (!surfaces[i])
				{
					os::Printer::log("CD3D12Driver::createDeviceDependentTexture: slice nulle dans le tableau", ELL_ERROR);
					return nullptr;
				}
			}

			CD3D12Texture* texture = new CD3D12Texture(surfaces, ResourceOwner, Type, name);
			if (!texture->hasDeviceResource())
			{
				os::Printer::log("CD3D12Driver::createDeviceDependentTexture: creation de la ressource D3D12 tableau impossible", name, ELL_ERROR);
				texture->drop();
				return nullptr;
			}
			return texture;
		}

		// ================== Destruction differee des ressources GPU ==================
		// Voir le long commentaire sur retireResource() dans CD3D12Driver.h pour le POURQUOI.

		void CD3D12Driver::retireResource(ComPtr<ID3D12Resource>&& resource)
		{
			if (!resource)
				return;

			// Toujours sur le proprietaire : c'est sa DirectQueue/Fence qui ordonne le travail GPU,
			// y compris celui enregistre par ses contextes differes (meme queue => execution dans
			// l'ordre de soumission, donc un Signal ulterieur sur cette fence couvre bien tout ce
			// qui a ete soumis avant).
			if (ResourceOwner != this)
			{
				ResourceOwner->retireResource(std::move(resource));
				return;
			}

			SRetiredResource retired;
			// FenceValue + 1 = la PROCHAINE valeur que signalFence() emettra. La ressource peut
			// encore etre referencee par la command list en cours d'enregistrement (pas encore
			// soumise, donc pas couverte par la valeur courante) : attendre la prochaine borne
			// couvre a la fois le deja-soumis et l'en-cours.
			retired.FenceValue = FenceValue + 1;
			retired.Resource = std::move(resource);

			std::lock_guard<std::mutex> lock(OwnedRetireMutex);
			OwnedRetiredResources.push_back(std::move(retired));
		}

		void CD3D12Driver::retireResource(ComPtr<ID3D12DescriptorHeap>&& heap)
		{
			if (!heap)
				return;

			if (ResourceOwner != this)
			{
				ResourceOwner->retireResource(std::move(heap));
				return;
			}

			SRetiredResource retired;
			retired.FenceValue = FenceValue + 1;
			retired.DescriptorHeapResource = std::move(heap);

			std::lock_guard<std::mutex> lock(OwnedRetireMutex);
			OwnedRetiredResources.push_back(std::move(retired));
		}

		void CD3D12Driver::retireDescriptor(CD3D12DescriptorHeapAllocator& heap, UINT index)
		{
			if (ResourceOwner != this)
			{
				ResourceOwner->retireDescriptor(heap, index);
				return;
			}

			SRetiredResource retired;
			retired.FenceValue = FenceValue + 1;
			retired.Heap = &heap;
			retired.DescriptorIndex = index;

			std::lock_guard<std::mutex> lock(OwnedRetireMutex);
			OwnedRetiredResources.push_back(std::move(retired));
		}

		void CD3D12Driver::drainRetiredResources(bool force)
		{
			if (ResourceOwner != this)
				return; // le proprietaire draine pour tout le monde

			const UINT64 completed = (Fence && !force) ? Fence->GetCompletedValue() : 0;

			std::lock_guard<std::mutex> lock(OwnedRetireMutex);
			for (size_t i = 0; i < OwnedRetiredResources.size(); )
			{
				SRetiredResource& retired = OwnedRetiredResources[i];
				if (!force && retired.FenceValue > completed)
				{
					++i;
					continue;
				}

				// Le GPU a depasse cette borne : plus aucune command list en vol ne peut referencer
				// cette ressource / ce slot.
				if (retired.Heap)
					retired.Heap->free(retired.DescriptorIndex);
				// (la ComPtr relache la ressource en sortant du vecteur)

				retired = std::move(OwnedRetiredResources.back());
				OwnedRetiredResources.pop_back();
			}
		}

		UINT64 CD3D12Driver::signalFence()
		{
			UINT64 value = ++FenceValue;
			DirectQueue->Signal(Fence.Get(), value);
			return value;
		}

		void CD3D12Driver::waitForFrame(UINT frameIndex)
		{
			UINT64 target = Frames[frameIndex].FenceValue;
			if (target != 0 && Fence->GetCompletedValue() < target)
			{
				Fence->SetEventOnCompletion(target, FenceEvent);
				::WaitForSingleObject(FenceEvent, INFINITE);
			}
		}

		// Voir le commentaire de declaration dans CD3D12Driver.h pour le POURQUOI (relecture CPU d'un
		// rendu encore non soumis). Ici, le COMMENT : une command list resetee ne retient RIEN de ce
		// que la precedente avait lie (heaps shader-visibles, render targets, viewport, scissor,
		// stream-output), il faut donc tout reposer -- exactement les memes bindings que beginScene()
		// pose en debut de frame.
		void CD3D12Driver::flushCommandList()
		{
			if (!SceneOpen)
				return; // command list fermee : rien d'enregistre, rien a soumettre

			CommandList->Close();
			ID3D12CommandList* lists[] = { CommandList.Get() };
			DirectQueue->ExecuteCommandLists(1, lists);

			const UINT64 target = signalFence();
			if (Fence->GetCompletedValue() < target)
			{
				Fence->SetEventOnCompletion(target, FenceEvent);
				::WaitForSingleObject(FenceEvent, INFINITE);
			}

			SD3D12FrameContext& frame = Frames[CurrentFrameIndex];
			// L'allocateur n'est PAS reset : il contient encore la memoire des commandes qu'on vient
			// d'executer, et la frame n'est pas finie -- il continue simplement d'accumuler. Seule la
			// command list est rouverte. (Reset de l'allocateur = beginScene(), une fois par frame.)
			CommandList->Reset(frame.CommandAllocator.Get(), nullptr);

			// Les anneaux (constantes/vertex) et le heap SRV de la frame ne sont PAS remis a zero non
			// plus : leurs curseurs continuent la ou ils en etaient. On pourrait les recycler puisque
			// le GPU a tout consomme, mais un flush est rare (relecture) et repartir de zero
			// invaliderait les descripteurs deja poses pour cette frame.
			ID3D12DescriptorHeap* shaderVisibleHeaps[] = { frame.ShaderVisibleSRVHeap.Get(), ShaderVisibleSamplerHeap.Get() };
			CommandList->SetDescriptorHeaps(2, shaderVisibleHeaps);

			D3D12_CPU_DESCRIPTOR_HANDLE* dsvPtr = CurrentSceneHasDepthStencil ? &CurrentDSVHandle : nullptr;
			CommandList->OMSetRenderTargets(CurrentRTVCount, CurrentRTVHandles, FALSE, dsvPtr);

			// Viewport : celui en vigueur (ViewPort, tenu a jour par setViewPort()/setRenderTarget()/
			// beginScene()), pas forcement le plein cadre -- RmlUI s'en sert pour son scissor.
			D3D12_VIEWPORT viewport = {
				static_cast<float>(ViewPort.UpperLeftCorner.X), static_cast<float>(ViewPort.UpperLeftCorner.Y),
				static_cast<float>(ViewPort.getWidth()), static_cast<float>(ViewPort.getHeight()), 0.0f, 1.0f };
			CommandList->RSSetViewports(1, &viewport);
			setScissorFromClip(nullptr);

			// Meme raison qu'apres le Reset() de beginScene() : SOSetTargets ne survit pas au reset.
			if (CurrentStreamOutputBuffer)
			{
				D3D12_STREAM_OUTPUT_BUFFER_VIEW soView = {};
				soView.BufferLocation = CurrentStreamOutputBuffer->getResource()->GetGPUVirtualAddress();
				soView.SizeInBytes = CurrentStreamOutputBuffer->getResource()->GetDesc().Width;
				soView.BufferFilledSizeLocation = StreamOutputCounter->GetGPUVirtualAddress();
				CommandList->SOSetTargets(0, 1, &soView);
			}
		}

		bool CD3D12Driver::beginScene(bool backBuffer, bool zBuffer, SColor color,
			const SExposedVideoData& videoData, core::rect<s32>* sourceRect)
		{
			// CNullDriver::beginScene() se contente d'incrementer FPSCounter/PrimitivesDrawn ; on
			// garde le compteur a jour maintenant qu'il existe reellement (getFPS()/
			// getPrimitiveCountDrawn() sont herites).
			CNullDriver::beginScene(backBuffer, zBuffer, color, videoData, sourceRect);

			waitForFrame(CurrentFrameIndex);

			// La frame qui vient de se terminer sur le GPU libere ses ressources retirees. C'est le
			// seul endroit ou une ID3D12Resource/un slot de descripteur est reellement relache --
			// voir retireResource().
			drainRetiredResources();

			// une nouvelle frame reprend toujours sur le back buffer (voir
			// l'OMSetRenderTargets(frame.RTVHandle, ...) plus bas), meme si l'appelant a oublie de
			// restaurer la cible precedente (setRenderTarget(0)) avant endScene() — sans ce reset,
			// CurrentRenderTargetSize/CurrentRTVCount/CurrentRTVFormats resteraient ceux de la
			// derniere render-target-texture liee, faussant a la fois le viewport de cette
			// fonction (rtSize, juste plus bas) et les PSO construits pendant la frame
			// (buildPSOKeyFromMaterial() lit CurrentRTVCount/CurrentRTVFormats).
			CurrentRenderTarget = nullptr;
			CurrentRenderTargetSize = WindowSize;
			CurrentRTVCount = 1;
			CurrentRTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
			CurrentRTVSampleCount = 1; // voir le commentaire de CurrentRTVSampleCount
			CurrentDSVHandle = DSVHandle;
			CurrentDSVFormat = DepthStencilFormat; // une frame reprend sur le depth buffer du back buffer

			SD3D12FrameContext& frame = Frames[CurrentFrameIndex];
			frame.CommandAllocator->Reset();
			CommandList->Reset(frame.CommandAllocator.Get(), nullptr);

			// Phase 5 : remet a zero les curseurs des anneaux par frame (voir
			// SD3D12FrameContext) — leur contenu de la frame precedente qui utilisait ce meme
			// slot est deja garanti consomme par le GPU (waitForFrame() ci-dessus) avant qu'on
			// commence a y ecrire par-dessus. Le heap SRV shader-visible doit etre lie une fois
			// par command list (ici, pas par draw) pour que SetGraphicsRootDescriptorTable()
			// dans drawMeshBuffer() puisse resoudre ses handles GPU.
			frame.ConstantRingOffset = 0;
			frame.ShaderVisibleSRVNext = 0;
			frame.VertexRingOffset = 0;
			// le heap sampler shader-visible est persistant (pas remis a zero par
			// frame, voir sa declaration dans CD3D12Driver.h) mais doit tout de meme etre lie a
			// chaque command list au meme titre que le heap SRV — un ID3D12GraphicsCommandList
			// ne retient pas les heaps lies par la command list precedente apres Reset().
			ID3D12DescriptorHeap* shaderVisibleHeaps[] = { frame.ShaderVisibleSRVHeap.Get(), ShaderVisibleSamplerHeap.Get() };
			CommandList->SetDescriptorHeaps(2, shaderVisibleHeaps);

			// Phase 5 : viewport/scissor n'etaient jamais fixes avant cette passe — sans ca,
			// D3D12 ne rasterise rien (contrairement a D3D11, pas de valeur par defaut
			// derivee du render target). Scissor plein cadre par defaut ; setScissorFromClip()
			// le retrecit ponctuellement pour le dessin 2D avec clipRect.
			const core::dimension2d<u32>& rtSize = CurrentRenderTargetSize;
			D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(rtSize.Width), static_cast<float>(rtSize.Height), 0.0f, 1.0f };
			CommandList->RSSetViewports(1, &viewport);
			// Une nouvelle frame repart en plein cadre -- getViewPort() doit le refleter (voir
			// setViewPort()), sinon il rendrait encore le viewport reduit du dernier scissor RmlUI.
			ViewPort = core::rect<s32>(0, 0, static_cast<s32>(rtSize.Width), static_cast<s32>(rtSize.Height));
			D3D12_RECT fullScissor = { 0, 0, static_cast<LONG>(rtSize.Width), static_cast<LONG>(rtSize.Height) };
			CommandList->RSSetScissorRects(1, &fullScissor);

			D3D12_CPU_DESCRIPTOR_HANDLE* dsvPtr = nullptr;
			CurrentSceneHasDepthStencil = zBuffer && HasDepthStencilBuffer;
			if (CurrentSceneHasDepthStencil)
			{
				// STENCIL en plus de DEPTH depuis la Phase 5 (DepthStencilFormat a maintenant un
				// plan stencil, voir CD3D12Driver.h) : sans ca, le stencil garderait les valeurs
				// de la frame precedente et corromprait drawStencilShadowVolume()/
				// drawStencilShadow().
				CommandList->ClearDepthStencilView(DSVHandle,
					D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, nullptr);
				dsvPtr = &static_cast<D3D12_CPU_DESCRIPTOR_HANDLE&>(DSVHandle);
			}

			CurrentSceneHasBackBuffer = backBuffer;
			if (backBuffer)
			{
				CD3DX12_RESOURCE_BARRIER toRT = CD3DX12_RESOURCE_BARRIER::Transition(
					frame.BackBuffer.Get(),
					D3D12_RESOURCE_STATE_PRESENT,
					D3D12_RESOURCE_STATE_RENDER_TARGET);
				CommandList->ResourceBarrier(1, &toRT);

				FLOAT clearColor[4] = {
					color.getRed() / 255.0f, color.getGreen() / 255.0f,
					color.getBlue() / 255.0f, color.getAlpha() / 255.0f
				};
				CommandList->ClearRenderTargetView(frame.RTVHandle, clearColor, 0, nullptr);
				CommandList->OMSetRenderTargets(1, &static_cast<D3D12_CPU_DESCRIPTOR_HANDLE&>(frame.RTVHandle),
					FALSE, dsvPtr);
				CurrentRTVHandles[0] = frame.RTVHandle; // pour re-lier apres un flushCommandList()
			}
			else if (dsvPtr)
			{
				// zBuffer demande sans backBuffer : bind quand meme le depth buffer seul,
				// pour un pass qui n'ecrit que la profondeur (shadow map sur render target
				// externe, etc.) — cas rare mais l'interface IVideoDriver l'autorise.
				CommandList->OMSetRenderTargets(0, nullptr, FALSE, dsvPtr);
			}

			// SOSetTargets()/le barrier vers STREAM_OUT poses par un setStreamOutputBuffer()
			// precedent ne survivent pas au Reset() plus haut — une command list resetee ne
			// retient ni l'etat SOSetTargets ni les barriers enregistres-mais-jamais-soumis via
			// ExecuteCommandLists, au meme titre que les heaps shader-visibles/viewport/scissor
			// re-etablis ci-dessus. Cas concret : un appelant qui fait
			// setStreamOutputBuffer(buffer) PUIS beginScene() (ordre valide et symetrique a
			// D3D11, dont le contexte immediat n'a pas cette contrainte — voir
			// VSGS_StreamOutput_ReadbackValidation) verrait sinon son GS ecrire dans le vide
			// silencieusement, sans qu'aucun barrier D3D12 n'echoue. CurrentState du buffer ment
			// aussi a ce stade (mis a STREAM_OUT par le transitionTo() jete avec l'ancienne
			// command list) — resyncCurrentState() le recale d'abord sur son seul etat de repos
			// possible (VERTEX_AND_CONSTANT_BUFFER, voir setStreamOutputBuffer()) pour que le
			// transitionTo() qui suit emette un vrai barrier plutot que de le sauter en pensant
			// que le buffer y est deja. Meme raisonnement pour StreamOutputCounterState.
			if (CurrentStreamOutputBuffer)
			{
				CurrentStreamOutputBuffer->resyncCurrentState(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
				CurrentStreamOutputBuffer->transitionTo(CommandList.Get(), D3D12_RESOURCE_STATE_STREAM_OUT);

				StreamOutputCounterState = D3D12_RESOURCE_STATE_STREAM_OUT;
				resetStreamOutputCounter();

				D3D12_STREAM_OUTPUT_BUFFER_VIEW soView = {};
				soView.BufferLocation = CurrentStreamOutputBuffer->getResource()->GetGPUVirtualAddress();
				soView.SizeInBytes = CurrentStreamOutputBuffer->getResource()->GetDesc().Width;
				soView.BufferFilledSizeLocation = StreamOutputCounter->GetGPUVirtualAddress();
				CommandList->SOSetTargets(0, 1, &soView);
			}

			SceneOpen = true;
			return true;
		}

		bool CD3D12Driver::endScene()
		{
			// Sans scene ouverte, CommandList est fermee : Close() echouerait, et
			// ExecuteCommandLists() sur une liste dans cet etat retire le device
			// (DXGI_ERROR_INVALID_CALL) -- ce qui fait ensuite echouer TOUTE creation de PSO et de
			// ressource jusqu'a la fin du processus. On refuse donc l'appel plutot que de laisser un
			// appelant mal apparie (endScene() sans beginScene()) detruire le driver. Rien a
			// presenter de toute facon : une liste fermee n'a rien enregistre.
			if (!SceneOpen)
			{
				os::Printer::log("CD3D12Driver::endScene: appele sans beginScene() correspondant"
					" (aucune scene ouverte) -- ignore", ELL_WARNING);
				return false;
			}
			SceneOpen = false;

			SD3D12FrameContext& frame = Frames[CurrentFrameIndex];

			// Only transition back if this scene's beginScene(backBuffer, ...) actually put the
			// back buffer into RENDER_TARGET (see CurrentSceneHasBackBuffer) -- a backBuffer=false
			// scene (e.g. a pure stream-output pass) never left PRESENT, and transitioning
			// "from RENDER_TARGET" here would be a barrier from a state the resource isn't
			// actually in.
			if (CurrentSceneHasBackBuffer)
			{
				CD3DX12_RESOURCE_BARRIER toPresent = CD3DX12_RESOURCE_BARRIER::Transition(
					frame.BackBuffer.Get(),
					D3D12_RESOURCE_STATE_RENDER_TARGET,
					D3D12_RESOURCE_STATE_PRESENT);
				CommandList->ResourceBarrier(1, &toPresent);
			}

			CommandList->Close();
			ID3D12CommandList* lists[] = { CommandList.Get() };
			DirectQueue->ExecuteCommandLists(1, lists);

			UINT presentFlags = (TearingSupported && !Params.Vsync) ? DXGI_PRESENT_ALLOW_TEARING : 0;
			SwapChain->Present(Params.Vsync ? 1 : 0, presentFlags);

			frame.FenceValue = signalFence();

			// memorise QUEL frame context vient d'etre presente avant que
			// CurrentFrameIndex n'avance vers le prochain back buffer a dessiner —
			// createScreenShot() lit ce frame-la, pas CurrentFrameIndex.
			LastPresentedFrameIndex = CurrentFrameIndex;
			HasPresentedFrame = true;

			CurrentFrameIndex = SwapChain->GetCurrentBackBufferIndex();

			// Ce que fait CNullDriver::endScene(), moins updateAllOcclusionQueries() : ce driver a
			// sa propre implementation des occlusion queries et les met deja a jour lui-meme --
			// chainer sur la base les traiterait deux fois. getFPS() (herite) fonctionne desormais
			// reellement.
			FPSCounter.registerFrame(os::Timer::getRealTime(), PrimitivesDrawn);

			// La frame a ete presentee avec succes, c'est ce que ce retour signifie par convention
			// IVideoDriver::endScene().
			return true;
		}

		bool CD3D12Driver::queryFeature(E_VIDEO_DRIVER_FEATURE feature) const
		{
			switch (feature)
			{
			case EVDF_RENDER_TO_TARGET:
				return true; // addRenderTargetTexture()/setRenderTarget() work, see above
			case EVDF_STENCIL_BUFFER:
				return true; // DepthStencilFormat = D24_UNORM_S8_UINT
			case EVDF_OCCLUSION_QUERY:
				return true;
			case EVDF_MIP_MAP:
				return true; // generated via a pixel-shader blit, see createMipGenPipeline()
			case EVDF_POLYGON_OFFSET:
				return true; // D3D12_RASTERIZER_DESC::DepthBias/SlopeScaledDepthBias, always available
			default:
				return false;
			}
		}

		IImage* CD3D12Driver::createScreenShot(video::ECOLOR_FORMAT format, video::E_RENDER_TARGET target)
		{
			if (target != video::ERT_FRAME_BUFFER)
			{
				os::Printer::log("CD3D12Driver::createScreenShot: only ERT_FRAME_BUFFER is supported", ELL_WARNING);
				return nullptr;
			}

			if (format == video::ECF_UNKNOWN)
				format = video::ECF_A8R8G8B8;

			if (format != video::ECF_A8R8G8B8)
			{
				// convertColor() isn't implemented on this driver (stub, see CD3D12Driver.h): no
				// generic format conversion is possible here, unlike CD3D11Driver::createScreenShot().
				// Fail cleanly rather than return a wrong image.
				os::Printer::log("CD3D12Driver::createScreenShot: only ECF_A8R8G8B8 is supported", ELL_WARNING);
				return nullptr;
			}

			if (!HasPresentedFrame)
			{
				os::Printer::log("CD3D12Driver::createScreenShot: no frame presented yet (call after endScene())", ELL_WARNING);
				return nullptr;
			}

			// Ensures the GPU has finished rendering AND presenting this frame before we read its
			// content -- same fence that already guards this back buffer's reuse in beginScene()
			// (see waitForFrame()).
			waitForFrame(LastPresentedFrameIndex);

			ID3D12Resource* backBuffer = Frames[LastPresentedFrameIndex].BackBuffer.Get();
			const u32 width = WindowSize.Width;
			const u32 height = WindowSize.Height;

			D3D12_RESOURCE_DESC destDesc = backBuffer->GetDesc();
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
			UINT numRows = 0;
			UINT64 rowSizeBytes = 0;
			UINT64 totalBytes = 0;
			Device->GetCopyableFootprints(&destDesc, 0, 1, 0, &footprint, &numRows, &rowSizeBytes, &totalBytes);

			// Same scheme as CD3D12Texture::lock(ETLM_READ_ONLY): a D3D12_HEAP_TYPE_READBACK
			// resource sized/laid out via GetCopyableFootprints, copied into with CopyTextureRegion,
			// then Map()'d directly for the caller to read (no extra CPU-side copy).
			D3D12_HEAP_PROPERTIES readbackHeapProps = {};
			readbackHeapProps.Type = D3D12_HEAP_TYPE_READBACK;

			D3D12_RESOURCE_DESC stagingDesc = {};
			stagingDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			stagingDesc.Width = totalBytes;
			stagingDesc.Height = 1;
			stagingDesc.DepthOrArraySize = 1;
			stagingDesc.MipLevels = 1;
			stagingDesc.Format = DXGI_FORMAT_UNKNOWN;
			stagingDesc.SampleDesc = { 1, 0 };
			stagingDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			stagingDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

			ComPtr<ID3D12Resource> staging;
			HRESULT hr = Device->CreateCommittedResource(&readbackHeapProps, D3D12_HEAP_FLAG_NONE, &stagingDesc,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&staging));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver::createScreenShot: CreateCommittedResource (readback) failed", ELL_ERROR);
				return nullptr;
			}

			UploadScope upload(this);
			ID3D12GraphicsCommandList* cmdList = upload.commandList();
			if (!cmdList)
				return nullptr;

			// The back buffer was just presented: it's in D3D12_RESOURCE_STATE_PRESENT (see
			// endScene()). No "CurrentState" tracked for it (unlike CD3D12Texture) -- PRESENT is
			// the only possible state here, outside of an in-progress beginScene()/endScene().
			CD3DX12_RESOURCE_BARRIER toSource = CD3DX12_RESOURCE_BARRIER::Transition(
				backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
			cmdList->ResourceBarrier(1, &toSource);

			D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
			srcLoc.pResource = backBuffer;
			srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			srcLoc.SubresourceIndex = 0;
			D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
			dstLoc.pResource = staging.Get();
			dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			dstLoc.PlacedFootprint = footprint;
			cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

			CD3DX12_RESOURCE_BARRIER toPresent = CD3DX12_RESOURCE_BARRIER::Transition(
				backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
			cmdList->ResourceBarrier(1, &toPresent);

			upload.endAndWait();

			void* mapped = nullptr;
			D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(totalBytes) };
			hr = staging->Map(0, &readRange, &mapped);
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12Driver::createScreenShot: Map (readback) failed", ELL_ERROR);
				return nullptr;
			}

			core::dimension2d<u32> size(width, height);
			IImage* image = new CImage(format, size);

			u8* dst = static_cast<u8*>(image->lock());
			const u8* src = static_cast<const u8*>(mapped);
			const u32 dstPitch = image->getPitch();

			// Back buffer = DXGI_FORMAT_R8G8B8A8_UNORM (R,G,B,A in memory -- see createSwapChain()),
			// while ECF_A8R8G8B8 is a u32 0xAARRGGBB, i.e. B,G,R,A in memory. Same pixel size (4
			// bytes), but R and B are swapped, hence the channel swap below rather than a plain
			// memcpy. CD3D11Driver::createScreenShot() does the exact same swap.
			for (UINT y = 0; y < numRows; ++y)
			{
				const u8* srcRow = src + y * footprint.Footprint.RowPitch;
				u8* dstRow = dst + y * dstPitch;
				for (UINT x = 0; x < width; ++x)
				{
					dstRow[x * 4 + 0] = srcRow[x * 4 + 2]; // B <- R
					dstRow[x * 4 + 1] = srcRow[x * 4 + 1]; // G <- G
					dstRow[x * 4 + 2] = srcRow[x * 4 + 0]; // R <- B
					dstRow[x * 4 + 3] = srcRow[x * 4 + 3]; // A <- A
				}
			}

			image->unlock();

			D3D12_RANGE emptyWrite = { 0, 0 };
			staging->Unmap(0, &emptyWrite);

			return image;
		}

		void CD3D12Driver::OnResize(const core::dimension2d<u32>& size)
		{
			if (!SwapChain || size == WindowSize)
				return;

			UINT64 finalValue = signalFence();
			if (Fence->GetCompletedValue() < finalValue)
			{
				Fence->SetEventOnCompletion(finalValue, FenceEvent);
				::WaitForSingleObject(FenceEvent, INFINITE);
			}
			for (auto& frame : Frames)
				frame.BackBuffer.Reset();

			WindowSize = size;
			// CurrentRenderTargetSize ne suit WindowSize que si la cible courante est le back
			// buffer (CurrentRenderTarget == nullptr) — une render target texture a sa propre
			// taille fixe, inchangee par un resize de fenetre.
			if (!CurrentRenderTarget)
				CurrentRenderTargetSize = size;
			SwapChain->ResizeBuffers(NativeFrameCount, size.Width, size.Height,
				DXGI_FORMAT_R8G8B8A8_UNORM,
				TearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
			CurrentFrameIndex = SwapChain->GetCurrentBackBufferIndex();
			updateRenderTargetViews();

			if (!createDepthStencilBuffer(size.Width, size.Height))
				os::Printer::log("CD3D12Driver::OnResize: createDepthStencilBuffer a echoue", ELL_WARNING);

			// CNullDriverCommon::OnResize() n'existe pas non plus — voir le commentaire dans
			// beginScene().
		}

		// ============================ Phase 2 : buffers vertex/index ============================

		std::shared_ptr<video::IHardwareBuffer> CD3D12Driver::createHardwareBuffer(scene::IIndexBuffer* indexBuffer)
		{
			if (!indexBuffer)
				return nullptr;

			auto existing = indexBuffer->getHardwareBuffer();
			if (existing && existing->getDriverType() == EDT_DIRECT3D12 && !existing->isRequiredUpdate())
				return existing;

			auto buffer = std::make_shared<CD3D12HardwareBuffer>(indexBuffer, this);
			indexBuffer->setHardwareBuffer(buffer);
			return buffer;
		}

		std::shared_ptr<video::IHardwareBuffer> CD3D12Driver::createHardwareBuffer(scene::IVertexBuffer* vertexBuffer)
		{
			if (!vertexBuffer)
				return nullptr;

			auto existing = vertexBuffer->getHardwareBuffer();
			if (existing && existing->getDriverType() == EDT_DIRECT3D12 && !existing->isRequiredUpdate())
				return existing;

			auto buffer = std::make_shared<CD3D12HardwareBuffer>(vertexBuffer, this);
			vertexBuffer->setHardwareBuffer(buffer);
			return buffer;
		}

		// ======================== Phase 2 (suite) : buffers de compute ========================

		std::shared_ptr<video::IHardwareBuffer> CD3D12Driver::createHardwareBuffer(scene::IComputeBuffer* computeBuffer)
		{
			if (!computeBuffer)
				return nullptr;

			auto existing = computeBuffer->getHardwareBuffer();
			if (existing && existing->getDriverType() == EDT_DIRECT3D12 && !existing->isRequiredUpdate())
				return existing;

			auto buffer = std::make_shared<CD3D12HardwareBuffer>(computeBuffer, this);
			computeBuffer->setHardwareBuffer(buffer);
			return buffer;
		}

		void CD3D12Driver::dispatchComputeShader(const core::vector3d<u32>& groupCount,
			scene::IComputeBuffer* Src, scene::IComputeBuffer* Dst)
		{
			if (!Src || !Dst || Src->getStructureCount() == 0 || Dst->getStructureCount() == 0)
				return;

			// getNativeRenderer() borne deja sur le registre du proprietaire et renvoie nullptr
			// pour un index hors bornes -- le test explicite juste apres suffit.
			CD3D12MaterialRenderer* renderer = getNativeRenderer(Material.MaterialType);
			if (!renderer || !renderer->CS)
			{
				os::Printer::log("CD3D12Driver::dispatchComputeShader: le materiau actif n'a pas "
					"de compute shader", ELL_ERROR);
				return;
			}
			ID3D12PipelineState* pso = getOrCreateComputePSO(renderer->CS.Get());
			if (!pso)
				return;

			// Cree/rafraichit les CD3D12HardwareBuffer de Src/Dst si besoin — meme sequence que
			// CD3D11Driver::dispatchComputeShader().
			if (!Src->getHardwareBuffer())
				createHardwareBuffer(Src);
			else if (Src->getHardwareBuffer()->isRequiredUpdate())
				Src->getHardwareBuffer()->update(Src->getHardwareMappingHint(),
					Src->getStructureCount() * Src->getStructureStride(), Src->getBufferPointer());

			if (!Dst->getHardwareBuffer())
				createHardwareBuffer(Dst);
			else if (Dst->getHardwareBuffer()->isRequiredUpdate())
				Dst->getHardwareBuffer()->update(Dst->getHardwareMappingHint(),
					Dst->getStructureCount() * Dst->getStructureStride(), Dst->getBufferPointer());

			CD3D12HardwareBuffer* srcBuf = static_cast<CD3D12HardwareBuffer*>(Src->getHardwareBuffer().get());
			CD3D12HardwareBuffer* dstBuf = static_cast<CD3D12HardwareBuffer*>(Dst->getHardwareBuffer().get());
			if (!srcBuf || !srcBuf->hasShaderResourceView() || !dstBuf || !dstBuf->hasUnorderedAccessView())
			{
				os::Printer::log("CD3D12Driver::dispatchComputeShader: Src/Dst sans vue SRV/UAV "
					"(pas cree par ce driver ?)", ELL_ERROR);
				return;
			}

			// Milestone D (fix) : dispatchComputeShader() ne peut pas s'appuyer sur le CommandList
			// par-frame — il n'est ouvert (Reset()) qu'entre beginScene()/endScene() (voir
			// beginScene()), et l'API generique IVideoDriver::dispatchComputeShader() (ainsi que
			// CD3D11Driver's, immediate-context) n'exige pas d'etre appelee dans cette fenetre —
			// voir les tests ComputeShaderTests*/TerrainShaderTests*, qui dispatchent sans jamais
			// appeler beginScene(). Enregistrer sur un CommandList ferme ne plante pas mais ne
			// soumet rien au GPU (Dispatch() silencieusement perdu). Utilise donc le meme
			// mecanisme synchrone hors-frame que les uploads de texture (beginUpload()/
			// endUploadAndWait()) : ouvre sa propre command list, execute, attend le GPU avant de
			// retourner — coherent avec le fait que l'appelant lit le resultat juste apres via
			// IComputeBuffer::downloadFromGPU().
			UploadScope upload(this);
			ID3D12GraphicsCommandList* cmdList = upload.commandList();
			if (!cmdList)
				return;

			// Reserve everything this dispatch can possibly consume (SRV + UAV descriptor tables
			// plus the CBV table) BEFORE binding the heap to cmdList: if the reserve grows the
			// heap, growShaderVisibleSRVHeap() rebinds the new heap to the per-frame CommandList
			// (rebindShaderVisibleHeaps()), never to this dedicated cmdList -- any growth AFTER
			// the SetDescriptorHeaps() below would leave cmdList bound to a retired heap while
			// the descriptor handles it's given point into the new one. Same reservation pattern
			// as bindDrawState() for the graphics path (see reserveShaderVisibleSRVDescriptors()).
			SD3D12FrameContext& frame = Frames[CurrentFrameIndex];
			if (!reserveShaderVisibleSRVDescriptors(frame, MaxShaderVisibleSRVDescriptorsPerDraw))
				return;

			// SetDescriptorHeaps() is per command list (not per device): the per-frame CommandList
			// does it once per beginScene(), but this dedicated command list never had it.
			ID3D12DescriptorHeap* shaderVisibleHeaps[] = { frame.ShaderVisibleSRVHeap.Get() };
			cmdList->SetDescriptorHeaps(1, shaderVisibleHeaps);

			// createStaticOrComputeResource() cree tout buffer de compute avec les deux
			// vues (UAV+SRV, voir CD3D12HardwareBuffer.h) mais un seul etat de repos —
			// transitionTo() est un no-op si l'etat demande est deja le bon (ex. Dst reste souvent
			// UNORDERED_ACCESS d'un dispatch au suivant).
			srcBuf->transitionTo(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			dstBuf->transitionTo(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			cmdList->SetComputeRootSignature(ComputeRootSignature.Get());
			cmdList->SetPipelineState(pso);

			// Milestone D : meme convention que bindDrawState() pour le pipeline graphique —
			// ActiveMaterialRendererIndex avant le callback pour que
			// getComputeShaderConstantID()/setComputeShaderConstant() (appeles par
			// OnSetConstants()) sachent quel renderer est actif.
			ActiveMaterialRendererIndex = Material.MaterialType;
			if (renderer->CallBack)
			{
				renderer->CallBack->OnSetMaterial(Material);
				renderer->CallBack->OnSetConstants(this, renderer->UserData);
			}

			D3D12_GPU_DESCRIPTOR_HANDLE srvTable = allocateDescriptorTableSlot(srcBuf->getShaderResourceView());
			if (srvTable.ptr != 0)
				cmdList->SetComputeRootDescriptorTable(0, srvTable);

			D3D12_GPU_DESCRIPTOR_HANDLE uavTable = allocateDescriptorTableSlot(dstBuf->getUnorderedAccessView());
			if (uavTable.ptr != 0)
				cmdList->SetComputeRootDescriptorTable(1, uavTable);

			// Compute stays space0-only -- see the "COMPUTE root signature" note on
			// MaxUserShaderRegisterSpaces and compileComputeFromHLSL()'s space0 guard, which already
			// rejected compilation if CSBuffers contained anything else.
			D3D12_GPU_DESCRIPTOR_HANDLE cbvTable = allocateUserCBVTable(renderer->CSBuffers, UserShaderRegisterSpace);
			if (cbvTable.ptr != 0)
				cmdList->SetComputeRootDescriptorTable(2, cbvTable);

			cmdList->Dispatch(groupCount.X, groupCount.Y, groupCount.Z);

			upload.endAndWait();
		}

		// Same shape as dispatchComputeShader() above, but the UAV target is a texture
		// (created via addUAVTexture()) instead of a structured buffer, so the result can
		// be sampled afterward by ordinary Texture2D/Texture2DArray shader code (e.g. an
		// FFT displacement/normal map consumed by WaterDomainShader/pixelMain) rather than
		// read back to the CPU. Still fully synchronous (endAndWait()): the texture is left
		// in PIXEL_SHADER_RESOURCE state before returning, so the very next draw call - on
		// the per-frame CommandList, a different list than this dispatch's own UploadScope -
		// can safely sample it without further synchronization.
		void CD3D12Driver::dispatchComputeShaderToTexture(const core::vector3d<u32>& groupCount,
			scene::IComputeBuffer* Src, ITexture* Dst)
		{
			if (!Src || !Dst || Src->getStructureCount() == 0 || !Dst->isUnorderedAccess())
				return;

			CD3D12MaterialRenderer* renderer = getNativeRenderer(Material.MaterialType);
			if (!renderer || !renderer->CS)
			{
				os::Printer::log("CD3D12Driver::dispatchComputeShaderToTexture: le materiau actif n'a pas "
					"de compute shader", ELL_ERROR);
				return;
			}
			ID3D12PipelineState* pso = getOrCreateComputePSO(renderer->CS.Get());
			if (!pso)
				return;

			if (!Src->getHardwareBuffer())
				createHardwareBuffer(Src);
			else if (Src->getHardwareBuffer()->isRequiredUpdate())
				Src->getHardwareBuffer()->update(Src->getHardwareMappingHint(),
					Src->getStructureCount() * Src->getStructureStride(), Src->getBufferPointer());

			CD3D12HardwareBuffer* srcBuf = static_cast<CD3D12HardwareBuffer*>(Src->getHardwareBuffer().get());
			CD3D12Texture* dstTex = static_cast<CD3D12Texture*>(Dst);
			if (!srcBuf || !srcBuf->hasShaderResourceView() || !dstTex->hasUnorderedAccessView())
			{
				os::Printer::log("CD3D12Driver::dispatchComputeShaderToTexture: Src/Dst sans vue SRV/UAV "
					"(pas cree par ce driver ?)", ELL_ERROR);
				return;
			}

			UploadScope upload(this);
			ID3D12GraphicsCommandList* cmdList = upload.commandList();
			if (!cmdList)
				return;

			SD3D12FrameContext& frame = Frames[CurrentFrameIndex];
			if (!reserveShaderVisibleSRVDescriptors(frame, MaxShaderVisibleSRVDescriptorsPerDraw))
				return;

			ID3D12DescriptorHeap* shaderVisibleHeaps[] = { frame.ShaderVisibleSRVHeap.Get() };
			cmdList->SetDescriptorHeaps(1, shaderVisibleHeaps);

			srcBuf->transitionTo(cmdList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			dstTex->transitionTo(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			cmdList->SetComputeRootSignature(ComputeRootSignature.Get());
			cmdList->SetPipelineState(pso);

			ActiveMaterialRendererIndex = Material.MaterialType;
			if (renderer->CallBack)
			{
				renderer->CallBack->OnSetMaterial(Material);
				renderer->CallBack->OnSetConstants(this, renderer->UserData);
			}

			D3D12_GPU_DESCRIPTOR_HANDLE srvTable = allocateDescriptorTableSlot(srcBuf->getShaderResourceView());
			if (srvTable.ptr != 0)
				cmdList->SetComputeRootDescriptorTable(0, srvTable);

			D3D12_GPU_DESCRIPTOR_HANDLE uavTable = allocateDescriptorTableSlot(dstTex->getUnorderedAccessView());
			if (uavTable.ptr != 0)
				cmdList->SetComputeRootDescriptorTable(1, uavTable);

			D3D12_GPU_DESCRIPTOR_HANDLE cbvTable = allocateUserCBVTable(renderer->CSBuffers, UserShaderRegisterSpace);
			if (cbvTable.ptr != 0)
				cmdList->SetComputeRootDescriptorTable(2, cbvTable);

			cmdList->Dispatch(groupCount.X, groupCount.Y, groupCount.Z);

			// Leave the texture ready to be sampled by the next draw, since endAndWait()
			// below blocks until the GPU has actually finished this dispatch.
			dstTex->transitionTo(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

			upload.endAndWait();
		}

		// ================================ Phase 2 : textures ================================

		// Le verrou est pris ici et relache par le destructeur : tout le bloc
		// Reset()/enregistrement/Close()/Execute()/attente est donc atomique vis-a-vis des autres
		// threads qui uploadent (voir le commentaire sur UploadScope dans CD3D12Driver.h).
		CD3D12Driver::UploadScope::UploadScope(CD3D12Driver* driver)
			: Driver(driver)
			, Lock(driver->UploadMutex)
			, CmdList(driver->beginUpload())
		{
		}

		CD3D12Driver::UploadScope::~UploadScope()
		{
			// Filet de securite pour les appelants qui sortent en plein enregistrement (echec de
			// CreateCommittedResource au milieu d'un upload de volume, par exemple) : sans ca,
			// UploadInProgress resterait vrai et tous les uploads suivants du driver echoueraient.
			// endUploadAndWait() est un no-op si l'appelant a deja appele endAndWait().
			endAndWait();
		}

		void CD3D12Driver::UploadScope::endAndWait()
		{
			if (!CmdList)
				return;
			Driver->endUploadAndWait();
			CmdList = nullptr;
		}

		ID3D12GraphicsCommandList* CD3D12Driver::beginUpload()
		{
			if (UploadInProgress)
			{
				os::Printer::log("CD3D12Driver: beginUpload() appele alors qu'un upload est deja en cours"
					" (endUploadAndWait() manquant ?)", ELL_ERROR);
				return nullptr;
			}
			UploadAllocator->Reset();
			UploadCommandList->Reset(UploadAllocator.Get(), nullptr);
			UploadInProgress = true;
			return UploadCommandList.Get();
		}

		void CD3D12Driver::endUploadAndWait()
		{
			if (!UploadInProgress)
				return;

			UploadCommandList->Close();
			ID3D12CommandList* lists[] = { UploadCommandList.Get() };
			DirectQueue->ExecuteCommandLists(1, lists);

			// Synchrone par design (voir commentaire dans le .h) : on attend que ce transfert
			// soit termine avant de continuer, pour garantir que la resource destination est
			// utilisable des le retour de cet appel. Fence dediee (UploadFence, pas Fence) --
			// voir sa declaration dans le .h : partagee entre driver immediat et
			// CD3D12DeferredContext, contrairement a Fence qui reste propre a chaque instance et
			// qu'on ne peut donc pas partager sans risquer une course sur ++FenceValue entre
			// plusieurs contextes qui dessinent en parallele.
			UINT64 value = ++UploadFenceValue;
			DirectQueue->Signal(UploadFence.Get(), value);
			if (UploadFence->GetCompletedValue() < value)
			{
				UploadFence->SetEventOnCompletion(value, UploadFenceEvent);
				::WaitForSingleObject(UploadFenceEvent, INFINITE);
			}
			UploadInProgress = false;
		}

		void CD3D12Driver::transitionTexture(CD3D12Texture* texture, D3D12_RESOURCE_STATES newState)
		{
			if (texture)
				texture->transitionTo(CommandList.Get(), newState);
		}

		// addTexture(name, IImage*) n'est plus surcharge : CNullDriver l'implemente en appelant
		// createDeviceDependentTexture() ci-dessus et en gerant le cache (grab a l'insertion, drop
		// a la sortie) -- c'est aussi ce qui fait qu'un echec de creation remonte desormais en
		// nullptr a l'appelant au lieu d'une texture sans ressource GPU.
		//
		// addTexture(size, name, format) reste surcharge pour UNE raison : la garde
		// IImage::isRenderTargetOnlyFormat() de CNullDriver. Malgre son nom, cette fonction ne
		// teste rien du materiel -- elle renvoie `true` par DEFAUT pour tout format hors d'une
		// petite liste historique (A1R5G5B5/R5G6B5/R8G8B8/A8R8G8B8 + les BC/DXT). Elle refuse donc
		// TOUS les formats flottants : ECF_R16F, ECF_A16B16G16R16F, ECF_R32F, ECF_B32G32R32F,
		// ECF_A32B32G32R32F... C'est une prudence heritee du vieil Irrlicht (ses convertisseurs
		// CImage ne savent pas manipuler ces formats), pas une limite du GPU : D3D12 cree
		// parfaitement une texture flottante normale et sait y uploader des donnees. Appliquer
		// cette garde ici supprimerait une capacite reelle du driver (cf. le test
		// TextureFormat_B32G32R32F_CreatesAndRoundTripsFloatData). Le reste du corps est celui de
		// CNullDriver::addTexture().
		ITexture* CD3D12Driver::addTexture(const core::dimension2d<u32>& size,
			const io::path& name, ECOLOR_FORMAT format)
		{
			if (0 == name.size())
				return nullptr;

			IImage* image = new CImage(format, size);
			ITexture* texture = createDeviceDependentTexture(image, name);
			image->drop();

			if (!texture)
				return nullptr;

			CNullDriver::addTexture(texture);
			texture->drop();
			return texture;
		}

		ITexture* CD3D12Driver::addRenderTargetTexture(const core::dimension2d<u32>& size,
			const io::path& name, const ECOLOR_FORMAT format)
		{
			return addRenderTargetTexture(size, name, format, 1, 0, 1);
		}

		ITexture* CD3D12Driver::addRenderTargetTexture(const core::dimension2d<u32>& size,
			const io::path& name, const ECOLOR_FORMAT format,
			u32 sampleCount, u32 sampleQuality, u32 arraySlices)
		{
			// MSAA+tableau reste hors scope (meme choix que CD3D11Texture, qui ne
			// supporte pas non plus cette combinaison) -- repli sur une seule tranche plutot que
			// d'echouer entierement (une appli qui ne demande pas vraiment arraySlices > 1 en
			// meme temps que sampleCount > 1 n'est pas impactee).
			if (arraySlices > 1 && sampleCount > 1)
			{
				os::Printer::log("CD3D12Driver::addRenderTargetTexture: arraySlices > 1 combine a"
					" sampleCount > 1 non supporte sur ce driver, une seule tranche creee", ELL_WARNING);
				arraySlices = 1;
			}

			CD3D12Texture* texture = new CD3D12Texture(ResourceOwner, size, name, format, true, sampleCount, sampleQuality, arraySlices);
			if (!texture->hasDeviceResource())
			{
				os::Printer::log("CD3D12Driver::addRenderTargetTexture: creation de la ressource D3D12 impossible", name, ELL_ERROR);
				texture->drop();
				return nullptr;
			}

			// Meme contrat que CNullDriver::addTexture(size, ...) : le cache prend sa reference
			// (grab), on rend la notre, l'appelant recoit une texture possedee par le driver.
			CNullDriver::addTexture(texture);
			texture->drop();
			return texture;
		}

		ITexture* CD3D12Driver::addUAVTexture(const core::dimension2d<u32>& size,
			const io::path& name, const ECOLOR_FORMAT format)
		{
			CD3D12Texture* texture = new CD3D12Texture(ResourceOwner, size, name, format, false, 1, 0, 1, true);
			if (!texture->hasDeviceResource() || !texture->hasUnorderedAccessView())
			{
				os::Printer::log("CD3D12Driver::addUAVTexture: creation de la ressource D3D12 impossible", name, ELL_ERROR);
				texture->drop();
				return nullptr;
			}

			CNullDriver::addTexture(texture);
			texture->drop();
			return texture;
		}

		ITexture* CD3D12Driver::addTextureArray(const core::array<IImage*>& images, E_TEXTURE_TYPE type, const io::path& name)
		{
			if (images.size() == 0)
			{
				os::Printer::log("CD3D12Driver::addTextureArray: tableau d'images vide", ELL_ERROR);
				return nullptr;
			}
			if (type == ETT_CUBE && images.size() != 6)
			{
				os::Printer::log("CD3D12Driver::addTextureArray: ETT_CUBE necessite exactement 6 images (faces)", ELL_ERROR);
				return nullptr;
			}
			if (type == ETT_CUBE_ARRAY && (images.size() % 6) != 0)
			{
				os::Printer::log("CD3D12Driver::addTextureArray: ETT_CUBE_ARRAY necessite un multiple de 6 images", ELL_ERROR);
				return nullptr;
			}

			CD3D12Texture* texture = new CD3D12Texture(images, ResourceOwner, type, name);
			if (!texture->hasDeviceResource())
			{
				os::Printer::log("CD3D12Driver::addTextureArray: creation de la ressource D3D12 impossible", name, ELL_ERROR);
				texture->drop();
				return nullptr;
			}

			CNullDriver::addTexture(texture);
			texture->drop();
			return texture;
		}

		// removeTexture()/removeAllTextures() sont ceux de CNullDriver (drop sous
		// textureArrayLock). Le drop final n'est PAS une liberation immediate de la ressource
		// GPU : ~CD3D12Texture remet ses ComPtr/slots de heap a la file de retrait fencee du
		// driver (voir retireResource()/drainRetiredResources()), sans quoi liberer une texture
		// encore referencee par une command list en vol retire le device
		// (OBJECT_DELETED_WHILE_STILL_IN_USE).

		// ============================ Phase 6 : contexte differe ============================
		// Voir CD3D12DeferredContext.h/.cpp pour l'implementation elle-meme et le commentaire de
		// tete de CD3D12Driver::createDeferredContext() ci-dessous (h:~548) pour la decision
		// d'architecture (reutilisation de l'IDeferredContext generique).

		IVideoDriver* CD3D12Driver::createDeferredContext()
		{
			CD3D12DeferredContext* deferred = new CD3D12DeferredContext(this);

			// echec de construction (device/queue/allocator/command list) : ne pas rendre un
			// objet a moitie construit dont le premier appel planterait -- meme raisonnement que
			// CD3D11Driver::createDeferredContext() (voir Cd3d11deferredcontext.cpp).
			if (!deferred->getCommandList())
			{
				deferred->drop();
				return nullptr;
			}

			return deferred;
		}

		void CD3D12Driver::executeDeferredContext(IDeferredContext* context)
		{
			if (!context)
			{
				os::Printer::log("CD3D12Driver::executeDeferredContext: contexte nul", ELL_ERROR);
				return;
			}
			context->execute(this);
		}

		// ============================ registre de materiaux ============================
		// Registre unifie IMaterialRenderer (types integres + shaders utilisateur), voir le
		// commentaire sur
		// CD3D12MaterialRenderer/SD3D12MaterialRendererEntry (CD3D12Driver.h). Limitations
		// assumees pour le contenu shader (Option A) : HLSL uniquement, pas de geometry/hull/
		// domain shader, un seul cbuffer par etage (registres fixes b3/b4, voir
		// createRootSignature()), texture/sampler t0/s0 partages entre tous les materiaux.

		// L'enregistrement lui-meme (grab, nom par defaut depuis sBuiltInMaterialTypeNames, index
		// de retour = SMaterial::MaterialType) est celui de CNullDriver ; getMaterialRenderer()/
		// getMaterialRendererCount()/getMaterialRendererName()/setMaterialRendererName() aussi.
		// Ne reste ici que le maintien de NativeRenderers au meme index (voir sa declaration).
		s32 CD3D12Driver::addMaterialRenderer(IMaterialRenderer* renderer, const c8* name)
		{
			const s32 index = CNullDriver::addMaterialRenderer(renderer, name);
			if (index < 0)
				return index;

			// Un renderer "etranger" (pas construit par ce driver) laisse un trou nul : il reste
			// consultable via getMaterialRenderer(), mais getPSOForMaterial() ne peut rien en tirer.
			NativeRenderers.resize(MaterialRenderers.size(), nullptr);
			NativeRenderers[index] = dynamic_cast<CD3D12MaterialRenderer*>(renderer);
			return index;
		}

		bool CD3D12Driver::createBuiltInMaterialRenderers()
		{
			// Ordre = valeurs E_MATERIAL_TYPE (EMaterialTypes.h) : l'index d'enregistrement
			// (addMaterialRenderer() ci-dessus) doit coincider avec le MaterialType, meme
			// convention que CD3D11Driver::createMaterialRenderers()/CNullDriver::addMaterialRenderer().
			// Chaque materiau autre que ceux explicitement geres ci-dessous retombe sur
			// PSMain/BlendMode::None ("solid"), exactement le comportement du switch qu'il
			// remplace (voir buildPSOKeyFromMaterial()/choosePixelShaderForMaterial()).
			static const E_MATERIAL_TYPE order[] =
			{
				EMT_SOLID, EMT_SOLID_2_LAYER, EMT_LIGHTMAP, EMT_LIGHTMAP_ADD, EMT_LIGHTMAP_M2,
				EMT_LIGHTMAP_M4, EMT_LIGHTMAP_LIGHTING, EMT_LIGHTMAP_LIGHTING_M2, EMT_LIGHTMAP_LIGHTING_M4,
				EMT_DETAIL_MAP, EMT_SPHERE_MAP, EMT_REFLECTION_2_LAYER, EMT_TRANSPARENT_ADD_COLOR,
				EMT_TRANSPARENT_ALPHA_CHANNEL, EMT_TRANSPARENT_ALPHA_CHANNEL_REF, EMT_TRANSPARENT_VERTEX_ALPHA,
				EMT_TRANSPARENT_REFLECTION_2_LAYER, EMT_NORMAL_MAP_SOLID, EMT_NORMAL_MAP_TRANSPARENT_ADD_COLOR,
				EMT_NORMAL_MAP_TRANSPARENT_VERTEX_ALPHA, EMT_PARALLAX_MAP_SOLID,
				EMT_PARALLAX_MAP_TRANSPARENT_ADD_COLOR, EMT_PARALLAX_MAP_TRANSPARENT_VERTEX_ALPHA,
				EMT_ONETEXTURE_BLEND,
			};

			for (size_t i = 0; i < _countof(order); ++i)
			{
				CD3D12MaterialRenderer* renderer = new CD3D12MaterialRenderer();

				// Point d'entree vertex+pixel shader + mode de blend pour ce E_MATERIAL_TYPE — VSMain
				// est le VS commun aux ~18 types a texture unique/UV partagee ; les 6 types normal
				// map/parallax map ci-dessous passent a VSMainTangents
				// (EVT_TANGENTS -- voir son commentaire dans CD3D12DefaultShaders.h). Chaque renderer
				// compile sa propre copie via CD3D12MaterialRenderer::compileBuiltIn()
				// (CD3D12MaterialRenderer.cpp) : meme repartition des responsabilites que
				// CD3D11Driver::createMaterialRenderers()/CD3D11MaterialRenderer (le renderer
				// compile, le driver decide juste quel point d'entree/mode de blend va avec quel type).
				const c8* vsEntryPoint = "VSMain";
				const c8* psEntryPoint = "PSMain";
				// non nuls uniquement pour les 12 types multi-texture ci-dessous —
				// voir CD3D12MaterialRenderer::compileBuiltIn()/VS2TCoords/PS2TCoords.
				const c8* vsEntryPoint2T = nullptr;
				const c8* psEntryPointUV2 = nullptr;
				SPSOKey::EBlendMode blendMode = SPSOKey::EBlendMode::None;
				switch (order[i])
				{
				case EMT_TRANSPARENT_VERTEX_ALPHA:
					psEntryPoint = "PSMainVertexAlpha";
					blendMode = SPSOKey::EBlendMode::AlphaBlend;
					break;
				case EMT_TRANSPARENT_ALPHA_CHANNEL_REF:
					// Alpha-test (clip() a seuil fixe dans PSMainAlphaTest), PAS de blend — meme
					// choix que CD3D9MaterialRenderer_TRANSPARENT_ALPHA_CHANNEL_REF::isTransparent()
					// qui renvoie false (ce materiau n'est pas transparent, juste troue).
					psEntryPoint = "PSMainAlphaTest";
					blendMode = SPSOKey::EBlendMode::None;
					break;
				case EMT_TRANSPARENT_ADD_COLOR:
					blendMode = SPSOKey::EBlendMode::AddColor;
					break;
				case EMT_TRANSPARENT_ALPHA_CHANNEL:
					blendMode = SPSOKey::EBlendMode::AlphaBlend;
					break;
				case EMT_ONETEXTURE_BLEND:
					// Valeur de repli seulement : les vrais facteurs sont par-instance
					// (SMaterial::MaterialTypeParam), voir buildPSOKeyFromMaterial().
					blendMode = SPSOKey::EBlendMode::Custom;
					break;
				// les 12 types multi-texture ci-dessous — meme opaque/blend que leur
				// CD3D11MaterialRenderer_* respectif (CD3D11FixedPipelineRenderer.h) : tous
				// BlendEnable=FALSE sauf EMT_TRANSPARENT_REFLECTION_2_LAYER (SrcAlpha/InvSrcAlpha,
				// meme reglage que EMT_TRANSPARENT_ALPHA_CHANNEL/VERTEX_ALPHA ci-dessus).
				case EMT_SOLID_2_LAYER:
					psEntryPoint = "PSMainSolid2Layer";
					vsEntryPoint2T = "VSMain2TCoords"; psEntryPointUV2 = "PSMainSolid2LayerUV2";
					break;
				case EMT_LIGHTMAP:
					psEntryPoint = "PSMainLightmap";
					vsEntryPoint2T = "VSMain2TCoords"; psEntryPointUV2 = "PSMainLightmapUV2";
					break;
				case EMT_LIGHTMAP_ADD:
					psEntryPoint = "PSMainLightmapAdd";
					vsEntryPoint2T = "VSMain2TCoords"; psEntryPointUV2 = "PSMainLightmapAddUV2";
					break;
				case EMT_LIGHTMAP_M2:
					psEntryPoint = "PSMainLightmapM2";
					vsEntryPoint2T = "VSMain2TCoords"; psEntryPointUV2 = "PSMainLightmapM2UV2";
					break;
				case EMT_LIGHTMAP_M4:
					psEntryPoint = "PSMainLightmapM4";
					vsEntryPoint2T = "VSMain2TCoords"; psEntryPointUV2 = "PSMainLightmapM4UV2";
					break;
				case EMT_LIGHTMAP_LIGHTING:
					psEntryPoint = "PSMainLightmapLighting";
					vsEntryPoint2T = "VSMain2TCoords"; psEntryPointUV2 = "PSMainLightmapLightingUV2";
					break;
				case EMT_LIGHTMAP_LIGHTING_M2:
					psEntryPoint = "PSMainLightmapLightingM2";
					vsEntryPoint2T = "VSMain2TCoords"; psEntryPointUV2 = "PSMainLightmapLightingM2UV2";
					break;
				case EMT_LIGHTMAP_LIGHTING_M4:
					psEntryPoint = "PSMainLightmapLightingM4";
					vsEntryPoint2T = "VSMain2TCoords"; psEntryPointUV2 = "PSMainLightmapLightingM4UV2";
					break;
				case EMT_DETAIL_MAP:
					psEntryPoint = "PSMainDetailMap";
					vsEntryPoint2T = "VSMain2TCoords"; psEntryPointUV2 = "PSMainDetailMapUV2";
					break;
				case EMT_SPHERE_MAP:
					psEntryPoint = "PSMainSphereMap";
					vsEntryPoint2T = "VSMain2TCoords"; psEntryPointUV2 = "PSMainSphereMapUV2";
					break;
				case EMT_REFLECTION_2_LAYER:
					psEntryPoint = "PSMainReflection2Layer";
					vsEntryPoint2T = "VSMain2TCoords"; psEntryPointUV2 = "PSMainReflection2LayerUV2";
					break;
				case EMT_TRANSPARENT_REFLECTION_2_LAYER:
					psEntryPoint = "PSMainTransparentReflection2Layer";
					vsEntryPoint2T = "VSMain2TCoords"; psEntryPointUV2 = "PSMainTransparentReflection2LayerUV2";
					blendMode = SPSOKey::EBlendMode::AlphaBlend;
					break;
				// Normal map/parallax map -- EVT_TANGENTS (VSMainTangents), eclairage
				// par-pixel (voir le commentaire de tete de PSMainNormalMap/PSMainParallaxMap,
				// CD3D12DefaultShaders.h). _TRANSPARENT_ADD_COLOR reutilise la formule "solide"
				// (meme motif que EMT_TRANSPARENT_ADD_COLOR/PSMain plus haut, seul le blend differe) ;
				// _TRANSPARENT_VERTEX_ALPHA a sa propre variante (alpha du vertex, pas de la
				// texture -- meme motif que PSMainVertexAlpha).
				case EMT_NORMAL_MAP_SOLID:
					vsEntryPoint = "VSMainTangents";
					psEntryPoint = "PSMainNormalMap";
					break;
				case EMT_NORMAL_MAP_TRANSPARENT_ADD_COLOR:
					vsEntryPoint = "VSMainTangents";
					psEntryPoint = "PSMainNormalMap";
					blendMode = SPSOKey::EBlendMode::AddColor;
					break;
				case EMT_NORMAL_MAP_TRANSPARENT_VERTEX_ALPHA:
					vsEntryPoint = "VSMainTangents";
					psEntryPoint = "PSMainNormalMapVertexAlpha";
					blendMode = SPSOKey::EBlendMode::AlphaBlend;
					break;
				case EMT_PARALLAX_MAP_SOLID:
					vsEntryPoint = "VSMainTangents";
					psEntryPoint = "PSMainParallaxMap";
					break;
				case EMT_PARALLAX_MAP_TRANSPARENT_ADD_COLOR:
					vsEntryPoint = "VSMainTangents";
					psEntryPoint = "PSMainParallaxMap";
					blendMode = SPSOKey::EBlendMode::AddColor;
					break;
				case EMT_PARALLAX_MAP_TRANSPARENT_VERTEX_ALPHA:
					vsEntryPoint = "VSMainTangents";
					psEntryPoint = "PSMainParallaxMapVertexAlpha";
					blendMode = SPSOKey::EBlendMode::AlphaBlend;
					break;
				default:
					break;
				}

				if (!renderer->compileBuiltIn(D3D12DefaultShaderHLSL, vsEntryPoint, psEntryPoint, vsEntryPoint2T, psEntryPointUV2))
				{
					renderer->drop();
					os::Printer::log("CD3D12Driver: compilation d'un material renderer integre a echoue "
						"— createBuiltInMaterialRenderers() a echoue", ELL_ERROR);
					return false;
				}
				renderer->BlendMode = blendMode;

				// Root signature de ce materiau. Un type integre ne reflechit aucun cbuffer et n'a
				// qu'un VS+PS, donc buildMaterialRootSignature() calcule ici exactement la meme cle de
				// layout que createRootSignature() : les 24 renderers recoivent le MEME objet, celui
				// pointe par RootSignature (voir getOrCreateRootSignature()).
				if (!buildMaterialRootSignature(renderer))
				{
					renderer->drop();
					os::Printer::log("CD3D12Driver: root signature d'un material renderer integre a echoue "
						"— createBuiltInMaterialRenderers() a echoue", ELL_ERROR);
					return false;
				}

				s32 idx = addMaterialRenderer(renderer, nullptr);
				renderer->drop(); // addMaterialRenderer() a fait un grab()

				if (idx != static_cast<s32>(i))
				{
					os::Printer::log("CD3D12Driver: index de material renderer integre desynchronise "
						"de E_MATERIAL_TYPE — createBuiltInMaterialRenderers() a echoue", ELL_ERROR);
					return false;
				}
			}

			return true;
		}

		//! lit un io::IReadFile en entier dans une core::stringc (source HLSL passee
		//! telle quelle a D3DCompile ensuite, voir registerUserShaderMaterial()). Fichier vide/nul
		//! -> chaine vide (contrat IGPUProgrammingServices : un shader vide pour un etage signifie
		//! "pas de shader pour cet etage", voir vertexShaderProgramFileName/pixelShaderProgramFileName
		//! dans IGPUProgrammingServices.h).
		static bool readShaderSource(io::IReadFile* file, core::stringc& out)
		{
			if (!file)
			{
				out = "";
				return true;
			}
			long size = file->getSize();
			if (size <= 0)
			{
				out = "";
				return true;
			}
			std::vector<char> buffer(static_cast<size_t>(size) + 1, 0);
			file->read(buffer.data(), static_cast<u32>(size));
			buffer[static_cast<size_t>(size)] = 0;
			out = buffer.data();
			return true;
		}

		static bool loadShaderSource(io::IFileSystem* fs, const io::path& filename, core::stringc& out)
		{
			if (filename.size() == 0)
			{
				out = "";
				return true;
			}
			io::IReadFile* file = fs->createAndOpenFile(filename);
			if (!file)
			{
				os::Printer::log("CD3D12Driver: impossible d'ouvrir le fichier shader ",
					filename.c_str(), ELL_ERROR);
				return false;
			}
			bool ok = readShaderSource(file, out);
			file->drop();
			return ok;
		}

		s32 CD3D12Driver::registerUserShaderMaterial(
			const c8* vertexShaderProgram, const c8* vertexShaderEntryPointName,
			E_VERTEX_SHADER_TYPE vsCompileTarget,
			const c8* pixelShaderProgram, const c8* pixelShaderEntryPointName,
			E_PIXEL_SHADER_TYPE psCompileTarget,
			const c8* geometryShaderProgram, const c8* geometryShaderEntryPointName,
			E_GEOMETRY_SHADER_TYPE gsCompileTarget,
			const c8* hullShaderProgram, const c8* hullShaderEntryPointName, E_HULL_SHADER_TYPE hsCompileTarget,
			const c8* domainShaderProgram, const c8* domainShaderEntryPointName, E_DOMAIN_SHADER_TYPE dsCompileTarget,
			IVertexDescriptor* vertexTypeOut,
			IShaderConstantSetCallBack* callback, E_MATERIAL_TYPE baseMaterial, s32 userData)
		{
			// Meme pattern que le D3D11 : un shader utilisateur
			// est un CD3D12MaterialRenderer comme les ~24 types integres (voir
			// createBuiltInMaterialRenderers()) — la compilation/reflexion est deleguee a l'objet
			// lui-meme (CD3D12MaterialRenderer::compileFromHLSL(), voir CD3D12MaterialRenderer.cpp),
			// meme repartition des responsabilites que CD3D11Driver::addHighLevelShaderMaterial()/
			// CD3D11MaterialRenderer (le renderer compile+reflechit, le driver orchestre juste la
			// lecture des fichiers/callback/enregistrement).
			CD3D12MaterialRenderer* renderer = new CD3D12MaterialRenderer();
			if (!renderer->compileFromHLSL(vertexShaderProgram, vertexShaderEntryPointName, vsCompileTarget,
				pixelShaderProgram, pixelShaderEntryPointName, psCompileTarget,
				geometryShaderProgram, geometryShaderEntryPointName, gsCompileTarget,
				hullShaderProgram, hullShaderEntryPointName, hsCompileTarget,
				domainShaderProgram, domainShaderEntryPointName, dsCompileTarget,
				FileSystem))
			{
				renderer->drop();
				return -1;
			}

			// Root signature propre a ce shader, construite a partir de la reflexion que
			// compileFromHLSL() vient de faire (VSBuffers/PSBuffers/GSBuffers/HSBuffers/DSBuffers) :
			// les 7 parametres fixes du driver plus une table CBV par couple (etage, espace de
			// registres) reellement declare — voir buildMaterialRootSignature(). Doit venir APRES la
			// compilation (rien a reflechir avant) et AVANT addMaterialRenderer() : sans elle
			// getPSOForMaterial() ne peut construire aucun PSO, donc le materiau serait enregistre
			// mais indessinable.
			if (!buildMaterialRootSignature(renderer))
			{
				renderer->drop();
				return -1;
			}

			// Milestone B : vertexTypeOut ne compte que si un GS a reellement ete fourni/compile —
			// sinon c'est un parametre orphelin (VS+PS seul, pas de stream-output possible).
			if (geometryShaderProgram && geometryShaderProgram[0] && vertexTypeOut)
			{
				vertexTypeOut->grab();
				renderer->StreamOutputVertexType = vertexTypeOut;
			}

			if (callback)
			{
				callback->grab();
				renderer->CallBack = callback;
			}
			renderer->UserData = userData;

			// Un shader utilisateur n'a pas d'etat de blend a lui : il herite de celui de son
			// E_MATERIAL_TYPE de base, comme un CD3D11MaterialRenderer delegue OnSetMaterial() a son
			// BaseRenderer. Sans ca, baseMaterial etait purement et simplement ignore et TOUT shader
			// utilisateur etait rendu opaque quel que soit son type de base — un materiau
			// EMT_TRANSPARENT_ALPHA_CHANNEL dessinait ses quads en opaque (boites noires autour des
			// glyphes RmlUI), un EMT_TRANSPARENT_ADD_COLOR masquait ce qu'il aurait du additionner
			// (lens flares).
			renderer->BaseMaterialType = baseMaterial;
			if (const CD3D12MaterialRenderer* base = getNativeRenderer(baseMaterial))
			{
				renderer->BlendMode = base->BlendMode;
				renderer->CustomSrcBlend = base->CustomSrcBlend;
				renderer->CustomDestBlend = base->CustomDestBlend;
				renderer->CustomSrcBlendAlpha = base->CustomSrcBlendAlpha;
				renderer->CustomDestBlendAlpha = base->CustomDestBlendAlpha;
				renderer->CustomBlendOp = base->CustomBlendOp;
			}

			s32 materialType = addMaterialRenderer(renderer, nullptr);
			renderer->drop(); // addMaterialRenderer() a fait un grab()
			return materialType;
		}

		s32 CD3D12Driver::addHighLevelShaderMaterial(
			const c8* vertexShaderProgram, const c8* vertexShaderEntryPointName,
			E_VERTEX_SHADER_TYPE vsCompileTarget, const c8* pixelShaderProgram,
			const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
			const c8* geometryShaderProgram, const c8* geometryShaderEntryPointName,
			E_GEOMETRY_SHADER_TYPE gsCompileTarget,
			scene::E_PRIMITIVE_TYPE inType, scene::E_PRIMITIVE_TYPE outType,
			u32 verticesOut, IShaderConstantSetCallBack* callback,
			E_MATERIAL_TYPE baseMaterial, IVertexDescriptor* vertexTypeOut,
			s32 userData, E_GPU_SHADING_LANGUAGE shadingLang)
		{
			return registerUserShaderMaterial(vertexShaderProgram, vertexShaderEntryPointName, vsCompileTarget,
				pixelShaderProgram, pixelShaderEntryPointName, psCompileTarget,
				geometryShaderProgram, geometryShaderEntryPointName, gsCompileTarget,
				nullptr, nullptr, EHST_COUNT, nullptr, nullptr, EDST_COUNT,
				vertexTypeOut, callback, baseMaterial, userData);
		}

		s32 CD3D12Driver::addHighLevelShaderMaterial(
			const c8* vertexShaderProgram, const c8* vertexShaderEntryPointName,
			E_VERTEX_SHADER_TYPE vsCompileTarget, const c8* pixelShaderProgram,
			const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
			const c8* geometryShaderProgram, const c8* geometryShaderEntryPointName,
			E_GEOMETRY_SHADER_TYPE gsCompileTarget,
			const c8* hullShaderProgram, const c8* hullShaderEntryPointName, E_HULL_SHADER_TYPE hsCompileTarget,
			const c8* domainShaderProgram, const c8* domainShaderEntryPointName, E_DOMAIN_SHADER_TYPE dsCompileTarget,
			scene::E_PRIMITIVE_TYPE inType, scene::E_PRIMITIVE_TYPE outType,
			u32 verticesOut, IShaderConstantSetCallBack* callback,
			E_MATERIAL_TYPE baseMaterial, IVertexDescriptor* vertexTypeOut,
			s32 userData, E_GPU_SHADING_LANGUAGE shadingLang)
		{
			return registerUserShaderMaterial(vertexShaderProgram, vertexShaderEntryPointName, vsCompileTarget,
				pixelShaderProgram, pixelShaderEntryPointName, psCompileTarget,
				geometryShaderProgram, geometryShaderEntryPointName, gsCompileTarget,
				hullShaderProgram, hullShaderEntryPointName, hsCompileTarget,
				domainShaderProgram, domainShaderEntryPointName, dsCompileTarget,
				vertexTypeOut, callback, baseMaterial, userData);
		}

		s32 CD3D12Driver::addHighLevelShaderMaterialFromFiles(
			const io::path& vertexShaderProgramFileName, const c8* vertexShaderEntryPointName,
			E_VERTEX_SHADER_TYPE vsCompileTarget, const io::path& pixelShaderProgramFileName,
			const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
			const io::path& geometryShaderProgramFileName, const c8* geometryShaderEntryPointName,
			E_GEOMETRY_SHADER_TYPE gsCompileTarget,
			scene::E_PRIMITIVE_TYPE inType, scene::E_PRIMITIVE_TYPE outType,
			u32 verticesOut, IShaderConstantSetCallBack* callback,
			E_MATERIAL_TYPE baseMaterial, IVertexDescriptor* vertexTypeOut,
			s32 userData, E_GPU_SHADING_LANGUAGE shadingLang)
		{
			core::stringc vsSource, psSource, gsSource;
			if (!loadShaderSource(FileSystem, vertexShaderProgramFileName, vsSource) ||
				!loadShaderSource(FileSystem, pixelShaderProgramFileName, psSource))
				return -1;
			if (geometryShaderProgramFileName.size() != 0 &&
				!loadShaderSource(FileSystem, geometryShaderProgramFileName, gsSource))
				return -1;

			return registerUserShaderMaterial(vsSource.c_str(), vertexShaderEntryPointName, vsCompileTarget,
				psSource.c_str(), pixelShaderEntryPointName, psCompileTarget,
				gsSource.size() != 0 ? gsSource.c_str() : nullptr, geometryShaderEntryPointName, gsCompileTarget,
				nullptr, nullptr, EHST_COUNT, nullptr, nullptr, EDST_COUNT,
				vertexTypeOut, callback, baseMaterial, userData);
		}

		s32 CD3D12Driver::addHighLevelShaderMaterialFromFiles(
			const io::path& vertexShaderProgramFile, const c8* vertexShaderEntryPointName,
			E_VERTEX_SHADER_TYPE vsCompileTarget, const io::path& pixelShaderProgramFile,
			const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
			const io::path& geometryShaderProgramFileName, const c8* geometryShaderEntryPointName,
			E_GEOMETRY_SHADER_TYPE gsCompileTarget,
			const io::path& hullShaderProgram, const c8* hullShaderEntryPointName, E_HULL_SHADER_TYPE hsCompileTarget,
			const io::path& domainShaderProgram, const c8* domainShaderEntryPointName, E_DOMAIN_SHADER_TYPE dsCompileTarget,
			scene::E_PRIMITIVE_TYPE inType, scene::E_PRIMITIVE_TYPE outType,
			u32 verticesOut, IShaderConstantSetCallBack* callback,
			E_MATERIAL_TYPE baseMaterial, IVertexDescriptor* vertexTypeOut,
			s32 userData, E_GPU_SHADING_LANGUAGE shadingLang)
		{
			core::stringc vsSource, psSource, gsSource;
			if (!loadShaderSource(FileSystem, vertexShaderProgramFile, vsSource) ||
				!loadShaderSource(FileSystem, pixelShaderProgramFile, psSource))
				return -1;
			if (geometryShaderProgramFileName.size() != 0 &&
				!loadShaderSource(FileSystem, geometryShaderProgramFileName, gsSource))
				return -1;
			core::stringc hsSource, dsSource;
			if (hullShaderProgram.size() != 0 && !loadShaderSource(FileSystem, hullShaderProgram, hsSource))
				return -1;
			if (domainShaderProgram.size() != 0 && !loadShaderSource(FileSystem, domainShaderProgram, dsSource))
				return -1;

			return registerUserShaderMaterial(vsSource.c_str(), vertexShaderEntryPointName, vsCompileTarget,
				psSource.c_str(), pixelShaderEntryPointName, psCompileTarget,
				gsSource.size() != 0 ? gsSource.c_str() : nullptr, geometryShaderEntryPointName, gsCompileTarget,
				hsSource.size() != 0 ? hsSource.c_str() : nullptr, hullShaderEntryPointName, hsCompileTarget,
				dsSource.size() != 0 ? dsSource.c_str() : nullptr, domainShaderEntryPointName, dsCompileTarget,
				vertexTypeOut, callback, baseMaterial, userData);
		}

		s32 CD3D12Driver::addHighLevelShaderMaterialFromFiles(
			io::IReadFile* vertexShaderProgram, const c8* vertexShaderEntryPointName,
			E_VERTEX_SHADER_TYPE vsCompileTarget, io::IReadFile* pixelShaderProgram,
			const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
			io::IReadFile* geometryShaderProgram, const c8* geometryShaderEntryPointName,
			E_GEOMETRY_SHADER_TYPE gsCompileTarget,
			scene::E_PRIMITIVE_TYPE inType, scene::E_PRIMITIVE_TYPE outType,
			u32 verticesOut, IShaderConstantSetCallBack* callback,
			E_MATERIAL_TYPE baseMaterial, IVertexDescriptor* vertexTypeOut,
			s32 userData, E_GPU_SHADING_LANGUAGE shadingLang)
		{
			core::stringc vsSource, psSource, gsSource;
			if (!readShaderSource(vertexShaderProgram, vsSource) || !readShaderSource(pixelShaderProgram, psSource))
				return -1;
			if (geometryShaderProgram && !readShaderSource(geometryShaderProgram, gsSource))
				return -1;

			return registerUserShaderMaterial(vsSource.c_str(), vertexShaderEntryPointName, vsCompileTarget,
				psSource.c_str(), pixelShaderEntryPointName, psCompileTarget,
				gsSource.size() != 0 ? gsSource.c_str() : nullptr, geometryShaderEntryPointName, gsCompileTarget,
				nullptr, nullptr, EHST_COUNT, nullptr, nullptr, EDST_COUNT,
				vertexTypeOut, callback, baseMaterial, userData);
		}

		s32 CD3D12Driver::addHighLevelShaderMaterialFromFiles(
			io::IReadFile* vertexShaderProgram, const c8* vertexShaderEntryPointName,
			E_VERTEX_SHADER_TYPE vsCompileTarget, io::IReadFile* pixelShaderProgram,
			const c8* pixelShaderEntryPointName, E_PIXEL_SHADER_TYPE psCompileTarget,
			io::IReadFile* geometryShaderProgram, const c8* geometryShaderEntryPointName,
			E_GEOMETRY_SHADER_TYPE gsCompileTarget,
			io::IReadFile* hullShaderProgram, const c8* hullShaderEntryPointName, E_HULL_SHADER_TYPE hsCompileTarget,
			io::IReadFile* domainShaderProgram, const c8* domainShaderEntryPointName, E_DOMAIN_SHADER_TYPE dsCompileTarget,
			scene::E_PRIMITIVE_TYPE inType, scene::E_PRIMITIVE_TYPE outType,
			u32 verticesOut, IShaderConstantSetCallBack* callback,
			E_MATERIAL_TYPE baseMaterial, IVertexDescriptor* vertexTypeOut,
			s32 userData, E_GPU_SHADING_LANGUAGE shadingLang)
		{
			core::stringc vsSource, psSource, gsSource;
			if (!readShaderSource(vertexShaderProgram, vsSource) || !readShaderSource(pixelShaderProgram, psSource))
				return -1;
			if (geometryShaderProgram && !readShaderSource(geometryShaderProgram, gsSource))
				return -1;
			core::stringc hsSource, dsSource;
			if (hullShaderProgram && !readShaderSource(hullShaderProgram, hsSource))
				return -1;
			if (domainShaderProgram && !readShaderSource(domainShaderProgram, dsSource))
				return -1;

			return registerUserShaderMaterial(vsSource.c_str(), vertexShaderEntryPointName, vsCompileTarget,
				psSource.c_str(), pixelShaderEntryPointName, psCompileTarget,
				gsSource.size() != 0 ? gsSource.c_str() : nullptr, geometryShaderEntryPointName, gsCompileTarget,
				hsSource.size() != 0 ? hsSource.c_str() : nullptr, hullShaderEntryPointName, hsCompileTarget,
				dsSource.size() != 0 ? dsSource.c_str() : nullptr, domainShaderEntryPointName, dsCompileTarget,
				vertexTypeOut, callback, baseMaterial, userData);
		}

		s32 CD3D12Driver::addComputeShader(const c8* computeShaderProgram, const c8* computeShaderEntryPointName,
			E_COMPUTE_SHADER_TYPE csCompileTarget, IShaderConstantSetCallBack* callback, s32 userData)
		{
			// Meme repartition des responsabilites que registerUserShaderMaterial() : le renderer
			// compile+reflechit (CD3D12MaterialRenderer::compileComputeFromHLSL()), le driver
			// orchestre juste callback/enregistrement.
			CD3D12MaterialRenderer* renderer = new CD3D12MaterialRenderer();
			if (!renderer->compileComputeFromHLSL(computeShaderProgram, computeShaderEntryPointName, csCompileTarget, FileSystem))
			{
				renderer->drop();
				return -1;
			}

			if (callback)
			{
				callback->grab();
				renderer->CallBack = callback;
			}
			renderer->UserData = userData;

			s32 materialType = addMaterialRenderer(renderer, nullptr);
			renderer->drop(); // addMaterialRenderer() a fait un grab()
			return materialType;
		}

		s32 CD3D12Driver::addComputeShaderFromFile(const io::path& computeShaderProgramFileName,
			const c8* computeShaderEntryPointName, E_COMPUTE_SHADER_TYPE csCompileTarget,
			IShaderConstantSetCallBack* callback, s32 userData)
		{
			core::stringc csSource;
			if (!loadShaderSource(FileSystem, computeShaderProgramFileName, csSource))
				return -1;

			return addComputeShader(csSource.c_str(), computeShaderEntryPointName, csCompileTarget, callback, userData);
		}

		// --- IMaterialRendererServices : voir bindDrawState() pour quand
		// ActiveMaterialRendererIndex est mis a jour. var.Buffer indexe VSBuffers/PSBuffers (plusieurs
		// cbuffers par etage possibles depuis le passage aux tables space1, voir
		// CD3D12MaterialRenderer::reflectCBuffer()). Un materiau integre a un Native valide mais des
		// VSVariables/PSVariables vides (voir createBuiltInMaterialRenderers()), donc ces methodes
		// echouent proprement (-1/false) sans avoir besoin de les distinguer explicitement d'un
		// shader utilisateur.

		namespace
		{
			//! Un nom de constante introuvable dans la reflexion fait echouer l'ecriture EN SILENCE :
			//! la variable garde 0 cote shader et le rendu est faux sans le moindre message (cas reel :
			//! "STemp" dans Star.hlsl, coordonnee de lecture du degrade de temperature -- a 0 l'etoile
			//! echantillonne le mauvais bout de la rampe et ressort de la mauvaise couleur). On le
			//! signale donc, une fois par nom pour ne pas inonder le log a chaque draw.
			void warnUnknownShaderConstantOnce(const char* stage, const c8* name)
			{
				static std::mutex mutex;
				static std::set<core::stringc> alreadyWarned;

				std::lock_guard<std::mutex> lock(mutex);
				if (!alreadyWarned.insert(core::stringc(name)).second)
					return;

				core::stringc msg = "CD3D12Driver: constante de shader introuvable dans la reflexion (";
				msg += stage;
				msg += ") : \"";
				msg += name;
				msg += "\" -- ecriture ignoree, la variable restera a 0 cote shader";
				os::Printer::log(msg.c_str(), ELL_WARNING);
			}
		}

		s32 CD3D12Driver::getVertexShaderConstantID(const c8* name)
		{
			if (ActiveMaterialRendererIndex < 0 || !name)
				return -1;
			const CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return -1;
			const auto& vars = renderer->VSVariables;
			for (size_t i = 0; i < vars.size(); ++i)
				if (vars[i].Name == name)
					return static_cast<s32>(i);
			warnUnknownShaderConstantOnce("VS", name);
			return -1;
		}

		s32 CD3D12Driver::getPixelShaderConstantID(const c8* name)
		{
			if (ActiveMaterialRendererIndex < 0 || !name)
				return -1;
			const CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return -1;
			const auto& vars = renderer->PSVariables;
			for (size_t i = 0; i < vars.size(); ++i)
				if (vars[i].Name == name)
					return static_cast<s32>(i);
			warnUnknownShaderConstantOnce("PS", name);
			return -1;
		}

		s32 CD3D12Driver::getGeometryShaderConstantID(const c8* name)
		{
			if (ActiveMaterialRendererIndex < 0 || !name)
				return -1;
			const CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return -1;
			const auto& vars = renderer->GSVariables;
			for (size_t i = 0; i < vars.size(); ++i)
				if (vars[i].Name == name)
					return static_cast<s32>(i);
			return -1;
		}

		namespace
		{
			//! Ecrit `count` floats dans le miroir CPU (Scratch) du cbuffer auquel appartient la
			//! variable `index`, en TRANSPOSANT si c'est un float4x4 column-major (voir
			//! SD3D12UserShaderVariable::TransposeOnSet) -- meme regle que
			//! CD3D11MaterialRenderer::setVariable(), dont dependent tous les shaders du moteur.
			//! Factorise les six setters (VS/PS/GS/HS/DS/CS), qui n'avaient que ce corps en commun.
			//! Width-aware sibling of writeUserShaderConstant: the typed one assumes 4-byte elements,
			//! so a 64-bit type set through it would copy only half its bytes.
			bool writeUserShaderConstantRaw(std::vector<SD3D12UserShaderCBuffer>& buffers,
				const std::vector<SD3D12UserShaderVariable>& vars, s32 index, const void* data, u32 byteCount)
			{
				if (!data || byteCount == 0)
					return false;
				if (index < 0 || static_cast<size_t>(index) >= vars.size())
					return false;
				const SD3D12UserShaderVariable& var = vars[index];
				if (var.Buffer < 0 || static_cast<size_t>(var.Buffer) >= buffers.size())
					return false;
				std::vector<u8>& scratch = buffers[var.Buffer].Scratch;
				if (var.Offset >= scratch.size())
					return false;

				size_t bytes = byteCount;
				const size_t avail = scratch.size() - var.Offset;
				if (bytes > avail)
					bytes = avail;

				memcpy(scratch.data() + var.Offset, data, bytes);
				return true;
			}

			bool writeUserShaderConstant(std::vector<SD3D12UserShaderCBuffer>& buffers,
				const std::vector<SD3D12UserShaderVariable>& vars, s32 index, const f32* floats, int count)
			{
				if (!floats || count <= 0)
					return false;
				if (index < 0 || static_cast<size_t>(index) >= vars.size())
					return false;
				const SD3D12UserShaderVariable& var = vars[index];
				if (var.Buffer < 0 || static_cast<size_t>(var.Buffer) >= buffers.size())
					return false;
				std::vector<u8>& scratch = buffers[var.Buffer].Scratch;
				if (var.Offset >= scratch.size())
					return false;

				size_t bytes = static_cast<size_t>(count) * sizeof(f32);
				const size_t avail = scratch.size() - var.Offset;
				if (bytes > avail)
					bytes = avail;

				if (var.TransposeOnSet && count >= 16 && bytes >= sizeof(f32) * 16)
				{
					// Transposition dans une copie locale, PAS sur place : CD3D11MaterialRenderer
					// reecrit le tableau de l'appelant ("*m = m->getTransposed()"), ce qui corrompt
					// silencieusement la matrice de l'appelant s'il la reutilise apres coup.
					core::matrix4 m;
					memcpy(m.pointer(), floats, sizeof(f32) * 16);
					const core::matrix4 transposed = m.getTransposed();
					memcpy(scratch.data() + var.Offset, transposed.pointer(), sizeof(f32) * 16);
					return true;
				}

				memcpy(scratch.data() + var.Offset, floats, bytes);
				return true;
			}
		}

		bool CD3D12Driver::setGeometryShaderConstant(s32 index, const f32* floats, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstant(renderer->GSBuffers, renderer->GSVariables, index, floats, count);
		}

		s32 CD3D12Driver::getHullShaderConstantID(const c8* name)
		{
			if (ActiveMaterialRendererIndex < 0 || !name)
				return -1;
			const CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return -1;
			const auto& vars = renderer->HSVariables;
			for (size_t i = 0; i < vars.size(); ++i)
				if (vars[i].Name == name)
					return static_cast<s32>(i);
			return -1;
		}

		bool CD3D12Driver::setHullShaderConstant(s32 index, const f32* floats, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstant(renderer->HSBuffers, renderer->HSVariables, index, floats, count);
		}

		s32 CD3D12Driver::getDomainShaderConstantID(const c8* name)
		{
			if (ActiveMaterialRendererIndex < 0 || !name)
				return -1;
			const CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return -1;
			const auto& vars = renderer->DSVariables;
			for (size_t i = 0; i < vars.size(); ++i)
				if (vars[i].Name == name)
					return static_cast<s32>(i);
			return -1;
		}

		bool CD3D12Driver::setDomainShaderConstant(s32 index, const f32* floats, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstant(renderer->DSBuffers, renderer->DSVariables, index, floats, count);
		}

		s32 CD3D12Driver::getComputeShaderConstantID(const c8* name)
		{
			if (ActiveMaterialRendererIndex < 0 || !name)
				return -1;
			const CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return -1;
			const auto& vars = renderer->CSVariables;
			for (size_t i = 0; i < vars.size(); ++i)
				if (vars[i].Name == name)
					return static_cast<s32>(i);
			return -1;
		}

		bool CD3D12Driver::setComputeShaderConstant(s32 index, const f32* floats, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstant(renderer->CSBuffers, renderer->CSVariables, index, floats, count);
		}

		bool CD3D12Driver::setVertexShaderConstant(s32 index, const u32* uints, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstantRaw(renderer->VSBuffers, renderer->VSVariables, index, uints, (u32)(count * sizeof(u32)));
		}

		bool CD3D12Driver::setVertexShaderConstant(s32 index, const f64* doubles, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstantRaw(renderer->VSBuffers, renderer->VSVariables, index, doubles, (u32)(count * sizeof(f64)));
		}

		bool CD3D12Driver::setVertexShaderConstant(s32 index, const s64* longs, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstantRaw(renderer->VSBuffers, renderer->VSVariables, index, longs, (u32)(count * sizeof(s64)));
		}

		bool CD3D12Driver::setVertexShaderConstant(s32 index, const u64* ulongs, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstantRaw(renderer->VSBuffers, renderer->VSVariables, index, ulongs, (u32)(count * sizeof(u64)));
		}

		bool CD3D12Driver::setPixelShaderConstant(s32 index, const u32* uints, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstantRaw(renderer->PSBuffers, renderer->PSVariables, index, uints, (u32)(count * sizeof(u32)));
		}

		bool CD3D12Driver::setPixelShaderConstant(s32 index, const f64* doubles, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstantRaw(renderer->PSBuffers, renderer->PSVariables, index, doubles, (u32)(count * sizeof(f64)));
		}

		bool CD3D12Driver::setPixelShaderConstant(s32 index, const s64* longs, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstantRaw(renderer->PSBuffers, renderer->PSVariables, index, longs, (u32)(count * sizeof(s64)));
		}

		bool CD3D12Driver::setPixelShaderConstant(s32 index, const u64* ulongs, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstantRaw(renderer->PSBuffers, renderer->PSVariables, index, ulongs, (u32)(count * sizeof(u64)));
		}

		bool CD3D12Driver::setGeometryShaderConstant(s32 index, const u32* uints, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstantRaw(renderer->GSBuffers, renderer->GSVariables, index, uints, (u32)(count * sizeof(u32)));
		}

		bool CD3D12Driver::setGeometryShaderConstant(s32 index, const f64* doubles, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstantRaw(renderer->GSBuffers, renderer->GSVariables, index, doubles, (u32)(count * sizeof(f64)));
		}

		bool CD3D12Driver::setGeometryShaderConstant(s32 index, const s64* longs, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstantRaw(renderer->GSBuffers, renderer->GSVariables, index, longs, (u32)(count * sizeof(s64)));
		}

		bool CD3D12Driver::setGeometryShaderConstant(s32 index, const u64* ulongs, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstantRaw(renderer->GSBuffers, renderer->GSVariables, index, ulongs, (u32)(count * sizeof(u64)));
		}

		bool CD3D12Driver::setHullShaderConstant(s32 index, const u32* uints, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstantRaw(renderer->HSBuffers, renderer->HSVariables, index, uints, (u32)(count * sizeof(u32)));
		}

		bool CD3D12Driver::setHullShaderConstant(s32 index, const f64* doubles, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstantRaw(renderer->HSBuffers, renderer->HSVariables, index, doubles, (u32)(count * sizeof(f64)));
		}

		bool CD3D12Driver::setHullShaderConstant(s32 index, const s64* longs, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstantRaw(renderer->HSBuffers, renderer->HSVariables, index, longs, (u32)(count * sizeof(s64)));
		}

		bool CD3D12Driver::setHullShaderConstant(s32 index, const u64* ulongs, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstantRaw(renderer->HSBuffers, renderer->HSVariables, index, ulongs, (u32)(count * sizeof(u64)));
		}

		bool CD3D12Driver::setDomainShaderConstant(s32 index, const u32* uints, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstantRaw(renderer->DSBuffers, renderer->DSVariables, index, uints, (u32)(count * sizeof(u32)));
		}

		bool CD3D12Driver::setDomainShaderConstant(s32 index, const f64* doubles, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstantRaw(renderer->DSBuffers, renderer->DSVariables, index, doubles, (u32)(count * sizeof(f64)));
		}

		bool CD3D12Driver::setDomainShaderConstant(s32 index, const s64* longs, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstantRaw(renderer->DSBuffers, renderer->DSVariables, index, longs, (u32)(count * sizeof(s64)));
		}

		bool CD3D12Driver::setDomainShaderConstant(s32 index, const u64* ulongs, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstantRaw(renderer->DSBuffers, renderer->DSVariables, index, ulongs, (u32)(count * sizeof(u64)));
		}

		bool CD3D12Driver::setComputeShaderConstant(s32 index, const u32* uints, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstantRaw(renderer->CSBuffers, renderer->CSVariables, index, uints, (u32)(count * sizeof(u32)));
		}

		bool CD3D12Driver::setComputeShaderConstant(s32 index, const f64* doubles, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstantRaw(renderer->CSBuffers, renderer->CSVariables, index, doubles, (u32)(count * sizeof(f64)));
		}

		bool CD3D12Driver::setComputeShaderConstant(s32 index, const s64* longs, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstantRaw(renderer->CSBuffers, renderer->CSVariables, index, longs, (u32)(count * sizeof(s64)));
		}

		bool CD3D12Driver::setComputeShaderConstant(s32 index, const u64* ulongs, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstantRaw(renderer->CSBuffers, renderer->CSVariables, index, ulongs, (u32)(count * sizeof(u64)));
		}

		bool CD3D12Driver::setComputeShaderConstant(s32 index, const s32* ints, int count)
		{
			return setComputeShaderConstant(index, reinterpret_cast<const f32*>(ints), count);
		}

		bool CD3D12Driver::setGeometryShaderConstant(s32 index, const s32* ints, int count)
		{
			return setGeometryShaderConstant(index, reinterpret_cast<const f32*>(ints), count);
		}

		bool CD3D12Driver::setHullShaderConstant(s32 index, const s32* ints, int count)
		{
			return setHullShaderConstant(index, reinterpret_cast<const f32*>(ints), count);
		}

		bool CD3D12Driver::setDomainShaderConstant(s32 index, const s32* ints, int count)
		{
			return setDomainShaderConstant(index, reinterpret_cast<const f32*>(ints), count);
		}

		// Note : les implementations ci-dessous tronquent silencieusement (au lieu de deborder)
		// si le shader declare une variable plus petite que ce que l'appelant fournit — meme
		// esprit defensif que le reste de ce driver (ex. allocateConstant() qui log plutot que
		// planter quand l'anneau est plein).

		bool CD3D12Driver::setVertexShaderConstant(s32 index, const f32* floats, int count)
		{
			if (ActiveMaterialRendererIndex < 0)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstant(renderer->VSBuffers, renderer->VSVariables, index, floats, count);
		}

		bool CD3D12Driver::setVertexShaderConstant(s32 index, const s32* ints, int count)
		{
			return setVertexShaderConstant(index, reinterpret_cast<const f32*>(ints), count);
		}

		bool CD3D12Driver::setPixelShaderConstant(s32 index, const f32* floats, int count)
		{
			if (ActiveMaterialRendererIndex < 0 || !floats)
				return false;
			CD3D12MaterialRenderer* renderer = getNativeRenderer(ActiveMaterialRendererIndex);
			if (!renderer)
				return false;
			return writeUserShaderConstant(renderer->PSBuffers, renderer->PSVariables, index, floats, count);
		}

		bool CD3D12Driver::setPixelShaderConstant(s32 index, const s32* ints, int count)
		{
			return setPixelShaderConstant(index, reinterpret_cast<const f32*>(ints), count);
		}

		void CD3D12Driver::setVertexShaderConstant(const f32* data, s32 startRegister, s32 constantAmount)
		{
			os::Printer::log("CD3D12Driver::setVertexShaderConstant(register): shaders assembleur non "
				"supportes, utiliser la variante par nom (HLSL)", ELL_ERROR);
		}

		void CD3D12Driver::setPixelShaderConstant(const f32* data, s32 startRegister, s32 constantAmount)
		{
			os::Printer::log("CD3D12Driver::setPixelShaderConstant(register): shaders assembleur non "
				"supportes, utiliser la variante par nom (HLSL)", ELL_ERROR);
		}

	} // end namespace video
} // end namespace irr

namespace irr
{
	namespace video
	{
#ifdef _IRR_COMPILE_WITH_DIRECT3D_12_
		//! Cree le driver D3D12 natif (distinct de createDirectX11On12Driver() = interop 11-on-12).
		IVideoDriver* createDirectX12Driver(const irr::SIrrlichtCreationParameters& params,
			io::IFileSystem* io, HWND window)
		{
			CD3D12Driver* driver = new CD3D12Driver(params, io, window);
			if (!driver->initDriver(window))
			{
				driver->drop();
				driver = nullptr;
			}
			return driver;
		}
#endif
	}
}

#endif // _IRR_COMPILE_WITH_DIRECT3D_12_
