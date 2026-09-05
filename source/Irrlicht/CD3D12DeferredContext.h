// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// Deferred context for the native D3D12 driver, built on a real
// ID3D12GraphicsCommandList/ID3D12CommandAllocator rather than on the generic software
// abstraction of CCommandBufferDriver. Counterpart of CD3D11DeferredContext, but with the
// differences imposed by the D3D12 model:
//
//   - D3D11: ID3D11DeviceContext::CreateDeferredContext() + FinishCommandList() produces a
//     "software" ID3D11CommandList; ExecuteCommandList() injects it into the ImmediateContext's
//     stream, which keeps whatever state (render target/viewport) is already bound.
//   - D3D12: an ID3D12GraphicsCommandList is a self-contained recording of GPU commands, not a
//     delta applied on top of existing state. Submitted via ExecuteCommandLists() on the queue,
//     it inherits NO state from any other command list -- render target/viewport/shader-visible
//     descriptor heap must all be set INSIDE this command list if it is to draw anywhere.
//
// Render target choice: CD3D12Driver::beginScene()/endScene() record everything into a single
// ID3D12GraphicsCommandList per frame -- the PRESENT->RENDER_TARGET barrier (beginScene) and the
// RENDER_TARGET->PRESENT barrier (endScene) are part of that SAME command list, closed and
// submitted once, inside endScene(). So there is no point, between a beginScene() call and its
// matching endScene(), where the back buffer is actually in the RENDER_TARGET state on the GPU --
// the transition only takes effect when endScene()'s command list is submitted. A deferred
// context execute()'d (i.e. submitted via ExecuteCommandLists()) between those two calls would run
// BEFORE that transition on the GPU timeline, not after -- drawing to the back buffer from this
// context at that point would violate its resource state. Splicing a deferred context into the
// middle of the immediate frame would require restructuring endScene() to accept secondary
// command lists between its two barriers, which this context does not attempt.
//
// Instead this context draws into its OWN offscreen render target texture (created via
// CD3D12Driver::addRenderTargetTexture()), with its own depth/stencil -- fully self-contained,
// with no assumption about ordering relative to the immediate driver. Composing the result back
// into the swapchain frame (drawing Target as an SRV texture from the immediate driver, in a
// LATER frame) is not wired up here, but needs no extra API -- a render target texture is just an
// ITexture like any other (see getRenderTarget()).
//
// Reuses the existing generic IDeferredContext interface rather than a lighter D3D12-specific one,
// for a uniform API across all backends (see also CD3D12Driver.h near createDeferredContext()).

#ifndef __C_D3D12_DEFERRED_CONTEXT_H_INCLUDED__
#define __C_D3D12_DEFERRED_CONTEXT_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_12_

#include "CD3D12Driver.h"

namespace irr
{
	namespace video
	{
		class CD3D12DeferredContext : public CD3D12Driver, public IDeferredContext
		{
		public:
			//! `immediate` is shared (Device/DirectQueue/RootSignature/default shaders/NullTexture
			//! copied via ComPtr or raw pointer), not owned -- same as CD3D11DeferredContext, the
			//! caller guarantees `immediate` outlives this object.
			explicit CD3D12DeferredContext(CD3D12Driver* immediate);
			virtual ~CD3D12DeferredContext();

			//! No back buffer/Present of its own -- no-op, same as CD3D11DeferredContext.
			virtual bool beginScene(bool backBuffer = true, bool zBuffer = true,
				SColor color = SColor(255, 0, 0, 0),
				const SExposedVideoData& videoData = SExposedVideoData(),
				core::rect<s32>* sourceRect = 0) _IRR_OVERRIDE_;
			virtual bool endScene() _IRR_OVERRIDE_;

			//! No swapchain to resize on a deferred context -- logs and ignores rather than
			//! dereferencing a null SwapChain (inherited from CD3D12Driver::OnResize()).
			virtual void OnResize(const core::dimension2d<u32>& size) _IRR_OVERRIDE_;

			//! A null texture (ERT_FRAME_BUFFER) means this context's own target (see
			//! getRenderTarget()), never the immediate driver's back buffer; any other render
			//! target texture binds as on the immediate driver.
			virtual bool setRenderTarget(video::ITexture* texture, bool clearBackBuffer = true,
				bool clearZBuffer = true, SColor color = video::SColor(0, 0, 0, 0),
				video::ITexture* depthStencil = 0) _IRR_OVERRIDE_;

			// IDeferredContext
			virtual void execute(IVideoDriver* driver = nullptr) _IRR_OVERRIDE_;
			virtual void beginRecording() _IRR_OVERRIDE_;
			virtual size_t pendingCommandCount() const _IRR_OVERRIDE_ { return 0; }
			virtual void waitForCompletion() _IRR_OVERRIDE_;
			virtual IDeferredContext* getDeferredContextControl() _IRR_OVERRIDE_ { return this; }
			virtual core::dimension2d<u32> getRecordingSize() const _IRR_OVERRIDE_
			{
				return Target ? Target->getSize() : core::dimension2d<u32>(0, 0);
			}

			//! Nesting a deferred context inside a deferred context is not supported -- fails
			//! loudly rather than silently building something half-functional.
			virtual IVideoDriver* createDeferredContext() _IRR_OVERRIDE_;

			// --- Texture cache: delegated to the immediate driver ---
			//! A deferred context is a command list recorder, not a second owner of GPU
			//! resources, so its own texture cache stays empty and every call below forwards to
			//! ImmediateDriver. A texture is therefore only ever loaded/created once, and stays
			//! owned by the immediate driver.
			virtual ITexture* getTexture(const io::path& filename) _IRR_OVERRIDE_ { return ImmediateDriver->getTexture(filename); }
			virtual ITexture* getTexture(io::IReadFile* file) _IRR_OVERRIDE_ { return ImmediateDriver->getTexture(file); }
			virtual ITexture* getTexture(const core::array<io::path>& files, E_TEXTURE_TYPE Type) _IRR_OVERRIDE_ { return ImmediateDriver->getTexture(files, Type); }
			virtual ITexture* findTexture(const io::path& filename) _IRR_OVERRIDE_ { return ImmediateDriver->findTexture(filename); }
			virtual ITexture* getTextureByIndex(u32 index) _IRR_OVERRIDE_ { return ImmediateDriver->getTextureByIndex(index); }
			virtual u32 getTextureCount() const _IRR_OVERRIDE_ { return ImmediateDriver->getTextureCount(); }
			virtual void renameTexture(ITexture* texture, const io::path& newName) _IRR_OVERRIDE_ { ImmediateDriver->renameTexture(texture, newName); }
			virtual ITexture* addTexture(const core::dimension2d<u32>& size, const io::path& name,
				ECOLOR_FORMAT format = ECF_A8R8G8B8) _IRR_OVERRIDE_ { return ImmediateDriver->addTexture(size, name, format); }
			virtual ITexture* addTexture(const io::path& name, IImage* image, void* mipmapData = 0) _IRR_OVERRIDE_ { return ImmediateDriver->addTexture(name, image, mipmapData); }

			// Same reason as the textures above: this context skips the driver's own setup, so its descriptor table is empty.
			virtual IVertexDescriptor* getVertexDescriptor(u32 id) const _IRR_OVERRIDE_ { return ImmediateDriver->getVertexDescriptor(id); }
			virtual IVertexDescriptor* getVertexDescriptor(const core::stringc& pName) const _IRR_OVERRIDE_ { return ImmediateDriver->getVertexDescriptor(pName); }
			virtual u32 getVertexDescriptorCount() const _IRR_OVERRIDE_ { return ImmediateDriver->getVertexDescriptorCount(); }
			virtual IVertexDescriptor* addVertexDescriptor(const core::stringc& pName) _IRR_OVERRIDE_ { return ImmediateDriver->addVertexDescriptor(pName); }

			virtual IGPUProgrammingServices* getGPUProgrammingServices() _IRR_OVERRIDE_ { return ImmediateDriver->getGPUProgrammingServices(); }
			virtual ITexture* addRenderTargetTexture(const core::dimension2d<u32>& size,
				const io::path& name = "rt", const ECOLOR_FORMAT format = ECF_UNKNOWN) _IRR_OVERRIDE_
			{
				return ImmediateDriver->addRenderTargetTexture(size, name, format);
			}
			virtual ITexture* addRenderTargetTexture(const core::dimension2d<u32>& size,
				const io::path& name, const ECOLOR_FORMAT format,
				u32 sampleCount, u32 sampleQuality, u32 arraySlices) _IRR_OVERRIDE_
			{
				return ImmediateDriver->addRenderTargetTexture(size, name, format, sampleCount, sampleQuality, arraySlices);
			}
			virtual void removeTexture(ITexture* texture) _IRR_OVERRIDE_ { ImmediateDriver->removeTexture(texture); }
			virtual void removeAllTextures() _IRR_OVERRIDE_ { ImmediateDriver->removeAllTextures(); }

			// --- Material renderer registry: delegated to the immediate driver, same reason ---
			//! Not a copy: a material registered on the immediate driver AFTER this context was
			//! constructed (e.g. a user shader via addHighLevelShaderMaterial()) must still be
			//! visible from here, or its MaterialType would be out of range and no PSO could be
			//! built for it.
			virtual s32 addMaterialRenderer(IMaterialRenderer* renderer, const c8* name = 0) _IRR_OVERRIDE_ { return ImmediateDriver->addMaterialRenderer(renderer, name); }
			virtual IMaterialRenderer* getMaterialRenderer(u32 idx) _IRR_OVERRIDE_ { return ImmediateDriver->getMaterialRenderer(idx); }
			virtual u32 getMaterialRendererCount() const _IRR_OVERRIDE_ { return ImmediateDriver->getMaterialRendererCount(); }
			virtual const c8* getMaterialRendererName(u32 idx) const _IRR_OVERRIDE_ { return ImmediateDriver->getMaterialRendererName(idx); }
			virtual void setMaterialRendererName(s32 idx, const c8* name) _IRR_OVERRIDE_ { ImmediateDriver->setMaterialRendererName(idx, name); }

			//! Used by CD3D12Driver::createDeferredContext() to check that construction
			//! (device/allocator/command list) succeeded before handing this object to the caller.
			ID3D12GraphicsCommandList* getCommandList() const { return CommandList.Get(); }

			//! The offscreen render target texture this context draws into (see file header
			//! comment). Lives in the TextureCache shared with ImmediateDriver, so it outlives
			//! this context and is only released by ~CD3D12Driver() on the immediate driver.
			//! Exposed so it can later be composed into another frame as a normal SRV texture.
			virtual ITexture* getRenderTarget() const _IRR_OVERRIDE_ { return Target; }

		private:
			CD3D12Driver* ImmediateDriver;
			ITexture* Target = nullptr;

			//! Resets the ring cursors (constants/vertex/shader-visible SRV) of the current frame
			//! slot, (re)binds its shader-visible SRV heap on CommandList, and targets this
			//! context's own Target render target/depth-stencil, cleared. Shared by the constructor
			//! and beginRecording().
			void prepareRecordingState();
			//! Binds Target with this context's depth-stencil and refreshes the render-target
			//! bookkeeping the PSO key and the 2D projection read (as CD3D12Driver::setRenderTarget()
			//! does for a texture). Used at recording start and by setRenderTarget(0).
			void bindOwnTarget(bool clearColor, bool clearDepth, SColor color);
		};
	}
}
#endif // _IRR_COMPILE_WITH_DIRECT3D_12_
#endif
