// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "CVulkanOcclusionQuery.h"
#ifdef _IRR_COMPILE_WITH_VULKAN_

#include "IMesh.h"
#include "IMeshBuffer.h"
#include "IVertexBuffer.h"
#include "IIndexBuffer.h"
#include "S3DVertex.h"
#include "os.h"

namespace irr
{
	namespace video
	{
		CVulkanOcclusionQuery::CVulkanOcclusionQuery(const SVulkanContext& context)
			: Context(context)
		{
		}

		CVulkanOcclusionQuery::~CVulkanOcclusionQuery()
		{
			destroy();
		}

		bool CVulkanOcclusionQuery::create()
		{
			if (QueryPool != VK_NULL_HANDLE)
				return true;
			if (Context.Device == VK_NULL_HANDLE || !vk::CreateQueryPool)
			{
				os::Printer::log("CVulkanOcclusionQuery: no Vulkan device to create the query pool on", ELL_ERROR);
				return false;
			}

			VkQueryPoolCreateInfo poolInfo = {};
			poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
			poolInfo.queryType = VK_QUERY_TYPE_OCCLUSION;
			poolInfo.queryCount = QueryCapacity;

			VkResult result = vk::CreateQueryPool(Context.Device, &poolInfo, nullptr, &QueryPool);
			if (vulkanFailed("CVulkanOcclusionQuery: vkCreateQueryPool (occlusion)", result))
			{
				QueryPool = VK_NULL_HANDLE;
				return false;
			}

			// Handed out from the back, so the first node registered lands on slot 0.
			FreeSlots.clear();
			FreeSlots.reserve(QueryCapacity);
			for (u32 i = 0; i < QueryCapacity; ++i)
				FreeSlots.push_back(QueryCapacity - 1 - i);

			return true;
		}

		void CVulkanOcclusionQuery::destroy()
		{
			Queries.clear();
			FreeSlots.clear();

			if (QueryPool != VK_NULL_HANDLE)
			{
				if (vk::DestroyQueryPool && Context.Device != VK_NULL_HANDLE)
					vk::DestroyQueryPool(Context.Device, QueryPool, nullptr);
				QueryPool = VK_NULL_HANDLE;
			}
		}

		// ================================ Registration ================================

		void CVulkanOcclusionQuery::addQuery(std::shared_ptr<scene::ISceneNode> node, const scene::IMesh* mesh)
		{
			if (!node)
				return;
			if (!mesh)
			{
				os::Printer::log("CVulkanOcclusionQuery::addQuery: a null mesh is not supported"
					" (the node's mesh is not looked up automatically)", ELL_WARNING);
				return;
			}
			if (Queries.find(node) != Queries.end())
				return; // already registered

			if (FreeSlots.empty())
			{
				os::Printer::log("CVulkanOcclusionQuery::addQuery: occlusion query capacity reached"
					" (see QueryCapacity)", ELL_WARNING);
				return;
			}

			SVulkanOcclusionQuery query;
			query.Slot = FreeSlots.back();
			FreeSlots.pop_back();

			// Every mesh buffer flattened into one position-only triangle list, indices expanded:
			// the driver draws it non-indexed, and the per-buffer materials are deliberately dropped
			// (the query volume is drawn with the driver's own occlusion material).
			for (u32 mbIdx = 0; mbIdx < mesh->getMeshBufferCount(); ++mbIdx)
			{
				scene::IMeshBuffer* buffer = mesh->getMeshBuffer(mbIdx);
				if (!buffer || buffer->getVertexBufferCount() == 0)
					continue;
				scene::IVertexBuffer* vb = buffer->getVertexBuffer(0);
				scene::IIndexBuffer* ib = buffer->getIndexBuffer();
				if (!vb || vb->getVertexCount() == 0)
					continue;
				const S3DVertex* verts = static_cast<const S3DVertex*>(vb->getVertices());

				if (ib && ib->getIndexCount() > 0)
				{
					if (ib->getType() == EIT_32BIT)
					{
						const u32* idx = static_cast<const u32*>(ib->getIndices());
						for (u32 i = 0; i < ib->getIndexCount(); ++i)
							query.Positions.push_back(verts[idx[i]].Pos);
					}
					else
					{
						const u16* idx = static_cast<const u16*>(ib->getIndices());
						for (u32 i = 0; i < ib->getIndexCount(); ++i)
							query.Positions.push_back(verts[idx[i]].Pos);
					}
				}
				else
				{
					for (u32 i = 0; i < vb->getVertexCount(); ++i)
						query.Positions.push_back(verts[i].Pos);
				}
			}

			Queries[node] = std::move(query);
		}

		void CVulkanOcclusionQuery::removeQuery(std::shared_ptr<scene::ISceneNode> node)
		{
			auto it = Queries.find(node);
			if (it == Queries.end())
				return;
			FreeSlots.push_back(it->second.Slot);
			Queries.erase(it);
		}

		void CVulkanOcclusionQuery::removeAll()
		{
			for (auto& kv : Queries)
				FreeSlots.push_back(kv.second.Slot);
			Queries.clear();
		}

		bool CVulkanOcclusionQuery::hasQuery(const std::shared_ptr<scene::ISceneNode>& node) const
		{
			return Queries.find(node) != Queries.end();
		}

		u32 CVulkanOcclusionQuery::getSlot(const std::shared_ptr<scene::ISceneNode>& node) const
		{
			auto it = Queries.find(node);
			if (it == Queries.end())
				return ~0u;
			return it->second.Slot;
		}

		const std::vector<core::vector3df>* CVulkanOcclusionQuery::getPositions(
			const std::shared_ptr<scene::ISceneNode>& node) const
		{
			auto it = Queries.find(node);
			if (it == Queries.end())
				return nullptr;
			return &it->second.Positions;
		}

		std::vector<std::shared_ptr<scene::ISceneNode>> CVulkanOcclusionQuery::getNodes() const
		{
			std::vector<std::shared_ptr<scene::ISceneNode>> nodes;
			nodes.reserve(Queries.size());
			for (const auto& kv : Queries)
				nodes.push_back(kv.first);
			return nodes;
		}

		// ================================ Recording ================================

		void CVulkanOcclusionQuery::resetQuery(VkCommandBuffer commandBuffer, u32 slot) const
		{
			// Mandatory before every begin, and illegal inside a dynamic-rendering instance --
			// see the header. The driver records this before vk::CmdBeginRendering.
			if (QueryPool == VK_NULL_HANDLE || commandBuffer == VK_NULL_HANDLE ||
				slot >= QueryCapacity || !vk::CmdResetQueryPool)
				return;
			vk::CmdResetQueryPool(commandBuffer, QueryPool, slot, 1);
		}

		void CVulkanOcclusionQuery::resetPool(VkCommandBuffer commandBuffer) const
		{
			if (QueryPool == VK_NULL_HANDLE || commandBuffer == VK_NULL_HANDLE || !vk::CmdResetQueryPool)
				return;
			vk::CmdResetQueryPool(commandBuffer, QueryPool, 0, QueryCapacity);
		}

		void CVulkanOcclusionQuery::beginQuery(VkCommandBuffer commandBuffer, u32 slot) const
		{
			if (QueryPool == VK_NULL_HANDLE || commandBuffer == VK_NULL_HANDLE ||
				slot >= QueryCapacity || !vk::CmdBeginQuery)
				return;
			// PRECISE only when the driver confirmed the feature is enabled, otherwise the result is
			// merely "non-zero if anything was visible" -- enough for a binary visibility test.
			const VkQueryControlFlags flags = PreciseCounts ? VK_QUERY_CONTROL_PRECISE_BIT : 0;
			vk::CmdBeginQuery(commandBuffer, QueryPool, slot, flags);
		}

		void CVulkanOcclusionQuery::endQuery(VkCommandBuffer commandBuffer, u32 slot) const
		{
			if (QueryPool == VK_NULL_HANDLE || commandBuffer == VK_NULL_HANDLE ||
				slot >= QueryCapacity || !vk::CmdEndQuery)
				return;
			vk::CmdEndQuery(commandBuffer, QueryPool, slot);
		}

		void CVulkanOcclusionQuery::markPending(const std::shared_ptr<scene::ISceneNode>& node, u64 frameMarker)
		{
			auto it = Queries.find(node);
			if (it == Queries.end())
				return;
			it->second.PendingFrame = frameMarker ? frameMarker : 1; // 0 is reserved for "not in flight"
			it->second.EverRecorded = true;
		}

		bool CVulkanOcclusionQuery::hasRun(const std::shared_ptr<scene::ISceneNode>& node) const
		{
			auto it = Queries.find(node);
			return it != Queries.end() && it->second.EverRecorded;
		}

		u64 CVulkanOcclusionQuery::getPendingFrame(const std::shared_ptr<scene::ISceneNode>& node) const
		{
			auto it = Queries.find(node);
			return (it == Queries.end()) ? 0 : it->second.PendingFrame;
		}

		// ================================ Readback ================================

		void CVulkanOcclusionQuery::updateResult(const std::shared_ptr<scene::ISceneNode>& node, bool block)
		{
			if (!node || QueryPool == VK_NULL_HANDLE || !vk::GetQueryPoolResults)
				return;
			auto it = Queries.find(node);
			if (it == Queries.end())
				return;
			SVulkanOcclusionQuery& q = it->second;
			if (q.PendingFrame == 0)
				return; // never recorded; WAIT_BIT on an unsubmitted query would never return

			VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT;
			if (block)
				flags |= VK_QUERY_RESULT_WAIT_BIT;

			u64 samples = 0;
			VkResult result = vk::GetQueryPoolResults(Context.Device, QueryPool, q.Slot, 1,
				sizeof(samples), &samples, sizeof(samples), flags);

			// VK_NOT_READY without WAIT_BIT: the GPU has not reached the query yet. Keep the previous
			// result and stay pending -- "update might not occur in this case", as IVideoDriver.h says.
			if (result == VK_NOT_READY)
				return;
			if (vulkanFailed("CVulkanOcclusionQuery: vkGetQueryPoolResults", result))
				return;

			q.LastResult = static_cast<u32>(samples);
			q.PendingFrame = 0;
		}

		void CVulkanOcclusionQuery::updateAllResults(bool block)
		{
			for (auto& kv : Queries)
				updateResult(kv.first, block);
		}

		u32 CVulkanOcclusionQuery::getResult(const std::shared_ptr<scene::ISceneNode>& node) const
		{
			auto it = Queries.find(node);
			if (it == Queries.end())
				return ~0u; // "no query for this node", NOT "zero pixels visible"
			return it->second.LastResult;
		}

	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_VULKAN_
