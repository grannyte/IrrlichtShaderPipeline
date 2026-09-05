// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h
// See CVulkanDeferredContext.h for the architecture and the threading contract.

#include "CVulkanDeferredContext.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include "CVulkanTexture.h"
#include "os.h"

namespace irr
{
	namespace video
	{
		// The CVulkanDriver base constructor stores the parameters, the file system and the window
		// size and builds the EVT_STANDARD input state; nothing touches the loader or a device until
		// initDriver(), which this context never calls. Everything device-side is copied from
		// `immediate` below.
		CVulkanDeferredContext::CVulkanDeferredContext(CVulkanDriver* immediate)
			: CVulkanDriver(immediate->Params, immediate->FileSystem, (HWND)0)
			, ImmediateDriver(immediate)
		{
			if (!immediate->Context.Device)
			{
				os::Printer::log("CVulkanDeferredContext: the immediate driver has no device", ELL_ERROR);
				return;
			}

			// Borrowed handles and capability flags: one SVulkanContext copy is the whole "device"
			// of this context. The destructor of CVulkanDriver skips every owner-side teardown when
			// OwnsDevice is false, and every lookup/creation that must reach the owner's tables goes
			// through ResourceOwner.
			OwnsDevice = false;
			ResourceOwner = immediate;
			Context = immediate->Context;
			DepthFormat = immediate->DepthFormat;
			SwapchainFormat = immediate->SwapchainFormat;
			HasGeometryShader = immediate->HasGeometryShader;
			HasPreciseOcclusionQuery = immediate->HasPreciseOcclusionQuery;
			// Stream output, compute and occlusion stay immediate-only (see the header): the flag
			// below and the null Compute/Occlusion make queryFeature() say so.
			TransformFeedbackEnabled = false;
			Context.HasTransformFeedback = false;
			NullTexture = immediate->NullTexture;
			MaterialTextureSetLayout = immediate->MaterialTextureSetLayout;
			EmptySetLayout = immediate->EmptySetLayout;
			DriverUniformSetLayout = immediate->DriverUniformSetLayout;
			BuiltInPipelineLayout = immediate->BuiltInPipelineLayout;
			FrameCounter = immediate->FrameCounter;

			// Own upload pool: a copyTexture()/screenshot issued from here goes through
			// beginUpload() on this object, and command pools are not shareable across threads.
			VkCommandPoolCreateInfo poolInfo = {};
			poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
			poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
			poolInfo.queueFamilyIndex = Context.GraphicsQueueFamily;
			if (vulkanFailed("CVulkanDeferredContext: upload command pool",
				vk::CreateCommandPool(Context.Device, &poolInfo, nullptr, &UploadPool)))
				return;

			// Two frame slots of our own, so a recording can be prepared while the previous
			// submission is still executing, same as the owner and as the D3D12 context's ring.
			if (!createFrameContexts())
			{
				os::Printer::log("CVulkanDeferredContext: frame contexts failed", ELL_ERROR);
				return;
			}

			DepthPool = new CVulkanDepthBufferPool(Context, DepthFormat);
			RenderTarget = new CVulkanRenderTarget(Context);

			// The offscreen target, the size of the owner's current target (the swapchain, unless it
			// is mid-pass). Created through the owner so it sits in the shared texture cache, which
			// is the only owner it has: this context never drops it.
			CurrentRenderTargetSize = immediate->CurrentRenderTargetSize;
			if (CurrentRenderTargetSize.Width == 0 || CurrentRenderTargetSize.Height == 0)
				CurrentRenderTargetSize = immediate->ScreenSize;
			Target = immediate->addRenderTargetTexture(CurrentRenderTargetSize,
				"CVulkanDeferredContext_Target", ECF_A8R8G8B8);
			if (!Target)
			{
				os::Printer::log("CVulkanDeferredContext: render target creation failed", ELL_ERROR);
				return;
			}

			Ready = true;
			beginRecording();
			Ready = SceneOpen;
		}

		CVulkanDeferredContext::~CVulkanDeferredContext()
		{
			// A submission still running must not lose its command buffer or descriptor pools;
			// ~CVulkanDriver() then tears down what this object created and nothing of the owner's.
			waitForCompletion();
			if (SceneOpen && Context.Device)
			{
				// A recording that was never executed: close the buffer so the pool reset is legal.
				endRendering();
				unbindRenderTarget();
				vk::EndCommandBuffer(Frames[CurrentFrameIndex].CommandBuffer);
				SceneOpen = false;
			}
		}

		bool CVulkanDeferredContext::beginScene(bool, bool, SColor, const SExposedVideoData&, core::rect<s32>*)
		{
			// The recording is already open (constructor/beginRecording()); a scene here has no
			// swapchain image to acquire.
			return Ready;
		}

		bool CVulkanDeferredContext::endScene()
		{
			return Ready;
		}

		void CVulkanDeferredContext::OnResize(const core::dimension2d<u32>&)
		{
			os::Printer::log("CVulkanDeferredContext::OnResize: a deferred context has no swapchain, "
				"ignored", ELL_WARNING);
		}

		bool CVulkanDeferredContext::setRenderTarget(video::ITexture* texture, bool clearBackBuffer,
			bool clearZBuffer, SColor color, video::ITexture* depthStencil)
		{
			// "The frame buffer" of this context is its own target, never the owner's swapchain.
			if (texture && texture != Target)
				return CVulkanDriver::setRenderTarget(texture, clearBackBuffer, clearZBuffer, color, depthStencil);
			if (!SceneOpen)
			{
				os::Printer::log("CVulkanDeferredContext::setRenderTarget: no recording is open", ELL_WARNING);
				return false;
			}
			endRendering();
			unbindRenderTarget();
			MrtBlend.reset();
			CVulkanTexture* target = static_cast<CVulkanTexture*>(Target);
			if (!RenderTarget->setTarget(target, nullptr, DepthPool))
				return false;
			RenderTargetActive = true;
			return activateRenderTarget(clearBackBuffer, clearZBuffer, color);
		}

		IVideoDriver* CVulkanDeferredContext::createDeferredContext()
		{
			os::Printer::log("CVulkanDeferredContext::createDeferredContext: nesting deferred contexts "
				"is not supported, create it from the immediate driver", ELL_ERROR);
			return nullptr;
		}

		bool CVulkanDeferredContext::prepareRecordingState()
		{
			CVulkanTexture* target = static_cast<CVulkanTexture*>(Target);
			RenderTargetActive = false;
			RenderTarget->reset();
			if (!RenderTarget->setTarget(target, nullptr, DepthPool))
			{
				os::Printer::log("CVulkanDeferredContext: could not bind the render target", ELL_ERROR);
				return false;
			}
			RenderTargetActive = true;
			MrtBlend.reset();
			// bind() records the layout transitions and beginRendering() clears: a recording always
			// starts from an opaque black target, as the D3D12 context's does.
			return activateRenderTarget(true, true, SColor(255, 0, 0, 0));
		}

		void CVulkanDeferredContext::beginRecording()
		{
			if (!Ready)
				return;
			if (SceneOpen)
			{
				os::Printer::log("CVulkanDeferredContext::beginRecording: the previous recording was "
					"not executed, discarding it", ELL_WARNING);
				endRendering();
				unbindRenderTarget();
				vk::EndCommandBuffer(Frames[CurrentFrameIndex].CommandBuffer);
				SceneOpen = false;
			}

			CurrentFrameIndex = (CurrentFrameIndex + 1) % FrameCount;
			SVulkanFrameContext& frame = Frames[CurrentFrameIndex];

			// Same per-slot reset as CVulkanDriver::beginScene(), minus the swapchain: the fence
			// proves the GPU is done with everything this slot recorded two executes ago.
			vk::WaitForFences(Context.Device, 1, &frame.Fence, VK_TRUE, UINT64_MAX);
			vk::ResetFences(Context.Device, 1, &frame.Fence);
			Submitted[CurrentFrameIndex] = false;
			vk::ResetCommandPool(Context.Device, frame.CommandPool, 0);
			for (size_t p = 0; p < frame.DescriptorPools.size(); ++p)
				vk::ResetDescriptorPool(Context.Device, frame.DescriptorPools[p], 0);
			frame.CurrentDescriptorPool = 0;
			frame.UniformRingNext = 0;
			for (size_t r = 0; r < frame.RetiredUniformBuffers.size(); ++r)
			{
				vk::DestroyBuffer(Context.Device, frame.RetiredUniformBuffers[r], nullptr);
				vk::FreeMemory(Context.Device, frame.RetiredUniformMemory[r], nullptr);
			}
			frame.RetiredUniformBuffers.clear();
			frame.RetiredUniformMemory.clear();
			frame.Immediate->reset(true);

			VkCommandBufferBeginInfo begin = {};
			begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			if (vulkanFailed("CVulkanDeferredContext: vkBeginCommandBuffer",
				vk::BeginCommandBuffer(frame.CommandBuffer, &begin)))
				return;

			SceneOpen = true;
			RenderingActive = false;
			// A deferred frame is a frame of the owner's timeline for the stamps it hands out.
			FrameCounter = ImmediateDriver->FrameCounter;
			if (!prepareRecordingState())
			{
				vk::EndCommandBuffer(frame.CommandBuffer);
				SceneOpen = false;
			}
		}

		void CVulkanDeferredContext::execute(IVideoDriver*)
		{
			if (!Ready || !SceneOpen)
			{
				os::Printer::log("CVulkanDeferredContext::execute: nothing recorded (call "
					"beginRecording() after each execute())", ELL_WARNING);
				return;
			}

			SVulkanFrameContext& frame = Frames[CurrentFrameIndex];

			// Close the pass and hand the target over in the sampled layout: the owner draws it next.
			endRendering();
			unbindRenderTarget();
			if (vulkanFailed("CVulkanDeferredContext: vkEndCommandBuffer",
				vk::EndCommandBuffer(frame.CommandBuffer)))
			{
				SceneOpen = false;
				return;
			}

			VkSubmitInfo submit = {};
			submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			submit.commandBufferCount = 1;
			submit.pCommandBuffers = &frame.CommandBuffer;

			// No semaphores: the owner's frame and this submission are ordered by the queue, and
			// the target is only ever read by a frame recorded after execute() returned. The lock
			// is the owner's; the queue is shared with its endScene() and every upload.
			{
				std::lock_guard<std::mutex> queueLock(ResourceOwner->QueueMutex);
				if (!vulkanFailed("CVulkanDeferredContext: vkQueueSubmit",
					vk::QueueSubmit(Context.GraphicsQueue, 1, &submit, frame.Fence)))
					Submitted[CurrentFrameIndex] = true;
			}
			SceneOpen = false;
		}

		void CVulkanDeferredContext::waitForCompletion()
		{
			if (!Context.Device)
				return;
			for (u32 i = 0; i < FrameCount; ++i)
			{
				if (!Submitted[i])
					continue;
				vk::WaitForFences(Context.Device, 1, &Frames[i].Fence, VK_TRUE, UINT64_MAX);
				// Stays signalled until beginRecording() reuses the slot; nothing left to wait for.
				Submitted[i] = false;
			}
		}
	}
}
#endif // _IRR_COMPILE_WITH_VULKAN_
