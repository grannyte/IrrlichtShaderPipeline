// Copyright (C) 2002-2012 Nikolaus Gebhardt
// This file is part of the "Irrlicht Engine".
// For conditions of distribution and use, see copyright notice in irrlicht.h

// Local CD3DX12_* helpers, shared between CD3D12Driver and CD3D11On12Driver.

#ifndef __C_D3D12_HELPERS_H_INCLUDED__
#define __C_D3D12_HELPERS_H_INCLUDED__
#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_12_
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi.h>
#include <stdio.h>
#include <vector>
#include "os.h"

namespace irr
{
	namespace video
	{

		struct CD3DX12_DEFAULT {};
		struct CD3DX12_CPU_DESCRIPTOR_HANDLE : public D3D12_CPU_DESCRIPTOR_HANDLE
		{
			CD3DX12_CPU_DESCRIPTOR_HANDLE() = default;
			explicit CD3DX12_CPU_DESCRIPTOR_HANDLE(const D3D12_CPU_DESCRIPTOR_HANDLE& o) :
				D3D12_CPU_DESCRIPTOR_HANDLE(o)
			{}
			CD3DX12_CPU_DESCRIPTOR_HANDLE(CD3DX12_DEFAULT) { ptr = 0; }
			CD3DX12_CPU_DESCRIPTOR_HANDLE(_In_ const D3D12_CPU_DESCRIPTOR_HANDLE& other, INT offsetScaledByIncrementSize)
			{
				InitOffsetted(other, offsetScaledByIncrementSize);
			}

			CD3DX12_CPU_DESCRIPTOR_HANDLE(_In_ const D3D12_CPU_DESCRIPTOR_HANDLE& other, INT offsetInDescriptors, UINT descriptorIncrementSize)
			{
				InitOffsetted(other, offsetInDescriptors, descriptorIncrementSize);
			}

			CD3DX12_CPU_DESCRIPTOR_HANDLE& Offset(INT offsetInDescriptors, UINT descriptorIncrementSize)
			{
				ptr += INT64(offsetInDescriptors) * UINT64(descriptorIncrementSize);
				return *this;
			}

			CD3DX12_CPU_DESCRIPTOR_HANDLE& Offset(INT offsetScaledByIncrementSize)
			{
				ptr += offsetScaledByIncrementSize;
				return *this;
			}
			bool operator==(_In_ const D3D12_CPU_DESCRIPTOR_HANDLE& other) const
			{
				return (ptr == other.ptr);
			}
			bool operator!=(_In_ const D3D12_CPU_DESCRIPTOR_HANDLE& other) const
			{
				return (ptr != other.ptr);
			}
			CD3DX12_CPU_DESCRIPTOR_HANDLE& operator=(const D3D12_CPU_DESCRIPTOR_HANDLE& other)
			{
				ptr = other.ptr;
				return *this;
			}
			inline void InitOffsetted(_In_ const D3D12_CPU_DESCRIPTOR_HANDLE& base, INT offsetScaledByIncrementSize)
			{
				InitOffsetted(*this, base, offsetScaledByIncrementSize);
			}
			inline void InitOffsetted(_In_ const D3D12_CPU_DESCRIPTOR_HANDLE& base, INT offsetInDescriptors, UINT descriptorIncrementSize)
			{
				InitOffsetted(*this, base, offsetInDescriptors, descriptorIncrementSize);
			}
			static inline void InitOffsetted(_Out_ D3D12_CPU_DESCRIPTOR_HANDLE& handle, _In_ const D3D12_CPU_DESCRIPTOR_HANDLE& base, INT offsetScaledByIncrementSize)
			{
				handle.ptr = base.ptr + offsetScaledByIncrementSize;
			}
			static inline void InitOffsetted(_Out_ D3D12_CPU_DESCRIPTOR_HANDLE& handle, _In_ const D3D12_CPU_DESCRIPTOR_HANDLE& base, INT offsetInDescriptors, UINT descriptorIncrementSize)
			{
				handle.ptr = static_cast<SIZE_T>(base.ptr + INT64(offsetInDescriptors) * UINT64(descriptorIncrementSize));
			}
		};
		struct CD3DX12_RESOURCE_BARRIER : public D3D12_RESOURCE_BARRIER
		{
			CD3DX12_RESOURCE_BARRIER() = default;
			explicit CD3DX12_RESOURCE_BARRIER(const D3D12_RESOURCE_BARRIER& o) :
				D3D12_RESOURCE_BARRIER(o)
			{}
			static inline CD3DX12_RESOURCE_BARRIER Transition(
				_In_ ID3D12Resource* pResource,
				D3D12_RESOURCE_STATES stateBefore,
				D3D12_RESOURCE_STATES stateAfter,
				UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
				D3D12_RESOURCE_BARRIER_FLAGS flags = D3D12_RESOURCE_BARRIER_FLAG_NONE)
			{
				CD3DX12_RESOURCE_BARRIER result = {};
				D3D12_RESOURCE_BARRIER& barrier = result;
				result.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				result.Flags = flags;
				barrier.Transition.pResource = pResource;
				barrier.Transition.StateBefore = stateBefore;
				barrier.Transition.StateAfter = stateAfter;
				barrier.Transition.Subresource = subresource;
				return result;
			}
			static inline CD3DX12_RESOURCE_BARRIER Aliasing(
				_In_ ID3D12Resource* pResourceBefore,
				_In_ ID3D12Resource* pResourceAfter)
			{
				CD3DX12_RESOURCE_BARRIER result = {};
				D3D12_RESOURCE_BARRIER& barrier = result;
				result.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
				barrier.Aliasing.pResourceBefore = pResourceBefore;
				barrier.Aliasing.pResourceAfter = pResourceAfter;
				return result;
			}
			static inline CD3DX12_RESOURCE_BARRIER UAV(
				_In_ ID3D12Resource* pResource)
			{
				CD3DX12_RESOURCE_BARRIER result = {};
				D3D12_RESOURCE_BARRIER& barrier = result;
				result.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
				barrier.UAV.pResource = pResource;
				return result;
			}
		};

		//! Readable name for HRESULTs returned by D3D12 Create*() calls.
		inline const char* d3d12ResultName(HRESULT hr)
		{
			switch (hr)
			{
			case S_OK:                            return "S_OK";
			case E_OUTOFMEMORY:                   return "E_OUTOFMEMORY";
			case E_INVALIDARG:                    return "E_INVALIDARG";
			case DXGI_ERROR_DEVICE_REMOVED:       return "DXGI_ERROR_DEVICE_REMOVED";
			case DXGI_ERROR_DEVICE_RESET:         return "DXGI_ERROR_DEVICE_RESET";
			case DXGI_ERROR_DEVICE_HUNG:          return "DXGI_ERROR_DEVICE_HUNG";
			case DXGI_ERROR_DRIVER_INTERNAL_ERROR:return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
			case DXGI_ERROR_INVALID_CALL:         return "DXGI_ERROR_INVALID_CALL";
			default:                              return "HRESULT inconnu";
			}
		}

		//! Drains messages accumulated by the D3D12 debug layer into the Irrlicht log.
		//! No-op if the debug layer isn't active (Release).
		inline void drainD3D12DebugMessages(ID3D12Device* device)
		{
			if (!device)
				return;

			ID3D12InfoQueue* infoQueue = nullptr;
			if (FAILED(device->QueryInterface(IID_PPV_ARGS(&infoQueue))) || !infoQueue)
				return;

			const UINT64 count = infoQueue->GetNumStoredMessages();
			for (UINT64 i = 0; i < count; ++i)
			{
				SIZE_T length = 0;
				if (FAILED(infoQueue->GetMessage(i, nullptr, &length)) || length == 0)
					continue;

				std::vector<char> storage(length);
				D3D12_MESSAGE* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
				if (SUCCEEDED(infoQueue->GetMessage(i, message, &length)) && message->pDescription)
					os::Printer::log(message->pDescription, ELL_ERROR);
			}

			infoQueue->ClearStoredMessages();
			infoQueue->Release();
		}

		//! Logs a failed D3D12 call and checks whether the device was removed.
		inline void logD3D12Failure(const char* what, HRESULT hr, ID3D12Device* device)
		{
			char msg[512];
			_snprintf_s(msg, sizeof(msg), _TRUNCATE, "%s a echoue : hr=0x%08lX (%s)",
				what, static_cast<unsigned long>(hr), d3d12ResultName(hr));
			os::Printer::log(msg, ELL_ERROR);

			if (!device)
				return;

			const HRESULT removed = device->GetDeviceRemovedReason();
			if (FAILED(removed))
			{
				_snprintf_s(msg, sizeof(msg), _TRUNCATE,
					"    ^ DEVICE RETIRE : GetDeviceRemovedReason()=0x%08lX (%s). Tous les Create*() "
					"echoueront desormais ; la cause premiere est ANTERIEURE a ce message.",
					static_cast<unsigned long>(removed), d3d12ResultName(removed));
				os::Printer::log(msg, ELL_ERROR);
			}

			// E_INVALIDARG: the HRESULT alone doesn't say WHAT is invalid -- the debug layer does.
			drainD3D12DebugMessages(device);
		}
	} // end namespace video
} // end namespace irr

#endif // _IRR_COMPILE_WITH_DIRECT3D_12_
#endif
