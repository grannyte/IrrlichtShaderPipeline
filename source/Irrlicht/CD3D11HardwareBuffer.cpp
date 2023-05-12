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
			scene::E_HARDWARE_MAPPING mapping, u32 size, u32 flags, const void* initialData)
			: IHardwareBuffer(mapping, flags, size, type, driver->getDriverType()),
			Device(NULL), Context(NULL), Buffer(NULL), UAView(NULL), SRView(NULL), Driver(driver),
			TempStagingBuffer(NULL), UseTempStagingBuffer(false), LastMapDirection((D3D11_MAP)0),
			 LinkedBuffer(0)
		{
#ifdef _DEBUG
			//setDebugName("CD3D11HardwareBuffer");
#endif

			Device = driver->getExposedVideoData().D3D11.D3DDev11;
			if (Device)
			{
				Device->AddRef();
				Device->GetImmediateContext(&Context);
			}

			createInternalBuffer(initialData);

			// set need of staging buffer
			//if (AccessType == EHBA_DYNAMIC && AccessType == EHBA_IMMUTABLE)
			//UseTempStagingBuffer = true;

			RequiredUpdate = false;
		}

		CD3D11HardwareBuffer::CD3D11HardwareBuffer(scene::IIndexBuffer* indexBuffer, CD3D11Driver* driver) :
			IHardwareBuffer(scene::EHM_NEVER, 0, 0, EHBT_INDEX, EDT_DIRECT3D11), Device(NULL), Context(NULL),
			Buffer(NULL), UAView(NULL), SRView(NULL), Driver(driver), TempStagingBuffer(NULL), UseTempStagingBuffer(false),
			LastMapDirection((D3D11_MAP)0),  LinkedBuffer(0)
		{
#ifdef _DEBUG
			//setDebugName("CD3D11HardwareBuffer");
#endif

			Device = Driver->getExposedVideoData().D3D11.D3DDev11;

			if (Device)
			{
				Device->AddRef();
				Device->GetImmediateContext(&Context);
			}

			if (indexBuffer)
			{
				Mapping = indexBuffer->getHardwareMappingHint();
				Size = indexBuffer->getIndexSize() * indexBuffer->getIndexCount();
				Stride = indexBuffer->getIndexSize();
				createInternalBuffer(indexBuffer->getIndices());

				RequiredUpdate = false;

				LinkedBuffer = indexBuffer;
			}
		}

		CD3D11HardwareBuffer::CD3D11HardwareBuffer(scene::IVertexBuffer* vertexBuffer, CD3D11Driver* driver) :
			IHardwareBuffer(scene::EHM_NEVER, 0, 0, ConvertBufferType(vertexBuffer->getBufferType()), EDT_DIRECT3D11), Device(NULL), Context(NULL),
			Buffer(NULL), UAView(NULL), SRView(NULL), Driver(driver), TempStagingBuffer(NULL), UseTempStagingBuffer(false),
			LastMapDirection((D3D11_MAP)0),  LinkedBuffer(0)
		{
#ifdef _DEBUG
			//setDebugName("CD3D11HardwareBuffer");
#endif

			Device = Driver->getExposedVideoData().D3D11.D3DDev11;

			if (Device)
			{
				Device->AddRef();
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

			// Otherwise, map this buffer
			D3D11_MAPPED_SUBRESOURCE mappedData;
			HRESULT hr = Context->Map(Buffer, 0, LastMapDirection, 0, &mappedData);
			if (FAILED(hr))
				return 0;

			return mappedData.pData;
		}

		//! Unlock function. Must be called after a lock() to the buffer.
		void CD3D11HardwareBuffer::unlock()
		{
			if (!Buffer)
				return;

			// If using staging, return its pointer
			if (UseTempStagingBuffer)
			{
				TempStagingBuffer->unlock();

				// If write, copy staging to this
				if (LastMapDirection & D3D11_MAP_WRITE)
					this->copyFromBuffer(TempStagingBuffer, 0, 0, Size);

				return;
			}

			// Otherwise, unmap this
			Context->Unmap(Buffer, 0);
		}

		//! Copy data from system memory
		void CD3D11HardwareBuffer::copyFromMemory(const void* sysData, u32 offset, u32 length)
		{
			if (Buffer && Mapping == scene::EHM_DYNAMIC)
			{
				D3D11_BOX box;
				box.left = offset;
				box.top = 0;
				box.front = 0;
				box.right = length;
				box.bottom = 1;
				box.back = 1;
				Context->UpdateSubresource(Buffer, 0, &box, sysData, 0, 0);
			}
			else if (Buffer && Mapping != scene::EHM_STATIC)
			{
				D3D11_MAPPED_SUBRESOURCE mappedData;
				HRESULT hr = Context->Map(Buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedData);
				if (FAILED(hr))
				{
					os::Printer::log("Error Could not map dynamic buffr", ELL_ERROR);
					return;
				}
				memcpy(mappedData.pData, sysData, length);
				Context->Unmap(Buffer, 0);
			}
			else
			{
				if (TempStagingBuffer)
				{
					TempStagingBuffer = NULL;
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
		void CD3D11HardwareBuffer::copyFromBuffer(IHardwareBuffer* buffer, u32 srcOffset, u32 destOffset, u32 length)
		{
			if (!Buffer)
				return;

			if (buffer->getDriverType() != EDT_DIRECT3D11)
			{
				os::Printer::log("Fatal Error: Tried to copy data from a buffer not owned by this driver.", ELL_ERROR);
				return;
			}

			CD3D11HardwareBuffer* srcBuffer = static_cast<CD3D11HardwareBuffer*>(buffer);

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

			D3D11_BUFFER_DESC desc;
			desc.ByteWidth = Size;
			desc.StructureByteStride = Stride;
			desc.MiscFlags = 0;
			desc.CPUAccessFlags = 0;

			// Create new buffer
			switch (Mapping)
			{
			case scene::EHM_NEVER:
				desc.Usage = D3D11_USAGE_DYNAMIC;
				desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				break;
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
				desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
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
			auto plinkedBuffer = LinkedBuffer;
			auto pGetVertices = LinkedBuffer ? ((scene::IVertexBuffer*)LinkedBuffer)->getVertices() : 0;
			auto pgetvtx0 = LinkedBuffer ? ((scene::IVertexBuffer*)LinkedBuffer)->getVertex(0) : 0;
			auto pInitialData = initialData;
			auto pdatapsysmem = data.pSysMem;

			hr = Device->CreateBuffer(&desc, &data, &Buffer);
			if (FAILED(hr))
			{
				printf("\nvertexBuffer : %p", plinkedBuffer);
				printf("vertexBuffer->getVertices() : %p", pGetVertices);
				printf("vertexBuffer->getVertex(0) : %p", pgetvtx0);

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
			case EHBT_SHADER_RESOURCE:
			{
				D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc;
				SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
				SRVDesc.Format = DXGI_FORMAT_R32_FLOAT;
				SRVDesc.Buffer.ElementOffset = 0;
				SRVDesc.Buffer.ElementWidth = desc.ByteWidth / 4;

				hr = Device->CreateShaderResourceView(Buffer, &SRVDesc, &SRView);
				if (FAILED(hr))
				{
					os::Printer::log("Error creating shader resource view for buffer", ELL_ERROR);
					return false;
				}

				return true;
			}
			// If buffer if of type compute, create view
			case EHBT_COMPUTE:
			{
				D3D11_UNORDERED_ACCESS_VIEW_DESC UAVDesc;
				UAVDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
				UAVDesc.Buffer.FirstElement = 0;
				UAVDesc.Buffer.Flags = 0;

				if (Driver->queryFeature(EVDF_COMPUTING_SHADER_5_0))
				{
					UAVDesc.Format = DXGI_FORMAT_UNKNOWN;
					UAVDesc.Buffer.NumElements = desc.ByteWidth / 4;	// size in floats
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