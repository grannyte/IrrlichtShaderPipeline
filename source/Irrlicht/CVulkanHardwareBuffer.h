// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#ifndef __C_VULKAN_HARDWARE_BUFFER_H_INCLUDED__
#define __C_VULKAN_HARDWARE_BUFFER_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include "IHardwareBuffer.h"
#include "IVertexBuffer.h"
#include "IIndexBuffer.h"
#include "CVulkanHelpers.h"

namespace irr
{
	namespace video
	{

		//! One VkBuffer plus its VkDeviceMemory, the counterpart of CD3D12HardwareBuffer.
		//! Immutable/static content (and every compute or stream-output buffer) goes to
		//! DEVICE_LOCAL memory filled through a staging buffer on the upload context; everything
		//! else to HOST_VISIBLE|HOST_COHERENT memory mapped once for its whole lifetime.
		class CVulkanHardwareBuffer : public IHardwareBuffer
		{
		public:
			//! EHBT_VERTEX, or EHBT_STREAM_OUTPUT for a scene::EBT_STREAM source.
			CVulkanHardwareBuffer(const SVulkanContext& context, IVulkanUploadContext& uploadContext,
				scene::IVertexBuffer* vertexBuffer);

			CVulkanHardwareBuffer(const SVulkanContext& context, IVulkanUploadContext& uploadContext,
				scene::IIndexBuffer* indexBuffer);

			//! Generic form. A null initialData allocates without filling.
			CVulkanHardwareBuffer(const SVulkanContext& context, IVulkanUploadContext& uploadContext,
				u32 size, E_HARDWARE_BUFFER_TYPE type, E_HARDWARE_BUFFER_ACCESS access,
				u32 flags = 0, u32 stride = 0, const void* initialData = 0);

			virtual ~CVulkanHardwareBuffer();

			//! Copies data in, growing (destroy + recreate) past the current allocation.
			bool update(const scene::E_HARDWARE_MAPPING mapping, const u32 size, const void* data) _IRR_OVERRIDE_;

			//! The persistent pointer, or a staging buffer (read back first when readOnly).
			void* lock(bool readOnly = false) _IRR_OVERRIDE_;

			void unlock() _IRR_OVERRIDE_;

			VkBuffer getBuffer() const { return Buffer; }
			VkDeviceMemory getMemory() const { return Memory; }
			u32 getSize() const { return Size; }
			E_HARDWARE_BUFFER_ACCESS getAccess() const { return Access; }
			u32 getStride() const { return Stride; }
			video::E_INDEX_TYPE getIndexType() const { return IndexType; } // EHBT_INDEX only
			bool isDeviceLocal() const { return DeviceLocal; }

		private:
			bool createInternalBuffer(const void* initialData);
			void destroyInternalBuffer();

			//! Device-local path: pushes size bytes through a throwaway staging buffer, blocking.
			bool uploadThroughStaging(const void* data, u32 size);
			void releaseStagingBuffer();

			//! TRANSFER_WRITE -> dstAccess barrier, so the copy just recorded is visible next.
			void recordCopyBarrier(VkCommandBuffer commandBuffer, VkBuffer buffer,
				VkAccessFlags dstAccess, VkPipelineStageFlags dstStage);

			//! Bind points implied by Type plus the extras asked for through Flags. The transfer
			//! bits are added by the caller, which knows which memory the buffer lands in.
			VkBufferUsageFlags bufferUsageFlags() const;
			bool wantsDeviceLocal() const;

			const SVulkanContext& Context;
			IVulkanUploadContext& Upload;
			E_HARDWARE_BUFFER_ACCESS Access;

			VkBuffer Buffer = VK_NULL_HANDLE;
			VkDeviceMemory Memory = VK_NULL_HANDLE;
			void* MappedData = 0;
			bool DeviceLocal = false;

			//! Scratch for lock()/unlock() on the device-local path, rebuilt on each lock.
			VkBuffer StagingBuffer = VK_NULL_HANDLE;
			VkDeviceMemory StagingMemory = VK_NULL_HANDLE;
			void* MappedStagingData = 0;
			bool StagingIsReadback = false;

			video::E_INDEX_TYPE IndexType = video::EIT_16BIT;
		};

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
#endif
