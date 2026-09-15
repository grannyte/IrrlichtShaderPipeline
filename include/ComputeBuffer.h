#pragma once

#include "IComputebuffer.h"
#include "IHardwareBuffer.h"

namespace irr
{
	namespace scene
	{
		template <typename T>
		class ComputeBuffer : public IComputeBuffer
		{
		public:
			ComputeBuffer() :IComputeBuffer()
			{
#ifdef _DEBUG
				setDebugName("ComputeBuffer");
#endif // _DEBUG

			}
			virtual ~ComputeBuffer() override
			{
			}


			virtual void clear() override
			{
				Data.clear();
				HostReleasedCount = 0;
			}

			virtual void set_used(u32 used) override
			{
				Data.resize(used);
				HostReleasedCount = 0;
			}

			virtual void reallocate(u32 size) override
			{
				Data.resize(size);
				Data.shrink_to_fit();
				HostReleasedCount = 0;
			}

			// Frees the host copy of a GPU-written buffer; the structure count stays so binds keep
			// working. Only valid once the hardware buffer exists; downloadFromGPU restores Data.
			void releaseHostCopy()
			{
				if (!HardwareBuffer || Data.empty())
					return;
				HostReleasedCount = (u32)Data.size();
				std::vector<T>().swap(Data);
			}

			bool isHostCopyReleased() const
			{
				return HostReleasedCount != 0;
			}

			virtual u32 allocated_size() const override
			{
				return Data.capacity();
			}

			// Not virtual: a virtual body instantiates with the class, forcing operator== on
			// every T. Non-virtual defers it to first use, so element types without == work.
			s32 linear_reverse_search(const T& element) const
			{
				for (s32 i = Data.size() - 1; i >= 0; --i)
				{
					if (Data[i] == element)
						return i;
				}
				return -1;
			}

			virtual T& getElement(u32 elem)
			{
				return Data[elem];
			}
			virtual void SetElement(u32 elem, const T& element)
			{
				Data[elem] = element;
			}

			u32 getStructureCount() const override
			{
				return Data.empty() ? HostReleasedCount : (u32)Data.size();
			}

			u32 getStructureStride() const override
			{
				return sizeof(T);
			}

			u32 getBufferSize() const override
			{
				return sizeof(T) * getStructureCount();
			}

			void* getBufferPointer() const override
			{
				return (void*)Data.data();
			}

			virtual void downloadFromGPU() override
			{
				if (HardwareBuffer)
				{
					// Force staging readback
					void* locked = HardwareBuffer->lock(true);
					if (!locked)
					{
						// lock a retourne null - hardware buffer pas lisible
						return;
					}
					if (Data.size() != HardwareBuffer->size() / sizeof(T))
						Data.resize(HardwareBuffer->size() / sizeof(T));
					HostReleasedCount = 0;
					memcpy(Data.data(), locked, HardwareBuffer->size());
					HardwareBuffer->unlock();
				}
			}

		protected:
			std::vector<T> Data;
			u32 HostReleasedCount = 0;
		};
	}
}