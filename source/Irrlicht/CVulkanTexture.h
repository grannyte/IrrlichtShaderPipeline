// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#ifndef __C_VULKAN_TEXTURE_H_INCLUDED__
#define __C_VULKAN_TEXTURE_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include "ITexture.h"
#include "IImage.h"
#include "SMaterialLayer.h" // E_TEXTURE_CLAMP
#include "irrArray.h"
#include "CVulkanHelpers.h"
#include <vector>

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

			//! Empty, render target or storage texture, single mip. A depth format gives a
			//! depth/stencil attachment, anything else a color one; both stay sampled.
			//! `arrayLayers` > 1 makes an ETT_2D_ARRAY whose layers are reachable one at a time
			//! through getLayerView(); `storage` adds STORAGE usage for compute writes
			//! (isUnorderedAccess()). `sampleCount` > 1 (a power of two the caller has checked
			//! against the device) makes a multisampled colour target: the samples live in a second
			//! image that is the attachment (getAttachmentView()) and is resolved into this one at the
			//! end of every pass, so sampling, lock() and copyTexture() see single-sample texels -- the
			//! explicit-resolve scheme CD3D12Texture uses. A multisampled depth texture has no resolve
			//! companion: the image itself carries the samples.
			CVulkanTexture(const SVulkanContext& context, IVulkanUploadContext& upload,
				const core::dimension2d<u32>& size, ECOLOR_FORMAT format,
				bool renderTarget, const io::path& name, u32 arrayLayers = 1, bool storage = false,
				u32 sampleCount = 1);

			//! Cube map / 2D array / cube array built from already-uploaded Vulkan textures of one
			//! size and format: every slice's mip chain is copied on the GPU into one layer. `type`
			//! is ETT_CUBE (6 slices), ETT_CUBE_ARRAY (a multiple of 6) or ETT_2D_ARRAY.
			CVulkanTexture(const SVulkanContext& context, IVulkanUploadContext& upload,
				const core::array<ITexture*>& slices, E_TEXTURE_TYPE type, const io::path& name);

			virtual ~CVulkanTexture();

			//! Maps a staging buffer holding one mip level of layer 0; the image itself is never
			//! mapped (optimal tiling has no CPU layout). Returns 0 for a block-compressed format.
			virtual void* lock(E_TEXTURE_LOCK_MODE mode = ETLM_READ_WRITE, u32 mipmapLevel = 0) _IRR_OVERRIDE_;
			virtual void unlock() _IRR_OVERRIDE_;

			//! Re-blits the chain from the current mip 0 of every layer; `mipmapData` is ignored.
			virtual void regenerateMipMapLevels(void* mipmapData = 0) _IRR_OVERRIDE_;

			//! ECOLOR_FORMAT -> VkFormat, VK_FORMAT_UNDEFINED (logged) when unsupported.
			//! Public so the driver can describe attachments with the same table.
			static VkFormat getVulkanFormat(ECOLOR_FORMAT format);

			static bool isDepthFormat(VkFormat format);

			VkImage getImage() const { return Image; }
			//! The view over every layer: 2D, 2D_ARRAY, CUBE or CUBE_ARRAY per getTextureType().
			VkImageView getImageView() const { return View; }
			//! What a render pass attaches: the multisampled image's view for a multisampled colour
			//! target (getImageView() is then the resolve destination), getImageView() otherwise.
			VkImageView getAttachmentView() const { return MsaaView != VK_NULL_HANDLE ? MsaaView : View; }
			//! Samples a pass rendering into this texture rasterizes at; 1 for everything but a
			//! multisampled render target.
			VkSampleCountFlagBits getSampleCount() const { return SampleCount; }
			//! Layout barrier on the multisampled companion image, a no-op when there is none.
			void transitionMultisampleTo(VkCommandBuffer commandBuffer, VkImageLayout newLayout);
			//! A 2D view over one layer, created on first use, for binding a slice as an attachment
			//! or copying into it. Layer 0 of a single-layer texture is getImageView() itself.
			VkImageView getLayerView(u32 layer);
			VkSampler getSampler() const { return Sampler; }
			VkFormat getVkFormat() const { return Format; }
			VkImageLayout getImageLayout() const { return CurrentLayout; }
			VkImageAspectFlags getAspectMask() const { return Aspect; }
			u32 getMipLevelCount() const { return MipLevelCount; }
			u32 getLayerCount() const { return LayerCount; }

			//! False when the image could not be created; a constructor cannot report that
			//! otherwise, so the caller must check this and drop the object.
			bool hasDeviceResource() const { return Image != VK_NULL_HANDLE; }

			//! Barrier over every mip and layer, then updates CurrentLayout. No-op if already there.
			void transitionTo(VkCommandBuffer commandBuffer, VkImageLayout newLayout);

			//! For a layout change the texture did not record, e.g. a pass' finalLayout.
			void setImageLayout(VkImageLayout layout) { CurrentLayout = layout; }

			//! Rebuilds the sampler from SMaterialLayer settings, no-op if unchanged.
			bool createSampler(bool bilinear, bool trilinear, u8 anisotropic, E_TEXTURE_CLAMP wrap);

		private:
			bool createImage(VkImageUsageFlags usage);
			bool createImageView();
			//! The multisampled companion of a colour render target (SampleCount samples, one mip,
			//! LayerCount layers) and its view.
			bool createMultisampleImage(VkImageUsageFlags usage);

			//! Staging buffer + copy into mip 0 of layer 0, ending in SHADER_READ_ONLY_OPTIMAL.
			//! `expandR8G8B8` widens 24 bit source rows to 32 bit with alpha 0xFF.
			bool uploadImageData(const void* data, u32 sourcePitchBytes, u32 dataSizeBytes,
				bool expandR8G8B8);

			//! The raw-file path of CImageLoaderDDS (block-compressed, wide format, cube map, array
			//! or volume), which hands over the RAW .dds file (header included,
			//! CImage::CompressedSize bytes) rather than pixels, the way DDSTextureLoader12 consumes
			//! it on D3D12. Parses the header, creates the image with every mip level, cube face,
			//! array slice and depth slice the file carries, and uploads them all in one copy. False
			//! (logged) on a malformed or truncated file.
			bool uploadDdsFile(const u8* bytes, u32 byteCount);

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
			//! Per-layer 2D views, filled lazily by getLayerView(); empty for a single-layer image.
			std::vector<VkImageView> LayerViews;
			VkSampler Sampler = VK_NULL_HANDLE;
			VkFormat Format = VK_FORMAT_UNDEFINED;
			VkImageLayout CurrentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			VkImageAspectFlags Aspect = VK_IMAGE_ASPECT_COLOR_BIT;
			u32 MipLevelCount = 1;
			u32 LayerCount = 1;
			//! Slices of an ETT_3D texture (a volume .dds); 1 for everything else.
			u32 Depth = 1;

			//! Sample count of the attachment a pass renders into (see getSampleCount()).
			VkSampleCountFlagBits SampleCount = VK_SAMPLE_COUNT_1_BIT;
			//! Samples of Image itself: SampleCount for a multisampled depth texture, 1 otherwise --
			//! a multisampled colour target keeps Image single-sample as the resolve destination.
			VkSampleCountFlagBits ImageSamples = VK_SAMPLE_COUNT_1_BIT;
			//! The multisampled companion of a colour render target, see the constructor comment.
			VkImage MsaaImage = VK_NULL_HANDLE;
			VkDeviceMemory MsaaMemory = VK_NULL_HANDLE;
			VkImageView MsaaView = VK_NULL_HANDLE;
			VkImageLayout MsaaLayout = VK_IMAGE_LAYOUT_UNDEFINED;

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
