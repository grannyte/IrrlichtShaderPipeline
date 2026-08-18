// Copyright (C) 2002-2009 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_11_

#include "CD3D11Driver.h"
#include "CD3D11HardwareBuffer.h"
#include "os.h"
#include <iostream>

namespace irr
{
	namespace video
	{
		E_HARDWARE_BUFFER_TYPE ConvertBufferType(irr::scene::E_BUFFER_TYPE type)
		{
			switch (type)
			{
			case irr::scene::E_BUFFER_TYPE::EBT_VERTEX:
				return EHBT_VERTEX;
				break;
			case irr::scene::E_BUFFER_TYPE::EBT_STREAM:
				return EHBT_STREAM_OUTPUT;
				break;
			default:
				return EHBT_VERTEX;
				break;
			}
		}

		CD3D11HardwareBuffer::CD3D11HardwareBuffer(CD3D11Driver* driver, E_HARDWARE_BUFFER_TYPE type,
			scene::E_HARDWARE_MAPPING mapping, u32 size, u32 flags,u32 stride, const void* initialData)
			: IHardwareBuffer(mapping, flags, size, type, driver->getDriverType()),
			Device(driver->getExposedVideoData().D3D11.D3DDev11), Context(NULL), Buffer(NULL), UAView(NULL), SRView(NULL), Driver(driver),
			LastMapDirection((D3D11_MAP)0),
			 LinkedBuffer(0)
		{
			Stride = stride;
#ifdef _DEBUG
			//setDebugName("CD3D11HardwareBuffer");
#endif

			if (Device)
			{
				Device->AddRef();
				// Stays the IMMEDIATE context: lock() can Map for READ, which is only legal there.
				// Uploads pick their own context -- see copyFromMemory().
				Device->GetImmediateContext(&Context);
			}

			createInternalBuffer(initialData);

			// set need of staging buffer
			//if (AccessType == EHBA_DYNAMIC && AccessType == EHBA_IMMUTABLE)
			//UseTempStagingBuffer = true;

			RequiredUpdate = false;
		}

		CD3D11HardwareBuffer::CD3D11HardwareBuffer(scene::IIndexBuffer* indexBuffer, CD3D11Driver* driver) :
			IHardwareBuffer(scene::EHM_NEVER, 0, 0, EHBT_INDEX, EDT_DIRECT3D11), Device(driver->getExposedVideoData().D3D11.D3DDev11), Context(NULL),
			Buffer(NULL), UAView(NULL), SRView(NULL), Driver(driver),
			LastMapDirection((D3D11_MAP)0),  LinkedBuffer(0)
		{
#ifdef _DEBUG
			//setDebugName("CD3D11HardwareBuffer");
#endif

			if (Device)
			{
				Device->AddRef();
				// Stays the IMMEDIATE context: lock() can Map for READ, which is only legal there.
				// Uploads pick their own context -- see copyFromMemory().
				Device->GetImmediateContext(&Context);
			}

			if (indexBuffer)
			{
				Mapping = indexBuffer->getHardwareMappingHint();
				Size = indexBuffer->getIndexSize() * indexBuffer->getIndexCount();
				Stride = indexBuffer->getIndexSize();
				LinkedBuffer = indexBuffer;
				createInternalBuffer(indexBuffer->getIndices());

				RequiredUpdate = false;

			}
		}

		CD3D11HardwareBuffer::CD3D11HardwareBuffer(scene::IVertexBuffer* vertexBuffer, CD3D11Driver* driver) :
			IHardwareBuffer(scene::EHM_NEVER, 0, 0, ConvertBufferType(vertexBuffer->getBufferType()), EDT_DIRECT3D11), Device(driver->getExposedVideoData().D3D11.D3DDev11), Context(NULL),
			Buffer(NULL), UAView(NULL), SRView(NULL), Driver(driver), 
			LastMapDirection((D3D11_MAP)0),  LinkedBuffer(0)
		{
#ifdef _DEBUG
			//setDebugName("CD3D11HardwareBuffer");
#endif

			if (Device)
			{
				Device->AddRef();
				// Stays the IMMEDIATE context: lock() can Map for READ, which is only legal there.
				// Uploads pick their own context -- see copyFromMemory().
				Device->GetImmediateContext(&Context);
			}

			if (vertexBuffer)
			{
				Mapping = vertexBuffer->getHardwareMappingHint();
				Size = vertexBuffer->getVertexSize() * vertexBuffer->getVertexCount();
				Stride = vertexBuffer->getVertexSize();
				LinkedBuffer = vertexBuffer;
				createInternalBuffer(vertexBuffer->getVertices());

				RequiredUpdate = false;


			}
		}

		CD3D11HardwareBuffer::CD3D11HardwareBuffer(scene::IComputeBuffer* computeBuffer, CD3D11Driver* driver) :
			IHardwareBuffer(scene::EHM_NEVER, computeBuffer ? computeBuffer->getBufferFlags() : 0, 0, EHBT_COMPUTE, EDT_DIRECT3D11), Device(driver->getExposedVideoData().D3D11.D3DDev11), Context(NULL),
			Buffer(NULL), UAView(NULL), SRView(NULL), Driver(driver),
			LastMapDirection((D3D11_MAP)0),  LinkedBuffer(0)
		{
			#ifdef _DEBUG
			//setDebugName("CD3D11HardwareBuffer");
			#endif

			if (Device)
			{
				Device->AddRef();
				// Stays the IMMEDIATE context: lock() can Map for READ, which is only legal there.
				// Uploads pick their own context -- see copyFromMemory().
				Device->GetImmediateContext(&Context);
			}

			if (computeBuffer)
			{
				Mapping = computeBuffer->getHardwareMappingHint();
				Size = computeBuffer->getBufferSize();
				Stride = computeBuffer->getStructureStride();
				LinkedBuffer = computeBuffer;
				createInternalBuffer(computeBuffer->getBufferPointer());

				RequiredUpdate = false;


			}
		

		}
		CD3D11HardwareBuffer::~CD3D11HardwareBuffer()
		{
			if (LinkedBuffer)
			{
				switch (Type)
				{
				case EHBT_INDEX:
					((scene::IIndexBuffer*)LinkedBuffer)->setHardwareBuffer(0);
					break;
				case EHBT_VERTEX:
					((scene::IVertexBuffer*)LinkedBuffer)->setHardwareBuffer(0);
					break;
				default:
					break;
				}
			}


			if (SRView)
				SRView->Release();

			if (UAView)
				UAView->Release();

			if (Buffer)
				Buffer->Release();

			if (Context)
				Context->Release();

			if (Device)
				Device->Release();
		}

		bool CD3D11HardwareBuffer::update(const scene::E_HARDWARE_MAPPING mapping, const u32 size, const void* data)
		{
			if (!Buffer || size > Size)
			{
				// Release buffer if need to expand
				if (size > Size)
				{
					if (TempStagingBuffer)
					{
						TempStagingBuffer = NULL;
						StagingLocked = false;
					}

					if (SRView)
					{
						SRView->Release();
						SRView = NULL;
					}

					if (UAView)
					{
						UAView->Release();
						UAView = NULL;
					}

					if (Buffer)
					{
						Buffer->Release();
						Buffer = NULL;
					}
				}

				Size = size;
				Mapping = mapping;

				createInternalBuffer(data);

				if (!Buffer)
					return false;
			}
			else if (mapping != Mapping)
			{
				if (TempStagingBuffer)
				{
					TempStagingBuffer = NULL;
					StagingLocked = false;
				}

				if (SRView)
				{
					SRView->Release();
					SRView = NULL;
				}

				if (UAView)
				{
					UAView->Release();
					UAView = NULL;
				}

				if (Buffer)
				{
					Buffer->Release();
					Buffer = NULL;
				}

				Size = size;
				Mapping = mapping;

				createInternalBuffer(data);
			}
			else // just update
			{
				copyFromMemory(data, 0, size);
			}

			RequiredUpdate = false;

			return true;
		}

		//! Lock function.
		void* CD3D11HardwareBuffer::lock(bool readOnly)
		{
			if (!Buffer)
				return 0;

			if (readOnly)
				LastMapDirection = D3D11_MAP_READ;
			else
				LastMapDirection = (D3D11_MAP)(D3D11_MAP_WRITE | D3D11_MAP_READ);

			if (Mapping == scene::EHM_STAGING) {
				// Otherwise, map this buffer
				D3D11_MAPPED_SUBRESOURCE mappedData;
				HRESULT hr = Context->Map(Buffer, 0, LastMapDirection, 0, &mappedData);
				if (FAILED(hr))
					return 0;
				mappedDimension.X = mappedData.RowPitch;
				mappedDimension.Y = mappedData.DepthPitch;
				return mappedData.pData;
			}
			else
			{
				// Reused across locks: creating a staging buffer per readback costs a driver
				// allocation every frame. Resize/mapping changes null it above, so a survivor fits.
				if (!TempStagingBuffer)
					TempStagingBuffer = std::make_shared<CD3D11HardwareBuffer>(Driver, EHBT_SYSTEM, scene::EHM_STAGING, Size, 0, Stride);
				TempStagingBuffer->copyFromBuffer(shared_from_this(), 0, 0, Size);
				StagingLocked = true;
				return TempStagingBuffer->lock(readOnly);
			}
		}

		//! Unlock function. Must be called after a lock() to the buffer.
		void CD3D11HardwareBuffer::unlock()
		{
			if (!Buffer)
				return;

			// If using staging, return its pointer
			if (StagingLocked && TempStagingBuffer)
			{
				TempStagingBuffer->unlock();

				// If write, copy staging to this
				if (LastMapDirection & D3D11_MAP_WRITE)
					copyFromBuffer(TempStagingBuffer, 0, 0, Size);
				StagingLocked = false;
				return;
			}

			// Otherwise, unmap this
			Context->Unmap(Buffer, 0);
		}

		//! Copy data from system memory
		void CD3D11HardwareBuffer::copyFromMemory(const void* sysData, u32 offset, u32 length)
		{
			// Uploads on the creating driver's context -- the recording context while recording.
			// Both branches below are legal there (UpdateSubresource always; Map only because this
			// is WRITE_DISCARD on a non-static, i.e. dynamic, buffer).
			ID3D11DeviceContext* uploadContext = Driver ? Driver->getContext() : Context;
			if (!uploadContext)
				uploadContext = Context;

			if (Buffer && Mapping == scene::EHM_DYNAMIC)
			{
				D3D11_BOX box;
				box.left = offset;
				box.top = 0;
				box.front = 0;
				box.right = length;
				box.bottom = 1;
				box.back = 1;
				uploadContext->UpdateSubresource(Buffer, 0, &box, sysData, 0, 0);
			}
			else if (Buffer && Mapping != scene::EHM_STATIC)
			{
				D3D11_MAPPED_SUBRESOURCE mappedData;
				HRESULT hr = uploadContext->Map(Buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedData);
				if (FAILED(hr))
				{
					os::Printer::log("Error Could not map dynamic buffr", ELL_ERROR);
					return;
				}
				memcpy(mappedData.pData, sysData, length);
				uploadContext->Unmap(Buffer, 0);
			}
			else
			{
				if (TempStagingBuffer)
				{
					TempStagingBuffer = NULL;
					StagingLocked = false;
				}

				if (SRView)
				{
					SRView->Release();
					SRView = NULL;
				}

				if (UAView)
				{
					UAView->Release();
					UAView = NULL;
				}

				if (Buffer)
				{
					Buffer->Release();
					Buffer = NULL;
				}

				Size = length;

				createInternalBuffer(sysData);
			}
		}

		//! Copy data from another buffer
		void CD3D11HardwareBuffer::copyFromBuffer(const std::shared_ptr<IHardwareBuffer>& buffer, u32 srcOffset, u32 destOffset, u32 length)
		{
			if (!Buffer)
				return;

			if (buffer->getDriverType() != EDT_DIRECT3D11)
			{
				os::Printer::log("Fatal Error: Tried to copy data from a buffer not owned by this driver.", ELL_ERROR);
				return;
			}

			auto srcBuffer = std::static_pointer_cast<CD3D11HardwareBuffer>(buffer);

			// try fast copy if possible
			if (srcOffset == 0 && destOffset == 0 && length == Size
				&& Size == buffer->size())
			{
				Context->CopyResource(Buffer, srcBuffer->getBuffer());
			}
			else	// else, copy subregion
			{
				D3D11_BOX srcBox;
				srcBox.left = (UINT)srcOffset;
				srcBox.right = (UINT)srcOffset + length;
				srcBox.top = 0;
				srcBox.bottom = 1;
				srcBox.front = 0;
				srcBox.back = 1;

				Context->CopySubresourceRegion(Buffer, 0, (UINT)destOffset, 0, 0,
					srcBuffer->getBuffer(), 0, &srcBox);
			}
		}

		//! return unordered access view
		ID3D11UnorderedAccessView* CD3D11HardwareBuffer::getUnorderedAccessView() const
		{
			return UAView;
		}

		ID3D11ShaderResourceView* CD3D11HardwareBuffer::getShaderResourceView() const
		{
			return SRView;
		}

		bool CD3D11HardwareBuffer::createInternalBuffer(const void* initialData)
		{
			HRESULT hr = 0;

			// Report rather than fault in CreateBuffer below: a null Device or a zero/absurd Size
			// is a caller bug, and the AV it used to produce named this function instead of them.
			if (!Device || !Size)
			{
				core::stringc msg = "createInternalBuffer: bad state, Device=";
				msg += (s32)(Device != NULL);
				msg += " Size="; msg += (s32)Size;
				msg += " Stride="; msg += (s32)Stride;
				msg += " Type="; msg += (s32)Type;
				msg += " Mapping="; msg += (s32)Mapping;
				msg += " initialData="; msg += (s32)(initialData != NULL);
				os::Printer::log(msg.c_str(), ELL_ERROR);
				return false;
			}

			D3D11_BUFFER_DESC desc;
				desc.ByteWidth = Size;
				desc.StructureByteStride = 0;   // only set for structured (EHBT_COMPUTE) buffers below
				desc.MiscFlags = 0;
				desc.CPUAccessFlags = 0;

			// Create new buffer
			switch (Mapping)
			{
			case scene::EHM_NEVER:
			case scene::EHM_STREAM:
				desc.Usage = D3D11_USAGE_DYNAMIC;
				desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				break;
			case scene::EHM_DYNAMIC:
				desc.Usage = D3D11_USAGE_DEFAULT;
				//		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				break;
			case scene::EHM_STATIC:
				desc.Usage = D3D11_USAGE_IMMUTABLE;
				break;
			case scene::EHM_STAGING:
				desc.Usage = D3D11_USAGE_STAGING;
				desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;
				break;
			default:
				desc.Usage = D3D11_USAGE_DEFAULT;
				break;
			}

			// Check bind flags
			switch (Type)
			{
			case EHBT_VERTEX:
				desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
				break;
			case EHBT_INDEX:
				desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
				break;
			case EHBT_STREAM_OUTPUT:
				desc.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_STREAM_OUTPUT;
				break;
			case EHBT_COMPUTE:
					desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
					desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
					desc.StructureByteStride = Stride;
					// An args buffer for DispatchIndirect is raw, not structured - those two misc
					// flags are mutually exclusive. ALLOW_RAW_VIEWS is required as well, or the
					// raw UAV built below fails to create on every such buffer.
					if (Flags & EHBF_DRAW_INDIRECT_ARGS)
					{
						desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS
									   | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
						desc.StructureByteStride = 0;
						desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
					}
					// D3D11 forbids MISC_BUFFER_STRUCTURED together with BIND_VERTEX_BUFFER, so a
					// compute-written vertex stream has to be a raw buffer instead of a structured one.
					else if (Flags & EHBF_VERTEX_ADDITIONAL_BIND)
					{
						desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
						desc.StructureByteStride = 0;
						desc.BindFlags |= D3D11_BIND_VERTEX_BUFFER;
					}
					break;
			case EHBT_SHADER_RESOURCE:
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				break;
			case EHBT_CONSTANTS:
				desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
				break;
			case EHBT_SYSTEM:
				desc.BindFlags = 0;
				break;
			default:
				desc.BindFlags = 0;
				break;
			}

			// Load initial data
			D3D11_SUBRESOURCE_DATA data;
			data.pSysMem = initialData;
			data.SysMemPitch = 0;
			data.SysMemSlicePitch = 0;

			//char dump = 0;
			//for (int i = 0; i < Size; ++i)
			{
				//	dump += ((char*)initialData)[i];
			}
			//std::cout << dump << std::endl;
			// Create buffer
			// D3D11 does not allow initial data for stream output buffers
			// (D3D11 ERROR: CREATEBUFFER_INVALIDARG when pInitialData != NULL and
			//  D3D11_BIND_STREAM_OUTPUT is set). Always pass NULL for SO buffers.
			if (Type == EHBT_STREAM_OUTPUT)
				initialData = nullptr;

			auto plinkedBuffer = LinkedBuffer;
			auto pGetVertices = LinkedBuffer && LinkedBuffer->getBufferType() == irr::scene::EBT_VERTEX ? ((scene::IVertexBuffer*)LinkedBuffer)->getVertices() : LinkedBuffer && LinkedBuffer->getBufferType() == irr::scene::EBT_INDEX ? ((scene::IIndexBuffer*)LinkedBuffer)->getIndices():0;
			auto pInitialData = initialData;
			auto pdatapsysmem = data.pSysMem;

			//Print the content of desc and data
			hr = Device->CreateBuffer(&desc, initialData != nullptr? &data:0, &Buffer);
			if (FAILED(hr))
			{
				printf("\nvertexBuffer : %p", plinkedBuffer);
				printf("vertexBuffer->getVertices() : %p", pGetVertices);

				printf("initialData : %p", pInitialData);
				printf("data.pSysMem : %p", pdatapsysmem);
				;
				os::Printer::log("Error creating hardware buffer", ELL_ERROR);
				auto removereason = Device->GetDeviceRemovedReason();
				os::Printer::log(core::stringw(removereason).c_str(), ELL_ERROR);
				return false;
			}

			switch (Type)
			{
				// If buffer is of type shader resource, create view

			// If buffer if of type compute, create view
			case EHBT_COMPUTE:
			{
				D3D11_UNORDERED_ACCESS_VIEW_DESC UAVDesc;
				UAVDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
				UAVDesc.Buffer.FirstElement = 0;
				UAVDesc.Buffer.Flags = 0;

				// Only an APPEND/COUNTER view owns a hidden counter, which is what
				// copyStructureCount() reads to feed an indirect dispatch.
				if (Flags & (EHBF_COMPUTE_APPEND | EHBF_COMPUTE_CONSUME))
					UAVDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;

				if (Flags & (EHBF_DRAW_INDIRECT_ARGS | EHBF_VERTEX_ADDITIONAL_BIND))
				{
					// Raw view: no structure stride to divide by.
					UAVDesc.Format = DXGI_FORMAT_R32_TYPELESS;
					UAVDesc.Buffer.Flags |= D3D11_BUFFER_UAV_FLAG_RAW;
					UAVDesc.Buffer.NumElements = desc.ByteWidth / 4;
				}
				else if (Driver->queryFeature(EVDF_COMPUTING_SHADER_5_0))
				{
					UAVDesc.Format = DXGI_FORMAT_UNKNOWN;
					UAVDesc.Buffer.NumElements = desc.ByteWidth / desc.StructureByteStride;	// size in floats
				}
				else
				{
					UAVDesc.Format = DXGI_FORMAT_R32_TYPELESS;
					UAVDesc.Buffer.NumElements = desc.ByteWidth;		// size in bytes
				}

				hr = Device->CreateUnorderedAccessView(Buffer, &UAVDesc, &UAView);
				if (FAILED(hr))
				{
					os::Printer::log("Error creating unordered access view for buffer", ELL_ERROR);
					return false;
				}

				// An args buffer has no SRV bind flag and no structure stride, so stop here
				// rather than falling into the divide-by-stride below.
				if (Flags & EHBF_DRAW_INDIRECT_ARGS)
					return true;

				// A vertex-bindable compute buffer is raw too, so its SRV must be BUFFEREX/raw.
				if (Flags & EHBF_VERTEX_ADDITIONAL_BIND)
				{
					D3D11_SHADER_RESOURCE_VIEW_DESC RawSRVDesc;
					ZeroMemory(&RawSRVDesc, sizeof(RawSRVDesc));
					RawSRVDesc.Format = DXGI_FORMAT_R32_TYPELESS;
					RawSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
					RawSRVDesc.BufferEx.NumElements = desc.ByteWidth / 4;
					RawSRVDesc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;

					hr = Device->CreateShaderResourceView(Buffer, &RawSRVDesc, &SRView);
					if (FAILED(hr))
					{
						os::Printer::log("Error creating raw shader resource view for buffer", ELL_ERROR);
						return false;
					}
					return true;
				}

				// Deliberate fallthrough: a compute buffer also gets an SRV, so it can be bound
				// read-only to a later dispatch. bindComputeBuffer() relies on this.
			}
			case EHBT_SHADER_RESOURCE:
			{
				D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc;
				ZeroMemory(&SRVDesc, sizeof(SRVDesc));
				SRVDesc.Format = DXGI_FORMAT_UNKNOWN;
				SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
				SRVDesc.Buffer.FirstElement = 0;
				SRVDesc.Buffer.NumElements = desc.ByteWidth / desc.StructureByteStride;

				hr = Device->CreateShaderResourceView(Buffer, &SRVDesc, &SRView);
				if (FAILED(hr))
				{
					os::Printer::log("Error creating shader resource view for buffer", ELL_ERROR);
					return false;
				}

				return true;
			}
			default:
				return true;
			}
		}

		ID3D11Buffer* CD3D11HardwareBuffer::getBuffer() const
		{
			return Buffer;
		}

	}
}

#endif