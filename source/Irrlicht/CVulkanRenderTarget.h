// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// Vulkan render targets: the attachment set one setRenderTarget() call binds, the VkRenderingInfo
// handed to vk::CmdBeginRendering, and a pool of depth images shared by the targets created without
// one of their own. This backend requires VK_KHR_dynamic_rendering, so there is no VkRenderPass path
// and the attachments are described afresh on every begin; the VkRenderingAttachmentInfo arrays are
// members, not locals, because CmdBeginRendering reads through the pointers.

#ifndef __C_VULKAN_RENDER_TARGET_H_INCLUDED__
#define __C_VULKAN_RENDER_TARGET_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include "CVulkanHelpers.h"
#include "SColor.h"
#include "dimension2d.h"
#include <vector>

namespace irr
{
	namespace video
	{
		class CVulkanTexture;

		//! One pooled depth/stencil image owned by CVulkanDepthBufferPool: never sampled or locked.
		struct SVulkanRTTDepthBuffer
		{
			// Width/Height/SampleCount together are the pool key.
			u32 Width = 0;
			u32 Height = 0;
			VkSampleCountFlagBits SampleCount = VK_SAMPLE_COUNT_1_BIT;

			VkFormat Format = VK_FORMAT_UNDEFINED;
			VkImage Image = VK_NULL_HANDLE;
			VkDeviceMemory Memory = VK_NULL_HANDLE;
			VkImageView View = VK_NULL_HANDLE;
			VkImageAspectFlags Aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
			//! Tracked like CVulkanTexture::CurrentLayout, so a reuse emits the right barrier.
			VkImageLayout CurrentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		};

		//! Depth/stencil buffers shared by every render target that came without an explicit depth
		//! attachment, indexed by (width, height, sample count) -- the same strategy as the D3D12
		//! driver's pool, refcounting aside: it grows as new keys are met and lives until destroyed.
		class CVulkanDepthBufferPool
		{
		public:
			//! `preferredFormat` is a hint; the device's support decides.
			CVulkanDepthBufferPool(const SVulkanContext& context,
				VkFormat preferredFormat = VK_FORMAT_D32_SFLOAT);

			~CVulkanDepthBufferPool();

			//! The entry matching the key, created on first request; 0 (logged) means "no depth".
			SVulkanRTTDepthBuffer* checkDepthBuffer(u32 width, u32 height,
				VkSampleCountFlagBits sampleCount = VK_SAMPLE_COUNT_1_BIT);

			void clear();

			VkFormat getDepthFormat() const { return DepthFormat; }
			u32 getBufferCount() const { return (u32)Buffers.size(); }

		private:
			// No copying: the entries are raw device objects with single ownership.
			CVulkanDepthBufferPool(const CVulkanDepthBufferPool&);
			CVulkanDepthBufferPool& operator=(const CVulkanDepthBufferPool&);

			bool createDepthBuffer(SVulkanRTTDepthBuffer& depth);
			VkFormat pickDepthFormat(VkFormat preferred) const;

			const SVulkanContext& Context;
			VkFormat DepthFormat = VK_FORMAT_UNDEFINED;
			std::vector<SVulkanRTTDepthBuffer*> Buffers;
		};

		//! The attachment set of one setRenderTarget(): up to 8 colour textures plus an optional depth
		//! one, VkFormats cached so the pipeline key needs no re-query. Owns none of them.
		class CVulkanRenderTarget
		{
		public:
			//! The D3D12 limit, and the floor Vulkan guarantees for maxColorAttachments.
			static const u32 MaxColorAttachments = 8;

			CVulkanRenderTarget(const SVulkanContext& context);
			~CVulkanRenderTarget();

			//! Colour textures validated against the first (usable, non-depth, same size) and truncated
			//! at the first bad entry rather than rejected wholesale. `depthTexture` is the explicit depth
			//! attachment, or 0 to borrow one from `depthPool`; with neither, the target has no depth.
			bool setTargets(CVulkanTexture* const* colorTextures, u32 colorCount,
				CVulkanTexture* depthTexture, CVulkanDepthBufferPool* depthPool);

			//! `layer` selects one slice of an array texture as the colour attachment (the
			//! setRenderTargetSlice() case); WholeImage binds the texture's own view.
			static const u32 WholeImage = ~0u;
			bool setTarget(CVulkanTexture* colorTexture, CVulkanTexture* depthTexture,
				CVulkanDepthBufferPool* depthPool, u32 layer = WholeImage);

			void reset();

			//! Colour attachments to COLOR_ATTACHMENT_OPTIMAL and depth to
			//! DEPTH_STENCIL_ATTACHMENT_OPTIMAL, ready for CmdBeginRendering.
			void bind(VkCommandBuffer commandBuffer);

			//! Colour attachments back to SHADER_READ_ONLY_OPTIMAL so the next pass can sample them. An
			//! explicit depth texture follows when `depthToShaderRead`; a pooled one has no sampled usage.
			void unbind(VkCommandBuffer commandBuffer, bool depthToShaderRead = true);

			//! A populated VkRenderingInfo for vk::CmdBeginRendering, its arrays owned here and valid
			//! until the next call to this method or setTargets()/reset(). storeOp is always STORE.
			const VkRenderingInfo& getRenderingInfo(bool clearColor, bool clearDepth, SColor color);

			//! Clear values in attachment order, for a mid-pass vk::CmdClearAttachments.
			const VkClearValue* getClearValues(SColor color);

			u32 getClearValueCount() const { return ColorCount + (hasDepth() ? 1u : 0u); }

			u32 getColorAttachmentCount() const { return ColorCount; }

			//! MaxColorAttachments entries, VK_FORMAT_UNDEFINED past the attachment count.
			const VkFormat* getColorFormats() const { return ColorFormats; }

			VkFormat getColorFormat(u32 index) const
			{
				return (index < MaxColorAttachments) ? ColorFormats[index] : VK_FORMAT_UNDEFINED;
			}

			//! VK_FORMAT_UNDEFINED without depth: a pipeline declaring one while none is bound is rejected.
			VkFormat getDepthFormat() const { return DepthFormat; }

			VkSampleCountFlagBits getSampleCount() const { return SampleCount; }

			bool hasDepth() const { return DepthView != VK_NULL_HANDLE; }

			const core::dimension2d<u32>& getSize() const { return Size; }

			//! False until a successful setTargets(); nothing else may be called before then.
			bool isValid() const { return ColorCount > 0; }

			CVulkanTexture* getColorTexture(u32 index) const
			{
				return (index < ColorCount) ? ColorTextures[index] : 0;
			}

			//! The explicit depth texture, or 0 when the depth image came from the pool.
			CVulkanTexture* getDepthTexture() const { return DepthTexture; }

		private:
			//! No copying: the rendering info points into this object's own arrays.
			CVulkanRenderTarget(const CVulkanRenderTarget&);
			CVulkanRenderTarget& operator=(const CVulkanRenderTarget&);

			void attachDepth(CVulkanTexture* depthTexture, CVulkanDepthBufferPool* depthPool);

			const SVulkanContext& Context;

			CVulkanTexture* ColorTextures[MaxColorAttachments] = {};
			VkFormat ColorFormats[MaxColorAttachments] = {};
			u32 ColorCount = 0;
			//! Slice of ColorTextures[0] bound as the attachment, or WholeImage. Single-target only.
			u32 ColorLayer = WholeImage;

			CVulkanTexture* DepthTexture = 0;		//!< Explicit attachment, owned elsewhere.
			SVulkanRTTDepthBuffer* PooledDepth = 0;	//!< Borrowed, owned by the pool.
			VkImageView DepthView = VK_NULL_HANDLE;	//!< From whichever of the two is in use.
			VkFormat DepthFormat = VK_FORMAT_UNDEFINED;
			VkImageAspectFlags DepthAspect = VK_IMAGE_ASPECT_DEPTH_BIT;

			//! Part of both the pipeline key and the pool key: the colour textures' sample count.
			VkSampleCountFlagBits SampleCount = VK_SAMPLE_COUNT_1_BIT;
			core::dimension2d<u32> Size = core::dimension2d<u32>(0, 0);

			// Storage read through by CmdBeginRendering, hence members and not locals.
			VkRenderingAttachmentInfo ColorAttachments[MaxColorAttachments] = {};
			VkRenderingAttachmentInfo DepthAttachment = {};
			VkRenderingInfo RenderingInfo = {};
			//! Colours then depth, so index ColorCount is the depth value when there is one.
			VkClearValue ClearValues[MaxColorAttachments + 1] = {};
		};

	}
}

#endif // _IRR_COMPILE_WITH_VULKAN_
#endif
