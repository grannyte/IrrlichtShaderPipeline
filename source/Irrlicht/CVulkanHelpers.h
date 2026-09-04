// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// Dynamic Vulkan entry point loading, shared by every CVulkan* file.
//
// The vendored Khronos headers (source/Irrlicht/vulkan) are declared with VK_NO_PROTOTYPES, so
// nothing links against vulkan-1.lib: the loader is opened at runtime with LoadLibrary and every
// entry point resolved through vkGetInstanceProcAddr/vkGetDeviceProcAddr into the vk::* table
// below. That keeps the engine buildable and shippable on a machine with no Vulkan SDK installed,
// and lets createDeviceEx() fall back to another driver when the loader is simply absent.
//
// Load order, all done by CVulkanDriver::initDriver():
//   loadVulkanLibrary()          -> vkCreateInstance and the other global entry points
//   loadInstanceFunctions(inst)  -> physical device, surface and swapchain-creation entry points
//   loadDeviceFunctions(device)  -> everything recorded per frame (the hot path)

#ifndef __C_VULKAN_HELPERS_H_INCLUDED__
#define __C_VULKAN_HELPERS_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include "irrTypes.h"

#define VK_NO_PROTOTYPES
#ifdef _IRR_WINDOWS_API_
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

namespace irr
{
	namespace video
	{
		//! Every Vulkan entry point the driver uses, resolved by the load*() functions below.
		//! Declared as plain globals in one namespace rather than passed around: the D3D12 driver
		//! reaches its API through a device object it already owns, whereas Vulkan's are free
		//! functions, and threading a dispatch table through every call site buys nothing here.
		namespace vk
		{
			// --- Global (no instance yet) ---
			extern PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
			extern PFN_vkCreateInstance CreateInstance;
			extern PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
			extern PFN_vkEnumerateInstanceLayerProperties EnumerateInstanceLayerProperties;

			// --- Instance ---
			extern PFN_vkDestroyInstance DestroyInstance;
			extern PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
			extern PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
			extern PFN_vkGetPhysicalDeviceFeatures GetPhysicalDeviceFeatures;
			extern PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
			extern PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
			extern PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties;
			extern PFN_vkGetPhysicalDeviceImageFormatProperties GetPhysicalDeviceImageFormatProperties;
			extern PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
			extern PFN_vkCreateDevice CreateDevice;
			extern PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
			extern PFN_vkDestroySurfaceKHR DestroySurfaceKHR;
			extern PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR;
			extern PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR;
			extern PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR;
			extern PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR;
#ifdef VK_USE_PLATFORM_WIN32_KHR
			extern PFN_vkCreateWin32SurfaceKHR CreateWin32SurfaceKHR;
#endif

			// --- Instance: VK_EXT_debug_utils. Optional, and the only two entries in this table
			// allowed to stay null after a successful load: the extension is only asked for in a
			// debug build, and only when the loader actually offers it.
			extern PFN_vkCreateDebugUtilsMessengerEXT CreateDebugUtilsMessengerEXT;
			extern PFN_vkDestroyDebugUtilsMessengerEXT DestroyDebugUtilsMessengerEXT;

			// --- Device: lifetime ---
			extern PFN_vkDestroyDevice DestroyDevice;
			extern PFN_vkGetDeviceQueue GetDeviceQueue;
			extern PFN_vkDeviceWaitIdle DeviceWaitIdle;
			extern PFN_vkQueueWaitIdle QueueWaitIdle;
			extern PFN_vkQueueSubmit QueueSubmit;
			extern PFN_vkQueuePresentKHR QueuePresentKHR;

			// --- Device: swapchain ---
			extern PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
			extern PFN_vkDestroySwapchainKHR DestroySwapchainKHR;
			extern PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
			extern PFN_vkAcquireNextImageKHR AcquireNextImageKHR;

			// --- Device: command recording ---
			extern PFN_vkCreateCommandPool CreateCommandPool;
			extern PFN_vkDestroyCommandPool DestroyCommandPool;
			extern PFN_vkResetCommandPool ResetCommandPool;
			extern PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
			extern PFN_vkFreeCommandBuffers FreeCommandBuffers;
			extern PFN_vkBeginCommandBuffer BeginCommandBuffer;
			extern PFN_vkEndCommandBuffer EndCommandBuffer;
			extern PFN_vkResetCommandBuffer ResetCommandBuffer;

			// --- Device: synchronisation ---
			extern PFN_vkCreateFence CreateFence;
			extern PFN_vkDestroyFence DestroyFence;
			extern PFN_vkWaitForFences WaitForFences;
			extern PFN_vkResetFences ResetFences;
			extern PFN_vkGetFenceStatus GetFenceStatus;
			extern PFN_vkCreateSemaphore CreateSemaphore;
			extern PFN_vkDestroySemaphore DestroySemaphore;

			// --- Device: memory and resources ---
			extern PFN_vkAllocateMemory AllocateMemory;
			extern PFN_vkFreeMemory FreeMemory;
			extern PFN_vkMapMemory MapMemory;
			extern PFN_vkUnmapMemory UnmapMemory;
			extern PFN_vkFlushMappedMemoryRanges FlushMappedMemoryRanges;
			extern PFN_vkCreateBuffer CreateBuffer;
			extern PFN_vkDestroyBuffer DestroyBuffer;
			extern PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
			extern PFN_vkBindBufferMemory BindBufferMemory;
			extern PFN_vkCreateImage CreateImage;
			extern PFN_vkDestroyImage DestroyImage;
			extern PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
			extern PFN_vkBindImageMemory BindImageMemory;
			extern PFN_vkCreateImageView CreateImageView;
			extern PFN_vkDestroyImageView DestroyImageView;
			extern PFN_vkCreateSampler CreateSampler;
			extern PFN_vkDestroySampler DestroySampler;

			// --- Device: descriptors ---
			extern PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
			extern PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
			extern PFN_vkCreateDescriptorPool CreateDescriptorPool;
			extern PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
			extern PFN_vkResetDescriptorPool ResetDescriptorPool;
			extern PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
			extern PFN_vkUpdateDescriptorSets UpdateDescriptorSets;

			// --- Device: pipelines ---
			extern PFN_vkCreateShaderModule CreateShaderModule;
			extern PFN_vkDestroyShaderModule DestroyShaderModule;
			extern PFN_vkCreatePipelineLayout CreatePipelineLayout;
			extern PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
			extern PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
			extern PFN_vkCreateComputePipelines CreateComputePipelines;
			extern PFN_vkDestroyPipeline DestroyPipeline;
			extern PFN_vkCreatePipelineCache CreatePipelineCache;
			extern PFN_vkDestroyPipelineCache DestroyPipelineCache;

			// --- Device: render passes (fallback when dynamic rendering is unavailable) ---
			extern PFN_vkCreateRenderPass CreateRenderPass;
			extern PFN_vkDestroyRenderPass DestroyRenderPass;
			extern PFN_vkCreateFramebuffer CreateFramebuffer;
			extern PFN_vkDestroyFramebuffer DestroyFramebuffer;

			// --- Device: commands recorded per draw ---
			extern PFN_vkCmdBeginRenderPass CmdBeginRenderPass;
			extern PFN_vkCmdEndRenderPass CmdEndRenderPass;
			extern PFN_vkCmdBindPipeline CmdBindPipeline;
			extern PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
			extern PFN_vkCmdBindVertexBuffers CmdBindVertexBuffers;
			extern PFN_vkCmdBindIndexBuffer CmdBindIndexBuffer;
			extern PFN_vkCmdDraw CmdDraw;
			extern PFN_vkCmdDrawIndexed CmdDrawIndexed;
			extern PFN_vkCmdDrawIndexedIndirect CmdDrawIndexedIndirect;
			extern PFN_vkCmdDispatch CmdDispatch;
			extern PFN_vkCmdDispatchIndirect CmdDispatchIndirect;
			extern PFN_vkCmdSetViewport CmdSetViewport;
			extern PFN_vkCmdSetScissor CmdSetScissor;
			extern PFN_vkCmdPushConstants CmdPushConstants;
			extern PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
			extern PFN_vkCmdCopyBuffer CmdCopyBuffer;
			extern PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
			extern PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
			extern PFN_vkCmdCopyImage CmdCopyImage;
			extern PFN_vkCmdBlitImage CmdBlitImage;
			extern PFN_vkCmdClearColorImage CmdClearColorImage;
			extern PFN_vkCmdClearDepthStencilImage CmdClearDepthStencilImage;
			extern PFN_vkCmdClearAttachments CmdClearAttachments;

			// --- Device: queries. Unlike D3D12 a query must be reset before every use, and the
			// reset cannot be recorded inside a rendering instance -- see CVulkanOcclusionQuery.h.
			extern PFN_vkCreateQueryPool CreateQueryPool;
			extern PFN_vkDestroyQueryPool DestroyQueryPool;
			extern PFN_vkCmdResetQueryPool CmdResetQueryPool;
			extern PFN_vkCmdBeginQuery CmdBeginQuery;
			extern PFN_vkCmdEndQuery CmdEndQuery;
			extern PFN_vkGetQueryPoolResults GetQueryPoolResults;

			// --- Device: dynamic rendering (core in 1.3, else VK_KHR_dynamic_rendering) ---
			extern PFN_vkCmdBeginRendering CmdBeginRendering;
			extern PFN_vkCmdEndRendering CmdEndRendering;
		}

		//! Opens the Vulkan loader and resolves the global entry points. Returns false (logged) if
		//! vulkan-1.dll is absent or too old, which is the signal for createDeviceEx() to fall back
		//! to another driver rather than fail outright.
		bool loadVulkanLibrary();

		//! Releases the loader handle and clears the table. Safe to call without a prior load.
		void unloadVulkanLibrary();

		//! Resolves the instance-level entry points. Call once, right after vkCreateInstance.
		bool loadInstanceFunctions(VkInstance instance);

		//! Resolves the device-level entry points, including the dynamic-rendering aliases. Call
		//! once, right after vkCreateDevice. `hasDynamicRendering` reports whether CmdBeginRendering/
		//! CmdEndRendering actually resolved, so the caller can pick the render-pass fallback.
		bool loadDeviceFunctions(VkDevice device, bool& hasDynamicRendering);

		//! Human-readable VkResult, for logging.
		const c8* vulkanResultName(VkResult result);

		//! Logs `what` with the decoded result when `result` is not VK_SUCCESS, and returns true in
		//! that case, so call sites read as `if (vulkanFailed(...)) return false;`.
		bool vulkanFailed(const c8* what, VkResult result);

		//! Index of a memory type satisfying `typeBits` and every flag in `required`, or UINT32_MAX
		//! if the physical device exposes none. `memProps` comes from
		//! GetPhysicalDeviceMemoryProperties.
		u32 findMemoryTypeIndex(const VkPhysicalDeviceMemoryProperties& memProps,
			u32 typeBits, VkMemoryPropertyFlags required);

		//! The device-level state every CVulkan* class needs. Passed by const reference instead of
		//! a CVulkanDriver back-pointer so textures, buffers and the pipeline cache do not have to
		//! see the driver's full definition -- the same reason CD3D12Texture only keeps what it
		//! actually uses. Owned by CVulkanDriver, valid for as long as the driver is.
		struct SVulkanContext
		{
			VkDevice Device = VK_NULL_HANDLE;
			VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
			VkPhysicalDeviceMemoryProperties MemoryProperties = {};
			VkPhysicalDeviceProperties DeviceProperties = {};
			VkQueue GraphicsQueue = VK_NULL_HANDLE;
			u32 GraphicsQueueFamily = 0;
			//! VK_KHR_dynamic_rendering (core in 1.3). When false the driver falls back to explicit
			//! VkRenderPass/VkFramebuffer objects.
			bool HasDynamicRendering = false;
		};

		//! One-shot command buffer for resource upload, used by texture and buffer creation the way
		//! CD3D12Driver::UploadScope is: begin(), record copies, then endAndWait() submits and
		//! blocks until the GPU is done, so the staging buffer can be freed immediately.
		//! Implemented by CVulkanDriver; declared here so the resource classes need not include it.
		class IVulkanUploadContext
		{
		public:
			virtual ~IVulkanUploadContext() {}
			//! VK_NULL_HANDLE if no command buffer could be obtained (already logged).
			virtual VkCommandBuffer beginUpload() = 0;
			virtual void endUploadAndWait(VkCommandBuffer commandBuffer) = 0;
		};

		//! Allocates a VkBuffer plus its backing memory in one step, the pattern every caller here
		//! needs. Returns false (logged) and leaves both handles untouched on failure.
		bool createVulkanBuffer(const SVulkanContext& context, VkDeviceSize size,
			VkBufferUsageFlags usage, VkMemoryPropertyFlags memoryFlags,
			VkBuffer& outBuffer, VkDeviceMemory& outMemory);

		//! Records an image layout transition covering every mip and array layer, picking
		//! conservative stage masks from the two layouts.
		void transitionImageLayout(VkCommandBuffer commandBuffer, VkImage image,
			VkImageLayout oldLayout, VkImageLayout newLayout,
			VkImageAspectFlags aspect, u32 mipLevels = 1, u32 layerCount = 1);

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
#endif
