// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "CVulkanTexture.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_
#include "CImage.h" // CompressedSize, the raw .dds byte count the DDS loader leaves on the image
#include "os.h"
#include <string.h>

namespace irr
{
	namespace video
	{
		namespace
		{
			//! Size of `level` in a chain starting at `base`, never below 1x1.
			void getMipDimension(const core::dimension2d<u32>& base, u32 level, u32& width, u32& height)
			{
				width = base.Width >> level;
				height = base.Height >> level;
				if (!width)
					width = 1;
				if (!height)
					height = 1;
			}

			VkSamplerAddressMode getAddressMode(E_TEXTURE_CLAMP clamp)
			{
				switch (clamp)
				{
				case ETC_REPEAT:                   return VK_SAMPLER_ADDRESS_MODE_REPEAT;
				case ETC_CLAMP:
				case ETC_CLAMP_TO_EDGE:            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
				case ETC_CLAMP_TO_BORDER:          return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
				case ETC_MIRROR:                   return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
				// MIRROR_CLAMP_TO_EDGE is an optional feature (samplerMirrorClampToEdge),
				// not queried here; the plain clamp is the safe approximation.
				case ETC_MIRROR_CLAMP:
				case ETC_MIRROR_CLAMP_TO_EDGE:
				case ETC_MIRROR_CLAMP_TO_BORDER:   return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
				default:                           return VK_SAMPLER_ADDRESS_MODE_REPEAT;
				}
			}
		}

		VkFormat CVulkanTexture::getVulkanFormat(ECOLOR_FORMAT format)
		{
			switch (format)
			{
			// ECF_A8R8G8B8 is 0xAARRGGBB, i.e. B,G,R,A in memory: B8G8R8A8 reads byte 0
			// as blue and needs no shader swizzle.
			case ECF_A8R8G8B8:      return VK_FORMAT_B8G8R8A8_UNORM;
			case ECF_A8R8G8B8S:     return VK_FORMAT_R8G8B8A8_SNORM;
			// No unpacked 24 bit format is guaranteed to be sampleable, so ECF_R8G8B8 is
			// promoted to 32 bit (alpha forced to 0xFF on upload) and the caller updates
			// ColorFormat to ECF_A8R8G8B8.
			case ECF_R8G8B8:        return VK_FORMAT_R8G8B8A8_UNORM;
			case ECF_A1R5G5B5:      return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
			case ECF_R5G6B5:        return VK_FORMAT_R5G6B5_UNORM_PACK16;
			case ECF_R8:            return VK_FORMAT_R8_UNORM;
			case ECF_R8S:           return VK_FORMAT_R8_SNORM;
			case ECF_R8G8:          return VK_FORMAT_R8G8_UNORM;
			case ECF_R16:           return VK_FORMAT_R16_UNORM;
			case ECF_R16G16:        return VK_FORMAT_R16G16_UNORM;
			case ECF_R16F:          return VK_FORMAT_R16_SFLOAT;
			case ECF_G16R16F:       return VK_FORMAT_R16G16_SFLOAT;
			case ECF_A16B16G16R16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
			case ECF_R32F:          return VK_FORMAT_R32_SFLOAT;
			case ECF_G32R32F:       return VK_FORMAT_R32G32_SFLOAT;
			case ECF_B32G32R32F:    return VK_FORMAT_R32G32B32_SFLOAT;
			case ECF_A32B32G32R32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
			// Block-compressed. ECF_DXT2/4 (premultiplied alpha) share the BC2/BC3 block
			// layout, there is no dedicated VkFormat for them.
			case ECF_DXT1:          return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
			case ECF_DXT1_SRGB:     return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
			case ECF_DXT2:
			case ECF_DXT3:          return VK_FORMAT_BC2_UNORM_BLOCK;
			case ECF_DXT3_SRGB:     return VK_FORMAT_BC2_SRGB_BLOCK;
			case ECF_DXT4:
			case ECF_DXT5:          return VK_FORMAT_BC3_UNORM_BLOCK;
			case ECF_DXT5_SRGB:     return VK_FORMAT_BC3_SRGB_BLOCK;
			case ECF_BC4_U:         return VK_FORMAT_BC4_UNORM_BLOCK;
			case ECF_BC4_S:         return VK_FORMAT_BC4_SNORM_BLOCK;
			case ECF_BC5_U:         return VK_FORMAT_BC5_UNORM_BLOCK;
			case ECF_BC5_S:         return VK_FORMAT_BC5_SNORM_BLOCK;
			case ECF_BC6_U:         return VK_FORMAT_BC6H_UFLOAT_BLOCK;
			case ECF_BC6_S:         return VK_FORMAT_BC6H_SFLOAT_BLOCK;
			case ECF_BC7_U:         return VK_FORMAT_BC7_UNORM_BLOCK;
			case ECF_BC7_S:         return VK_FORMAT_BC7_SRGB_BLOCK;
			case ECF_D16:           return VK_FORMAT_D16_UNORM;
			case ECF_D32:           return VK_FORMAT_D32_SFLOAT;
			case ECF_D24S8:         return VK_FORMAT_D24_UNORM_S8_UINT;
			case ECF_DF32S8:        return VK_FORMAT_D32_SFLOAT_S8_UINT;
			default:
				os::Printer::log("CVulkanTexture: unsupported ECOLOR_FORMAT", ELL_ERROR);
				return VK_FORMAT_UNDEFINED;
			}
		}

		bool CVulkanTexture::isDepthFormat(VkFormat format)
		{
			switch (format)
			{
			case VK_FORMAT_D16_UNORM:
			case VK_FORMAT_D32_SFLOAT:
			case VK_FORMAT_D24_UNORM_S8_UINT:
			case VK_FORMAT_D32_SFLOAT_S8_UINT:
				return true;
			default:
				return false;
			}
		}

		CVulkanTexture::CVulkanTexture(const SVulkanContext& context, IVulkanUploadContext& upload,
			IImage* image, u32 flags, const io::path& name)
			: ITexture(name), Context(context), Upload(upload)
		{
			DriverType = EDT_VULKAN;
			TextureType = ETT_2D;
			Source = ETS_UNKNOWN;

			if (!image)
			{
				os::Printer::log("CVulkanTexture: null image", ELL_ERROR);
				return;
			}

			OriginalSize = Size = image->getDimension();
			const ECOLOR_FORMAT sourceFormat = image->getColorFormat();
			Format = getVulkanFormat(sourceFormat);
			if (Format == VK_FORMAT_UNDEFINED)
				return; // already logged

			HasAlpha = IImage::hasAlphaFormat(sourceFormat);

			const bool expandR8G8B8 = (sourceFormat == ECF_R8G8B8);
			ColorFormat = expandR8G8B8 ? ECF_A8R8G8B8 : sourceFormat;
			Aspect = VK_IMAGE_ASPECT_COLOR_BIT;

			// A block-compressed, wide-format, cube, array or volume .dds arrives as the whole file,
			// not as pixels: the loader leaves the header in place so a driver can read the mip
			// chain, cube faces, slices and depth out of it -- see CImageLoaderDDS and
			// CD3D12Texture, which does the same.
			if (image->isCompressed())
			{
				const u8* bytes = static_cast<const u8*>(image->lock());
				const u32 byteCount = static_cast<CImage*>(image)->CompressedSize;
				const bool uploaded = bytes && uploadDdsFile(bytes, byteCount);
				image->unlock();
				if (!uploaded)
				{
					os::Printer::log("CVulkanTexture: could not upload the .dds file", name, ELL_ERROR);
					return;
				}
				createSampler(true, MipLevelCount > 1, 0,
					(TextureType == ETT_CUBE || TextureType == ETT_CUBE_ARRAY || TextureType == ETT_3D) ?
					ETC_CLAMP_TO_EDGE : ETC_REPEAT);
				return;
			}

			// The blit chain writes each level with a filtered downscale, which neither
			// works on block-compressed data nor on a format the device cannot filter.
			MipMaps = (flags & ETCF_CREATE_MIP_MAPS) != 0
				&& !IImage::isCompressedFormat(ColorFormat) && supportsLinearBlit();
			MipLevelCount = MipMaps ? computeMipLevels(Size.Width, Size.Height) : 1;

			// TRANSFER_SRC is unconditional: the blit chain reads level n-1, and lock()
			// copies the image back into its staging buffer.
			if (!createImage(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
				VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
				return;

			if (!createImageView())
				return;

			void* pixels = image->lock();
			if (pixels)
			{
				uploadImageData(pixels, image->getPitch(), image->getImageDataSizeInBytes(), expandR8G8B8);
				image->unlock();
			}

			Pitch = expandR8G8B8 ? (Size.Width * 4) : image->getPitch();

			if (MipLevelCount > 1)
				generateMips();

			createSampler(true, MipLevelCount > 1, 0, ETC_REPEAT);
		}

		CVulkanTexture::CVulkanTexture(const SVulkanContext& context, IVulkanUploadContext& upload,
			const core::dimension2d<u32>& size, ECOLOR_FORMAT format, bool renderTarget,
			const io::path& name, u32 arrayLayers, bool storage, u32 sampleCount)
			: ITexture(name), Context(context), Upload(upload)
		{
			DriverType = EDT_VULKAN;
			LayerCount = arrayLayers ? arrayLayers : 1;
			TextureType = (LayerCount > 1) ? ETT_2D_ARRAY : ETT_2D;
			Source = ETS_UNKNOWN;

			OriginalSize = Size = size;
			// ECF_UNKNOWN is what addRenderTargetTexture() passes when it does not care.
			ColorFormat = (format == ECF_UNKNOWN) ? ECF_A8R8G8B8 : format;
			Format = getVulkanFormat(ColorFormat);
			if (Format == VK_FORMAT_UNDEFINED)
				return;

			IsDepthStencil = isDepthFormat(Format);
			IsRenderTarget = renderTarget;
			HasAlpha = (ColorFormat == ECF_A8R8G8B8 || ColorFormat == ECF_A16B16G16R16F ||
				ColorFormat == ECF_A32B32G32R32F);
			Pitch = size.Width * (IImage::getBitsPerPixelFromFormat(ColorFormat) / 8);

			// A depth format can never be a color attachment, and only the combined
			// formats carry a stencil aspect.
			VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT |
				VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			if (IsDepthStencil)
			{
				usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
				Aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
				if (Format == VK_FORMAT_D24_UNORM_S8_UINT || Format == VK_FORMAT_D32_SFLOAT_S8_UINT)
					Aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
			}
			else
			{
				usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
				Aspect = VK_IMAGE_ASPECT_COLOR_BIT;
			}

			// STORAGE is what a compute shader's image store needs; the format has to support it in
			// optimal tiling, which every float and 8 bit UNORM colour format does on real hardware.
			if (storage && !IsDepthStencil)
			{
				VkFormatProperties properties = {};
				vk::GetPhysicalDeviceFormatProperties(Context.PhysicalDevice, Format, &properties);
				if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
				{
					os::Printer::log("CVulkanTexture: this format cannot be a storage image on this "
						"device", name, ELL_ERROR);
					return;
				}
				usage |= VK_IMAGE_USAGE_STORAGE_BIT;
				IsUnorderedAccess = true;
			}

			// The caller (addRenderTargetTexture) validated the count against the device limits.
			if (renderTarget && sampleCount > 1 && sampleCount <= 64 && (sampleCount & (sampleCount - 1)) == 0)
			{
				SampleCount = static_cast<VkSampleCountFlagBits>(sampleCount);
				// A depth target is sampled as a multisampled image (no averaging makes sense for
				// depth), a colour target keeps a single-sample image to resolve into.
				if (IsDepthStencil)
					ImageSamples = SampleCount;
			}

			if (!createImage(usage) || !createImageView())
				return;
			if (SampleCount > VK_SAMPLE_COUNT_1_BIT && !IsDepthStencil &&
				!createMultisampleImage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
				return;

			// A freshly created image is in VK_IMAGE_LAYOUT_UNDEFINED, which no read and
			// no attachment accepts; park it in the sampled layout so binding it as a
			// texture is valid before anything has been rendered into it.
			VkCommandBuffer commandBuffer = Upload.beginUpload();
			if (commandBuffer != VK_NULL_HANDLE)
			{
				transitionTo(commandBuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				Upload.endUploadAndWait(commandBuffer);
			}

			createSampler(true, false, 0, ETC_CLAMP_TO_EDGE);
		}

		// The slices are the ETT_2D textures CNullDriver::getTexture(files, type) loaded one by one;
		// each one's whole mip chain is copied into its layer, so the array carries the same mips
		// its slices did (all of them, or none when any slice lacks a chain).
		CVulkanTexture::CVulkanTexture(const SVulkanContext& context, IVulkanUploadContext& upload,
			const core::array<ITexture*>& slices, E_TEXTURE_TYPE type, const io::path& name)
			: ITexture(name), Context(context), Upload(upload)
		{
			DriverType = EDT_VULKAN;
			Source = ETS_UNKNOWN;
			TextureType = type;

			const u32 count = slices.size();
			if (count == 0)
			{
				os::Printer::log("CVulkanTexture: an array texture needs at least one slice", name, ELL_ERROR);
				return;
			}
			if (type == ETT_CUBE && count != 6)
			{
				os::Printer::log("CVulkanTexture: a cube map needs exactly 6 slices", name, ELL_ERROR);
				return;
			}
			if (type == ETT_CUBE_ARRAY && (count % 6) != 0)
			{
				os::Printer::log("CVulkanTexture: a cube array needs a multiple of 6 slices", name, ELL_ERROR);
				return;
			}
			if (type != ETT_CUBE && type != ETT_CUBE_ARRAY && type != ETT_2D_ARRAY)
			{
				os::Printer::log("CVulkanTexture: only ETT_2D_ARRAY, ETT_CUBE and ETT_CUBE_ARRAY can be "
					"built from slices", name, ELL_ERROR);
				return;
			}

			// Every slice has to be one of ours, of one size and one format: vkCmdCopyImage moves
			// texels without conversion.
			CVulkanTexture* first = nullptr;
			for (u32 i = 0; i < count; ++i)
			{
				ITexture* slice = slices[i];
				if (!slice || slice->getDriverType() != EDT_VULKAN ||
					!static_cast<CVulkanTexture*>(slice)->hasDeviceResource())
				{
					os::Printer::log("CVulkanTexture: array slice is not a usable Vulkan texture", name, ELL_ERROR);
					return;
				}
				CVulkanTexture* native = static_cast<CVulkanTexture*>(slice);
				if (!first)
					first = native;
				else if (native->getSize() != first->getSize() || native->getVkFormat() != first->getVkFormat())
				{
					os::Printer::log("CVulkanTexture: array slices differ in size or format", name, ELL_ERROR);
					return;
				}
			}

			LayerCount = count;
			OriginalSize = Size = first->getSize();
			ColorFormat = first->getColorFormat();
			Format = first->getVkFormat();
			Aspect = first->getAspectMask();
			HasAlpha = first->hasAlpha();
			Pitch = first->getPitch();
			IsDepthStencil = isDepthFormat(Format);

			// The chain is copied, not rebuilt, so the array has exactly the levels every slice has.
			MipLevelCount = first->getMipLevelCount();
			for (u32 i = 1; i < count; ++i)
				MipLevelCount = core::min_(MipLevelCount, static_cast<CVulkanTexture*>(slices[i])->getMipLevelCount());
			MipMaps = MipLevelCount > 1;

			if (!createImage(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
				VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || !createImageView())
				return;

			VkCommandBuffer commandBuffer = Upload.beginUpload();
			if (commandBuffer == VK_NULL_HANDLE)
				return;

			transitionImageLayout(commandBuffer, Image, VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, Aspect, MipLevelCount, LayerCount);

			std::vector<VkImageCopy> regions(MipLevelCount);
			for (u32 layer = 0; layer < count; ++layer)
			{
				CVulkanTexture* slice = static_cast<CVulkanTexture*>(slices[layer]);
				const VkImageLayout previous = slice->getImageLayout();
				slice->transitionTo(commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

				for (u32 level = 0; level < MipLevelCount; ++level)
				{
					u32 width = 0, height = 0;
					getMipDimension(Size, level, width, height);

					VkImageCopy& region = regions[level];
					region = VkImageCopy();
					region.srcSubresource.aspectMask = Aspect;
					region.srcSubresource.mipLevel = level;
					region.srcSubresource.baseArrayLayer = 0;
					region.srcSubresource.layerCount = 1;
					region.dstSubresource = region.srcSubresource;
					region.dstSubresource.baseArrayLayer = layer;
					region.extent.width = width;
					region.extent.height = height;
					region.extent.depth = 1;
				}
				vk::CmdCopyImage(commandBuffer, slice->getImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, MipLevelCount, regions.data());

				// The slice goes back to where it was; a never-written one lands in the sampled
				// layout, nothing may transition back to UNDEFINED.
				slice->transitionTo(commandBuffer, (previous == VK_IMAGE_LAYOUT_UNDEFINED) ?
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : previous);
			}

			transitionImageLayout(commandBuffer, Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, Aspect, MipLevelCount, LayerCount);
			CurrentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			Upload.endUploadAndWait(commandBuffer);

			createSampler(true, MipLevelCount > 1, 0, (type == ETT_2D_ARRAY) ? ETC_REPEAT : ETC_CLAMP_TO_EDGE);
		}

		CVulkanTexture::~CVulkanTexture()
		{
			if (Context.Device == VK_NULL_HANDLE)
				return;

			// Every copy this class records is submitted with endUploadAndWait(), but a
			// draw still in flight may reference the image or its sampler.
			vk::DeviceWaitIdle(Context.Device);

			if (MappedData)
			{
				vk::UnmapMemory(Context.Device, StagingMemory);
				MappedData = nullptr;
			}
			destroyStagingBuffer();

			// Views before image, and memory last: the image still owns its binding until
			// vkDestroyImage returns.
			for (size_t i = 0; i < LayerViews.size(); ++i)
				if (LayerViews[i] != VK_NULL_HANDLE && LayerViews[i] != View)
					vk::DestroyImageView(Context.Device, LayerViews[i], nullptr);
			LayerViews.clear();
			if (View != VK_NULL_HANDLE)
				vk::DestroyImageView(Context.Device, View, nullptr);
			if (Sampler != VK_NULL_HANDLE)
				vk::DestroySampler(Context.Device, Sampler, nullptr);
			if (Image != VK_NULL_HANDLE)
				vk::DestroyImage(Context.Device, Image, nullptr);
			if (Memory != VK_NULL_HANDLE)
				vk::FreeMemory(Context.Device, Memory, nullptr);
			if (MsaaView != VK_NULL_HANDLE)
				vk::DestroyImageView(Context.Device, MsaaView, nullptr);
			if (MsaaImage != VK_NULL_HANDLE)
				vk::DestroyImage(Context.Device, MsaaImage, nullptr);
			if (MsaaMemory != VK_NULL_HANDLE)
				vk::FreeMemory(Context.Device, MsaaMemory, nullptr);

			View = VK_NULL_HANDLE;
			Sampler = VK_NULL_HANDLE;
			Image = VK_NULL_HANDLE;
			Memory = VK_NULL_HANDLE;
		}

		bool CVulkanTexture::createImage(VkImageUsageFlags usage)
		{
			VkImageCreateInfo imageInfo = {};
			imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
			imageInfo.imageType = (TextureType == ETT_3D) ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
			imageInfo.format = Format;
			imageInfo.extent.width = Size.Width;
			imageInfo.extent.height = Size.Height;
			imageInfo.extent.depth = Depth;
			imageInfo.mipLevels = MipLevelCount;
			imageInfo.arrayLayers = LayerCount;
			imageInfo.samples = ImageSamples;
			imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
			imageInfo.usage = usage;
			imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			// A cube view can only be created over an image that was declared cube compatible.
			if (TextureType == ETT_CUBE || TextureType == ETT_CUBE_ARRAY)
				imageInfo.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

			if (vulkanFailed("vkCreateImage", vk::CreateImage(Context.Device, &imageInfo, nullptr, &Image)))
			{
				Image = VK_NULL_HANDLE;
				return false;
			}
			CurrentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

			VkMemoryRequirements requirements = {};
			vk::GetImageMemoryRequirements(Context.Device, Image, &requirements);

			VkMemoryAllocateInfo allocInfo = {};
			allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			allocInfo.allocationSize = requirements.size;
			allocInfo.memoryTypeIndex = findMemoryTypeIndex(Context.MemoryProperties,
				requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			if (allocInfo.memoryTypeIndex == 0xffffffffu)
			{
				os::Printer::log("CVulkanTexture: no device local memory type for image", ELL_ERROR);
				vk::DestroyImage(Context.Device, Image, nullptr);
				Image = VK_NULL_HANDLE;
				return false;
			}

			if (vulkanFailed("vkAllocateMemory", vk::AllocateMemory(Context.Device, &allocInfo, nullptr, &Memory)) ||
				vulkanFailed("vkBindImageMemory", vk::BindImageMemory(Context.Device, Image, Memory, 0)))
			{
				if (Memory != VK_NULL_HANDLE)
					vk::FreeMemory(Context.Device, Memory, nullptr);
				vk::DestroyImage(Context.Device, Image, nullptr);
				Memory = VK_NULL_HANDLE;
				Image = VK_NULL_HANDLE;
				return false;
			}
			return true;
		}

		bool CVulkanTexture::createImageView()
		{
			VkImageViewCreateInfo viewInfo = {};
			viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			viewInfo.image = Image;
			switch (TextureType)
			{
			case ETT_CUBE:       viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE; break;
			case ETT_CUBE_ARRAY: viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; break;
			case ETT_2D_ARRAY:   viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
			case ETT_3D:         viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D; break;
			default:             viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D; break;
			}
			viewInfo.format = Format;
			// A stencil aspect is never sampled here, so a combined depth/stencil view
			// exposes depth only - the shader reads a single channel.
			viewInfo.subresourceRange.aspectMask = (Aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ?
				VK_IMAGE_ASPECT_DEPTH_BIT : Aspect;
			viewInfo.subresourceRange.levelCount = MipLevelCount;
			viewInfo.subresourceRange.layerCount = LayerCount;

			if (vulkanFailed("vkCreateImageView", vk::CreateImageView(Context.Device, &viewInfo, nullptr, &View)))
			{
				View = VK_NULL_HANDLE;
				return false;
			}
			return true;
		}

		bool CVulkanTexture::createMultisampleImage(VkImageUsageFlags usage)
		{
			VkImageCreateInfo imageInfo = {};
			imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
			imageInfo.imageType = VK_IMAGE_TYPE_2D;
			imageInfo.format = Format;
			imageInfo.extent.width = Size.Width;
			imageInfo.extent.height = Size.Height;
			imageInfo.extent.depth = 1;
			imageInfo.mipLevels = 1; // a multisampled image has no mip chain
			imageInfo.arrayLayers = LayerCount;
			imageInfo.samples = SampleCount;
			imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
			imageInfo.usage = usage;
			imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

			if (vulkanFailed("vkCreateImage (multisample)", vk::CreateImage(Context.Device, &imageInfo, nullptr, &MsaaImage)))
			{
				MsaaImage = VK_NULL_HANDLE;
				return false;
			}
			MsaaLayout = VK_IMAGE_LAYOUT_UNDEFINED;

			VkMemoryRequirements requirements = {};
			vk::GetImageMemoryRequirements(Context.Device, MsaaImage, &requirements);

			VkMemoryAllocateInfo allocInfo = {};
			allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			allocInfo.allocationSize = requirements.size;
			allocInfo.memoryTypeIndex = findMemoryTypeIndex(Context.MemoryProperties,
				requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			if (allocInfo.memoryTypeIndex == 0xffffffffu ||
				vulkanFailed("vkAllocateMemory (multisample)", vk::AllocateMemory(Context.Device, &allocInfo, nullptr, &MsaaMemory)) ||
				vulkanFailed("vkBindImageMemory (multisample)", vk::BindImageMemory(Context.Device, MsaaImage, MsaaMemory, 0)))
			{
				if (MsaaMemory != VK_NULL_HANDLE)
					vk::FreeMemory(Context.Device, MsaaMemory, nullptr);
				vk::DestroyImage(Context.Device, MsaaImage, nullptr);
				MsaaMemory = VK_NULL_HANDLE;
				MsaaImage = VK_NULL_HANDLE;
				return false;
			}

			VkImageViewCreateInfo viewInfo = {};
			viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			viewInfo.image = MsaaImage;
			viewInfo.viewType = (LayerCount > 1) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
			viewInfo.format = Format;
			viewInfo.subresourceRange.aspectMask = Aspect;
			viewInfo.subresourceRange.levelCount = 1;
			viewInfo.subresourceRange.layerCount = LayerCount;
			if (vulkanFailed("vkCreateImageView (multisample)", vk::CreateImageView(Context.Device, &viewInfo, nullptr, &MsaaView)))
			{
				MsaaView = VK_NULL_HANDLE;
				return false;
			}
			return true;
		}

		void CVulkanTexture::transitionMultisampleTo(VkCommandBuffer commandBuffer, VkImageLayout newLayout)
		{
			if (MsaaImage == VK_NULL_HANDLE || newLayout == MsaaLayout)
				return;
			transitionImageLayout(commandBuffer, MsaaImage, MsaaLayout, newLayout, Aspect, 1, LayerCount);
			MsaaLayout = newLayout;
		}

		VkImageView CVulkanTexture::getLayerView(u32 layer)
		{
			if (Image == VK_NULL_HANDLE || layer >= LayerCount)
				return VK_NULL_HANDLE;
			if (LayerCount == 1)
				return View;

			if (LayerViews.size() < LayerCount)
				LayerViews.resize(LayerCount, VK_NULL_HANDLE);
			if (LayerViews[layer] != VK_NULL_HANDLE)
				return LayerViews[layer];

			// A single-layer 2D view: what a colour attachment or a per-slice copy wants. The
			// depth-only aspect rule of createImageView() applies to a sampled view; an attachment
			// view over a combined format must name both aspects.
			VkImageViewCreateInfo viewInfo = {};
			viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			viewInfo.image = Image;
			viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			viewInfo.format = Format;
			viewInfo.subresourceRange.aspectMask = Aspect;
			viewInfo.subresourceRange.levelCount = MipLevelCount;
			viewInfo.subresourceRange.baseArrayLayer = layer;
			viewInfo.subresourceRange.layerCount = 1;

			VkImageView view = VK_NULL_HANDLE;
			if (vulkanFailed("vkCreateImageView (layer)", vk::CreateImageView(Context.Device, &viewInfo, nullptr, &view)))
				return VK_NULL_HANDLE;
			LayerViews[layer] = view;
			return view;
		}

		bool CVulkanTexture::createSampler(bool bilinear, bool trilinear, u8 anisotropic, E_TEXTURE_CLAMP wrap)
		{
			if (Sampler != VK_NULL_HANDLE && bilinear == SamplerBilinear && trilinear == SamplerTrilinear &&
				anisotropic == SamplerAnisotropic && wrap == SamplerWrap)
				return true;

			const VkSamplerAddressMode address = getAddressMode(wrap);
			const bool linear = bilinear || trilinear;

			VkSamplerCreateInfo samplerInfo = {};
			samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
			samplerInfo.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
			samplerInfo.minFilter = samplerInfo.magFilter;
			samplerInfo.mipmapMode = trilinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
			samplerInfo.addressModeU = address;
			samplerInfo.addressModeV = address;
			samplerInfo.addressModeW = address;
			samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
			samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
			samplerInfo.maxAnisotropy = 1.0f;
			samplerInfo.maxLod = static_cast<f32>(MipLevelCount);

			// samplerAnisotropy is an optional device feature; maxSamplerAnisotropy stays
			// at 1.0 when it was not enabled, which also caps the requested level.
			const f32 maxSupported = Context.DeviceProperties.limits.maxSamplerAnisotropy;
			if (anisotropic > 1 && maxSupported > 1.0f)
			{
				samplerInfo.anisotropyEnable = VK_TRUE;
				samplerInfo.maxAnisotropy = core::min_(static_cast<f32>(anisotropic), maxSupported);
			}

			VkSampler sampler = VK_NULL_HANDLE;
			if (vulkanFailed("vkCreateSampler", vk::CreateSampler(Context.Device, &samplerInfo, nullptr, &sampler)))
				return false;

			// The previous sampler may still be referenced by a descriptor set in flight;
			// callers change sampler settings between frames, not inside one.
			if (Sampler != VK_NULL_HANDLE)
				vk::DestroySampler(Context.Device, Sampler, nullptr);

			Sampler = sampler;
			SamplerBilinear = bilinear;
			SamplerTrilinear = trilinear;
			SamplerAnisotropic = anisotropic;
			SamplerWrap = wrap;
			return true;
		}

		bool CVulkanTexture::uploadImageData(const void* data, u32 sourcePitchBytes, u32 dataSizeBytes,
			bool expandR8G8B8)
		{
			const VkDeviceSize uploadSize = expandR8G8B8 ?
				static_cast<VkDeviceSize>(Size.Width) * Size.Height * 4 : dataSizeBytes;
			if (!data || !uploadSize)
				return false;

			VkBuffer staging = VK_NULL_HANDLE;
			VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
			if (!createVulkanBuffer(Context, uploadSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				staging, stagingMemory))
				return false;

			void* mapped = nullptr;
			if (vulkanFailed("vkMapMemory", vk::MapMemory(Context.Device, stagingMemory, 0, uploadSize, 0, &mapped)))
			{
				vk::DestroyBuffer(Context.Device, staging, nullptr);
				vk::FreeMemory(Context.Device, stagingMemory, nullptr);
				return false;
			}

			if (expandR8G8B8)
			{
				// 24 -> 32 bit widening, alpha forced opaque. Byte order is preserved, so
				// the R8G8B8A8 image ends up with the same channel order as the source.
				const u8* source = static_cast<const u8*>(data);
				u8* dest = static_cast<u8*>(mapped);
				for (u32 y = 0; y < Size.Height; ++y)
				{
					const u8* sourceRow = source + static_cast<size_t>(y) * sourcePitchBytes;
					for (u32 x = 0; x < Size.Width; ++x)
					{
						*dest++ = sourceRow[x * 3 + 0];
						*dest++ = sourceRow[x * 3 + 1];
						*dest++ = sourceRow[x * 3 + 2];
						*dest++ = 0xff;
					}
				}
			}
			else
			{
				memcpy(mapped, data, static_cast<size_t>(uploadSize));
			}
			vk::UnmapMemory(Context.Device, stagingMemory);

			VkCommandBuffer commandBuffer = Upload.beginUpload();
			if (commandBuffer == VK_NULL_HANDLE)
			{
				vk::DestroyBuffer(Context.Device, staging, nullptr);
				vk::FreeMemory(Context.Device, stagingMemory, nullptr);
				return false;
			}

			// Every level goes to TRANSFER_DST although only level 0 is written here: the
			// chain must leave this function in one uniform layout, and generateMips()
			// re-transitions it anyway.
			transitionImageLayout(commandBuffer, Image, VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, Aspect, MipLevelCount, LayerCount);

			VkBufferImageCopy region = {};
			region.imageSubresource.aspectMask = Aspect;
			region.imageSubresource.layerCount = 1;
			region.imageExtent.width = Size.Width;
			region.imageExtent.height = Size.Height;
			region.imageExtent.depth = 1;
			vk::CmdCopyBufferToImage(commandBuffer, staging, Image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

			transitionImageLayout(commandBuffer, Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, Aspect, MipLevelCount, LayerCount);
			CurrentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			Upload.endUploadAndWait(commandBuffer);

			// endUploadAndWait() blocked until the copy completed, so the staging buffer
			// is free immediately.
			vk::DestroyBuffer(Context.Device, staging, nullptr);
			vk::FreeMemory(Context.Device, stagingMemory, nullptr);
			return true;
		}

		namespace
		{
			// The on-disk DDS layout: magic, 124-byte header, optional 20-byte DX10 header, then the
			// surfaces -- for each array slice / cube face, every mip level from the largest down
			// (a volume level is its depth slices back to back), tightly packed.
			enum
			{
				DdsMagic = 0x20534444u, // "DDS "
				DdsHeaderSize = 124,
				DdsFlagMipMapCount = 0x20000,
				DdsFlagDepth = 0x800000,
				DdsPixelFormatFourCC = 0x4,
				DdsCaps2Cubemap = 0x200,
				DdsCaps2CubemapAllFaces = 0xFC00,
				DdsCaps2Volume = 0x200000,
				DdsDx10MiscTextureCube = 0x4,
				DdsDx10DimensionTexture3D = 4,
				FourCCDx10 = 0x30315844u // "DX10"
			};

#pragma pack(push, 1)
			struct SDdsPixelFormat
			{
				u32 Size, Flags, FourCC, RGBBitCount, RBitMask, GBitMask, BBitMask, ABitMask;
			};

			struct SDdsHeader
			{
				u32 Magic;
				u32 Size, Flags, Height, Width, PitchOrLinearSize, Depth, MipMapCount;
				u32 Reserved1[11];
				SDdsPixelFormat PixelFormat;
				u32 Caps, Caps2, Caps3, Caps4, Reserved2;
			};

			struct SDdsHeaderDx10
			{
				u32 DxgiFormat, ResourceDimension, MiscFlag, ArraySize, MiscFlags2;
			};
#pragma pack(pop)

			inline u32 ddsMipExtent(u32 base, u32 level)
			{
				const u32 e = base >> level;
				return e ? e : 1;
			}
		}

		bool CVulkanTexture::uploadDdsFile(const u8* bytes, u32 byteCount)
		{
			if (!bytes || byteCount < sizeof(SDdsHeader))
			{
				os::Printer::log("CVulkanTexture: .dds data too short for a header", ELL_ERROR);
				return false;
			}

			SDdsHeader header;
			memcpy(&header, bytes, sizeof(header));
			if (header.Magic != DdsMagic || header.Size != DdsHeaderSize)
			{
				os::Printer::log("CVulkanTexture: not a .dds header", ELL_ERROR);
				return false;
			}

			u32 dataOffset = sizeof(SDdsHeader);
			u32 arraySize = 1;
			bool cube = (header.Caps2 & DdsCaps2Cubemap) != 0;
			bool volume = ((header.Flags & DdsFlagDepth) && header.Depth > 1) || (header.Caps2 & DdsCaps2Volume) != 0;
			if ((header.PixelFormat.Flags & DdsPixelFormatFourCC) && header.PixelFormat.FourCC == FourCCDx10)
			{
				if (byteCount < dataOffset + sizeof(SDdsHeaderDx10))
				{
					os::Printer::log("CVulkanTexture: .dds data too short for its DX10 header", ELL_ERROR);
					return false;
				}
				SDdsHeaderDx10 dx10;
				memcpy(&dx10, bytes + dataOffset, sizeof(dx10));
				dataOffset += sizeof(SDdsHeaderDx10);
				if (dx10.ArraySize > 1)
					arraySize = dx10.ArraySize;
				if (dx10.MiscFlag & DdsDx10MiscTextureCube)
					cube = true;
				if (dx10.ResourceDimension == DdsDx10DimensionTexture3D)
					volume = true;
			}
			if (cube && (header.Caps2 & DdsCaps2CubemapAllFaces) != DdsCaps2CubemapAllFaces &&
				!(header.PixelFormat.FourCC == FourCCDx10))
			{
				os::Printer::log("CVulkanTexture: partial cube maps (fewer than 6 faces) are not supported", ELL_ERROR);
				return false;
			}
			if (volume && (cube || arraySize > 1))
			{
				os::Printer::log("CVulkanTexture: a volume .dds cannot also be a cube map or an array", ELL_ERROR);
				return false;
			}

			// The loader already decided ColorFormat from the pixel format / DXGI format; what the
			// header adds is the geometry: dimensions, depth, mip chain, faces and slices.
			Size.Width = header.Width;
			Size.Height = header.Height;
			OriginalSize = Size;
			Depth = (volume && header.Depth) ? header.Depth : 1;
			const u32 faces = cube ? 6 : 1;
			LayerCount = faces * arraySize;
			MipLevelCount = (header.Flags & DdsFlagMipMapCount) && header.MipMapCount > 1 ? header.MipMapCount : 1;
			MipMaps = MipLevelCount > 1;
			if (volume)
				TextureType = ETT_3D;
			else if (cube)
				TextureType = (arraySize > 1) ? ETT_CUBE_ARRAY : ETT_CUBE;
			else if (LayerCount > 1)
				TextureType = ETT_2D_ARRAY;
			HasAlpha = IImage::hasAlphaFormat(ColorFormat);

			// The device has to sample this format at all; BC support is optional in Vulkan.
			VkFormatProperties properties = {};
			vk::GetPhysicalDeviceFormatProperties(Context.PhysicalDevice, Format, &properties);
			if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
			{
				os::Printer::log("CVulkanTexture: the device cannot sample this .dds format", ELL_ERROR);
				return false;
			}

			// Every surface, in file order, so the copy regions can be laid out in one pass. A
			// volume level is one region whose depth covers its slices, stored back to back.
			std::vector<VkBufferImageCopy> regions;
			regions.reserve(LayerCount * MipLevelCount);
			u32 offset = dataOffset;
			for (u32 layer = 0; layer < LayerCount; ++layer)
			{
				for (u32 level = 0; level < MipLevelCount; ++level)
				{
					const u32 width = ddsMipExtent(Size.Width, level);
					const u32 height = ddsMipExtent(Size.Height, level);
					const u32 depth = ddsMipExtent(Depth, level);
					const u32 levelBytes = IImage::getSurfaceSizeInBytes(ColorFormat, width, height) * depth;
					if (levelBytes == 0 || offset + levelBytes > byteCount)
					{
						os::Printer::log("CVulkanTexture: .dds file is shorter than its header claims", ELL_ERROR);
						return false;
					}

					VkBufferImageCopy region = {};
					region.bufferOffset = offset - dataOffset;
					region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
					region.imageSubresource.mipLevel = level;
					region.imageSubresource.baseArrayLayer = layer;
					region.imageSubresource.layerCount = 1;
					region.imageExtent.width = width;
					region.imageExtent.height = height;
					region.imageExtent.depth = depth;
					regions.push_back(region);

					offset += levelBytes;
				}
			}
			const VkDeviceSize dataBytes = offset - dataOffset;
			Pitch = IImage::isCompressedFormat(ColorFormat) ?
				((Size.Width + 3) / 4) * IImage::getBlockBytes(ColorFormat) :
				Size.Width * (IImage::getBitsPerPixelFromFormat(ColorFormat) / 8);

			if (!createImage(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
				VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || !createImageView())
				return false;

			VkBuffer staging = VK_NULL_HANDLE;
			VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
			if (!createVulkanBuffer(Context, dataBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				staging, stagingMemory))
				return false;

			void* mapped = nullptr;
			if (vulkanFailed("vkMapMemory", vk::MapMemory(Context.Device, stagingMemory, 0, dataBytes, 0, &mapped)))
			{
				vk::DestroyBuffer(Context.Device, staging, nullptr);
				vk::FreeMemory(Context.Device, stagingMemory, nullptr);
				return false;
			}
			memcpy(mapped, bytes + dataOffset, static_cast<size_t>(dataBytes));
			vk::UnmapMemory(Context.Device, stagingMemory);

			VkCommandBuffer commandBuffer = Upload.beginUpload();
			if (commandBuffer == VK_NULL_HANDLE)
			{
				vk::DestroyBuffer(Context.Device, staging, nullptr);
				vk::FreeMemory(Context.Device, stagingMemory, nullptr);
				return false;
			}

			transitionImageLayout(commandBuffer, Image, VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, Aspect, MipLevelCount, LayerCount);
			vk::CmdCopyBufferToImage(commandBuffer, staging, Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				static_cast<u32>(regions.size()), regions.data());
			transitionImageLayout(commandBuffer, Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, Aspect, MipLevelCount, LayerCount);
			CurrentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			Upload.endUploadAndWait(commandBuffer);

			vk::DestroyBuffer(Context.Device, staging, nullptr);
			vk::FreeMemory(Context.Device, stagingMemory, nullptr);
			return true;
		}

		void CVulkanTexture::transitionTo(VkCommandBuffer commandBuffer, VkImageLayout newLayout)
		{
			if (Image == VK_NULL_HANDLE || newLayout == CurrentLayout)
				return;

			transitionImageLayout(commandBuffer, Image, CurrentLayout, newLayout, Aspect, MipLevelCount, LayerCount);
			CurrentLayout = newLayout;
		}

		u32 CVulkanTexture::getBytesPerPixel() const
		{
			if (IImage::isCompressedFormat(ColorFormat))
				return 0;
			return IImage::getBitsPerPixelFromFormat(ColorFormat) / 8;
		}

		void CVulkanTexture::destroyStagingBuffer()
		{
			if (StagingBuffer != VK_NULL_HANDLE)
				vk::DestroyBuffer(Context.Device, StagingBuffer, nullptr);
			if (StagingMemory != VK_NULL_HANDLE)
				vk::FreeMemory(Context.Device, StagingMemory, nullptr);
			StagingBuffer = VK_NULL_HANDLE;
			StagingMemory = VK_NULL_HANDLE;
			StagingSize = 0;
		}

		void* CVulkanTexture::lock(E_TEXTURE_LOCK_MODE mode, u32 mipmapLevel)
		{
			if (Image == VK_NULL_HANDLE)
			{
				os::Printer::log("CVulkanTexture::lock: texture without GPU image",
					getName().getPath(), ELL_ERROR);
				return nullptr;
			}
			if (mipmapLevel >= MipLevelCount)
			{
				os::Printer::log("CVulkanTexture::lock: mipmapLevel out of range", ELL_WARNING);
				return nullptr;
			}

			// A block-compressed level has no linear CPU layout to hand out, and the
			// engine's pixel helpers cannot address it either.
			const u32 bytesPerPixel = getBytesPerPixel();
			if (!bytesPerPixel)
			{
				os::Printer::log("CVulkanTexture::lock: compressed format cannot be locked", ELL_WARNING);
				return nullptr;
			}

			if (MappedData)
				unlock(); // a previous lock was never released

			u32 levelWidth = 0;
			u32 levelHeight = 0;
			getMipDimension(Size, mipmapLevel, levelWidth, levelHeight);
			const VkDeviceSize rowPitch = static_cast<VkDeviceSize>(levelWidth) * bytesPerPixel;
			StagingSize = rowPitch * levelHeight;

			// TRANSFER_DST as well as SRC: the same buffer receives the readback and
			// feeds unlock()'s upload, unlike a D3D12 readback heap.
			if (!createVulkanBuffer(Context, StagingSize,
				VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
				StagingBuffer, StagingMemory))
				return nullptr;

			LastLockMode = mode;
			LastLockMipLevel = mipmapLevel;
			// Pitch has to describe the level actually handed out, not level 0.
			Pitch = static_cast<u32>(rowPitch);

			if (mode != ETLM_WRITE_ONLY)
			{
				VkCommandBuffer commandBuffer = Upload.beginUpload();
				if (commandBuffer == VK_NULL_HANDLE)
				{
					destroyStagingBuffer();
					return nullptr;
				}

				const VkImageLayout previousLayout = CurrentLayout;
				transitionTo(commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

				VkBufferImageCopy region = {};
				region.imageSubresource.aspectMask = Aspect;
				region.imageSubresource.mipLevel = mipmapLevel;
				region.imageSubresource.layerCount = 1;
				region.imageExtent.width = levelWidth;
				region.imageExtent.height = levelHeight;
				region.imageExtent.depth = 1;
				vk::CmdCopyImageToBuffer(commandBuffer, Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					StagingBuffer, 1, &region);

				// Nothing may transition back to UNDEFINED, so an image that was never
				// written lands in the sampled layout instead.
				transitionTo(commandBuffer, (previousLayout == VK_IMAGE_LAYOUT_UNDEFINED) ?
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : previousLayout);
				Upload.endUploadAndWait(commandBuffer);
			}

			if (vulkanFailed("vkMapMemory", vk::MapMemory(Context.Device, StagingMemory, 0, StagingSize, 0, &MappedData)))
			{
				MappedData = nullptr;
				destroyStagingBuffer();
				return nullptr;
			}
			return MappedData;
		}

		void CVulkanTexture::unlock()
		{
			if (!MappedData)
				return;

			vk::UnmapMemory(Context.Device, StagingMemory);
			MappedData = nullptr;

			if (LastLockMode != ETLM_READ_ONLY)
			{
				VkCommandBuffer commandBuffer = Upload.beginUpload();
				if (commandBuffer != VK_NULL_HANDLE)
				{
					u32 levelWidth = 0;
					u32 levelHeight = 0;
					getMipDimension(Size, LastLockMipLevel, levelWidth, levelHeight);

					transitionTo(commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

					VkBufferImageCopy region = {};
					region.imageSubresource.aspectMask = Aspect;
					region.imageSubresource.mipLevel = LastLockMipLevel;
					region.imageSubresource.layerCount = 1;
					region.imageExtent.width = levelWidth;
					region.imageExtent.height = levelHeight;
					region.imageExtent.depth = 1;
					vk::CmdCopyBufferToImage(commandBuffer, StagingBuffer, Image,
						VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

					transitionTo(commandBuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
					Upload.endUploadAndWait(commandBuffer);
				}
			}

			destroyStagingBuffer();
		}

		u32 CVulkanTexture::computeMipLevels(u32 width, u32 height)
		{
			u32 levels = 1;
			u32 w = width;
			u32 h = height;
			while (w > 1 || h > 1)
			{
				w = (w > 1) ? (w >> 1) : 1;
				h = (h > 1) ? (h >> 1) : 1;
				++levels;
			}
			return levels;
		}

		bool CVulkanTexture::supportsLinearBlit() const
		{
			if (Format == VK_FORMAT_UNDEFINED || Context.PhysicalDevice == VK_NULL_HANDLE)
				return false;

			VkFormatProperties properties = {};
			vk::GetPhysicalDeviceFormatProperties(Context.PhysicalDevice, Format, &properties);
			return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
		}

		void CVulkanTexture::regenerateMipMapLevels(void* mipmapData)
		{
			// mipmapData is ignored: the chain is always rebuilt from the image's own
			// level 0, never from CPU data.
			if (MipLevelCount <= 1)
			{
				os::Printer::log("CVulkanTexture::regenerateMipMapLevels: texture has no mip chain "
					"(ETCF_CREATE_MIP_MAPS was not set at creation)", ELL_WARNING);
				return;
			}
			generateMips();
		}

		void CVulkanTexture::generateMips()
		{
			if (MipLevelCount <= 1 || Image == VK_NULL_HANDLE)
				return;

			if (!supportsLinearBlit())
			{
				os::Printer::log("CVulkanTexture: format cannot be linearly filtered, "
					"mip levels left undefined", ELL_WARNING);
				return;
			}

			VkCommandBuffer commandBuffer = Upload.beginUpload();
			if (commandBuffer == VK_NULL_HANDLE)
				return;

			transitionTo(commandBuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

			// Every layer at once: the barriers and blits below name all of them, so an array
			// texture gets its chain rebuilt layer by layer in the same pass.
			VkImageMemoryBarrier barrier = {};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = Image;
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barrier.subresourceRange.aspectMask = Aspect;
			barrier.subresourceRange.levelCount = 1;
			barrier.subresourceRange.layerCount = LayerCount;

			s32 levelWidth = static_cast<s32>(Size.Width);
			s32 levelHeight = static_cast<s32>(Size.Height);

			for (u32 level = 1; level < MipLevelCount; ++level)
			{
				// The source level must be readable before it feeds the blit; every other
				// level stays a copy destination.
				barrier.subresourceRange.baseMipLevel = level - 1;
				vk::CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
					VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

				const s32 nextWidth = (levelWidth > 1) ? (levelWidth >> 1) : 1;
				const s32 nextHeight = (levelHeight > 1) ? (levelHeight >> 1) : 1;

				VkImageBlit blit = {};
				blit.srcSubresource.aspectMask = Aspect;
				blit.srcSubresource.mipLevel = level - 1;
				blit.srcSubresource.layerCount = LayerCount;
				blit.srcOffsets[1].x = levelWidth;
				blit.srcOffsets[1].y = levelHeight;
				blit.srcOffsets[1].z = 1;
				blit.dstSubresource = blit.srcSubresource;
				blit.dstSubresource.mipLevel = level;
				blit.dstOffsets[1].x = nextWidth;
				blit.dstOffsets[1].y = nextHeight;
				blit.dstOffsets[1].z = 1;
				vk::CmdBlitImage(commandBuffer, Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

				levelWidth = nextWidth;
				levelHeight = nextHeight;
			}

			// The last level was never a blit source, so it still has to join the others
			// before the whole chain moves to the sampled layout in one barrier.
			barrier.subresourceRange.baseMipLevel = MipLevelCount - 1;
			vk::CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

			transitionImageLayout(commandBuffer, Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, Aspect, MipLevelCount, LayerCount);
			CurrentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			Upload.endUploadAndWait(commandBuffer);
			MipMaps = true;
		}

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
