#pragma once


#include "IReferenceCounted.h"
#include "irrArray.h"
#include "IBuffer.h"
#include "IHardwareBuffer.h"

namespace irr
{
	namespace scene
	{

		class IComputeBuffer : public IBuffer
		{
		public: 
			IComputeBuffer() : IBuffer(E_BUFFER_TYPE::EBT_COMPUTE)
			{

			}

			virtual ~IComputeBuffer() override {
			
			}

			virtual void* getBufferPointer() const = 0;

			virtual u32 getStructureCount() const = 0;

			virtual u32 getStructureStride() const = 0;

			virtual u32 getBufferSize() const = 0;

			//! E_HARDWARE_BUFFER_FLAGS the hardware buffer is created with. Must be set before
			//! the first bind/dispatch - the views are built once, on creation.
			virtual u32 getBufferFlags() const
			{
				return BufferFlags;
			}

			virtual void setBufferFlags(u32 flags)
			{
				BufferFlags = flags;
			}

		protected:
			u32 BufferFlags = 0;
		};
	}
}
