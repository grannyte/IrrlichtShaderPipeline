// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// Vulkan 2D textures and render target textures. One object owns the VkImage, its
// VkDeviceMemory, the VkImageView used for binding and a VkSampler. VkImageLayout is
// tracked per texture, valid as long as an image is not recorded on two buffers at once.

#ifndef __C_VULKAN_TEXTURE_H_INCLUDED__
#define __C_VULKAN_TEXTURE_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include "ITexture.h"
#include "IImage.h"
#include "SMaterialLayer.h"
#include "CVulkanHelpers.h"

namespace irr
{
	namespace video
	{
		class CVulkanTexture : public ITexture
		{
		public:
			//! Loading path. ETCF_CREATE_MIP_MAPS in `flags` builds and fills the mip chain.
			CVulkanTexture(const SVulkanContext& context, IVulkanUploadContext& upload,
				IImage* image, u32 flags, const io::path& name);

			//! Empty or render target texture, single mip. A depth format gives a
			//! depth/stencil attachment, anything else a color one; both stay sampled.
			CVulkanTexture(const SVulkanContext& context, IVulkanUploadContext& upload,
				const core::dimension2d<u32>& size, ECOLOR_FORMAT format,
				bool renderTarget, const io::path& name);

			virtual ~CVulkanTexture();

			//! Maps a staging buffer holding one mip level; the image itself is never mapped
			//! (optimal tiling has no CPU layout). Returns 0 for a block-compressed format.
			virtual void* lock(E_TEXTURE_LOCK_MODE mode = ETLM_READ_WRITE, u32 mipmapLevel = 0) _IRR_OVERRIDE_;
			virtual void unlock() _IRR_OVERRIDE_;

			//! Re-blits the chain from the current mip 0; `mipmapData` is ignored.
			virtual void regenerateMipMapLevels(void* mipmapData = 0) _IRR_OVERRIDE_;

			//! ECOLOR_FORMAT -> VkFormat, VK_FORMAT_UNDEFINED (logged) when unsupported.
			//! Public so the driver can describe attachments with the same table.
			static VkFormat getVulkanFormat(ECOLOR_FORMAT format);

			static bool isDepthFormat(VkFormat format);

			VkImage getImage() const { return Image; }
			VkImageView getImageView() const { return View; }
			VkSampler getSampler() const { return Sampler; }
			VkFormat getVkFormat() const { return Format; }
			VkImageLayout getImageLayout() const { return CurrentLayout; }
			VkImageAspectFlags getAspectMask() const { return Aspect; }
			u32 getMipLevelCount() const { return MipLevelCount; }

			//! False when the image could not be created; a constructor cannot report that
			//! otherwise, so the caller must check this and drop the object.
			bool hasDeviceResource() const { return Image != VK_NULL_HANDLE; }

			//! Barrier over every mip, then updates CurrentLayout. No-op if already there.
			void transitionTo(VkCommandBuffer commandBuffer, VkImageLayout newLayout);

			//! For a layout change the texture did not record, e.g. a pass' finalLayout.
			void setImageLayout(VkImageLayout layout) { CurrentLayout = layout; }

			//! Rebuilds the sampler from SMaterialLayer settings, no-op if unchanged.
			bool createSampler(bool bilinear, bool trilinear, u8 anisotropic, E_TEXTURE_CLAMP wrap);

		private:
			bool createImage(VkImageUsageFlags usage);
			bool createImageView();

			//! Staging buffer + copy into mip 0, ending in SHADER_READ_ONLY_OPTIMAL.
			//! `expandR8G8B8` widens 24 bit source rows to 32 bit with alpha 0xFF.
			bool uploadImageData(const void* data, u32 sourcePitchBytes, u32 dataSizeBytes,
				bool expandR8G8B8);

			void destroyStagingBuffer();

			//! floor(log2(max(w,h))) + 1, i.e. a chain down to 1x1.
			static u32 computeMipLevels(u32 width, u32 height);

			//! vkCmdBlitImage needs the format linearly filterable in optimal tiling.
			bool supportsLinearBlit() const;

			void generateMips();

			//! Bytes per pixel of ColorFormat, 0 for a block-compressed format.
			u32 getBytesPerPixel() const;

			const SVulkanContext& Context;
			IVulkanUploadContext& Upload;

			VkImage Image = VK_NULL_HANDLE;
			VkDeviceMemory Memory = VK_NULL_HANDLE;
			VkImageView View = VK_NULL_HANDLE;
			VkSampler Sampler = VK_NULL_HANDLE;
			VkFormat Format = VK_FORMAT_UNDEFINED;
			VkImageLayout CurrentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			VkImageAspectFlags Aspect = VK_IMAGE_ASPECT_COLOR_BIT;
			u32 MipLevelCount = 1;

			// Settings the current sampler was built with, tested by createSampler().
			bool SamplerBilinear = true;
			bool SamplerTrilinear = false;
			u8 SamplerAnisotropic = 0;
			E_TEXTURE_CLAMP SamplerWrap = ETC_REPEAT;

			// lock()/unlock() staging buffer, created per lock and destroyed on unlock.
			VkBuffer StagingBuffer = VK_NULL_HANDLE;
			VkDeviceMemory StagingMemory = VK_NULL_HANDLE;
			void* MappedData = nullptr;
			VkDeviceSize StagingSize = 0;
			E_TEXTURE_LOCK_MODE LastLockMode = ETLM_READ_WRITE;
			u32 LastLockMipLevel = 0;
		};

	}
}

#endif // _IRR_COMPILE_WITH_VULKAN_
#endif
