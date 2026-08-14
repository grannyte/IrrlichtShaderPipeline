// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "CD3D12HardwareBuffer.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_12_
#include "CD3D12Driver.h"

namespace irr
{
	namespace video
	{
		CD3D12HardwareBuffer::CD3D12HardwareBuffer(CD3D12Driver* driver, E_HARDWARE_BUFFER_TYPE type,
			scene::E_HARDWARE_MAPPING mapping, u32 size, u32 flags, u32 stride, const void* initialData)
			: IHardwareBuffer(mapping, flags, size, type, EDT_DIRECT3D12), Driver(driver), Stride(stride)
		{
			IsDefaultHeapPath = (mapping == scene::EHM_STATIC) || (type == EHBT_COMPUTE);
			if (IsDefaultHeapPath)
			{
				bool asUAV = (type == EHBT_COMPUTE);
				D3D12_RESOURCE_STATES finalState = asUAV ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS :
					(type == EHBT_INDEX ? D3D12_RESOURCE_STATE_INDEX_BUFFER : D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
				createStaticOrComputeResource(initialData, asUAV, finalState);
				if (asUAV)
					createComputeViews();
			}
			else
			{
				createDynamicResources(initialData);
			}
		}

		CD3D12HardwareBuffer::CD3D12HardwareBuffer(scene::IVertexBuffer* vertexBuffer, CD3D12Driver* driver)
			: IHardwareBuffer(vertexBuffer->getHardwareMappingHint(), 0,
				vertexBuffer->getVertexCount() * vertexBuffer->getVertexSize(),
				(vertexBuffer->getBufferType() == scene::EBT_STREAM) ? EHBT_STREAM_OUTPUT : EHBT_VERTEX,
				EDT_DIRECT3D12),
			Driver(driver), Stride(vertexBuffer->getVertexSize())
		{
			// A scene::EBT_STREAM buffer (stream-output target) must always live on the default
			// heap (GPU-local, transitionable to D3D12_RESOURCE_STATE_STREAM_OUT, see
			// CD3D12Driver::setStreamOutputBuffer()) regardless of its mapping hint -- the
			// persistent upload-heap path (createDynamicResources()) stays fixed in GENERIC_READ
			// for its whole life and can never serve as a GPU write target.
			IsDefaultHeapPath = (Mapping == scene::EHM_STATIC) || (Type == EHBT_STREAM_OUTPUT);
			if (IsDefaultHeapPath)
				createStaticOrComputeResource(vertexBuffer->getVertices(), false, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
			else
				createDynamicResources(vertexBuffer->getVertices());
		}

		CD3D12HardwareBuffer::CD3D12HardwareBuffer(scene::IIndexBuffer* indexBuffer, CD3D12Driver* driver)
			: IHardwareBuffer(indexBuffer->getHardwareMappingHint(), 0,
				indexBuffer->getIndexCount() * indexBuffer->getIndexSize(),
				EHBT_INDEX, EDT_DIRECT3D12),
			Driver(driver), Stride(indexBuffer->getIndexSize()), IndexType(indexBuffer->getType())
		{
			IsDefaultHeapPath = (Mapping == scene::EHM_STATIC);
			if (IsDefaultHeapPath)
				createStaticOrComputeResource(indexBuffer->getIndices(), false, D3D12_RESOURCE_STATE_INDEX_BUFFER);
			else
				createDynamicResources(indexBuffer->getIndices());
		}

		CD3D12HardwareBuffer::CD3D12HardwareBuffer(scene::IComputeBuffer* computeBuffer, CD3D12Driver* driver)
			: IHardwareBuffer(computeBuffer->getHardwareMappingHint(), 0,
				computeBuffer->getBufferSize(), EHBT_COMPUTE, EDT_DIRECT3D12),
			Driver(driver), Stride(computeBuffer->getStructureStride())
		{
			// Always default-heap regardless of the mapping hint -- see the .h file header
			// comment: a compute buffer must be able to carry UNORDERED_ACCESS, which a
			// persistent mapped upload heap cannot.
			IsDefaultHeapPath = true;
			createStaticOrComputeResource(computeBuffer->getBufferPointer(), true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			createComputeViews();
		}

		// Same lifetime rule as ~CD3D12Texture: a vertex/index/compute buffer is just as likely
		// to still be referenced by an in-flight command list (in fact the common case -- it is
		// bound on every draw). Releasing it here would trigger
		// OBJECT_DELETED_WHILE_STILL_IN_USE and eventually device removal, so it's queued on the
		// driver's fenced retirement queue instead.
		CD3D12HardwareBuffer::~CD3D12HardwareBuffer()
		{
			for (size_t i = 0; i < Resources.size(); ++i)
			{
				if (Resources[i] && i < MappedData.size() && MappedData[i])
					Resources[i]->Unmap(0, nullptr); // no precise "written range": upload heap, coherent by construction
			}

			if (!Driver)
				return;

			if (HasUAV)
				Driver->retireDescriptor(Driver->getSRVHeap(), UAVHeapIndex);
			if (HasSRV)
				Driver->retireDescriptor(Driver->getSRVHeap(), SRVHeapIndex);

			for (size_t i = 0; i < Resources.size(); ++i)
				Driver->retireResource(std::move(Resources[i]));
			Driver->retireResource(std::move(StagingResource));
		}

		bool CD3D12HardwareBuffer::createDynamicResources(const void* initialData)
		{
			ID3D12Device2* device = Driver->getDevice();
			if (!device || Size == 0)
			{
				os::Printer::log("CD3D12HardwareBuffer: no device or zero size", ELL_ERROR);
				return false;
			}

			UINT frameCount = Driver->getFrameCount();
			Resources.assign(frameCount, ComPtr<ID3D12Resource>());
			MappedData.assign(frameCount, nullptr);

			D3D12_HEAP_PROPERTIES heapProps = {};
			heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
			heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
			heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

			D3D12_RESOURCE_DESC desc = {};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			desc.Alignment = 0;
			desc.Width = Size;
			desc.Height = 1;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.SampleDesc = { 1, 0 };
			desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			desc.Flags = D3D12_RESOURCE_FLAG_NONE;

			// If a slot fails partway through, slots [0..i) are already created/mapped: leaving
			// them half-filled in Resources/MappedData would let a future update() pick a slot in
			// [i..frameCount) that was never mapped (MappedData[slot] == nullptr) and memcpy into
			// it -- hence this rollback, which empties the buffer entirely rather than leaving it
			// half-built. These resources were just created and never submitted to the GPU, so a
			// direct release (via Resources.clear()) is safe -- no need to go through
			// retireResource().
			auto rollbackPartial = [&](UINT createdCount)
			{
				for (UINT j = 0; j < createdCount; ++j)
				{
					if (MappedData[j])
						Resources[j]->Unmap(0, nullptr);
				}
				Resources.clear();
				MappedData.clear();
			};

			// Each frame-in-flight slot gets its own resource, always GENERIC_READ (upload heap)
			// so no resource barrier is ever needed. The N resources start out with the SAME
			// initial data: a slot that hasn't received its own explicit update() yet must never
			// read uninitialized data.
			for (UINT i = 0; i < frameCount; ++i)
			{
				HRESULT hr = device->CreateCommittedResource(
					&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
					IID_PPV_ARGS(&Resources[i]));
				if (FAILED(hr))
				{
					logD3D12Failure("CD3D12HardwareBuffer: CreateCommittedResource (upload, frame-in-flight)", hr, device);
					rollbackPartial(i);
					return false;
				}

				D3D12_RANGE noRead = { 0, 0 };
				hr = Resources[i]->Map(0, &noRead, &MappedData[i]);
				if (FAILED(hr))
				{
					logD3D12Failure("CD3D12HardwareBuffer: Map", hr, device);
					rollbackPartial(i);
					return false;
				}

				if (initialData)
					memcpy(MappedData[i], initialData, Size);
			}

			RequiredUpdate = false;
			return true;
		}

		bool CD3D12HardwareBuffer::createStaticOrComputeResource(const void* initialData, bool asUAV, D3D12_RESOURCE_STATES finalState)
		{
			ID3D12Device2* device = Driver->getDevice();
			if (!device || Size == 0)
			{
				os::Printer::log("CD3D12HardwareBuffer: no device or zero size (default-heap)", ELL_ERROR);
				return false;
			}

			// On an update() (as opposed to construction), Resources[0] already exists and was
			// most likely just drawn with -- an in-flight command list may still reference it.
			// Letting Resources.assign() below destroy it in place would trigger
			// OBJECT_DELETED_WHILE_STILL_IN_USE, so it's queued on the driver's fenced retirement
			// queue instead, exactly like ~CD3D12HardwareBuffer() does.
			for (size_t i = 0; i < Resources.size(); ++i)
			{
				if (Resources[i] && i < MappedData.size() && MappedData[i])
					Resources[i]->Unmap(0, nullptr);
				if (Resources[i] && Driver)
					Driver->retireResource(std::move(Resources[i]));
			}
			Resources.clear();
			MappedData.clear();

			Resources.assign(1, ComPtr<ID3D12Resource>());
			MappedData.assign(1, nullptr); // never mapped on this path

			D3D12_HEAP_PROPERTIES heapProps = {};
			heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

			D3D12_RESOURCE_DESC desc = {};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			desc.Alignment = 0;
			desc.Width = Size;
			desc.Height = 1;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.SampleDesc = { 1, 0 };
			desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			desc.Flags = asUAV ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;

			CurrentState = D3D12_RESOURCE_STATE_COPY_DEST;
			HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
				CurrentState, nullptr, IID_PPV_ARGS(&Resources[0]));
			if (FAILED(hr))
			{
				logD3D12Failure("CD3D12HardwareBuffer: CreateCommittedResource (default heap)", hr, device);
				return false;
			}

			if (initialData)
			{
				// Intermediate upload resource, discarded after the transfer -- same mechanism as
				// CD3D12Texture::uploadInitialData(), but CopyBufferRegion for a linear buffer (no
				// pitch footprint/alignment to compute like for a texture).
				D3D12_HEAP_PROPERTIES uploadHeapProps = {};
				uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

				D3D12_RESOURCE_DESC uploadDesc = desc;
				uploadDesc.Flags = D3D12_RESOURCE_FLAG_NONE; // an upload heap cannot carry ALLOW_UNORDERED_ACCESS

				ComPtr<ID3D12Resource> uploadBuffer;
				hr = device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer));
				if (FAILED(hr))
				{
					os::Printer::log("CD3D12HardwareBuffer: CreateCommittedResource (intermediate upload) failed", ELL_ERROR);
					return false;
				}

				void* mapped = nullptr;
				D3D12_RANGE noRead = { 0, 0 };
				uploadBuffer->Map(0, &noRead, &mapped);
				memcpy(mapped, initialData, Size);
				uploadBuffer->Unmap(0, nullptr);

				CD3D12Driver::UploadScope upload(Driver);
			ID3D12GraphicsCommandList* cmdList = upload.commandList();
				if (!cmdList)
					return false;

				cmdList->CopyBufferRegion(Resources[0].Get(), 0, uploadBuffer.Get(), 0, Size);
				transitionTo(cmdList, finalState);

				upload.endAndWait(); // blocking, like CD3D12Texture -- uploadBuffer can be discarded on return
			}
			else
			{
				// No initial data (e.g. a compute buffer created empty): still transitions to the
				// expected final state, via a dedicated upload command list to stay consistent
				// with the rest of the lifecycle (no "cold" barrier outside a command list).
				CD3D12Driver::UploadScope upload(Driver);
			ID3D12GraphicsCommandList* cmdList = upload.commandList();
				if (cmdList)
				{
					transitionTo(cmdList, finalState);
					upload.endAndWait();
				}
			}

			RequiredUpdate = false;
			return true;
		}

		bool CD3D12HardwareBuffer::createComputeViews()
		{
			ID3D12Device2* device = Driver->getDevice();
			if (!device || Resources.empty() || !Resources[0])
				return false;

			// Also called from update(), where a UAV/SRV may already exist and describe the
			// resource that was just retired: without this we'd leak their heap slots on every
			// update and leave an in-flight command list reading a stale descriptor.
			if (HasUAV)
			{
				Driver->retireDescriptor(Driver->getSRVHeap(), UAVHeapIndex);
				HasUAV = false;
			}
			if (HasSRV)
			{
				Driver->retireDescriptor(Driver->getSRVHeap(), SRVHeapIndex);
				HasSRV = false;
			}

			UINT elementCount = (Stride > 0) ? (Size / Stride) : 0;

			CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandle;
			if (!Driver->getSRVHeap().allocate(UAVHeapIndex, uavHandle))
			{
				os::Printer::log("CD3D12HardwareBuffer: CBV/SRV/UAV heap full (UAV)", ELL_ERROR);
				return false;
			}
			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
			uavDesc.Format = DXGI_FORMAT_UNKNOWN;
			uavDesc.Buffer.FirstElement = 0;
			uavDesc.Buffer.NumElements = elementCount;
			uavDesc.Buffer.StructureByteStride = Stride;
			uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
			device->CreateUnorderedAccessView(Resources[0].Get(), nullptr, &uavDesc, uavHandle);
			UAVHandle = uavHandle;
			HasUAV = true;

			CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle;
			if (!Driver->getSRVHeap().allocate(SRVHeapIndex, srvHandle))
			{
				os::Printer::log("CD3D12HardwareBuffer: CBV/SRV/UAV heap full (SRV)", ELL_ERROR);
				return false;
			}
			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srvDesc.Format = DXGI_FORMAT_UNKNOWN;
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Buffer.FirstElement = 0;
			srvDesc.Buffer.NumElements = elementCount;
			srvDesc.Buffer.StructureByteStride = Stride;
			srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
			device->CreateShaderResourceView(Resources[0].Get(), &srvDesc, srvHandle);
			SRVHandle = srvHandle;
			HasSRV = true;

			return true;
		}

		void CD3D12HardwareBuffer::transitionTo(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES newState)
		{
			if (!cmdList || Resources.empty() || !Resources[0] || CurrentState == newState)
				return;

			CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
				Resources[0].Get(), CurrentState, newState);
			cmdList->ResourceBarrier(1, &barrier);
			CurrentState = newState;
		}

		UINT CD3D12HardwareBuffer::currentFrameSlot() const
		{
			if (Resources.size() <= 1)
				return 0;
			return Driver->getCurrentFrameIndex() % static_cast<UINT>(Resources.size());
		}

		bool CD3D12HardwareBuffer::update(const scene::E_HARDWARE_MAPPING mapping, const u32 size, const void* data)
		{
			if (IsDefaultHeapPath)
			{
				// EHM_STATIC / EHBT_COMPUTE: no directly mapped pointer, so this recreates the
				// resource entirely via the default-heap+copy path -- consistent with these
				// buffers being expected to change rarely (see the .h file header comment).
				bool asUAV = (Type == EHBT_COMPUTE);
				D3D12_RESOURCE_STATES finalState = asUAV ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS :
					(Type == EHBT_INDEX ? D3D12_RESOURCE_STATE_INDEX_BUFFER : D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
				Mapping = mapping;
				Size = size;
				bool ok = createStaticOrComputeResource(data, asUAV, finalState);
				if (ok && asUAV)
					ok = createComputeViews();
				return ok;
			}

			if (Resources.empty() || MappedData.empty())
				return false;

			if (size > Size)
			{
				// Buffer must grow: recreates all frame-in-flight slots at the new size.
				//
				// Same rule as createStaticOrComputeResource(): older slots may still be read by
				// the GPU (frames N-1/N-2 in flight), so they're queued on the fenced retirement
				// queue instead of being destroyed in place.
				for (size_t i = 0; i < Resources.size(); ++i)
				{
					if (Resources[i] && i < MappedData.size() && MappedData[i])
						Resources[i]->Unmap(0, nullptr);
					if (Resources[i] && Driver)
						Driver->retireResource(std::move(Resources[i]));
				}
				Resources.clear();
				MappedData.clear();
				Size = size;
				Mapping = mapping;
				return createDynamicResources(data);
			}

			// Only writes into the resource for the CURRENT frame slot -- never into a slot the
			// GPU might still be reading from a previous frame. See the .h file header comment
			// for the accepted trade-off (a slot that isn't updated keeps its last content).
			UINT slot = currentFrameSlot();
			if (!MappedData[slot])
			{
				// Should no longer happen given the rollback in createDynamicResources() -- kept
				// as a diagnostic to confirm whether this path is still reachable, rather than
				// silently crashing on a memcpy to a null pointer.
				os::Printer::log("CD3D12HardwareBuffer::update: MappedData[slot] is null -- previous "
					"creation likely failed under memory pressure, update skipped", ELL_ERROR);
				return false;
			}
			memcpy(MappedData[slot], data, size);
			Mapping = mapping;
			RequiredUpdate = false;
			return true;
		}

		void* CD3D12HardwareBuffer::lock(bool readOnly)
		{
			if (!IsDefaultHeapPath)
			{
				// Upload-heap "dynamic" path: already mapped persistently per frame slot (see
				// createDynamicResources()) -- no extra Map() needed. Returns the active slot's
				// pointer.
				if (Resources.empty() || MappedData.empty())
					return nullptr;
				return MappedData[currentFrameSlot()];
			}

			// Default-heap path (EHM_STATIC for writes, EHBT_COMPUTE for reads or writes): no
			// directly mapped pointer, goes through an intermediate staging resource -- same shape
			// as CD3D12Texture::lock().
			ID3D12Device2* device = Driver->getDevice();
			if (!device || Resources.empty() || !Resources[0])
				return nullptr;

			D3D12_HEAP_PROPERTIES stagingHeapProps = {};
			D3D12_RESOURCE_DESC stagingDesc = {};
			stagingDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			stagingDesc.Width = Size;
			stagingDesc.Height = 1;
			stagingDesc.DepthOrArraySize = 1;
			stagingDesc.MipLevels = 1;
			stagingDesc.Format = DXGI_FORMAT_UNKNOWN;
			stagingDesc.SampleDesc = { 1, 0 };
			stagingDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

			StagingIsReadback = readOnly;

			if (readOnly)
			{
				// Read (compute buffer, via ComputeBuffer<T>::downloadFromGPU()): GPU->CPU copy
				// through a READBACK resource.
				stagingHeapProps.Type = D3D12_HEAP_TYPE_READBACK;
				HRESULT hr = device->CreateCommittedResource(&stagingHeapProps, D3D12_HEAP_FLAG_NONE, &stagingDesc,
					D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&StagingResource));
				if (FAILED(hr))
					return nullptr;

				CD3D12Driver::UploadScope upload(Driver);
			ID3D12GraphicsCommandList* cmdList = upload.commandList();
				if (!cmdList)
					return nullptr;

				D3D12_RESOURCE_STATES stateBeforeCopy = CurrentState;
				transitionTo(cmdList, D3D12_RESOURCE_STATE_COPY_SOURCE);
				cmdList->CopyBufferRegion(StagingResource.Get(), 0, Resources[0].Get(), 0, Size);
				transitionTo(cmdList, stateBeforeCopy);
				upload.endAndWait();

				D3D12_RANGE readRange = { 0, Size };
				StagingResource->Map(0, &readRange, &MappedStagingData);
			}
			else
			{
				// Write (EHM_STATIC, or a compute buffer reset from the CPU): upload heap, mapped
				// directly -- the copy to the default-heap resource happens in unlock().
				stagingHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
				HRESULT hr = device->CreateCommittedResource(&stagingHeapProps, D3D12_HEAP_FLAG_NONE, &stagingDesc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&StagingResource));
				if (FAILED(hr))
					return nullptr;

				D3D12_RANGE noRead = { 0, 0 };
				StagingResource->Map(0, &noRead, &MappedStagingData);
			}

			return MappedStagingData;
		}

		void CD3D12HardwareBuffer::unlock()
		{
			if (!IsDefaultHeapPath)
				return; // nothing to do: already mapped persistently, see lock()

			if (!StagingResource || !MappedStagingData)
				return;

			if (StagingIsReadback)
			{
				StagingResource->Unmap(0, nullptr);
			}
			else
			{
				D3D12_RANGE writtenRange = { 0, Size };
				StagingResource->Unmap(0, &writtenRange);

				CD3D12Driver::UploadScope upload(Driver);
			ID3D12GraphicsCommandList* cmdList = upload.commandList();
				if (cmdList)
				{
					D3D12_RESOURCE_STATES stateBeforeCopy = CurrentState;
					transitionTo(cmdList, D3D12_RESOURCE_STATE_COPY_DEST);
					cmdList->CopyBufferRegion(Resources[0].Get(), 0, StagingResource.Get(), 0, Size);
					transitionTo(cmdList, stateBeforeCopy);
					upload.endAndWait();
				}
			}

			StagingResource.Reset();
			MappedStagingData = nullptr;
		}

		ID3D12Resource* CD3D12HardwareBuffer::getResource() const
		{
			if (Resources.empty())
				return nullptr;
			return Resources[currentFrameSlot()].Get();
		}

		D3D12_VERTEX_BUFFER_VIEW CD3D12HardwareBuffer::getVertexBufferView() const
		{
			D3D12_VERTEX_BUFFER_VIEW view = {};
			ID3D12Resource* resource = getResource();
			if (resource)
			{
				view.BufferLocation = resource->GetGPUVirtualAddress();
				view.SizeInBytes = Size;
				view.StrideInBytes = Stride;
			}
			return view;
		}

		D3D12_INDEX_BUFFER_VIEW CD3D12HardwareBuffer::getIndexBufferView() const
		{
			D3D12_INDEX_BUFFER_VIEW view = {};
			ID3D12Resource* resource = getResource();
			if (resource)
			{
				view.BufferLocation = resource->GetGPUVirtualAddress();
				view.SizeInBytes = Size;
				view.Format = (IndexType == video::EIT_32BIT) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
			}
			return view;
		}

	}
}

#endif // _IRR_COMPILE_WITH_DIRECT3D_12_
