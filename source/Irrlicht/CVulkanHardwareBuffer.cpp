// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "CVulkanHardwareBuffer.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include "os.h"
#include <string.h>

namespace irr
{
	namespace video
	{
		namespace
		{
			const VkMemoryPropertyFlags HostMemoryFlags =
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

			//! Same rule as CD3D11HardwareBuffer: a scene::EBT_STREAM source is a stream-output
			//! target, anything else is plain geometry.
			E_HARDWARE_BUFFER_TYPE hardwareTypeOf(scene::E_BUFFER_TYPE type)
			{
				return (type == scene::EBT_STREAM) ? EHBT_STREAM_OUTPUT : EHBT_VERTEX;
			}

			E_HARDWARE_BUFFER_ACCESS accessFromMapping(scene::E_HARDWARE_MAPPING mapping)
			{
				switch (mapping)
				{
				case scene::EHM_STATIC:
					return EHBA_IMMUTABLE;
				case scene::EHM_STAGING:
					return EHBA_SYSTEM_MEMORY;
				default: // EHM_NEVER/EHM_DYNAMIC/EHM_STREAM: CPU writes it, possibly every frame
					return EHBA_DYNAMIC;
				}
			}

			scene::E_HARDWARE_MAPPING mappingFromAccess(E_HARDWARE_BUFFER_ACCESS access)
			{
				switch (access)
				{
				case EHBA_IMMUTABLE:
					return scene::EHM_STATIC;
				case EHBA_DEFAULT:
					return scene::EHM_DYNAMIC;
				case EHBA_SYSTEM_MEMORY:
					return scene::EHM_STAGING;
				default:
					return scene::EHM_STREAM;
				}
			}
		}

		CVulkanHardwareBuffer::CVulkanHardwareBuffer(const SVulkanContext& context,
			IVulkanUploadContext& uploadContext, scene::IVertexBuffer* vertexBuffer)
			: IHardwareBuffer(vertexBuffer->getHardwareMappingHint(), 0,
				vertexBuffer->getVertexCount() * vertexBuffer->getVertexSize(),
				hardwareTypeOf(vertexBuffer->getBufferType()), EDT_VULKAN),
			Context(context), Upload(uploadContext),
			Access(accessFromMapping(vertexBuffer->getHardwareMappingHint()))
		{
			// Stride is the inherited member, kept for the driver's vertex binding.
			Stride = vertexBuffer->getVertexSize();
			createInternalBuffer(vertexBuffer->getVertices());
		}

		CVulkanHardwareBuffer::CVulkanHardwareBuffer(const SVulkanContext& context,
			IVulkanUploadContext& uploadContext, scene::IIndexBuffer* indexBuffer)
			: IHardwareBuffer(indexBuffer->getHardwareMappingHint(), 0,
				indexBuffer->getIndexCount() * indexBuffer->getIndexSize(),
				EHBT_INDEX, EDT_VULKAN),
			Context(context), Upload(uploadContext),
			Access(accessFromMapping(indexBuffer->getHardwareMappingHint())),
			IndexType(indexBuffer->getType())
		{
			Stride = indexBuffer->getIndexSize();
			createInternalBuffer(indexBuffer->getIndices());
		}

		CVulkanHardwareBuffer::CVulkanHardwareBuffer(const SVulkanContext& context,
			IVulkanUploadContext& uploadContext, u32 size, E_HARDWARE_BUFFER_TYPE type,
			E_HARDWARE_BUFFER_ACCESS access, u32 flags, u32 stride, const void* initialData)
			: IHardwareBuffer(mappingFromAccess(access), flags, size, type, EDT_VULKAN),
			Context(context), Upload(uploadContext), Access(access)
		{
			Stride = stride;
			createInternalBuffer(initialData);
		}

		// Unlike the D3D12 backend there is no fenced retirement queue here: the driver is
		// expected to have waited for the device to go idle before dropping its buffers, so the
		// handles can be destroyed straight away.
		CVulkanHardwareBuffer::~CVulkanHardwareBuffer()
		{
			releaseStagingBuffer();
			releaseReadbackSlots();
			releaseCounterBuffer();
			destroyInternalBuffer();
		}

		VkBuffer CVulkanHardwareBuffer::getCounterBuffer(bool create)
		{
			if (CounterBuffer != VK_NULL_HANDLE || !create || Context.Device == VK_NULL_HANDLE)
				return CounterBuffer;

			// Host-visible: resetStructureCount() writes it from the CPU and the atomics a dispatch
			// runs on it are few. 16 bytes rather than 4 keeps every minimum alignment happy.
			if (!createVulkanBuffer(Context, 16,
				VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				HostMemoryFlags, CounterBuffer, CounterMemory))
				return VK_NULL_HANDLE;

			void* mapped = 0;
			if (vulkanFailed("CVulkanHardwareBuffer: vkMapMemory (append counter)",
				vk::MapMemory(Context.Device, CounterMemory, 0, VK_WHOLE_SIZE, 0, &mapped)))
			{
				releaseCounterBuffer();
				return VK_NULL_HANDLE;
			}
			CounterMapped = static_cast<u32*>(mapped);
			memset(CounterMapped, 0, 16);
			return CounterBuffer;
		}

		bool CVulkanHardwareBuffer::setCounterValue(u32 value)
		{
			if (getCounterBuffer(true) == VK_NULL_HANDLE || !CounterMapped)
				return false;
			*CounterMapped = value;
			return true;
		}

		bool CVulkanHardwareBuffer::getCounterValue(u32& outValue) const
		{
			if (!CounterMapped)
				return false;
			outValue = *CounterMapped;
			return true;
		}

		void CVulkanHardwareBuffer::releaseCounterBuffer()
		{
			if (Context.Device == VK_NULL_HANDLE)
				return;
			if (CounterMapped)
				vk::UnmapMemory(Context.Device, CounterMemory);
			CounterMapped = nullptr;
			if (CounterBuffer != VK_NULL_HANDLE)
				vk::DestroyBuffer(Context.Device, CounterBuffer, 0);
			if (CounterMemory != VK_NULL_HANDLE)
				vk::FreeMemory(Context.Device, CounterMemory, 0);
			CounterBuffer = VK_NULL_HANDLE;
			CounterMemory = VK_NULL_HANDLE;
		}

		void CVulkanHardwareBuffer::releaseReadbackSlots()
		{
			if (Context.Device == VK_NULL_HANDLE)
				return;

			for (u32 i = 0; i < ReadbackSlotCount; ++i)
			{
				if (Readback[i].Buffer != VK_NULL_HANDLE)
					vk::DestroyBuffer(Context.Device, Readback[i].Buffer, 0);
				if (Readback[i].Memory != VK_NULL_HANDLE)
					vk::FreeMemory(Context.Device, Readback[i].Memory, 0);
				Readback[i] = SReadbackSlot();
			}
		}

		bool CVulkanHardwareBuffer::beginAsyncReadback(u32 slot)
		{
			if (slot >= ReadbackSlotCount || Buffer == VK_NULL_HANDLE || Size == 0)
				return false;

			SReadbackSlot& readback = Readback[slot];
			// A buffer grown by update() outgrows its slot copy; rebuild that one to match.
			if (readback.Buffer != VK_NULL_HANDLE && readback.Size < Size)
			{
				vk::DestroyBuffer(Context.Device, readback.Buffer, 0);
				vk::FreeMemory(Context.Device, readback.Memory, 0);
				readback = SReadbackSlot();
			}
			if (readback.Buffer == VK_NULL_HANDLE)
			{
				if (!createVulkanBuffer(Context, Size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, HostMemoryFlags,
					readback.Buffer, readback.Memory))
					return false;
				readback.Size = Size;
			}

			VkCommandBuffer commandBuffer = Upload.beginUpload();
			if (commandBuffer == VK_NULL_HANDLE)
				return false;

			// The producer is a dispatch whose barrierAfterDispatch() already made its writes visible
			// to TRANSFER; a host-written buffer needs nothing more than the copy itself.
			VkBufferCopy region = {};
			region.size = Size;
			vk::CmdCopyBuffer(commandBuffer, Buffer, readback.Buffer, 1, &region);
			recordCopyBarrier(commandBuffer, readback.Buffer, VK_ACCESS_HOST_READ_BIT,
				VK_PIPELINE_STAGE_HOST_BIT);
			Upload.endUploadAndWait(commandBuffer);

			readback.Ready = true;
			return true;
		}

		bool CVulkanHardwareBuffer::tryAsyncReadback(u32 slot, void* dst, u32 bytes, bool /*wait*/)
		{
			if (slot >= ReadbackSlotCount || !dst || bytes == 0)
				return false;

			SReadbackSlot& readback = Readback[slot];
			if (!readback.Ready || readback.Buffer == VK_NULL_HANDLE)
				return false;

			void* mapped = 0;
			if (vulkanFailed("CVulkanHardwareBuffer: vkMapMemory (readback)",
				vk::MapMemory(Context.Device, readback.Memory, 0, VK_WHOLE_SIZE, 0, &mapped)))
				return false;

			const VkDeviceSize count = (bytes < readback.Size) ? bytes : readback.Size;
			memcpy(dst, mapped, static_cast<size_t>(count));
			vk::UnmapMemory(Context.Device, readback.Memory);
			return true;
		}

		bool CVulkanHardwareBuffer::createInternalBuffer(const void* initialData)
		{
			destroyInternalBuffer();

			if (Context.Device == VK_NULL_HANDLE || Size == 0)
			{
				os::Printer::log("CVulkanHardwareBuffer: no device or zero size", ELL_ERROR);
				return false;
			}

			DeviceLocal = wantsDeviceLocal();

			VkBufferUsageFlags usage = bufferUsageFlags();
			VkMemoryPropertyFlags memoryFlags = HostMemoryFlags;
			if (DeviceLocal)
			{
				// TRANSFER_DST for the staging upload, TRANSFER_SRC so lock(true) can read back.
				usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
				memoryFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
			}

			if (!createVulkanBuffer(Context, Size, usage, memoryFlags, Buffer, Memory))
				return false;

			if (!DeviceLocal)
			{
				// Mapped once for the buffer's whole lifetime. HOST_COHERENT, so writes need no
				// vkFlushMappedMemoryRanges and the pointer stays valid until the destructor.
				VkResult result = vk::MapMemory(Context.Device, Memory, 0, VK_WHOLE_SIZE, 0, &MappedData);
				if (vulkanFailed("CVulkanHardwareBuffer: vkMapMemory", result))
				{
					MappedData = 0;
					destroyInternalBuffer();
					return false;
				}

				if (initialData)
					memcpy(MappedData, initialData, Size);
			}
			else if (initialData && !uploadThroughStaging(initialData, Size))
			{
				destroyInternalBuffer();
				return false;
			}

			RequiredUpdate = false;
			return true;
		}

		void CVulkanHardwareBuffer::destroyInternalBuffer()
		{
			if (Context.Device == VK_NULL_HANDLE)
				return;

			if (MappedData)
			{
				vk::UnmapMemory(Context.Device, Memory);
				MappedData = 0;
			}

			if (Buffer != VK_NULL_HANDLE)
			{
				vk::DestroyBuffer(Context.Device, Buffer, 0);
				Buffer = VK_NULL_HANDLE;
			}

			if (Memory != VK_NULL_HANDLE)
			{
				vk::FreeMemory(Context.Device, Memory, 0);
				Memory = VK_NULL_HANDLE;
			}
		}

		bool CVulkanHardwareBuffer::uploadThroughStaging(const void* data, u32 size)
		{
			if (Buffer == VK_NULL_HANDLE || !data || size == 0)
				return false;

			// Sized to the copy, not to the buffer: update() may push less than the allocation.
			VkBuffer staging = VK_NULL_HANDLE;
			VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
			if (!createVulkanBuffer(Context, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
				HostMemoryFlags, staging, stagingMemory))
				return false;

			void* mapped = 0;
			VkResult result = vk::MapMemory(Context.Device, stagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped);
			if (vulkanFailed("CVulkanHardwareBuffer: vkMapMemory (staging)", result))
			{
				vk::DestroyBuffer(Context.Device, staging, 0);
				vk::FreeMemory(Context.Device, stagingMemory, 0);
				return false;
			}

			memcpy(mapped, data, size);
			vk::UnmapMemory(Context.Device, stagingMemory);

			bool ok = false;
			VkCommandBuffer commandBuffer = Upload.beginUpload();
			if (commandBuffer != VK_NULL_HANDLE)
			{
				VkBufferCopy region = {};
				region.size = size;
				vk::CmdCopyBuffer(commandBuffer, staging, Buffer, 1, &region);
				recordCopyBarrier(commandBuffer, Buffer, VK_ACCESS_MEMORY_READ_BIT,
					VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

				// Blocking, like the D3D12 upload scope: the staging buffer is dead on return.
				Upload.endUploadAndWait(commandBuffer);
				ok = true;
			}

			vk::DestroyBuffer(Context.Device, staging, 0);
			vk::FreeMemory(Context.Device, stagingMemory, 0);
			return ok;
		}

		void CVulkanHardwareBuffer::releaseStagingBuffer()
		{
			if (Context.Device == VK_NULL_HANDLE)
				return;

			if (MappedStagingData)
			{
				vk::UnmapMemory(Context.Device, StagingMemory);
				MappedStagingData = 0;
			}

			if (StagingBuffer != VK_NULL_HANDLE)
			{
				vk::DestroyBuffer(Context.Device, StagingBuffer, 0);
				StagingBuffer = VK_NULL_HANDLE;
			}

			if (StagingMemory != VK_NULL_HANDLE)
			{
				vk::FreeMemory(Context.Device, StagingMemory, 0);
				StagingMemory = VK_NULL_HANDLE;
			}
		}

		void CVulkanHardwareBuffer::recordCopyBarrier(VkCommandBuffer commandBuffer, VkBuffer buffer,
			VkAccessFlags dstAccess, VkPipelineStageFlags dstStage)
		{
			// Deliberately coarse: these copies are rare and always followed by a queue wait.
			VkBufferMemoryBarrier barrier = {};
			barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = dstAccess;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.buffer = buffer;
			barrier.offset = 0;
			barrier.size = VK_WHOLE_SIZE;

			vk::CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, dstStage,
				0, 0, 0, 1, &barrier, 0, 0);
		}

		VkBufferUsageFlags CVulkanHardwareBuffer::bufferUsageFlags() const
		{
			VkBufferUsageFlags usage = 0;

			switch (Type)
			{
			case EHBT_VERTEX:
				usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
				break;
			case EHBT_INDEX:
				usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
				break;
			case EHBT_STREAM_OUTPUT:
				// Written as a storage buffer by the shader, then read back as geometry.
				usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
				break;
			case EHBT_COMPUTE:
			case EHBT_SHADER_RESOURCE:
				usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
				break;
			case EHBT_CONSTANTS:
				usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
				break;
			default:
				break; // EHBT_NONE/EHBT_SYSTEM: pure transfer buffer, handled below
			}

			// Extra bind points, same flag set CD3D11HardwareBuffer honours.
			if (Flags & EHBF_VERTEX_ADDITIONAL_BIND)
				usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
			if (Flags & EHBF_INDEX_ADDITIONAL_BIND)
				usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
			if (Flags & EHBF_SHADER_ADDITIONAL_BIND)
				usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
			if (Flags & EHBF_DRAW_INDIRECT_ARGS)
				usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

			// vkCreateBuffer rejects an empty usage mask, so a type with no bind point of its own
			// still gets the transfer bits that make it useful as a copy source/target.
			if (usage == 0)
				usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

			return usage;
		}

		bool CVulkanHardwareBuffer::wantsDeviceLocal() const
		{
			// A compute or stream-output buffer is a GPU write target and must sit in VRAM
			// whatever the access hint says -- same override as the D3D12 default-heap path.
			if (Type == EHBT_COMPUTE || Type == EHBT_STREAM_OUTPUT)
				return true;

			return Access == EHBA_IMMUTABLE || Access == EHBA_DEFAULT;
		}

		bool CVulkanHardwareBuffer::update(const scene::E_HARDWARE_MAPPING mapping, const u32 size, const void* data)
		{
			if (!data || size == 0)
				return false;

			// The hint is recorded even when the allocation is reused, as CD3D11HardwareBuffer does.
			Mapping = mapping;

			// A VkBuffer has a fixed size: growing past the allocation means destroying it and
			// building a new one, with the new content as its initial data.
			if (size > Size || Buffer == VK_NULL_HANDLE)
			{
				Size = size;
				Access = accessFromMapping(mapping);
				return createInternalBuffer(data);
			}

			if (!DeviceLocal)
			{
				if (!MappedData)
					return false;
				memcpy(MappedData, data, size);
			}
			else if (!uploadThroughStaging(data, size))
			{
				return false;
			}

			RequiredUpdate = false;
			return true;
		}

		void* CVulkanHardwareBuffer::lock(bool readOnly)
		{
			if (!DeviceLocal)
				return MappedData; // persistently mapped, nothing to do

			if (Buffer == VK_NULL_HANDLE || Size == 0 || StagingBuffer != VK_NULL_HANDLE)
				return 0;

			// One lock at a time: a second one would leak the first staging buffer.
			StagingIsReadback = readOnly;
			VkBufferUsageFlags usage = readOnly ? VK_BUFFER_USAGE_TRANSFER_DST_BIT
				: VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
			if (!createVulkanBuffer(Context, Size, usage, HostMemoryFlags, StagingBuffer, StagingMemory))
				return 0;

			if (readOnly)
			{
				// Pull the current content down first, and wait: the caller reads on return.
				VkCommandBuffer commandBuffer = Upload.beginUpload();
				if (commandBuffer == VK_NULL_HANDLE)
				{
					releaseStagingBuffer();
					return 0;
				}

				VkBufferCopy region = {};
				region.size = Size;
				vk::CmdCopyBuffer(commandBuffer, Buffer, StagingBuffer, 1, &region);
				recordCopyBarrier(commandBuffer, StagingBuffer, VK_ACCESS_HOST_READ_BIT,
					VK_PIPELINE_STAGE_HOST_BIT);
				Upload.endUploadAndWait(commandBuffer);
			}

			VkResult result = vk::MapMemory(Context.Device, StagingMemory, 0, VK_WHOLE_SIZE, 0, &MappedStagingData);
			if (vulkanFailed("CVulkanHardwareBuffer: vkMapMemory (lock)", result))
			{
				MappedStagingData = 0;
				releaseStagingBuffer();
				return 0;
			}

			return MappedStagingData;
		}

		void CVulkanHardwareBuffer::unlock()
		{
			if (!DeviceLocal || StagingBuffer == VK_NULL_HANDLE)
				return;

			if (MappedStagingData)
			{
				vk::UnmapMemory(Context.Device, StagingMemory);
				MappedStagingData = 0;
			}

			// A read lock had nothing to give back; a write lock still has to reach the GPU.
			if (!StagingIsReadback)
			{
				VkCommandBuffer commandBuffer = Upload.beginUpload();
				if (commandBuffer != VK_NULL_HANDLE)
				{
					VkBufferCopy region = {};
					region.size = Size;
					vk::CmdCopyBuffer(commandBuffer, StagingBuffer, Buffer, 1, &region);
					recordCopyBarrier(commandBuffer, Buffer, VK_ACCESS_MEMORY_READ_BIT,
						VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
					Upload.endUploadAndWait(commandBuffer);
				}
			}

			releaseStagingBuffer();
		}

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
