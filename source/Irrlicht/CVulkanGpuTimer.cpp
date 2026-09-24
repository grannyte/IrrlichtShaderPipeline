// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "CVulkanGpuTimer.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include "os.h"

namespace irr
{
	namespace video
	{
		CVulkanGpuTimer::CVulkanGpuTimer(const SVulkanContext& context, u32 frameCount)
			: Context(context), FrameCount(frameCount)
		{
		}

		CVulkanGpuTimer::~CVulkanGpuTimer()
		{
			destroy();
		}

		SVulkanGpuTimer* CVulkanGpuTimer::find(const core::stringc& name)
		{
			for (size_t i = 0; i < Timers.size(); ++i)
			{
				if (Timers[i].Name == name)
					return &Timers[i];
			}
			return nullptr;
		}

		bool CVulkanGpuTimer::create()
		{
			if (QueryPool != VK_NULL_HANDLE)
				return true;
			if (Context.Device == VK_NULL_HANDLE || !vk::CreateQueryPool || !vk::CmdWriteTimestamp)
			{
				os::Printer::log("CVulkanGpuTimer: no Vulkan device to create the query pool on", ELL_ERROR);
				return false;
			}
			// timestampComputeAndGraphics guarantees every queue can timestamp; short of that, the
			// graphics queue family's own valid-bit count (cached at pickPhysicalDevice()) decides.
			if (Context.DeviceProperties.limits.timestampPeriod <= 0.f ||
				Context.GraphicsQueueTimestampValidBits == 0)
			{
				os::Printer::log("CVulkanGpuTimer: device/queue does not support timestamp queries", ELL_WARNING);
				return false;
			}

			VkQueryPoolCreateInfo poolInfo = {};
			poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
			poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
			poolInfo.queryCount = FrameCount * QueryCapacity * 2;

			VkResult result = vk::CreateQueryPool(Context.Device, &poolInfo, nullptr, &QueryPool);
			if (vulkanFailed("CVulkanGpuTimer: vkCreateQueryPool (timestamp)", result))
			{
				QueryPool = VK_NULL_HANDLE;
				return false;
			}

			TimestampPeriodNs = static_cast<double>(Context.DeviceProperties.limits.timestampPeriod);

			// Handed out from the back, so the first timer registered lands on slot 0.
			FreeSlots.clear();
			for (u32 i = 0; i < QueryCapacity; ++i)
				FreeSlots.push_back(QueryCapacity - 1 - i);

			return true;
		}

		void CVulkanGpuTimer::destroy()
		{
			Timers.clear();
			FreeSlots.clear();

			if (QueryPool != VK_NULL_HANDLE)
			{
				if (vk::DestroyQueryPool && Context.Device != VK_NULL_HANDLE)
					vk::DestroyQueryPool(Context.Device, QueryPool, nullptr);
				QueryPool = VK_NULL_HANDLE;
			}
		}

		void CVulkanGpuTimer::addTimer(const core::stringc& name)
		{
			if (find(name))
				return;
			if (FreeSlots.empty())
			{
				os::Printer::log("CVulkanGpuTimer::addTimer: timer capacity reached (see QueryCapacity)", ELL_WARNING);
				return;
			}

			SVulkanGpuTimer timer(name, FrameCount);
			timer.Slot = FreeSlots.back();
			FreeSlots.pop_back();
			Timers.push_back(timer);
		}

		void CVulkanGpuTimer::removeTimer(const core::stringc& name)
		{
			for (size_t i = 0; i < Timers.size(); ++i)
			{
				if (Timers[i].Name != name)
					continue;
				FreeSlots.push_back(Timers[i].Slot);
				Timers.erase(Timers.begin() + i);
				return;
			}
		}

		void CVulkanGpuTimer::removeAll()
		{
			for (size_t i = 0; i < Timers.size(); ++i)
				FreeSlots.push_back(Timers[i].Slot);
			Timers.clear();
		}

		void CVulkanGpuTimer::beginTimer(VkCommandBuffer commandBuffer, const core::stringc& name, u32 frameIndex)
		{
			if (QueryPool == VK_NULL_HANDLE || commandBuffer == VK_NULL_HANDLE)
				return;
			SVulkanGpuTimer* timer = find(name);
			if (!timer || frameIndex >= FrameCount)
				return;

			const u32 base = frameIndex * QueryCapacity * 2 + timer->Slot * 2;
			if (vk::CmdResetQueryPool)
				vk::CmdResetQueryPool(commandBuffer, QueryPool, base, 2);
			vk::CmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, QueryPool, base);
			timer->BeginIssued[frameIndex] = true;
			timer->EndIssued[frameIndex] = false;
		}

		void CVulkanGpuTimer::endTimer(VkCommandBuffer commandBuffer, const core::stringc& name, u32 frameIndex)
		{
			if (QueryPool == VK_NULL_HANDLE || commandBuffer == VK_NULL_HANDLE)
				return;
			SVulkanGpuTimer* timer = find(name);
			if (!timer || frameIndex >= FrameCount || !timer->BeginIssued[frameIndex])
				return; // no matching begin this frame -- drop the sample

			const u32 base = frameIndex * QueryCapacity * 2 + timer->Slot * 2;
			vk::CmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, QueryPool, base + 1);
			timer->EndIssued[frameIndex] = true;
		}

		void CVulkanGpuTimer::readFrame(SVulkanGpuTimer& timer, u32 frameIndex, bool block)
		{
			if (!vk::GetQueryPoolResults)
				return;

			const u32 base = frameIndex * QueryCapacity * 2 + timer.Slot * 2;
			u64 stamps[2] = { 0, 0 };
			VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT;
			if (block)
				flags |= VK_QUERY_RESULT_WAIT_BIT;

			VkResult result = vk::GetQueryPoolResults(Context.Device, QueryPool, base, 2,
				sizeof(stamps), stamps, sizeof(u64), flags);

			// Not ready without WAIT_BIT: stays pending, previous Result untouched.
			if (result == VK_NOT_READY)
				return;
			if (!vulkanFailed("CVulkanGpuTimer: vkGetQueryPoolResults", result) && stamps[1] >= stamps[0])
				timer.Result = static_cast<f32>((stamps[1] - stamps[0]) * TimestampPeriodNs / 1000000.0);

			timer.BeginIssued[frameIndex] = false;
			timer.EndIssued[frameIndex] = false;
		}

		void CVulkanGpuTimer::harvestFrame(u32 frameIndex)
		{
			if (QueryPool == VK_NULL_HANDLE || frameIndex >= FrameCount)
				return;
			// Fence already waited by the caller, so WAIT_BIT here returns immediately.
			for (size_t i = 0; i < Timers.size(); ++i)
			{
				if (Timers[i].BeginIssued[frameIndex] && Timers[i].EndIssued[frameIndex])
					readFrame(Timers[i], frameIndex, true);
			}
		}

		void CVulkanGpuTimer::updateResult(const core::stringc& name, bool block, u32 currentFrameIndex)
		{
			SVulkanGpuTimer* timer = find(name);
			if (!timer)
				return;
			for (u32 i = 0; i < FrameCount; ++i)
			{
				if (!timer->BeginIssued[i] || !timer->EndIssued[i])
					continue;
				// currentFrameIndex's range is not yet submitted -- waiting on it would hang.
				const bool submitted = (i != currentFrameIndex);
				readFrame(*timer, i, block && submitted);
			}
		}

		void CVulkanGpuTimer::updateAllResults(bool block, u32 currentFrameIndex)
		{
			for (size_t i = 0; i < Timers.size(); ++i)
			{
				for (u32 f = 0; f < FrameCount; ++f)
				{
					if (!Timers[i].BeginIssued[f] || !Timers[i].EndIssued[f])
						continue;
					const bool submitted = (f != currentFrameIndex);
					readFrame(Timers[i], f, block && submitted);
				}
			}
		}

		f32 CVulkanGpuTimer::getResult(const core::stringc& name) const
		{
			for (size_t i = 0; i < Timers.size(); ++i)
			{
				if (Timers[i].Name == name)
					return Timers[i].Result;
			}
			return 0.f;
		}

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
