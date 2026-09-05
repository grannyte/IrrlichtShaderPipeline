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

			//! Readback slots behind IVideoDriver::beginComputeReadback()/tryReadComputeBuffer(). The
			//! copy is recorded on the upload context, which waits for it, so a queued readback is
			//! complete on return and tryAsyncReadback() never has to poll -- correct, if not
			//! overlapped with GPU work the way the D3D11 staging copies are.
			static const u32 ReadbackSlotCount = 4;
			bool beginAsyncReadback(u32 slot);
			//! Copies a completed readback into `dst` (at most `bytes`). False while nothing was
			//! queued in that slot; `wait` is accepted for interface parity and changes nothing.
			bool tryAsyncReadback(u32 slot, void* dst, u32 bytes, bool wait);

			//! The hidden counter of an append/consume buffer: one uint in a host-visible buffer of its
			//! own, created on first request (D3D11 keeps it inside the UAV; SPIR-V has no such thing,
			//! DXC emits a separate one-uint buffer instead, see SVulkanComputeBinding::CounterOf).
			//! VK_NULL_HANDLE when `create` is false and none exists yet.
			VkBuffer getCounterBuffer(bool create);
			//! CPU-side write/read of the counter. Only valid while no dispatch touching it is in
			//! flight -- always the case here, every dispatch is submitted and waited on.
			bool setCounterValue(u32 value);
			bool getCounterValue(u32& outValue) const;

			//! True once setStreamOutputBuffer() captured into this buffer: its counter then holds the
			//! byte count transform feedback wrote, and a draw from it takes the byte-count form (the
			//! D3D11 DrawAuto) rather than the vertex count of the source IVertexBuffer. Cleared by
			//! update(): CPU data uploaded afterwards is drawn the ordinary way again.
			bool hasStreamOutputCount() const
			{
				return StreamOutputCaptured && Type == EHBT_STREAM_OUTPUT && CounterBuffer != VK_NULL_HANDLE;
			}
			void setStreamOutputCaptured(bool captured) { StreamOutputCaptured = captured; }

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

			//! One host-visible copy per readback slot, sized to the buffer on first use.
			struct SReadbackSlot
			{
				VkBuffer Buffer = VK_NULL_HANDLE;
				VkDeviceMemory Memory = VK_NULL_HANDLE;
				VkDeviceSize Size = 0;
				bool Ready = false;
			};
			SReadbackSlot Readback[ReadbackSlotCount];
			void releaseReadbackSlots();

			//! See hasStreamOutputCount().
			bool StreamOutputCaptured = false;

			//! See getCounterBuffer(); mapped for its whole lifetime.
			VkBuffer CounterBuffer = VK_NULL_HANDLE;
			VkDeviceMemory CounterMemory = VK_NULL_HANDLE;
			u32* CounterMapped = nullptr;
			void releaseCounterBuffer();

			video::E_INDEX_TYPE IndexType = video::EIT_16BIT;
		};

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
#endif
