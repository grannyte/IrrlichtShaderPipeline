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
			}

			virtual void set_used(u32 used) override
			{
				Data.resize(used);
			}

			virtual void reallocate(u32 size) override
			{
				Data.resize(size);
				Data.shrink_to_fit();
			}

			virtual u32 allocated_size() const override
			{
				return Data.capacity();
			}

			virtual s32 linear_reverse_search(const T& element) const  override
			{
				for (s32 i = Data.size() - 1; i >= 0; --i)
				{
					if (Data[i] == element)
						return i;
				}
				return -1;
			}

			virtual T& getElement(u32 elem) override
			{
				return Data[elem];
			}
			virtual void SetElement(u32 elem, const T& element) override
			{
				Data[elem] = element;
			}

			virtual u32 getStructureCount() const override
			{
				return Data.size();
			}

			virtual void getStructureStride() const override
			{
				return sizeof(T);
			}

			virtual void getBufferSize() const override
			{
				return sizeof(T) * Data.size();
			}

			virtual void* getBufferPointer() override
			{
				return Data.data();
			}

			virtual void downloadFromGPU() override
			{
				if (HardwareBuffer)
				{
					void* lcked = HardwareBuffer->lock(true);
					// resize the data if needed
					if (Data.size() != HardwareBuffer->size() / sizeof(T))
						Data.resize(HardwareBuffer->size() / sizeof(T));
					// copy the data
					memcpy(Data.data(), lcked, HardwareBuffer->size());


				}
			}

		protected:
			std::vector<T> Data;
		};
	}
}