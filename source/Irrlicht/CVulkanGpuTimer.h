// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// One VK_QUERY_TYPE_TIMESTAMP pool, one range per driver frame-in-flight slot -- never shared --
// so begin/end/read never wait on GPU work. vkCmdResetQueryPool is illegal inside dynamic rendering.

#ifndef __C_VULKAN_GPU_TIMER_H_INCLUDED__
#define __C_VULKAN_GPU_TIMER_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include "CVulkanHelpers.h"
#include "irrString.h"
#include <vector>

namespace irr
{
	namespace video
	{
		//! Everything kept per named timer. BeginIssued/EndIssued are indexed by frame-in-flight
		//! slot, sized to frameCount at addTimer() -- more than one slot can be pending at once.
		struct SVulkanGpuTimer
		{
			SVulkanGpuTimer() {}
			SVulkanGpuTimer(const core::stringc& name, u32 frameCount)
				: Name(name), BeginIssued(frameCount, false), EndIssued(frameCount, false) {}
			bool operator==(const SVulkanGpuTimer& other) const { return other.Name == Name; }

			core::stringc Name;
			u32 Slot = 0;
			std::vector<bool> BeginIssued;
			std::vector<bool> EndIssued;
			f32 Result = 0.f;
		};

		//! Takes the shared device context by const reference, like CVulkanOcclusionQuery.
		class CVulkanGpuTimer
		{
		public:
			//! frameCount must match the driver's own frames-in-flight count (CVulkanDriver::FrameCount).
			CVulkanGpuTimer(const SVulkanContext& context, u32 frameCount);
			~CVulkanGpuTimer();

			CVulkanGpuTimer(const CVulkanGpuTimer&) = delete;
			CVulkanGpuTimer& operator=(const CVulkanGpuTimer&) = delete;

			//! False (logged) when the device/queue cannot timestamp, or the pool fails to create;
			//! every call below then degrades to a no-op, as CVulkanOcclusionQuery does.
			bool create();
			void destroy();
			bool isValid() const { return QueryPool != VK_NULL_HANDLE; }

			//! Named timers per frame-in-flight range, two query indices (begin/end) each.
			static const u32 QueryCapacity = 64;

			void addTimer(const core::stringc& name);
			void removeTimer(const core::stringc& name);
			void removeAll();

			//! Resets and writes the begin timestamp into frameIndex's own range only -- never touches
			//! another frame's range, so this never has to wait on anything.
			void beginTimer(VkCommandBuffer commandBuffer, const core::stringc& name, u32 frameIndex);

			//! Records the end timestamp; a no-op without a matching begin this frame, which is how an
			//! unpaired begin drops its sample.
			void endTimer(VkCommandBuffer commandBuffer, const core::stringc& name, u32 frameIndex);

			//! Call once per beginScene() for the frame index about to be reused, after its fence wait
			//! -- WAIT_BIT here is free. Must run before that range is reset, or a sample is dropped.
			void harvestFrame(u32 frameIndex);

			//! Never waits on currentFrameIndex's own range -- still being recorded, not submitted.
			void updateResult(const core::stringc& name, bool block, u32 currentFrameIndex);
			void updateAllResults(bool block, u32 currentFrameIndex);

			f32 getResult(const core::stringc& name) const;

		private:
			const SVulkanContext& Context;
			const u32 FrameCount;
			VkQueryPool QueryPool = VK_NULL_HANDLE;
			std::vector<u32> FreeSlots;
			std::vector<SVulkanGpuTimer> Timers;
			//! Nanoseconds per timestamp tick, cached at create() from the device limits.
			double TimestampPeriodNs = 0.0;

			//! Linear search by name, as CNullDriver::GpuTimers does; ~64 entries, never hot.
			SVulkanGpuTimer* find(const core::stringc& name);
			//! vkGetQueryPoolResults for timer's pair in frameIndex's range; on success writes Result
			//! and clears Begin/EndIssued[frameIndex], on VK_NOT_READY leaves both untouched.
			void readFrame(SVulkanGpuTimer& timer, u32 frameIndex, bool block);
		};

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
#endif
