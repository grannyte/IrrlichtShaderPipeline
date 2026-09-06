// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// Real VkCommandBuffer counterpart to CD3D12DeferredContext -- draws into its own render target
// since a Vulkan command buffer can't submit mid-frame into the swapchain-bracketed primary one.

#ifndef __C_VULKAN_DEFERRED_CONTEXT_H_INCLUDED__
#define __C_VULKAN_DEFERRED_CONTEXT_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include "CVulkanDriver.h"

namespace irr
{
	namespace video
	{
		class CVulkanDeferredContext : public CVulkanDriver, public IDeferredContext
		{
		public:
			//! `immediate` is borrowed, not owned; the caller keeps it alive for as long as this
			//! context exists, as with the D3D11 and D3D12 deferred contexts.
			explicit CVulkanDeferredContext(CVulkanDriver* immediate);
			virtual ~CVulkanDeferredContext();

			//! Whether construction built everything a recording needs; the immediate driver's
			//! createDeferredContext() refuses to hand out a context that did not.
			bool isReady() const { return Ready; }

			//! No swapchain of its own: beginRecording()/execute() are the frame boundaries.
			virtual bool beginScene(bool backBuffer = true, bool zBuffer = true,
				SColor color = SColor(255, 0, 0, 0),
				const SExposedVideoData& videoData = SExposedVideoData(),
				core::rect<s32>* sourceRect = 0) _IRR_OVERRIDE_;
			virtual bool endScene() _IRR_OVERRIDE_;

			//! Nothing to resize; logged and ignored rather than touching the owner's swapchain.
			virtual void OnResize(const core::dimension2d<u32>& size) _IRR_OVERRIDE_;

			//! A null texture (ERT_FRAME_BUFFER) means this context's own target, not the owner's
			//! swapchain; any other texture binds as on the immediate driver.
			virtual bool setRenderTarget(video::ITexture* texture, bool clearBackBuffer = true,
				bool clearZBuffer = true, SColor color = video::SColor(0, 0, 0, 0),
				video::ITexture* depthStencil = 0) _IRR_OVERRIDE_;

			// IDeferredContext
			//! Ends the recording, submits it on the shared queue with this slot's fence and
			//! leaves the target in the sampled layout. `driver` is unused: the submission needs
			//! nothing from the caller, the queue being shared.
			virtual void execute(IVideoDriver* driver = nullptr) _IRR_OVERRIDE_;
			//! Moves to the next frame slot, waits for the GPU to be done with it and starts a new
			//! recording targeting the cleared render target. Called once by the constructor.
			virtual void beginRecording() _IRR_OVERRIDE_;
			virtual size_t pendingCommandCount() const _IRR_OVERRIDE_ { return 0; }
			//! Blocks until every submission made by execute() has completed.
			virtual void waitForCompletion() _IRR_OVERRIDE_;
			virtual IDeferredContext* getDeferredContextControl() _IRR_OVERRIDE_ { return this; }
			virtual core::dimension2d<u32> getRecordingSize() const _IRR_OVERRIDE_
			{
				return Target ? Target->getSize() : core::dimension2d<u32>(0, 0);
			}

			//! Nesting is refused, as on the D3D drivers.
			virtual IVideoDriver* createDeferredContext() _IRR_OVERRIDE_;

			// --- Texture cache, vertex descriptors and material registry: the immediate driver's ---
			//! A deferred context records commands; it owns no GPU resource of the scene. Every
			//! lookup and creation below forwards, so a texture exists once and a material registered
			//! after this context was created is still visible from it.
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
			virtual ITexture* addUAVTexture(const core::dimension2d<u32>& size, const io::path& name,
				const ECOLOR_FORMAT format = ECF_UNKNOWN) _IRR_OVERRIDE_
			{
				return ImmediateDriver->addUAVTexture(size, name, format);
			}
			virtual void removeTexture(ITexture* texture) _IRR_OVERRIDE_ { ImmediateDriver->removeTexture(texture); }
			virtual void removeAllTextures() _IRR_OVERRIDE_ { ImmediateDriver->removeAllTextures(); }

			virtual IVertexDescriptor* getVertexDescriptor(u32 id) const _IRR_OVERRIDE_ { return ImmediateDriver->getVertexDescriptor(id); }
			virtual IVertexDescriptor* getVertexDescriptor(const core::stringc& pName) const _IRR_OVERRIDE_ { return ImmediateDriver->getVertexDescriptor(pName); }
			virtual u32 getVertexDescriptorCount() const _IRR_OVERRIDE_ { return ImmediateDriver->getVertexDescriptorCount(); }
			virtual IVertexDescriptor* addVertexDescriptor(const core::stringc& pName) _IRR_OVERRIDE_ { return ImmediateDriver->addVertexDescriptor(pName); }

			virtual IGPUProgrammingServices* getGPUProgrammingServices() _IRR_OVERRIDE_ { return ImmediateDriver->getGPUProgrammingServices(); }
			virtual s32 addMaterialRenderer(IMaterialRenderer* renderer, const c8* name = 0) _IRR_OVERRIDE_ { return ImmediateDriver->addMaterialRenderer(renderer, name); }
			virtual IMaterialRenderer* getMaterialRenderer(u32 idx) _IRR_OVERRIDE_ { return ImmediateDriver->getMaterialRenderer(idx); }
			virtual u32 getMaterialRendererCount() const _IRR_OVERRIDE_ { return ImmediateDriver->getMaterialRendererCount(); }
			virtual const c8* getMaterialRendererName(u32 idx) const _IRR_OVERRIDE_ { return ImmediateDriver->getMaterialRendererName(idx); }
			virtual void setMaterialRendererName(s32 idx, const c8* name) _IRR_OVERRIDE_ { ImmediateDriver->setMaterialRendererName(idx, name); }

			//! The render target texture every recording draws into. Named
			//! "CVulkanDeferredContext_Target" in the immediate driver's cache, which owns it; after
			//! execute() it is in the sampled layout, ready to be drawn by the immediate driver.
			virtual ITexture* getRenderTarget() const _IRR_OVERRIDE_ { return Target; }

		private:
			CVulkanDriver* ImmediateDriver;
			ITexture* Target = nullptr;
			bool Ready = false;
			//! Whether execute() submitted the slot since its fence was last reset; a fence that was
			//! never submitted stays in whatever state the last wait left it.
			bool Submitted[FrameCount] = {};

			//! Binds Target (cleared to opaque black) with a depth buffer from the pool on the
			//! current frame's command buffer, so the first draw of a recording has somewhere to go.
			bool prepareRecordingState();
		};
	}
}
#endif // _IRR_COMPILE_WITH_VULKAN_
#endif
