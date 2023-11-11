#pragma once
#include "IReferenceCounted.h"
#include "irrArray.h"
#include "IHardwareBuffer.h"

namespace irr
{
	namespace scene
	{
		class IBuffer : public virtual IReferenceCounted
		{
		public:
			//! Constructor
			IBuffer(E_BUFFER_TYPE type) : HardwareBuffer(0), BufferType(type), HardwareMappingHint(EHM_NEVER), ChangedID(1)
			{
			}

			virtual ~IBuffer() override
			{
			}

			virtual void clear() = 0;

			virtual void set_used(u32 used) = 0;

			virtual void reallocate(u32 size) = 0;

			virtual u32 allocated_size() const = 0;

			virtual void downloadFromGPU() = 0;

			virtual E_HARDWARE_MAPPING getHardwareMappingHint() const
			{
				return HardwareMappingHint;
			}

			virtual void setHardwareMappingHint(E_HARDWARE_MAPPING hardwareMappingHint)
			{
				if (HardwareMappingHint != hardwareMappingHint)
					setDirty();

				HardwareMappingHint = hardwareMappingHint;
			}


			virtual void setDirty()
			{
				if (HardwareBuffer)
					HardwareBuffer->requestUpdate();

				++ChangedID;
			}

			virtual u32 getChangedID() const
			{
				return ChangedID;
			}

			virtual void setBufferType(E_BUFFER_TYPE type)
			{
				BufferType = type;
			};

			virtual E_BUFFER_TYPE getBufferType() const
			{
				return BufferType;
			};



			const std::shared_ptr<video::IHardwareBuffer> &getHardwareBuffer() const
			{
				return HardwareBuffer;
			}

			// externalMemoryHandler parameter is used only by hardware buffers.
			void setHardwareBuffer(const std::shared_ptr<video::IHardwareBuffer>& hardwareBuffer)
			{
				HardwareBuffer = hardwareBuffer;
			}
		protected:
			std::shared_ptr<video::IHardwareBuffer> HardwareBuffer;

			E_BUFFER_TYPE BufferType;

			u32 ChangedID;

			E_HARDWARE_MAPPING HardwareMappingHint;

		};
	}
}