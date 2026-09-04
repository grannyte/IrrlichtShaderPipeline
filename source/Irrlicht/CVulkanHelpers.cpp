// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// Vulkan entry point table and the small helpers shared by every CVulkan* file:
// result decoding, memory type lookup, buffer allocation and image layout transitions.

#include "CVulkanHelpers.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include "os.h"
#include <stdio.h>

namespace irr
{
	namespace video
	{
		// One definition per extern in the header, all null until the matching load*() runs.
		namespace vk
		{
			// --- Global (no instance yet) ---
			PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
			PFN_vkCreateInstance CreateInstance = nullptr;
			PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;
			PFN_vkEnumerateInstanceLayerProperties EnumerateInstanceLayerProperties = nullptr;

			// --- Instance ---
			PFN_vkDestroyInstance DestroyInstance = nullptr;
			PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
			PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
			PFN_vkGetPhysicalDeviceFeatures GetPhysicalDeviceFeatures = nullptr;
			PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
			PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
			PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties = nullptr;
			PFN_vkGetPhysicalDeviceImageFormatProperties GetPhysicalDeviceImageFormatProperties = nullptr;
			PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
			PFN_vkCreateDevice CreateDevice = nullptr;
			PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
			PFN_vkDestroySurfaceKHR DestroySurfaceKHR = nullptr;
			PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR = nullptr;
			PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
			PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR = nullptr;
			PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR = nullptr;
#ifdef VK_USE_PLATFORM_WIN32_KHR
			PFN_vkCreateWin32SurfaceKHR CreateWin32SurfaceKHR = nullptr;
#endif
			PFN_vkCreateDebugUtilsMessengerEXT CreateDebugUtilsMessengerEXT = nullptr;
			PFN_vkDestroyDebugUtilsMessengerEXT DestroyDebugUtilsMessengerEXT = nullptr;

			// --- Device: lifetime ---
			PFN_vkDestroyDevice DestroyDevice = nullptr;
			PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
			PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
			PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
			PFN_vkQueueSubmit QueueSubmit = nullptr;
			PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;

			// --- Device: swapchain ---
			PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
			PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
			PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;
			PFN_vkAcquireNextImageKHR AcquireNextImageKHR = nullptr;

			// --- Device: command recording ---
			PFN_vkCreateCommandPool CreateCommandPool = nullptr;
			PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
			PFN_vkResetCommandPool ResetCommandPool = nullptr;
			PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
			PFN_vkFreeCommandBuffers FreeCommandBuffers = nullptr;
			PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
			PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
			PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;

			// --- Device: synchronisation ---
			PFN_vkCreateFence CreateFence = nullptr;
			PFN_vkDestroyFence DestroyFence = nullptr;
			PFN_vkWaitForFences WaitForFences = nullptr;
			PFN_vkResetFences ResetFences = nullptr;
			PFN_vkGetFenceStatus GetFenceStatus = nullptr;
			PFN_vkCreateSemaphore CreateSemaphore = nullptr;
			PFN_vkDestroySemaphore DestroySemaphore = nullptr;

			// --- Device: memory and resources ---
			PFN_vkAllocateMemory AllocateMemory = nullptr;
			PFN_vkFreeMemory FreeMemory = nullptr;
			PFN_vkMapMemory MapMemory = nullptr;
			PFN_vkUnmapMemory UnmapMemory = nullptr;
			PFN_vkFlushMappedMemoryRanges FlushMappedMemoryRanges = nullptr;
			PFN_vkCreateBuffer CreateBuffer = nullptr;
			PFN_vkDestroyBuffer DestroyBuffer = nullptr;
			PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
			PFN_vkBindBufferMemory BindBufferMemory = nullptr;
			PFN_vkCreateImage CreateImage = nullptr;
			PFN_vkDestroyImage DestroyImage = nullptr;
			PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
			PFN_vkBindImageMemory BindImageMemory = nullptr;
			PFN_vkCreateImageView CreateImageView = nullptr;
			PFN_vkDestroyImageView DestroyImageView = nullptr;
			PFN_vkCreateSampler CreateSampler = nullptr;
			PFN_vkDestroySampler DestroySampler = nullptr;

			// --- Device: descriptors ---
			PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
			PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
			PFN_vkCreateDescriptorPool CreateDescriptorPool = nullptr;
			PFN_vkDestroyDescriptorPool DestroyDescriptorPool = nullptr;
			PFN_vkResetDescriptorPool ResetDescriptorPool = nullptr;
			PFN_vkAllocateDescriptorSets AllocateDescriptorSets = nullptr;
			PFN_vkUpdateDescriptorSets UpdateDescriptorSets = nullptr;

			// --- Device: pipelines ---
			PFN_vkCreateShaderModule CreateShaderModule = nullptr;
			PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
			PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
			PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
			PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
			PFN_vkCreateComputePipelines CreateComputePipelines = nullptr;
			PFN_vkDestroyPipeline DestroyPipeline = nullptr;
			PFN_vkCreatePipelineCache CreatePipelineCache = nullptr;
			PFN_vkDestroyPipelineCache DestroyPipelineCache = nullptr;

			// --- Device: render passes ---
			PFN_vkCreateRenderPass CreateRenderPass = nullptr;
			PFN_vkDestroyRenderPass DestroyRenderPass = nullptr;
			PFN_vkCreateFramebuffer CreateFramebuffer = nullptr;
			PFN_vkDestroyFramebuffer DestroyFramebuffer = nullptr;

			// --- Device: commands recorded per draw ---
			PFN_vkCmdBeginRenderPass CmdBeginRenderPass = nullptr;
			PFN_vkCmdEndRenderPass CmdEndRenderPass = nullptr;
			PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
			PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
			PFN_vkCmdBindVertexBuffers CmdBindVertexBuffers = nullptr;
			PFN_vkCmdBindIndexBuffer CmdBindIndexBuffer = nullptr;
			PFN_vkCmdDraw CmdDraw = nullptr;
			PFN_vkCmdDrawIndexed CmdDrawIndexed = nullptr;
			PFN_vkCmdDrawIndexedIndirect CmdDrawIndexedIndirect = nullptr;
			PFN_vkCmdDispatch CmdDispatch = nullptr;
			PFN_vkCmdDispatchIndirect CmdDispatchIndirect = nullptr;
			PFN_vkCmdSetViewport CmdSetViewport = nullptr;
			PFN_vkCmdSetScissor CmdSetScissor = nullptr;
			PFN_vkCmdPushConstants CmdPushConstants = nullptr;
			PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
			PFN_vkCmdCopyBuffer CmdCopyBuffer = nullptr;
			PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage = nullptr;
			PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = nullptr;
			PFN_vkCmdCopyImage CmdCopyImage = nullptr;
			PFN_vkCmdBlitImage CmdBlitImage = nullptr;
			PFN_vkCmdClearColorImage CmdClearColorImage = nullptr;
			PFN_vkCmdClearDepthStencilImage CmdClearDepthStencilImage = nullptr;
			PFN_vkCmdClearAttachments CmdClearAttachments = nullptr;

			PFN_vkCreateQueryPool CreateQueryPool = nullptr;
			PFN_vkDestroyQueryPool DestroyQueryPool = nullptr;
			PFN_vkCmdResetQueryPool CmdResetQueryPool = nullptr;
			PFN_vkCmdBeginQuery CmdBeginQuery = nullptr;
			PFN_vkCmdEndQuery CmdEndQuery = nullptr;
			PFN_vkGetQueryPoolResults GetQueryPoolResults = nullptr;

			// --- Device: dynamic rendering ---
			PFN_vkCmdBeginRendering CmdBeginRendering = nullptr;
			PFN_vkCmdEndRendering CmdEndRendering = nullptr;
		}

		namespace
		{
			HMODULE VulkanLibrary = 0;

			//! Returns false so the resolve macros can fold the report straight into their ok flag.
			bool logMissingEntryPoint(const c8* name)
			{
				os::Printer::log("Vulkan entry point not found", name, ELL_ERROR);
				return false;
			}
		}

		// The three levels differ only in which resolver they call, so one macro per level keeps
		// the tables below a plain list of names.
#define IRR_VK_GLOBAL_PROC(name) \
	vk::name = (PFN_vk##name)vk::GetInstanceProcAddr(VK_NULL_HANDLE, "vk" #name); \
	if (!vk::name) ok = logMissingEntryPoint("vk" #name);

#define IRR_VK_INSTANCE_PROC(name) \
	vk::name = (PFN_vk##name)vk::GetInstanceProcAddr(instance, "vk" #name); \
	if (!vk::name) ok = logMissingEntryPoint("vk" #name);

#define IRR_VK_DEVICE_PROC(name) \
	vk::name = (PFN_vk##name)vk::GetDeviceProcAddr(device, "vk" #name); \
	if (!vk::name) ok = logMissingEntryPoint("vk" #name);

		bool loadVulkanLibrary()
		{
			// Idempotent: createDeviceEx() may try the Vulkan driver again after a failed attempt.
			if (VulkanLibrary)
				return vk::CreateInstance != nullptr;

			VulkanLibrary = LoadLibraryA("vulkan-1.dll");
			if (!VulkanLibrary)
			{
				// Warning, not error: no loader is a normal machine state, and the caller falls back.
				os::Printer::log("Could not load vulkan-1.dll, no Vulkan loader installed", ELL_WARNING);
				return false;
			}

			// The only place a Vulkan symbol is touched by name; everything else goes through vk::*.
			vk::GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(VulkanLibrary, "vkGetInstanceProcAddr");
			if (!vk::GetInstanceProcAddr)
			{
				os::Printer::log("vulkan-1.dll does not export vkGetInstanceProcAddr", ELL_ERROR);
				unloadVulkanLibrary();
				return false;
			}

			bool ok = true;
			IRR_VK_GLOBAL_PROC(CreateInstance)
			IRR_VK_GLOBAL_PROC(EnumerateInstanceExtensionProperties)
			IRR_VK_GLOBAL_PROC(EnumerateInstanceLayerProperties)

			// Nothing at all is reachable without these three, so drop the loader again.
			if (!ok)
			{
				unloadVulkanLibrary();
				return false;
			}

			return true;
		}

		void unloadVulkanLibrary()
		{
			if (VulkanLibrary)
			{
				FreeLibrary(VulkanLibrary);
				VulkanLibrary = 0;
			}

			// Wiping the whole table, not just the global part: the instance and device pointers
			// belong to objects that cannot outlive the loader either. Listed by hand rather than
			// cleared in bulk because the table is a set of globals, not one struct.
			vk::GetInstanceProcAddr = nullptr;
			vk::CreateInstance = nullptr;
			vk::EnumerateInstanceExtensionProperties = nullptr;
			vk::EnumerateInstanceLayerProperties = nullptr;

			vk::DestroyInstance = nullptr;
			vk::EnumeratePhysicalDevices = nullptr;
			vk::GetPhysicalDeviceProperties = nullptr;
			vk::GetPhysicalDeviceFeatures = nullptr;
			vk::GetPhysicalDeviceMemoryProperties = nullptr;
			vk::GetPhysicalDeviceQueueFamilyProperties = nullptr;
			vk::GetPhysicalDeviceFormatProperties = nullptr;
			vk::GetPhysicalDeviceImageFormatProperties = nullptr;
			vk::EnumerateDeviceExtensionProperties = nullptr;
			vk::CreateDevice = nullptr;
			vk::GetDeviceProcAddr = nullptr;
			vk::CreateDebugUtilsMessengerEXT = nullptr;
			vk::DestroyDebugUtilsMessengerEXT = nullptr;
			vk::DestroySurfaceKHR = nullptr;
			vk::GetPhysicalDeviceSurfaceSupportKHR = nullptr;
			vk::GetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
			vk::GetPhysicalDeviceSurfaceFormatsKHR = nullptr;
			vk::GetPhysicalDeviceSurfacePresentModesKHR = nullptr;
#ifdef VK_USE_PLATFORM_WIN32_KHR
			vk::CreateWin32SurfaceKHR = nullptr;
#endif

			vk::DestroyDevice = nullptr;
			vk::GetDeviceQueue = nullptr;
			vk::DeviceWaitIdle = nullptr;
			vk::QueueWaitIdle = nullptr;
			vk::QueueSubmit = nullptr;
			vk::QueuePresentKHR = nullptr;

			vk::CreateSwapchainKHR = nullptr;
			vk::DestroySwapchainKHR = nullptr;
			vk::GetSwapchainImagesKHR = nullptr;
			vk::AcquireNextImageKHR = nullptr;

			vk::CreateCommandPool = nullptr;
			vk::DestroyCommandPool = nullptr;
			vk::ResetCommandPool = nullptr;
			vk::AllocateCommandBuffers = nullptr;
			vk::FreeCommandBuffers = nullptr;
			vk::BeginCommandBuffer = nullptr;
			vk::EndCommandBuffer = nullptr;
			vk::ResetCommandBuffer = nullptr;

			vk::CreateFence = nullptr;
			vk::DestroyFence = nullptr;
			vk::WaitForFences = nullptr;
			vk::ResetFences = nullptr;
			vk::GetFenceStatus = nullptr;
			vk::CreateSemaphore = nullptr;
			vk::DestroySemaphore = nullptr;

			vk::AllocateMemory = nullptr;
			vk::FreeMemory = nullptr;
			vk::MapMemory = nullptr;
			vk::UnmapMemory = nullptr;
			vk::FlushMappedMemoryRanges = nullptr;
			vk::CreateBuffer = nullptr;
			vk::DestroyBuffer = nullptr;
			vk::GetBufferMemoryRequirements = nullptr;
			vk::BindBufferMemory = nullptr;
			vk::CreateImage = nullptr;
			vk::DestroyImage = nullptr;
			vk::GetImageMemoryRequirements = nullptr;
			vk::BindImageMemory = nullptr;
			vk::CreateImageView = nullptr;
			vk::DestroyImageView = nullptr;
			vk::CreateSampler = nullptr;
			vk::DestroySampler = nullptr;

			vk::CreateDescriptorSetLayout = nullptr;
			vk::DestroyDescriptorSetLayout = nullptr;
			vk::CreateDescriptorPool = nullptr;
			vk::DestroyDescriptorPool = nullptr;
			vk::ResetDescriptorPool = nullptr;
			vk::AllocateDescriptorSets = nullptr;
			vk::UpdateDescriptorSets = nullptr;

			vk::CreateShaderModule = nullptr;
			vk::DestroyShaderModule = nullptr;
			vk::CreatePipelineLayout = nullptr;
			vk::DestroyPipelineLayout = nullptr;
			vk::CreateGraphicsPipelines = nullptr;
			vk::CreateComputePipelines = nullptr;
			vk::DestroyPipeline = nullptr;
			vk::CreatePipelineCache = nullptr;
			vk::DestroyPipelineCache = nullptr;

			vk::CreateRenderPass = nullptr;
			vk::DestroyRenderPass = nullptr;
			vk::CreateFramebuffer = nullptr;
			vk::DestroyFramebuffer = nullptr;

			vk::CmdBeginRenderPass = nullptr;
			vk::CmdEndRenderPass = nullptr;
			vk::CmdBindPipeline = nullptr;
			vk::CmdBindDescriptorSets = nullptr;
			vk::CmdBindVertexBuffers = nullptr;
			vk::CmdBindIndexBuffer = nullptr;
			vk::CmdDraw = nullptr;
			vk::CmdDrawIndexed = nullptr;
			vk::CmdDrawIndexedIndirect = nullptr;
			vk::CmdDispatch = nullptr;
			vk::CmdDispatchIndirect = nullptr;
			vk::CmdSetViewport = nullptr;
			vk::CmdSetScissor = nullptr;
			vk::CmdPushConstants = nullptr;
			vk::CmdPipelineBarrier = nullptr;
			vk::CmdCopyBuffer = nullptr;
			vk::CmdCopyBufferToImage = nullptr;
			vk::CmdCopyImageToBuffer = nullptr;
			vk::CmdCopyImage = nullptr;
			vk::CmdBlitImage = nullptr;
			vk::CmdClearColorImage = nullptr;
			vk::CmdClearDepthStencilImage = nullptr;
			vk::CmdClearAttachments = nullptr;
			vk::CreateQueryPool = nullptr;
			vk::DestroyQueryPool = nullptr;
			vk::CmdResetQueryPool = nullptr;
			vk::CmdBeginQuery = nullptr;
			vk::CmdEndQuery = nullptr;
			vk::GetQueryPoolResults = nullptr;

			vk::CmdBeginRendering = nullptr;
			vk::CmdEndRendering = nullptr;
		}

		bool loadInstanceFunctions(VkInstance instance)
		{
			if (!vk::GetInstanceProcAddr || instance == VK_NULL_HANDLE)
				return false;

			// The KHR surface entries only resolve if the matching extensions were enabled at
			// instance creation, so a failure here means the instance was built wrong.
			bool ok = true;
			IRR_VK_INSTANCE_PROC(DestroyInstance)
			IRR_VK_INSTANCE_PROC(EnumeratePhysicalDevices)
			IRR_VK_INSTANCE_PROC(GetPhysicalDeviceProperties)
			IRR_VK_INSTANCE_PROC(GetPhysicalDeviceFeatures)
			IRR_VK_INSTANCE_PROC(GetPhysicalDeviceMemoryProperties)
			IRR_VK_INSTANCE_PROC(GetPhysicalDeviceQueueFamilyProperties)
			IRR_VK_INSTANCE_PROC(GetPhysicalDeviceFormatProperties)
			IRR_VK_INSTANCE_PROC(GetPhysicalDeviceImageFormatProperties)
			IRR_VK_INSTANCE_PROC(EnumerateDeviceExtensionProperties)
			IRR_VK_INSTANCE_PROC(CreateDevice)
			// Instance-level itself, and the gateway to everything loadDeviceFunctions() resolves.
			IRR_VK_INSTANCE_PROC(GetDeviceProcAddr)
			IRR_VK_INSTANCE_PROC(DestroySurfaceKHR)
			IRR_VK_INSTANCE_PROC(GetPhysicalDeviceSurfaceSupportKHR)
			IRR_VK_INSTANCE_PROC(GetPhysicalDeviceSurfaceCapabilitiesKHR)
			IRR_VK_INSTANCE_PROC(GetPhysicalDeviceSurfaceFormatsKHR)
			IRR_VK_INSTANCE_PROC(GetPhysicalDeviceSurfacePresentModesKHR)
#ifdef VK_USE_PLATFORM_WIN32_KHR
			IRR_VK_INSTANCE_PROC(CreateWin32SurfaceKHR)
#endif

			// Resolved directly, not through the macro: a null here is normal (the extension is
			// off in release builds) and must not fail the load.
			vk::CreateDebugUtilsMessengerEXT = (PFN_vkCreateDebugUtilsMessengerEXT)
				vk::GetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
			vk::DestroyDebugUtilsMessengerEXT = (PFN_vkDestroyDebugUtilsMessengerEXT)
				vk::GetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");

			return ok;
		}

		bool loadDeviceFunctions(VkDevice device, bool& hasDynamicRendering)
		{
			// Cleared up front so a bail-out never leaves the caller with a stale true.
			hasDynamicRendering = false;

			if (!vk::GetDeviceProcAddr || device == VK_NULL_HANDLE)
				return false;

			// Resolved per device rather than per instance: these bypass the loader's dispatch
			// trampoline, which matters for the entries recorded once per draw.
			bool ok = true;
			IRR_VK_DEVICE_PROC(DestroyDevice)
			IRR_VK_DEVICE_PROC(GetDeviceQueue)
			IRR_VK_DEVICE_PROC(DeviceWaitIdle)
			IRR_VK_DEVICE_PROC(QueueWaitIdle)
			IRR_VK_DEVICE_PROC(QueueSubmit)
			IRR_VK_DEVICE_PROC(QueuePresentKHR)

			// Needs VK_KHR_swapchain in the device extension list.
			IRR_VK_DEVICE_PROC(CreateSwapchainKHR)
			IRR_VK_DEVICE_PROC(DestroySwapchainKHR)
			IRR_VK_DEVICE_PROC(GetSwapchainImagesKHR)
			IRR_VK_DEVICE_PROC(AcquireNextImageKHR)

			IRR_VK_DEVICE_PROC(CreateCommandPool)
			IRR_VK_DEVICE_PROC(DestroyCommandPool)
			IRR_VK_DEVICE_PROC(ResetCommandPool)
			IRR_VK_DEVICE_PROC(AllocateCommandBuffers)
			IRR_VK_DEVICE_PROC(FreeCommandBuffers)
			IRR_VK_DEVICE_PROC(BeginCommandBuffer)
			IRR_VK_DEVICE_PROC(EndCommandBuffer)
			IRR_VK_DEVICE_PROC(ResetCommandBuffer)

			IRR_VK_DEVICE_PROC(CreateFence)
			IRR_VK_DEVICE_PROC(DestroyFence)
			IRR_VK_DEVICE_PROC(WaitForFences)
			IRR_VK_DEVICE_PROC(ResetFences)
			IRR_VK_DEVICE_PROC(GetFenceStatus)
			IRR_VK_DEVICE_PROC(CreateSemaphore)
			IRR_VK_DEVICE_PROC(DestroySemaphore)

			IRR_VK_DEVICE_PROC(AllocateMemory)
			IRR_VK_DEVICE_PROC(FreeMemory)
			IRR_VK_DEVICE_PROC(MapMemory)
			IRR_VK_DEVICE_PROC(UnmapMemory)
			IRR_VK_DEVICE_PROC(FlushMappedMemoryRanges)
			IRR_VK_DEVICE_PROC(CreateBuffer)
			IRR_VK_DEVICE_PROC(DestroyBuffer)
			IRR_VK_DEVICE_PROC(GetBufferMemoryRequirements)
			IRR_VK_DEVICE_PROC(BindBufferMemory)
			IRR_VK_DEVICE_PROC(CreateImage)
			IRR_VK_DEVICE_PROC(DestroyImage)
			IRR_VK_DEVICE_PROC(GetImageMemoryRequirements)
			IRR_VK_DEVICE_PROC(BindImageMemory)
			IRR_VK_DEVICE_PROC(CreateImageView)
			IRR_VK_DEVICE_PROC(DestroyImageView)
			IRR_VK_DEVICE_PROC(CreateSampler)
			IRR_VK_DEVICE_PROC(DestroySampler)

			IRR_VK_DEVICE_PROC(CreateDescriptorSetLayout)
			IRR_VK_DEVICE_PROC(DestroyDescriptorSetLayout)
			IRR_VK_DEVICE_PROC(CreateDescriptorPool)
			IRR_VK_DEVICE_PROC(DestroyDescriptorPool)
			IRR_VK_DEVICE_PROC(ResetDescriptorPool)
			IRR_VK_DEVICE_PROC(AllocateDescriptorSets)
			IRR_VK_DEVICE_PROC(UpdateDescriptorSets)

			IRR_VK_DEVICE_PROC(CreateShaderModule)
			IRR_VK_DEVICE_PROC(DestroyShaderModule)
			IRR_VK_DEVICE_PROC(CreatePipelineLayout)
			IRR_VK_DEVICE_PROC(DestroyPipelineLayout)
			IRR_VK_DEVICE_PROC(CreateGraphicsPipelines)
			IRR_VK_DEVICE_PROC(CreateComputePipelines)
			IRR_VK_DEVICE_PROC(DestroyPipeline)
			IRR_VK_DEVICE_PROC(CreatePipelineCache)
			IRR_VK_DEVICE_PROC(DestroyPipelineCache)

			IRR_VK_DEVICE_PROC(CreateRenderPass)
			IRR_VK_DEVICE_PROC(DestroyRenderPass)
			IRR_VK_DEVICE_PROC(CreateFramebuffer)
			IRR_VK_DEVICE_PROC(DestroyFramebuffer)

			IRR_VK_DEVICE_PROC(CmdBeginRenderPass)
			IRR_VK_DEVICE_PROC(CmdEndRenderPass)
			IRR_VK_DEVICE_PROC(CmdBindPipeline)
			IRR_VK_DEVICE_PROC(CmdBindDescriptorSets)
			IRR_VK_DEVICE_PROC(CmdBindVertexBuffers)
			IRR_VK_DEVICE_PROC(CmdBindIndexBuffer)
			IRR_VK_DEVICE_PROC(CmdDraw)
			IRR_VK_DEVICE_PROC(CmdDrawIndexed)
			IRR_VK_DEVICE_PROC(CmdDrawIndexedIndirect)
			IRR_VK_DEVICE_PROC(CmdDispatch)
			IRR_VK_DEVICE_PROC(CmdDispatchIndirect)
			IRR_VK_DEVICE_PROC(CmdSetViewport)
			IRR_VK_DEVICE_PROC(CmdSetScissor)
			IRR_VK_DEVICE_PROC(CmdPushConstants)
			IRR_VK_DEVICE_PROC(CmdPipelineBarrier)
			IRR_VK_DEVICE_PROC(CmdCopyBuffer)
			IRR_VK_DEVICE_PROC(CmdCopyBufferToImage)
			IRR_VK_DEVICE_PROC(CmdCopyImageToBuffer)
			IRR_VK_DEVICE_PROC(CmdCopyImage)
			IRR_VK_DEVICE_PROC(CmdBlitImage)
			IRR_VK_DEVICE_PROC(CmdClearColorImage)
			IRR_VK_DEVICE_PROC(CmdClearDepthStencilImage)
			IRR_VK_DEVICE_PROC(CmdClearAttachments)
			IRR_VK_DEVICE_PROC(CreateQueryPool)
			IRR_VK_DEVICE_PROC(DestroyQueryPool)
			IRR_VK_DEVICE_PROC(CmdResetQueryPool)
			IRR_VK_DEVICE_PROC(CmdBeginQuery)
			IRR_VK_DEVICE_PROC(CmdEndQuery)
			IRR_VK_DEVICE_PROC(GetQueryPoolResults)

			// Optional, so resolved by hand: core name first, then the extension alias a 1.1/1.2
			// driver with VK_KHR_dynamic_rendering exposes. Absence is not a load failure.
			vk::CmdBeginRendering = (PFN_vkCmdBeginRendering)vk::GetDeviceProcAddr(device, "vkCmdBeginRendering");
			if (!vk::CmdBeginRendering)
				vk::CmdBeginRendering = (PFN_vkCmdBeginRendering)vk::GetDeviceProcAddr(device, "vkCmdBeginRenderingKHR");

			vk::CmdEndRendering = (PFN_vkCmdEndRendering)vk::GetDeviceProcAddr(device, "vkCmdEndRendering");
			if (!vk::CmdEndRendering)
				vk::CmdEndRendering = (PFN_vkCmdEndRendering)vk::GetDeviceProcAddr(device, "vkCmdEndRenderingKHR");

			hasDynamicRendering = (vk::CmdBeginRendering != nullptr && vk::CmdEndRendering != nullptr);

			return ok;
		}

#undef IRR_VK_GLOBAL_PROC
#undef IRR_VK_INSTANCE_PROC
#undef IRR_VK_DEVICE_PROC

		const c8* vulkanResultName(VkResult result)
		{
			// Only the results this driver can actually observe are spelled out.
			switch (result)
			{
			case VK_SUCCESS: return "VK_SUCCESS";
			case VK_NOT_READY: return "VK_NOT_READY";
			case VK_TIMEOUT: return "VK_TIMEOUT";
			case VK_EVENT_SET: return "VK_EVENT_SET";
			case VK_EVENT_RESET: return "VK_EVENT_RESET";
			case VK_INCOMPLETE: return "VK_INCOMPLETE";
			case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
			case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
			case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
			case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
			case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
			case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
			case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
			case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
			case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
			case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
			case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
			case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
			case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
			case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
			case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
			case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
			case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
			case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
			case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
			case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR: return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
			case VK_ERROR_VALIDATION_FAILED_EXT: return "VK_ERROR_VALIDATION_FAILED_EXT";
			case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT: return "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT";
			default:
				break;
			}

			// Extension results the driver never asks for still have to print something stable.
			// The shared buffer is not thread safe, but only an error log ever reaches it.
			static c8 buffer[32];
			sprintf_s(buffer, sizeof(buffer), "VK_ERROR_%d", (s32)result);
			return buffer;
		}

		bool vulkanFailed(const c8* what, VkResult result)
		{
			// Strictly VK_SUCCESS: VK_SUBOPTIMAL_KHR and friends are the call site's to interpret.
			if (result == VK_SUCCESS)
				return false;

			os::Printer::log(what, vulkanResultName(result), ELL_ERROR);
			return true;
		}

		u32 findMemoryTypeIndex(const VkPhysicalDeviceMemoryProperties& memProps,
			u32 typeBits, VkMemoryPropertyFlags required)
		{
			for (u32 i = 0; i < memProps.memoryTypeCount; ++i)
			{
				// typeBits is a mask of permitted indices, taken from the memory requirements.
				if ((typeBits & (1u << i)) == 0)
					continue;

				// First match wins: Vulkan lists memory types cheapest-first for a given heap.

				if ((memProps.memoryTypes[i].propertyFlags & required) == required)
					return i;
			}

			// Every caller treats this as a hard failure, so no separate error out-parameter.
			return 0xFFFFFFFFu;
		}

		bool createVulkanBuffer(const SVulkanContext& context, VkDeviceSize size,
			VkBufferUsageFlags usage, VkMemoryPropertyFlags memoryFlags,
			VkBuffer& outBuffer, VkDeviceMemory& outMemory)
		{
			VkBufferCreateInfo bufferInfo = {};
			bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
			bufferInfo.size = size;
			bufferInfo.usage = usage;
			// EXCLUSIVE: the driver only ever touches buffers from its single graphics queue.
			bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

			// Built into locals so the caller's handles stay untouched if any step fails. No
			// allocation callbacks anywhere in the driver, the host allocator is fine.
			VkBuffer buffer = VK_NULL_HANDLE;
			if (vulkanFailed("Could not create Vulkan buffer", vk::CreateBuffer(context.Device, &bufferInfo, 0, &buffer)))
				return false;

			VkMemoryRequirements requirements = {};
			vk::GetBufferMemoryRequirements(context.Device, buffer, &requirements);

			const u32 typeIndex = findMemoryTypeIndex(context.MemoryProperties, requirements.memoryTypeBits, memoryFlags);
			if (typeIndex == 0xFFFFFFFFu)
			{
				os::Printer::log("No Vulkan memory type matches the requested buffer properties", ELL_ERROR);
				vk::DestroyBuffer(context.Device, buffer, 0);
				return false;
			}

			VkMemoryAllocateInfo allocInfo = {};
			allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			// requirements.size, not size: alignment padding can push the real cost higher.
			allocInfo.allocationSize = requirements.size;
			allocInfo.memoryTypeIndex = typeIndex;

			VkDeviceMemory memory = VK_NULL_HANDLE;
			if (vulkanFailed("Could not allocate Vulkan buffer memory", vk::AllocateMemory(context.Device, &allocInfo, 0, &memory)))
			{
				vk::DestroyBuffer(context.Device, buffer, 0);
				return false;
			}

			if (vulkanFailed("Could not bind Vulkan buffer memory", vk::BindBufferMemory(context.Device, buffer, memory, 0)))
			{
				vk::FreeMemory(context.Device, memory, 0);
				vk::DestroyBuffer(context.Device, buffer, 0);
				return false;
			}

			outBuffer = buffer;
			outMemory = memory;
			return true;
		}

		void transitionImageLayout(VkCommandBuffer commandBuffer, VkImage image,
			VkImageLayout oldLayout, VkImageLayout newLayout,
			VkImageAspectFlags aspect, u32 mipLevels, u32 layerCount)
		{
			// A same-layout barrier is legal but still costs a stall, and callers hit this often.
			if (oldLayout == newLayout)
				return;

			VkImageMemoryBarrier barrier = {};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.oldLayout = oldLayout;
			barrier.newLayout = newLayout;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = image;
			// Whole image in one barrier; aspect is the caller's because depth/stencil needs both bits.
			barrier.subresourceRange.aspectMask = aspect;
			barrier.subresourceRange.baseMipLevel = 0;
			barrier.subresourceRange.levelCount = mipLevels;
			barrier.subresourceRange.baseArrayLayer = 0;
			barrier.subresourceRange.layerCount = layerCount;

			// Seeded with the widest stages, so only the default access-mask case below is left
			// to fall through untouched.
			VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

			// UNDEFINED discards the old contents, which is exactly what a fresh upload wants.
			if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
			{
				barrier.srcAccessMask = 0;
				barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
				dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			}
			else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
			{
				barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
				// Vertex stage too: a texture may be sampled for displacement, not just shading.
				dstStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			}
			else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
			{
				barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
				barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				srcStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
				dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			}
			else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
			{
				barrier.srcAccessMask = 0;
				barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
				srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
				dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			}
			// The transfer-to-transfer pairs are what mip generation walks the chain with.
			else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
			{
				barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
				srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
				dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			}
			else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
			{
				barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
				barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
				dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			}
			else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
			{
				barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
				barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
				dstStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			}
			else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
			{
				barrier.srcAccessMask = 0;
				barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
				srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
				dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			}
			else if (oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
			{
				// No dst access mask: the present engine is synchronised by the semaphore instead.
				barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
				barrier.dstAccessMask = 0;
				srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
			}
			else if (oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR && newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
			{
				// Re-acquired swapchain image: nothing of ours wrote it, so no source access.
				barrier.srcAccessMask = 0;
				barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
				srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			}
			// A render target being sampled back, the usual second pass of a post effect.
			else if (oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
			{
				barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
				barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				dstStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			}
			else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
			{
				barrier.srcAccessMask = 0;
				barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
				srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
				// Both test stages: the layout covers depth reads and writes alike.
				dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			}
			else
			{
				// Correct for any pair, just needlessly wide; the listed pairs are the hot ones.
				barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
				barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			}

			// One image barrier; the memory and buffer barrier slots stay empty.
			vk::CmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, 0, 0, 0, 1, &barrier);
		}

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
