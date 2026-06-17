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


		};
	}
}
