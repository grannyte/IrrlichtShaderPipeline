// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// Vulkan occlusion queries: one VK_QUERY_TYPE_OCCLUSION pool of fixed size, a free-slot list and one
// record per scene node -- the bookkeeping the D3D12 backend keeps. This class records no draw: it
// owns the pool, the geometry and the results, the driver draws between beginQuery() and endQuery().
//
// RESET BEFORE USE, the one real difference from D3D12: a Vulkan query must be reset before EVERY
// use, and vkCmdResetQueryPool is illegal inside a dynamic-rendering instance. Hence resetQuery()/
// resetPool() as their own calls, to be recorded BEFORE vk::CmdBeginRendering (or after
// CmdEndRendering) -- never between the two. Per frame:
//   resetQuery -> beginRendering -> beginQuery -> draw -> endQuery -> markPending -> endRendering

#ifndef __C_VULKAN_OCCLUSION_QUERY_H_INCLUDED__
#define __C_VULKAN_OCCLUSION_QUERY_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include "CVulkanHelpers.h"
#include "vector3d.h"
#include <memory>
#include <unordered_map>
#include <vector>

namespace irr
{
	namespace scene
	{
		class ISceneNode;
		class IMesh;
	}

	namespace video
	{
		//! Everything kept per registered node, mirroring the D3D12 record.
		struct SVulkanOcclusionQuery
		{
			u32 Slot = 0;
			//! Flattened local geometry, snapshotted at addQuery() and never re-read from the mesh.
			std::vector<core::vector3df> Positions;
			//! Submission that last recorded this slot, 0 = nothing in flight. It is what separates
			//! "not ready yet" from "never run" -- blocking on an unsubmitted query never returns.
			u64 PendingFrame = 0;
			u32 LastResult = 0; //!< last value read back, in visible samples
		};

		//! Takes the shared device context by const reference, like every other CVulkan* resource
		//! class, so it never has to see the driver itself.
		class CVulkanOcclusionQuery
		{
		public:
			//! Touches no device state; call create() once the context holds a live VkDevice.
			explicit CVulkanOcclusionQuery(const SVulkanContext& context);
			~CVulkanOcclusionQuery();

			CVulkanOcclusionQuery(const CVulkanOcclusionQuery&) = delete;
			CVulkanOcclusionQuery& operator=(const CVulkanOcclusionQuery&) = delete;

			//! Creates the pool and fills the free list. False (logged) leaves every call below a no-op.
			bool create();

			//! Drops the pool and every record; the caller waits for GPU idle first.
			void destroy();

			bool isValid() const { return QueryPool != VK_NULL_HANDLE; }

			//! Slots in the pool, mirroring the D3D12 query heap's capacity.
			static const u32 QueryCapacity = 256;

			// --- Registration ---

			//! Takes a slot and flattens every mesh buffer into one position-only vertex list, as the
			//! D3D12 backend does: indexed buffers expanded through their 16- or 32-bit indices,
			//! non-indexed ones copied straight. A null mesh, a repeat call and a full pool are all
			//! ignored, logged.
			void addQuery(std::shared_ptr<scene::ISceneNode> node, const scene::IMesh* mesh);

			void removeQuery(std::shared_ptr<scene::ISceneNode> node);
			void removeAll();
			bool hasQuery(const std::shared_ptr<scene::ISceneNode>& node) const;

			//! Slot to hand the recording helpers, or ~0u when the node has no query.
			u32 getSlot(const std::shared_ptr<scene::ISceneNode>& node) const;

			//! Snapshotted geometry, or 0 for an unknown node; points into the record.
			const std::vector<core::vector3df>* getPositions(const std::shared_ptr<scene::ISceneNode>& node) const;

			//! Copied out, so the caller can walk the set while it mutates.
			std::vector<std::shared_ptr<scene::ISceneNode>> getNodes() const;

			// --- Recording. No-ops on an invalid pool or an out-of-range slot. ---

			//! vkCmdResetQueryPool for one slot. MUST be recorded OUTSIDE dynamic rendering and
			//! before the matching beginQuery() -- see the file header.
			void resetQuery(VkCommandBuffer commandBuffer, u32 slot) const;

			//! The whole pool at once, for a driver clearing it at the top of the frame. Same rule.
			void resetPool(VkCommandBuffer commandBuffer) const;

			void beginQuery(VkCommandBuffer commandBuffer, u32 slot) const;
			void endQuery(VkCommandBuffer commandBuffer, u32 slot) const;

			//! Called right after endQuery(). `frameMarker` is opaque and only has to be non-zero:
			//! whatever counter the driver already bumps per submitted frame.
			void markPending(const std::shared_ptr<scene::ISceneNode>& node, u64 frameMarker);

			//! The marker markPending() stored, 0 when nothing is in flight or the node is unknown.
			//! The driver compares it with its current frame before blocking: waiting on a query
			//! recorded in the frame still being recorded would never return.
			u64 getPendingFrame(const std::shared_ptr<scene::ISceneNode>& node) const;

			// --- Readback ---

			//! vkGetQueryPoolResults with VK_QUERY_RESULT_64_BIT. Non-blocking by default: a result the
			//! GPU has not produced yet leaves the query pending and the last value untouched, the
			//! "update might not occur in this case" IVideoDriver documents. `block` adds WAIT_BIT.
			void updateResult(const std::shared_ptr<scene::ISceneNode>& node, bool block = true);
			void updateAllResults(bool block = true);

			//! Last visible-sample count, or ~0u for a node with no query -- "no query", NOT "zero
			//! pixels visible", the convention the D3D12 backend uses.
			u32 getResult(const std::shared_ptr<scene::ISceneNode>& node) const;

			//! VK_QUERY_CONTROL_PRECISE_BIT on every beginQuery(): an exact sample count instead of the
			//! zero/non-zero a plain query guarantees. Off by default, legal only on a device created
			//! with the occlusionQueryPrecise feature, which this class cannot check on its own.
			void setPreciseCounts(bool precise) { PreciseCounts = precise; }

		private:
			const SVulkanContext& Context;
			VkQueryPool QueryPool = VK_NULL_HANDLE;
			std::vector<u32> FreeSlots; //!< popped from the back, so a fresh pool hands out 0, 1, 2...
			//! Keyed on the shared_ptr itself, as the D3D12 map is: the node is identified by pointer
			//! and kept alive for as long as its query exists.
			std::unordered_map<std::shared_ptr<scene::ISceneNode>, SVulkanOcclusionQuery> Queries;
			bool PreciseCounts = false;
		};

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
#endif
