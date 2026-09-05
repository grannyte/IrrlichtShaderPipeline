// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h
// See CD3D12DeferredContext.h for the architecture and known limitations.

#include "CD3D12DeferredContext.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_12_

#include "os.h"

namespace irr
{
	namespace video
	{
		// The CD3D12Driver(params, io, window, sharedResources) base constructor only stores
		// Params/FileSystem/WindowSize and links TextureCache/RTVHeap/DSVHeap/CBVSRVUAVHeap/
		// Upload*/UploadFence*/ResourceOwner to those of `immediate` -- no device, no swapchain.
		// `window` is passed as 0 -- never stored anywhere except in swapchain/upload creation,
		// which this context never triggers.
		//
		// A deferred context is a command list recorder, not a second owner of GPU resources:
		// passing `immediate` here means TextureCache/descriptor heaps/upload pipeline are
		// literally the SAME as immediate's, not independent copies -- a texture loaded from
		// either context reuses the same GPU resource/descriptor slot, and CD3D12Texture/
		// CD3D12HardwareBuffer constructed here receive ResourceOwner (= immediate) rather than
		// `this`, so they stay valid even if this context is destroyed before them.

		CD3D12DeferredContext::CD3D12DeferredContext(CD3D12Driver* immediate)
			: CD3D12Driver(immediate->Params, immediate->FileSystem, (HWND)0, immediate)
			, ImmediateDriver(immediate)
		{
			if (!immediate || !immediate->Device)
			{
				os::Printer::log("CD3D12DeferredContext: invalid immediate driver", ELL_ERROR);
				return;
			}

			// Shares device/queue/root signature/default shaders/fallback texture with the
			// immediate driver -- plain ComPtr copies (AddRef) or a raw pointer copy for
			// NullTexture (lifetime guaranteed by `immediate`'s contract, see the .h).
			// ExecuteCommandLists()/Signal() on a shared ID3D12CommandQueue are thread-safe by
			// D3D12 contract, so sharing DirectQueue with the immediate driver is correct.
			//
			// PSOCache is deliberately kept as its OWN member (empty initially, inherited from
			// CD3D12Driver): sharing the same unordered_map between two recording threads without
			// a mutex would be a genuine race (CD3D12PSOCache::getOrCreate() isn't thread-safe) --
			// the accepted cost is that a handful of PSOs may be duplicated rather than reused
			// between this context and the immediate driver.
			//
			// RootSignatureCache is likewise NOT copied, and needs no equivalent of the PSO caveat:
			// nothing on a deferred context ever populates it. Root signatures are built once at
			// registration time on the immediate driver (createBuiltInMaterialRenderers()/
			// registerUserShaderMaterial() -> buildMaterialRootSignature()) and reached from here
			// through the delegated material registry, as CD3D12MaterialRenderer::RootSignature. Only
			// the DEFAULT one is copied below, as the fallback rootSignatureForRenderer() needs.
			Device = immediate->Device;
			DirectQueue = immediate->DirectQueue;
			RootSignature = immediate->RootSignature;
			NullTexture = immediate->NullTexture;

			// The material renderer registry (built-in types + user shaders, see
			// CD3D12Driver::MaterialRenderers) is delegated rather than copied: neither the
			// texture cache nor the material registry are copied here -- both stay EMPTY on this
			// context (getTexture()/getMaterialRenderer()/... in the .h forward to the immediate
			// driver instead, and getNativeRenderer() always reads through ResourceOwner-
			// >NativeRenderers). Delegating always sees the up-to-date registry, including a user
			// shader material registered after this context was constructed.
			//
			// NullTexture stays a raw pointer shared with, and owned by, the immediate driver's
			// cache (CD3D12Driver::createNullTexture()): ~CNullDriver() on THIS context therefore
			// drops nothing and cannot destroy a texture still used elsewhere.

			// One allocator per ring slot: beginRecording() rotates through them so it never resets
			// one the GPU may still be executing, which is the only way to avoid blocking on the
			// previous submission (the caller cannot wait -- see beginRecording).
			HRESULT hr = S_OK;
			for (UINT i = 0; i < NativeFrameCount; ++i)
			{
				hr = Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
					IID_PPV_ARGS(&Frames[i].CommandAllocator));
				if (FAILED(hr))
				{
					os::Printer::log("CD3D12DeferredContext: CreateCommandAllocator failed", ELL_ERROR);
					return;
				}
			}

			// Builds the constant ring, shader-visible SRV heap and vertex ring for all of
			// Frames' slots (not just Frames[0]) using the same helper as CD3D12Driver::initDriver()
			// -- this context only ever uses one slot (no swapchain frame cycle here), but
			// duplicating createFrameDrawResources() for a single slot would only risk drifting
			// from the tested version. Also creates the occlusion query resources via the same
			// call, making drawStencilShadowVolume()/occlusion queries usable from a deferred
			// context with no extra work.
			if (!createFrameDrawResources())
			{
				os::Printer::log("CD3D12DeferredContext: createFrameDrawResources failed", ELL_ERROR);
				return;
			}

			hr = Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
				Frames[0].CommandAllocator.Get(), nullptr, IID_PPV_ARGS(&CommandList));
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12DeferredContext: CreateCommandList failed", ELL_ERROR);
				CommandList = nullptr;
				return;
			}
			// An ID3D12GraphicsCommandList is created already open (ready to record) --
			// unlike ID3D11DeviceContext::CreateDeferredContext(), no initial reset is required
			// before the first recording.

			// Upload allocator/command list and the RTV/DSV/CBV-SRV-UAV descriptor heaps no
			// longer need to be created here: they are now the SAME objects as `immediate`'s (see
			// UploadAllocator/UploadCommandList/RTVHeap/DSVHeap/CBVSRVUAVHeap in CD3D12Driver.h
			// and the base constructor above) -- recreating them would overwrite the immediate
			// driver's already-initialized heap and orphan its already-allocated descriptors.

			// Offscreen render target texture OWNED by this context -- see the .h file header
			// comment for why this context does not draw into the immediate driver's back buffer.
			// Sized to the immediate driver's current render target size at construction time
			// (just a reasonable starting value, not a live link to `immediate`).
			const core::dimension2d<u32> targetSize = immediate->CurrentRenderTargetSize;
			Target = addRenderTargetTexture(targetSize, "CD3D12DeferredContext_Target", ECF_A8R8G8B8);
			if (!Target)
				os::Printer::log("CD3D12DeferredContext: addRenderTargetTexture (own target)"
					" failed -- this context will not be able to draw correctly", ELL_ERROR);

			if (!createDepthStencilBuffer(targetSize.Width, targetSize.Height))
				os::Printer::log("CD3D12DeferredContext: createDepthStencilBuffer failed"
					" (no depth/stencil on this context's target)", ELL_WARNING);

			// Completion fence owned by this context -- NOT the immediate driver's, which tracks
			// its own swapchain frames on a potentially different thread (concurrent draws are
			// allowed, see CD3D12Driver.h). Distinct from UploadFence (shared with `immediate`,
			// see the base constructor above): endUploadAndWait() never touches this Fence.
			hr = Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence));
			if (FAILED(hr))
				os::Printer::log("CD3D12DeferredContext: CreateFence failed"
					" (waitForCompletion() will not be able to wait on the GPU)", ELL_ERROR);
			FenceEvent = ::CreateEvent(nullptr, FALSE, FALSE, nullptr);

			prepareRecordingState();
		}

		CD3D12DeferredContext::~CD3D12DeferredContext()
		{
			// No explicit cleanup here: ImmediateDriver is not owned (no drop()), and waiting for
			// the GPU to finish the last submission before freeing resources (allocator/rings/
			// heaps) is already handled by ~CD3D12Driver() -- its `if (DirectQueue && Fence)`
			// guard is true for this context (shared DirectQueue non-null, our own Fence), so the
			// base destructor waits correctly.
		}

		bool CD3D12DeferredContext::beginScene(bool, bool, SColor, const SExposedVideoData&, core::rect<s32>*)
		{
			return true;
		}

		bool CD3D12DeferredContext::endScene()
		{
			return true;
		}

		void CD3D12DeferredContext::OnResize(const core::dimension2d<u32>&)
		{
			os::Printer::log("CD3D12DeferredContext::OnResize: no swapchain on a deferred"
				" context -- ignored", ELL_WARNING);
		}

		void CD3D12DeferredContext::prepareRecordingState()
		{
			// Per-slot, not slot 0: the rings and heap bound here must belong to the same frame as
			// the allocator beginRecording() just reset, or a still-in-flight submission's data is
			// overwritten under it.
			SD3D12FrameContext& frame = Frames[CurrentFrameIndex];
			if (!CommandList || !frame.ShaderVisibleSRVHeap)
				return;

			frame.ConstantRingOffset = 0;
			frame.ShaderVisibleSRVNext = 0;
			frame.VertexRingOffset = 0;
			ID3D12DescriptorHeap* heaps[] = { frame.ShaderVisibleSRVHeap.Get() };
			CommandList->SetDescriptorHeaps(1, heaps);

			if (!Target)
				return;

			// The command list is open: bindDrawState() refuses every draw while SceneOpen is false
			// (its "draw outside beginScene()" guard), and this context's beginScene() is a no-op,
			// so the recording itself is the open scene here.
			SceneOpen = true;

			// Clears on every (re)start of recording -- makes an execute() reproducible even
			// with no draw calls (useful for tests), and resets the target to a clean state
			// after reuse via beginRecording().
			bindOwnTarget(true, true, SColor(255, 0, 0, 0));
		}

		void CD3D12DeferredContext::bindOwnTarget(bool clearColor, bool clearDepth, SColor color)
		{
			// Draws into its OWN offscreen render target texture -- see the .h file header
			// comment for why this context doesn't target the immediate driver's back buffer.
			CD3D12Texture* target = static_cast<CD3D12Texture*>(Target);
			// Created in RENDER_TARGET, but the immediate driver drawing the previous recording's
			// result as a texture left it in PIXEL_SHADER_RESOURCE; a no-op the first time round.
			target->transitionTo(CommandList.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);

			// The same bookkeeping CD3D12Driver::setRenderTarget() keeps: buildPSOKeyFromMaterial()
			// reads the formats and sample count, the 2D projection reads the size. Without it a
			// recording that bound a user texture leaves those describing that texture, and the next
			// recording into this target draws with the wrong size (nothing visible) or a rejected PSO.
			const core::dimension2d<u32>& size = Target->getSize();
			CurrentRenderTarget = Target;
			CurrentRenderTargetSize = size;
			CurrentRTVCount = 1;
			CurrentRTVFormats[0] = target->getDxgiFormat();
			CurrentRTVSampleCount = 1;
			MrtBlend.reset();

			D3D12_CPU_DESCRIPTOR_HANDLE* dsvPtr = nullptr;
			if (HasDepthStencilBuffer)
			{
				CurrentDSVHandle = DSVHandle;
				CurrentDSVFormat = DepthStencilFormat;
				dsvPtr = &CurrentDSVHandle;
			}
			else
				CurrentDSVHandle = {};
			CurrentSceneHasDepthStencil = (dsvPtr != nullptr);

			D3D12_CPU_DESCRIPTOR_HANDLE rtv = target->getRenderTargetView();
			if (clearColor)
			{
				FLOAT clear[4] = { color.getRed() / 255.0f, color.getGreen() / 255.0f,
					color.getBlue() / 255.0f, color.getAlpha() / 255.0f };
				CommandList->ClearRenderTargetView(rtv, clear, 0, nullptr);
			}
			if (dsvPtr && clearDepth)
				CommandList->ClearDepthStencilView(*dsvPtr,
					D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
			CommandList->OMSetRenderTargets(1, &rtv, FALSE, dsvPtr);
			CurrentRTVHandles[0] = rtv;

			D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(size.Width),
				static_cast<float>(size.Height), 0.0f, 1.0f };
			CommandList->RSSetViewports(1, &viewport);
			ViewPort = core::rect<s32>(0, 0, static_cast<s32>(size.Width), static_cast<s32>(size.Height));
			setScissorFromClip(nullptr);
		}

		bool CD3D12DeferredContext::setRenderTarget(video::ITexture* texture, bool clearBackBuffer,
			bool clearZBuffer, SColor color, video::ITexture* depthStencil)
		{
			// "The frame buffer" of this context is its own target; the immediate driver's back
			// buffer RTV lives in its frame ring, which this context does not have.
			if (texture && texture != Target)
				return CD3D12Driver::setRenderTarget(texture, clearBackBuffer, clearZBuffer, color, depthStencil);
			if (!SceneOpen || !Target)
			{
				os::Printer::log("CD3D12DeferredContext::setRenderTarget: no recording is open", ELL_WARNING);
				return false;
			}
			bindOwnTarget(clearBackBuffer, clearZBuffer, color);
			return true;
		}

		void CD3D12DeferredContext::beginRecording()
		{
			if (!CommandList)
				return;

			// IDeferredContext::beginRecording() contract: "only safe to call once execute() has
			// fully drained the previous batch". Unlike D3D11 (where FinishCommandList() makes
			// resetting the deferred context safe immediately), an ID3D12CommandAllocator can
			// only be Reset() once the GPU has finished executing ALL command lists that used it
			// -- otherwise corruption/debug layer errors. We warn rather than implicitly waiting
			// here (waiting silently would turn a deferred context into a plain blocking context,
			// defeating the point of recording in parallel): it's up to the caller to have called
			// waitForCompletion() (or otherwise know the previous submission is done) before
			// calling beginRecording() again.
			// Rotate through the frame ring instead of resetting one allocator every frame: an
			// allocator may only be Reset() once the GPU has finished every list that used it, and
			// the caller cannot wait here (blocking on the UI submission deadlocks against
			// Present). With NativeFrameCount slots the wait below is effectively never taken,
			// which is what makes recording in parallel safe rather than merely unblocked.
			CurrentFrameIndex = (CurrentFrameIndex + 1) % NativeFrameCount;
			SD3D12FrameContext& frame = Frames[CurrentFrameIndex];

			if (Fence && frame.FenceValue != 0 && Fence->GetCompletedValue() < frame.FenceValue)
			{
				if (FenceEvent)
				{
					Fence->SetEventOnCompletion(frame.FenceValue, FenceEvent);
					WaitForSingleObject(FenceEvent, INFINITE);
				}
				else
				{
					while (Fence->GetCompletedValue() < frame.FenceValue)
						/* spin: no event to wait on */;
				}
			}

			frame.CommandAllocator->Reset();
			CommandList->Reset(frame.CommandAllocator.Get(), nullptr);
			prepareRecordingState();
		}

		void CD3D12DeferredContext::execute(IVideoDriver* driver)
		{
			if (!CommandList)
			{
				os::Printer::log("CD3D12DeferredContext::execute: no command list"
					" (construction failed?)", ELL_ERROR);
				return;
			}

			CD3D12Driver* target = driver ? static_cast<CD3D12Driver*>(driver) : ImmediateDriver;
			if (!target || !target->DirectQueue)
			{
				os::Printer::log("CD3D12DeferredContext::execute: target driver has no command queue", ELL_ERROR);
				return;
			}

			// Closed either way: nothing may be recorded until beginRecording() reopens it.
			SceneOpen = false;
			HRESULT hr = CommandList->Close();
			if (FAILED(hr))
			{
				os::Printer::log("CD3D12DeferredContext::execute: Close failed", ELL_ERROR);
				return;
			}

			ID3D12CommandList* lists[] = { CommandList.Get() };
			target->DirectQueue->ExecuteCommandLists(1, lists);

			if (Fence)
			{
				++FenceValue;
				target->DirectQueue->Signal(Fence.Get(), FenceValue);
				// Stamp the slot this submission used, so beginRecording() knows when it may be
				// reset. Without this the ring rotates but never actually waits for anything.
				Frames[CurrentFrameIndex].FenceValue = FenceValue;
			}
		}

		void CD3D12DeferredContext::waitForCompletion()
		{
			if (!Fence || !FenceEvent)
				return;

			if (Fence->GetCompletedValue() < FenceValue)
			{
				Fence->SetEventOnCompletion(FenceValue, FenceEvent);
				::WaitForSingleObject(FenceEvent, INFINITE);
			}
		}

		IVideoDriver* CD3D12DeferredContext::createDeferredContext()
		{
			os::Printer::log("CD3D12DeferredContext::createDeferredContext: nested deferred"
				" contexts are not supported", ELL_ERROR);
			return nullptr;
		}
	}
}
#endif // _IRR_COMPILE_WITH_DIRECT3D_12_
