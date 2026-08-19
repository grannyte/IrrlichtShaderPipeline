// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "CVulkanImmediate.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include "os.h"
#include "irrMath.h"
#include <string.h>
#include <math.h>

namespace irr
{
	namespace video
	{
		namespace
		{
			const VkMemoryPropertyFlags RingMemoryFlags =
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

			//! Both bind points on one buffer: a single ring serves 2D quads and index data alike.
			const VkBufferUsageFlags RingUsageFlags =
				VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;

			//! Rounds up to a power-of-two boundary.
			inline VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
			{
				if (alignment <= 1)
					return value;
				return (value + alignment - 1) & ~(alignment - 1);
			}

			//! Builds the 6 vertices (2 triangles) of a screen quad, corners coloured/textured
			//! independently (order: top-left, top-right, bottom-left, bottom-right). Same layout as
			//! the D3D12 buildQuadVertices() so both backends emit identical geometry.
			void buildQuadVertices(S3DVertex out[6], const core::rect<s32>& destRect,
				const core::rect<f32>& uvRect, SColor colorUL, SColor colorUR, SColor colorLL, SColor colorLR)
			{
				const f32 x0 = f32(destRect.UpperLeftCorner.X), y0 = f32(destRect.UpperLeftCorner.Y);
				const f32 x1 = f32(destRect.LowerRightCorner.X), y1 = f32(destRect.LowerRightCorner.Y);
				out[0] = S3DVertex(x0, y0, 0, 0, 0, 0, colorUL, uvRect.UpperLeftCorner.X, uvRect.UpperLeftCorner.Y);
				out[1] = S3DVertex(x1, y0, 0, 0, 0, 0, colorUR, uvRect.LowerRightCorner.X, uvRect.UpperLeftCorner.Y);
				out[2] = S3DVertex(x0, y1, 0, 0, 0, 0, colorLL, uvRect.UpperLeftCorner.X, uvRect.LowerRightCorner.Y);
				out[3] = out[1];
				out[4] = S3DVertex(x1, y1, 0, 0, 0, 0, colorLR, uvRect.LowerRightCorner.X, uvRect.LowerRightCorner.Y);
				out[5] = out[2];
			}

			//! Cuts destRect down to clipRect and slides the UV rectangle by the same proportion, so
			//! the visible part of the image keeps sampling exactly the texels it would have without
			//! clipping. False means nothing survived the intersection.
			bool clipQuad(core::rect<s32>& destRect, core::rect<f32>& uvRect, const core::rect<s32>* clipRect)
			{
				if (!clipRect)
					return destRect.getWidth() > 0 && destRect.getHeight() > 0;

				const f32 fullW = f32(destRect.getWidth());
				const f32 fullH = f32(destRect.getHeight());
				if (fullW <= 0.0f || fullH <= 0.0f)
					return false;

				core::rect<s32> clipped = destRect;
				clipped.clipAgainst(*clipRect);
				if (clipped.getWidth() <= 0 || clipped.getHeight() <= 0)
					return false;

				const f32 uSpan = uvRect.LowerRightCorner.X - uvRect.UpperLeftCorner.X;
				const f32 vSpan = uvRect.LowerRightCorner.Y - uvRect.UpperLeftCorner.Y;
				const f32 u0 = uvRect.UpperLeftCorner.X;
				const f32 v0 = uvRect.UpperLeftCorner.Y;

				uvRect.UpperLeftCorner.X = u0 + uSpan * (f32(clipped.UpperLeftCorner.X - destRect.UpperLeftCorner.X) / fullW);
				uvRect.UpperLeftCorner.Y = v0 + vSpan * (f32(clipped.UpperLeftCorner.Y - destRect.UpperLeftCorner.Y) / fullH);
				uvRect.LowerRightCorner.X = u0 + uSpan * (f32(clipped.LowerRightCorner.X - destRect.UpperLeftCorner.X) / fullW);
				uvRect.LowerRightCorner.Y = v0 + vSpan * (f32(clipped.LowerRightCorner.Y - destRect.UpperLeftCorner.Y) / fullH);

				destRect = clipped;
				return true;
			}

			//! Bytes per index, also the alignment vk::CmdBindIndexBuffer demands of the offset.
			VkDeviceSize indexSizeOf(VkIndexType indexType)
			{
				switch (indexType)
				{
				case VK_INDEX_TYPE_UINT16: return 2;
				case VK_INDEX_TYPE_UINT8_KHR: return 1;
				default: return 4; // VK_INDEX_TYPE_UINT32
				}
			}
		}

		// ================================ transient ring ================================

		CVulkanImmediateRing::CVulkanImmediateRing(const SVulkanContext& context, VkDeviceSize initialSize)
			: Context(context), InitialSize(initialSize ? initialSize : DefaultRingSize)
		{
		}

		CVulkanImmediateRing::~CVulkanImmediateRing()
		{
			destroy();
		}

		bool CVulkanImmediateRing::init()
		{
			if (Buffer != VK_NULL_HANDLE)
				return true;

			if (!createRingBuffer(InitialSize, Buffer, Memory, Mapped))
				return false;

			Capacity = InitialSize;
			Cursor = 0;
			return true;
		}

		void CVulkanImmediateRing::destroy()
		{
			destroyRetired();

			if (Memory != VK_NULL_HANDLE)
			{
				if (Mapped)
					vk::UnmapMemory(Context.Device, Memory);
				vk::FreeMemory(Context.Device, Memory, 0);
			}
			if (Buffer != VK_NULL_HANDLE)
				vk::DestroyBuffer(Context.Device, Buffer, 0);

			Buffer = VK_NULL_HANDLE;
			Memory = VK_NULL_HANDLE;
			Mapped = 0;
			Capacity = 0;
			Cursor = 0;
		}

		bool CVulkanImmediateRing::createRingBuffer(VkDeviceSize size, VkBuffer& outBuffer,
			VkDeviceMemory& outMemory, u8*& outMapped)
		{
			VkBuffer buffer = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			if (!createVulkanBuffer(Context, size, RingUsageFlags, RingMemoryFlags, buffer, memory))
				return false;

			// Mapped once and never unmapped: HOST_COHERENT, so writes are visible to the GPU
			// without an explicit flush, exactly like the D3D12 upload heap kept permanently mapped.
			void* mapped = 0;
			const VkResult result = vk::MapMemory(Context.Device, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
			if (vulkanFailed("CVulkanImmediateRing: mapping the transient ring", result))
			{
				vk::DestroyBuffer(Context.Device, buffer, 0);
				vk::FreeMemory(Context.Device, memory, 0);
				return false;
			}

			outBuffer = buffer;
			outMemory = memory;
			outMapped = static_cast<u8*>(mapped);
			return true;
		}

		bool CVulkanImmediateRing::grow(VkDeviceSize minCapacity)
		{
			// Unconditional doubling, same reason as CD3D12Driver::growVertexRing(): allocate()
			// passes the size of the single allocation that just failed, not the frame's total.
			VkDeviceSize newCapacity = Capacity ? Capacity : InitialSize;
			newCapacity *= 2;
			while (newCapacity < minCapacity)
				newCapacity *= 2;

			VkBuffer newBuffer = VK_NULL_HANDLE;
			VkDeviceMemory newMemory = VK_NULL_HANDLE;
			u8* newMapped = 0;
			if (!createRingBuffer(newCapacity, newBuffer, newMemory, newMapped))
			{
				os::Printer::log("CVulkanImmediateRing: growing the transient ring failed", ELL_ERROR);
				return false;
			}

			// The old buffer is retired, not destroyed: offsets already returned this frame still
			// point into it and the command buffer still references them. See the lifetime rule on
			// the class -- destroyRetired() only runs on reset(true).
			if (Buffer != VK_NULL_HANDLE)
			{
				SRetiredBuffer retired;
				retired.Buffer = Buffer;
				retired.Memory = Memory;
				Retired.push_back(retired);
			}

			Buffer = newBuffer;
			Memory = newMemory;
			Mapped = newMapped;
			Capacity = newCapacity;
			Cursor = 0;

			os::Printer::log("CVulkanImmediateRing: transient ring grown for this frame", ELL_INFORMATION);
			return true;
		}

		void CVulkanImmediateRing::destroyRetired()
		{
			for (u32 i = 0; i < Retired.size(); ++i)
			{
				if (Retired[i].Memory != VK_NULL_HANDLE)
				{
					vk::UnmapMemory(Context.Device, Retired[i].Memory);
					vk::FreeMemory(Context.Device, Retired[i].Memory, 0);
				}
				if (Retired[i].Buffer != VK_NULL_HANDLE)
					vk::DestroyBuffer(Context.Device, Retired[i].Buffer, 0);
			}
			Retired.clear();
		}

		SVulkanTransientRange CVulkanImmediateRing::allocate(const void* data, VkDeviceSize sizeBytes,
			VkDeviceSize alignment)
		{
			SVulkanTransientRange range;
			if (!data || !sizeBytes)
				return range;

			if (Buffer == VK_NULL_HANDLE && !init())
				return range;

			VkDeviceSize offset = alignUp(Cursor, alignment);
			if (offset + sizeBytes > Capacity)
			{
				if (!grow(offset + sizeBytes))
				{
					os::Printer::log("CVulkanImmediateRing: transient ring full for this frame and"
						" growing it failed, draw skipped", ELL_WARNING);
					return range;
				}
				offset = alignUp(Cursor, alignment); // Cursor is back at 0 in the new buffer
			}

			memcpy(Mapped + offset, data, static_cast<size_t>(sizeBytes));
			Cursor = offset + sizeBytes;

			range.Buffer = Buffer;
			range.Offset = offset;
			range.SizeBytes = sizeBytes;
			return range;
		}

		SVulkanTransientRange CVulkanImmediateRing::allocateVertices(const void* data, u32 count, u32 stride)
		{
			SVulkanTransientRange range;
			if (!count || !stride)
				return range;

			// 16 bytes covers every scalar attribute alignment of the built-in vertex layouts, and
			// keeps a fetch from straddling a cache line more often than it must.
			return allocate(data, static_cast<VkDeviceSize>(count) * stride, 16);
		}

		SVulkanTransientRange CVulkanImmediateRing::allocateIndices(const void* data, u32 count,
			VkIndexType indexType)
		{
			SVulkanTransientRange range;
			if (!count)
				return range;

			const VkDeviceSize indexSize = indexSizeOf(indexType);
			return allocate(data, static_cast<VkDeviceSize>(count) * indexSize, indexSize);
		}

		void CVulkanImmediateRing::reset(bool retiredBuffersAreFree)
		{
			Cursor = 0;
			if (retiredBuffersAreFree)
				destroyRetired();
		}

		// ================================ geometry builders ================================

		core::vector3df CVulkanImmediateGeometry::screenToClip(f32 x, f32 y,
			const core::dimension2d<u32>& renderTargetSize, f32 z)
		{
			const f32 w = f32(renderTargetSize.Width);
			const f32 h = f32(renderTargetSize.Height);
			if (w <= 0.0f || h <= 0.0f)
				return core::vector3df(0, 0, 0);

			// Pre-flip clip space (+y up, top of screen at +1): the built-in vertex shaders apply
			// "clipPos.y = -clipPos.y" afterwards, which lands this on Vulkan's +y-down NDC.
			// Algebraically identical to multiplying by build2DProjection().
			return core::vector3df(x * 2.0f / w - 1.0f, 1.0f - y * 2.0f / h, z * 0.5f + 0.5f);
		}

		core::matrix4 CVulkanImmediateGeometry::build2DProjection(const core::dimension2d<u32>& renderTargetSize)
		{
			core::matrix4 m;
			m.buildProjectionMatrixOrthoLH(f32(renderTargetSize.Width),
				f32(-static_cast<s32>(renderTargetSize.Height)), -1.0f, 1.0f);
			m.setTranslation(core::vector3df(-1, 1, 0));
			return m;
		}

		core::rect<f32> CVulkanImmediateGeometry::toUVRect(const core::rect<s32>& sourceRect,
			const core::dimension2d<u32>& textureSize)
		{
			if (textureSize.Width == 0 || textureSize.Height == 0)
				return core::rect<f32>(0, 0, 1, 1);
			return core::rect<f32>(
				f32(sourceRect.UpperLeftCorner.X) / textureSize.Width,
				f32(sourceRect.UpperLeftCorner.Y) / textureSize.Height,
				f32(sourceRect.LowerRightCorner.X) / textureSize.Width,
				f32(sourceRect.LowerRightCorner.Y) / textureSize.Height);
		}

		VkPrimitiveTopology CVulkanImmediateGeometry::build2DImageQuad(core::array<S3DVertex>& out,
			const core::rect<s32>& destRect, const core::rect<s32>& sourceRect,
			const core::dimension2d<u32>& textureSize, SColor color, const core::rect<s32>* clipRect)
		{
			return build2DImageQuad(out, destRect, sourceRect, textureSize, color, color, color, color, clipRect);
		}

		VkPrimitiveTopology CVulkanImmediateGeometry::build2DImageQuad(core::array<S3DVertex>& out,
			const core::rect<s32>& destRect, const core::rect<s32>& sourceRect,
			const core::dimension2d<u32>& textureSize, SColor colorUL, SColor colorUR,
			SColor colorLL, SColor colorLR, const core::rect<s32>* clipRect)
		{
			out.set_used(0);

			core::rect<s32> dest = destRect;
			core::rect<f32> uv = toUVRect(sourceRect, textureSize);
			if (!clipQuad(dest, uv, clipRect))
				return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

			S3DVertex verts[6];
			buildQuadVertices(verts, dest, uv, colorUL, colorUR, colorLL, colorLR);
			for (u32 i = 0; i < 6; ++i)
				out.push_back(verts[i]);

			return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		}

		VkPrimitiveTopology CVulkanImmediateGeometry::build2DRectangle(core::array<S3DVertex>& out,
			const core::rect<s32>& pos, SColor colorUL, SColor colorUR, SColor colorLL, SColor colorLR,
			const core::rect<s32>* clipRect)
		{
			out.set_used(0);

			// The full [0,1] UV range is what the D3D12 version writes for an untextured rectangle;
			// keeping it means the same shader path works whether or not a texture is bound.
			core::rect<s32> dest = pos;
			core::rect<f32> uv(0, 0, 1, 1);
			if (!clipQuad(dest, uv, clipRect))
				return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

			S3DVertex verts[6];
			buildQuadVertices(verts, dest, uv, colorUL, colorUR, colorLL, colorLR);
			for (u32 i = 0; i < 6; ++i)
				out.push_back(verts[i]);

			return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		}

		VkPrimitiveTopology CVulkanImmediateGeometry::build2DLine(core::array<S3DVertex>& out,
			const core::position2d<s32>& start, const core::position2d<s32>& end, SColor color)
		{
			out.set_used(0);
			out.push_back(S3DVertex(f32(start.X), f32(start.Y), 0, 0, 0, 0, color, 0, 0));
			out.push_back(S3DVertex(f32(end.X), f32(end.Y), 0, 0, 0, 0, color, 0, 0));
			return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
		}

		VkPrimitiveTopology CVulkanImmediateGeometry::build2DRectangleOutline(core::array<S3DVertex>& out,
			const core::rect<s32>& pos, SColor color)
		{
			out.set_used(0);

			const f32 x0 = f32(pos.UpperLeftCorner.X), y0 = f32(pos.UpperLeftCorner.Y);
			const f32 x1 = f32(pos.LowerRightCorner.X), y1 = f32(pos.LowerRightCorner.Y);
			const S3DVertex verts[8] = {
				S3DVertex(x0, y0, 0, 0, 0, 0, color, 0, 0), S3DVertex(x1, y0, 0, 0, 0, 0, color, 0, 0),
				S3DVertex(x1, y0, 0, 0, 0, 0, color, 0, 0), S3DVertex(x1, y1, 0, 0, 0, 0, color, 0, 0),
				S3DVertex(x1, y1, 0, 0, 0, 0, color, 0, 0), S3DVertex(x0, y1, 0, 0, 0, 0, color, 0, 0),
				S3DVertex(x0, y1, 0, 0, 0, 0, color, 0, 0), S3DVertex(x0, y0, 0, 0, 0, 0, color, 0, 0)
			};
			for (u32 i = 0; i < 8; ++i)
				out.push_back(verts[i]);

			return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
		}

		VkPrimitiveTopology CVulkanImmediateGeometry::build2DPolygon(core::array<S3DVertex>& out,
			const core::position2d<s32>& center, f32 radius, SColor color, s32 vertexCount)
		{
			out.set_used(0);
			if (vertexCount < 2)
				return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

			out.reallocate(static_cast<u32>(vertexCount) * 2);
			for (s32 i = 0; i < vertexCount; ++i)
			{
				// One independent segment per edge (not a strip): identical vertex stream to the
				// D3D12 draw2DPolygon(), which closes the loop with the modulo below.
				const f32 a0 = (2.0f * core::PI * i) / vertexCount;
				const f32 a1 = (2.0f * core::PI * ((i + 1) % vertexCount)) / vertexCount;
				out.push_back(S3DVertex(center.X + radius * cosf(a0), center.Y + radius * sinf(a0),
					0, 0, 0, 0, color, 0, 0));
				out.push_back(S3DVertex(center.X + radius * cosf(a1), center.Y + radius * sinf(a1),
					0, 0, 0, 0, color, 0, 0));
			}

			return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
		}

		VkPrimitiveTopology CVulkanImmediateGeometry::build3DLine(core::array<S3DVertex>& out,
			const core::vector3df& start, const core::vector3df& end, SColor color)
		{
			out.set_used(0);
			out.push_back(S3DVertex(start.X, start.Y, start.Z, 0, 0, 0, color, 0, 0));
			out.push_back(S3DVertex(end.X, end.Y, end.Z, 0, 0, 0, color, 0, 0));
			return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
		}

		VkPrimitiveTopology CVulkanImmediateGeometry::build3DBox(core::array<S3DVertex>& out,
			const core::aabbox3d<f32>& box, SColor color)
		{
			out.set_used(0);

			const core::vector3df& mn = box.MinEdge;
			const core::vector3df& mx = box.MaxEdge;
			const core::vector3df v[8] = {
				core::vector3df(mn.X, mn.Y, mn.Z), core::vector3df(mx.X, mn.Y, mn.Z),
				core::vector3df(mx.X, mx.Y, mn.Z), core::vector3df(mn.X, mx.Y, mn.Z),
				core::vector3df(mn.X, mn.Y, mx.Z), core::vector3df(mx.X, mn.Y, mx.Z),
				core::vector3df(mx.X, mx.Y, mx.Z), core::vector3df(mn.X, mx.Y, mx.Z)
			};
			static const u32 edges[12][2] = {
				{0,1},{1,2},{2,3},{3,0}, // Z=min face
				{4,5},{5,6},{6,7},{7,4}, // Z=max face
				{0,4},{1,5},{2,6},{3,7}  // vertical edges connecting the two faces
			};

			out.reallocate(24);
			for (u32 i = 0; i < 12; ++i)
			{
				const core::vector3df& a = v[edges[i][0]];
				const core::vector3df& b = v[edges[i][1]];
				out.push_back(S3DVertex(a.X, a.Y, a.Z, 0, 0, 0, color, 0, 0));
				out.push_back(S3DVertex(b.X, b.Y, b.Z, 0, 0, 0, color, 0, 0));
			}

			return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
		}

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
