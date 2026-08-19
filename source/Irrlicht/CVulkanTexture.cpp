// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "CVulkanTexture.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_
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
			// layout, there is no dedicated VkFormat for them. BC4/BC5 have no
			// ECOLOR_FORMAT value to map from.
			case ECF_DXT1:          return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
			case ECF_DXT2:
			case ECF_DXT3:          return VK_FORMAT_BC2_UNORM_BLOCK;
			case ECF_DXT4:
			case ECF_DXT5:          return VK_FORMAT_BC3_UNORM_BLOCK;
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

			HasAlpha = (sourceFormat == ECF_A8R8G8B8 || sourceFormat == ECF_A1R5G5B5 ||
				sourceFormat == ECF_A16B16G16R16F || sourceFormat == ECF_A32B32G32R32F ||
				sourceFormat == ECF_DXT3 || sourceFormat == ECF_DXT5);

			const bool expandR8G8B8 = (sourceFormat == ECF_R8G8B8);
			ColorFormat = expandR8G8B8 ? ECF_A8R8G8B8 : sourceFormat;
			Aspect = VK_IMAGE_ASPECT_COLOR_BIT;

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
			const io::path& name)
			: ITexture(name), Context(context), Upload(upload)
		{
			DriverType = EDT_VULKAN;
			TextureType = ETT_2D;
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

			if (!createImage(usage) || !createImageView())
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

			// View before image, and memory last: the image still owns its binding until
			// vkDestroyImage returns.
			if (View != VK_NULL_HANDLE)
				vk::DestroyImageView(Context.Device, View, nullptr);
			if (Sampler != VK_NULL_HANDLE)
				vk::DestroySampler(Context.Device, Sampler, nullptr);
			if (Image != VK_NULL_HANDLE)
				vk::DestroyImage(Context.Device, Image, nullptr);
			if (Memory != VK_NULL_HANDLE)
				vk::FreeMemory(Context.Device, Memory, nullptr);

			View = VK_NULL_HANDLE;
			Sampler = VK_NULL_HANDLE;
			Image = VK_NULL_HANDLE;
			Memory = VK_NULL_HANDLE;
		}

		bool CVulkanTexture::createImage(VkImageUsageFlags usage)
		{
			VkImageCreateInfo imageInfo = {};
			imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
			imageInfo.imageType = VK_IMAGE_TYPE_2D;
			imageInfo.format = Format;
			imageInfo.extent.width = Size.Width;
			imageInfo.extent.height = Size.Height;
			imageInfo.extent.depth = 1;
			imageInfo.mipLevels = MipLevelCount;
			imageInfo.arrayLayers = 1;
			imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
			imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
			imageInfo.usage = usage;
			imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

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
			viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			viewInfo.format = Format;
			// A stencil aspect is never sampled here, so a combined depth/stencil view
			// exposes depth only - the shader reads a single channel.
			viewInfo.subresourceRange.aspectMask = (Aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ?
				VK_IMAGE_ASPECT_DEPTH_BIT : Aspect;
			viewInfo.subresourceRange.levelCount = MipLevelCount;
			viewInfo.subresourceRange.layerCount = 1;

			if (vulkanFailed("vkCreateImageView", vk::CreateImageView(Context.Device, &viewInfo, nullptr, &View)))
			{
				View = VK_NULL_HANDLE;
				return false;
			}
			return true;
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
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, Aspect, MipLevelCount, 1);

			VkBufferImageCopy region = {};
			region.imageSubresource.aspectMask = Aspect;
			region.imageSubresource.layerCount = 1;
			region.imageExtent.width = Size.Width;
			region.imageExtent.height = Size.Height;
			region.imageExtent.depth = 1;
			vk::CmdCopyBufferToImage(commandBuffer, staging, Image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

			transitionImageLayout(commandBuffer, Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, Aspect, MipLevelCount, 1);
			CurrentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			Upload.endUploadAndWait(commandBuffer);

			// endUploadAndWait() blocked until the copy completed, so the staging buffer
			// is free immediately.
			vk::DestroyBuffer(Context.Device, staging, nullptr);
			vk::FreeMemory(Context.Device, stagingMemory, nullptr);
			return true;
		}

		void CVulkanTexture::transitionTo(VkCommandBuffer commandBuffer, VkImageLayout newLayout)
		{
			if (Image == VK_NULL_HANDLE || newLayout == CurrentLayout)
				return;

			transitionImageLayout(commandBuffer, Image, CurrentLayout, newLayout, Aspect, MipLevelCount, 1);
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
			barrier.subresourceRange.layerCount = 1;

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
				blit.srcSubresource.layerCount = 1;
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
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, Aspect, MipLevelCount, 1);
			CurrentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			Upload.endUploadAndWait(commandBuffer);
			MipMaps = true;
		}

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
