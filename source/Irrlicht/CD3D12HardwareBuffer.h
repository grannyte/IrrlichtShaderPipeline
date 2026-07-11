// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// Vertex/index/compute buffers for the D3D12 driver. Mirrors the role of CD3D11HardwareBuffer.
//
// Two allocation strategies depending on Type/Mapping (chosen once at construction, see
// IsDefaultHeapPath):
//
// 1. Persistent upload-heap path -- EHBT_VERTEX/EHBT_INDEX with Mapping != EHM_STATIC (i.e.
//    EHM_NEVER/EHM_DYNAMIC/EHM_STREAM/EHM_STAGING). N resources (Driver->getFrameCount(), one per
//    frame-in-flight), each kept in D3D12_RESOURCE_STATE_GENERIC_READ and mapped once for its
//    whole lifetime -- no resource barrier ever needed. update() only writes into the resource
//    for the CURRENT frame slot (see currentFrameSlot()), so a previous frame's GPU read of an
//    older slot is never raced. Trade-off: a frame slot that never received its own explicit
//    update() keeps the content of its last update (or the initial data) -- a caller wanting
//    fresh data every frame must call update() every frame the buffer is used, same contract as
//    any per-frame-in-flight ring buffer (see SD3D12FrameContext).
//
// 2. Default-heap + one-shot copy path -- EHM_STATIC (vertex/index), any EHBT_COMPUTE, and any
//    EHBT_STREAM_OUTPUT (a stream-output target, scene::EBT_STREAM on the source IVertexBuffer --
//    see CD3D12Driver::setStreamOutputBuffer(); forced regardless of the mapping hint, since a
//    buffer the GS writes to must be able to transition to D3D12_RESOURCE_STATE_STREAM_OUT, which
//    the upload-heap path above cannot do since it stays fixed in GENERIC_READ for its whole
//    life). A single D3D12_HEAP_TYPE_DEFAULT (VRAM) resource, populated through an intermediate
//    upload resource discarded after the transfer (same mechanism as
//    CD3D12Texture::uploadInitialData(), see createStaticOrComputeResource()). No directly mapped
//    pointer: lock()/unlock() go through a staging resource (upload for writes, readback for
//    reads -- needed by ComputeBuffer<T>::downloadFromGPU()). update() on this path recreates the
//    resource entirely rather than patching in place -- acceptable since EHM_STATIC/compute
//    buffers are expected to change rarely.
//
// Compute buffers (EHBT_COMPUTE) always create both a UAV and an SRV (same contract as
// CD3D11HardwareBuffer) -- dispatchComputeShader() decides at call time which of the two Src/Dst
// buffers plays which role.

#ifndef __C_DIRECTX12_HARDWARE_BUFFER_H_INCLUDED__
#define __C_DIRECTX12_HARDWARE_BUFFER_H_INCLUDED__

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_12_

#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include "IHardwareBuffer.h"
#include "IVertexBuffer.h"
#include "IIndexBuffer.h"
#include "IComputebuffer.h"
#include "CD3D12Helpers.h"

namespace irr
{
	namespace video
	{
		using Microsoft::WRL::ComPtr;

		class CD3D12Driver;

		class CD3D12HardwareBuffer : public IHardwareBuffer
		{
		public:
			//! Generic construction (also used for EHBT_CONSTANTS in the future).
			CD3D12HardwareBuffer(CD3D12Driver* driver, E_HARDWARE_BUFFER_TYPE type,
				scene::E_HARDWARE_MAPPING mapping, u32 size, u32 flags, u32 stride,
				const void* initialData = nullptr);

			//! Construct from an existing IVertexBuffer / IIndexBuffer, like
			//! CD3D11HardwareBuffer -- matches the two overloads of
			//! CD3D12Driver::createHardwareBuffer().
			CD3D12HardwareBuffer(scene::IVertexBuffer* vertexBuffer, CD3D12Driver* driver);
			CD3D12HardwareBuffer(scene::IIndexBuffer* indexBuffer, CD3D12Driver* driver);

			//! Compute buffer. Always allocated on the default-heap path (must be able to carry
			//! the UNORDERED_ACCESS state, which a persistent mapped upload heap cannot) regardless
			//! of computeBuffer->getHardwareMappingHint().
			CD3D12HardwareBuffer(scene::IComputeBuffer* computeBuffer, CD3D12Driver* driver);

			virtual ~CD3D12HardwareBuffer();

			bool update(const scene::E_HARDWARE_MAPPING mapping, const u32 size, const void* data) _IRR_OVERRIDE_;
			void* lock(bool readOnly = false) override;
			void unlock() override;

			//! Resource for the current frame (dynamic path) or the single resource (default-heap
			//! path).
			ID3D12Resource* getResource() const;

			//! Views ready to pass to IASetVertexBuffers/IASetIndexBuffer, resolved against the
			//! current frame's resource.
			D3D12_VERTEX_BUFFER_VIEW getVertexBufferView() const;
			D3D12_INDEX_BUFFER_VIEW getIndexBufferView() const;

			//! Compute buffer views (EHBT_COMPUTE only, otherwise return false / a null handle).
			//! Always created together, see the file header comment.
			bool hasUnorderedAccessView() const { return HasUAV; }
			D3D12_CPU_DESCRIPTOR_HANDLE getUnorderedAccessView() const { return UAVHandle; }
			bool hasShaderResourceView() const { return HasSRV; }
			D3D12_CPU_DESCRIPTOR_HANDLE getShaderResourceView() const { return SRVHandle; }

			//! Emits a CurrentState -> newState barrier on cmdList if needed (no-op otherwise).
			//! Only relevant on the default-heap path (IsDefaultHeapPath): upload-heap resources
			//! stay fixed in GENERIC_READ, no barrier is ever valid for them. Public for
			//! stream-output: CD3D12Driver::setStreamOutputBuffer() uses it to transition the
			//! target buffer to D3D12_RESOURCE_STATE_STREAM_OUT and, once detached, back to
			//! VERTEX_AND_CONSTANT_BUFFER so a later draw can read what the geometry shader wrote.
			void transitionTo(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES newState);

			//! Rewrites CurrentState WITHOUT emitting a barrier -- reserved for cases where the
			//! caller knows, by construction, that the real GPU state is already trueState due to
			//! something outside this class's view. Concrete case: a barrier recorded by
			//! transitionTo() that was never submitted via ExecuteCommandLists (because the
			//! command list was Reset() beforehand, see CD3D12Driver::beginScene()) leaves
			//! CurrentState lying about the real GPU state -- the caller must resync it before
			//! issuing a new (real) barrier via transitionTo(), otherwise that call would compute
			//! a transition from the wrong starting state. Only use this to fix that desync, never
			//! as a shortcut to skip a barrier that is actually needed.
			void resyncCurrentState(D3D12_RESOURCE_STATES trueState) { CurrentState = trueState; }

		private:
			//! Path 1 (see file header comment): Driver->getFrameCount() upload-heap resources,
			//! each mapped once for its whole lifetime.
			bool createDynamicResources(const void* initialData);

			//! Path 2 (see file header comment): a single default-heap resource, populated via a
			//! temporary upload resource discarded after the transfer. asUAV adds
			//! D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; finalState is the state the resource
			//! should end up in (VERTEX_AND_CONSTANT_BUFFER/INDEX_BUFFER for static geometry,
			//! UNORDERED_ACCESS for a compute buffer).
			bool createStaticOrComputeResource(const void* initialData, bool asUAV, D3D12_RESOURCE_STATES finalState);

			//! EHBT_COMPUTE only: allocates a UAV + SRV (structured buffer views, stride = Stride)
			//! in Driver->getSRVHeap() -- same heap as textures' CBV/SRV/UAV, just a different view
			//! type created in it (D3D12 has no separate heap for UAVs).
			bool createComputeViews();

			//! Index of the "active" resource in Resources: always 0 on the default-heap path (a
			//! single element), otherwise Driver->getCurrentFrameIndex() modulo N.
			UINT currentFrameSlot() const;

			bool IsDefaultHeapPath = false; // true => EHM_STATIC (vertex/index), EHBT_COMPUTE, or EHBT_STREAM_OUTPUT

			CD3D12Driver* Driver;
			std::vector<ComPtr<ID3D12Resource>> Resources;
			std::vector<void*> MappedData; // parallel to Resources; empty/nullptr on the default-heap path
			u32 Stride = 0;
			video::E_INDEX_TYPE IndexType = video::EIT_16BIT; // only relevant when Type == EHBT_INDEX

			//! Current state of Resources[0] -- only kept up to date on the default-heap path.
			D3D12_RESOURCE_STATES CurrentState = D3D12_RESOURCE_STATE_COMMON;

			bool HasUAV = false;
			CD3DX12_CPU_DESCRIPTOR_HANDLE UAVHandle;
			UINT UAVHeapIndex = 0;
			bool HasSRV = false;
			CD3DX12_CPU_DESCRIPTOR_HANDLE SRVHandle;
			UINT SRVHeapIndex = 0;

			//! Intermediate resource for lock()/unlock() on the default-heap path (EHM_STATIC for
			//! writes, EHBT_COMPUTE for reads via ComputeBuffer<T>::downloadFromGPU() or writes).
			//! Recreated on each lock, released on unlock -- same shape as
			//! CD3D12Texture::lock()/unlock().
			ComPtr<ID3D12Resource> StagingResource;
			void* MappedStagingData = nullptr;
			bool StagingIsReadback = false;
		};

	}
}

#endif // _IRR_COMPILE_WITH_DIRECT3D_12_
#endif
