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

			virtual void* getBufferPointer() = 0;

			virtual u32 getStructureCount() = 0;

			virtual u32 getStructureStride() = 0;

			virtual u32 getBufferSize() = 0;


		};
	}
}
