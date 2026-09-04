// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "CVulkanRenderTarget.h"

#ifdef _IRR_COMPILE_WITH_VULKAN_

#include "CVulkanTexture.h"
#include "os.h"

namespace irr
{
	namespace video
	{
		// ============================== CVulkanDepthBufferPool ==============================

		CVulkanDepthBufferPool::CVulkanDepthBufferPool(const SVulkanContext& context,
			VkFormat preferredFormat)
			: Context(context)
		{
			DepthFormat = pickDepthFormat(preferredFormat);
			if (DepthFormat == VK_FORMAT_UNDEFINED)
				os::Printer::log("CVulkanDepthBufferPool: no usable depth format on this device", ELL_ERROR);
		}

		CVulkanDepthBufferPool::~CVulkanDepthBufferPool()
		{
			clear();
		}

		VkFormat CVulkanDepthBufferPool::pickDepthFormat(VkFormat preferred) const
		{
			if (Context.PhysicalDevice == VK_NULL_HANDLE)
				return VK_FORMAT_UNDEFINED;

			// Reversed-Z wants the precision of a float depth, so the D32 variants come first;
			// D24S8 is the fallback every desktop driver has, D16 the last resort.
			const VkFormat candidates[] = { preferred, VK_FORMAT_D32_SFLOAT,
				VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM };

			for (u32 i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i)
			{
				if (candidates[i] == VK_FORMAT_UNDEFINED)
					continue;

				VkFormatProperties properties = {};
				vk::GetPhysicalDeviceFormatProperties(Context.PhysicalDevice, candidates[i], &properties);
				if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
					return candidates[i];
			}
			return VK_FORMAT_UNDEFINED;
		}

		SVulkanRTTDepthBuffer* CVulkanDepthBufferPool::checkDepthBuffer(u32 width, u32 height,
			VkSampleCountFlagBits sampleCount)
		{
			if (width == 0 || height == 0 || DepthFormat == VK_FORMAT_UNDEFINED)
				return 0;

			// The key: same extent and same sample count. Both are hard requirements of the
			// attachment set the buffer will be bound into, so an entry differing in either is
			// unusable and a new one is created instead.
			for (size_t i = 0; i < Buffers.size(); ++i)
			{
				SVulkanRTTDepthBuffer* depth = Buffers[i];
				if (depth->Width == width && depth->Height == height && depth->SampleCount == sampleCount)
					return depth;
			}

			SVulkanRTTDepthBuffer* depth = new SVulkanRTTDepthBuffer();
			depth->Width = width;
			depth->Height = height;
			depth->SampleCount = sampleCount;
			depth->Format = DepthFormat;
			depth->Aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
			if (DepthFormat == VK_FORMAT_D24_UNORM_S8_UINT || DepthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT)
				depth->Aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;

			if (!createDepthBuffer(*depth))
			{
				delete depth;
				return 0;
			}

			Buffers.push_back(depth);
			return depth;
		}

		bool CVulkanDepthBufferPool::createDepthBuffer(SVulkanRTTDepthBuffer& depth)
		{
			VkImageCreateInfo imageInfo = {};
			imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
			imageInfo.imageType = VK_IMAGE_TYPE_2D;
			imageInfo.format = depth.Format;
			imageInfo.extent.width = depth.Width;
			imageInfo.extent.height = depth.Height;
			imageInfo.extent.depth = 1;
			imageInfo.mipLevels = 1;
			imageInfo.arrayLayers = 1;
			imageInfo.samples = depth.SampleCount;
			imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
			// Never sampled: a caller that wants to read its depth back passes an explicit
			// CVulkanTexture instead, which carries VK_IMAGE_USAGE_SAMPLED_BIT.
			imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
			imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

			if (vulkanFailed("CVulkanDepthBufferPool: vkCreateImage",
				vk::CreateImage(Context.Device, &imageInfo, nullptr, &depth.Image)))
			{
				depth.Image = VK_NULL_HANDLE;
				return false;
			}
			depth.CurrentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

			VkMemoryRequirements requirements = {};
			vk::GetImageMemoryRequirements(Context.Device, depth.Image, &requirements);

			VkMemoryAllocateInfo allocInfo = {};
			allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			allocInfo.allocationSize = requirements.size;
			allocInfo.memoryTypeIndex = findMemoryTypeIndex(Context.MemoryProperties,
				requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			if (allocInfo.memoryTypeIndex == 0xffffffffu)
			{
				os::Printer::log("CVulkanDepthBufferPool: no device local memory type for depth image", ELL_ERROR);
				vk::DestroyImage(Context.Device, depth.Image, nullptr);
				depth.Image = VK_NULL_HANDLE;
				return false;
			}

			if (vulkanFailed("CVulkanDepthBufferPool: vkAllocateMemory",
					vk::AllocateMemory(Context.Device, &allocInfo, nullptr, &depth.Memory)) ||
				vulkanFailed("CVulkanDepthBufferPool: vkBindImageMemory",
					vk::BindImageMemory(Context.Device, depth.Image, depth.Memory, 0)))
			{
				if (depth.Memory != VK_NULL_HANDLE)
					vk::FreeMemory(Context.Device, depth.Memory, nullptr);
				vk::DestroyImage(Context.Device, depth.Image, nullptr);
				depth.Memory = VK_NULL_HANDLE;
				depth.Image = VK_NULL_HANDLE;
				return false;
			}

			VkImageViewCreateInfo viewInfo = {};
			viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
			viewInfo.image = depth.Image;
			viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
			viewInfo.format = depth.Format;
			// Full aspect, stencil included: this view is only ever an attachment, and the
			// depth/stencil attachment layouts cover both aspects at once.
			viewInfo.subresourceRange.aspectMask = depth.Aspect;
			viewInfo.subresourceRange.levelCount = 1;
			viewInfo.subresourceRange.layerCount = 1;

			if (vulkanFailed("CVulkanDepthBufferPool: vkCreateImageView",
				vk::CreateImageView(Context.Device, &viewInfo, nullptr, &depth.View)))
			{
				depth.View = VK_NULL_HANDLE;
				vk::FreeMemory(Context.Device, depth.Memory, nullptr);
				vk::DestroyImage(Context.Device, depth.Image, nullptr);
				depth.Memory = VK_NULL_HANDLE;
				depth.Image = VK_NULL_HANDLE;
				return false;
			}
			return true;
		}

		void CVulkanDepthBufferPool::clear()
		{
			if (Context.Device == VK_NULL_HANDLE)
			{
				Buffers.clear();
				return;
			}

			// A pass that used one of these may still be in flight; nothing here is refcounted,
			// so the only safe point to destroy is an idle device.
			vk::DeviceWaitIdle(Context.Device);

			for (size_t i = 0; i < Buffers.size(); ++i)
			{
				SVulkanRTTDepthBuffer* depth = Buffers[i];
				if (depth->View != VK_NULL_HANDLE)
					vk::DestroyImageView(Context.Device, depth->View, nullptr);
				if (depth->Image != VK_NULL_HANDLE)
					vk::DestroyImage(Context.Device, depth->Image, nullptr);
				if (depth->Memory != VK_NULL_HANDLE)
					vk::FreeMemory(Context.Device, depth->Memory, nullptr);
				delete depth;
			}
			Buffers.clear();
		}

		// =============================== CVulkanRenderTarget ================================

		CVulkanRenderTarget::CVulkanRenderTarget(const SVulkanContext& context)
			: Context(context)
		{
			reset();
		}

		CVulkanRenderTarget::~CVulkanRenderTarget()
		{
			// Owns no device object: the colour textures belong to the texture cache and the
			// depth image, when pooled, to CVulkanDepthBufferPool.
		}

		void CVulkanRenderTarget::reset()
		{
			for (u32 i = 0; i < MaxColorAttachments; ++i)
			{
				ColorTextures[i] = 0;
				ColorFormats[i] = VK_FORMAT_UNDEFINED;
			}
			ColorCount = 0;
			ColorLayer = WholeImage;

			DepthTexture = 0;
			PooledDepth = 0;
			DepthView = VK_NULL_HANDLE;
			DepthFormat = VK_FORMAT_UNDEFINED;
			DepthAspect = VK_IMAGE_ASPECT_DEPTH_BIT;

			SampleCount = VK_SAMPLE_COUNT_1_BIT;
			Size = core::dimension2d<u32>(0, 0);
			RenderingInfo = VkRenderingInfo();
		}

		bool CVulkanRenderTarget::setTarget(CVulkanTexture* colorTexture,
			CVulkanTexture* depthTexture, CVulkanDepthBufferPool* depthPool, u32 layer)
		{
			if (!setTargets(&colorTexture, 1, depthTexture, depthPool))
				return false;

			if (layer != WholeImage)
			{
				if (layer >= colorTexture->getLayerCount() || colorTexture->getLayerView(layer) == VK_NULL_HANDLE)
				{
					os::Printer::log("CVulkanRenderTarget: render target slice out of range", ELL_ERROR);
					reset();
					return false;
				}
				ColorLayer = layer;
			}
			return true;
		}

		bool CVulkanRenderTarget::setTargets(CVulkanTexture* const* colorTextures, u32 colorCount,
			CVulkanTexture* depthTexture, CVulkanDepthBufferPool* depthPool)
		{
			reset();

			if (!colorTextures || colorCount == 0)
			{
				os::Printer::log("CVulkanRenderTarget: no colour attachment given", ELL_ERROR);
				return false;
			}

			u32 count = core::min_(colorCount, (u32)MaxColorAttachments);

			// Truncate at the first unusable entry rather than failing outright, so a caller
			// asking for more targets than it has valid textures still gets the good ones.
			for (u32 i = 0; i < count; ++i)
			{
				CVulkanTexture* texture = colorTextures[i];
				if (!texture || !texture->hasDeviceResource() || texture->getImageView() == VK_NULL_HANDLE)
				{
					os::Printer::log("CVulkanRenderTarget: missing or incomplete MRT texture", ELL_WARNING);
					count = i;
					break;
				}
				if (CVulkanTexture::isDepthFormat(texture->getVkFormat()))
				{
					os::Printer::log("CVulkanRenderTarget: depth texture used as a colour attachment", ELL_WARNING);
					count = i;
					break;
				}
				// Vulkan renders one area for the whole set, so every attachment must share
				// the extent of the first one -- the same rule D3D enforces on MRT.
				if (texture->getSize() != colorTextures[0]->getSize())
				{
					os::Printer::log("CVulkanRenderTarget: inconsistent MRT size", ELL_WARNING);
					count = i;
					break;
				}
			}

			if (count == 0)
			{
				os::Printer::log("CVulkanRenderTarget: no valid MRT target", ELL_ERROR);
				return false;
			}

			for (u32 i = 0; i < count; ++i)
			{
				ColorTextures[i] = colorTextures[i];
				ColorFormats[i] = colorTextures[i]->getVkFormat();
			}
			ColorCount = count;
			Size = colorTextures[0]->getSize();
			// CVulkanTexture creates single-sample images only; kept as a variable because the
			// pipeline key and the depth pool are both keyed on it.
			SampleCount = VK_SAMPLE_COUNT_1_BIT;

			attachDepth(depthTexture, depthPool);
			return true;
		}

		void CVulkanRenderTarget::attachDepth(CVulkanTexture* depthTexture, CVulkanDepthBufferPool* depthPool)
		{
			// An explicit depth texture wins: it is the deferred-rendering case, where the
			// lighting pass samples back the very depth the G-buffer wrote, which a pooled
			// image (attachment usage only) could not serve.
			if (depthTexture && depthTexture->hasDeviceResource() &&
				CVulkanTexture::isDepthFormat(depthTexture->getVkFormat()))
			{
				if (depthTexture->getSize() != Size)
				{
					os::Printer::log("CVulkanRenderTarget: depth texture size differs from the colour targets,"
						" falling back on the shared depth buffer", ELL_WARNING);
				}
				else
				{
					DepthTexture = depthTexture;
					DepthView = depthTexture->getImageView();
					DepthFormat = depthTexture->getVkFormat();
					DepthAspect = depthTexture->getAspectMask();
					return;
				}
			}
			else if (depthTexture)
			{
				os::Printer::log("CVulkanRenderTarget: the given depthStencil texture has no depth format,"
					" falling back on the shared depth buffer", ELL_WARNING);
			}

			if (!depthPool)
				return; // No pool and no explicit texture: a colour-only target, depth disabled.

			PooledDepth = depthPool->checkDepthBuffer(Size.Width, Size.Height, SampleCount);
			if (PooledDepth)
			{
				DepthView = PooledDepth->View;
				DepthFormat = PooledDepth->Format;
				DepthAspect = PooledDepth->Aspect;
			}
		}

		void CVulkanRenderTarget::bind(VkCommandBuffer commandBuffer)
		{
			if (commandBuffer == VK_NULL_HANDLE || !isValid())
				return;

			for (u32 i = 0; i < ColorCount; ++i)
				ColorTextures[i]->transitionTo(commandBuffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

			if (DepthTexture)
			{
				DepthTexture->transitionTo(commandBuffer, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
			}
			else if (PooledDepth)
			{
				// The pool entry tracks its own layout, so a buffer reused by a second target
				// this frame transitions from where it actually is, not from UNDEFINED.
				transitionImageLayout(commandBuffer, PooledDepth->Image, PooledDepth->CurrentLayout,
					VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, PooledDepth->Aspect);
				PooledDepth->CurrentLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			}
		}

		void CVulkanRenderTarget::unbind(VkCommandBuffer commandBuffer, bool depthToShaderRead)
		{
			if (commandBuffer == VK_NULL_HANDLE || !isValid())
				return;

			// Straight to the sampled layout: what was just rendered is almost always the input
			// of the next pass, and doing it here saves the draw path a barrier it would
			// otherwise have to emit when the texture is bound.
			for (u32 i = 0; i < ColorCount; ++i)
				ColorTextures[i]->transitionTo(commandBuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

			// Only an explicit depth texture can be sampled at all; a pooled image has no
			// SAMPLED usage and stays in its attachment layout for the next target that takes it.
			if (DepthTexture && depthToShaderRead)
				DepthTexture->transitionTo(commandBuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}

		const VkClearValue* CVulkanRenderTarget::getClearValues(SColor color)
		{
			const float r = color.getRed() / 255.f;
			const float g = color.getGreen() / 255.f;
			const float b = color.getBlue() / 255.f;
			const float a = color.getAlpha() / 255.f;

			for (u32 i = 0; i < ColorCount; ++i)
			{
				ClearValues[i].color.float32[0] = r;
				ClearValues[i].color.float32[1] = g;
				ClearValues[i].color.float32[2] = b;
				ClearValues[i].color.float32[3] = a;
			}

			if (hasDepth())
			{
				// 0.0f, not 1.0f: this fork uses a reversed-Z convention (the projection maps the
				// near plane to ~1 and the far plane to ~0, hence the ECFN_GREATER default of
				// SMaterial::ZBuffer), so the far value to clear to is zero.
				ClearValues[ColorCount].depthStencil.depth = 0.0f;
				ClearValues[ColorCount].depthStencil.stencil = 0;
			}
			return ClearValues;
		}

		const VkRenderingInfo& CVulkanRenderTarget::getRenderingInfo(bool clearColor, bool clearDepth,
			SColor color)
		{
			getClearValues(color);

			for (u32 i = 0; i < ColorCount; ++i)
			{
				VkRenderingAttachmentInfo& attachment = ColorAttachments[i];
				attachment = VkRenderingAttachmentInfo();
				attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
				// A whole array texture would need layerCount > 1 and layered rendering; one slice
				// at a time, through its own 2D view, is what setRenderTargetSlice() asks for.
				attachment.imageView = (i == 0 && ColorLayer != WholeImage) ?
					ColorTextures[0]->getLayerView(ColorLayer) : ColorTextures[i]->getImageView();
				attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				attachment.resolveMode = VK_RESOLVE_MODE_NONE;
				attachment.loadOp = clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
				// Always stored: a target only exists to be read by a later pass.
				attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				attachment.clearValue = ClearValues[i];
			}

			DepthAttachment = VkRenderingAttachmentInfo();
			if (hasDepth())
			{
				DepthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
				DepthAttachment.imageView = DepthView;
				DepthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
				DepthAttachment.resolveMode = VK_RESOLVE_MODE_NONE;
				DepthAttachment.loadOp = clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
				DepthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				DepthAttachment.clearValue = ClearValues[ColorCount];
			}

			RenderingInfo = VkRenderingInfo();
			RenderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
			RenderingInfo.renderArea.offset.x = 0;
			RenderingInfo.renderArea.offset.y = 0;
			RenderingInfo.renderArea.extent.width = Size.Width;
			RenderingInfo.renderArea.extent.height = Size.Height;
			RenderingInfo.layerCount = 1;
			RenderingInfo.colorAttachmentCount = ColorCount;
			// The arrays are members, so these pointers stay valid until the next call here.
			RenderingInfo.pColorAttachments = ColorAttachments;
			RenderingInfo.pDepthAttachment = hasDepth() ? &DepthAttachment : nullptr;
			// Stencil is described only when the format actually carries one; pointing a
			// stencil attachment at a depth-only format is invalid.
			RenderingInfo.pStencilAttachment =
				(hasDepth() && (DepthAspect & VK_IMAGE_ASPECT_STENCIL_BIT)) ? &DepthAttachment : nullptr;

			return RenderingInfo;
		}

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
