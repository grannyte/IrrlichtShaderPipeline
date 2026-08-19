// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#ifndef __C_VULKAN_IMMEDIATE_H_INCLUDED__
#define __C_VULKAN_IMMEDIATE_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include "CVulkanHelpers.h"
#include "S3DVertex.h"
#include "SColor.h"
#include "rect.h"
#include "position2d.h"
#include "aabbox3d.h"
#include "matrix4.h"
#include "dimension2d.h"
#include "irrArray.h"

namespace irr
{
	namespace video
	{

		//! Where one transient allocation landed, for vk::CmdBindVertexBuffers/CmdBindIndexBuffer.
		//! The buffer travels with the offset: a growing ring swaps its VkBuffer mid-frame.
		struct SVulkanTransientRange
		{
			VkBuffer Buffer = VK_NULL_HANDLE;
			VkDeviceSize Offset = 0;
			VkDeviceSize SizeBytes = 0; // 0 when the allocation failed

			bool isValid() const { return Buffer != VK_NULL_HANDLE && SizeBytes != 0; }
		};

		//! Per-frame transient vertex/index ring, the Vulkan counterpart of the SD3D12FrameContext
		//! vertex ring (CD3D12Driver::allocateVertices/growVertexRing): a HOST_VISIBLE|HOST_COHERENT
		//! VkBuffer mapped for its whole lifetime, sub-allocated by a bump cursor. One instance per
		//! frame-in-flight slot, used by that slot's single recording thread.
		//!
		//! BUFFER LIFETIME RULE. An allocation that does not fit never wraps over data already handed
		//! out: the ring creates a larger buffer, retires the current one and restarts the cursor at
		//! 0, so offsets returned earlier in the frame stay valid inside the retired buffer. Retired
		//! buffers die only on reset(true), the owner's promise that the GPU is done with everything
		//! recorded here; reset(false) keeps them one more frame, which is safe and only costs memory.
		class CVulkanImmediateRing
		{
		public:
			//! Starting capacity only, same budget as CD3D12Driver::VertexRingSizePerFrame.
			static const VkDeviceSize DefaultRingSize = 2 * 1024 * 1024;

			//! `context` must outlive this object. Nothing is allocated until init().
			CVulkanImmediateRing(const SVulkanContext& context, VkDeviceSize initialSize = DefaultRingSize);

			~CVulkanImmediateRing();

			bool init(); // creates and maps the first buffer; false (logged) on failure

			//! Destroys every buffer; the GPU must be idle, as for reset(true). Idempotent.
			void destroy();

			//! Copies count*stride bytes in; an invalid range (logged) means growth failed, skip the draw.
			SVulkanTransientRange allocateVertices(const void* data, u32 count, u32 stride);

			//! Same for indices; the offset is aligned to the index size, as CmdBindIndexBuffer wants.
			SVulkanTransientRange allocateIndices(const void* data, u32 count, VkIndexType indexType);

			//! Raw form behind both of the above; `alignment` must be a power of two.
			SVulkanTransientRange allocate(const void* data, VkDeviceSize sizeBytes, VkDeviceSize alignment);

			//! Frame start: rewinds the cursor. Pass true only per the lifetime rule above.
			void reset(bool retiredBuffersAreFree);

			VkBuffer getBuffer() const { return Buffer; } // changes on growth: prefer allocate*()'s
			VkDeviceSize getCapacity() const { return Capacity; }
			VkDeviceSize getUsed() const { return Cursor; }
			u32 getRetiredCount() const { return Retired.size(); }

		private:
			//! A ring buffer replaced by a bigger one, still referenced by recorded commands.
			struct SRetiredBuffer
			{
				VkBuffer Buffer = VK_NULL_HANDLE;
				VkDeviceMemory Memory = VK_NULL_HANDLE;
			};

			CVulkanImmediateRing(const CVulkanImmediateRing&);            // non-copyable: owns handles
			CVulkanImmediateRing& operator=(const CVulkanImmediateRing&);

			//! Vertex+index capable buffer, mapped for its whole lifetime (HOST_COHERENT, no flush).
			bool createRingBuffer(VkDeviceSize size, VkBuffer& outBuffer,
				VkDeviceMemory& outMemory, u8*& outMapped);

			//! Doubles the capacity until it covers minCapacity, retires the old buffer, cursor to 0.
			bool grow(VkDeviceSize minCapacity);

			void destroyRetired();

			const SVulkanContext& Context;

			VkBuffer Buffer = VK_NULL_HANDLE;
			VkDeviceMemory Memory = VK_NULL_HANDLE;
			u8* Mapped = 0;
			VkDeviceSize Capacity = 0;
			VkDeviceSize Cursor = 0; // bump cursor, rewound by reset()
			VkDeviceSize InitialSize = DefaultRingSize;

			core::array<SRetiredBuffer> Retired;
		};

		//! Stateless builders for the driver's immediate 2D/3D entry points, mirroring the
		//! CD3D12Driver bodies vertex for vertex. Each build*() fills `out` (cleared first) and
		//! returns the topology; an empty `out` means "fully clipped, nothing to draw".
		//!
		//! 2D CONVENTION. Positions come out in SCREEN PIXELS, origin top-left, +y down -- the
		//! engine's own 2D space, untouched, like the D3D12 builders; the driver feeds them through
		//! build2DProjection(), world and view left identity. That matrix yields a D3D-style clip
		//! position (+y UP, top of screen at clip y = +1); the built-in vertex shaders then apply
		//! "clipPos.y = -clipPos.y", landing it on Vulkan's +y-down NDC -- hence the driver's
		//! POSITIVE-height VkViewport. screenToClip() produces that same PRE-flip position.
		class CVulkanImmediateGeometry
		{
		public:
			//! Screen pixel -> pre-flip clip position; `z` is mapped [-1,1] -> [0,1] as the matrix does.
			static core::vector3df screenToClip(f32 x, f32 y,
				const core::dimension2d<u32>& renderTargetSize, f32 z = 0.0f);

			//! Screen pixels -> the same space, same formula as CD3D12Driver::build2DProjection().
			static core::matrix4 build2DProjection(const core::dimension2d<u32>& renderTargetSize);

			//! Texture-pixel rectangle -> normalized UVs ([0,1] for a zero-sized texture).
			static core::rect<f32> toUVRect(const core::rect<s32>& sourceRect,
				const core::dimension2d<u32>& textureSize);

			//! draw2DImage: 6 vertices (2 triangles), single tint. `clipRect` is intersected with
			//! `destRect` GEOMETRICALLY and the UVs re-mapped, so it is right with or without scissor.
			static VkPrimitiveTopology build2DImageQuad(core::array<S3DVertex>& out,
				const core::rect<s32>& destRect, const core::rect<s32>& sourceRect,
				const core::dimension2d<u32>& textureSize, SColor color,
				const core::rect<s32>* clipRect = 0);

			//! Same with draw2DImage(destRect, colors[4])'s corner colours, not re-interpolated on clip.
			static VkPrimitiveTopology build2DImageQuad(core::array<S3DVertex>& out,
				const core::rect<s32>& destRect, const core::rect<s32>& sourceRect,
				const core::dimension2d<u32>& textureSize, SColor colorUL, SColor colorUR,
				SColor colorLL, SColor colorLR, const core::rect<s32>* clipRect = 0);

			//! draw2DRectangle: 6 untextured vertices, one colour per corner.
			static VkPrimitiveTopology build2DRectangle(core::array<S3DVertex>& out,
				const core::rect<s32>& pos, SColor colorUL, SColor colorUR,
				SColor colorLL, SColor colorLR, const core::rect<s32>* clipRect = 0);

			//! The line-list family, always independent segments (never a strip) so the vertex stream
			//! matches D3D12: draw2DLine 2 vertices, draw2DRectangleOutline 8 for the 4 edges,
			//! draw2DPolygon 2 per segment, draw3DLine 2, draw3DBox 24 for the 12 edges. The two 3D
			//! ones stay in world space -- no 2D transform, the caller's matrices apply.
			static VkPrimitiveTopology build2DLine(core::array<S3DVertex>& out,
				const core::position2d<s32>& start, const core::position2d<s32>& end, SColor color);

			static VkPrimitiveTopology build2DRectangleOutline(core::array<S3DVertex>& out,
				const core::rect<s32>& pos, SColor color);

			static VkPrimitiveTopology build2DPolygon(core::array<S3DVertex>& out,
				const core::position2d<s32>& center, f32 radius, SColor color, s32 vertexCount);

			static VkPrimitiveTopology build3DLine(core::array<S3DVertex>& out,
				const core::vector3df& start, const core::vector3df& end, SColor color);

			static VkPrimitiveTopology build3DBox(core::array<S3DVertex>& out,
				const core::aabbox3d<f32>& box, SColor color);
		};

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
#endif
